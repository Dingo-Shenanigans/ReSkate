// Reviewed native contract for supported_build::game_sha256 (September 29 2026).
// DLSS frame generation's options, built for NVIDIA Streamline each frame.
#pragma once

#include <array>
#include <cstdint>

namespace dingosdk::game::build::v20260929::frame_generation {
// The frame generation update (0x03f224b0) fills a DLSSGOptions on its stack every frame and passes
// it to slDLSSGSetOptions. Its mode is off for any frame where one of its own checks fails, which
// happens in bursts for 10 to 15 s after every map load and now and then while riding, on every map
// tried (2026-11-11, an RTX 5070). It sets the flags' "retain resources when off" bit (1 << 3)
// only when a byte of its object (+0x180) is set, which it is not, so each one-frame off frees
// frame generation's resources and the next frame makes them again: a stall of about 220 ms each
// time, and after a map change the game kept freezing until frame generation was turned off and
// on. The mode it asks for when its checks pass is the dword at +0x17c of the same object, copied
// into the options at 0x03f22595. The game's
// sl.dlss_g.dll (Streamline 2.11.1) names the values in its mode-to-text switch: 0 eOff, 1 eOn,
// 2 eAuto, 3 eDynamic. The game passes 3 on only when a byte of its settings (+0x2e3) is set, with a
// number from another object as a float beside it (likely dynamic frame generation's target frame
// rate), and otherwise turns it into 1; 0 to 2 go through as they are.
// At the flag:
//   movzx eax, byte ptr [rbx + 0x180]   ; the retain byte
//   neg al / sbb ecx, ecx / and ecx, 8 / or dword ptr [rbp - 0x21], ecx   (left as they are)
// No branch lands inside the movzx (the jumps above all land on its first byte).
inline constexpr std::uintptr_t retain_flag_rva = 0x03f225e8;
inline constexpr std::uint32_t mode_offset = 0x17c;
inline constexpr std::array<std::uint8_t, 7> retain_flag_prefix{0x0f, 0xb6, 0x83, 0x80, 0x01, 0x00, 0x00};
} // namespace dingosdk::game::build::v20260929::frame_generation
