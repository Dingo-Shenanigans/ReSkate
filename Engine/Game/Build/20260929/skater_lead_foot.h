#pragma once
#include <cstdint>

namespace dingosdk::game::build::v20260929::skater_lead_foot {
// Supported game SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// Context +0x13c4 bit 5 names the leading foot, not switch stance. Measured
// on goofy and regular riders (2026-10-20); retained when the old grind-exit
// module was removed from ReSkate 2.1.2. Base stance comes from the profile.
inline constexpr std::uintptr_t stance_flags_offset = 0x13c4;
inline constexpr std::uint8_t left_foot_leads_mask = 0x20;
} // namespace dingosdk::game::build::v20260929::skater_lead_foot
