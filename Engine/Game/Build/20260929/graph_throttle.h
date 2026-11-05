#pragma once
#include "engine.h"
#include <cstdint>

namespace dingosdk::game::build::v20260929::graph_throttle {
// What a loaded graph's delayed re-post slots are bound to before the throttle
// takes them over.
inline constexpr std::uintptr_t post_seconds_fast = engine::post_event_delayed_seconds;
inline constexpr std::uintptr_t post_frames_fast = engine::post_event_delayed_frames;

// The two re-post natives' hashes in a graph's native call table.
//   PostEventDelayedSeconds 0x5e3aee8e: self, event, port offset, payload, float seconds.
//   PostEventDelayedFrames  0x9967fc07: self, payload, port offset, payload type, int frames.
inline constexpr std::uint32_t post_seconds_hash = 0x5e3aee8e, post_frames_hash = 0x9967fc07;

// Each graph as shipped (read from a res_all dump; the feature checks every one of these values
// before it patches, so a graph that differs is refused): header +0x20 frame bytes, +0x24
// constant bytes, +0x2c code words; code at code_offset with its first word; the native
// call table of {hash, pc} pairs. The graph calls `native` `sites` times; the one patched is
// at `pc` (word 0 = next << 8 | 4: four operands and the delay), with S0 as its first operand
// and the zero delay constant K`delay` as its fifth.
struct Repost {
    std::uint32_t hash;
    const char* name;
    std::uint32_t frame, constants, code_words, code_offset, first_word, table_offset, table_count;
    std::uint32_t native, sites, pc, next, delay;
};
// Re-posts at pc 0x38, then GetBoolSetting("CommunityEvent.DebugDrawQueues") (a retail stub:
// its default, false) sends it to RETURN.
inline constexpr Repost coordinator_debug_draw{0xe300874d, "MPActivityCoordinator.OnDebugDrawUpdate", 0x90, 0x1c0, 0x309,
    0x360, 0xc27, 0xfb4, 48, post_seconds_hash, 1, 0x38, 0x6c, 0xec};
// Re-posts at pc 0x38, then returns while field +0x50 is clear (every traced run).
inline constexpr Repost input_visualization{0xe399e493, "InputVisualization_Dev.OnUpdate", 0x40, 0x158, 0x2ce, 0x248,
    0xc27, 0xd90, 63, post_seconds_hash, 1, 0x38, 0x6c, 0xcc};
// Re-posts at pc 0x38; its per-entry print loop needs field +0x118 and the retail-stubbed
// "DingoActivity.DrawActivityDebug".
inline constexpr Repost leaderboard_debug_print{0xf5a89f23, "ActivityLeaderboardManager.OnDebugPrintLoop", 0x70, 0x1d8,
    0x2b7, 0x3a8, 0xc27, 0xebc, 49, post_seconds_hash, 1, 0x38, 0x6c, 0x110};
// Play Event failed (pc 0x20 -> 0x2454): logs "…retrying for {0}" and re-posts the
// activation at pc 0x24ec. Its other re-post (pc 0x608, success path) is another call.
inline constexpr Repost narrative_play_retry{0xa4becd75, "NarrativeSet Play Event retry", 0xb0, 0x888, 0x949, 0xa58,
    0x102e, 0x3044, 167, post_seconds_hash, 2, 0x24ec, 0x2520, 0x35c};
// Not IsOnline && IsSessionReady (pc 0x84 -> 0xadc): asks again at pc 0xadc.
inline constexpr Repost quick_drop_entitlements{0xf64d93b7, "QuickDropPersistenceManager.OnWaitForEntitlements", 0x60,
    0x210, 0x2c5, 0x3b0, 0xc27, 0xee4, 52, post_seconds_hash, 1, 0xadc, 0xb10, 0x124};
// Not ready (pc 0x198 -> 0x4f8): asks again at pc 0x4f8. Its ready re-post (pc 0x4c4) is another call.
inline constexpr Repost milestone_init{0x7fab7dbd, "MilestoneManager.OnInitLoop", 0x40, 0x100, 0x14c, 0x210, 0xc27,
    0x750, 26, post_seconds_hash, 2, 0x4f8, 0x52c, 0xa8};
// Not IsOnline && quests present (pc 0x84 -> 0xc8): asks again next frame at pc 0xc8. Its
// ready branch (pc 0x94, two frames) is another call.
inline constexpr Repost quest_delayed_init{0xa24d0261, "QuestManager.OnDelayedInit", 0x40, 0x28, 0x40, 0xb8, 0xc27,
    0x1b8, 5, post_frames_hash, 2, 0xc8, 0xfc, 0x1c};
}
