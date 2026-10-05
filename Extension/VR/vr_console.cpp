// Console commands for VR: "vr" shows the status, "vr.recenter" recentres and
// "vr.<setting>" reads or changes a setting.
#include "Extension/Console/commands.h"
#include "vr.h"
#include <format>
#include <functional>

namespace dingosdk::console {
namespace {
using Settings = vr::Settings;

State settings_state(std::string value) { return {true, std::move(value), {}, {}, false}; }

void flag(Commands& registry, const char* name, const char* description, bool Settings::*member) {
    auto entry = variable(name, description, Group::graphics, argument("0|1", Type::boolean));
    entry.execution = Execution::local;
    entry.inspect = [member](const Model&) { return boolean_state(true, vr::settings().*member); };
    entry.run = [member](const Model&, const Values& args, const Output& out) {
        auto value = vr::settings();
        value.*member = std::get<bool>(args[0]);
        if (!vr::set_settings(value)) out("error: VR setting rejected.");
    };
    entry.reset = [member](const Model&, const Output&) {
        auto value = vr::settings();
        value.*member = Settings{}.*member;
        (void)vr::set_settings(value);
    };
    registry.add(std::move(entry));
}

void real(Commands& registry, const char* name, const char* description, float Settings::*member, double low, double high) {
    auto value_argument = argument("value", Type::number);
    value_argument.minimum = low;
    value_argument.maximum = high;
    auto entry = variable(name, description, Group::graphics, value_argument);
    entry.execution = Execution::local;
    entry.inspect = [member](const Model&) { return settings_state(value_text(static_cast<double>(vr::settings().*member))); };
    entry.run = [member](const Model&, const Values& args, const Output& out) {
        auto value = vr::settings();
        value.*member = static_cast<float>(std::get<double>(args[0]));
        if (!vr::set_settings(value)) out("error: VR setting out of range.");
    };
    entry.reset = [member](const Model&, const Output&) {
        auto value = vr::settings();
        value.*member = Settings{}.*member;
        (void)vr::set_settings(value);
    };
    registry.add(std::move(entry));
}

void whole(Commands& registry, const char* name, const char* description, int Settings::*member, int low, int high) {
    auto value_argument = argument("value", Type::integer);
    value_argument.minimum = low;
    value_argument.maximum = high;
    auto entry = variable(name, description, Group::graphics, value_argument);
    entry.execution = Execution::local;
    entry.inspect = [member](const Model&) { return settings_state(std::to_string(vr::settings().*member)); };
    entry.run = [member](const Model&, const Values& args, const Output& out) {
        auto value = vr::settings();
        value.*member = static_cast<int>(std::get<std::int64_t>(args[0]));
        if (!vr::set_settings(value)) out("error: VR setting out of range.");
    };
    entry.reset = [member](const Model&, const Output&) {
        auto value = vr::settings();
        value.*member = Settings{}.*member;
        (void)vr::set_settings(value);
    };
    registry.add(std::move(entry));
}
}

void register_vr_commands(Commands& registry) {
    using Limits = vr::Limits;
    auto status = action("vr", "Show the VR headset, session and frame pairing status", Group::graphics);
    status.execution = Execution::local;
    status.run = [](const Model&, const Values&, const Output& out) {
        const auto s = vr::status();
        const auto options = vr::settings();
        out(std::format("VR {} | state: {}{}", options.enabled ? "on" : "off", s.state, s.message.empty() ? "" : " | " + s.message));
        if (!s.runtime.empty()) out(std::format("Headset: {} on {}", s.system, s.runtime));
        if (s.recommended_width)
            out(std::format("Recommended {}x{} per eye; game image {}x{}", s.recommended_width, s.recommended_height,
                s.image_width, s.image_height));
        if (s.running) {
            out(std::format("Headset {:.0f} Hz | game presents {:.0f}/s | VR camera frames {:.0f}/s | rendered FOV {:.1f}",
                s.display_hz, s.present_hz, s.camera_hz, s.render_fov_degrees));
            out(std::format("{} | eye images {} | repeats {} | frames {}", s.camera_active ? "VR camera active"
                : s.theater_active ? "Flat screen (first person is off)" : "Idle", s.eye_images, s.repeated_images,
                s.frames_submitted));
            if (s.camera_thread)
                out(std::format("Camera thread {} | Present thread {}{}", s.camera_thread, s.present_thread,
                    s.camera_thread == s.present_thread ? " (same thread)" : ""));
        }
    };
    registry.add(std::move(status));

    auto recenter = action("vr.recenter", "Make the current head direction straight ahead", Group::graphics);
    recenter.execution = Execution::local;
    recenter.run = [](const Model&, const Values&, const Output& out) {
        vr::recenter();
        out("VR view recentred.");
    };
    registry.add(std::move(recenter));

    flag(registry, "vr.enabled", "Show first person in an OpenXR headset", &Settings::enabled);
    flag(registry, "vr.auto_first_person", "Turn first person (the VR camera) on by itself while the headset runs",
        &Settings::auto_first_person);
    flag(registry, "vr.positional", "Move the camera with the headset's position", &Settings::positional);
    flag(registry, "vr.controllers", "The headset's controllers act as the gamepad", &Settings::controllers);
    flag(registry, "vr.lens_effects_off", "Turn vignette and chromatic aberration off while VR runs", &Settings::lens_effects_off);
    flag(registry, "vr.level", "Keep the horizon level; only the skater's heading turns the view", &Settings::level_horizon);
    flag(registry, "vr.fov_vertical", "Treat the game's camera FOV as vertical (0 = horizontal)", &Settings::fov_vertical);
    real(registry, "vr.turn_smoothing", "Third person: seconds for the camera to swing behind the direction of travel", &Settings::turn_smoothing, 0,
        Limits::turn_smoothing_max);
    real(registry, "vr.fov_correction", "Widen the game's FOV only (not the headset's); raise if the world looks magnified",
        &Settings::fov_correction, Limits::fov_correction_min, Limits::fov_correction_max);
    real(registry, "vr.seat_forward", "On the board: metres to move the camera forward of the skater's eyes (negative = back)", &Settings::seat_forward,
        Limits::seat_forward_min, Limits::seat_max);
    real(registry, "vr.seat_side", "On the board: metres to move the camera right of the skater's eyes (negative = left)", &Settings::seat_side,
        Limits::seat_min, Limits::seat_max);
    real(registry, "vr.seat_up", "On the board: metres to raise the camera above the skater's eyes (negative = lower)", &Settings::seat_up,
        Limits::seat_min, Limits::seat_max);
    real(registry, "vr.seat_forward_foot", "On foot: metres to move the camera forward of the skater's eyes (negative = back)",
        &Settings::seat_forward_foot, Limits::seat_forward_min, Limits::seat_max);
    real(registry, "vr.seat_side_foot", "On foot: metres to move the camera right of the skater's eyes (negative = left)",
        &Settings::seat_side_foot, Limits::seat_min, Limits::seat_max);
    real(registry, "vr.seat_up_foot", "On foot: metres to raise the camera above the skater's eyes (negative = lower)",
        &Settings::seat_up_foot, Limits::seat_min, Limits::seat_max);
    real(registry, "vr.world_scale", "Game metres per real metre of head movement and eye separation", &Settings::world_scale,
        Limits::world_scale_min, Limits::world_scale_max);
    real(registry, "vr.fov_scale", "Rendered FOV relative to the headset's", &Settings::fov_scale, Limits::fov_scale_min,
        Limits::fov_scale_max);
    whole(registry, "vr.stereo", "0 alternate eyes, 1 side-by-side input (ReShade SuperDepth3D), 2 depth stereo (built in)", &Settings::stereo_mode, 0,
        Limits::stereo_mode_max);
    whole(registry, "vr.edge_widening", "Depth stereo: pixels near edges are widened by (0-3)", &Settings::edge_widening, 0,
        Limits::edge_widening_max);
    whole(registry, "vr.gap_fill", "Depth stereo holes: 1 background, 0 stretched edge", &Settings::gap_fill, 0, 1);
    real(registry, "vr.depth_strength", "Depth stereo: eye shift multiplier (1 = real eye separation)", &Settings::depth_strength,
        0.0f, Limits::depth_strength_max);
    real(registry, "vr.tilt_filter", "Seconds of pitch/roll wobble filtering (Level horizon off)",
        &Settings::tilt_filter, 0.0f, Limits::tilt_filter_max);
    whole(registry, "vr.view", "0 first person, 1 third person", &Settings::view_mode, 0, Limits::view_mode_max);
    real(registry, "vr.vignette", "Comfort vignette: edges darken while the view turns without your head (0 off, 1 strong)",
        &Settings::comfort_vignette, 0.0f, Limits::comfort_vignette_max);
    whole(registry, "vr.turn_style", "Right-stick turns: 0 snap, 1 smooth", &Settings::look_turn_style, 0, 1);
    real(registry, "vr.turn_speed", "Smooth right-stick turns: degrees per second", &Settings::smooth_turn_speed,
        Limits::smooth_turn_speed_min, Limits::smooth_turn_speed_max);
    real(registry, "vr.crouch", "How much crouching lowers the view (0 none, 1 all)", &Settings::crouch_follow, 0.0f, 1.0f);
    flag(registry, "vr.grab_view", "Grabs: the view moves outside, behind the skater", &Settings::grab_view);
    flag(registry, "vr.grab_body", "Grabs: first person shows the whole skater", &Settings::grab_body);
    flag(registry, "vr.bail_camera", "During a wipeout, watch the skater from a fixed spot behind them", &Settings::bail_camera);
    flag(registry, "vr.walk_where_you_look", "On foot the view turns only with your head (and look turns); the stick walks where you look",
        &Settings::walk_where_you_look);
    whole(registry, "vr.look_turn", "On foot: degrees the right stick turns the view per flick (0 off)", &Settings::look_turn_degrees, 0,
        Limits::look_turn_max);
    real(registry, "vr.third_distance", "Third person: metres behind the skater", &Settings::third_distance,
        Limits::third_distance_min, Limits::third_distance_max);
    real(registry, "vr.third_height", "Third person: metres above the skater's head", &Settings::third_height,
        Limits::third_height_min, Limits::third_height_max);
    whole(registry, "vr.hide_body", "Hide in first person on the board: 0 head, 1 neck and head, 2 torso (arms kept), 3 waist up, "
        "4 all but the feet (FeetOnly mod)",
        &Settings::hide_body, 0, Limits::hide_body_max);
    flag(registry, "vr.others_see_outfit", "Feet only: other players see your saved outfit, not the costume",
        &Settings::others_see_outfit);
    whole(registry, "vr.hide_body_foot", "Hide in first person on foot: 0 head, 1 neck and head, 2 torso (arms kept), 3 waist up, "
        "4 all but the feet (FeetOnly mod)",
        &Settings::hide_body_foot, 0, Limits::hide_body_max);
    whole(registry, "vr.frame_lag", "Presents between a camera frame and its image (fixes swapped eyes)", &Settings::frame_lag, 0,
        Limits::frame_lag_max);
    real(registry, "vr.theater_distance", "Flat screen distance in metres", &Settings::theater_distance,
        Limits::theater_distance_min, Limits::theater_distance_max);
    flag(registry, "vr.chat_in_vr", "Chat on its own panel in VR (0: in the game image)", &Settings::chat_in_vr);
    flag(registry, "vr.chat_on_hand", "Chat panel on the left controller (the view's spot while it is not tracked)",
        &Settings::chat_on_hand);
    real(registry, "vr.chat_view_size", "Chat panel in the view: width in metres (at 0.85 m)", &Settings::chat_view_size,
        Limits::chat_view_size_min, Limits::chat_view_size_max);
    real(registry, "vr.chat_view_x", "Chat panel in the view: metres right (+) or left (-) of straight ahead",
        &Settings::chat_view_x, -Limits::chat_view_offset_max, Limits::chat_view_offset_max);
    real(registry, "vr.chat_view_y", "Chat panel in the view: metres above (+) or below (-) straight ahead",
        &Settings::chat_view_y, -Limits::chat_view_offset_max, Limits::chat_view_offset_max);
    real(registry, "vr.theater_width", "Flat screen width in metres", &Settings::theater_width, Limits::theater_width_min,
        Limits::theater_width_max);
    whole(registry, "vr.recenter_key", "Virtual-key code that recentres the view (0 = none; 119 = F8)", &Settings::recenter_key, 0,
        0xfe);
}
}
