#include "trainer.h"
#include "trainer_classes.h"
#include "trainer_jump.h"
#include "trainer_presets.h"
#include "trainer_session.h"
#include "trainer_landing.h"
#include "trainer_waypoint.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/physics_tuning.h"
#include "Engine/Game/Multiplayer/session_physics.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/physics_tuning.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>

// Game thread only. See trainer.h for how the menu reaches this.
namespace dingosdk::trainer {
namespace {
namespace tuning = dingosdk::physics_tuning;
using Vec3 = std::array<float, 3>;
constexpr float max_coordinate = 1.0e6f;
constexpr std::uint32_t air_states_first = 200, air_states_end = 300;
constexpr std::uint32_t xinput_up = 0x1, xinput_down = 0x2, xinput_left = 0x4, xinput_right = 0x8,
                        xinput_lb = 0x100, xinput_rb = 0x200;

struct Entry {
    std::string id, key, label, group; // key: lower-case id
    Kind kind{};
    std::uint16_t offset{}, size{};
    std::size_t curve{};
    double value{}, stock{};
    bool touched{}, frozen{};
    bool detail{};
    bool used{}; // the game's code was found to read it (trainer_used.inc), or it is linked
    std::vector<std::size_t> drives; // graph multipliers this value scales (value_links)
    std::vector<std::size_t> members; // graph: its outputs (Y0.., Min_Y, Max_Y)
    int class_field{-1};              // a field of one of the game's data-defined classes (trainer_classes.h), not of the tuning asset
};
struct MapData {
    std::array<Marker, marker_slots> markers{};
    std::string preset;
};
struct MapFile { // Mods/<mod>/trainer.json
    std::string note, preset_name;
    std::vector<std::pair<std::string, double>> preset;
    std::vector<Spot> spots;
};
struct Saved {
    double value{};
    bool frozen{};
};
struct SelfTest {
    int step{}; // 0: idle
    std::uint64_t until{};
    std::size_t entry{};
    double before{}, target{};
    Vec3 origin{};
    int failures{};
};
struct Motion {
    bool valid{};
    Vec3 position{}, velocity{};
    std::int64_t stamp{};
    bool airborne{};
    Vec3 takeoff{}, takeoff_velocity{};
    std::int64_t takeoff_stamp{};
    float apex{};
    bool axes_valid{};
    Vec3 forward{}, up{};
    float spin{}, spin_rate{}, flip{};
    float board_turn{}; // the board's fastest turn rate in this jump, degrees per second
    std::uint32_t air_state{}, ground_state{};
};
// The trick multipliers and auto push in force: x of the game's own, and for auto push the
// speed a rolling skater is carried up to (m/s, 0: off).
struct Boosts {
    float flip{1}, hippy{1}, nocomply{1}, boneless{1}, offboard{1}, cruise{};
};
struct State {
    bool loaded{}; // the store was read
    std::shared_ptr<const tuning::Model> model;
    std::vector<Entry> entries;
    std::map<std::string, std::size_t, std::less<>> index;
    std::uintptr_t base{}, asset{}, entity{};
    tuning::Values own, want;
    bool write_due{}, view_due{true}, save_due{};
    bool profile_due{}; // the loaded map's preset is still to be applied
    bool refresh_due{}; // values were written while the skater had no cached copy to refresh
    std::uint64_t next_hold{}, next_save{}, next_skater{}, revision{};
    std::string status = "Waiting for the game.", last;
    bool ready{};
    std::map<std::string, Saved, std::less<>> saved;                                    // by key
    std::map<std::string, std::map<std::string, double, std::less<>>, std::less<>> user; // presets
    std::vector<std::string> active;                                                    // presets applied since Stock
    std::map<std::string, MapData, std::less<>> maps;
    std::string map;
    MapFile map_file;
    int slot{};
    bool auto_return{}, pad_shortcuts{}, hud{}, hud_jump{}, logging{};
    // The hippy jump's height is set by the game's trick scripts, not by tuning: the trainer
    // scales the upward velocity when it sees one start.
    float hippy_height{1};
    // An on-foot jump: the same scaling, when a skater standing off the board starts to rise.
    float offboard_height{1};
    // Board flip tricks: a multiplier on the game's flip speed curves (trainer_classes.h).
    float flip_speed{1};
    bool offboard_grounded{}; // off the board and not moving up or down at the last tick
    const char *boost_name{""};
    // The no comply and the boneless are launched by the trick scripts too (trainer_jump.cpp).
    float nocomply_height{1}, boneless_height{1};
    std::uint32_t last_state{};
    float boost_factor{}; // velocity factor of the jump now starting, 0: none
    std::uint64_t boost_until{};
    float return_delay{1.5f};
    std::uint64_t seen_wipeouts{}, return_at{}, open_serial{};
    std::uint64_t class_search_at{}; // when to look for the game's tuning classes (0: not due)
    int class_search_tries{};
    std::uint64_t class_search_seen{};      // the searches that had finished when this level loaded
    bool class_list_wanted{};               // the player has the list of every value open on this level
    std::vector<std::size_t> class_entries; // the entries that are fields of those classes
    // A session whose host sets everyone's physics (session_physics.h): what the host shares
    // beyond its tuning, and what each class field holds for it. The player's own stand down.
    bool enforced{};
    std::uint64_t host_revision{};
    SessionExtras host;
    std::array<float, class_field_count> host_classes{};
    Boosts boosts;         // what is in force this tick
    bool extras_due{true}; // the player's own extras are to be published for a session again
    bool wipeouts_known{}; // the wipeout count the session started with was read
    int open_tab{};
    std::uint32_t pad_previous{};
    // The pause map's waypoint. The registry is read only while the player has a use for the
    // answer (the TELEPORT card is on screen, or `trainer waypoint`), and the waypoint is
    // forgotten on a new level: it belonged to the old one.
    std::optional<Vec3> waypoint;
    std::string waypoint_detail, waypoint_level;
    std::uint64_t waypoint_wanted_until{}, waypoint_next_read{};
    // A ground-snapped arrival being watched (trainer_landing.h).
    landing::Watch landing;
    Motion motion;
    Telemetry telemetry;
    std::ofstream log;
    std::int64_t log_start{};
    std::map<std::uint32_t, std::uint64_t> states; // physics state -> ticks seen
    SelfTest test;
};
State &state() {
    static auto *value = new State;
    return *value;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
// "MaxPushSpeed" -> "Max Push Speed"
std::string spaced(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool upper = std::isupper(c) != 0;
        if (i && upper) {
            const auto before = static_cast<unsigned char>(text[i - 1]);
            const bool next_lower = i + 1 < text.size() && std::islower(static_cast<unsigned char>(text[i + 1]));
            if (std::islower(before) || std::isdigit(before) || (std::isupper(before) && next_lower)) result += ' ';
        }
        result += text[i] == '.' ? ' ' : text[i];
        if (text[i] == '.') result += "/ ";
    }
    return result;
}
std::optional<double> number(std::string_view text) {
    double value{};
    const auto *last = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) return std::nullopt;
    return value;
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::skater, text); }

// ---- storage ---------------------------------------------------------------------------
std::filesystem::path data_directory() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"trainer";
    if (raw) CoTaskMemFree(raw);
    return result;
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
        if (text.empty() || text.size() > 1024 * 1024) return std::nullopt;
        return Json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}
std::optional<Vec3> read_position(const Json &value) {
    if (!value.is_array() || value.size() != 3) return std::nullopt;
    Vec3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!value.at(i).is_number()) return std::nullopt;
        result[i] = value.at(i).get<float>();
        if (!std::isfinite(result[i]) || std::abs(result[i]) > max_coordinate) return std::nullopt;
    }
    return result;
}
Json write_position(const Vec3 &p) { return Json::array({Json(p[0]), Json(p[1]), Json(p[2])}); }

