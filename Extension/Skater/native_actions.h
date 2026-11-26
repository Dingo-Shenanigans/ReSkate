#pragma once
#include "Engine/Game/Skater/actor_control_policy.h"
#include <cstdint>

namespace dingosdk::skater_action {
// A concrete SDK writer enters this scope before publication or native work.
// Reserving an actor claim and entering a writer share one atomic ownership gate.
bool enter_motion_writer(actor_control::IdleCall& call) noexcept;
// Publish the current local actor before plugin callbacks; false invalidates it.
void tick(std::uintptr_t image_base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk::skater_action
