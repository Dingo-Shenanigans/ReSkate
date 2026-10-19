#pragma once
// What the drone can put on the pilot's skater while it flies (goggles, a hat): for now, finding
// the items. `droneitems` lists the installed cosmetics whose names contain a word, and the local
// skater's outfit slots, so the right item can be chosen. Game thread (console commands).
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::drone::outfit {
// What the `goggles` setting picks (1 = the first): eyewear from the game's own catalog that
// looks most like FPV goggles. The pilot's skater wears it while the drone flies.
struct Eyewear { const char* label; const char* item; };
inline constexpr Eyewear eyewear[] = {
    {"Large visor 1", "Own_EyewearGlasses_Gen_SunglassesVisorLarge_00001"},
    {"Large visor 2", "Own_EyewearGlasses_Gen_SunglassesVisorLarge_00002"},
    {"Large visor 3", "Own_EyewearGlasses_Gen_SunglassesVisorLarge_00003"},
    {"Large visor 4", "Own_EyewearGlasses_Gen_SunglassesVisorLarge_00004"},
    {"Shield aviators", "Own_EyewearGlasses_Gen_ShieldAviator_00001"},
    {"Wraparounds", "Own_EyewearGlasses_Gen_WrapAround_00001"},
    {"Slim wraparounds", "Own_EyewearGlasses_Gen_SunglassesWraparoundSlim_00001"},
    {"Speed shades", "Own_EyewearGlasses_Dime_SpeedShades_00001"},
};
inline constexpr std::size_t eyewear_count = sizeof(eyewear) / sizeof(eyewear[0]);
// Once per client tick (Runtime/client_tick.cpp): puts the chosen eyewear on the local skater
// while the drone flies and gives the skater's own back when it stops.
void on_client_tick() noexcept;
// Installed cosmetic names containing `word` (any case), at most `limit` of them; `total` is how many matched.
std::vector<std::string> find_items(std::string_view word, std::size_t limit, std::size_t& total);
// The local skater's outfit slots, one line each: index, slot name and what it holds now.
std::vector<std::string> slots();
}
