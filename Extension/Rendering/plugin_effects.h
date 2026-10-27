#pragma once
#include <utility>
#include <vector>
#include "Extension/Plugins/plugin_effects.h"

// The game-thread owner of plugins' native effects. No native reference crosses into a VM.
namespace dingosdk::effects {
void tick(std::uintptr_t base, std::uintptr_t client, bool ready, bool noclip, bool editor) noexcept;
void before_level_transition(unsigned next) noexcept;
std::uint64_t create(std::string_view plugin, const plugins::EffectRequest& request);
void move(std::string_view plugin, std::uint64_t id, const plugins::EffectMatrix& matrix);
void stop(std::string_view plugin, std::uint64_t id);
void opacity(std::string_view plugin, std::uint64_t id, float value);
void release(std::string_view plugin, std::uint64_t id);
std::vector<std::uint64_t> live(std::string_view plugin);
void clear(std::string_view plugin);
std::pair<bool, std::uint64_t> status();
}