void load_store() {
    auto &s = state();
    s.loaded = true;
    const auto json = read_json(data_directory() / L"trainer.json");
    if (!json || !json->is_object()) return;
    try {
        if (json->contains("options") && json->at("options").is_object()) {
            const auto &o = json->at("options");
            s.auto_return = o.value("auto_return", false);
            s.pad_shortcuts = o.value("pad_shortcuts", false);
            s.hud = o.value("hud", false);
            s.hud_jump = o.value("hud_jump", false);
            s.return_delay = std::clamp(o.value("return_delay", 1.5f), 0.0f, 10.0f);
            s.hippy_height = std::clamp(o.value("hippy_height", 1.0f), height_low, height_high);
            s.nocomply_height = std::clamp(o.value("nocomply_height", 1.0f), height_low, height_high);
            s.offboard_height = std::clamp(o.value("offboard_height", 1.0f), height_low, height_high);
            s.flip_speed = std::clamp(o.value("flip_speed", 1.0f), flip_low, flip_high);
            s.boneless_height = std::clamp(o.value("boneless_height", 1.0f), height_low, height_high);
            s.slot = std::clamp(o.value("slot", 0), 0, static_cast<int>(marker_slots) - 1);
        }
        if (json->contains("values") && json->at("values").is_object())
            for (const auto &[key, row] : json->at("values").items())
                if (row.is_object() && row.contains("value") && row.at("value").is_number())
                    s.saved[lower(key)] = {row.at("value").get<double>(), row.value("frozen", false)};
        if (json->contains("presets") && json->at("presets").is_object())
            for (const auto &[name, values] : json->at("presets").items()) {
                if (!values.is_object() || name.empty() || name.size() > 48) continue;
                auto &preset = s.user[name];
                for (const auto &[key, value] : values.items())
                    if (value.is_number()) preset[lower(key)] = value.get<double>();
            }
        if (json->contains("active") && json->at("active").is_array())
            for (const auto &name : json->at("active"))
                if (name.is_string()) s.active.push_back(name.string());
        if (json->contains("maps") && json->at("maps").is_object())
            for (const auto &[level, row] : json->at("maps").items()) {
                if (!row.is_object()) continue;
                auto &map = s.maps[lower(level)];
                map.preset = row.value("preset", "");
                if (row.contains("markers") && row.at("markers").is_array())
                    for (std::size_t i = 0; i < marker_slots && i < row.at("markers").size(); ++i)
                        if (const auto position = read_position(row.at("markers").at(i))) map.markers[i] = {true, *position};
            }
    } catch (...) {
        say(logging::Level::warning, "Trainer: trainer.json is damaged; starting from what could be read.");
    }
}
void save_store() {
    auto &s = state();
    s.save_due = false;
    try {
        Json json = Json::object();
        json["schema"] = 1;
        Json options = Json::object();
        options["auto_return"] = s.auto_return;
        options["pad_shortcuts"] = s.pad_shortcuts;
        options["hud"] = s.hud;
        options["hud_jump"] = s.hud_jump;
        options["return_delay"] = s.return_delay;
        options["hippy_height"] = s.hippy_height;
        options["nocomply_height"] = s.nocomply_height;
        options["offboard_height"] = s.offboard_height;
        options["flip_speed"] = s.flip_speed;
        options["boneless_height"] = s.boneless_height;
        options["slot"] = s.slot;
        json["options"] = std::move(options);
        Json values = Json::object();
        // Values of fields this build's tuning does not have are kept as they were read.
        for (const auto &[key, saved] : s.saved) {
            Json row = Json::object();
            row["value"] = saved.value;
            row["frozen"] = saved.frozen;
            values[key] = std::move(row);
        }
        json["values"] = std::move(values);
        Json presets = Json::object();
        for (const auto &[name, preset] : s.user) {
            Json row = Json::object();
            for (const auto &[key, value] : preset) row[key] = value;
            presets[name] = std::move(row);
        }
        json["presets"] = std::move(presets);
        Json active = Json::array();
        for (const auto &name : s.active) active.push_back(name);
        json["active"] = std::move(active);
        Json maps = Json::object();
        for (const auto &[level, map] : s.maps) {
            if (level.empty()) continue;
            Json row = Json::object();
            Json markers = Json::array();
            for (const auto &marker : map.markers) markers.push_back(marker.set ? write_position(marker.position) : Json(nullptr));
            row["markers"] = std::move(markers);
            row["preset"] = map.preset;
            maps[level] = std::move(row);
        }
        json["maps"] = std::move(maps);
        const auto directory = data_directory();
        if (directory.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto target = directory / L"trainer.json", temporary = directory / L"trainer.json.tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << json.dump(2);
            if (!file) return;
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(target, error);
            std::filesystem::rename(temporary, target, error);
        }
    } catch (...) {
        say(logging::Level::warning, "Trainer: could not save trainer.json.");
    }
}
void changed(bool save = true) {
    auto &s = state();
    s.view_due = true;
    s.extras_due = true;
    if (save) {
        s.save_due = true;
        s.next_save = GetTickCount64() + 1500;
    }
}

