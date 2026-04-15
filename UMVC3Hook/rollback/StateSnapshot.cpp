#include "StateSnapshot.h"
#include "../utils/addr.h"
#include <algorithm>
#include <atomic>

namespace umvc3 {

// ---- Internal helpers ----

static bool ReadStaticPointer(uint64_t staticVA, uint64_t* out) {
    *out = 0;
    return SafeReadPtr(_addr(staticVA), out) && *out != 0;
}

static bool ResolveGameSpeedObject(uint64_t* out) {
    uint64_t ptr = 0;
    if (!SafeReadPtr(_addr(ADDR_GameSpeed), &ptr) || ptr == 0) return false;
    // GameSpeed is a pointer to an object; dereference once
    *out = ptr;
    return true;
}

static bool ResolveRngSeedRegion(uint64_t* stateAddr, uint64_t* seedAddr) {
    // The RNG accessor at ADDR_RngAccessor returns a pointer to a state block.
    // The seed lives at offset +0x1C within that block.
    uint64_t accessor = _addr(ADDR_RngAccessor);
    // Read the LEA target: the accessor function loads a static pointer.
    // We replicate what the research DLL does: read the pointer the accessor would return.
    uint64_t rngState = 0;

    // The accessor at ADDR_RngAccessor is a function. Instead of calling it,
    // we read the static it references. The research DLL found the inner address
    // at ADDR_RngAccessor + 0x24 (ADDR_RngAccessorInner), which LEAs a global.
    // For safety, try the direct approach: call the accessor if it's readable.
    // Simpler: the research DLL stores the result of probing this at runtime.
    // For now, just read the function's target static directly.
    uint64_t innerAddr = _addr(0x1409AFAE4);  // ADDR_RngAccessorInner
    int32_t rel32 = 0;
    if (!SafeRead<int32_t>(innerAddr + 3, &rel32)) return false;
    uint64_t staticAddr = innerAddr + 7 + rel32;
    if (!SafeReadPtr(staticAddr, &rngState) || rngState == 0) return false;

    *stateAddr = rngState;
    *seedAddr = rngState + 0x1C;
    return true;
}

// The input buffer base is discovered at runtime by the frame boundary hook.
// The research DLL caches it in an atomic after the first hook fire.
// We replicate this: FrameSync sets it, we read it here.
static std::atomic<uint64_t> g_inputBufferBase{0};

void SetInputBufferBase(uint64_t addr) {
    g_inputBufferBase.store(addr, std::memory_order_release);
}

static bool ResolveInputBase(uint64_t* outBase) {
    // Primary: use the cached address from the frame boundary hook
    uint64_t cached = g_inputBufferBase.load(std::memory_order_acquire);
    if (cached != 0 && IsReadable(cached, INPUT_BUFFER_SIZE)) {
        *outBase = cached;
        return true;
    }

    // Fallback: try InputDisplay pointer chain
    uint64_t displayPtr = 0;
    if (ReadStaticPointer(ADDR_InputDisplay, &displayPtr) &&
        IsReadable(displayPtr, INPUT_BUFFER_SIZE)) {
        *outBase = displayPtr;
        return true;
    }

    return false;
}

static void ReserveRegionBytes(MemoryRegion* region, size_t size) {
    if (!region) {
        return;
    }
    if (region->bytes.capacity() < size) {
        region->bytes.reserve(size);
    }
}

static void ResetRegionForCapture(MemoryRegion* region) {
    if (!region) {
        return;
    }
    region->addr = 0;
    region->bytes.clear();
}

static void ResetCollisionTableForCapture(CollisionTableSnapshot* table) {
    if (!table) {
        return;
    }
    table->fighterAddr = 0;
    table->tableFieldAddr = 0;
    table->isHurtbox = false;
    table->countField = 0;
    table->capturedCount = 0;
    table->wrapperSize = 0;
    ResetRegionForCapture(&table->tableHeaderRegion);
    ResetRegionForCapture(&table->pointerArrayRegion);
    table->entries.clear();
}

static void ResetSnapshotForCapture(GameSnapshot* snapshot) {
    snapshot->valid = false;
    snapshot->frameCounter = 0;
    memset(snapshot->fighterAddrs, 0, sizeof(snapshot->fighterAddrs));
    for (int i = 0; i < MAX_FIGHTERS; i++) {
        ResetRegionForCapture(&snapshot->fighters[i]);
        ResetCollisionTableForCapture(&snapshot->hitboxes[i]);
        ResetCollisionTableForCapture(&snapshot->hurtboxes[i]);
    }

    ResetRegionForCapture(&snapshot->teams[0]);
    ResetRegionForCapture(&snapshot->teams[1]);
    ResetRegionForCapture(&snapshot->sActionCore);
    ResetRegionForCapture(&snapshot->mysteryTableRegion);
    ResetRegionForCapture(&snapshot->gameSpeedRegion);
    snapshot->rngStateAddr = 0;
    ResetRegionForCapture(&snapshot->rngSeedRegion);
    snapshot->inputBase = 0;
    ResetRegionForCapture(&snapshot->inputP1);
    ResetRegionForCapture(&snapshot->inputP2);
    ResetRegionForCapture(&snapshot->recordingDataState);
    snapshot->projectiles.clear();
    snapshot->totalBytes = 0;
    snapshot->captureMicros = 0;
}

void PrepareSnapshotStorage(GameSnapshot* snapshot) {
    if (!snapshot) {
        return;
    }

    for (int i = 0; i < MAX_FIGHTERS; i++) {
        ReserveRegionBytes(&snapshot->fighters[i], FIGHTER_SNAPSHOT_SIZE);

        ReserveRegionBytes(&snapshot->hitboxes[i].tableHeaderRegion, COLLISION_TABLE_HEADER_SIZE);
        ReserveRegionBytes(
            &snapshot->hitboxes[i].pointerArrayRegion,
            MAX_COLLISION_TABLE_ENTRIES * sizeof(uint64_t));
        if (snapshot->hitboxes[i].entries.capacity() < MAX_COLLISION_TABLE_ENTRIES) {
            snapshot->hitboxes[i].entries.reserve(MAX_COLLISION_TABLE_ENTRIES);
        }

        ReserveRegionBytes(&snapshot->hurtboxes[i].tableHeaderRegion, COLLISION_TABLE_HEADER_SIZE);
        ReserveRegionBytes(
            &snapshot->hurtboxes[i].pointerArrayRegion,
            MAX_COLLISION_TABLE_ENTRIES * sizeof(uint64_t));
        if (snapshot->hurtboxes[i].entries.capacity() < MAX_COLLISION_TABLE_ENTRIES) {
            snapshot->hurtboxes[i].entries.reserve(MAX_COLLISION_TABLE_ENTRIES);
        }
    }

    ReserveRegionBytes(&snapshot->teams[0], TEAM_SNAPSHOT_SIZE);
    ReserveRegionBytes(&snapshot->teams[1], TEAM_SNAPSHOT_SIZE);
    ReserveRegionBytes(&snapshot->sActionCore, MATCH_CORE_SNAPSHOT_SIZE);
    ReserveRegionBytes(&snapshot->mysteryTableRegion, MYSTERY_TABLE_SIZE);
    ReserveRegionBytes(&snapshot->gameSpeedRegion, GAME_SPEED_REGION_SIZE);
    ReserveRegionBytes(&snapshot->rngSeedRegion, sizeof(uint32_t));
    ReserveRegionBytes(&snapshot->inputP1, INPUT_BUFFER_SIZE);
    ReserveRegionBytes(&snapshot->inputP2, INPUT_BUFFER_SIZE);
    ReserveRegionBytes(&snapshot->recordingDataState, RECORDING_DATA_SIZE);

    const size_t maxProjectileSlots = static_cast<size_t>(MAX_PROJECTILE_NODES) * 3;
    if (snapshot->projectiles.capacity() < maxProjectileSlots) {
        snapshot->projectiles.reserve(maxProjectileSlots);
    }
}

// ---- Collision table capture/restore ----

static bool IsAddressWithinRange(uint64_t addr, uint64_t base, size_t size) {
    return addr >= base && addr < (base + size);
}

static bool IsInMainModule(uint64_t addr) {
    const uint64_t base = _addr(IMAGE_BASE);
    return addr >= base && addr < (base + 0x02000000);
}

static size_t CountPointerArrayEntries(uint64_t arrayAddr,
                                       size_t hardCap,
                                       std::vector<uint64_t>* outPtrs) {
    if (outPtrs) {
        outPtrs->clear();
    }
    if (arrayAddr == 0) {
        return 0;
    }

    size_t count = 0;
    for (; count < hardCap; count++) {
        uint64_t value = 0;
        if (!SafeReadPtr(arrayAddr + (count * sizeof(uint64_t)), &value) || value == 0) {
            break;
        }
        if (outPtrs) {
            outPtrs->push_back(value);
        }
    }
    return count;
}

static size_t InferCollisionWrapperSize(const std::vector<uint64_t>& entryPtrs) {
    if (entryPtrs.size() < 2) {
        return COLLISION_WRAPPER_DEFAULT_SIZE;
    }

    const uint64_t stride = entryPtrs[1] - entryPtrs[0];
    if (stride < COLLISION_WRAPPER_MIN_SIZE || stride > 0x400) {
        return COLLISION_WRAPPER_DEFAULT_SIZE;
    }

    for (size_t i = 2; i < entryPtrs.size(); i++) {
        if ((entryPtrs[i] - entryPtrs[i - 1]) != stride) {
            return COLLISION_WRAPPER_DEFAULT_SIZE;
        }
    }
    return static_cast<size_t>(stride);
}

static bool CaptureCollisionWrapperRegion(uint64_t entryAddr,
                                          size_t preferredSize,
                                          MemoryRegion* out) {
    const size_t candidateSizes[] = {
        preferredSize,
        COLLISION_WRAPPER_DEFAULT_SIZE,
        0x80,
        COLLISION_WRAPPER_MIN_SIZE,
    };

    for (size_t i = 0; i < sizeof(candidateSizes) / sizeof(candidateSizes[0]); i++) {
        const size_t size = candidateSizes[i];
        if (size == 0) {
            continue;
        }
        if (CaptureRegion(entryAddr, size, out)) {
            return true;
        }
    }
    return false;
}

struct CollisionRelocationSpan {
    uint64_t sourceAddr = 0;
    size_t sourceSize = 0;
    uint64_t destinationAddr = 0;
};

static std::vector<uint64_t> DecodePointerArrayValues(const MemoryRegion& region) {
    std::vector<uint64_t> values;
    const size_t entryCount = region.bytes.size() / sizeof(uint64_t);
    values.reserve(entryCount);
    for (size_t i = 0; i < entryCount; i++) {
        uint64_t value = 0;
        memcpy(&value, region.bytes.data() + (i * sizeof(uint64_t)), sizeof(uint64_t));
        values.push_back(value);
    }
    return values;
}

static std::vector<uint64_t> DecodeCollisionSecondaryPointerValues(const CollisionTableSnapshot& table) {
    std::vector<uint64_t> values;
    values.reserve(table.entries.size());
    for (size_t i = 0; i < table.entries.size(); i++) {
        uint64_t value = 0;
        if (table.entries[i].wrapperRegion.bytes.size() >=
            (COLLISION_WRAPPER_SECONDARY_PTR_OFFSET + sizeof(uint64_t))) {
            memcpy(&value,
                   table.entries[i].wrapperRegion.bytes.data() + COLLISION_WRAPPER_SECONDARY_PTR_OFFSET,
                   sizeof(uint64_t));
        }
        values.push_back(value);
    }
    return values;
}

static bool PointerListsDiffer(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    if (a.size() != b.size()) {
        return true;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) {
            return true;
        }
    }
    return false;
}

