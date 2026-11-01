#pragma once
#include "Extension/Multiplayer/Net/protocol.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace dingosdk::multiplayer {
// Audited against the native copy/difference functions
// (addr::native_audio::frame_copy and frame_difference).
// Only values cross the wire. Matrices come from the buffered peer pose;
// native padding, dirty masks, handles and asset pointers never cross it.
inline constexpr std::array<std::uint16_t, 49> audio_float_offsets{
    0xb0,  0xb4,  0xb8,  0xc4,  0xd0,  0xd4,  0xd8,  0xdc,  0xe0,  0xe8,  0xec,  0xf0,  0xf4,
    0xfc,  0x100, 0x104, 0x108, 0x10c, 0x110, 0x11c, 0x120, 0x128, 0x12c, 0x134, 0x140, 0x144,
    0x148, 0x154, 0x160, 0x16c, 0x170, 0x178, 0x184, 0x188, 0x194, 0x198, 0x19c, 0x1a4, 0x1ac,
    0x1b0, 0x1b8, 0x1c0, 0x1c4, 0x1c8, 0x1d0, 0x1d4, 0x1dc, 0x1e0, 0x1e4};
inline constexpr std::array<std::uint16_t, 26> audio_selector_offsets{
    0xbc,  0xc8,  0xcc,  0xe4,  0xf8,  0x114, 0x118, 0x138, 0x13c, 0x14c, 0x150, 0x158, 0x15c,
    0x168, 0x174, 0x17c, 0x180, 0x18c, 0x190, 0x1a0, 0x1a8, 0x1b4, 0x1bc, 0x1cc, 0x1d8, 0x1e8};
// The selectors the skater sound update (0x44b430) hands to the physics-material lookups (0x7111c0,
// 0x45e120): packed material words, flags in bits 0-5, the material slot in 6-18, the property slot
// in 19-31 (Engine/Resource/material_grid.h). Neither lookup checks the slot against the grid.
inline constexpr std::array<std::uint16_t, 18> audio_material_offsets{
    0xc8,  0xcc,  0xe4,  0x114, 0x118, 0x138, 0x13c, 0x150, 0x15c,
    0x168, 0x17c, 0x18c, 0x190, 0x1a8, 0x1b4, 0x1bc, 0x1d8, 0x1e8};
// The root level's own grid (DingoLevel_Root materialgrid_win32) has these slots on every install.
// A Studio map's surfaces are appended after them, numbered by each install's own set of map mods,
// so the same word names another surface, or one past the end of the grid, on the other side
// (Skate.exe+0x41da7b and +0x6fc93b, a remote skater on a Studio map's surface; 2026-10-25).
inline constexpr std::uint32_t base_material_slots = 116;
// A material word as every install reads it the same: one of the map's own surfaces becomes the
// grid's default (0), as a sound on that surface cannot be named portably.
inline std::uint32_t portable_material(std::uint32_t word) noexcept {
    return ((word >> 6) & 0x1fff) < base_material_slots && (word >> 19) < base_material_slots ? word : 0;
}
inline bool audio_material(std::uint16_t offset) noexcept {
    return std::find(audio_material_offsets.begin(), audio_material_offsets.end(), offset) != audio_material_offsets.end();
}
struct alignas(16) NativeAudioFrame {
    std::array<std::byte, 0x220> bytes{};
};
template <class T> T audio_value(const NativeAudioFrame &f, std::size_t offset) {
    T out{};
    std::memcpy(&out, f.bytes.data() + offset, sizeof(out));
    return out;
}
template <class T> void audio_put(NativeAudioFrame &f, std::size_t offset, const T &v) {
    std::memcpy(f.bytes.data() + offset, &v, sizeof(v));
}
inline AudioState audio_state(const NativeAudioFrame &frame) {
    AudioState out;
    for (std::size_t i = 0; i < 9; ++i)
        out.values[i] = audio_value<float>(frame, 0x80 + (i / 3) * 16 + (i % 3) * 4);
    for (std::size_t i = 0; i < audio_float_offsets.size(); ++i)
        out.values[9 + i] = audio_value<float>(frame, audio_float_offsets[i]);
    for (std::size_t i = 0; i < audio_selector_offsets.size(); ++i) {
        const auto value = audio_value<std::uint32_t>(frame, audio_selector_offsets[i]);
        out.selectors[i] = audio_material(audio_selector_offsets[i]) ? portable_material(value) : value;
    }
    std::memcpy(out.flags.data(), frame.bytes.data() + 0x1ec, out.flags.size());
    out.flags[0] = 0;
    if (!valid_audio(out))
        throw std::runtime_error("Native skater sound values are unavailable.");
    return out;
}
inline NativeAudioFrame audio_frame(const AudioState &state, const Pose &pose) {
    // Playback admission validates every bone. Audio only consumes the two
    // world anchors, so avoid a full skeleton walk for every sound update.
    if (!valid_audio(state) || !valid_transform(pose.root) ||
        (!pose.board.empty() && !valid_transform(pose.board.front())))
        throw std::runtime_error("Peer sound state is invalid.");
    NativeAudioFrame out;
    audio_put(out, 0, to_matrix(pose.root));
    audio_put(out, 0x40, to_matrix(pose.board.empty() ? pose.root : pose.board.front()));
    for (std::size_t i = 0; i < 9; ++i)
        audio_put(out, 0x80 + (i / 3) * 16 + (i % 3) * 4, state.values[i]);
    for (std::size_t i = 0; i < audio_float_offsets.size(); ++i)
        audio_put(out, audio_float_offsets[i], state.values[9 + i]);
    // Older clients send their own slot numbers: they are made portable here too.
    for (std::size_t i = 0; i < audio_selector_offsets.size(); ++i)
        audio_put(out, audio_selector_offsets[i],
                  audio_material(audio_selector_offsets[i]) ? portable_material(state.selectors[i]) : state.selectors[i]);
    std::memcpy(out.bytes.data() + 0x1ec, state.flags.data(), state.flags.size());
    for (const auto off : {0x164, 0x130, 0x124})
        audio_put(out, off, UINT32_MAX);
    audio_put(out, 0xc0, std::uint32_t{0x07ffffff});
    return out;
}
} // namespace dingosdk::multiplayer