// ---- values ----------------------------------------------------------------------------
double read_field(const std::vector<std::uint8_t> &image, const Entry &e) {
    const auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        float value;
        std::memcpy(&value, at, 4);
        return value;
    }
    if (e.kind == Kind::flag) return *at ? 1 : 0;
    switch (e.size) {
    case 1: { std::int8_t v; std::memcpy(&v, at, 1); return v; }
    case 2: { std::int16_t v; std::memcpy(&v, at, 2); return v; }
    case 4: { std::int32_t v; std::memcpy(&v, at, 4); return v; }
    default: { std::int64_t v; std::memcpy(&v, at, 8); return static_cast<double>(v); }
    }
}
void write_field(std::vector<std::uint8_t> &image, const Entry &e, double value) {
    auto *at = image.data() + e.offset;
    if (e.kind == Kind::real) {
        const auto v = static_cast<float>(value);
        std::memcpy(at, &v, 4);
    } else if (e.kind == Kind::flag) {
        *at = value != 0 ? 1 : 0;
    } else {
        switch (e.size) {
        case 1: { const auto v = static_cast<std::int8_t>(std::clamp(value, -128.0, 127.0)); std::memcpy(at, &v, 1); break; }
        case 2: { const auto v = static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0)); std::memcpy(at, &v, 2); break; }
        case 4: { const auto v = static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0)); std::memcpy(at, &v, 4); break; }
        default: { const auto v = static_cast<std::int64_t>(value); std::memcpy(at, &v, 8); break; }
        }
    }
}
// A curve's outputs times `factor`: a point is 0x1c bytes with its output at +0x14.
tuning::Curve scaled_curve(const tuning::Curve &source, double factor) {
    auto result = source;
    for (std::size_t p = 0; p + addr::physics_tuning::curve_point_size <= result.points.size(); p += addr::physics_tuning::curve_point_size) {
        float y;
        std::memcpy(&y, result.points.data() + p + 0x14, 4);
        y = static_cast<float>(y * factor);
        if (std::isfinite(y)) std::memcpy(result.points.data() + p + 0x14, &y, 4);
    }
    return result;
}
double sane(const Entry &e, double value) {
    if (!std::isfinite(value)) return e.stock;
    if (e.kind == Kind::flag) return value != 0 ? 1 : 0;
    if (e.kind == Kind::integer) return std::round(value);
    if (e.kind == Kind::curve || e.kind == Kind::graph) return std::clamp(value, 0.0, 1.0e6);
    return std::clamp(value, -1.0e6, 1.0e6);
}
// What a class field should hold: the player's value, or the host's while a session's host
// sets everyone's physics.
void want_class(const Entry &e) {
    auto &s = state();
    const auto field = static_cast<std::size_t>(e.class_field);
    want_class_value(field, s.enforced ? s.host_classes[field] : static_cast<float>(e.touched ? e.value : e.stock));
}
void apply_entry(Entry &e) {
    auto &s = state();
    if (e.class_field >= 0) {
        want_class(e);
        return;
    }
    if (e.kind == Kind::graph) {
        // A point the player set by hand keeps their value.
        for (const auto index : e.members) {
            const auto &member = s.entries[index];
            if (!member.touched && static_cast<std::size_t>(member.offset) + member.size <= s.want.image.size())
                write_field(s.want.image, member, e.touched ? member.stock * e.value : member.stock);
        }
    } else if (e.kind == Kind::curve) {
        if (e.curve < s.own.curves.size() && s.own.curves[e.curve] && e.curve < s.want.curves.size())
            s.want.curves[e.curve] = e.touched ? scaled_curve(*s.own.curves[e.curve], e.value) : *s.own.curves[e.curve];
    } else if (static_cast<std::size_t>(e.offset) + e.size <= s.want.image.size()) {
        write_field(s.want.image, e, e.touched ? e.value : e.stock);
    }
    s.write_due = true;
}
void remember(const Entry &e) {
    auto &s = state();
    if (e.touched || e.frozen) s.saved[e.key] = {e.value, e.frozen};
    else s.saved.erase(e.key);
}
void set_entry(Entry &e, double value) {
    e.value = sane(e, value);
    e.touched = e.value != e.stock;
    apply_entry(e);
    remember(e);
    // A linked value scales its graphs by the same ratio (the graphs are what the game reads).
    if (!e.drives.empty() && e.stock != 0) {
        const auto ratio = e.value / e.stock;
        const auto targets = e.drives; // set_entry may not touch `e` again, but keep the list stable
        auto &s = state();
        for (const auto index : targets)
            if (index < s.entries.size() && !s.entries[index].frozen) {
                auto &target = s.entries[index];
                const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
                set_entry(target, multiplier ? ratio : target.stock * ratio);
            }
    }
}
// A value a locked linked value drives (its graphs, the push class's speeds) is locked with it:
// resetting it alone would leave the lock showing a number that no longer does anything.
bool is_locked(const Entry &e) {
    if (e.frozen) return true;
    auto &s = state();
    const auto index = static_cast<std::size_t>(&e - s.entries.data());
    return std::ranges::any_of(s.entries, [&](const Entry &other) {
        return other.frozen && std::ranges::find(other.drives, index) != other.drives.end();
    });
}
Entry *find_entry(std::string_view id) {
    auto &s = state();
    const auto found = s.index.find(lower(id));
    return found == s.index.end() ? nullptr : &s.entries[found->second];
}
void build_entries() {
    auto &s = state();
    s.entries.clear();
    s.index.clear();
    s.class_entries.clear();
    const auto &m = *s.model;
    const auto add = [&](std::string name, Kind kind, std::uint16_t offset, std::uint16_t size, std::size_t curve) {
        if (name.empty()) name = std::format("Unnamed.0x{:x}", offset);
        Entry e;
        e.id = kind == Kind::curve || kind == Kind::graph ? name + " x" : name;
        e.key = lower(e.id);
        if (s.index.contains(e.key)) { // arrays of structures repeat names
            e.id += std::format(" @0x{:x}", offset);
            e.key = lower(e.id);
        }
        const auto dot = name.find('.');
        auto group = dot == std::string::npos ? std::string("General") : name.substr(0, dot);
        for (const std::string_view prefix : {"SkatePhysics", "Physics"})
            if (group.size() > prefix.size() && group.starts_with(prefix)) {
                group.erase(0, prefix.size());
                break;
            }
        e.group = spaced(group);
        e.label = spaced(dot == std::string::npos ? name : name.substr(dot + 1));
        if (kind == Kind::curve || kind == Kind::graph) e.label += " (x)";
        e.kind = kind;
        e.offset = offset;
        e.size = size;
        e.curve = curve;
        e.used = value_used(offset);
        s.index.emplace(e.key, s.entries.size());
        s.entries.push_back(std::move(e));
    };
    for (std::size_t i = 0; i < m.fields.size(); ++i) {
        const auto &f = m.fields[i];
        add(i < m.field_names.size() ? m.field_names[i] : std::string{}, f.real ? Kind::real : f.flag ? Kind::flag : Kind::integer,
            f.offset, f.size, 0);
    }
    for (std::size_t i = 0; i < m.curve_slots.size(); ++i)
        add(i < m.curve_names.size() ? m.curve_names[i] : std::string{}, Kind::curve, m.curve_slots[i], 8, i);
    // Graphs stored in the asset: sixteen X and Y values, a point count and bounds. Each
    // becomes one multiplier row; its points stay editable but out of the way.
    const auto plain = s.entries.size();
    for (std::size_t i = 0; i < plain; ++i) {
        if (!s.entries[i].id.ends_with(".Y0") || s.entries[i].kind != Kind::real) continue;
        const auto parent = s.entries[i].id.substr(0, s.entries[i].id.size() - 3);
        const auto offset = s.entries[i].offset;
        std::vector<std::size_t> outputs;
        for (std::size_t j = 0; j < plain; ++j) {
            auto &other = s.entries[j];
            if (other.id.size() <= parent.size() + 1 || !other.id.starts_with(parent) || other.id[parent.size()] != '.') continue;
            const std::string_view leaf(other.id.c_str() + parent.size() + 1);
            if (leaf.find('.') != std::string_view::npos) continue;
            other.detail = true;
            const bool output = (leaf.starts_with("Y") && leaf.size() <= 3) || leaf == "Min_Y" || leaf == "Max_Y";
            if (output && other.kind == Kind::real) outputs.push_back(j);
        }
        add(parent, Kind::graph, offset, 0, 0);
        s.entries.back().members = std::move(outputs);
    }
    // The game's data-defined classes: one row per number field, under the class's name.
    for (std::size_t c = 0; c < class_count; ++c)
        for (std::size_t i = 0; i < class_specs[c].count; ++i) {
            const auto &field = class_fields[class_specs[c].first + i];
            Entry e;
            e.id = std::format("{}.{}", class_specs[c].key, field.name);
            e.key = lower(e.id);
            if (s.index.contains(e.key)) continue;
            e.group = class_specs[c].group;
            e.label = spaced(field.name);
            e.kind = Kind::real;
            e.used = field.seen;
            e.class_field = static_cast<int>(class_specs[c].first + i);
            e.stock = e.value = field.stock;
            s.index.emplace(e.key, s.entries.size());
            s.class_entries.push_back(s.entries.size());
            s.entries.push_back(std::move(e));
        }
    for (const auto &link : value_links()) {
        const auto from = s.index.find(link.value), to = s.index.find(link.drives);
        if (from == s.index.end() || to == s.index.end()) continue;
        s.entries[from->second].drives.push_back(to->second);
        s.entries[from->second].used = true;
    }
    // A host's values may have arrived before there were entries to hold them.
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    s.extras_due = true;
    say(logging::Level::info, std::format("Trainer: {} tuning values and {} curves are editable.", m.fields.size(), m.curve_slots.size()));
}
// A new copy of the asset (first sight, or a level load): its values are the stock ones.
void adopt(const tuning::Values &live) {
    auto &s = state();
    s.own = live;
    s.want = live;
    for (auto &e : s.entries) {
        e.stock = e.class_field >= 0 ? class_fields[e.class_field].stock : e.kind == Kind::curve || e.kind == Kind::graph ? 1.0 : read_field(s.own.image, e);
        const auto found = s.saved.find(e.key);
        if (found != s.saved.end()) {
            e.frozen = found->second.frozen;
            e.value = sane(e, found->second.value);
            e.touched = e.value != e.stock;
        } else if (!e.touched) {
            e.value = e.stock;
        }
        if (e.touched) apply_entry(e);
    }
    // A linked value saved by itself (before links existed, or edited in the file) still
    // drives its graphs.
    for (auto &e : s.entries) {
        if (e.drives.empty() || !e.touched || e.stock == 0) continue;
        for (const auto index : e.drives) {
            auto &target = s.entries[index];
            if (target.touched || target.frozen) continue;
            const bool multiplier = target.kind == Kind::curve || target.kind == Kind::graph;
            target.value = sane(target, multiplier ? e.value / e.stock : target.stock * e.value / e.stock);
            target.touched = target.value != target.stock;
            if (target.touched) {
                apply_entry(target);
                remember(target);
            }
        }
    }
    s.write_due = true;
    changed(false);
}
std::string apply_preset(std::string_view name);
bool editable(std::string *why = nullptr) {
    // A guest's physics are the host's while the session enforces them: the tuning
    // (physics_tuning::enforce), and what the host shares beyond it (sync_session). Writing
    // over either would only fight the session, so the player's own stand down.
    if (session_tuning_enforced()) {
        if (why) *why = "This session's host sets everyone's physics: you skate with the host's.";
        return false;
    }
    return true;
}
// Auto push is the trainer's doing: the game's flag reaches only its animation.
float own_cruise() {
    const auto *auto_push = find_entry("physicsmode.autopushenabled"), *auto_speed = find_entry("physicspush.maxspeedforautopush");
    return auto_push && auto_speed && auto_push->touched && auto_push->value != 0 ? static_cast<float>(auto_speed->value) : 0.0f;
}
// The host's where a session's host sets everyone's physics, whatever the boosts switch says
// (that switch is about a guest's own); otherwise the player's, and the game's own in a
// session that has turned boosts off.
Boosts boosts_in_force() {
    auto &s = state();
    if (s.enforced)
        return {s.host.flip_speed, s.host.hippy_height, s.host.nocomply_height, s.host.boneless_height, s.host.offboard_height, s.host.cruise};
    if (multiplayer_session_active() && !session_boosts_allowed()) return {};
    return {s.flip_speed, s.hippy_height, s.nocomply_height, s.boneless_height, s.offboard_height, own_cruise()};
}
// The player's own, for a session they host to share with its guests.
void publish_extras() {
    auto &s = state();
    s.extras_due = false;
    SessionExtras own;
    own.flip_speed = s.flip_speed;
    own.hippy_height = s.hippy_height;
    own.nocomply_height = s.nocomply_height;
    own.boneless_height = s.boneless_height;
    own.offboard_height = s.offboard_height;
    own.cruise = own_cruise();
    for (const auto index : s.class_entries)
        if (const auto &e = s.entries[index]; e.touched)
            own.classes.emplace_back(static_cast<std::uint16_t>(e.class_field), static_cast<float>(e.value));
    set_local_physics_extras(encode_session_extras(own));
}
// Follows a session into and out of its host's physics, and the host's changes while there.
void sync_session() {
    auto &s = state();
    const bool enforced = session_tuning_enforced();
    std::vector<std::uint8_t> bytes;
    bool fresh = false;
    if (enforced) fresh = host_physics_extras(s.host_revision, bytes);
    else s.host_revision = 0; // read again the next time a host sets the physics
    if (enforced == s.enforced && !fresh) return;
    if (fresh) {
        auto decoded = decode_session_extras(bytes);
        if (!decoded) say(logging::Level::warning, "Trainer: what the host shares beyond its tuning could not be read; the game's own is used for it.");
        s.host = decoded ? std::move(*decoded) : SessionExtras{};
        for (std::size_t i = 0; i < class_field_count; ++i) s.host_classes[i] = class_fields[i].stock;
        for (const auto &[index, value] : s.host.classes) s.host_classes[index] = value;
        if (!s.host.stock())
            say(logging::Level::info, std::format("Trainer: skating with the host's trick multipliers and {} of its class values.", s.host.classes.size()));
    }
    s.enforced = enforced;
    for (const auto index : s.class_entries) want_class(s.entries[index]);
    changed(false);
}
void hold(std::uint64_t now) {
    auto &s = state();
    if (!s.base) return;
    if (!s.model) {
        tuning::prepare();
        std::string error;
        s.model = tuning::game_tuning(&error);
        if (!s.model) {
            const auto next = error.empty() ? std::string("Reading the game's physics tuning...") : "Physics tuning is unavailable: " + error + ".";
            if (next != s.status) { s.status = next; changed(false); }
            return;
        }
        build_entries();
    }
    if (!s.write_due && now < s.next_hold) return;
    s.next_hold = now + 500;
    tuning::Values live;
    std::uintptr_t asset{};
    if (!tuning::read_live(s.base, live, &asset)) {
        if (s.ready) { s.ready = false; s.status = "Waiting for a level."; changed(false); }
        return;
    }
    if (asset != s.asset || !s.ready) {
        s.asset = asset;
        adopt(live);
        s.ready = true;
        s.status = "Ready.";
    }
    if (!editable()) return;
    if (s.profile_due) {
        s.profile_due = false;
        if (const auto found = s.maps.find(s.map); found != s.maps.end() && !found->second.preset.empty())
            say(logging::Level::info, "Trainer: this map's preset - " + apply_preset(found->second.preset));
    }
    const bool pending = s.write_due || std::ranges::any_of(s.entries, [](const Entry &e) { return e.touched; });
    s.write_due = false;
    if (!pending) return;
    const auto written = tuning::write_live(s.base, s.entity, s.want);
    if (written.failed) {
        s.status = "The physics tuning could not be written.";
        changed(false);
    }
    // The skater skates from its own copy of part of the tuning: keep asking until it took it.
    if (written.values || written.curves) {
        s.refresh_due = !written.refreshed;
        say(logging::Level::info, std::format("Trainer: wrote {} values and {} curves; skater copy refreshed: {}.", written.values, written.curves,
                                              written.refreshed ? "yes" : "not yet"));
    } else if (s.refresh_due && tuning::refresh_skater(s.base, s.entity)) {
        s.refresh_due = false;
        say(logging::Level::info, "Trainer: skater copy refreshed.");
    }
}