static bool WriteU64LE(std::vector<uint8_t>* bytes, size_t offset, uint64_t value) {
    if (!bytes || (offset + sizeof(uint64_t)) > bytes->size()) {
        return false;
    }
    memcpy(bytes->data() + offset, &value, sizeof(uint64_t));
    return true;
}

static bool WriteU32LE(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
    if (!bytes || (offset + sizeof(uint32_t)) > bytes->size()) {
        return false;
    }
    memcpy(bytes->data() + offset, &value, sizeof(uint32_t));
    return true;
}

static uint32_t DecodeCollisionTableCapacity(const CollisionTableSnapshot& table) {
    uint32_t value = 0;
    if (table.tableHeaderRegion.bytes.size() >=
        (COLLISION_TABLE_CAPACITY_OFFSET + sizeof(uint32_t))) {
        memcpy(&value,
               table.tableHeaderRegion.bytes.data() + COLLISION_TABLE_CAPACITY_OFFSET,
               sizeof(uint32_t));
    }
    return value;
}

static void RelocatePointerLikeQwords(std::vector<uint8_t>* bytes,
                                      const std::vector<CollisionRelocationSpan>& spans) {
    if (!bytes || bytes->empty() || spans.empty()) {
        return;
    }

    for (size_t offset = 0; offset + sizeof(uint64_t) <= bytes->size(); offset += sizeof(uint64_t)) {
        uint64_t value = 0;
        memcpy(&value, bytes->data() + offset, sizeof(uint64_t));
        for (size_t spanIndex = 0; spanIndex < spans.size(); spanIndex++) {
            const CollisionRelocationSpan& span = spans[spanIndex];
            if (span.sourceAddr == 0 || span.destinationAddr == 0 || span.sourceSize == 0) {
                continue;
            }
            if (value >= span.sourceAddr && value < (span.sourceAddr + span.sourceSize)) {
                value = span.destinationAddr + (value - span.sourceAddr);
                memcpy(bytes->data() + offset, &value, sizeof(uint64_t));
                break;
            }
        }
    }
}

