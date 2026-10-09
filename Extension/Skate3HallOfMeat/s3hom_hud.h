#pragma once
#include <string>
#include <string_view>
#include <vector>
// Hall Of Meat on screen: samples the trainer's skater telemetry every presented frame, runs the
// bail tracker and draws the live bail and the result card. Presentation thread only.
namespace dingosdk::overlay {
// Game thread: once per client tick, after the trainer has published its telemetry.
// `stand_down` while ReSkate's own Hall of Meat is on: this one shows nothing then.
void skate3_hom_tick(bool stand_down = false);
// True while a bail is live or a result is on screen (also keeps frames coming so bails are seen).
bool skate3_hom_hud_pending();
void draw_skate3_hom_hud();
// `hom <verb> ...` console commands (any thread).
std::string skate3_hom_command(std::string_view verb, const std::vector<std::string> &words);
} // namespace dingosdk::overlay
