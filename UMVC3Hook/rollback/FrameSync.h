#pragma once
#include <cstdint>
#include <atomic>

namespace umvc3 {

// Frame synchronization and renderless simulation.
//
// The game calls an input-reading function at 0x140289C5A once per frame.
// We hook this call site to get a reliable frame boundary signal, which
// serves as the synchronization point for rollback operations.
//
// CRITICAL: Save/load MUST happen at the frame boundary, not from arbitrary
// threads. The game's simulation thread is between frames only at this hook
// point. Calling CaptureSnapshot/LoadSnapshot from another thread races with
// the simulation and WILL eventually crash.
//
// Use RequestSave/RequestLoad to queue operations that execute safely at
// the next frame boundary.

// Install the input boundary hook. Must be called after the game is loaded.
// Returns false if the hook could not be installed.
bool InstallFrameBoundaryHook();

// Returns true if the frame boundary hook is active.
bool IsFrameBoundaryHookInstalled();

// Current frame boundary count (incremented each game frame).
uint64_t GetFrameBoundaryCount();

// ---- Frame-boundary-safe save/load ----
// These queue a command that executes at the next frame boundary.
// They block the calling thread until completion (up to ~500ms timeout).
// Returns true if the operation succeeded.

enum class FrameCommandResult {
    Success,
    Failed,
    Timeout,
    NotInstalled,
};

constexpr uint32_t SNAPSHOT_RING_SIZE = 16;

FrameCommandResult RequestSave();
FrameCommandResult RequestLoad();
FrameCommandResult RequestLoadFramesAgo(uint32_t framesAgo);

// Access the last snapshot (owned by the frame boundary thread).
// Only valid after a successful RequestSave().
struct GameSnapshot; // forward decl
const GameSnapshot& GetLastSnapshot();
uint32_t GetStoredSnapshotCount();
uint32_t GetSnapshotRingCapacity();
int32_t GetLastSnapshotSlotIndex();
uint64_t GetLastSnapshotChecksum();
uint64_t GetLastLoadMicros();

// ---- Renderless frame advance ----

bool DisableRendering();
bool EnableRendering();
bool IsRenderingDisabled();

} // namespace umvc3
