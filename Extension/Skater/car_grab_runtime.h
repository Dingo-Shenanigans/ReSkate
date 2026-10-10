#pragma once
#include <cstdint>

namespace dingosdk {
// Called on the verified local client thread. It publishes only a short lease;
// physics rechecks ownership, controls and multiplayer before any body write.
void publish_car_grab_client(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
}

namespace dingosdk::client_source::detail {
// The existing physics hook acquires SourceState::busy before calling this.
// Do not acquire that flag recursively. All native body access stays here.
void car_grab_physics_tick(std::uintptr_t core) noexcept;
}
