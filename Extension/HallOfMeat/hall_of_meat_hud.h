#pragma once
#include <string>
#include <string_view>
#include <vector>
// Hall Of Meat on screen: samples the trainer's skater telemetry every presented frame, runs the
// bail tracker and draws the live bail and the result card. Presentation thread only.
namespace dingosdk::overlay {
// Game thread: once per client tick, after the trainer has published its telemetry.
void hall_of_meat_tick();
// True while a bail is live or a result is on screen (also keeps frames coming so bails are seen).
bool hall_of_meat_hud_pending();
void draw_hall_of_meat_hud();
// `hom <verb> ...` console commands (any thread).
std::string hall_of_meat_command(std::string_view verb, const std::vector<std::string> &words);
} // namespace dingosdk::overlay
