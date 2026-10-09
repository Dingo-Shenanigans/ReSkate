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
// Its asset pack is installed and switched on (nothing is read from the game otherwise).
bool skate3_hom_installed();
// Game thread, with the map being played (empty without one): loads that map's best bail.
void skate3_hom_set_level(std::string_view level);
// Skate 3's broken-bone slow motion (IsBrokenBoneSlowMo): the game speed a break asks for now
// (1 when none), and the speed the game actually runs at (the slow motion may be refused, as in a
// multiplayer session). The bail is timed on the game's clock.
float skate3_hom_game_speed();
void skate3_hom_set_applied_speed(float speed);
void draw_skate3_hom_hud();
// `hom <verb> ...` console commands (any thread).
std::string skate3_hom_command(std::string_view verb, const std::vector<std::string> &words);
} // namespace dingosdk::overlay