// ---- presets ---------------------------------------------------------------------------
bool matches(const std::string &key, std::string_view pattern) {
    std::size_t start = 0;
    while (start < pattern.size()) {
        auto end = pattern.find(' ', start);
        if (end == std::string_view::npos) end = pattern.size();
        const auto token = pattern.substr(start, end - start);
        if (!token.empty()) {
            const bool exclude = token.front() == '!';
            const auto word = exclude ? token.substr(1) : token;
            if ((key.find(word) != std::string::npos) == exclude) return false;
        }
        start = end + 1;
    }
    return true;
}
// What each built-in preset's rules reach in the running game: (entry, rule) pairs, worked out
// once per table of entries.
const std::vector<std::vector<std::pair<std::size_t, std::size_t>>> &preset_targets() {
    static std::vector<std::vector<std::pair<std::size_t, std::size_t>>> cache;
    static const Entry *cached_for{};
    static std::size_t cached_count{};
    auto &s = state();
    if (cached_for == s.entries.data() && cached_count == s.entries.size() && !cache.empty()) return cache;
    cached_for = s.entries.data();
    cached_count = s.entries.size();
    const auto &presets = builtin_presets();
    cache.assign(presets.size(), {});
    for (std::size_t p = 0; p < presets.size(); ++p)
        for (std::size_t r = 0; r < presets[p].rules.size(); ++r) {
            const auto &rule = presets[p].rules[r];
            for (std::size_t i = 0; i < s.entries.size(); ++i) {
                const auto &e = s.entries[i];
                const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                if (e.detail || scale != rule.curves || !matches(e.key, rule.pattern)) continue;
                if (e.kind == Kind::flag && rule.multiply) continue;
                cache[p].emplace_back(i, r);
            }
        }
    return cache;
}
// A preset as a dial: `factor` is where its first value goes (x of stock); the others follow in
// proportion, so 1 is the game's own and the first rule's amount is the preset as it ships.
std::string apply_dial(std::string_view name, double factor) {
    auto &s = state();
    const auto &presets = builtin_presets();
    const auto wanted = lower(name);
    std::size_t p = 0;
    while (p < presets.size() && lower(presets[p].name) != wanted) ++p;
    if (p == presets.size()) return "error: no built-in preset is called \"" + std::string(name) + "\".";
    const auto &rules = presets[p].rules;
    if (rules.empty() || !rules[0].multiply || rules[0].amount <= 0 || rules[0].amount == 1)
        return "error: " + std::string(presets[p].name) + " is a switch, not a dial.";
    if (!(factor > 0) || !std::isfinite(factor)) return "error: a dial needs a multiplier above 0; 1 is the game's own.";
    const double t = std::log(factor) / std::log(rules[0].amount);
    std::size_t count{}, locked{};
    for (const auto &[index, r] : preset_targets()[p]) {
        auto &e = s.entries[index];
        const auto &rule = rules[r];
        if (is_locked(e)) {
            ++locked;
            continue;
        }
        if (rule.multiply) set_entry(e, rule.amount > 0 ? e.stock * std::pow(rule.amount, t) : e.stock * (1 + (rule.amount - 1) * t));
        else set_entry(e, t > 0.01 ? rule.amount : e.stock); // a switch that goes with the preset's direction
        ++count;
    }
    std::erase(s.active, std::string(presets[p].name));
    if (std::abs(t - 1) < 1e-3) s.active.emplace_back(presets[p].name);
    changed();
    return std::format("{} x{:.2f}: {} values set{}.", presets[p].name, factor, count, locked ? std::format("; {} locked values left alone", locked) : "");
}
std::string remove_preset(std::string_view name) {
    auto &s = state();
    const auto wanted = lower(name);
    std::size_t count{};
    std::string removed;
    const auto put_back = [&](Entry &e) {
        if (is_locked(e)) return;
        if (e.touched) {
            set_entry(e, e.stock);
            ++count;
            return;
        }
        // A linked value at stock whose graphs were set directly (by an older version's preset).
        for (const auto index : e.drives)
            if (auto &target = s.entries[index]; target.touched && !is_locked(target)) {
                set_entry(target, target.stock);
                ++count;
            }
    };
    for (const auto &[user_name, values] : s.user) {
        if (lower(user_name) != wanted) continue;
        for (const auto &saved : values)
            if (auto *e = find_entry(saved.first)) put_back(*e);
        removed = user_name;
        break;
    }
    if (removed.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (e.detail || scale != rule.curves || !matches(e.key, rule.pattern)) continue;
                    put_back(e);
                }
            removed = preset.name;
            break;
        }
    if (removed.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    std::erase(s.active, removed);
    // Presets that are still on may share values with the one switched off: set theirs again.
    const auto still_on = s.active;
    for (const auto &other : still_on) (void)apply_preset(other);
    changed();
    return std::format("{} off: {} values put back.", removed, count);
}
std::size_t reset_all() {
    auto &s = state();
    std::size_t count{};
    for (auto &e : s.entries) {
        if (!e.touched || is_locked(e)) continue;
        set_entry(e, e.stock);
        ++count;
    }
    s.active.clear();
    return count;
}
std::string apply_preset(std::string_view name) {
    auto &s = state();
    const auto wanted = lower(name);
    if (wanted == "stock") {
        const auto count = reset_all();
        changed();
        return std::format("Stock: {} values put back (frozen values kept).", count);
    }
    std::size_t count{}, locked{};
    std::string applied;
    if (wanted == "map") {
        if (s.map_file.preset.empty()) return "error: this map ships no trainer preset.";
        for (const auto &[id, value] : s.map_file.preset)
            if (auto *e = find_entry(id); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
        applied = s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    }
    if (applied.empty())
        for (const auto &[user_name, values] : s.user) {
            if (lower(user_name) != wanted) continue;
            for (const auto &[key, value] : values)
                if (auto *e = find_entry(key); e && !is_locked(*e)) { set_entry(*e, value); ++count; }
            applied = user_name;
            break;
        }
    if (applied.empty())
        for (const auto &preset : builtin_presets()) {
            if (lower(preset.name) != wanted) continue;
            for (const auto &rule : preset.rules)
                for (auto &e : s.entries) {
                    const bool scale = e.kind == Kind::curve || e.kind == Kind::graph;
                    if (e.detail || scale != rule.curves || !matches(e.key, rule.pattern)) continue;
                    if (e.kind == Kind::flag && rule.multiply) continue;
                    if (is_locked(e)) {
                        ++locked;
                        continue;
                    }
                    set_entry(e, rule.multiply ? e.stock * rule.amount : rule.amount);
                    ++count;
                }
            applied = preset.name;
            break;
        }
    if (applied.empty()) return "error: no preset is called \"" + std::string(name) + "\".";
    if (std::ranges::find(s.active, applied) == s.active.end()) s.active.push_back(applied);
    changed();
    return std::format("{}: {} values set{}.", applied, count,
                       locked ? std::format("; {} locked values left alone (untick their boxes in Tune to let presets change them)", locked) : "");
}

// ---- maps ------------------------------------------------------------------------------
MapFile read_map_file(const std::string &level) {
    MapFile result;
    const auto mods = game_directory() / L"Mods";
    std::error_code error;
    for (std::filesystem::directory_iterator it(mods, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        const auto json = read_json(it->path() / L"trainer.json");
        if (!json || !json->is_object()) continue;
        try {
            // The file names its level, or it stands for every level its mod adds.
            bool mine = false;
            if (json->contains("level") && json->at("level").is_string()) mine = lower(json->at("level").string()) == level;
            else if (const auto levels = read_json(it->path() / L"reskate-levels.json"); levels && levels->contains("levels"))
                for (const auto &row : levels->at("levels"))
                    if (row.is_object() && lower(row.value("asset", "")) == level) mine = true;
            if (!mine) continue;
            result.note = json->value("note", "");
            if (json->contains("preset") && json->at("preset").is_object()) {
                const auto &preset = json->at("preset");
                result.preset_name = preset.value("name", "");
                if (preset.contains("values") && preset.at("values").is_object())
                    for (const auto &[id, value] : preset.at("values").items())
                        if (value.is_number() && result.preset.size() < 512) result.preset.emplace_back(id, value.get<double>());
            }
            if (json->contains("spots") && json->at("spots").is_array())
                for (const auto &row : json->at("spots")) {
                    if (!row.is_object() || !row.contains("position") || result.spots.size() >= 64) continue;
                    if (const auto position = read_position(row.at("position")))
                        result.spots.push_back({row.value("name", "Spot"), *position});
                }
            return result;
        } catch (...) {}
    }
    return result;
}
void enter_map(const std::string &level) {
    auto &s = state();
    s.map = level;
    s.map_file = level.empty() ? MapFile{} : read_map_file(level);
    s.motion = {};
    s.telemetry.last = {};
    s.telemetry.best = {};
    s.landing = {};
    s.telemetry.top_speed = 0;
    s.return_at = 0;
    s.profile_due = false;
    // A level brings fresh copies of the game's tuning classes: look for them once it has settled.
    s.class_search_at = level.empty() ? 0 : GetTickCount64() + 10000;
    s.class_search_tries = 0;
    s.class_search_seen = class_searches();
    s.class_list_wanted = false;
    if (!level.empty()) {
        const auto found = s.maps.find(level);
        s.profile_due = found != s.maps.end() && !found->second.preset.empty();
        if (!s.map_file.spots.empty() || !s.map_file.preset.empty())
            say(logging::Level::info, std::format("Trainer: the map ships {} spots and {} preset values.", s.map_file.spots.size(),
                                                    s.map_file.preset.size()));
    }
    changed(false);
}
MapData &current_map() { return state().maps[state().map]; }
bool teleport_allowed(std::string &why) {
    if (multiplayer_session_active() && !session_noclip_allowed()) {
        why = "The host has turned off teleporting in this session.";
        return false;
    }
    return true;
}
std::string go_to(const Vec3 &position, const std::string &what) {
    std::string why;
    if (!teleport_allowed(why)) return "error: " + why;
    if (!teleport_local_skater(position)) return "error: teleporting is unavailable right now.";
    state().motion.valid = false;
    return "Teleporting to " + what + ".";
}

// ---- ground-safe arrivals -------------------------------------------------------------
// The topmost collision surface at x, z (trainer_landing.h for the ray), from the client
// physics world. Empty while collision there is still streaming in.
std::optional<float> topmost_ground(float x, float z, float hint_y) {
    const auto first = landing::first_ray(hint_y);
    if (const auto hit = local_ground_height(x, z, first.top, first.bottom)) return hit;
    if (const auto retry = landing::retry_ray(first)) return local_ground_height(x, z, retry->top, retry->bottom);
    return std::nullopt;
}

// Teleports onto the surface at x, z rather than to a raw height: a map waypoint's height is
// not the ground, and arriving under it drops the skater through the map.
std::string land_at(float x, float z, float hint_y, const std::string &what) {
    auto &s = state();
    const auto ground = topmost_ground(x, z, hint_y);
    // Collision far away may not be loaded yet: go to the hint's height, and the landing
    // watch puts the skater on the surface once it streams in.
    const Vec3 to{x, ground ? *ground + landing::stand_height : hint_y + 2.0f, z};
    const auto answer = go_to(to, what);
    if (answer.starts_with("error")) return answer;
    s.landing = landing::start(x, z, to[1], GetTickCount64());
    say(logging::Level::info, ground ? std::format("Trainer: landing on the surface at {:.1f}, {:.1f}, {:.1f}.", to[0], to[1], to[2])
                                     : std::format("Trainer: no collision at {:.1f}, {:.1f} yet; going to height {:.1f} and waiting for the ground.", x, z, to[1]));
    const auto stem = answer.substr(0, answer.size() - 1);
    return ground ? stem + std::format(" (ground at {:.1f}).", *ground)
                  : stem + " (the ground there is still loading; you'll be put on it when it arrives).";
}

void update_landing(std::uint64_t now) {
    auto &s = state();
    auto &w = s.landing;
    if (!w.active || now < w.next || !s.telemetry.skater) return;
    const auto &p = s.telemetry.position;
    landing::Sample sample{.now = now, .position = p, .vertical = s.telemetry.vertical, .ground = std::nullopt, .may_resend = true};
    const float dx = p[0] - w.x, dz = p[2] - w.z;
    if (dx * dx + dz * dz <= landing::arrived_radius * landing::arrived_radius) sample.ground = topmost_ground(p[0], p[2], p[1]);
    switch (landing::step(w, sample)) {
    case landing::Step::resend: {
        const auto ground = topmost_ground(w.x, w.z, w.y);
        (void)go_to({w.x, ground ? *ground + landing::stand_height : w.y, w.z}, "the same spot");
        say(logging::Level::info, "Trainer: the teleport hadn't arrived; sent it again.");
        break;
    }
    case landing::Step::put_back:
        (void)go_to({p[0], *sample.ground + landing::stand_height, p[2]}, "the surface");
        say(logging::Level::info, std::format("Trainer: sank to {:.1f} under the surface at {:.1f}; put back on it.", p[1], *sample.ground));
        break;
    case landing::Step::landed:
        say(logging::Level::info, std::format("Trainer: landed at {:.1f}, {:.1f}, {:.1f}.", p[0], p[1], p[2]));
        break;
    case landing::Step::ended:
        say(logging::Level::info, std::format("Trainer: landing watch ended at {:.1f}, {:.1f}, {:.1f} after {} correction(s).", p[0], p[1], p[2], w.fixes));
        break;
    case landing::Step::wait: break;
    }
}

void refresh_waypoint() {
    auto &s = state();
    const auto read = read_map_waypoint(s.base);
    s.waypoint_detail = read.detail;
    if (const auto next = landing::remembered(s.waypoint, read.reading); next != s.waypoint) {
        s.waypoint = next;
        s.view_due = true;
    }
}

// ---- telemetry -------------------------------------------------------------------------
std::int64_t clock_now() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
double clock_seconds(std::int64_t ticks) {
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return static_cast<double>(ticks) / frequency;
}
float horizontal(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[2] * v[2]); }
void finish_jump(const Vec3 &landing, float landing_speed, std::int64_t landed_at) {
    auto &s = state();
    auto &m = s.motion;
    const auto air = static_cast<float>(clock_seconds(landed_at - m.takeoff_stamp));
    m.airborne = false;
    s.telemetry.airborne = false;
    if (air < 0.25f) return;
    Jump jump;
    jump.serial = s.telemetry.last.serial + 1;
    jump.air_time = air;
    jump.takeoff = m.takeoff;
    jump.landing = landing;
    jump.takeoff_speed = std::sqrt(m.takeoff_velocity[0] * m.takeoff_velocity[0] + m.takeoff_velocity[1] * m.takeoff_velocity[1] +
                                   m.takeoff_velocity[2] * m.takeoff_velocity[2]);
    jump.takeoff_angle = std::atan2(m.takeoff_velocity[1], std::max(horizontal(m.takeoff_velocity), 0.01f)) * 180.0f / std::numbers::pi_v<float>;
    jump.height = m.apex - m.takeoff[1];
    jump.distance = horizontal({landing[0] - m.takeoff[0], 0, landing[2] - m.takeoff[2]});
    jump.drop = m.takeoff[1] - landing[1];
    jump.landing_speed = landing_speed;
    jump.spin = std::abs(m.spin);
    jump.spin_rate = m.spin_rate;
    jump.flip = m.flip;
    jump.board_turn = m.board_turn;
    jump.state = m.air_state;
    s.telemetry.last = jump;
    if (jump.distance > s.telemetry.best.distance) s.telemetry.best = jump;
    // In the log for a player who is measuring: the read-out is on, or telemetry is recorded.
    if (!s.hud_jump && !s.logging) return;
    say(logging::Level::info,
        std::format("Trainer jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, "
                    "landing {:.1f} km/h at ({:.1f}, {:.1f}, {:.1f}); spin {:.0f} deg (peak {:.0f} deg/s), flip {:.0f} deg, "
                    "board {:.0f} deg/s, states {}>{}.",
                    jump.serial, jump.takeoff_speed * 3.6f, jump.takeoff_angle, jump.air_time, jump.height, jump.distance, jump.drop,
                    jump.landing_speed * 3.6f, landing[0], landing[1], landing[2], jump.spin, jump.spin_rate, jump.flip, jump.board_turn, m.ground_state,
                    m.air_state));
}
void observe(std::uintptr_t base, std::uintptr_t client, std::uint64_t now) {
    auto &s = state();
    auto &t = s.telemetry;
    auto &m = s.motion;
    overlay::DebugModel skater;
    std::array<float, 16> matrix{};
    bool found = false;
    if (now >= s.next_skater) {
        try {
            matrix = client_source::detail::debug_skater(base, client, skater);
            found = skater.skater_position_valid;
        } catch (...) {
            s.next_skater = now + 250; // no skater: menus, loading, a respawn
        }
    }
    if (!found) {
        if (t.skater) { t.skater = false; m.valid = false; s.entity = 0; }
        return;
    }
    s.entity = skater.skater_identity;
    watch_physics_state(client, s.entity);
    const auto watch = watched_physics_state();
    const auto stamp = clock_now();
    const Vec3 position = skater.skater_position;
    t.skater = true;
    t.position = position;
    // Rows: right, up, forward, position.
    t.heading = std::atan2(matrix[8], matrix[10]) * 180.0f / std::numbers::pi_v<float>;
    if (t.heading < 0) t.heading += 360.0f;
    t.physics_state = watch.valid ? watch.state : 0;
    if (watch.valid) ++s.states[watch.state];
    // A hippy jump leaves the board (504) straight from riding (100): measured. A plain
    // dismount does the same but is not rising, so it is left alone.
    if (watch.valid && watch.state != s.last_state) {
        if (watch.state == addr::no_bail::offboard_physics_state && s.last_state == 100 && s.boosts.hippy != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.hippy); // height goes with the square of the speed
            s.boost_until = now + 250;
            s.boost_name = "hippy jump";
        }
        s.last_state = watch.state;
    }
    if (s.boost_factor > 0) {
        const auto result = take_jump_scale_result();
        if (result.outcome > 0) {
            say(logging::Level::info, std::format("Trainer: {} boosted: up speed {:.2f} -> {:.2f} m/s.", s.boost_name, result.up_speed,
                                                  result.up_speed * s.boost_factor));
            s.boost_factor = 0;
        } else if (now >= s.boost_until || result.outcome == -2) {
            s.boost_factor = 0;
        } else {
            // The factor comes from the multipliers in force, so a session's rules are already in it.
            (void)queue_jump_scale(client, s.entity, s.boost_factor); // again each tick until the skater is rising
        }
    }
    const auto seconds = m.valid ? clock_seconds(stamp - m.stamp) : 0.0;
    const Vec3 step{position[0] - m.position[0], position[1] - m.position[1], position[2] - m.position[2]};
    const bool jumped = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]) > 25.0f;
    if (!m.valid || seconds <= 0.0005 || seconds > 0.25 || jumped) {
        // First sample, a hitch, or a teleport: nothing can be measured across it.
        m = {};
        m.valid = true;
        m.position = position;
        m.stamp = stamp;
        t.airborne = false;
        t.air_time = t.height = 0;
        return;
    }
    const auto dt = static_cast<float>(seconds);
    const Vec3 raw{step[0] / dt, step[1] / dt, step[2] / dt};
    const Vec3 velocity{m.velocity[0] * 0.5f + raw[0] * 0.5f, m.velocity[1] * 0.5f + raw[1] * 0.5f, m.velocity[2] * 0.5f + raw[2] * 0.5f};
    // An on-foot jump: off the board, level at the last tick and now rising faster than stairs
    // or a slope can lift a runner.
    if (watch.valid && watch.state == addr::no_bail::offboard_physics_state) {
        if (s.offboard_grounded && raw[1] > 2.2f && s.boost_factor == 0 && s.boosts.offboard != 1.0f) {
            s.boost_factor = std::sqrt(s.boosts.offboard);
            s.boost_until = now + 250;
            s.boost_name = "off-board jump";
        }
        s.offboard_grounded = std::abs(raw[1]) < 0.6f;
    } else {
        s.offboard_grounded = false;
    }
    // The skater's physics says when it is in the air: its states 200-299 are the pop and
    // the flight (100s ride the ground, 300 is a wipeout, 500s are off the board).
    const bool flying = watch.valid && watch.state >= air_states_first && watch.state < air_states_end;
    // Rotation in the air: about the vertical (spins) and of the up axis (flips and rolls).
    const Vec3 forward{matrix[8], matrix[9], matrix[10]}, up{matrix[4], matrix[5], matrix[6]};
    if (m.airborne && m.axes_valid) {
        const float yaw = std::atan2(m.forward[2] * forward[0] - m.forward[0] * forward[2], m.forward[0] * forward[0] + m.forward[2] * forward[2]);
        const float degrees = yaw * 180.0f / std::numbers::pi_v<float>;
        // A ragdoll or a pose snap turns further in one tick than any trick does: leave those out.
        if (std::abs(degrees) < 45.0f) {
            m.spin += degrees;
            m.spin_rate = std::max(m.spin_rate, std::abs(degrees) / dt);
        }
        const float cosine = std::clamp(m.up[0] * up[0] + m.up[1] * up[1] + m.up[2] * up[2], -1.0f, 1.0f);
        if (const float tumble = std::acos(cosine) * 180.0f / std::numbers::pi_v<float>; tumble < 45.0f) m.flip += tumble;
    }
    m.forward = forward;
    m.up = up;
    m.axes_valid = true;
    if (!flying && watch.valid) m.ground_state = watch.state;
    if (flying && !m.airborne) {
        m.spin = m.spin_rate = m.flip = m.board_turn = 0;
        m.air_state = watch.state;
        m.airborne = true;
        m.takeoff = m.position;
        m.takeoff_velocity = velocity;
        m.takeoff_stamp = m.stamp;
        m.apex = std::max(m.position[1], position[1]);
    }
    if (m.airborne) {
        m.apex = std::max(m.apex, position[1]);
        t.air_time = static_cast<float>(clock_seconds(stamp - m.takeoff_stamp));
        t.height = m.apex - m.takeoff[1];
        if (!flying)
            finish_jump(position, std::sqrt(m.velocity[0] * m.velocity[0] + m.velocity[1] * m.velocity[1] + m.velocity[2] * m.velocity[2]),
                        stamp);
    }
    t.airborne = m.airborne;
    t.speed = horizontal(velocity);
    t.vertical = velocity[1];
    t.top_speed = std::max(t.top_speed, t.speed);
    m.velocity = velocity;
    m.position = position;
    m.stamp = stamp;
    // How fast the board turns (flip tricks, shuvits): body 0's angular velocity (+0x90), read while a jump
    // is measured or telemetry is recorded.
    float board_turn = 0;
    bool turning = m.airborne || (s.logging && s.log);
    if (turning) {
        try {
            const auto bodies = client_source::detail::debug_noclip_bodies(base, client, s.entity);
            std::array<float, 3> spin{};
            if (!bodies.parts.empty() && memory::peek(bodies.parts[0] + 0x90, spin)) {
                board_turn = std::sqrt(spin[0] * spin[0] + spin[1] * spin[1] + spin[2] * spin[2]) * 180.0f / std::numbers::pi_v<float>;
                if (!std::isfinite(board_turn) || board_turn > 1.0e5f) board_turn = 0;
            }
        } catch (...) {}
        if (m.airborne) m.board_turn = std::max(m.board_turn, board_turn);
    }
    if (s.logging && s.log) {
        s.log << std::format("{:.4f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.1f},{},{},{:.0f}\n", clock_seconds(stamp - s.log_start), position[0],
                             position[1], position[2], t.speed, t.vertical, t.heading, t.airborne ? 1 : 0, t.physics_state, board_turn);
    }
    // A bail: back to the selected marker once the skater has had time to fall.
    if (watch.valid && (!s.wipeouts_known || watch.wipeouts != s.seen_wipeouts)) {
        if (s.wipeouts_known && s.auto_return) {
            const bool marked = current_map().markers[static_cast<std::size_t>(s.slot)].set;
            if (marked) s.return_at = now + static_cast<std::uint64_t>(s.return_delay * 1000.0f);
            say(logging::Level::info, marked ? std::format("Trainer: bail seen; back to marker {} in {:.1f} s.", s.slot + 1, s.return_delay)
                                             : std::format("Trainer: bail seen, but marker {} is empty.", s.slot + 1));
        }
        s.wipeouts_known = true;
        s.seen_wipeouts = watch.wipeouts;
    }
}
void set_logging(bool on) {
    auto &s = state();
    if (s.log.is_open()) s.log.close();
    s.logging = false;
    if (!on) return;
    const auto directory = data_directory() / L"telemetry";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::string name = s.map.empty() ? "unknown" : s.map.substr(s.map.find_last_of('/') + 1);
    for (auto &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    SYSTEMTIME time;
    GetLocalTime(&time);
    const auto file = directory / std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}.csv", name, time.wYear, time.wMonth, time.wDay, time.wHour,
                                              time.wMinute, time.wSecond);
    s.log.open(file, std::ios::binary | std::ios::trunc);
    if (!s.log) return;
    s.log << "seconds,x,y,z,speed_mps,vertical_mps,heading_deg,airborne,physics_state,board_turn_dps\n";
    s.log_start = clock_now();
    s.logging = true;
}
void shortcuts() {
    auto &s = state();
    ControllerInput input;
    DingoSDKOverlayReadControllerInput(&input);
    const auto buttons = input.available ? input.buttons : 0;
    const auto pressed = buttons & ~s.pad_previous;
    s.pad_previous = buttons;
    if (!s.pad_shortcuts || (buttons & (xinput_lb | xinput_rb)) != (xinput_lb | xinput_rb)) return;
    std::string result;
    if (pressed & xinput_up) result = command("marker", {"save"});
    else if (pressed & xinput_down) result = command("marker", {"go"});
    else if (pressed & xinput_left) result = command("slot", {std::to_string((s.slot + static_cast<int>(marker_slots) - 1) % static_cast<int>(marker_slots) + 1)});
    else if (pressed & xinput_right) result = command("slot", {std::to_string((s.slot + 1) % static_cast<int>(marker_slots) + 1)});
    if (!result.empty()) overlay::notify(overlay::NoticeLevel::info, "TRAINER", result);
}

