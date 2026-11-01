#pragma once
#include "Engine/Game/Build/fingerprint.h"

namespace dingosdk::game::build::v20260929::replay_activity {
// Supported SHA-256 fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9.
// Client ctor 50c1f0 publishes the singleton and creates the session manager
// at +568 with backend +50 (vtable 6084518). 6240e0/6241e0 install a lease
// at manager+8; the lease's +10 points back to that manager.
inline constexpr std::uintptr_t client = 0x71eaba8, manager = 0x568;
inline constexpr std::uintptr_t backend = 0x50, backend_vtable = 0x6084518;
inline constexpr std::uintptr_t lease = 8, lease_manager = 0x10, lease_vtable = 0x60ca700;
// Backend +88 -> 6123b0 -> 41a6960 reads the playhead at 718ffd0 when
// 7726468 is nonzero. Backend +90 -> 6254b0 -> 41a9f40 writes this time.
inline constexpr std::uintptr_t playing = 0x7726468, playhead = 0x718ffd0;
}
