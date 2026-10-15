#pragma once
#include <array>
#include <cstdint>

// The replay editor's video export (analysis/replay-editor.md).
namespace dingosdk::game::build::v20260929::replay_export {
// The engine movie encoder's frame submit: (encoder, pixels, pitch). The render
// thread calls it at the end of every rendered frame while a capture runs, and
// it encodes the frame, video and its share of audio, before it returns.
inline constexpr std::uintptr_t submit_frame = 0x34f4610;
inline constexpr std::array<unsigned char, 24> submit_frame_prefix{
    0x40,0x57,0x48,0x81,0xec,0x50,0x08,0x00,0x00,0x48,0x8b,0x05,0xa0,0xfd,0xcc,0x03,
    0x48,0x33,0xc4,0x48,0x89,0x84,0x24,0x20};
// Maps the export dialog's 8/12/18 Mbps selection to the H.264 encoder rate.
// Raise all three outputs so export cannot silently fall back to the low-bitrate
// preset. The replacement rate is 100 Mbps (100000 kbps).
inline constexpr std::uintptr_t bitrate_switch = 0x62c240;
inline constexpr std::array<unsigned char, 34> bitrate_switch_prefix{
    0x2b,0x85,0xc9,0x74,0x18,0x83,0xe9,0x01,0x74,0x05,0x83,0xf9,0x01,0x74,0x07,0xb8,
    0xe0,0x2e,0x00,0x00,0xeb,0x0c,0xb8,0x50,0x46,0x00,0x00,0xeb,0x05,0xb8,0x40,0x1f,
    0x00,0x00};
inline constexpr std::array<std::size_t, 3> bitrate_immediate_offsets{16, 23, 30};
inline constexpr std::array<std::uint32_t, 3> stock_bitrates_kbps{12000, 18000, 8000};
inline constexpr std::uint32_t replay_bitrate_kbps = 100000;
// The encoder the replay editor exports with; the engine has others.
inline constexpr std::uintptr_t replay_encoder_vtable = 0x608c0b0;
// Encoder byte: a frame may be encoded. A world update sets it and an encoded
// frame clears it; submit_frame drops the frame it is called with while clear.
inline constexpr std::uintptr_t frame_permit = 0x90;
}