// ---- self test -------------------------------------------------------------------------
void test_line(bool ok, const std::string &text) {
    auto &s = state();
    if (!ok) ++s.test.failures;
    say(ok ? logging::Level::info : logging::Level::error, std::format("trainer selftest: {} {}", ok ? "OK" : "FAIL", text));
}
void run_selftest(std::uint64_t now) {
    auto &s = state();
    auto &t = s.test;
    if (!t.step || now < t.until) return;
    tuning::Values live;
    switch (t.step) {
    case 1: {
        test_line(s.model != nullptr, std::format("game tuning read ({} values, {} curves)", s.model ? s.model->fields.size() : 0,
                                                  s.model ? s.model->curve_slots.size() : 0));
        std::size_t named{};
        if (s.model) named = static_cast<std::size_t>(std::ranges::count_if(s.model->field_names, [](const std::string &n) { return !n.empty(); }));
        test_line(s.model && named == s.model->fields.size(), std::format("field names ({} of {})", named, s.model ? s.model->fields.size() : 0));
        test_line(s.ready && s.asset, "running tuning asset found");
        test_line(s.telemetry.skater, "local skater found");
        // The first plain real that is not zero and that the player has not touched.
        t.entry = s.entries.size();
        for (std::size_t i = 0; i < s.entries.size(); ++i)
            if (s.entries[i].kind == Kind::real && !s.entries[i].detail && !s.entries[i].touched && std::abs(s.entries[i].stock) > 0.01) {
                t.entry = i;
                break;
            }
        if (!s.ready || t.entry == s.entries.size() || !editable()) {
            test_line(false, "no value to write (tuning not ready or locked by a session)");
            t.step = 4;
            break;
        }
        auto &e = s.entries[t.entry];
        t.before = e.stock;
        t.target = e.stock * 1.01;
        set_entry(e, t.target);
        t.step = 2;
        t.until = now + 1200;
        break;
    }
    case 2: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && std::abs(read_field(live.image, e) - static_cast<float>(t.target)) < 1e-4 * std::max(1.0, std::abs(t.target)),
                  std::format("write and hold {} = {:.5g}", e.id, t.target));
        set_entry(e, t.before);
        t.step = 3;
        t.until = now + 1200;
        break;
    }
    case 3: {
        auto &e = s.entries[t.entry];
        const bool read = tuning::read_live(s.base, live);
        test_line(read && read_field(live.image, e) == static_cast<float>(t.before) && !e.touched, std::format("restore {} = {:.5g}", e.id, t.before));
        t.step = 4;
        break;
    }
    case 4: {
        if (!s.telemetry.skater) {
            test_line(false, "teleport (no skater)");
            t.step = 6;
            break;
        }
        t.origin = s.telemetry.position;
        const Vec3 target{t.origin[0] + 3.0f, t.origin[1] + 1.0f, t.origin[2]};
        const auto result = go_to(target, "a test point");
        test_line(!result.starts_with("error"), "teleport request: " + result);
        t.origin = target;
        t.step = result.starts_with("error") ? 6 : 5;
        t.until = now + 700;
        break;
    }
    case 5: {
        const auto &p = s.telemetry.position;
        // The skater may already be rolling away from where it was put.
        const float away = horizontal({p[0] - t.origin[0], 0, p[2] - t.origin[2]});
        test_line(s.telemetry.skater && away < 25.0f, std::format("teleport put the skater {:.2f} m from the target", away));
        t.step = 6;
        t.until = now + 1500;
        break;
    }
    default:
        say(t.failures ? logging::Level::error : logging::Level::info,
            std::format("trainer selftest: DONE {} failures", t.failures));
        t = {};
        changed(false);
        break;
    }
}