static bool RestoreBytes(uint64_t destinationAddr,
                         const std::vector<uint8_t>& bytes,
                         bool required) {
    if (destinationAddr == 0 || bytes.empty()) {
        return true;
    }
    if (SafeWriteMem(reinterpret_cast<void*>(destinationAddr), bytes.data(), bytes.size())) {
        return true;
    }
    return !required;
}

static bool CaptureCollisionTable(uint64_t fighterAddr, size_t tableOffset,
                                  bool isHurtbox, CollisionTableSnapshot* out) {
    *out = CollisionTableSnapshot{};
    out->fighterAddr = fighterAddr;
    out->tableFieldAddr = fighterAddr + tableOffset;
    out->isHurtbox = isHurtbox;

    uint64_t tableBase = 0;
    if (!SafeReadPtr(fighterAddr + tableOffset, &tableBase) || tableBase == 0)
        return true;  // No table pointer — not an error, just empty

    // Read header
    if (!CaptureRegion(tableBase, COLLISION_TABLE_HEADER_SIZE, &out->tableHeaderRegion))
        return false;

    // Read count from header
    uint32_t count = 0;
    if (!SafeRead<uint32_t>(tableBase + COLLISION_TABLE_COUNT_OFFSET, &count))
        return false;
    out->countField = count;

    uint64_t ptrArrayAddr = 0;
    if (!SafeReadPtr(tableBase + COLLISION_TABLE_POINTER_ARRAY_OFFSET, &ptrArrayAddr)) {
        ptrArrayAddr = 0;
    }

    std::vector<uint64_t> entryPtrs;
    size_t scannedCount =
        CountPointerArrayEntries(ptrArrayAddr, MAX_COLLISION_TABLE_ENTRIES, &entryPtrs);
    size_t effectiveCount = scannedCount;
    if (out->countField > effectiveCount && out->countField <= MAX_COLLISION_TABLE_ENTRIES) {
        effectiveCount = out->countField;
    }

    if (ptrArrayAddr != 0 && effectiveCount > 0) {
        if (!CaptureRegion(ptrArrayAddr, effectiveCount * sizeof(uint64_t), &out->pointerArrayRegion)) {
            return false;
        }
    }

    out->capturedCount = static_cast<uint32_t>(entryPtrs.size());
    out->wrapperSize = InferCollisionWrapperSize(entryPtrs);

    for (size_t i = 0; i < entryPtrs.size(); i++) {
        CollisionEntrySnapshot entry;
        if (!CaptureCollisionWrapperRegion(entryPtrs[i], out->wrapperSize, &entry.wrapperRegion)) {
            return false;
        }

        uint64_t secondaryPtr = 0;
        if (SafeRead<uint64_t>(entryPtrs[i] + COLLISION_WRAPPER_SECONDARY_PTR_OFFSET, &secondaryPtr) &&
            secondaryPtr != 0 &&
            !IsInMainModule(secondaryPtr) &&
            !IsAddressWithinRange(secondaryPtr, fighterAddr, FIGHTER_SNAPSHOT_SIZE) &&
            IsReadable(secondaryPtr, COLLISION_SECONDARY_SIZE)) {
            if (!CaptureRegion(secondaryPtr, COLLISION_SECONDARY_SIZE, &entry.secondaryRegion)) {
                return false;
            }
        }

        out->entries.push_back(std::move(entry));
    }

    return true;
}

