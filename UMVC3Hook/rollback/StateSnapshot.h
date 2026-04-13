#pragma once
#include "Memory.h"
#include "Addresses.h"
#include <vector>

namespace umvc3 {

// Snapshot of one collision table (hitbox or hurtbox) for a single fighter.
struct CollisionEntrySnapshot {
    MemoryRegion wrapperRegion;
    MemoryRegion secondaryRegion;
};

struct CollisionTableSnapshot {
    uint64_t fighterAddr = 0;
    uint64_t tableFieldAddr = 0;
    bool     isHurtbox = false;
    uint32_t countField = 0;
    uint32_t capturedCount = 0;
    size_t   wrapperSize = 0;
    MemoryRegion tableHeaderRegion;
    MemoryRegion pointerArrayRegion;
    std::vector<CollisionEntrySnapshot> entries;
};

struct ProjectileSnapshot {
    uint64_t nodeAddr = 0;
    uint64_t shotAddr = 0;
    int      listIndex = -1;
    std::vector<uint8_t> nodeBytes;
    std::vector<uint8_t> shotBytes;
};

// Complete game state snapshot — everything needed for rollback save/load.
// Sized at ~213KB as of current state mapping (Phase 2B).
struct GameSnapshot {
    bool     valid = false;
    uint64_t frameCounter = 0;  // Input boundary count at capture time

    // Per-fighter state (6 fighters: P1 point/A1/A2, P2 point/A1/A2)
    uint64_t     fighterAddrs[MAX_FIGHTERS] = {};
    MemoryRegion fighters[MAX_FIGHTERS];
    CollisionTableSnapshot hitboxes[MAX_FIGHTERS];
    CollisionTableSnapshot hurtboxes[MAX_FIGHTERS];

    // Team and match state
    MemoryRegion teams[2];
    MemoryRegion sActionCore;
    MemoryRegion mysteryTableRegion;
    MemoryRegion gameSpeedRegion;

    // RNG
    uint64_t     rngStateAddr = 0;
    MemoryRegion rngSeedRegion;

    // Input buffers
    uint64_t     inputBase = 0;
    MemoryRegion inputP1;
    MemoryRegion inputP2;

    // Recording data (training mode)
    MemoryRegion recordingDataState;

    // Projectiles (variable count)
    std::vector<ProjectileSnapshot> projectiles;

    // Metrics
    size_t   totalBytes = 0;
    uint64_t captureMicros = 0;
};

// ---- Core API (maps to GGPO callbacks) ----

// Resolve the 6 fighter pointers from sCharacter.
// Returns false if sCharacter is null or unreadable.
bool ResolveFighterPointers(uint64_t outAddrs[MAX_FIGHTERS]);

// Capture the full game state into a snapshot.
// This is the save_game_state callback for GGPO.
bool CaptureSnapshot(GameSnapshot* snapshot);

// Restore a previously captured snapshot back to game memory.
// This is the load_game_state callback for GGPO.
// Returns false if the snapshot is invalid or any write fails.
bool LoadSnapshot(const GameSnapshot& snapshot);

// Compute a checksum of the snapshot for desync detection.
// Uses FNV-1a over the concatenated state bytes.
uint64_t ChecksumSnapshot(const GameSnapshot& snapshot);

// Called by FrameSync to cache the input buffer base address
// discovered from the frame boundary hook's parameter.
void SetInputBufferBase(uint64_t addr);

} // namespace umvc3
