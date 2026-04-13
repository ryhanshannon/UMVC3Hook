#pragma once
#include <cstdint>
#include <atomic>

namespace umvc3 {

// Frame synchronization and renderless simulation.
//
// The game calls an input-reading function at 0x140289C5A once per frame.
// We hook this call site to get a reliable frame boundary signal, which
// serves as the synchronization point for rollback operations.

// Install the input boundary hook. Must be called after the game is loaded.
// Returns false if the hook could not be installed.
bool InstallFrameBoundaryHook();

// Returns true if the frame boundary hook is active.
bool IsFrameBoundaryHookInstalled();

// Current frame boundary count (incremented each game frame).
uint64_t GetFrameBoundaryCount();

// ---- Renderless frame advance ----
// Patches render/VFX functions with RET (0xC3) to skip GPU work,
// then lets the game advance one simulation frame.

// Disable all render functions (patch with RET).
// Stores original bytes for later restoration.
// Returns false if any patch fails.
bool DisableRendering();

// Restore all render functions to their original bytes.
// Returns false if any restoration fails.
bool EnableRendering();

// Returns true if rendering is currently disabled.
bool IsRenderingDisabled();

} // namespace umvc3