static bool RestoreCollisionTablePreservingLiveState(const CollisionTableSnapshot& live) {
    if (live.tableFieldAddr == 0 || live.tableHeaderRegion.addr == 0) {
        return false;
    }

    bool ok = true;
    std::vector<uint8_t> tableFieldBytes(sizeof(uint64_t), 0);
    memcpy(tableFieldBytes.data(), &live.tableHeaderRegion.addr, sizeof(uint64_t));
    ok &= RestoreBytes(live.tableFieldAddr, tableFieldBytes, true);
    ok &= RestoreRegion(live.tableHeaderRegion);

    if (!live.pointerArrayRegion.bytes.empty()) {
        ok &= RestoreRegion(live.pointerArrayRegion);
    }
    return ok;
}

static bool RestoreCollisionTablePreservingLiveChain(const CollisionTableSnapshot& saved,
                                                     const CollisionTableSnapshot& live) {
    if (saved.tableHeaderRegion.addr == 0 || saved.pointerArrayRegion.bytes.empty()) {
        return false;
    }
    if (live.tableHeaderRegion.addr == 0 || live.pointerArrayRegion.addr == 0) {
        return false;
    }

    const std::vector<uint64_t> liveWrapperAddrs = DecodePointerArrayValues(live.pointerArrayRegion);
    if (liveWrapperAddrs.size() < saved.entries.size()) {
        return false;
    }

    const std::vector<uint64_t> liveSecondaryAddrs =
        DecodeCollisionSecondaryPointerValues(live);

    std::vector<CollisionRelocationSpan> spans;
    spans.reserve(2 + (saved.entries.size() * 2));
    spans.push_back({
        saved.tableHeaderRegion.addr,
        saved.tableHeaderRegion.bytes.size(),
        live.tableHeaderRegion.addr,
    });
    spans.push_back({
        saved.pointerArrayRegion.addr,
        saved.pointerArrayRegion.bytes.size(),
        live.pointerArrayRegion.addr,
    });

    for (size_t i = 0; i < saved.entries.size(); i++) {
        if (saved.entries[i].wrapperRegion.addr != 0 &&
            !saved.entries[i].wrapperRegion.bytes.empty()) {
            spans.push_back({
                saved.entries[i].wrapperRegion.addr,
                saved.entries[i].wrapperRegion.bytes.size(),
                liveWrapperAddrs[i],
            });
        }

        uint64_t destinationSecondaryAddr = 0;
        if (i < liveSecondaryAddrs.size()) {
            destinationSecondaryAddr = liveSecondaryAddrs[i];
        }
        if (destinationSecondaryAddr == 0) {
            destinationSecondaryAddr = saved.entries[i].secondaryRegion.addr;
        }
        if (saved.entries[i].secondaryRegion.addr != 0 &&
            !saved.entries[i].secondaryRegion.bytes.empty() &&
            destinationSecondaryAddr != 0) {
            spans.push_back({
                saved.entries[i].secondaryRegion.addr,
                saved.entries[i].secondaryRegion.bytes.size(),
                destinationSecondaryAddr,
            });
        }
    }

    bool ok = true;
    std::vector<uint8_t> tableFieldBytes(sizeof(uint64_t), 0);
    memcpy(tableFieldBytes.data(), &live.tableHeaderRegion.addr, sizeof(uint64_t));
    ok &= RestoreBytes(live.tableFieldAddr, tableFieldBytes, true);

    std::vector<uint8_t> headerBytes = saved.tableHeaderRegion.bytes;
    RelocatePointerLikeQwords(&headerBytes, spans);
    WriteU64LE(&headerBytes, COLLISION_TABLE_POINTER_ARRAY_OFFSET, live.pointerArrayRegion.addr);
    ok &= RestoreBytes(live.tableHeaderRegion.addr, headerBytes, true);

    std::vector<uint8_t> pointerArrayBytes(live.pointerArrayRegion.bytes.size(), 0);
    const size_t destinationPointerCount = pointerArrayBytes.size() / sizeof(uint64_t);
    const size_t pointerCount = (std::min)(saved.entries.size(), destinationPointerCount);
    for (size_t i = 0; i < pointerCount; i++) {
        memcpy(pointerArrayBytes.data() + (i * sizeof(uint64_t)),
               &liveWrapperAddrs[i],
               sizeof(uint64_t));
    }
    ok &= RestoreBytes(live.pointerArrayRegion.addr, pointerArrayBytes, true);

    for (size_t i = 0; i < saved.entries.size(); i++) {
        uint64_t destinationSecondaryAddr = 0;
        if (i < liveSecondaryAddrs.size()) {
            destinationSecondaryAddr = liveSecondaryAddrs[i];
        }
        if (destinationSecondaryAddr == 0) {
            destinationSecondaryAddr = saved.entries[i].secondaryRegion.addr;
        }

        if (!saved.entries[i].secondaryRegion.bytes.empty() &&
            destinationSecondaryAddr != 0) {
            std::vector<uint8_t> secondaryBytes = saved.entries[i].secondaryRegion.bytes;
            RelocatePointerLikeQwords(&secondaryBytes, spans);
            ok &= RestoreBytes(destinationSecondaryAddr, secondaryBytes, false);
        }

        if (!saved.entries[i].wrapperRegion.bytes.empty() && liveWrapperAddrs[i] != 0) {
            std::vector<uint8_t> wrapperBytes = saved.entries[i].wrapperRegion.bytes;
            RelocatePointerLikeQwords(&wrapperBytes, spans);
            if (destinationSecondaryAddr != 0) {
                WriteU64LE(&wrapperBytes,
                           COLLISION_WRAPPER_SECONDARY_PTR_OFFSET,
                           destinationSecondaryAddr);
            }
            ok &= RestoreBytes(liveWrapperAddrs[i], wrapperBytes, true);
        }
    }

    return ok;
}

