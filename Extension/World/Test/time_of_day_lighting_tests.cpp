#include "Extension/World/time_of_day_lighting.h"
#include <cstdio>
#include <map>
#include <stdexcept>
using namespace dingosdk::time_of_day_lighting;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Settings {
    std::map<std::string, std::string> values{{held_settings[0], "1"}, {held_settings[1], "1"}};
    bool readable = true;
    int writes = 0;
    void step(Hold& h, std::uint64_t now, std::uint64_t changed) {
        h.step(now, changed,
            [&](const char* name) -> std::optional<std::string> { if (!readable) return {}; return values.at(name); },
            [&](const char* name, const std::string& value) { values[name] = value; ++writes; return true; });
    }
};
int main() {
    try {
        check(is_time_layer("Levels/Game/BAM_LevelRoot/TOD_6_WeatherDay"), "a time layer");
        check(is_time_layer("Levels/game/BAM_LevelRoot/tod_3_Afternoon"), "a time layer in any case");
        check(!is_time_layer("Levels/Game/BAM_LevelRoot/BAM_LevelRoot"), "the map itself is not a time layer");
        check(!is_time_layer("Levels/Game/BAM_LevelRoot/TODO"), "a name only starting alike is not one");

        Settings s;
        Hold h;
        s.step(h, 100, 0);
        check(!h.holding() && s.writes == 0, "nothing is changed before a time changes");
        s.step(h, 1000, 1000);
        check(h.holding() && s.values[held_settings[0]] == "0" && s.values[held_settings[1]] == "0", "both caches go off at a change");
        s.step(h, 1000 + hold_ms - 1, 1000);
        check(h.holding(), "held for the whole time");
        s.step(h, 5000, 5000); // another change while held: still held, from the new change
        s.step(h, 1000 + hold_ms + 1, 5000);
        check(h.holding(), "a later change holds them longer");
        s.step(h, 5000 + hold_ms, 5000);
        check(!h.holding() && s.values[held_settings[0]] == "1" && s.values[held_settings[1]] == "1", "put back as they were");

        Settings off;
        off.values[held_settings[1]] = "0"; // turned off by the player
        Hold h2;
        off.step(h2, 1000, 1000);
        off.step(h2, 1000 + hold_ms, 1000);
        check(off.values[held_settings[1]] == "0", "one already off stays off");

        Settings theirs;
        Hold h3;
        theirs.step(h3, 1000, 1000);
        theirs.values[held_settings[0]] = "1"; // the console, while held
        theirs.step(h3, 1100, 1000);
        theirs.step(h3, 1200, 1000);
        check(theirs.values[held_settings[0]] == "1", "a value changed by someone else while held is theirs");
        theirs.step(h3, 1000 + hold_ms, 1000);
        check(theirs.values[held_settings[1]] == "1" && !h3.holding(), "the other is still put back");

        Settings later;
        later.readable = false;
        Hold h4;
        later.step(h4, 1000, 1000);
        check(!h4.holding(), "nothing taken while the settings cannot be read");
        later.readable = true;
        later.step(h4, 1100, 1000);
        check(h4.holding(), "taken once they can be read, within the time");
        later.readable = false;
        later.step(h4, 1000 + hold_ms, 1000);
        check(h4.holding(), "not dropped while they cannot be read");
        later.readable = true;
        later.step(h4, 1000 + hold_ms + 1, 1000);
        check(!h4.holding() && later.values[held_settings[0]] == "1", "put back once they can be read");
        std::puts("Time of day lighting checks passed."); return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
