#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// The game's own grade of each pop (EnumGS.PopHeight): low, medium or high, which the player's Minimum Pop Height
// setting moves. Playback chooses the clip for the grade the game gave the player's pop.
//
// Every skater's graph is heard, each grade with the graph that wrote it; the local skater's are told apart by his graph
// (PopGradeLog), so the grade is used online and beside AI skaters too.
namespace dingosdk::anim_studio::pop_grades {
// 1 low, 2 medium, 3 high (0 none).
using PopHeight = std::int32_t;

// Hooks the game's setter for the graph's int values, once. False with `why` when this game build's setter is not the
// one this was written for. Client thread.
bool install(std::uintptr_t base, std::string& why);

// The grade of the local skater's pop at `takeoff_ms` (GetTickCount64), asked at `now_ms` (0 none: not graded, or not
// known yet to be his). `solo`: no other skater in the game. `attempt`: the air it is for. Any thread.
[[nodiscard]] PopHeight local_pop_height(std::uint64_t takeoff_ms, std::uint64_t now_ms, bool solo, std::uint64_t attempt) noexcept;
} // namespace dingosdk::anim_studio::pop_grades
