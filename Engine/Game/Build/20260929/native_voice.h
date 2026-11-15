#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::native_voice {
// Sound functions.
inline constexpr std::uintptr_t sound_create = 0x13f3b70;
inline constexpr std::array<unsigned char, 13> sound_create_prefix{
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa};
inline constexpr std::uintptr_t sound_destroy = 0x13f3d40;
inline constexpr std::array<unsigned char, 10> sound_destroy_prefix{
    0x48,0x8b,0xd1,0x48,0x8b,0x0d,0x7e,0x38,0xed,0x05};
inline constexpr std::uintptr_t sound_set_input = 0x13f4af0;
inline constexpr std::array<unsigned char, 12> sound_set_input_prefix{
    0x40,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x38,0x48,0x8b,0xd9};
// Voice sample provider: submit PCM for a source id, and remove a source.
inline constexpr std::uintptr_t samples_submit = 0x454350;
inline constexpr std::array<unsigned char, 12> samples_submit_prefix{
    0x48,0x85,0xd2,0x0f,0x84,0x79,0x02,0x00,0x00,0x48,0x8b,0xc4};
inline constexpr std::uintptr_t samples_remove = 0x45c240;
inline constexpr std::array<unsigned char, 14> samples_remove_prefix{
    0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x6c,0x24,0x20,0x56,0x57,0x41,0x56};

// The rate the voice sound takes its samples at. Its setup (0x44dae0) makes a resampler from
// two rates, each given by a function that loads one float: this one the samples' own rate,
// 16000.0 (the float at 0x607c584), and 0x458710 the output's, 48000.0 (the float 4 bytes on,
// at 0x607c588). It is called from nowhere else. Found in the decompile and read back from the
// game's file. With the load's displacement moved those 4 bytes (0x7c to 0x80) the sound takes
// 48 kHz samples; the 16000.0 itself is left alone, as other code may read it.
inline constexpr std::uintptr_t source_rate = 0x458e00;
inline constexpr std::array<unsigned char, 9> source_rate_prefix{
    0xc5,0xfa,0x10,0x05,0x7c,0x37,0xc2,0x05,0xc3};
inline constexpr std::array<unsigned char, 9> source_rate_48k{
    0xc5,0xfa,0x10,0x05,0x80,0x37,0xc2,0x05,0xc3};
inline constexpr std::uintptr_t output_rate_value = 0x607c588;

// Globals and vtables
// Pointer to the sound manager.
inline constexpr std::uintptr_t sound_manager = 0x72c75c8;
inline constexpr std::uintptr_t sound_manager_vtable = 0x61e1b38;
// Pointer to the voice sample provider.
inline constexpr std::uintptr_t samples_provider = 0x71e3ad8;
// Pointer to the voice spatializer; its voice sound asset is at +0x228.
inline constexpr std::uintptr_t spatializer = 0x71e3ae0;
inline constexpr std::uintptr_t voice_asset_vtable = 0x61dbdc8;

// Sound input types
inline constexpr std::uintptr_t source_input_type = 0x71ff2d0;
inline constexpr std::uintptr_t transform_input_type = 0x72d0ba0;
}