void build_view() {
    auto &s = state();
    s.view_due = false;
    auto next = std::make_shared<View>();
    next->revision = ++s.revision;
    next->ready = s.ready;
    next->status = s.status;
    next->last = s.last;
    next->editable = editable(&next->blocked);
    next->rows.reserve(s.entries.size());
    for (const auto &e : s.entries) {
        int rank{};
        std::uint8_t modes{};
        const auto friendly = essential_name(e.key, &rank, &modes);
        next->rows.push_back({e.id, e.label, e.group, e.kind, e.value, e.stock, e.touched, e.frozen, e.detail, std::string(friendly), rank, modes,
                              e.used});
        if (e.touched) ++next->touched;
        if (std::ranges::find(next->groups, e.group) == next->groups.end()) next->groups.push_back(e.group);
    }
    const auto is_active = [&](std::string_view name) { return std::ranges::find(s.active, name) != s.active.end(); };
    next->presets.push_back({"Stock", "Everything back to how this map loaded (frozen values stay).", true, s.active.empty() && !next->touched});
    {
        // A built-in preset is on when the values say so, whatever switched them: a slider, a
        // dial or another preset.
        const auto &presets = builtin_presets();
        const auto &targets = preset_targets();
        for (std::size_t p = 0; p < presets.size(); ++p) {
            const auto &preset = presets[p];
            PresetRow row{std::string(preset.name), std::string(preset.note), true, false};
            bool any = false, on = true;
            const Entry *headline{};
            for (const auto &[index, r] : targets[p]) {
                const auto &e = s.entries[index];
                const auto &rule = preset.rules[r];
                if (!headline && r == 0) headline = &e;
                const double expected = rule.multiply ? e.stock * rule.amount : rule.amount;
                any = true;
                // To the two decimals a dial shows: dragging one to the preset's number switches it on.
                if (std::abs(e.value - sane(e, expected)) > 4e-3 * std::max(1e-3, std::abs(expected))) on = false;
            }
            row.active = any && on;
            const auto dial = preset_dial(preset.name);
            row.title = std::string(dial.title);
            row.modes = dial.modes;
            if (!preset.rules.empty() && preset.rules[0].multiply && headline && headline->stock != 0 && !dial.title.empty()) {
                row.dial = true;
                row.amount = preset.rules[0].amount;
                row.factor = headline->value / headline->stock;
            }
            next->presets.push_back(std::move(row));
        }
    }
    for (const auto &[name, values] : s.user) next->presets.push_back({name, std::format("{} values", values.size()), false, is_active(name)});
    next->slot = s.slot;
    next->auto_return = s.auto_return;
    next->hippy_height = s.hippy_height;
    next->nocomply_height = s.nocomply_height;
    next->offboard_height = s.offboard_height;
    next->flip_speed = s.flip_speed;
    next->boneless_height = s.boneless_height;
    next->return_delay = s.return_delay;
    next->pad_shortcuts = s.pad_shortcuts;
    next->hud = s.hud;
    next->hud_jump = s.hud_jump;
    next->logging = s.logging;
    next->map = s.map;
    next->map_note = s.map_file.note;
    next->map_preset = s.map_file.preset.empty() ? std::string{} : s.map_file.preset_name.empty() ? "Map preset" : s.map_file.preset_name;
    next->spots = s.map_file.spots;
    next->waypoint_set = s.waypoint.has_value();
    if (s.waypoint) next->waypoint = *s.waypoint;
    next->open_serial = s.open_serial;
    next->open_tab = s.open_tab;
    if (const auto found = s.maps.find(s.map); found != s.maps.end()) {
        next->markers = found->second.markers;
        next->profile_preset = found->second.preset;
    }
    publish(std::move(next));
}
bool flag(const std::string &text, bool &out) {
    const auto value = lower(text);
    if (value == "1" || value == "on" || value == "true") { out = true; return true; }
    if (value == "0" || value == "off" || value == "false") { out = false; return true; }
    return false;
}
std::optional<int> slot_argument(const std::vector<std::string> &arguments, std::size_t index) {
    auto &s = state();
    if (arguments.size() <= index) return s.slot;
    const auto value = number(arguments[index]);
    if (!value || *value < 1 || *value > static_cast<double>(marker_slots) || *value != std::floor(*value)) return std::nullopt;
    return static_cast<int>(*value) - 1;
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string &level) noexcept {
    try {
        auto &s = state();
        const auto now = GetTickCount64();
        if (!s.loaded) load_store();
        s.base = base;
        sync_session();
        s.boosts = boosts_in_force();
        if (const auto key = lower(playing ? level : std::string{}); key != s.map) enter_map(key);
        if (const auto key = lower(level); !key.empty() && key != s.waypoint_level) {
            s.waypoint_level = key;
            s.waypoint.reset();
            s.view_due = true;
        }
        if (take_teleport_card_shown()) s.waypoint_wanted_until = now + 1000;
        if (now < s.waypoint_wanted_until && now >= s.waypoint_next_read) {
            s.waypoint_next_read = now + 250;
            refresh_waypoint();
        }
        if (playing) {
            observe(base, client, now);
            shortcuts();
            update_landing(now);
            if (s.return_at && now >= s.return_at) {
                s.return_at = 0;
                const auto &marker = current_map().markers[static_cast<std::size_t>(s.slot)];
                if (marker.set) (void)go_to(marker.position, "your marker");
            }
        } else if (s.telemetry.skater) {
            s.telemetry.skater = false;
            s.motion.valid = false;
            s.entity = 0;
        }
        (void)start_trick_heights(base);
        if (take_class_list_shown()) s.class_list_wanted = true;
        // The search reads all of the game's writable memory, so it runs only for a player with
        // a use for it: one of the classes' values or the flip speed is not the game's own
        // (theirs, or a host's they skate with), or they have the list of every value open.
        if (s.class_search_at && now >= s.class_search_at && playing && s.telemetry.skater && !finding_classes() &&
            (s.class_list_wanted || classes_wanted())) {
            // A level brings fresh copies, so only a search finished since it loaded counts. The
            // skater's own classes arrive a little after the skater does: a search that came up
            // empty is repeated, twice.
            const bool have = class_searches() > s.class_search_seen && class_copies(0) != 0;
            if (have || s.class_search_tries >= 3) {
                s.class_search_at = 0;
            } else {
                ++s.class_search_tries;
                s.class_search_at = now + 15000;
                find_classes();
            }
        }
        want_flip_speed(s.boosts.flip);
        (void)apply_classes();
        set_trick_heights(s.boosts.nocomply, s.boosts.boneless);
        // The speeds pushes aim for are the push class's (value_links ties them to the tuning's top
        // pushing speed, which only gates whether a push may start).
        const auto *top_speed = find_entry("physicspush.maxpushablespeed");
        set_push_speed(client, s.entity, 1.0f, top_speed ? static_cast<float>(top_speed->stock) : 0.0f, s.boosts.cruise);
        for (TrickLaunch launch; take_trick_launch(launch);)
            if (launch.factor != 1.0f)
                say(logging::Level::info, std::format("Trainer trick: {} launched at {:.2f} m/s up, x{:.2f}.",
                                                      launch.trick == Trick::boneless ? "boneless" : "no comply", launch.up_speed, launch.factor));
        hold(now);
        if (s.extras_due) publish_extras();
        run_selftest(now);
        publish(s.telemetry);
        if (s.view_due) build_view();
        if (s.save_due && now >= s.next_save) save_store();
    } catch (...) {}
}

