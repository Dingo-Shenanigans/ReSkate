#include "domination_internal.h"
#include "domination_parks.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Vfs/mod_list.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/World/level_loading.h"
#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctime>
#include <format>
#include <iterator>

// Park files and the spots' outlines on the ground (domination_internal.h).
namespace dingosdk::domination::detail {
// ---- files ------------------------------------------------------------------------------
// <LOCALAPPDATA>\ReSkate\domination: the variable first, so a copy started with its own
// LOCALAPPDATA keeps its files apart.
std::filesystem::path data_directory() {
    std::vector<wchar_t> variable(32768);
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", variable.data(), static_cast<DWORD>(variable.size()));
    std::filesystem::path root;
    if (length && length < variable.size()) root = variable.data();
    else {
        PWSTR raw{};
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) root = raw;
        if (raw) CoTaskMemFree(raw);
    }
    return root.empty() ? root : root / L"ReSkate" / L"domination";
}
std::string stamp() {
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char text[32]{};
    std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &local);
    return text;
}
std::filesystem::path game_directory() {
    std::vector<wchar_t> exe(32768);
    const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) return {};
    return std::filesystem::path(exe.data()).parent_path();
}
std::optional<Json> read_json(const std::filesystem::path &path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 4 * 1024 * 1024) return std::nullopt;
        return Json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}
std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::string points(float score) {
    auto digits = std::to_string(static_cast<long long>(std::llround(score)));
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<std::size_t>(at), ",");
    return digits;
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::world, "Domination: " + text); }

std::optional<float> number(const std::string &word) {
    float value{};
    const auto *end = word.data() + word.size();
    const auto [at, error] = std::from_chars(word.data(), end, value);
    if (error != std::errc{} || at != end || !std::isfinite(value)) return std::nullopt;
    return value;
}
void toast(State &s, std::string text, int kind, Player who) {
    s.toasts.push_back({std::move(text), kind, Clock::now() + std::chrono::milliseconds(static_cast<int>(toast_shown * 1000)), who});
    while (s.toasts.size() > 4) s.toasts.pop_front();
}

// ---- park files -------------------------------------------------------------------------
std::vector<std::filesystem::path> park_files() {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    const auto own = data_directory() / L"parks";
    for (std::filesystem::directory_iterator it(own, error), end; it != end && !error; it.increment(error))
        if (it->path().extension() == L".json") files.push_back(it->path());
    // Mods switched on in the launcher, highest priority first: domination*.json beside the mod's
    // manifest (a map mod shipping its parks), and every .json in its domination folder (a pack of
    // parks, a mod of nothing else).
    const auto list = mods::scan_mods(game_directory());
    for (const auto &entry : list.entries) {
        if (!entry.enabled || !entry.mod.outdated.empty()) continue;
        std::error_code inner;
        for (std::filesystem::directory_iterator file(entry.mod.directory, inner), last; file != last && !inner; file.increment(inner)) {
            const auto name = lower(file->path().filename().string());
            if (name.starts_with("domination") && name.ends_with(".json")) files.push_back(file->path());
        }
        for (std::filesystem::directory_iterator file(entry.mod.directory / L"domination", inner), last; file != last && !inner; file.increment(inner))
            if (lower(file->path().extension().string()) == ".json") files.push_back(file->path());
    }
    return files;
}
// The park for `level`, from the first file that names it and reads cleanly.
// Every park there is: the files (a parks folder, map mods) first, then the ones built in, which
// a file of the same name overrides. Each with where it came from, for the log.
struct ParkSource {
    std::string id, where;
    Json json;
};
std::vector<ParkSource> park_sources() {
    std::vector<ParkSource> sources;
    for (const auto &path : park_files())
        if (auto json = read_json(path); json && json->is_object())
            sources.push_back({path.stem().string(), path.string(), std::move(*json)});
    for (const auto &builtin : builtin_parks) {
        if (std::ranges::any_of(sources, [&](const ParkSource &source) { return source.id == builtin.id; })) continue;
        try {
            sources.push_back({std::string(builtin.id), "built in", Json::parse(builtin.json)});
        } catch (const std::exception &error) {
            say(logging::Level::warning, std::format("built-in park {} unreadable: {}", builtin.id, error.what()));
        }
    }
    return sources;
}
bool names_level(const Json &json) { return json.contains("level") && json.at("level").is_string(); }

