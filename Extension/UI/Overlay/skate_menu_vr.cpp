#include "skate_menu_internal.h"
#include "Extension/VR/vr.h"
#include "Extension/VR/vr_costume.h"

#include <algorithm>
#include <format>

// SKATER > VR: first person in an OpenXR headset. Cards: headset, presets, view, comfort,
// controls, image, and advanced (hidden until asked for). Settings that do not apply to the
// current choices are not shown.
namespace dingosdk::overlay::menu {
namespace {
// A labelled slider that applies its value as soon as it changes.
template<class Apply>
void slider(SkateMenu& menu, const char* label, const char* tooltip, float value, float low, float high,
    const char* format, Apply&& apply) {
    field(menu, label, tooltip);
    ImGui::PushID(label);
    if (ImGui::SliderFloat("##vr-value", &value, low, high, format, ImGuiSliderFlags_AlwaysClamp)) apply(value);
    ImGui::PopID();
}
// A labelled strip of option tiles.
bool tiles(SkateMenu& menu, const char* label, const char* tooltip, const char* id, int& value,
    std::initializer_list<const char*> names) {
    field(menu, label, tooltip);
    return choice(menu, id, value, names);
}
}

void vr_controls(SkateMenu& menu, const Model& model) {
    using Limits = vr::Limits;
    const auto vr_status = vr::status();
    auto options = vr::settings();
    bool changed = false;
    const auto set = [&](auto& member) { return [&](auto v) { member = v; changed = true; }; };

    // ---- Headset: on/off, state, the two main actions.
    begin_card(menu, "vr", "VR HEADSET", "OpenXR: Virtual Desktop, Meta Horizon Link, SteamVR");
    if (toggle_row(menu, "VR", "Show first person in your headset. Start the headset's PC VR app first.", options.enabled))
        changed = true;
    if (toggle_row(menu, "Take over the camera", "While the headset runs, the VR camera turns on by itself (also after a "
            "map change). Off: the game's camera shows on a flat screen until you turn on First person.", options.auto_first_person))
        changed = true;
    info(menu, "State", vr_status.message.empty() ? vr_status.state : vr_status.state + " - " + vr_status.message);
    if (!vr_status.system.empty()) info(menu, "Headset", vr_status.system + " on " + vr_status.runtime);
    if (vr_status.running) {
        info(menu, "Frames", std::format("headset {:.0f} Hz, game {:.0f} fps, VR camera {:.0f} fps",
            vr_status.display_hz, vr_status.present_hz, vr_status.camera_hz));
        info(menu, "Image", std::format("game {}x{}, headset wants {}x{} per eye", vr_status.image_width,
            vr_status.image_height, vr_status.recommended_width, vr_status.recommended_height));
    }
    if (primary_button(menu, "Recenter view", vr_status.running)) vr::recenter();
    if (primary_button(menu, "Recommended settings", true)) {
        // Every VR value back to the tested defaults; VR stays on or off.
        vr::Settings recommended;
        recommended.enabled = options.enabled;
        recommended.recenter_key = options.recenter_key;
        options = recommended;
        changed = true;
    }
    if (!model.debug.first_person && !options.auto_first_person)
        note("Turn on First person in the CAMERA tab to see the world in 3D. Until then the game shows on a flat screen.");
    note("For a sharp, smooth image: windowed 2880x2880 in the launcher; V-Sync, frame generation and motion blur off.");
    end_card();

    // ---- Presets.
    begin_card(menu, "vr-presets", "PRESETS");
    {
        static int selected = 0;
        static char name[41] = "";
        const auto list = vr::presets();
        selected = std::clamp(selected, 0, static_cast<int>(list.size()) - 1);
        field(menu, "Preset", "Built-in presets, then the ones you saved.");
        if (ImGui::BeginCombo("##vr-preset", list[static_cast<std::size_t>(selected)].name.c_str())) {
            for (int i = 0; i < static_cast<int>(list.size()); ++i)
                if (ImGui::Selectable(list[static_cast<std::size_t>(i)].name.c_str(), i == selected)) selected = i;
            ImGui::EndCombo();
        }
        const auto& chosen = list[static_cast<std::size_t>(selected)];
        if (primary_button(menu, "Apply preset", true) && vr::apply_preset(chosen.name)) {
            options = vr::settings(); // The preset is applied; nothing else to set this frame.
            changed = false;
        }
        if (!chosen.builtin && primary_button(menu, "Delete preset", true)) (void)vr::delete_preset(chosen.name);
        field(menu, "Save as", "A name for the current settings (saving under an existing name replaces it).");
        ImGui::InputText("##vr-preset-name", name, sizeof(name));
        if (primary_button(menu, "Save current settings", name[0] != 0) && vr::save_preset(name)) {
            const auto saved = vr::presets();
            for (int i = 0; i < static_cast<int>(saved.size()); ++i)
                if (saved[static_cast<std::size_t>(i)].name == name) selected = i;
            name[0] = 0;
        }
    }
    end_card();

    // ---- View: where the camera is and what it turns with. First person looks through the
    // skater's eyes; third person follows the movement like the game's camera.
    const bool first = options.view_mode == 0;
    begin_card(menu, "vr-view", "VIEW");
    if (tiles(menu, "View", "First person: through the skater's eyes. Third person: behind the skater, like the game's camera.",
            "##vr-view", options.view_mode, {"First person", "Third person"}))
        changed = true;
    if (first) {
        // Feet only needs the FeetOnly mod (its costume): offered when installed or already chosen.
        const bool feet = vr::feet_only_installed() || options.hide_body == Limits::hide_feet_only ||
            options.hide_body_foot == Limits::hide_feet_only;
        const char* feet_tip = feet ? " Feet only: your skater wears the FeetOnly mod's costume while VR runs, so only the "
            "shoes show (the flip & bail view and grabs show the whole skater; your saved outfit does not change)." : "";
        const auto board_tip = std::format("How much of your skater you see on the board (the rest is hidden so it does not "
            "clip the camera).{}", feet_tip);
        const auto foot_tip = std::format("How much of your skater you see on foot. The hands carry the board here, so a "
            "choice with arms looks best.{}", feet_tip);
        const auto seen = [&](const char* label, const std::string& tip, const char* id, int& value) {
            return feet ? tiles(menu, label, tip.c_str(), id, value,
                              {"Neck down", "Shoulders down", "Arms and legs", "Legs only", "Feet only"})
                        : tiles(menu, label, tip.c_str(), id, value, {"Neck down", "Shoulders down", "Arms and legs", "Legs only"});
        };
        if (seen("You see (board)", board_tip, "##vr-hide", options.hide_body)) changed = true;
        if (seen("You see (on foot)", foot_tip, "##vr-hide-foot", options.hide_body_foot)) changed = true;
        if (feet && toggle_row(menu, "Others see your outfit", "Feet only: other players see your skater in your saved "
                "outfit; the costume is only in your view.", options.others_see_outfit))
            changed = true;
        // One seat on the board and one on foot; the view blends between them in about half a second.
        const auto seat = [&](const char* title, const char* when, float& forward, float& side, float& up) {
            section(menu, title);
            ImGui::PushID(title); // the two sets share slider labels
            slider(menu, "Forward", std::format("{}: move the camera forward (+) or back (-) from the skater's eyes.",
                when).c_str(), forward, Limits::seat_forward_min, Limits::seat_max, "%.2f m", set(forward));
            slider(menu, "Sideways", std::format("{}: move the camera right (+) or left (-) of the skater's eyes.",
                when).c_str(), side, Limits::seat_min, Limits::seat_max, "%.2f m", set(side));
            slider(menu, "Height", std::format("{}: raise (+) or lower (-) the camera from the skater's eyes.",
                when).c_str(), up, Limits::seat_min, Limits::seat_max, "%.2f m", set(up));
            ImGui::PopID();
        };
        seat("Camera offset on the board", "On the board (riding, airs, grinds)", options.seat_forward, options.seat_side,
            options.seat_up);
        seat("Camera offset on foot", "On foot", options.seat_forward_foot, options.seat_side_foot, options.seat_up_foot);
        section(menu, "On foot and outside views");
        if (toggle_row(menu, "Walk where you look", "On foot the view turns only with your head and the right stick, so the "
                "stick walks where you look.", options.walk_where_you_look))
            changed = true;
        if (toggle_row(menu, "Flip & bail view", "When the skater goes head over heels (bails, back and front flips), watch "
                "them from just behind; the right stick orbits.", options.bail_camera))
            changed = true;
        if (toggle_row(menu, "Grab view", "While grabbing in the air, watch the skater from just behind; the right stick "
                "orbits.", options.grab_view))
            changed = true;
        if (!options.grab_view &&
            toggle_row(menu, "Show body in grabs", "While grabbing in the air, first person shows the whole skater.", options.grab_body))
            changed = true;
        // Right-stick turns: on foot (walk where you look) and orbiting the outside views.
        if (options.walk_where_you_look || options.bail_camera || options.grab_view) {
            if (tiles(menu, "Stick turning", "How the right stick turns the view on foot and orbits the outside views.",
                    "##vr-turn-style", options.look_turn_style, {"Snap", "Smooth"}))
                changed = true;
            if (options.look_turn_style == 0) {
                constexpr int degrees[]{0, 30, 45, 60, 90};
                int pick = 0;
                for (int i = 0; i < 5; ++i)
                    if (degrees[i] == options.look_turn_degrees) pick = i;
                if (tiles(menu, "Snap angle", "How far a right-stick flick turns the view.", "##vr-look-turn", pick,
                        {"Off", "30", "45", "60", "90"})) {
                    options.look_turn_degrees = degrees[pick];
                    changed = true;
                }
            } else {
                slider(menu, "Turn speed", "Degrees per second with the stick pushed all the way.", options.smooth_turn_speed,
                    Limits::smooth_turn_speed_min, Limits::smooth_turn_speed_max, "%.0f deg/s", set(options.smooth_turn_speed));
            }
        }
    } else {
        slider(menu, "Distance", "Metres behind the skater.", options.third_distance, Limits::third_distance_min,
            Limits::third_distance_max, "%.1f m", set(options.third_distance));
        slider(menu, "Height", "Metres above the skater's head.", options.third_height, Limits::third_height_min,
            Limits::third_height_max, "%.1f m", set(options.third_height));
        slider(menu, "Follow", "Seconds for the camera to swing behind your direction of travel.", options.turn_smoothing, 0,
            Limits::turn_smoothing_max, "%.2f s", set(options.turn_smoothing));
    }
    if (toggle_row(menu, "Level horizon", "Tricks don't tilt the view; only turns do.", options.level_horizon))
        changed = true;
    if (toggle_row(menu, "Head movement", "Leaning moves the camera, not just turning your head.", options.positional))
        changed = true;
    end_card();

    // ---- Comfort.
    begin_card(menu, "vr-comfort", "COMFORT");
    slider(menu, "Comfort vignette", "Darkens the edges while the view turns without your head (turns, flips, bails). 0 is off.",
        options.comfort_vignette, 0.0f, Limits::comfort_vignette_max, options.comfort_vignette > 0 ? "%.2f" : "Off",
        set(options.comfort_vignette));
    slider(menu, "Crouch", "How much crouching (before an ollie, pushing) lowers your view. 0 keeps it level.", options.crouch_follow,
        0.0f, 1.0f, "%.2f", set(options.crouch_follow));
    if (!options.level_horizon)
        slider(menu, "Tilt filter", "Smooths small head wobble; flips still come through.", options.tilt_filter, 0.0f,
            Limits::tilt_filter_max, "%.2f s", set(options.tilt_filter));
    end_card();

    // ---- Controls: the headset's controllers as the gamepad.
    begin_card(menu, "vr-controls", "CONTROLS");
    if (toggle_row(menu, "VR controllers", "Play with the headset's controllers (a gamepad keeps working).", options.controllers))
        changed = true;
    if (options.controllers) {
        info(menu, "Right hand", "A push, B other push, trigger RB, grip grab (RT)");
        info(menu, "Left hand", "X board on/off, Y stop, trigger LB, grip grab (LT)");
        info(menu, "D-pad", "right thumb on the thumbrest + left stick");
        info(menu, "Menu button", "Start; hold for Back");
        info(menu, "Recenter", "hold both stick clicks for 1 s");
    }
    end_card();

    // ---- Chat: ReSkate's chat on its own panel in VR.
    begin_card(menu, "vr-chat", "CHAT");
    if (toggle_row(menu, "Show chat in VR", "ReSkate's chat on a panel in VR instead of in the game image. Type with the PC "
            "keyboard as usual; the headset stays in VR while you type.", options.chat_in_vr))
        changed = true;
    if (options.chat_in_vr) {
        if (toggle_row(menu, "Show chat on left hand controller", "The panel above your left hand, facing you. While the "
                "controller is not tracked (e.g. playing with a gamepad) it shows at the spot below.", options.chat_on_hand))
            changed = true;
        slider(menu, "Chat size", "Width of the chat panel in the view.", options.chat_view_size, Limits::chat_view_size_min,
            Limits::chat_view_size_max, "%.2f m", set(options.chat_view_size));
        slider(menu, "Chat left / right", "Move the chat panel in the view right (+) or left (-).", options.chat_view_x,
            -Limits::chat_view_offset_max, Limits::chat_view_offset_max, "%.2f m", set(options.chat_view_x));
        slider(menu, "Chat up / down", "Move the chat panel in the view up (+) or down (-).", options.chat_view_y,
            -Limits::chat_view_offset_max, Limits::chat_view_offset_max, "%.2f m", set(options.chat_view_y));
    }
    end_card();

    // ---- Image: stereo and field of view.
    begin_card(menu, "vr-image", "IMAGE");
    if (tiles(menu, "Stereo", "Depth: both eyes from every frame and the game's depth (smoothest). Side-by-side: a ReShade "
            "stereo shader draws both eyes. Alternate: each eye every other frame.", "##vr-stereo", options.stereo_mode,
            {"Alternate", "Side-by-side", "Depth"}))
        changed = true;
    if (options.stereo_mode == 2) {
        slider(menu, "Depth strength", "How strong the 3D is; 1 matches your eyes.", options.depth_strength, 0.0f,
            Limits::depth_strength_max, "%.2fx", set(options.depth_strength));
        field(menu, "Edge widening", "Pixels near edges are widened by. More keeps soft edges on their object; less keeps "
            "narrow gaps (board and body) open.");
        int widening = options.edge_widening;
        if (ImGui::SliderInt("##vr-widening", &widening, 0, Limits::edge_widening_max, "%d px", ImGuiSliderFlags_AlwaysClamp)) {
            options.edge_widening = widening;
            changed = true;
        }
        if (tiles(menu, "Gap fill", "What shows in gaps the game never saw (e.g. between board and body).", "##vr-gap",
                options.gap_fill, {"Stretch edge", "Background"}))
            changed = true;
    }
    slider(menu, "Render FOV", "Rendered field of view relative to the headset's. More covers fast head turns.",
        options.fov_scale, Limits::fov_scale_min, Limits::fov_scale_max, "%.2fx", set(options.fov_scale));
    slider(menu, "Zoom fix", "If the world looks magnified, raise this until objects look real-size.", options.fov_correction,
        Limits::fov_correction_min, Limits::fov_correction_max, "%.2fx", set(options.fov_correction));
    if (toggle_row(menu, "Lens effects off", "Vignette and chromatic aberration off while VR runs (your setting comes back after).",
            options.lens_effects_off))
        changed = true;
    end_card();

    // ---- Advanced: scale, pairing, flat screen.
    static bool advanced = false;
    begin_card(menu, "vr-advanced", "ADVANCED");
    (void)toggle_row(menu, "Show", "World scale, eye pairing, FOV axis and the flat screen.", advanced);
    if (advanced) {
        slider(menu, "World scale", "Above 1 the world looks smaller.", options.world_scale, Limits::world_scale_min,
            Limits::world_scale_max, "%.2fx", set(options.world_scale));
        field(menu, "Eye pairing", "Presents between a camera frame and its image. Try the next value if the world looks "
            "doubled or depth looks inside out.");
        int lag = options.frame_lag;
        if (ImGui::SliderInt("##vr-lag", &lag, 0, Limits::frame_lag_max, "%d", ImGuiSliderFlags_AlwaysClamp)) {
            options.frame_lag = lag;
            changed = true;
        }
        if (toggle_row(menu, "Vertical FOV", "The game's camera FOV is vertical. Turn off if the world looks stretched.",
                options.fov_vertical))
            changed = true;
        slider(menu, "Screen distance", "The flat screen shown while first person is off.", options.theater_distance,
            Limits::theater_distance_min, 10.0f, "%.1f m", set(options.theater_distance));
        slider(menu, "Screen width", "The flat screen shown while first person is off.", options.theater_width,
            Limits::theater_width_min, 10.0f, "%.1f m", set(options.theater_width));
    }
    end_card();

    if (changed) (void)vr::set_settings(options);
}
}
