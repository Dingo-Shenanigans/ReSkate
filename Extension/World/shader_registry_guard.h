#pragma once
#include <cstdint>

// Keeps the game's expression shader registry from holding pointers outside its databases
// (Engine/Game/Build/<build>/shader_registry.h): a database's lookup index past its records is
// pointed at a record that is there, and the registration of a database that was destroyed
// before it ran is not done. Each says so in the log. Databases the guard did not see made or
// destroyed are left to the game.
namespace dingosdk::shader_registry_guard {
bool install(std::uintptr_t image_base) noexcept;
}
