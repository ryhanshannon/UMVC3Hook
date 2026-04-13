#include "StateSnapshot.h"
#include "../utils/addr.h"

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

static bool ResolveInputBase(uint64_t* outBase) {
    // Input buffer base: read from the InputDisplay pointer chain.
    // InputDisplay -> points to a region containing P1 input buffer.
    uint64_t displayPtr = 0;
    if (!ReadStaticPointer(ADDR_InputDisplay, &displayPtr)) return false;
    *outBase = displayPtr;
    return true;
}

// ---- Collision table capture/restore ----

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

    if (count == 0 || count > 64) return true;  // Clamp sanity

    // Read pointer array
    size_t ptrArraySize = count * sizeof(uint64_t);
    uint64_t ptrArrayAddr = tableBase + COLLISION_TABLE_POINTER_ARRAY_OFFSET;
    if (!CaptureRegion(ptrArrayAddr, ptrArraySize, &out->pointerArrayRegion))
        return false;

    // Read each wrapper entry
    out->wrapperSize = isHurtbox ? COLLISION_WRAPPER_DEFAULT_SIZE : COLLISION_WRAPPER_DEFAULT_SIZE;
    out->entries.resize(count);
    out->capturedCount = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t wrapperPtr = 0;
        if (!SafeRead<uint64_t>(ptrArrayAddr + i * sizeof(uint64_t), &wrapperPtr) || wrapperPtr == 0)
            continue;

        size_t wSize = out->wrapperSize;
        if (!IsReadable(wrapperPtr, wSize)) {
            wSize = COLLISION_WRAPPER_MIN_SIZE;
            if (!IsReadable(wrapperPtr, wSize)) continue;
        }

        if (!CaptureRegion(wrapperPtr, wSize, &out->entries[i].wrapperRegion))
            continue;

        // Secondary region at wrapper + 0x10
        uint64_t secondaryPtr = 0;
        if (SafeRead<uint64_t>(wrapperPtr + COLLISION_WRAPPER_SECONDARY_PTR_OFFSET, &secondaryPtr) &&
            secondaryPtr != 0 && IsReadable(secondaryPtr, COLLISION_SECONDARY_SIZE)) {
            CaptureRegion(secondaryPtr, COLLISION_SECONDARY_SIZE, &out->entries[i].secondaryRegion);
        }

        out->capturedCount++;
    }

    return true;
}

static bool RestoreCollisionTable(const CollisionTableSnapshot& table) {
    if (table.tableHeaderRegion.bytes.empty()) return true;

    bool ok = true;
    ok &= RestoreRegion(table.tableHeaderRegion);
    ok &= RestoreRegion(table.pointerArrayRegion);

    for (size_t i = 0; i < table.entries.size(); i++) {
        if (!table.entries[i].wrapperRegion.bytes.empty())
            ok &= RestoreRegion(table.entries[i].wrapperRegion);
        if (!table.entries[i].secondaryRegion.bytes.empty())
            ok &= RestoreRegion(table.entries[i].secondaryRegion);
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

    uint64_t sChar = 0;
    if (!ReadStaticPointer(ADDR_sCharacter, &sChar)) return false;

    uint64_t tableBase = sChar + FIGHTER_TABLE_OFFSET;

    for (int i = 0; i < MAX_FIGHTERS; i++) {
        uint64_t entryAddr = tableBase + i * FIGHTER_TABLE_STRIDE;
        uint64_t fighterPtr = 0;
        if (SafeReadPtr(entryAddr, &fighterPtr) && fighterPtr != 0 &&
            IsReadable(fighterPtr, FIGHTER_SNAPSHOT_SIZE)) {
            outAddrs[i] = fighterPtr;
        }
    }

    return true;
}

bool CaptureSnapshot(GameSnapshot* snapshot) {
    *snapshot = GameSnapshot{};
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
