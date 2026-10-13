#pragma once
#include <cstdint>
#include <optional>
#include <string>

// Records a flip trick's clip on the trick stage, trying again when a take is not kept. The stage stays open. Offline only.
namespace dingosdk::trick_recorder {
// The recording starts this long before the stick moves, so that the clip opens with the skater standing still.
inline constexpr std::uint32_t still_ms = 600;
// Any thread.
bool start(std::uint8_t trick, bool ui_hidden, std::string &error);
// Any thread.
[[nodiscard]] bool active();
// How the last recording ended, or what it does now, as a lowercase fragment.
[[nodiscard]] std::string message();
// Any thread.
[[nodiscard]] bool kept();
// Why the trick was skipped after its tries were used up; empty after any other end. Any thread.
[[nodiscard]] std::optional<std::string> skipped_reason();
// Client thread only, after trick_stage::tick.
void tick(std::uintptr_t base, bool ready) noexcept;
} // namespace dingosdk::trick_recorder
