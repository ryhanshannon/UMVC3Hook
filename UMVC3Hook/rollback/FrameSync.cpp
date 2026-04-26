#include "FrameSync.h"
#include "Memory.h"
#include "Addresses.h"
#include "StateSnapshot.h"
#include "../utils/addr.h"
#include <windows.h>

namespace umvc3 {

// ---- Frame boundary hook state ----

static std::atomic<uint64_t> g_frameBoundaryCount{0};
static std::atomic<bool>     g_hookInstalled{false};
static void*                 g_hookStub = nullptr;

// The original function that the CALL at ADDR_InputHook targets.
using InputFuncPtr = void(__fastcall*)(long long*);
static InputFuncPtr g_originalInputFunc = nullptr;

// ---- Frame-boundary command queue ----
// Save/load MUST execute on the game's main thread at the frame boundary.
// Other threads post commands here; the hook processes them synchronously.

enum class PendingCommand : LONG {
    None = 0,
    Save = 1,
    Load = 2,
};

static volatile LONG g_pendingCommand = 0;     // Written by requester, read by hook
static volatile LONG g_pendingLoadFramesAgo = 0;
static volatile LONG g_commandResult = 0;      // 1=success, -1=fail, written by hook
static volatile LONG g_commandBusy = 0;        // Reentrancy guard
static HANDLE        g_commandDoneEvent = nullptr;

// The single shared snapshot — owned by the frame boundary thread.
struct SnapshotSlot {
    GameSnapshot snapshot;
    uint64_t     checksum = 0;
    uint64_t     frameCounter = 0;
    bool         valid = false;
};

static constexpr LONG INVALID_SNAPSHOT_SLOT = -1;

static SnapshotSlot          g_snapshotRing[SNAPSHOT_RING_SIZE];
static std::atomic<uint32_t> g_snapshotCount{0};
static std::atomic<uint32_t> g_nextWriteSlot{0};
static std::atomic<LONG>     g_latestSnapshotSlot{INVALID_SNAPSHOT_SLOT};
static std::atomic<uint64_t> g_lastLoadMicros{0};
static GameSnapshot          g_emptySnapshot;

static void InitializeSnapshotRingStorage() {
    for (uint32_t i = 0; i < SNAPSHOT_RING_SIZE; i++) {
        PrepareSnapshotStorage(&g_snapshotRing[i].snapshot);
        g_snapshotRing[i].checksum = 0;
        g_snapshotRing[i].frameCounter = 0;
        g_snapshotRing[i].valid = false;
    }
    g_snapshotCount.store(0, std::memory_order_release);
    g_nextWriteSlot.store(0, std::memory_order_release);
    g_latestSnapshotSlot.store(INVALID_SNAPSHOT_SLOT, std::memory_order_release);
    g_lastLoadMicros.store(0, std::memory_order_release);
}

static bool ResolveSnapshotSlotForFramesAgo(uint32_t framesAgo,
                                            SnapshotSlot** outSlot,
                                            uint32_t* outSlotIndex) {
    const uint32_t storedCount = g_snapshotCount.load(std::memory_order_acquire);
    const LONG latestSlot = g_latestSnapshotSlot.load(std::memory_order_acquire);
    if (storedCount == 0 || latestSlot == INVALID_SNAPSHOT_SLOT || framesAgo >= storedCount) {
        return false;
    }

    const uint32_t slotIndex =
        (static_cast<uint32_t>(latestSlot) + SNAPSHOT_RING_SIZE - (framesAgo % SNAPSHOT_RING_SIZE)) %
        SNAPSHOT_RING_SIZE;
    SnapshotSlot* slot = &g_snapshotRing[slotIndex];
    if (!slot->valid || !slot->snapshot.valid) {
        return false;
    }

    if (outSlot) {
        *outSlot = slot;
    }
    if (outSlotIndex) {
        *outSlotIndex = slotIndex;
    }
    return true;
}

static void ProcessPendingCommand() {
    LONG cmd = InterlockedExchange(&g_pendingCommand, static_cast<LONG>(PendingCommand::None));
    if (cmd == static_cast<LONG>(PendingCommand::None)) return;

    if (cmd == static_cast<LONG>(PendingCommand::Save)) {
        const uint32_t slotIndex = g_nextWriteSlot.load(std::memory_order_acquire);
        SnapshotSlot& slot = g_snapshotRing[slotIndex];
        slot.valid = false;
        slot.checksum = 0;
        slot.frameCounter = 0;

        bool ok = CaptureSnapshot(&slot.snapshot);
        if (ok) {
            slot.snapshot.frameCounter = g_frameBoundaryCount.load(std::memory_order_acquire);
            slot.frameCounter = slot.snapshot.frameCounter;
            slot.checksum = ChecksumSnapshot(slot.snapshot);
            slot.valid = true;

            const uint32_t storedCount = g_snapshotCount.load(std::memory_order_acquire);
            if (storedCount < SNAPSHOT_RING_SIZE) {
                g_snapshotCount.store(storedCount + 1, std::memory_order_release);
            }
            g_latestSnapshotSlot.store(static_cast<LONG>(slotIndex), std::memory_order_release);
            g_nextWriteSlot.store((slotIndex + 1) % SNAPSHOT_RING_SIZE, std::memory_order_release);
        }
        InterlockedExchange(&g_commandResult, ok ? 1 : -1);
        if (g_commandDoneEvent) SetEvent(g_commandDoneEvent);
    }
    else if (cmd == static_cast<LONG>(PendingCommand::Load)) {
        const uint32_t framesAgo = static_cast<uint32_t>(InterlockedExchange(&g_pendingLoadFramesAgo, 0));
        SnapshotSlot* slot = nullptr;
        bool ok = false;
        uint64_t loadMicros = 0;
        if (ResolveSnapshotSlotForFramesAgo(framesAgo, &slot, nullptr)) {
            const uint64_t startMicros = QueryPerformanceMicros();
            ok = LoadSnapshot(slot->snapshot);
            loadMicros = QueryPerformanceMicros() - startMicros;
        }
        g_lastLoadMicros.store(loadMicros, std::memory_order_release);
        InterlockedExchange(&g_commandResult, ok ? 1 : -1);
        if (g_commandDoneEvent) SetEvent(g_commandDoneEvent);
    }
}

static void __fastcall FrameBoundaryHookFn(long long* param_1) {
    // Cache param_1 — it IS the input buffer base pointer
    SetInputBufferBase(reinterpret_cast<uint64_t>(param_1));

    // CRITICAL: Call the original input function FIRST.
    // The game must process this frame's input before we save/load state.
    // The research DLL does the same (line 3440: original(param_1) before
    // any command processing).
    if (g_originalInputFunc) {
        g_originalInputFunc(param_1);
    }

    // Increment AFTER original runs (matches research DLL line 3442)
    g_frameBoundaryCount.fetch_add(1, std::memory_order_release);

    // Reentrancy guard — if we're already processing, skip
    if (InterlockedCompareExchange(&g_commandBusy, 1, 0) != 0)
        return;

    // Process pending save/load command with SEH protection
    __try {
        ProcessPendingCommand();
    }
    __finally {
        InterlockedExchange(&g_commandBusy, 0);
    }
}

// ---- Hook installation helpers ----

static bool IsRel32Reachable(uint64_t from, uint64_t to) {
    int64_t disp = static_cast<int64_t>(to) - static_cast<int64_t>(from + 5);
    return disp >= INT32_MIN && disp <= INT32_MAX;
}

static void* AllocateExecutableNear(uint64_t targetAddr, size_t size) {
    SYSTEM_INFO sysInfo = {};
    GetSystemInfo(&sysInfo);
    const uint64_t granularity = static_cast<uint64_t>(sysInfo.dwAllocationGranularity);
    uint64_t aligned = targetAddr & ~(granularity - 1);

    // Search forward
    uint64_t current = aligned;
    for (;;) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery((LPCVOID)current, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
            uint64_t candidate = (reinterpret_cast<uint64_t>(mbi.BaseAddress) + granularity - 1) & ~(granularity - 1);
            if (IsRel32Reachable(targetAddr, candidate)) {
                void* mem = VirtualAlloc((LPVOID)candidate, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (mem) return mem;
            }
        }
        if (mbi.RegionSize == 0 || current + mbi.RegionSize < current) break;
        current += mbi.RegionSize;
        if (!IsRel32Reachable(targetAddr, current)) break;
    }

    // Search backward
    current = aligned;
    while (current > granularity) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery((LPCVOID)(current - 1), &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
            uint64_t candidate = (reinterpret_cast<uint64_t>(mbi.BaseAddress) + granularity - 1) & ~(granularity - 1);
            if (IsRel32Reachable(targetAddr, candidate)) {
                void* mem = VirtualAlloc((LPVOID)candidate, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (mem) return mem;
            }
        }
        if (mbi.RegionSize == 0 || current < mbi.RegionSize) break;
        current -= mbi.RegionSize;
        if (!IsRel32Reachable(targetAddr, current)) break;
    }

    return nullptr;
}

bool InstallFrameBoundaryHook() {
    if (g_hookInstalled.load()) return true;
    InitializeSnapshotRingStorage();

    uint64_t callSite = _addr(ADDR_InputHook);

    // Verify it's a CALL instruction (0xE8)
    uint8_t opcode = 0;
    if (!SafeRead<uint8_t>(callSite, &opcode) || opcode != 0xE8)
        return false;

    // Read existing rel32 to find the original function
    int32_t origRel32 = 0;
    if (!SafeRead<int32_t>(callSite + 1, &origRel32))
        return false;
    g_originalInputFunc = reinterpret_cast<InputFuncPtr>(callSite + 5 + origRel32);

    // Allocate a stub near the call site for an absolute jump to our hook
    g_hookStub = AllocateExecutableNear(callSite, 64);
    if (!g_hookStub) return false;

    // Write: mov rax, <absolute address of FrameBoundaryHookFn>; jmp rax
    uint8_t* stub = static_cast<uint8_t*>(g_hookStub);
    stub[0] = 0x48; stub[1] = 0xB8;  // mov rax, imm64
    uint64_t hookAddr = reinterpret_cast<uint64_t>(&FrameBoundaryHookFn);
    memcpy(stub + 2, &hookAddr, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;  // jmp rax
    FlushInstructionCache(GetCurrentProcess(), g_hookStub, 12);

    // Patch the CALL's rel32 to point at our stub
    uint64_t stubAddr = reinterpret_cast<uint64_t>(g_hookStub);
    if (!IsRel32Reachable(callSite, stubAddr))
        return false;

    int32_t newRel32 = static_cast<int32_t>(static_cast<int64_t>(stubAddr) - static_cast<int64_t>(callSite + 5));
    if (!SafeWriteProtected(callSite + 1, &newRel32, sizeof(newRel32)))
        return false;

    g_hookInstalled.store(true, std::memory_order_release);
    return true;
}

bool IsFrameBoundaryHookInstalled() {
    return g_hookInstalled.load(std::memory_order_acquire);
}

uint64_t GetFrameBoundaryCount() {
    return g_frameBoundaryCount.load(std::memory_order_acquire);
}

// ---- Frame-boundary-safe request API ----

static FrameCommandResult IssueCommand(PendingCommand cmd) {
    if (!g_hookInstalled.load(std::memory_order_acquire))
        return FrameCommandResult::NotInstalled;

    // Lazy-init the event
    if (!g_commandDoneEvent) {
        g_commandDoneEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!g_commandDoneEvent) return FrameCommandResult::Failed;
    }

    // Post the command
    InterlockedExchange(&g_commandResult, 0);
    InterlockedExchange(&g_pendingCommand, static_cast<LONG>(cmd));

    // Wait for the hook to process it (timeout = 500ms = ~30 frames)
    DWORD wait = WaitForSingleObject(g_commandDoneEvent, 500);
    if (wait == WAIT_TIMEOUT)
        return FrameCommandResult::Timeout;

    LONG result = InterlockedCompareExchange(&g_commandResult, 0, 0);
    return (result == 1) ? FrameCommandResult::Success : FrameCommandResult::Failed;
}

FrameCommandResult RequestSave() {
    return IssueCommand(PendingCommand::Save);
}

FrameCommandResult RequestLoad() {
    return RequestLoadFramesAgo(0);
}

FrameCommandResult RequestLoadFramesAgo(uint32_t framesAgo) {
    if (framesAgo >= SNAPSHOT_RING_SIZE) {
        return FrameCommandResult::Failed;
    }
    InterlockedExchange(&g_pendingLoadFramesAgo, static_cast<LONG>(framesAgo));
    return IssueCommand(PendingCommand::Load);
}

const GameSnapshot& GetLastSnapshot() {
    SnapshotSlot* slot = nullptr;
    if (!ResolveSnapshotSlotForFramesAgo(0, &slot, nullptr)) {
        return g_emptySnapshot;
    }
    return slot->snapshot;
}

uint32_t GetStoredSnapshotCount() {
    return g_snapshotCount.load(std::memory_order_acquire);
}

uint32_t GetSnapshotRingCapacity() {
    return SNAPSHOT_RING_SIZE;
}

int32_t GetLastSnapshotSlotIndex() {
    return static_cast<int32_t>(g_latestSnapshotSlot.load(std::memory_order_acquire));
}

uint64_t GetLastSnapshotChecksum() {
    SnapshotSlot* slot = nullptr;
    if (!ResolveSnapshotSlotForFramesAgo(0, &slot, nullptr)) {
        return 0;
    }
    return slot->checksum;
}

uint64_t GetLastLoadMicros() {
    return g_lastLoadMicros.load(std::memory_order_acquire);
}

// ---- Renderless frame advance ----

struct RenderPatch {
    uint64_t runtimeAddr = 0;
    uint8_t  originalByte = 0;
    bool     applied = false;
};

static RenderPatch g_renderPatches[RENDER_TARGET_COUNT] = {};
static bool g_renderingDisabled = false;

bool DisableRendering() {
    if (g_renderingDisabled) return true;

    for (size_t i = 0; i < RENDER_TARGET_COUNT; i++) {
        uint64_t addr = _addr(RENDER_TARGETS[i].va);
        g_renderPatches[i].runtimeAddr = addr;

        if (!SafeRead<uint8_t>(addr, &g_renderPatches[i].originalByte))
            return false;

        uint8_t ret = 0xC3;
        if (!SafeWriteProtected(addr, &ret, 1))
            return false;

        g_renderPatches[i].applied = true;
    }

    g_renderingDisabled = true;
    return true;
}

bool EnableRendering() {
    if (!g_renderingDisabled) return true;

    bool ok = true;
    for (size_t i = 0; i < RENDER_TARGET_COUNT; i++) {
        if (!g_renderPatches[i].applied) continue;
        if (!SafeWriteProtected(g_renderPatches[i].runtimeAddr,
                                &g_renderPatches[i].originalByte, 1)) {
            ok = false;
        }
        g_renderPatches[i].applied = false;
    }

    g_renderingDisabled = false;
    return ok;
}

bool IsRenderingDisabled() {
    return g_renderingDisabled;
}

} // namespace umvc3
