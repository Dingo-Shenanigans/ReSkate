#pragma once
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/local_music.h"
#include <cstdint>

namespace dingosdk::profile_runtime {
// The root installs its construct hook from this contract.
using addr::local_music::music_model_construct_contract;
using MusicModelConstruct = std::uint64_t (*)(std::uintptr_t, std::uint8_t, std::uint64_t,
    std::uintptr_t, bool, std::uintptr_t);
extern MusicModelConstruct music_model_construct_original;
std::uint64_t music_model_construct_hook(std::uintptr_t manager, std::uint8_t mode,
    std::uint64_t id, std::uintptr_t type, bool flag, std::uintptr_t record);
void update_music_shelf();
void music_shelf_before_level_transition(unsigned next) noexcept;
} // namespace dingosdk::profile_runtime
