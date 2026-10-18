#pragma once
#include "Engine/Game/Skater/first_person_spring.h"
#include <array>
#include <cstdint>
namespace dingosdk::first_person_integration {
struct ControlsContext {
    bool verified{}, on_foot{}, eligible{}, turn_allowed{}, motion_ready{};
    bool telemetry_valid{}, teleport{}, bail{}, mounting{};
    bool getting_up{}, recovery_controllable{}, recovery_verified{}, normal_foot{}, wipeout_request{};
    std::uint32_t physics{};
    std::uintptr_t world_context{}, game_base{};
    std::array<std::uintptr_t,4> movement_state{};
    std::array<std::uintptr_t,4> owner{}; // core, entity, physics context, rig
    first_person::Matrix body{};
    double time{};
    std::uint64_t tick{};
};
// Native SourceReader, existing UI state and upstream movement classifications.
ControlsContext read_controls_context(std::uintptr_t base, std::uintptr_t client) noexcept;
}
