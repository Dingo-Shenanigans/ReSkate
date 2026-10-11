#include "skate_menu_internal.h"

#include <array>
#include <cmath>

// The SKATER page.
namespace dingosdk::overlay::menu {
namespace {
// A slider row with a trailing button (e.g. "Default") after a field() label.
float trailing_width(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
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
    if (toggle_row(menu, "Block Input(Enable Controller Support)", "Block player input and use the controller for the Freecam.", freecam_controller,
            model.bindings.available && callbacks.queue_debug)) {
        std::array<char, 512> result{};
        callbacks.queue_console_command(callbacks.user, freecam_controller ? "freecam_controller true" : "freecam_controller false", result.data(), result.size());
    }
    field(menu, "Teleport to Freecam");
    ImGui::BeginDisabled(!debug.free_camera || !debug.camera_position_valid || !callbacks.queue_console_command);
    if (ImGui::Button("Teleport", ImVec2(-1, 0))) {
        std::array<char, 512> result{};
        callbacks.queue_console_command(callbacks.user, "tp_to_freecam", result.data(), result.size());
    }
    ImGui::EndDisabled();
    {
        field(menu, "Field of view");
        const bool custom = debug.free_camera_fov > 0;
        int free_fov = static_cast<int>(std::lround(custom ? debug.free_camera_fov : debug.camera_fov));
        const float reset = trailing_width("Default");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reset - ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginDisabled(!callbacks.queue_debug);
        if (ImGui::SliderInt("##free-camera-fov", &free_fov, 40, 120, custom ? "%d degrees" : "%d degrees (game)",
                ImGuiSliderFlags_AlwaysClamp))
            debug_request(menu, callbacks, {DebugAction::set_free_camera_fov, false, static_cast<float>(free_fov)});
        ImGui::SameLine();
        ImGui::BeginDisabled(!custom);
        if (ImGui::Button("Default", ImVec2(reset, 0)))
            debug_request(menu, callbacks, {DebugAction::set_free_camera_fov, false, 0.0f});
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
    field(menu, "Flight speed", "Also used by Noclip.");
    ImGui::BeginDisabled((!debug.free_camera && !debug.noclip) || !debug.camera_available || !callbacks.queue_debug);
    constexpr std::array<float, 7> speeds{0.6f, 3, 5, 15, 60, 300, 1500};
    constexpr std::array<const char*, 7> labels{"Precise", "Slow", "Cruise", "Default", "Fast", "Travel", "Maximum"};
    int selected = 3;
    for (int i = 0; i < static_cast<int>(speeds.size()); ++i)
        if (std::abs(debug.camera_speed - speeds[i]) < .01f) selected = i;
    if (ImGui::SliderInt("##flight-speed", &selected, 0, 6, labels[selected], ImGuiSliderFlags_NoInput))
        debug_request(menu, callbacks, {DebugAction::set_camera_speed, false, speeds[selected]});
    ImGui::EndDisabled();
    if (debug.free_camera) note("Close the menu to fly. WASD / Q E move; hold the right mouse button to look.");
    if (!debug.camera_available) warn(debug.camera_unavailable.c_str());
    end_card();

    begin_card(menu, "first-person", "FIRST PERSON");
    bool first_person = debug.first_person;
    if (toggle_row(menu, "First person", "See the world through the skater's eyes.", first_person,
            debug.available && debug.camera_available && callbacks.queue_debug))
        debug_request(menu, callbacks, {DebugAction::set_first_person, first_person});
    field(menu, "Field of view");
    int fov = static_cast<int>(std::lround(debug.first_person_fov > 0 ? debug.first_person_fov : debug.camera_fov));
    ImGui::BeginDisabled(!debug.first_person || !callbacks.queue_debug);
    if (ImGui::SliderInt("##first-person-fov", &fov, 40, 120, "%d degrees", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_first_person_fov, false, static_cast<float>(fov)});
    ImGui::EndDisabled();
    {
        auto settings = debug.first_person_arm;
        const bool can_edit = debug.available && callbacks.queue_debug;
        if (toggle_row(menu, "True first person",
                "Keep the horizon level and the view steady; the head's heading is still followed.", settings.stabilize, can_edit))
            debug_request(menu, callbacks, {DebugAction::set_first_person_stabilize, settings.stabilize});
        ImGui::BeginDisabled(!settings.stabilize || !can_edit);
        const auto percent = [&](const char* label, const char* id, float value, DebugAction action) {
            field(menu, label);
            if (ImGui::SliderFloat(id, &value, 0, dingosdk::first_person::strength_limit, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
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
        if (toggle_row(menu, "Third person on foot",
                "Use the game's camera while walking; first person comes back when you get on the board.",
                settings.board_only, can_edit))
            debug_request(menu, callbacks, {DebugAction::set_first_person_board_only, settings.board_only});
        note("Nod, tilt and bob are the share of the head's own movement kept. Use Spring arm > Pitch to look "
             "further down at the board.");
    }
    if (ImGui::TreeNode("Spring arm")) {
        namespace fp = dingosdk::first_person;
        auto settings = debug.first_person_arm;
        if (toggle_row(menu, "Spring", "Let the camera lag behind and soften animated head movement.", settings.enabled,
                debug.available && callbacks.queue_debug))
            debug_request(menu, callbacks, {DebugAction::set_first_person_spring, settings.enabled});
        ImGui::BeginDisabled(!debug.available || !callbacks.queue_debug);
        const auto slider = [&](const char* label, float value, float low, float high, const char* format, DebugAction action) {
            field(menu, label);
            ImGui::PushID(label);
            if (ImGui::SliderFloat("##arm-value", &value, low, high, format, ImGuiSliderFlags_AlwaysClamp))
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
        if (ImGui::Button("Reset arm settings", ImVec2(-FLT_MIN, 0)))
            debug_request(menu, callbacks, {DebugAction::reset_first_person_arm});
        ImGui::EndDisabled();
        note("Higher smoothing follows more slowly; 0% follows directly. Offsets work with Spring off. "
             "Depth and roll use the average strength.");
        ImGui::TreePop();
    }
    end_card();
}

void movement_controls(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& debug = model.debug;
    begin_card(menu, "movement", "FLIGHT & BAILS");
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
    bool hall_of_meat = model.hall_of_meat.enabled;
    // No Bail (and noclip, which includes it) stops the bails Hall of Meat scores.
    const bool bails_off = debug.no_bail || debug.no_bail_active;
    const char* meat_help = hall_of_meat && bails_off
        ? "On, but No Bail is stopping your bails, so nothing shows. Turn No Bail (and noclip) off to use it."
        : bails_off
        ? "Your bails show the bones they hurt and score a Meat card. Needs No Bail (and noclip) off."
        : "Your bails show the bones they hurt and score a Meat card. A break slows the game in single player.";
    if (toggle_row(menu, "Hall of Meat", meat_help, hall_of_meat, model.hall_of_meat.available && callbacks.queue_console_command))
        send_console(menu, callbacks, hall_of_meat ? "hallofmeat 1" : "hallofmeat 0");
    bool road_rash = model.road_rash.enabled;
    const bool rash_console = model.road_rash.available && callbacks.queue_console_command;
    if (toggle_row(menu, "Road Rash", "Bails leave marks that build up: dust, scrapes, bruises, wounds. Needs the Road Rash item pack.",
            road_rash, rash_console))
        send_console(menu, callbacks, road_rash ? "roadrash 1" : "roadrash 0");
    bool rash_blood = model.road_rash.blood;
    if (toggle_row(menu, "Road Rash blood", "Fresh blood on the worst wounds. Off, they are dry and scabbed.", rash_blood,
            rash_console && model.road_rash.enabled))
        send_console(menu, callbacks, rash_blood ? "roadrashblood 1" : "roadrashblood 0");
    end_card();

    begin_card(menu, "boosts", "BOOSTS", "Buttons are set in Settings > Controls");
    ImGui::BeginDisabled(!callbacks.queue_debug);
    field(menu, "Forward boost");
    float forward_velocity = debug.forward_velocity_speed;
    if (ImGui::SliderFloat("##forward-velocity", &forward_velocity, 1.0f, 300.0f, "+%.1f", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_forward_velocity_speed, false, forward_velocity});
    field(menu, "Up boost");
    float up_velocity = debug.up_velocity_speed;
    if (ImGui::SliderFloat("##up-velocity", &up_velocity, 1.0f, 25.0f, "+%.1f", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_up_velocity_speed, false, up_velocity});
    field(menu, "Off-board up boost");
    float offboard_up_velocity = debug.offboard_up_velocity_speed;
    if (ImGui::SliderFloat("##offboard-up-velocity", &offboard_up_velocity, 1.0f, 25.0f, "+%.1f", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_offboard_up_velocity_speed, false, offboard_up_velocity});
    ImGui::EndDisabled();
    note("Controller: left stick moves, right stick looks, RT / LT rise and fall, click the left stick to boost.");
    note("Keyboard: WASD / Q E, Shift to boost. Close the menu to fly.");
    end_card();
}

void skater_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.skater_tab, {"CAMERA", "GAMEPLAY"}, "skater-tabs");
    ImGui::PushID(menu.skater_tab);
    ImGui::BeginChild("skater-tab", ImVec2(0, page_body_height(menu)));
    if (menu.skater_tab == 0) camera_controls(menu, model, callbacks);
    else movement_controls(menu, model, callbacks);
    ImGui::EndChild();
    ImGui::PopID();
}
}
