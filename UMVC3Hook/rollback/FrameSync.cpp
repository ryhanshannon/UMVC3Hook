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
// We call through to it after our hook logic.
using InputFuncPtr = void(__fastcall*)(long long*);
static InputFuncPtr g_originalInputFunc = nullptr;

static void __fastcall FrameBoundaryHookFn(long long* param_1) {
    // param_1 IS the input buffer base pointer — cache it for snapshot use
    SetInputBufferBase(reinterpret_cast<uint64_t>(param_1));

    g_frameBoundaryCount.fetch_add(1, std::memory_order_release);

    // Call the original input function
    if (g_originalInputFunc) {
        g_originalInputFunc(param_1);
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
