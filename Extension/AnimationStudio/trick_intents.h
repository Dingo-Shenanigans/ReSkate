#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What the game's gesture graph decides as a trick starts, and Record all tricks' say over it: the pop's strength
// (Float.Intent.JumpStrength) and the flip's speed (Float.Intent.TrickFlipSpeed), and the game's own grade of each pop
// (EnumGS.PopHeight), which the player's Minimum Pop Height setting moves. Every take is the quick flick, and the
// strength the graph writes is scaled as it writes it: the game then does that pop with its own physics and animation.
// From the ReSkate research ("Tricks by command"): strength 0.5 popped 0.28 m and 1.5 0.48-0.50 m (1.0: 0.38 m).
//
// The writes are not told apart by skater: any skater's graph is heard. Record all tricks refuses to start while other
// skaters are in the game, and playback uses the grade only in solo play with no AI skaters.
namespace dingosdk::anim_studio::trick_intents {
// The game's grade of a pop: 1 low, 2 medium, 3 high (0 none).
using PopHeight = std::int32_t;
// "low", "mid", "high", as the studio names its pops ("" for none).
[[nodiscard]] std::string_view pop_name(PopHeight height) noexcept;
[[nodiscard]] PopHeight pop_height_of(std::string_view name) noexcept;

// Hooks the game's setter for the graph's int values, once, to hear each pop's grade. False with `why` when this game
// build's setter is not the one this was written for. Client thread.
bool install_grading(std::uintptr_t base, std::string& why);
// Hooks the setter for the graph's float values as well, once, to scale the pop's strength in a take. Client thread.
bool install_scaling(std::uintptr_t base, std::string& why);

// A take starts: for `lasts_ms` at most, the game's pop strengths and flip speeds are these times its own (1 leaves
// them), and the values it writes are kept as it wrote them. Only Record all tricks does this, offline, for one take;
// the scale goes back to the game's own when the take ends or its time is up, whichever is first. Any thread.
void begin_take(float strength_scale, float flip_speed_scale, std::uint64_t lasts_ms) noexcept;
void end_take() noexcept;
// The values the game wrote in the take, in order, each change once, before any scaling (for the log).
struct Written {
    std::vector<float> strengths, flip_speeds;
};
[[nodiscard]] Written written();
// The grade of the pop in the take under way or just over (0 none yet). Any thread.
[[nodiscard]] PopHeight take_pop_height() noexcept;
// The grade of the latest pop the game made at or after `since_ms` (GetTickCount64), any skater's (0 none). Any thread.
[[nodiscard]] PopHeight pop_height_since(std::uint64_t since_ms) noexcept;
} // namespace dingosdk::anim_studio::trick_intents
