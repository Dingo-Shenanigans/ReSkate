#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// After a change of time of day (a "TOD_<n>_<name>" subworld loading or unloading), the ground
// away from the camera kept the old time's light for a few seconds: going from Afternoon to
// Weather Day, the places behind the camera glowed white until looked at, with a hard edge where
// they met the ground already lit anew. The game keeps the distant sun shadows and how much sky
// each place sees in caches (WorldRender.DistantShadowCacheEnable, WorldRender.DynamicSkyVisEnable)
// that are rebaked a part at a time, the part in view first. With both off for a while after the
// change, the glow was clearly less (tested in the game with the console, 2026-11-12); with them off
// all the time it was the same and no frames were lost, but they are only held off for a few seconds,
// and then put back as they were.
namespace dingosdk::time_of_day_lighting {

inline constexpr std::array<const char*, 2> held_settings{
    "WorldRender.DistantShadowCacheEnable",
    "WorldRender.DynamicSkyVisEnable",
};
// How long the caches stay off after the last change of the time layers.
inline constexpr std::uint64_t hold_ms = 8000;

// A subworld that is one of a map's time layers ("Levels/Game/BAM_LevelRoot/TOD_6_WeatherDay").
inline bool is_time_layer(std::string_view bundle) {
    const auto slash = bundle.find_last_of("/\\");
    const auto name = slash == std::string_view::npos ? bundle : bundle.substr(slash + 1);
    if (name.size() < 5) return false;
    const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
    return upper(name[0]) == 'T' && upper(name[1]) == 'O' && upper(name[2]) == 'D' && name[3] == '_' &&
           name[4] >= '0' && name[4] <= '9';
}

// The settings held off after a change, and put back after it. `get(name)` is the value now (or
// nothing), `set(name, value)` true when it was written. A setting already off is left alone; one
// changed by someone else while held (the console) is theirs, and is not taken again until the next
// change of time.
struct Hold {
    std::array<std::optional<std::string>, held_settings.size()> found; // what each was, while held
    std::uint64_t taken_for{}; // the change the settings were taken for

    template <class Get, class Set>
    void step(std::uint64_t now, std::uint64_t changed_at, Get&& get, Set&& set) {
        const bool wanted = changed_at != 0 && now < changed_at + hold_ms;
        bool all_read = true; // a setting not readable yet (the settings still being listed) is tried again
        for (std::size_t i = 0; i < held_settings.size(); ++i) {
            const char* name = held_settings[i];
            if (found[i]) {
                const auto value = get(name);
                if (!value) continue; // not readable this tick: still held
                if (*value != "0") { found[i].reset(); continue; } // someone else's now
                if (!wanted && set(name, *found[i])) found[i].reset();
                continue;
            }
            if (!wanted || taken_for == changed_at) continue;
            const auto value = get(name);
            if (!value) { all_read = false; continue; }
            if (*value != "0" && set(name, "0")) found[i] = *value;
        }
        if (wanted && all_read) taken_for = changed_at;
    }
    bool holding() const {
        for (const auto& value : found) if (value) return true;
        return false;
    }
};

// A time layer started or finished loading or unloading. Any thread.
void time_layer_changed(std::uint64_t now) noexcept;
// On the game update thread (the named settings' own), every tick.
void tick(std::uint64_t now) noexcept;
}