std::uint32_t park_hash(const Park &park) {
    std::uint32_t h = 2166136261u;
    const auto mix = [&](std::string_view text) {
        for (const unsigned char c : text) h = (h ^ c) * 16777619u;
        h = (h ^ 0x0a) * 16777619u;
    };
    mix(park.level);
    for (const auto &spot : park.spots) mix(spot.id);
    return h;
}
void scan_parks(State &s) {
    s.parks.clear();
    s.park_problems.clear();
    for (const auto &source : park_sources()) {
        if (!names_level(source.json)) continue;
        try {
            auto park = parse_park(source.json);
            const auto hash = park_hash(park);
            s.parks.push_back({source.id, source.where, std::move(park), hash});
        } catch (const std::exception &error) {
            s.park_problems.emplace_back(lower(source.json.at("level").string()), source.id + ": " + error.what());
        }
    }
}
const KnownPark *find_park(const State &s, std::string_view id) {
    const auto it = std::ranges::find_if(s.parks, [&](const KnownPark &k) { return k.id == id; });
    return it == s.parks.end() ? nullptr : &*it;
}
const KnownPark *next_park(const State &s) {
    if (const auto *chosen = find_park(s, s.chosen)) return chosen;
    // Unpicked: the red-bowl park, the first Domination park, when its map is here.
    if (const auto *park = find_park(s, default_park)) {
        const auto state = map_state(s, park->park);
        if (state != MapState::missing && state != MapState::off) return park;
    }
    if (!s.level.empty())
        for (const auto &k : s.parks)
            if (k.park.level == s.level) return &k;
    return s.parks.empty() ? nullptr : &s.parks.front();
}
std::string map_name(const Park &park) {
    if (!park.map.empty()) return park.map;
    const auto slash = park.level.find_last_of('/');
    return slash == std::string::npos ? park.level : park.level.substr(slash + 1);
}
// Whether each park's map can be played here, from the Mods folder (read at most every
// map_check_every: the menu asks a few times a second). A park with no package is on the game's own map.
void check_maps(State &s, Clock::time_point now) {
    if (now < s.next_map_check && !s.maps.empty()) return;
    s.next_map_check = now + map_check_every;
    s.maps.clear();
    const auto list = mods::scan_mods(game_directory());
    for (const auto &k : s.parks) {
        auto &state = s.maps[k.park.level];
        if (state == MapState::installed) continue;
        if (k.park.package.empty()) {
            state = MapState::installed;
            continue;
        }
        auto found = MapState::missing;
        for (const auto &entry : list.entries) {
            const bool level = std::ranges::any_of(entry.mod.levels, [&](const std::string &asset) { return lower(asset) == k.park.level; });
            if (!level && lower(entry.mod.name) != lower(k.park.package)) continue;
            if (entry.enabled && entry.mod.outdated.empty()) {
                found = MapState::installed;
                break;
            }
            found = MapState::off;
        }
        state = found;
    }
}
MapState map_state(const State &s, const Park &park) {
    if (park.level == s.level) return MapState::installed;
    const auto it = s.maps.find(park.level);
    return it == s.maps.end() ? MapState::unknown : it->second;
}
// ---- spot outlines ----------------------------------------------------------------------
// Points along a spot's edge seen from above, about half a metre apart (what a wall hides is cut
// out that finely), going round it once.
std::vector<Vec2> perimeter(const Spot &spot) {
    constexpr float step = 0.5f, pi = 3.14159265f;
    std::vector<Vec2> points;
    const auto line = [&](Vec2 a, Vec2 b) {
        const float dx = b[0] - a[0], dz = b[1] - a[1];
        const int n = std::max(1, static_cast<int>(std::ceil(std::hypot(dx, dz) / step)));
        for (int k = 0; k < n; ++k) points.push_back({a[0] + dx * k / n, a[1] + dz * k / n});
    };
    // Half a circle about `c`, from the angle `from` (about world up, from +x towards +z) on by pi.
    const auto arc = [&](Vec2 c, float from, float r) {
        const int n = std::max(6, static_cast<int>(std::ceil(pi * r / step)));
        for (int k = 0; k < n; ++k) {
            const float a = from + pi * k / n;
            points.push_back({c[0] + r * std::cos(a), c[1] + r * std::sin(a)});
        }
    };
    if (!spot.capsule) {
        for (std::size_t i = 0; i < spot.points.size(); ++i) line(spot.points[i], spot.points[(i + 1) % spot.points.size()]);
    } else {
        const auto &a = spot.a, &b = spot.b;
        const float r = spot.radius, angle = std::atan2(b[1] - a[1], b[0] - a[0]);
        const Vec2 n{-std::sin(angle) * r, std::cos(angle) * r}; // the side at angle + pi/2
        line({a[0] - n[0], a[1] - n[1]}, {b[0] - n[0], b[1] - n[1]});
        arc(b, angle - pi / 2, r);
        line({b[0] + n[0], b[1] + n[1]}, {a[0] + n[0], a[1] + n[1]});
        arc(a, angle + pi / 2, r);
    }
    constexpr std::size_t most = 800;
    if (points.size() > most) {
        std::vector<Vec2> fewer;
        for (std::size_t i = 0; i < most; ++i) fewer.push_back(points[i * points.size() / most]);
        points = std::move(fewer);
    }
    return points;
}
void begin_outlines(State &s) {
    s.outlines.clear();
    s.outline_since.clear();
    s.outline_retry.clear();
    s.outline_seen.clear();
    s.outline_shown.clear();
    s.sight_cursor = 0;
    s.outline_left = 0;
    if (!s.park) return;
    for (const auto &spot : s.park->spots) {
        auto &outline = s.outlines.emplace_back();
        for (const auto &p : perimeter(spot)) outline.push_back({p[0], std::numeric_limits<float>::quiet_NaN(), p[1]});
        s.outline_left += outline.size();
        s.outline_since.push_back({});
        s.outline_retry.push_back({});
        s.outline_seen.emplace_back(outline.size(), std::uint8_t{});
        s.outline_shown.emplace_back(outline.size(), 0.0f);
    }
}
// The heights still unknown, a few at a time, for the spots near the skater only: the ground
// under each point within its spot's height range. Collision streams in around the skater (a ray
// far from them hits nothing), so misses are tried again four times a second; points still missing
// 5 s after the spot's latest hit, the skater near all along (inside a wall: a ray that starts in a
// solid hits nothing), are filled in from the points either side. A spot never hit is not drawn.
void tick_outlines(State &s, const Vec3 &skater) {
    if (!s.park || !s.outline_left || s.outlines.size() != s.park->spots.size() || s.outline_since.size() != s.outlines.size() ||
        s.outline_retry.size() != s.outlines.size())
        return;
    constexpr float within = 120; // m from the skater to a spot's marker, seen from above
    constexpr auto most = std::chrono::seconds(5);
    const auto now = Clock::now();
    const auto until = now + std::chrono::milliseconds(2);
    const std::size_t spots = s.outlines.size(), first = s.outline_turn++ % spots; // each spot first in turn
    for (std::size_t n = 0; n < spots; ++n) {
        const std::size_t i = (first + n) % spots;
        const auto &spot = s.park->spots[i];
        auto &outline = s.outlines[i];
        const auto unknown = [](const Vec3 &p) { return std::isnan(p[1]); };
        if (std::ranges::none_of(outline, unknown)) continue;
        if (std::hypot(spot.marker[0] - skater[0], spot.marker[2] - skater[2]) > within) {
            s.outline_since[i] = {}; // away: the 5 s start again from a hit once back
            continue;
        }
        if (now < s.outline_retry[i]) continue;
        bool missed = false;
        for (auto &p : outline) {
            if (!unknown(p)) continue;
            if (Clock::now() >= until) return;
            if (const auto ground = local_ground_height(p[0], p[2], spot.y_max + 0.5f, spot.y_min - 2.0f)) {
                p[1] = *ground;
                --s.outline_left;
                s.outline_since[i] = now;
            } else
                missed = true;
        }
        if (missed && (s.outline_since[i] == Clock::time_point{} || now - s.outline_since[i] < most)) {
            s.outline_retry[i] = now + std::chrono::milliseconds(250);
            continue;
        }
        // All its heights known, or as many as will be: the rest filled in, and one or two points
        // off the top of a wall or a rail evened out.
        std::vector<float> heights;
        for (const auto &p : outline) heights.push_back(p[1]);
        s.outline_left -= static_cast<std::size_t>(std::ranges::count_if(outline, unknown));
        fill_in(heights, spot.y_min);
        even_out(heights);
        for (std::size_t k = 0; k < outline.size(); ++k) outline[k][1] = heights[k];
        if (!s.outline_left) say(logging::Level::info, "spot outlines measured");
    }
}