namespace {
std::string run(std::string_view verb, const std::vector<std::string> &a) {
    auto &s = state();
    const auto v = lower(verb);
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string{}; };
    if (v == "status") {
        std::size_t touched{}, frozen{};
        for (const auto &e : s.entries) { touched += e.touched; frozen += e.frozen; }
        return std::format("{} {} values ({} changed, {} frozen). Map: {}. Presets: {}.", s.status, s.entries.size(), touched, frozen,
                           s.map.empty() ? "none" : s.map, s.active.empty() ? "stock" : std::to_string(s.active.size()) + " applied");
    }
    if (v == "set" || v == "freeze" || v == "reset") {
        if (v == "reset" && lower(arg(0)) == "all") {
            const auto count = reset_all();
            std::size_t locked{};
            for (const auto &e : s.entries) locked += e.touched;
            changed();
            return locked ? std::format("{} values put back; {} locked values kept (Reset everything clears those too).", count, locked)
                          : std::format("{} values put back.", count);
        }
        if (v == "reset" && lower(arg(0)) == "tricks") {
            s.hippy_height = s.nocomply_height = s.boneless_height = s.offboard_height = s.flip_speed = 1.0f;
            changed();
            return "Trick heights and flip trick speed are the game's own again.";
        }
        if (v == "reset" && lower(arg(0)) == "presets") {
            const auto on = s.active;
            for (const auto &name : on) (void)remove_preset(name);
            s.active.clear();
            changed();
            return std::format("{} presets switched off.", on.size());
        }
        if (v == "reset" && lower(arg(0)) == "everything") {
            // The game as it shipped: no lock survives, no preset stays on, no trick multiplier.
            std::size_t count{};
            for (auto &e : s.entries) e.frozen = false;
            for (auto &e : s.entries) {
                if (e.touched) {
                    set_entry(e, e.stock);
                    ++count;
                } else {
                    remember(e);
                }
            }
            s.saved.clear();
            s.active.clear();
            s.hippy_height = s.nocomply_height = s.boneless_height = s.offboard_height = s.flip_speed = 1.0f;
            changed();
            return std::format("Everything is the game's own again: {} values put back, locks cleared, presets off, trick sliders at 1.", count);
        }
        auto *e = find_entry(arg(0));
        if (!e) return "error: no tuning value is called \"" + arg(0) + "\". Try: trainer find <text>";
        std::string why;
        if (v != "freeze" && !editable(&why)) return "error: " + why;
        if (v == "set") {
            const auto value = number(arg(1));
            if (!value) return "error: " + e->id + " needs a number.";
            set_entry(*e, *value);
        } else if (v == "freeze") {
            if (!flag(arg(1), e->frozen)) return "error: freeze needs 0 or 1.";
            remember(*e);
        } else {
            set_entry(*e, e->stock);
        }
        changed();
        return std::format("{} = {:.6g}{}{}", e->id, e->value, e->touched ? std::format(" (stock {:.6g})", e->stock) : "", e->frozen ? ", frozen" : "");
    }
    if (v == "find") {
        const auto pattern = lower(arg(0));
        std::string result;
        std::size_t count{};
        for (const auto &e : s.entries)
            if (matches(e.key, pattern) && ++count <= 12) result += std::format("{}{} = {:.6g}", result.empty() ? "" : "; ", e.id, e.value);
        return count ? std::format("{} match: {}{}", count, result, count > 12 ? "; ..." : "") : "Nothing matches.";
    }
    if (v == "dial") {
        // trainer dial <multiplier> <built-in preset>
        const auto factor = number(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (!factor || name.empty()) return "error: usage: trainer dial <multiplier> <preset name>";
        std::string why;
        if (!editable(&why)) return "error: " + why;
        return apply_dial(name, *factor);
    }
    if (v == "preset") {
        const auto action = lower(arg(0));
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (name.empty()) return "error: usage: trainer preset apply|remove|save|delete <name>";
        if (action == "remove") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return remove_preset(name);
        }
        if (action == "apply") {
            std::string why;
            if (!editable(&why)) return "error: " + why;
            return apply_preset(name);
        }
        if (action == "save") {
            if (name.size() > 48) return "error: that name is too long.";
            for (const auto &preset : builtin_presets())
                if (lower(preset.name) == lower(name)) return "error: that name belongs to a built-in preset.";
            if (lower(name) == "stock" || lower(name) == "map") return "error: that name is reserved.";
            std::map<std::string, double, std::less<>> values;
            for (const auto &e : s.entries)
                if (e.touched) values[e.key] = e.value;
            if (values.empty()) return "error: nothing is changed, so there is nothing to save.";
            const auto count = values.size();
            s.user[name] = std::move(values);
            changed();
            return std::format("Saved \"{}\" ({} values).", name, count);
        }
        if (action == "delete") {
            if (!s.user.erase(name)) return "error: no preset of yours is called \"" + name + "\".";
            std::erase(s.active, name);
            changed();
            return "Deleted \"" + name + "\".";
        }
        return "error: usage: trainer preset apply|save|delete <name>";
    }
    if (v == "slot") {
        const auto slot = slot_argument(a, 0);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        s.slot = *slot;
        changed();
        return std::format("Marker slot {}{}.", s.slot + 1, current_map().markers[static_cast<std::size_t>(s.slot)].set ? "" : " (empty)");
    }
    if (v == "marker") {
        const auto action = lower(arg(0));
        const auto slot = slot_argument(a, 1);
        if (!slot) return std::format("error: slots are 1 to {}.", marker_slots);
        if (s.map.empty()) return "error: load a level first.";
        auto &marker = current_map().markers[static_cast<std::size_t>(*slot)];
        if (action == "save") {
            if (!s.telemetry.skater) return "error: no skater to mark.";
            marker = {true, s.telemetry.position};
            changed();
            return std::format("Marker {} saved at ({:.1f}, {:.1f}, {:.1f}).", *slot + 1, marker.position[0], marker.position[1], marker.position[2]);
        }
        if (action == "go") {
            if (!marker.set) return std::format("error: marker {} is empty.", *slot + 1);
            return go_to(marker.position, std::format("marker {}", *slot + 1));
        }
        if (action == "clear") {
            marker = {};
            changed();
            return std::format("Marker {} cleared.", *slot + 1);
        }
        return "error: usage: trainer marker save|go|clear [slot]";
    }
    if (v == "ground") {
        const auto x = number(arg(0)), z = number(arg(1));
        if (!x || !z) return "error: usage: trainer ground <x> <z>  (teleport to the topmost surface there)";
        return land_at(static_cast<float>(*x), static_cast<float>(*z), s.telemetry.position[1], "the ground there");
    }
    if (v == "waypoint") {
        refresh_waypoint();
        if (lower(arg(0)) == "info")
            return "Map waypoint: " + (s.waypoint ? std::format("{:.1f}, {:.1f}, {:.1f}", (*s.waypoint)[0], (*s.waypoint)[1], (*s.waypoint)[2]) : std::string("none")) +
                   " (" + s.waypoint_detail + ").";
        if (!s.waypoint) return "error: no waypoint on the map. Open the pause map and place one first.";
        return land_at((*s.waypoint)[0], (*s.waypoint)[2], (*s.waypoint)[1], "your map waypoint");
    }
    if (v == "tp") {
        const auto x = number(arg(0)), y = number(arg(1)), z = number(arg(2));
        if (!x || !y || !z) return "error: usage: trainer tp <x> <y> <z>";
        return go_to({static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)}, "that point");
    }
    if (v == "spot") {
        const auto index = number(arg(0));
        if (!index || *index < 1 || *index > static_cast<double>(s.map_file.spots.size())) return "error: this map has no such spot.";
        const auto &spot = s.map_file.spots[static_cast<std::size_t>(*index) - 1];
        return go_to(spot.position, spot.name);
    }
    if (v == "option") {
        const auto name = lower(arg(0));
        bool on{};
        std::string why;
        if (name == "flip_speed") {
            const auto value = number(arg(1));
            if (!value) return "error: flip_speed needs a multiplier, 1 = the game's own speed.";
            if (!editable(&why)) return "error: " + why;
            s.flip_speed = std::clamp(static_cast<float>(*value), flip_low, flip_high);
        } else if (name == "hippy_height" || name == "nocomply_height" || name == "boneless_height" || name == "offboard_height") {
            const auto value = number(arg(1));
            if (!value) return "error: " + name + " needs a multiplier, 1 = the game's own height.";
            if (!editable(&why)) return "error: " + why;
            (name == "hippy_height" ? s.hippy_height : name == "nocomply_height" ? s.nocomply_height : name == "offboard_height" ? s.offboard_height : s.boneless_height) =
                std::clamp(static_cast<float>(*value), height_low, height_high);
        } else if (name == "return_delay") {
            const auto value = number(arg(1));
            if (!value) return "error: return_delay needs seconds.";
            s.return_delay = std::clamp(static_cast<float>(*value), 0.0f, 10.0f);
        } else if (!flag(arg(1), on)) {
            return "error: usage: trainer option hud|hud_jump|auto_return|pad|log 0|1";
        } else if (name == "hud") s.hud = on;
        else if (name == "hud_jump") s.hud_jump = on;
        else if (name == "auto_return") s.auto_return = on;
        else if (name == "pad") s.pad_shortcuts = on;
        else if (name == "log") {
            set_logging(on);
            if (on && !s.logging) return "error: the telemetry file could not be created.";
        } else return "error: no option is called \"" + name + "\".";
        changed();
        return name + " = " + arg(1);
    }
    if (v == "profile") {
        const auto action = lower(arg(0));
        if (s.map.empty()) return "error: load a level first.";
        if (action == "clear") {
            current_map().preset.clear();
            changed();
            return "This map no longer applies a preset on load.";
        }
        std::string name;
        for (std::size_t i = 1; i < a.size(); ++i) name += (i > 1 ? " " : "") + a[i];
        if (action != "set" || name.empty()) return "error: usage: trainer profile set <preset> | clear";
        current_map().preset = name;
        changed();
        return "\"" + name + "\" will be applied whenever this map loads.";
    }
    if (v == "open") {
        const auto tab = lower(arg(0));
        // The last three are the Tune tab on one of its lists.
        const std::array<std::string_view, 7> tabs{"tune", "presets", "practice", "map", "realistic", "fun", "everything"};
        const auto found = std::ranges::find(tabs, tab);
        if (!tab.empty() && found == tabs.end()) return "error: usage: trainer open [tune|presets|practice|map|realistic|fun|everything]";
        s.open_tab = tab.empty() ? 1 : static_cast<int>(found - tabs.begin());
        ++s.open_serial;
        s.view_due = true;
        return "Opening the trainer.";
    }
    if (v == "jumps") {
        const auto &j = s.telemetry.last;
        if (!j.serial) return "No jump measured yet.";
        return std::format("Jump {}: takeoff {:.1f} km/h at {:.1f} deg, air {:.2f} s, height {:.2f} m, distance {:.2f} m, drop {:.2f} m, landing {:.1f} km/h.",
                           j.serial, j.takeoff_speed * 3.6f, j.takeoff_angle, j.air_time, j.height, j.distance, j.drop, j.landing_speed * 3.6f);
    }
    if (v == "where") {
        if (!s.telemetry.skater) return "error: no skater.";
        const auto &p = s.telemetry.position;
        return std::format("({:.2f}, {:.2f}, {:.2f}) heading {:.0f}, {:.1f} km/h. Blender: ({:.2f}, {:.2f}, {:.2f})", p[0], p[1], p[2],
                           s.telemetry.heading, s.telemetry.speed * 3.6f, p[0], -p[2], p[1]);
    }
    if (v == "classes") {
        if (lower(arg(0)) == "find") {
            find_classes();
            return "Looking for the game's tuning classes.";
        }
        return "Copies found: " + classes_summary();
    }
    if (v == "refresh") return tuning::refresh_skater(s.base, s.entity) ? "Skater copy refreshed." : "error: the skater has no cached copy right now.";
    if (v == "states") {
        std::string result;
        for (const auto &[id, ticks] : s.states) result += std::format("{}{}: {}", result.empty() ? "" : ", ", id, ticks);
        return result.empty() ? "No physics state seen yet." : "Physics states seen (id: ticks): " + result;
    }
    if (v == "dump") {
        const auto directory = data_directory();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream file(directory / L"tuning-values.txt", std::ios::binary | std::ios::trunc);
        if (!file) return "error: could not write the list.";
        for (const auto &e : s.entries)
            file << std::format("{}\t0x{:x}\t{}\t{:.6g}\t{:.6g}\n", e.id, e.offset,
                                e.kind == Kind::real ? "real" : e.kind == Kind::flag ? "flag" : e.kind == Kind::curve ? "curve" :
                                e.kind == Kind::graph ? "graph" : "int", e.stock, e.value);
        return std::format("Wrote {} values to {}.", s.entries.size(), path_utf8(directory / L"tuning-values.txt"));
    }
    if (v == "selftest") {
        if (s.test.step) return "The self test is already running.";
        s.test = {};
        s.test.step = 1;
        s.test.until = GetTickCount64() + 500;
        return "Self test started; see the log for \"trainer selftest:\" lines.";
    }
    return "error: trainer status|open|set|freeze|reset|find|preset|slot|marker|tp|ground|waypoint|spot|option|profile|jumps|where|states|dump|selftest";
}
} // namespace

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto result = run(verb, arguments);
    // The menu shows the last answer, except for the ones a dragged slider sends every frame.
    if (verb != "set" || result.starts_with("error")) {
        auto &s = state();
        s.last = result;
        s.view_due = true;
    }
    return result;
}
} // namespace dingosdk::trainer
