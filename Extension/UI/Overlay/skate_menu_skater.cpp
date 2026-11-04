#include "skate_menu_internal.h"

#include <algorithm>
#include <array>
#include <cmath>

// The SKATER page.
namespace dingosdk::overlay::menu {
namespace {
// The flight speed Freecam and Noclip share, on both of their cards. It can be set with neither on, so it
// is ready before either is: the speed is kept and saved by itself, and both read it as they fly.
void flight_speed_row(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, const char* tooltip) {
    const auto& debug = model.debug;
    field(menu, "Flight speed", tooltip);
    ImGui::BeginDisabled(!callbacks.queue_debug);
    constexpr std::array<float, 7> speeds{0.6f, 3, 5, 15, 60, 300, 1500};
    constexpr std::array<const char*, 7> labels{"Precise", "Slow", "Cruise", "Default", "Fast", "Travel", "Maximum"};
    int selected = 3;
    for (int i = 0; i < static_cast<int>(speeds.size()); ++i)
        if (std::abs(debug.camera_speed - speeds[i]) < .01f) selected = i;
    if (slider_int("##flight-speed", &selected, 0, 6, labels[selected], ImGuiSliderFlags_NoInput))
        debug_request(menu, callbacks, {DebugAction::set_camera_speed, false, speeds[selected]});
    ImGui::EndDisabled();
}
}

void camera_controls(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& debug = model.debug;
    begin_card(menu, "freecam", "FREECAM");
    bool flight = debug.free_camera;
    if (toggle_row(menu, "Freecam", "Detach the camera and explore.", flight,
            debug.available && debug.camera_available && callbacks.queue_debug))
        debug_request(menu, callbacks, {DebugAction::set_free_camera, flight});
    bool freecam_controller = model.bindings.freecam_controller;
    if (toggle_row(menu, "Controller flies the freecam", "Block player input and use the controller for the Freecam.", freecam_controller,
            model.bindings.available && callbacks.queue_debug)) {
        std::array<char, 512> result{};
        callbacks.queue_console_command(callbacks.user, freecam_controller ? "freecam_controller true" : "freecam_controller false", result.data(), result.size());
    }
    flight_speed_row(menu, model, callbacks, "Also used by Noclip. Set it before or while flying.");
    const bool custom_fov = debug.free_camera_fov > 0;
    field(menu, "Field of view");
    int free_fov = static_cast<int>(std::lround(custom_fov ? debug.free_camera_fov : debug.camera_fov));
    ImGui::BeginDisabled(!callbacks.queue_debug);
    if (slider_int("##free-camera-fov", &free_fov, 40, 120, custom_fov ? "%d degrees" : "%d degrees (game)", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_free_camera_fov, false, static_cast<float>(free_fov)});
    ImGui::EndDisabled();
    // The camera's two one-off actions share a row, under the settings they go with.
    const int pressed = button_row(menu, "freecam-actions", {
        {"TELEPORT TO FREECAM", debug.free_camera && debug.camera_position_valid && callbacks.queue_console_command != nullptr, false,
         "Move the skater to where the freecam is."},
        {"GAME FIELD OF VIEW", custom_fov && callbacks.queue_debug != nullptr, false, "The freecam's field of view back to the game's own."}});
    if (pressed == 0) {
        std::array<char, 512> result{};
        callbacks.queue_console_command(callbacks.user, "tp_to_freecam", result.data(), result.size());
    }
    if (pressed == 1) debug_request(menu, callbacks, {DebugAction::set_free_camera_fov, false, 0.0f});
    if (debug.free_camera) note("Close the menu to fly. WASD / Q E move; hold the right mouse button to look.");
    if (!debug.camera_available) warn(debug.camera_unavailable.c_str());
    note("The replay editor's keyframe smoothing is in Settings > Replays.");
    end_card();
}

void first_person_controls(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    namespace fp = dingosdk::first_person;
    const auto& debug = model.debug;
    auto settings = debug.first_person_arm;
    const bool can_edit = debug.available && callbacks.queue_debug;
    begin_card(menu, "first-person", "FIRST PERSON");
    bool first_person = debug.first_person;
    if (toggle_row(menu, "First person", "See the world through the skater's eyes.", first_person,
            debug.available && debug.camera_available && callbacks.queue_debug))
        debug_request(menu, callbacks, {DebugAction::set_first_person, first_person});
    field(menu, "Field of view");
    int fov = static_cast<int>(std::lround(debug.first_person_fov > 0 ? debug.first_person_fov : debug.camera_fov));
    ImGui::BeginDisabled(!debug.first_person || !callbacks.queue_debug);
    if (slider_int("##first-person-fov", &fov, 40, 120, "%d degrees", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_first_person_fov, false, static_cast<float>(fov)});
    ImGui::EndDisabled();
    if (toggle_row(menu, "Third person on foot",
            "Use the game's camera while walking; first person comes back when you get on the board.",
            settings.board_only, can_edit))
        debug_request(menu, callbacks, {DebugAction::set_first_person_board_only, settings.board_only});
    if (toggle_row(menu, "First person in the camera cycle",
            "R3 on the board goes high camera, low camera, first person, then high again.",
            settings.camera_cycle, can_edit))
        debug_request(menu, callbacks, {DebugAction::set_first_person_camera_cycle, settings.camera_cycle});
    note("The camera cycle needs Camera height control on in the game's settings (Gameplay > Advanced Camera).");
    end_card();

    begin_card(menu, "true-first-person", "TRUE FIRST PERSON");
    if (toggle_row(menu, "True first person",
            "Keep the horizon level and the view steady; the head's heading is still followed.", settings.stabilize, can_edit))
        debug_request(menu, callbacks, {DebugAction::set_first_person_stabilize, settings.stabilize});
    ImGui::BeginDisabled(!settings.stabilize || !can_edit);
    const auto percent = [&](const char* label, const char* id, float value, DebugAction action) {
        field(menu, label);
        if (slider_float(id, &value, 0, fp::strength_limit, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            debug_request(menu, callbacks, {action, false, value});
    };
    percent("Smoothing", "##fp-smoothing", settings.smoothing, DebugAction::set_first_person_smoothing);
    percent("Head nod (up / down)", "##fp-head-pitch", settings.head_pitch, DebugAction::set_first_person_head_pitch);
    percent("Head tilt (horizon roll)", "##fp-head-roll", settings.head_roll, DebugAction::set_first_person_head_roll);
    percent("Head bob", "##fp-bob", settings.bob, DebugAction::set_first_person_bob);
    if (toggle_row(menu, "Follow flips", "Turn the view with the skater during flips and bails.", settings.follow_flips,
            can_edit && settings.stabilize))
        debug_request(menu, callbacks, {DebugAction::set_first_person_follow_flips, settings.follow_flips});
    ImGui::EndDisabled();
    note("Nod, tilt and bob are the share of the head's own movement kept. Spring arm > Pitch looks further down at the board.");
    end_card();

    // The spring arm is for fine tuning: folded away until asked for.
    if (fold_card(menu, "spring-arm", "SPRING ARM", "Offsets and lag", false)) {
        if (toggle_row(menu, "Spring", "Let the camera lag behind and soften animated head movement.", settings.enabled, can_edit))
            debug_request(menu, callbacks, {DebugAction::set_first_person_spring, settings.enabled});
        ImGui::BeginDisabled(!can_edit);
        const auto slider = [&](const char* label, float value, float low, float high, const char* format, DebugAction action) {
            field(menu, label);
            ImGui::PushID(label);
            if (slider_float("##arm-value", &value, low, high, format, ImGuiSliderFlags_AlwaysClamp))
                debug_request(menu, callbacks, {action, false, value});
            ImGui::PopID();
        };
        note("Position offset, metres from the head");
        slider("Right", settings.offset[0], -fp::offset_limit, fp::offset_limit, "%.3f", DebugAction::set_first_person_offset_x);
        slider("Up", settings.offset[1], -fp::offset_limit, fp::offset_limit, "%.3f", DebugAction::set_first_person_offset_y);
        slider("Forward", settings.offset[2], -fp::offset_limit, fp::offset_limit, "%.3f", DebugAction::set_first_person_offset_z);
        note("Rotation offset, degrees");
        slider("Pitch", settings.rotation[0], -fp::rotation_limit, fp::rotation_limit, "%.1f", DebugAction::set_first_person_pitch);
        slider("Yaw", settings.rotation[1], -fp::rotation_limit, fp::rotation_limit, "%.1f", DebugAction::set_first_person_yaw);
        slider("Roll", settings.rotation[2], -fp::rotation_limit, fp::rotation_limit, "%.1f", DebugAction::set_first_person_roll);
        note("Smoothing strength");
        slider("Moving up", settings.up, 0, fp::strength_limit, "%.0f%%", DebugAction::set_first_person_spring_up);
        slider("Moving down", settings.down, 0, fp::strength_limit, "%.0f%%", DebugAction::set_first_person_spring_down);
        slider("Moving left", settings.left, 0, fp::strength_limit, "%.0f%%", DebugAction::set_first_person_spring_left);
        slider("Moving right", settings.right, 0, fp::strength_limit, "%.0f%%", DebugAction::set_first_person_spring_right);
        if (button_row(menu, "arm-reset", {{"RESET ARM SETTINGS", can_edit, false, "Every spring arm setting back to its default."}}) == 0)
            debug_request(menu, callbacks, {DebugAction::reset_first_person_arm});
        ImGui::EndDisabled();
        note("Higher smoothing follows more slowly; 0% follows directly. Offsets work with Spring off. "
             "Depth and roll use the average strength.");
    }
    end_card();
}

// Skater > CHEATS: flying through things, never bailing, and the boosts.
void cheats_tab(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& debug = model.debug;
    begin_card(menu, "cheats", "NOCLIP AND NO BAIL");
    bool noclip = debug.noclip;
    if (toggle_row(menu, "Noclip", "Fly with the normal player camera. Includes No Bail; uses the Freecam flight speed.", noclip,
            (debug.noclip_available || debug.noclip) && callbacks.queue_debug))
        debug_request(menu, callbacks, {DebugAction::set_noclip, noclip});
    if (!debug.noclip_available && !debug.noclip) note(debug.noclip_unavailable.c_str());
    bool no_bail = debug.no_bail;
    const char* bail_help = debug.noclip && debug.no_bail_active
        ? "Protection is automatic during noclip. Enable this to keep it when flight ends."
        : debug.no_bail && !debug.no_bail_active
        ? "Enabled; waiting for an active local skater."
        : "Prevent new wipeouts. Recover from any current bail before enabling.";
    if (toggle_row(menu, "No Bail", bail_help, no_bail,
            (debug.no_bail_available || debug.no_bail) && callbacks.queue_debug))
        debug_request(menu, callbacks, {DebugAction::set_no_bail, no_bail});
    flight_speed_row(menu, model, callbacks, "Noclip's speed, shared with the Freecam. Set it before or while flying.");
    note("Close the menu to fly. Controller: left stick moves, right stick looks, RT / LT rise and fall, click the left stick to boost. "
         "Keyboard: WASD / Q E, Shift to boost.");
    end_card();

    // The sliders cover the usual range; the number box beside each takes more (up to 10000, Revert Boost x100).
    const auto boost_speed_ok = [](float speed) { return std::isfinite(speed) && speed >= 0.0f && speed <= 10000.0f; };
    begin_card(menu, "boosts", "BOOSTS", "0 is off");
    ImGui::BeginDisabled(!callbacks.queue_debug);
    field(menu, "Forward boost");
    float forward_velocity = debug.forward_velocity_speed;
    if (slider_float("##forward-velocity", &forward_velocity, 0.0f, 300.0f, "+%.1f") && boost_speed_ok(forward_velocity))
        debug_request(menu, callbacks, {DebugAction::set_forward_velocity_speed, false, forward_velocity});
    field(menu, "Up boost");
    float up_velocity = debug.up_velocity_speed;
    if (slider_float("##up-velocity", &up_velocity, 0.0f, 25.0f, "+%.1f") && boost_speed_ok(up_velocity))
        debug_request(menu, callbacks, {DebugAction::set_up_velocity_speed, false, up_velocity});
    field(menu, "Off-board up boost");
    float offboard_up_velocity = debug.offboard_up_velocity_speed;
    if (slider_float("##offboard-up-velocity", &offboard_up_velocity, 0.0f, 25.0f, "+%.1f") && boost_speed_ok(offboard_up_velocity))
        debug_request(menu, callbacks, {DebugAction::set_offboard_up_velocity_speed, false, offboard_up_velocity});
    ImGui::EndDisabled();
    bool revert_boost = debug.revert_boost;
    if (toggle_row(menu, "Revert Boost", "Speed boost when you land an auto revert or a board bend. Clean 180s get nothing.",
            revert_boost, callbacks.queue_debug != nullptr))
        debug_request(menu, callbacks, {DebugAction::set_revert_boost_enabled, revert_boost});
    ImGui::BeginDisabled(!callbacks.queue_debug || !debug.revert_boost);
    field(menu, "Revert Boost strength", "Auto reverts add 1.5 m/s and board bends 2 to 2.6 m/s, times this. Type up to x100 in the box.");
    float revert_speed = debug.revert_boost_speed;
    if (slider_float("##revert-boost-speed", &revert_speed, 0.5f, 5.0f, "x%.1f") && revert_speed >= 0.5f && revert_speed <= 100.0f)
        debug_request(menu, callbacks, {DebugAction::set_revert_boost_speed, false, revert_speed});
    ImGui::EndDisabled();
    note("Boost buttons are set in Settings > Controls. Type a number past a slider's end for more.");
    note("Dark Pop is on SANDBOX > TUNE > TRICKLINING.");
    end_card();
}

// Dark Pop (Extension/Skater/no_bail.cpp): shown with the tricklining tools on SANDBOX > TUNE > TRICKLINING.
// Dark Pop 3.0 (Sivaes/darkpop): the pre-0.32.0 dark pop put back through the game's own sink-and-pop state. One switch;
// everything that tested out is on by default. Not the auto-release version 0.5.0 shipped (which timed a D-pad Right
// release through XInput and could not act when another XInput hook, such as Steam's overlay, answered first).
void dark_pop_controls(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& debug = model.debug;
    if (fold_card(menu, "dark-pop", "DARK POP", "Catch a flip dark, press a D-pad direction in the air, land it dark")) {
        bool dark_pop = debug.dark_pop;
        if (toggle_row(menu, "Dark Pop", "The pre-0.32.0 dark pop: catch a flip dark, press D-pad Left, Down or Right in the air, land it still dark.",
                dark_pop, callbacks.queue_debug != nullptr))
            debug_request(menu, callbacks, {DebugAction::set_dark_pop_enabled, dark_pop});
        note("The body sinks into the ground and is thrown back up, about 8 m/s, into a normal flight: tricks, camera, landing. "
             "Any trick, any height, 180s included. Turn the board back over before landing to land normally.");
        note("Play with Dark Catch Assist off and Legacy Dark Catches on; No Bail can stay off.");
        warn("Known: a landing caught exactly flat (board up -1.00 in the log) sometimes stays dark in the air and bails.");
        bool hide_dpad = debug.dark_pop_hide_dpad;
        if (toggle_row(menu, "Hide the D-pad from the game in the air",
                "In the air the game sees D-pad Left, Down and Right let go (Dark Pop still sees them), so Right never opens the Quick Drop radial.",
                hide_dpad, callbacks.queue_debug != nullptr))
            debug_request(menu, callbacks, {DebugAction::set_dark_pop_hide_dpad, hide_dpad});
        note("A press held through the landing stays hidden until you let go, and in keyboard / mouse mode the radial's release can't "
             "pick the object browser. On the ground the D-pad works as normal.");
        bool probe = debug.probe;
        if (toggle_row(menu, "Dark pop probe",
                "Records what the game does to dark-pop-probe-<time>.csv and dark-pop-fast-<time>.csv in the logs folder, a new pair each time.",
                probe, callbacks.queue_debug != nullptr))
            debug_request(menu, callbacks, {DebugAction::set_probe_enabled, probe});
        note("Probe: turn it on, do your attempts, turn it off, and send both files.");
        note("Each dark landing and each pop's landing are written to logs\\ReSkate.log (lines starting \"Dark Pop:\").");
        note("Works with any controller the game reads (Xbox or PlayStation): nothing is sent through XInput.");
    }
    end_card();
}

void skater_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.skater_tab, {"FREECAM", "FIRST PERSON", "CHEATS"}, "skater-tabs");
    ImGui::PushID(menu.skater_tab);
    ImGui::BeginChild("skater-tab", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    switch (menu.skater_tab) {
    case 0: camera_controls(menu, model, callbacks); break;
    case 1: first_person_controls(menu, model, callbacks); break;
    default: cheats_tab(menu, model, callbacks); break;
    }
    ImGui::EndChild();
    ImGui::PopID();
}
}