// What the camera can see of the outlines near the skater, a few points a tick in turn: a ray from
// the camera to each point (a little above the ground) that reaches it, or stops within half a
// metre of it, sees it. Walls, ramps and the bowls' lips hide what is behind them; the skater
// does not (a ray stopped by them sees the point).
void tick_sight(State &s, const Vec3 &skater) {
    if (!s.park || s.outline_seen.size() != s.outlines.size() || Clock::now() - s.camera_at > std::chrono::milliseconds(500)) return;
    std::size_t total = 0;
    for (const auto &outline : s.outlines) total += outline.size();
    if (!total) return;
    const auto until = Clock::now() + std::chrono::microseconds(1500);
    for (std::size_t done = 0; done < total && Clock::now() < until; ++done) {
        std::size_t at = s.sight_cursor++ % total, i = 0;
        while (at >= s.outlines[i].size()) at -= s.outlines[i++].size();
        const auto &p = s.outlines[i][at];
        auto &seen = s.outline_seen[i][at];
        if (std::isnan(p[1]) || std::hypot(p[0] - skater[0], p[2] - skater[2]) > outline_far + 4) {
            seen = 0;
            continue;
        }
        const Vec3 target{p[0], p[1] + 0.2f, p[2]};
        const auto hit = local_surface_hit({s.camera[0], s.camera[1], s.camera[2]}, {target[0], target[1], target[2]});
        seen = !hit || std::hypot((*hit)[0] - target[0], (*hit)[1] - target[1], (*hit)[2] - target[2]) < 0.5f ||
               std::hypot((*hit)[0] - skater[0], (*hit)[1] - skater[1], (*hit)[2] - skater[2]) < 1.6f;
    }
}

void use_park(State &s, const KnownPark &k) {
    s.park = k.park;
    s.park_id = k.id;
    s.park_problem.clear();
    say(logging::Level::info, std::format("park \"{}\" ({} spots, {} starts) from {}", k.park.name, k.park.spots.size(), k.park.starts.size(), k.where));
    begin_outlines(s);
}
void load_park(State &s) {
    scan_parks(s);
    s.park.reset();
    s.park_id.clear();
    s.park_problem.clear();
    begin_outlines(s);
    for (const auto &[level, problem] : s.park_problems) say(logging::Level::warning, "park refused, " + problem);
    if (s.level.empty()) return;
    // The chosen park when it is on this map, else this map's first.
    const auto *k = next_park(s);
    if (k && k->park.level == s.level) use_park(s, *k);
    else
        for (const auto &[level, problem] : s.park_problems)
            if (level == s.level) s.park_problem = problem; // why this map has none
}

} // namespace dingosdk::domination::detail
