#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstddef>
#include <cstdint>

// The game's sound engine (EA::Audio::Core): the plugin that plays a streamed sound file
// ("SndPlayer1"), which is what the music player's songs come out of.
namespace dingosdk::game::build::v20260929::audio_core {
// SndPlayer1's processing routine: (player, mixer) -> nonzero when it produced a block.
inline constexpr Fingerprint stream_player_process{0x13de2b0, {0x40,0x53,0x41,0x55,0x48,0x83,0xec,0x58,0x80,0xb9,0x24,0x03,0x00,0x00,0x00,0x4c,0x8d,0x91,0x23,0x03,0x00,0x00,0x4c,0x89,0x54,0x24,0x20,0x4c,0x8b,0xea,0x48,0x8b}};
// SndPlayer1's command routine: (player, command, arguments). Command 4 asks it to play a
// file, whose name is the string at arguments + 0x18.
inline constexpr Fingerprint stream_player_command{0x13e2190, {0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x20,0x4c,0x8b,0x71,0x18,0x49,0x8b,0xf0,0x48,0x8b,0xf9,0x83,0xfa,0x04,0x0f,0x85,0x13,0x01,0x00,0x00,0x48,0x8b}};
inline constexpr int command_play = 4;
inline constexpr std::size_t command_play_name = 0x18;

// The player.
inline constexpr std::size_t player_block_frames = 0x318; // u16: the most frames of one block
inline constexpr std::size_t player_voices = 0x31c;       // u16: where its voices begin
inline constexpr std::size_t player_voice = 0x321;        // u8: the voice being played
inline constexpr std::size_t player_voice_count = 0x322;  // u8
inline constexpr std::size_t voice_size = 0x168;
inline constexpr std::size_t voice_rate = 0x14;           // float, Hz
inline constexpr std::size_t voice_frames = 0x18;         // u32: the length of the file
inline constexpr std::size_t voice_state = 0x2e;          // u8: 0 and 5 are not playing
inline constexpr std::size_t voice_channels = 0x2f;       // u8

// The mixer. A block is written to the buffer at mixer_write, and the two are then exchanged:
// after the routine, what it wrote is the buffer at mixer_written.
inline constexpr std::size_t mixer_written = 0x10090;
inline constexpr std::size_t mixer_write = 0x10098;
inline constexpr std::size_t mixer_frames = 0x100bc;      // u32: frames of the block
inline constexpr std::size_t mixer_rate = 0x100c0;        // float
inline constexpr std::size_t mixer_channels = 0x100c8;    // u8
// A buffer: 32-bit float samples, a channel after another.
inline constexpr std::size_t buffer_data = 0x8;           // channel pointers, or the samples
inline constexpr std::size_t buffer_stride = 0x12;        // u16: samples a channel, when together
inline constexpr std::size_t buffer_together = 0x15;      // u8: nonzero when buffer_data is the samples
}
