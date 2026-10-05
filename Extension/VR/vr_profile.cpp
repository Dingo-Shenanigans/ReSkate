// VR settings in the local profile ("ReSkate.VR.*").
#include "vr_profile.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::vr {
namespace {
namespace key {
constexpr const char* enabled = "VR.Enabled";
constexpr const char* positional = "VR.Positional";
constexpr const char* level_horizon = "VR.LevelHorizon";
constexpr const char* fov_vertical = "VR.FovVertical";
constexpr const char* turn_smoothing = "VR.TurnSmoothing";
constexpr const char* world_scale = "VR.WorldScale";
constexpr const char* seat_forward = "VR.SeatForward";
constexpr const char* fov_correction = "VR.FovCorrection";
constexpr const char* seat_up = "VR.SeatUp";
constexpr const char* seat_side = "VR.SeatSide";
constexpr const char* others_see_outfit = "VR.OthersSeeOutfit";
constexpr const char* seat_forward_foot = "VR.SeatForwardFoot";
constexpr const char* seat_up_foot = "VR.SeatUpFoot";
constexpr const char* seat_side_foot = "VR.SeatSideFoot";
constexpr const char* fov_scale = "VR.FovScale";
constexpr const char* frame_lag = "VR.FrameLag";
constexpr const char* hide_body = "VR.HideBody";
constexpr const char* hide_body_foot = "VR.HideBodyFoot";
constexpr const char* view_mode = "VR.ViewMode";
constexpr const char* stereo_mode = "VR.StereoMode";
constexpr const char* depth_strength = "VR.DepthStrength";
constexpr const char* lens_effects_off = "VR.LensEffectsOff";
constexpr const char* controllers = "VR.Controllers";
constexpr const char* auto_first_person = "VR.AutoFirstPerson";
constexpr const char* edge_widening = "VR.EdgeWidening";
constexpr const char* gap_fill = "VR.GapFill";
constexpr const char* tilt_filter = "VR.TiltFilter";
constexpr const char* walk_where_you_look = "VR.WalkWhereYouLook";
constexpr const char* look_turn = "VR.LookTurn";
constexpr const char* look_turn_style = "VR.LookTurnStyle";
constexpr const char* smooth_turn_speed = "VR.SmoothTurnSpeed";
constexpr const char* grab_view = "VR.GrabView";
constexpr const char* crouch_follow = "VR.CrouchFollow";
constexpr const char* grab_body = "VR.GrabBody";
constexpr const char* bail_camera = "VR.BailCamera";
constexpr const char* comfort_vignette = "VR.ComfortVignette";
constexpr const char* third_distance = "VR.ThirdDistance";
constexpr const char* third_height = "VR.ThirdHeight";
constexpr const char* theater_distance = "VR.TheaterDistance";
constexpr const char* theater_width = "VR.TheaterWidth";
constexpr const char* chat_in_vr = "VR.ChatInVr";
constexpr const char* chat_on_hand = "VR.ChatOnHand";
constexpr const char* chat_view_size = "VR.ChatViewSize";
constexpr const char* chat_view_x = "VR.ChatViewX";
constexpr const char* chat_view_y = "VR.ChatViewY";
constexpr const char* recenter_key = "VR.RecenterKey";
}
constexpr ULONGLONG save_delay_ms = 750;
bool loaded = false, pending = false;
ULONGLONG due = 0;

std::optional<double> number(const char* name) {
    const auto value = profile_runtime::local_value(name);
    if (!value || !value->is_number()) return std::nullopt;
    const auto result = value->get<double>();
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}

constexpr const char* presets_key = "VR.Presets";

// Every saved setting, by key. Booleans are profile preferences; numbers are
// stored as doubles so each key keeps one type.
struct Field {
    const char* key;
    bool Settings::*flag = nullptr;
    float Settings::*real = nullptr;
    int Settings::*whole = nullptr;
};
const std::array fields{
    Field{.key = key::enabled, .flag = &Settings::enabled},
    Field{.key = key::positional, .flag = &Settings::positional},
    Field{.key = key::level_horizon, .flag = &Settings::level_horizon},
    Field{.key = key::fov_vertical, .flag = &Settings::fov_vertical},
    Field{.key = key::turn_smoothing, .real = &Settings::turn_smoothing},
    Field{.key = key::world_scale, .real = &Settings::world_scale},
    Field{.key = key::seat_forward, .real = &Settings::seat_forward},
    Field{.key = key::fov_correction, .real = &Settings::fov_correction},
    Field{.key = key::seat_up, .real = &Settings::seat_up},
    Field{.key = key::seat_side, .real = &Settings::seat_side},
    Field{.key = key::others_see_outfit, .flag = &Settings::others_see_outfit},
    Field{.key = key::seat_forward_foot, .real = &Settings::seat_forward_foot},
    Field{.key = key::seat_up_foot, .real = &Settings::seat_up_foot},
    Field{.key = key::seat_side_foot, .real = &Settings::seat_side_foot},
    Field{.key = key::fov_scale, .real = &Settings::fov_scale},
    Field{.key = key::frame_lag, .whole = &Settings::frame_lag},
    Field{.key = key::hide_body, .whole = &Settings::hide_body},
    Field{.key = key::hide_body_foot, .whole = &Settings::hide_body_foot},
    Field{.key = key::view_mode, .whole = &Settings::view_mode},
    Field{.key = key::stereo_mode, .whole = &Settings::stereo_mode},
    Field{.key = key::depth_strength, .real = &Settings::depth_strength},
    Field{.key = key::lens_effects_off, .flag = &Settings::lens_effects_off},
    Field{.key = key::controllers, .flag = &Settings::controllers},
    Field{.key = key::auto_first_person, .flag = &Settings::auto_first_person},
    Field{.key = key::edge_widening, .whole = &Settings::edge_widening},
    Field{.key = key::gap_fill, .whole = &Settings::gap_fill},
    Field{.key = key::tilt_filter, .real = &Settings::tilt_filter},
    Field{.key = key::walk_where_you_look, .flag = &Settings::walk_where_you_look},
    Field{.key = key::look_turn, .whole = &Settings::look_turn_degrees},
    Field{.key = key::look_turn_style, .whole = &Settings::look_turn_style},
    Field{.key = key::smooth_turn_speed, .real = &Settings::smooth_turn_speed},
    Field{.key = key::grab_view, .flag = &Settings::grab_view},
    Field{.key = key::crouch_follow, .real = &Settings::crouch_follow},
    Field{.key = key::grab_body, .flag = &Settings::grab_body},
    Field{.key = key::bail_camera, .flag = &Settings::bail_camera},
    Field{.key = key::comfort_vignette, .real = &Settings::comfort_vignette},
    Field{.key = key::third_distance, .real = &Settings::third_distance},
    Field{.key = key::third_height, .real = &Settings::third_height},
    Field{.key = key::theater_distance, .real = &Settings::theater_distance},
    Field{.key = key::theater_width, .real = &Settings::theater_width},
    Field{.key = key::chat_in_vr, .flag = &Settings::chat_in_vr},
    Field{.key = key::chat_on_hand, .flag = &Settings::chat_on_hand},
    Field{.key = key::chat_view_size, .real = &Settings::chat_view_size},
    Field{.key = key::chat_view_x, .real = &Settings::chat_view_x},
    Field{.key = key::chat_view_y, .real = &Settings::chat_view_y},
    Field{.key = key::recenter_key, .whole = &Settings::recenter_key},
};

// Settings saved before the on-foot seat existed: on foot starts where the board seat is.
void inherit_foot_seat(Settings& value) {
    value.seat_forward_foot = value.seat_forward;
    value.seat_up_foot = value.seat_up;
    value.seat_side_foot = value.seat_side;
}

void clamp_choices(Settings& value) {
    value.look_turn_degrees = std::clamp(value.look_turn_degrees, 0, Limits::look_turn_max);
    value.look_turn_style = std::clamp(value.look_turn_style, 0, 1);
    value.smooth_turn_speed = std::clamp(value.smooth_turn_speed, Limits::smooth_turn_speed_min, Limits::smooth_turn_speed_max);
    value.hide_body = std::clamp(value.hide_body, 0, Limits::hide_body_max);
    value.hide_body_foot = std::clamp(value.hide_body_foot, 0, Limits::hide_body_max);
    value.stereo_mode = std::clamp(value.stereo_mode, 0, Limits::stereo_mode_max);
    value.edge_widening = std::clamp(value.edge_widening, 0, Limits::edge_widening_max);
    value.gap_fill = std::clamp(value.gap_fill, 0, 1);
}

Json to_json(const Settings& value) {
    auto result = Json::object();
    for (const auto& field : fields) {
        if (field.flag) result[field.key] = value.*field.flag;
        if (field.real) result[field.key] = static_cast<double>(value.*field.real);
        if (field.whole) result[field.key] = static_cast<double>(value.*field.whole);
    }
    return result;
}

void from_json(const Json& saved, Settings& value) {
    for (const auto& field : fields) {
        if (!saved.contains(field.key)) continue;
        const auto& item = saved[field.key];
        if (field.flag && item.is_boolean()) value.*field.flag = item.get<bool>();
        if (!item.is_number()) continue;
        const auto number = item.get<double>();
        if (!std::isfinite(number) || std::abs(number) > 1e6) continue;
        if (field.real) value.*field.real = static_cast<float>(number);
        if (field.whole) value.*field.whole = static_cast<int>(std::lround(number));
    }
    if (!saved.contains(key::seat_forward_foot)) inherit_foot_seat(value);
    clamp_choices(value);
}

void load() noexcept {
    try {
        auto value = settings();
        for (const auto& field : fields) {
            if (field.flag) {
                if (const auto saved = profile_runtime::local_preference(field.key)) value.*field.flag = *saved;
            } else if (const auto saved = number(field.key); saved && std::abs(*saved) < 1e6) {
                if (field.real) value.*field.real = static_cast<float>(*saved);
                if (field.whole) value.*field.whole = static_cast<int>(std::lround(*saved));
            }
        }
        if (!number(key::seat_forward_foot)) inherit_foot_seat(value);
        // Older builds offered choices that are gone; keep the rest.
        clamp_choices(value);
        // Out-of-range saves keep the defaults rather than half-applying.
        (void)set_settings(value);
        (void)take_settings_changed(); // Nothing new to save.

        // Saved presets: a JSON list of {name, settings}, stored as text.
        std::vector<Preset> saved;
        if (const auto text = profile_runtime::local_value(presets_key); text && text->is_string()) {
            const auto list = Json::parse(text->get<std::string>());
            if (list.is_array())
                for (const auto& item : list) {
                    if (!item.is_object() || !item.contains("name") || !item["name"].is_string() || !item.contains("settings")) continue;
                    Preset preset{item["name"].get<std::string>(), Settings{}, false};
                    from_json(item["settings"], preset.settings);
                    if (!preset.name.empty() && valid(preset.settings)) saved.push_back(std::move(preset));
                }
        }
        set_saved_presets(std::move(saved));
        (void)take_presets_changed();
    } catch (...) {}
}

void save() noexcept {
    try {
        const auto value = settings();
        std::vector<std::pair<std::string, Json>> values;
        const auto saved = to_json(value);
        for (const auto& field : fields) values.emplace_back(field.key, saved[field.key]);
        profile_runtime::set_local_values(values);
    } catch (...) { /* Best effort; the settings already apply. */ }
}

void save_presets() noexcept {
    try {
        auto list = Json::array();
        for (const auto& preset : saved_presets()) {
            auto item = Json::object();
            item["name"] = preset.name;
            item["settings"] = to_json(preset.settings);
            list.push_back(std::move(item));
        }
        profile_runtime::set_local_values({{presets_key, Json(list.dump())}});
    } catch (...) {}
}
}

void profile_tick() noexcept {
    if (!loaded) {
        loaded = true;
        load();
        return;
    }
    const auto now = GetTickCount64();
    if (take_presets_changed()) save_presets();
    if (take_settings_changed()) {
        pending = true;
        due = now + save_delay_ms;
    }
    if (pending && now >= due) {
        pending = false;
        save();
    }
}
}
