#pragma once
#include "engine.h"
#include <cstdint>

namespace dingosdk::game::build::v20260929::throwdown_debug_text {
// Fast thunks the loader puts in a graph's native slots (one pointer per operand).
// DebugDrawText2D (0x056455c2).
inline constexpr std::uintptr_t draw_text_fast = 0x5af0ec0;
// GetPlayerName (0x33c6ca2a), and the native it wraps.
inline constexpr std::uintptr_t player_name_fast = 0x5955a10;
inline constexpr std::uintptr_t player_name_raw = 0x821100;
// GetDebugScreenWidth (0x72823522) and GetDebugScreenHeight (0x2e18a002) share
// one retail stub: void(uint32* out).
inline constexpr std::uintptr_t debug_screen_size_fast = 0x597e720;
// PostEventDelayedFrames, which the HUD's own re-post is bound to.
inline constexpr std::uintptr_t post_frames_fast = engine::post_event_delayed_frames;
// ClientSkateThrowdown.OnDebugPrint re-posts itself for the next frame: PostEventDelayedFrames
// (hash 0x9967fc07) at pc 0xad2c, whose word 0 is 0xad6004, operands S0 (self), payload, port
// offset, payload type and the frame count K1c8c (1). The HUD patches that call's slot.
inline constexpr std::uint32_t repost_hash = 0x9967fc07, repost_pc = 0xad2c, repost_word = 0xad6004,
                               repost_frames_constant = 0x1c8c;
}