using CollisionTableGrowFn = void (*)(void* tableVector, uint32_t newCapacity);

static bool CallCollisionTableGrowHelper(uint64_t tableVectorAddr, uint32_t newCapacity) {
    CollisionTableGrowFn grow =
        reinterpret_cast<CollisionTableGrowFn>(_addr(ADDR_CollisionTableGrow));
    __try {
        grow(reinterpret_cast<void*>(tableVectorAddr), newCapacity);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool RestoreCollisionTableRebuildingMissingLivePointerArray(
    const CollisionTableSnapshot& saved,
    const CollisionTableSnapshot& live) {
    if (saved.tableFieldAddr == 0 ||
        saved.tableHeaderRegion.addr == 0 ||
        saved.entries.empty() ||
        live.tableFieldAddr == 0 ||
        live.tableHeaderRegion.addr == 0) {
        return false;
    }

    const uint32_t savedCapacity = DecodeCollisionTableCapacity(saved);
    const uint32_t liveCapacity = DecodeCollisionTableCapacity(live);
    const uint32_t minimumCapacity =
        (std::max)(static_cast<uint32_t>(saved.entries.size()), COLLISION_TABLE_GROW_STEP);
    uint32_t requestedCapacity = (std::max)(savedCapacity, minimumCapacity);
    if (requestedCapacity <= liveCapacity) {
        requestedCapacity = liveCapacity + COLLISION_TABLE_GROW_STEP;
    }
    if ((requestedCapacity % COLLISION_TABLE_GROW_STEP) != 0) {
        requestedCapacity +=
            COLLISION_TABLE_GROW_STEP - (requestedCapacity % COLLISION_TABLE_GROW_STEP);
    }

    if (!CallCollisionTableGrowHelper(
            live.tableHeaderRegion.addr + COLLISION_TABLE_VECTOR_OFFSET,
            requestedCapacity)) {
        return false;
    }

    uint64_t rebuiltPtrArrayAddr = 0;
    uint32_t rebuiltCapacity = 0;
    if (!SafeReadPtr(live.tableHeaderRegion.addr + COLLISION_TABLE_POINTER_ARRAY_OFFSET,
                     &rebuiltPtrArrayAddr) ||
        rebuiltPtrArrayAddr == 0 ||
        !SafeRead<uint32_t>(live.tableHeaderRegion.addr + COLLISION_TABLE_CAPACITY_OFFSET,
                            &rebuiltCapacity) ||
        rebuiltCapacity < saved.entries.size()) {
        return false;
    }

    bool ok = true;
    std::vector<uint64_t> savedWrappers = DecodePointerArrayValues(saved.pointerArrayRegion);
    std::vector<CollisionRelocationSpan> spans;
    spans.push_back({
        saved.tableHeaderRegion.addr,
        saved.tableHeaderRegion.bytes.size(),
        live.tableHeaderRegion.addr,
    });
    spans.push_back({
        saved.pointerArrayRegion.addr,
        saved.pointerArrayRegion.bytes.size(),
        rebuiltPtrArrayAddr,
    });

    std::vector<uint8_t> tableFieldBytes(sizeof(uint64_t), 0);
    memcpy(tableFieldBytes.data(), &live.tableHeaderRegion.addr, sizeof(uint64_t));
    ok &= RestoreBytes(live.tableFieldAddr, tableFieldBytes, true);

    for (size_t i = 0; i < saved.entries.size(); i++) {
        const uint64_t destinationSecondaryAddr = saved.entries[i].secondaryRegion.addr;
        if (!saved.entries[i].secondaryRegion.bytes.empty() &&
            destinationSecondaryAddr != 0) {
            std::vector<uint8_t> secondaryBytes = saved.entries[i].secondaryRegion.bytes;
            RelocatePointerLikeQwords(&secondaryBytes, spans);
            ok &= RestoreBytes(destinationSecondaryAddr, secondaryBytes, false);
        }

        if (!saved.entries[i].wrapperRegion.bytes.empty() &&
            saved.entries[i].wrapperRegion.addr != 0) {
            std::vector<uint8_t> wrapperBytes = saved.entries[i].wrapperRegion.bytes;
            RelocatePointerLikeQwords(&wrapperBytes, spans);
            if (destinationSecondaryAddr != 0) {
                WriteU64LE(&wrapperBytes,
                           COLLISION_WRAPPER_SECONDARY_PTR_OFFSET,
                           destinationSecondaryAddr);
            }
            ok &= RestoreBytes(saved.entries[i].wrapperRegion.addr, wrapperBytes, true);
        }
    }

    std::vector<uint8_t> pointerArrayBytes(
        static_cast<size_t>(rebuiltCapacity) * sizeof(uint64_t),
        0);
    const size_t wrapperCount = (std::min)(saved.entries.size(), savedWrappers.size());
    for (size_t i = 0; i < wrapperCount; i++) {
        memcpy(pointerArrayBytes.data() + (i * sizeof(uint64_t)),
               &savedWrappers[i],
               sizeof(uint64_t));
    }
    ok &= RestoreBytes(rebuiltPtrArrayAddr, pointerArrayBytes, true);

    std::vector<uint8_t> headerBytes = saved.tableHeaderRegion.bytes;
    RelocatePointerLikeQwords(&headerBytes, spans);
    WriteU32LE(&headerBytes, COLLISION_TABLE_CAPACITY_OFFSET, rebuiltCapacity);
    WriteU64LE(&headerBytes, COLLISION_TABLE_POINTER_ARRAY_OFFSET, rebuiltPtrArrayAddr);
    ok &= RestoreBytes(live.tableHeaderRegion.addr, headerBytes, true);

    return ok;
}

static bool RestoreCollisionTable(const CollisionTableSnapshot& table) {
    if (table.tableHeaderRegion.bytes.empty()) {
        return true;
    }

    if (table.fighterAddr != 0 &&
        table.tableFieldAddr >= table.fighterAddr &&
        table.tableHeaderRegion.addr != 0 &&
        !table.pointerArrayRegion.bytes.empty()) {
        const size_t tableOffset =
            static_cast<size_t>(table.tableFieldAddr - table.fighterAddr);
        CollisionTableSnapshot liveTable;
        if (CaptureCollisionTable(table.fighterAddr, tableOffset, table.isHurtbox, &liveTable)) {
            const std::vector<uint64_t> savedWrappers =
                DecodePointerArrayValues(table.pointerArrayRegion);
            const std::vector<uint64_t> liveWrappers =
                DecodePointerArrayValues(liveTable.pointerArrayRegion);
            const bool preserveLiveChain =
                liveTable.tableHeaderRegion.addr != 0 &&
                liveTable.pointerArrayRegion.addr != 0 &&
                liveWrappers.size() >= table.entries.size() &&
                (liveTable.tableHeaderRegion.addr != table.tableHeaderRegion.addr ||
                 liveTable.pointerArrayRegion.addr != table.pointerArrayRegion.addr ||
                 PointerListsDiffer(savedWrappers, liveWrappers));

            if (preserveLiveChain) {
                return RestoreCollisionTablePreservingLiveChain(table, liveTable);
            }

            const bool missingLiveHitboxChain =
                !table.isHurtbox &&
                !table.entries.empty() &&
                liveTable.tableHeaderRegion.addr != 0 &&
                liveTable.countField == 0 &&
                liveTable.pointerArrayRegion.addr == 0;
            if (missingLiveHitboxChain) {
                if (RestoreCollisionTableRebuildingMissingLivePointerArray(table, liveTable)) {
                    return true;
                }
                return RestoreCollisionTablePreservingLiveState(liveTable);
            }
        }
    }

    bool ok = true;
    ok &= RestoreRegion(table.tableHeaderRegion);
    ok &= RestoreRegion(table.pointerArrayRegion, true);

    for (size_t i = 0; i < table.entries.size(); i++) {
        ok &= RestoreRegion(table.entries[i].secondaryRegion, false);
        ok &= RestoreRegion(table.entries[i].wrapperRegion, true);
    }

    return ok;
}

// ---- Projectile capture ----

static void CaptureProjectiles(GameSnapshot* snapshot) {
    snapshot->projectiles.clear();

    struct ShotListEntry { uint64_t addr; int listIndex; };
    ShotListEntry shotListAddrs[] = {
        {ADDR_sShotList, 0},
        {ADDR_P1Shots,   1},
        {ADDR_P2Shots,   2},
    };

    for (auto& entry : shotListAddrs) {
        uint64_t listHead = 0;
        if (!ReadStaticPointer(entry.addr, &listHead)) continue;

        uint64_t current = listHead;
        for (int count = 0; count < MAX_PROJECTILE_NODES && current != 0; count++) {
            ProjectileSnapshot proj;
            proj.nodeAddr = current;
            proj.listIndex = entry.listIndex;
            proj.nodeBytes.resize(SHOT_NODE_SNAPSHOT_SIZE);
            if (!SafeReadMem((void*)current, proj.nodeBytes.data(), SHOT_NODE_SNAPSHOT_SIZE))
                break;

            // Shot object pointer is at node + 0x10
            uint64_t shotObj = 0;
            if (SafeRead<uint64_t>(current + 0x10, &shotObj) && shotObj != 0 &&
                IsReadable(shotObj, SHOT_OBJECT_SNAPSHOT_SIZE)) {
                proj.shotAddr = shotObj;
                proj.shotBytes.resize(SHOT_OBJECT_SNAPSHOT_SIZE);
                SafeReadMem((void*)shotObj, proj.shotBytes.data(), SHOT_OBJECT_SNAPSHOT_SIZE);
            }

            snapshot->projectiles.push_back(std::move(proj));

            // Next node pointer is at offset 0x00
            uint64_t next = 0;
            if (!SafeRead<uint64_t>(current, &next) || next == current) break;
            current = next;
        }
    }
}

// ---- Public API ----

bool ResolveFighterPointers(uint64_t outAddrs[MAX_FIGHTERS]) {
    memset(outAddrs, 0, MAX_FIGHTERS * sizeof(uint64_t));

    // Fighter pointers live in the "mystery table" (gSound region),
    // NOT in sCharacter. The research DLL discovered this empirically.
    uint64_t mysteryTable = 0;
    if (!ReadStaticPointer(ADDR_gSound, &mysteryTable)) return false;

    for (int i = 0; i < MAX_FIGHTERS; i++) {
        SafeReadPtr(mysteryTable + FIGHTER_TABLE_OFFSET + (FIGHTER_TABLE_STRIDE * i), &outAddrs[i]);
    }

    // At least one fighter must be valid
    int validCount = 0;
    for (int i = 0; i < MAX_FIGHTERS; i++)
        if (outAddrs[i] != 0) validCount++;
    return validCount > 0;
}

bool CaptureSnapshot(GameSnapshot* snapshot) {
    if (!snapshot) {
        return false;
    }
    PrepareSnapshotStorage(snapshot);
    ResetSnapshotForCapture(snapshot);
    const uint64_t startMicros = QueryPerformanceMicros();

    if (!ResolveFighterPointers(snapshot->fighterAddrs))
        return false;

    // Fighters + collision tables
    for (int i = 0; i < MAX_FIGHTERS; i++) {
        if (snapshot->fighterAddrs[i] == 0) continue;

        if (!CaptureRegion(snapshot->fighterAddrs[i], FIGHTER_SNAPSHOT_SIZE, &snapshot->fighters[i]))
            return false;
        snapshot->totalBytes += snapshot->fighters[i].bytes.size();

        if (!CaptureCollisionTable(snapshot->fighterAddrs[i], FIGHTER_HITBOX_TABLE_OFFSET,
                                   false, &snapshot->hitboxes[i]))
            return false;
        if (!CaptureCollisionTable(snapshot->fighterAddrs[i], FIGHTER_HURTBOX_TABLE_OFFSET,
                                   true, &snapshot->hurtboxes[i]))
            return false;

        // Count collision bytes
        auto countCollision = [&](const CollisionTableSnapshot& t) {
            snapshot->totalBytes += t.tableHeaderRegion.bytes.size();
            snapshot->totalBytes += t.pointerArrayRegion.bytes.size();
            for (auto& e : t.entries) {
                snapshot->totalBytes += e.wrapperRegion.bytes.size();
                snapshot->totalBytes += e.secondaryRegion.bytes.size();
            }
        };
        countCollision(snapshot->hitboxes[i]);
        countCollision(snapshot->hurtboxes[i]);
    }

    // sAction core + teams
    uint64_t sAction = 0;
    if (ReadStaticPointer(ADDR_sAction, &sAction)) {
        if (CaptureRegion(sAction, MATCH_CORE_SNAPSHOT_SIZE, &snapshot->sActionCore))
            snapshot->totalBytes += snapshot->sActionCore.bytes.size();
        if (CaptureRegion(sAction + TEAM_1_OFFSET, TEAM_SNAPSHOT_SIZE, &snapshot->teams[0]))
            snapshot->totalBytes += snapshot->teams[0].bytes.size();
        if (CaptureRegion(sAction + TEAM_2_OFFSET, TEAM_SNAPSHOT_SIZE, &snapshot->teams[1]))
            snapshot->totalBytes += snapshot->teams[1].bytes.size();
    }

    // Mystery table (gSound region — captured by research DLL, purpose TBD)
    uint64_t mysteryTable = 0;
    if (ReadStaticPointer(ADDR_gSound, &mysteryTable)) {
        if (CaptureRegion(mysteryTable, MYSTERY_TABLE_SIZE, &snapshot->mysteryTableRegion))
            snapshot->totalBytes += snapshot->mysteryTableRegion.bytes.size();
    }

    // Game speed region
    uint64_t gameSpeedObj = 0;
    if (ResolveGameSpeedObject(&gameSpeedObj)) {
        if (CaptureRegion(gameSpeedObj, GAME_SPEED_REGION_SIZE, &snapshot->gameSpeedRegion))
            snapshot->totalBytes += snapshot->gameSpeedRegion.bytes.size();
    }

    // RNG seed
    uint64_t rngState = 0, rngSeed = 0;
    if (ResolveRngSeedRegion(&rngState, &rngSeed)) {
        snapshot->rngStateAddr = rngState;
        if (CaptureRegion(rngSeed, sizeof(uint32_t), &snapshot->rngSeedRegion))
            snapshot->totalBytes += snapshot->rngSeedRegion.bytes.size();
    }

    // Input buffers
    uint64_t inputBase = 0;
    if (ResolveInputBase(&inputBase)) {
        snapshot->inputBase = inputBase;
        if (CaptureRegion(inputBase, INPUT_BUFFER_SIZE, &snapshot->inputP1))
            snapshot->totalBytes += snapshot->inputP1.bytes.size();
        if (CaptureRegion(inputBase + INPUT_BUFFER_P2_OFFSET, INPUT_BUFFER_SIZE, &snapshot->inputP2))
            snapshot->totalBytes += snapshot->inputP2.bytes.size();
    }

    // Projectiles
    CaptureProjectiles(snapshot);
    for (auto& p : snapshot->projectiles) {
        snapshot->totalBytes += p.nodeBytes.size();
        snapshot->totalBytes += p.shotBytes.size();
    }

    snapshot->captureMicros = QueryPerformanceMicros() - startMicros;
    snapshot->valid = true;
    return true;
}

bool LoadSnapshot(const GameSnapshot& snapshot) {
    if (!snapshot.valid) return false;

    bool ok = true;

    // Restore in roughly reverse order of dependency
    ok &= RestoreRegion(snapshot.mysteryTableRegion, true);
    ok &= RestoreRegion(snapshot.sActionCore, true);
    ok &= RestoreRegion(snapshot.teams[0], true);
    ok &= RestoreRegion(snapshot.teams[1], true);
    ok &= RestoreRegion(snapshot.rngSeedRegion, true);
    ok &= RestoreRegion(snapshot.recordingDataState, true);

    // Fighters
    for (int i = 0; i < MAX_FIGHTERS; i++) {
        ok &= RestoreRegion(snapshot.fighters[i], true);
    }

    // Collision tables
    for (int i = 0; i < MAX_FIGHTERS; i++) {
        ok &= RestoreCollisionTable(snapshot.hitboxes[i]);
        ok &= RestoreCollisionTable(snapshot.hurtboxes[i]);
    }

    // Projectiles
    for (auto& p : snapshot.projectiles) {
        if (!p.shotBytes.empty() && p.shotAddr != 0 &&
            IsReadable(p.shotAddr, p.shotBytes.size())) {
            SafeWriteMem((void*)p.shotAddr, p.shotBytes.data(), p.shotBytes.size());
        }
        if (!p.nodeBytes.empty() && p.nodeAddr != 0 &&
            IsReadable(p.nodeAddr, p.nodeBytes.size())) {
            SafeWriteMem((void*)p.nodeAddr, p.nodeBytes.data(), p.nodeBytes.size());
        }
    }

    // Input buffers and game speed last (least critical for state correctness)
    ok &= RestoreRegion(snapshot.inputP1, true);
    ok &= RestoreRegion(snapshot.inputP2, true);
    ok &= RestoreRegion(snapshot.gameSpeedRegion, true);

    return ok;
}

uint64_t ChecksumSnapshot(const GameSnapshot& snapshot) {
    // FNV-1a 64-bit
    uint64_t hash = 0xCBF29CE484222325ULL;
    auto feed = [&](const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= 0x100000001B3ULL;
        }
    };
    auto feedRegion = [&](const MemoryRegion& r) {
        if (!r.bytes.empty()) feed(r.bytes.data(), r.bytes.size());
    };

    for (int i = 0; i < MAX_FIGHTERS; i++) feedRegion(snapshot.fighters[i]);
    feedRegion(snapshot.teams[0]);
    feedRegion(snapshot.teams[1]);
    feedRegion(snapshot.sActionCore);
    feedRegion(snapshot.rngSeedRegion);
    feedRegion(snapshot.inputP1);
    feedRegion(snapshot.inputP2);

    for (auto& p : snapshot.projectiles) {
        if (!p.shotBytes.empty()) feed(p.shotBytes.data(), p.shotBytes.size());
        if (!p.nodeBytes.empty()) feed(p.nodeBytes.data(), p.nodeBytes.size());
    }

    return hash;
}

} // namespace umvc3
