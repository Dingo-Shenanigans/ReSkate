#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>

namespace dingosdk::game::build::v20260929::ui_model {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// The UI model's write of one value (manager, handle, type, value bytes, flag; true when written),
// found on 2026-10-07. The game's widget logic is data (expression functions: no native code names
// the fields), so its writes land here too. The publish the native menu reader uses (0x1924850,
// which takes the model lock at manager + 0xf20 around it) and the model's other writers call it:
// the array element writer (0x1921e80), the path setter (0x1924570) and the data model setter
// (0x1923480), each under the same lock.
inline constexpr Fingerprint model_write{0x19248b0, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x55,0x57,
    0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48,0x81,0xec,0x80,0x00,0x00,0x00,0x4d,0x8b,0xe1,0x49}};
}
