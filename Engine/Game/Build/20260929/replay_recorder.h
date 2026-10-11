#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>

// The game's rolling replay recording (Extension/Settings/replay_guard.cpp).
namespace dingosdk::game::build::v20260929::replay_recorder {
// end_chunk(recorder, force): run on a job worker as the game plays. Every recorder + 0x110
// frames, or when the stream (recorder + 0x950) is as full as recorder + 0x30 allows, it ends
// the chunk being recorded (recorder + 0xa80) with a "Tail" marker and starts the next: it has
// the stream drop old chunks (0x48f0e30), then asks it for the new chunk's head (0x48fb5a0, an
// allocation tagged "rplyFreezeClip"). It returns whether a chunk was ended (al).
inline constexpr Fingerprint end_chunk{0x48ef8a0, {
    0x40,0x53,0x55,0x57,0x48,0x81,0xec,0xc0,0x01,0x00,0x00,0x48,0x8b,0x05,0x0e,0x4b,
    0x8d,0x02,0x48,0x33,0xc4,0x48,0x89,0x84,0x24,0x90,0x01,0x00,0x00,0xc5,0xfb,0x10}};
// The store of the new head's first field, right after that allocation, with no check that the
// stream had room for one: an access violation writing 0x24 when it had none (crash reports of
// 2026-10-10, v2.0.3, skating on a server).
inline constexpr std::uintptr_t end_chunk_head_store = 0x48efa34;
}
