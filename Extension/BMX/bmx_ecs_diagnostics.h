#pragma once
#include <cstdint>
#include <string>

namespace dingosdk::bmx {
// Passive, development-only snapshots; neither function calls engine code.
std::string ecs_startup_diagnostics(std::uintptr_t base, std::uintptr_t client);
// Called only after the existing exact InitRecipeTemplates VM/layout guard.
void observe_recipe_initializer_context() noexcept;
}
