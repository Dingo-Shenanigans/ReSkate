#pragma once
#include <filesystem>
#include <string>

namespace dingosdk::overlay { struct GrabbedFrame; }
// Research aids for the board turnaround (`turnaround grab|boards|stick` in the console); their
// answers go to ReSkate.log. See turnaround_research.cpp.
namespace dingosdk::turnaround::research {
// `turnaround grab`: the next presented frame is saved to Fit Checks\ beside Skate.exe.
void request_grab() noexcept;
bool grab_requested() noexcept;
void take_grab(const overlay::GrabbedFrame& frame) noexcept; // render thread
// `turnaround boards [seconds]`: the skateboard entities in memory, and what in them changes
// while the player spins the board on show.
std::string request_boards(double seconds);
// `turnaround parts`: every item of the game's skater presets as it holds them now, with titles.
std::string request_parts();
// `turnaround stick <x> <y> [seconds]`: the right stick pushed, -1..1, on top of the pad's own.
std::string request_stick(double x, double y, double seconds);
// Where the research aids' files go: the Fit Checks folder beside Skate.exe.
std::filesystem::path folder();
} // namespace dingosdk::turnaround::research
