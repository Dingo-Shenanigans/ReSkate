#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstdint>
namespace dingosdk::game::build::v20260929::trick_names {
// Shipped UI/Features/HUD/Scoring/TrickHistoryItemViewModel, verified against
// the loaded type registry of the supported executable. Label.Text is UTF-8
// or a localization key; rows also hold the separate stance/spin labels.
inline constexpr std::uint32_t row_schema = 0xbf5e8a16, label_schema = 0xbfe23948,
    label_text = 0x4d8e01b9;
// const char* getString(uint32 idHash, bool useFallback). The native bounded
// copy at 0x33230b0 consumes this as char bytes. Called only on the client
// thread, copied synchronously; neither the text nor the database is owned.
inline constexpr Fingerprint localized_string{0x3322fd0, {
    0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x05,0xd7,0x84,0x2f,
    0x04,0x0f,0xb6,0xfa,0x8b,0xd9,0x48,0x85,0xc0,0x74,0x04,0xff,0xd0,0x8b,0xd8,0xe8}};
}
