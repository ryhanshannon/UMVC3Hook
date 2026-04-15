#pragma once
#include <cstdint>

// All addresses are virtual addresses with image base 0x140000000
// for the Steam version of UMvC3 (AppID 357190, x64).
// Use the _addr() helper from utils/addr.h to rebase at runtime.

namespace umvc3 {

// Image base for VA-to-RVA conversion
constexpr uint64_t IMAGE_BASE = 0x140000000;

// ---- Global static pointers ----
constexpr uint64_t ADDR_sCharacter     = 0x140D44A70;
constexpr uint64_t ADDR_sAction        = 0x140D47E68;
constexpr uint64_t ADDR_sShotList      = 0x140D47F98;
constexpr uint64_t ADDR_P1Shots        = 0x140D47FA0;
constexpr uint64_t ADDR_P2Shots        = 0x140D47FC8;
constexpr uint64_t ADDR_sBattleSetting = 0x140D50E58;
constexpr uint64_t ADDR_sGameConfig    = 0x140D50F88;
constexpr uint64_t ADDR_RecordingData  = 0x140D510A0;
constexpr uint64_t ADDR_gSound         = 0x140D533E0;
constexpr uint64_t ADDR_GameSpeed      = 0x140E177E8;
constexpr uint64_t ADDR_Camera         = 0x140E17930;
constexpr uint64_t ADDR_InputDisplay   = 0x140E1BC98;
constexpr uint64_t ADDR_GameName       = 0x140B12D10;

// ---- Function addresses ----
constexpr uint64_t ADDR_GetMvc3Manager  = 0x140001AF0;
constexpr uint64_t ADDR_CollisionTableGrow = 0x140011660;
constexpr uint64_t ADDR_InputHook       = 0x140289C5A;  // CALL site, once per frame
constexpr uint64_t ADDR_OrigInputFunc   = 0x1402B41B0;
constexpr uint64_t ADDR_BaseFighterTick = 0x14004BD30;
constexpr uint64_t ADDR_RngAccessor     = 0x1409AFAC0;
constexpr uint64_t ADDR_RngFunction     = 0x1409A5F9C;

// ---- Render/VFX function addresses (patched with 0xC3 for renderless sim) ----
struct RenderTarget {
    const char* name;
    uint64_t    va;
};

constexpr RenderTarget RENDER_TARGETS[] = {
    {"camera_update_tick", 0x140532D80},
    {"hud_render_1",       0x140326A70},
    {"hud_render_2",       0x140324240},
    {"full_hud_render",    0x1404537B0},
    {"visual_effects_1",   0x14077EA40},
    {"visual_effects_2",   0x14077D330},
};
constexpr size_t RENDER_TARGET_COUNT = sizeof(RENDER_TARGETS) / sizeof(RENDER_TARGETS[0]);

// ---- Struct sizes and offsets (from research DLL state map) ----
constexpr int MAX_FIGHTERS             = 6;
constexpr int FIGHTER_SNAPSHOT_SIZE     = 0x7000;
constexpr int TEAM_SNAPSHOT_SIZE        = 0x960;
constexpr int MATCH_CORE_SNAPSHOT_SIZE  = 0x8E0;
constexpr int MYSTERY_TABLE_SIZE        = 9200;
constexpr int INPUT_BUFFER_SIZE         = 550;
constexpr int INPUT_BUFFER_P2_OFFSET    = 0x2C0;
constexpr int GAME_SPEED_REGION_SIZE    = 0x90;
constexpr int SHOT_NODE_SNAPSHOT_SIZE   = 0x40;
constexpr int SHOT_OBJECT_SNAPSHOT_SIZE = 0x80;
constexpr int RECORDING_DATA_SIZE       = 0x100;
constexpr int MAX_PROJECTILE_NODES      = 64;
constexpr int MAX_COLLISION_TABLE_ENTRIES = 64;

// Fighter table (inside sCharacter) — offsets for pointer chain walk
constexpr int FIGHTER_TABLE_OFFSET = 0xAA0;
constexpr int FIGHTER_TABLE_STRIDE = 0x438;

// Collision table offsets (inside each fighter struct)
constexpr size_t FIGHTER_HITBOX_TABLE_OFFSET  = 0x4200;
constexpr size_t FIGHTER_HURTBOX_TABLE_OFFSET = 0x4E10;
constexpr size_t COLLISION_TABLE_VECTOR_OFFSET        = 0x18;
constexpr size_t COLLISION_TABLE_COUNT_OFFSET         = 0x20;
constexpr size_t COLLISION_TABLE_CAPACITY_OFFSET      = 0x24;
constexpr size_t COLLISION_TABLE_POINTER_ARRAY_OFFSET = 0x30;
constexpr size_t COLLISION_TABLE_HEADER_SIZE          = 0x40;
constexpr size_t COLLISION_WRAPPER_SECONDARY_PTR_OFFSET = 0x10;
constexpr size_t COLLISION_WRAPPER_DEFAULT_SIZE = 0x120;
constexpr size_t COLLISION_WRAPPER_MIN_SIZE     = 0x40;
constexpr size_t COLLISION_SECONDARY_SIZE       = 0x70;
constexpr uint32_t COLLISION_TABLE_GROW_STEP    = 0x20;

// Team offsets relative to sAction base
constexpr size_t TEAM_1_OFFSET = 0x350;
constexpr size_t TEAM_2_OFFSET = 0x610;

} // namespace umvc3
