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
// The encoder the replay editor exports with; the engine has others.
inline constexpr std::uintptr_t replay_encoder_vtable = 0x608c0b0;
// Encoder byte: a frame may be encoded. A world update sets it and an encoded
// frame clears it; submit_frame drops the frame it is called with while clear.
inline constexpr std::uintptr_t frame_permit = 0x90;
}
