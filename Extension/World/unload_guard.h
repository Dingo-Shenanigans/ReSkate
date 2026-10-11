#pragma once
#include <cstdint>
#include <string>

namespace dingosdk {
// Patches the game's level unload so an entry with no asset is skipped, not read (see
// Engine/Game/Build/20260929/unload_guard.h). Call before any level loads. False with `error`
// when the game's code is not what the patch was written for; the game then runs as shipped.
bool start_unload_guard(std::uintptr_t base, std::string& error) noexcept;
}
