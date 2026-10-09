#pragma once
#include "Extension/Profile/runtime_internal.h"
#include "Engine/Game/World/park_randomization.h"

namespace dingosdk::profile_runtime {
struct ParkRuntime {
    using Tick = void (*)(std::uintptr_t, float);
    using Construct = void (*)(std::uintptr_t);
    using Notify = void (*)(std::uintptr_t, const NativeString*, const NativeString*);
    Tick tick{};
    Construct construct{};
    Notify notify{};
    Notify original_notify{};
    std::atomic<bool> active{};
    ParksModel model;
    ParkLaunchRandomization launch_randomization;
    ParkChoices sent;
    bool lobby_active{};
    std::array<bool, park_lots.size()> clear_unset{};
    std::uintptr_t owner{}, context{};
    unsigned pending_lot{static_cast<unsigned>(park_lots.size())};
    std::uint64_t next_request{};
    std::uintptr_t observed_owner{};
    std::optional<bool> observed_ready;
    std::uint64_t observed_since{}, next_observation{};
    bool readiness_warned{};
};

ParkRuntime& park_runtime();

void reset_park_session();

bool park_owner_ready(std::uintptr_t manager, std::uintptr_t& context, bool require_park = true);

void update_park_rotation(std::uintptr_t manager, std::uint64_t now);

void park_tick_hook(std::uintptr_t manager, float delta);

void park_construct_hook(std::uintptr_t context);

void start_park_rotation() noexcept;
}
