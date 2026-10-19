#include "Extension/Skater/FirstPerson/integration.h"
#include "skate_menu_internal.h"

#include <algorithm>
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
    first_person_integration::observe_bindings(model.bindings);
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
        if (::dingosdk::overlay::menu::slider_int("##free-camera-fov", &free_fov, 40, 120, custom ? "%d degrees" : "%d degrees (game)",
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
    if (::dingosdk::overlay::menu::slider_int("##flight-speed", &selected, 0, 6, labels[selected], ImGuiSliderFlags_NoInput))
        debug_request(menu, callbacks, {DebugAction::set_camera_speed, false, speeds[selected]});
    ImGui::EndDisabled();
    if (debug.free_camera) note("Close the menu to fly. WASD / Q E move; hold the right mouse button to look.");
    if (!debug.camera_available) warn(debug.camera_unavailable.c_str());
    end_card();

    // Keyframe smoothing (Extension/ReplayKeyframes/replay_keyframes.h).
    begin_card(menu, "replay-editor", "REPLAY EDITOR");
    bool keysmooth = model.keyframe_smoothing.enabled;
    if (toggle_row(menu, "Keyframe smoothing",
            "The free camera glides through its keys instead of changing speed at each one. Every key is still met, at its time.",
            keysmooth, model.keyframe_smoothing.available && callbacks.queue_console_command))
        send_console(menu, callbacks, keysmooth ? "keysmooth on" : "keysmooth off");
    field(menu, "Smoothness", "100% is the smoothest that still meets every key; 0% the game's own curve.");
    // Sent once it is let go: every change rebuilds the open replay's camera.
    static int dragged = -1;
    int smoothness = dragged >= 0 ? dragged : keyframe_smoothness_percent(model.keyframe_smoothing);
    ImGui::BeginDisabled(!model.keyframe_smoothing.available || !model.keyframe_smoothing.enabled || !callbacks.queue_console_command);
    if (::dingosdk::overlay::menu::slider_int("##keyframe-smoothness", &smoothness, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp))
        dragged = smoothness;
    if (ImGui::IsItemDeactivatedAfterEdit() && dragged >= 0)
        send_console(menu, callbacks, "keysmooth amount " + std::to_string(dragged / 100.0));
    if (!ImGui::IsItemActive()) dragged = -1;
    ImGui::EndDisabled();
    end_card();
}

void first_person_controls(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& debug = model.debug;
    begin_card(menu, "first-person", "FIRST PERSON");
    bool first_person = debug.first_person;
    if (toggle_row(menu, "First person", "See the world through the skater's eyes.", first_person,
            debug.available && debug.camera_available && callbacks.queue_debug))
        debug_request(menu, callbacks, {DebugAction::set_first_person, first_person});
    first_person_main_controls(menu, model, callbacks);
    if(ImGui::CollapsingHeader("Advanced")) {
        if(ImGui::TreeNodeEx("View and stabilization",ImGuiTreeNodeFlags_SpanAvailWidth)) {
    first_person_view_controls(menu);
    field(menu, "Field of view");
    const bool custom_fov = debug.first_person_fov > 0;
    int fov = static_cast<int>(std::lround(custom_fov ? debug.first_person_fov : debug.camera_fov));
    const float fov_reset_width = trailing_width("Reset");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fov_reset_width - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(!debug.first_person || !callbacks.queue_debug);
    if (::dingosdk::overlay::menu::slider_int("##first-person-fov", &fov, 40, 120, "%d degrees", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_first_person_fov, false, static_cast<float>(fov)});
    ImGui::SameLine();
    ImGui::BeginDisabled(!custom_fov);
    if (ImGui::Button("Reset", ImVec2(fov_reset_width, 0)))
        debug_request(menu, callbacks, {DebugAction::set_first_person_fov, false, 0.0f});
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    {
        auto settings = debug.first_person_arm;
        const bool can_edit = debug.available && callbacks.queue_debug;
        if (toggle_row(menu, "True first person",
                "Keep the horizon level and the view steady; the head's heading is still followed.", settings.stabilize, can_edit))
            debug_request(menu, callbacks, {DebugAction::set_first_person_stabilize, settings.stabilize});
        ImGui::BeginDisabled(!settings.stabilize || !can_edit);
        const auto percent = [&](const char* label, const char* id, float value, DebugAction action,float reset) {
            field(menu, label);
            if (first_person_slider(id,value,0,dingosdk::first_person::strength_limit,"%.0f%%",reset))

                debug_request(menu, callbacks, {action, false, value});
        };
        percent("Smoothing", "##fp-smoothing", settings.smoothing, DebugAction::set_first_person_smoothing,dingosdk::first_person::Settings{}.smoothing);
        percent("Head nod (up / down)", "##fp-head-pitch", settings.head_pitch, DebugAction::set_first_person_head_pitch,dingosdk::first_person::Settings{}.head_pitch);
        percent("Head tilt (horizon roll)", "##fp-head-roll", settings.head_roll, DebugAction::set_first_person_head_roll,dingosdk::first_person::Settings{}.head_roll);
        percent("Head bob", "##fp-bob", settings.bob, DebugAction::set_first_person_bob,dingosdk::first_person::Settings{}.bob);
        if (toggle_row(menu, "Follow flips", "Turn the view with the skater during flips and bails.", settings.follow_flips,
                can_edit && settings.stabilize))
            debug_request(menu, callbacks, {DebugAction::set_first_person_follow_flips, settings.follow_flips});
        ImGui::EndDisabled();
        note("Nod, tilt and bob are the share of the head's own movement kept. Use Spring arm > Pitch to look "
             "further down at the board.");
    }
        ImGui::TreePop();
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
            if (first_person_slider("arm-value",value,low,high,format,
                action>=DebugAction::set_first_person_spring_up && action<=DebugAction::set_first_person_spring_right ? 50.f:0.f))

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
        first_person_foot_spring_control(menu);
        ImGui::TreePop();
    }
        first_person_stick_controls(menu);
        if(ImGui::TreeNodeEx("Directional glances",ImGuiTreeNodeFlags_SpanAvailWidth)) {
            first_person_glance_controls(menu);ImGui::TreePop();
        }
        if(ImGui::TreeNodeEx("Automatic camera switching",ImGuiTreeNodeFlags_SpanAvailWidth)) {
    {
        auto settings = debug.first_person_arm;
        const bool can_edit = debug.available && callbacks.queue_debug;
        if (toggle_row(menu, "Third person on foot",
                "Use the game's camera while walking; first person comes back when you get on the board.",
                settings.board_only, can_edit))
            debug_request(menu, callbacks, {DebugAction::set_first_person_board_only, settings.board_only});
    }
        {
            auto settings=debug.first_person_arm;
            const bool can_edit=debug.available && callbacks.queue_debug;
        if (toggle_row(menu, "First person in the camera cycle",
                "Clicking the right stick (R3) on the board goes high camera, low camera, first person, then high again. "
                "Needs Camera height control on in the game's settings (Gameplay > Advanced Camera).",
                settings.camera_cycle, can_edit))
            debug_request(menu, callbacks, {DebugAction::set_first_person_camera_cycle, settings.camera_cycle});
        }
        ImGui::TreePop();
        }
    }
    end_card();
}

// Board Break's settings (Extension/BoardBreak/board_break_feel.h): how a board breaks, wears and
// sounds. Each is a console verb, kept with the profile.
void board_break_settings_card(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& bb = model.board_break;
    if (!bb.available) return;
    const bool console = bb.enabled && callbacks.queue_console_command;
    std::string subtitle = !bb.has_deck ? "No Board Break deck is in the game"
                                        : "Your board: " + std::to_string(static_cast<int>(bb.worn * 100.0f + 0.5f)) + "% worn (" + bb.condition + ")";
    begin_card(menu, "board-break-settings", "BOARD BREAK SETTINGS", subtitle.c_str());
    if (!bb.enabled) note("Switch Board Break on to change these.");
    const auto flag = [&](const char* label, const char* hint, bool on, const char* verb) {
        bool value = on;
        if (toggle_row(menu, label, hint, value, console)) send_console(menu, callbacks, std::string("boardbreak ") + verb + (value ? " 1" : " 0"));
    };
    flag("Always break", "Every slam breaks the board, and every slide that comes down onto a rail: for replay clips.", bb.always, "always");
    flag("Rail snaps", "A boardslide or darkslide that comes down too hard onto a rail or ledge can snap the board, and you go down with it.",
         bb.rails, "rails");
    flag("Wear and tear", "Every hit wears your board. A worn board turns lazier, wobbles and breaks more easily; worn out, it can give way. "
         "Turn on Board wear (Mod Options) to see the scratches.", bb.wear, "wear");
    flag("Break sounds", "The crack of a break, and metal when it breaks on metal.", bb.crack_sounds, "cracks");
    flag("Body hit sounds", "Each hit of your body in a bail, as hard as it hits.", bb.body_sounds, "bodysounds");
    // Sent once a slider is let go; the dragged value shows until the setting comes back.
    const auto percent = [&](const char* id, const char* label, const char* hint, int value, int low, int high, int step, const char* verb) {
        static std::array<int, 3> edited{-1, -1, -1};
        static std::array<double, 3> edited_at{};
        const auto slot = std::string_view(id) == "bb-sensitivity" ? 0u : std::string_view(id) == "bb-joined" ? 1u : 2u;
        int shown = edited[slot] >= 0 ? edited[slot] : value;
        field(menu, label, hint);
        ImGui::BeginDisabled(!console);
        if (::dingosdk::overlay::menu::slider_int((std::string("##") + id).c_str(), &shown, low, high, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
            edited[slot] = std::clamp((shown + step / 2) / step * step, low, high);
            edited_at[slot] = ImGui::GetTime();
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && edited[slot] >= 0) {
            send_console(menu, callbacks, std::string("boardbreak ") + verb + " " + std::to_string(edited[slot]));
            edited_at[slot] = ImGui::GetTime();
        }
        if (edited[slot] >= 0 && !ImGui::IsItemActive() && (value == edited[slot] || ImGui::GetTime() - edited_at[slot] > 2.0)) edited[slot] = -1;
        ImGui::EndDisabled();
    };
    percent("bb-sensitivity", "Break sensitivity", "How easily a board breaks. 200% takes half the force, 50% twice as much.", bb.sensitivity, 10, 400, 10,
            "sensitivity");
    percent("bb-joined", "Grip tape holds", "How many breaks the grip tape holds together instead of a piece coming off.", bb.joined, 0, 100, 5, "joined");
    percent("bb-wear-rate", "Wear speed", "How fast boards wear. 0% keeps a board as it is.", bb.wear_rate, 0, 500, 10, "wearrate");
    if (console_style()) {
        if (button_row(menu, "bb-new", {{"NEW BOARD", console && bb.has_deck, false, "Your board as good as new: no wear."}}) == 0)
            send_console(menu, callbacks, "boardbreak worn 0");
    } else {
        ImGui::BeginDisabled(!console || !bb.has_deck);
        if (ImGui::Button("New board", ImVec2(-FLT_MIN, 0))) send_console(menu, callbacks, "boardbreak worn 0");
        ImGui::EndDisabled();
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
    end_card();

    begin_card(menu, "boosts", "BOOSTS", "Buttons are set in Settings > Controls");
    ImGui::BeginDisabled(!callbacks.queue_debug);
    field(menu, "Forward boost");
    float forward_velocity = debug.forward_velocity_speed;
    if (slider_float("##forward-velocity", &forward_velocity, 1.0f, 300.0f, "+%.1f", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_forward_velocity_speed, false, forward_velocity});
    field(menu, "Up boost");
    float up_velocity = debug.up_velocity_speed;
    if (slider_float("##up-velocity", &up_velocity, 1.0f, 25.0f, "+%.1f", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_up_velocity_speed, false, up_velocity});
    field(menu, "Off-board up boost");
    float offboard_up_velocity = debug.offboard_up_velocity_speed;
    if (slider_float("##offboard-up-velocity", &offboard_up_velocity, 1.0f, 25.0f, "+%.1f", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_offboard_up_velocity_speed, false, offboard_up_velocity});
    ImGui::EndDisabled();
    note("Controller: left stick moves, right stick looks, RT / LT rise and fall, click the left stick to boost.");
    note("Keyboard: WASD / Q E, Shift to boost. Close the menu to fly.");
    end_card();
}

// Addons > ROAD RASH: bails leave marks.
void road_rash_addon(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "road-rash", "ROAD RASH");
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
}

// Addons > HALL OF MEAT: bails show the bones they hurt and score a Meat card.
void hall_of_meat_addon(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& debug = model.debug;
    begin_card(menu, "hall-of-meat", "HALL OF MEAT");
    bool hall_of_meat = model.hall_of_meat.enabled;
    // No Bail (and noclip, which includes it) stops the bails Hall of Meat scores.
    const bool bails_off = debug.no_bail || debug.no_bail_active;
    const char* meat_help = hall_of_meat && bails_off
        ? "On, but No Bail is stopping your bails, so nothing shows. Turn No Bail (and noclip) off in Skater > Cheats."
        : bails_off
        ? "Your bails show the bones they hurt and score a Meat card. Needs No Bail (and noclip) off."
        : "Your bails show the bones they hurt and score a Meat card. A break slows the game in single player.";
    if (toggle_row(menu, "Hall of Meat", meat_help, hall_of_meat, model.hall_of_meat.available && callbacks.queue_console_command))
        send_console(menu, callbacks, hall_of_meat ? "hallofmeat 1" : "hallofmeat 0");
    end_card();
}

// Addons > BOARD BREAK: the switch, the deck in the game, and how boards break, wear and sound.
void board_break_addon(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "board-break", "BOARD BREAK");
    bool board_break = model.board_break.enabled;
    if (toggle_row(menu, "Board Break", model.board_break.has_deck
                ? "A hard slam can snap your board: at the nose, in the middle or at the tail. Whole again once you pick it up."
                : "A hard slam can snap your board. Needs one of the Board Break deck packs.",
            board_break, model.board_break.available && callbacks.queue_console_command))
        send_console(menu, callbacks, board_break ? "boardbreak 1" : "boardbreak 0");
    end_card();
    board_break_deck_card(menu, model, callbacks);
    board_break_settings_card(menu, model, callbacks);
}

// Addons > GRIND EXITS: grind exit control (Extension/Skater/grind_exit_control.h).
void grind_exits_addon(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& grind = model.grind_exit;
    const bool grind_console = grind.available && callbacks.queue_console_command;
    begin_card(menu, "grind-exits", "GRIND EXITS", "Choose the side you pop out of a grind using LT/RT");
    bool grind_on = grind.enabled;
    if (toggle_row(menu, "Grind exit triggers", "Press LT or RT while grinding to pick the side you pop out to.",
            grind_on, grind_console))
        send_console(menu, callbacks, grind_on ? "grindexit exits on" : "grindexit exits off");
    field(menu, "Input Mode");
    int grind_sides = grind.toe_heel ? 0 : 1;
    if (choice(menu, "grind-sides", grind_sides, {"Toe / Heel", "Left / Right"}, grind_console, 0.0f,
            {"Direction is based on your skater's toe/heel side", "Direction is based on left or right of your direction of travel"}))
        send_console(menu, callbacks, grind_sides == 0 ? "grindexit sides toeheel" : "grindexit sides travel");
    if (grind.toe_heel) {
        field(menu, "Your stance");
        int grind_stance = grind.stance == 2 ? 1 : grind.stance == 1 ? 0 : -1;
        if (choice(menu, "grind-stance", grind_stance, {"Regular", "Goofy"}, grind_console))
            send_console(menu, callbacks, grind_stance == 1 ? "grindexit stance goofy" : "grindexit stance regular");
        if (grind.stance == 0) warn("Pick your stance. Until then LT / RT are left / right of travel.");
    }
    {
        // Sent once the slider is let go (not every frame of a drag); the dragged value shows
        // until the setting comes back, in steps of 5.
        static int edited = -1;
        static double edited_at = 0;
        int strength = edited >= 0 ? edited : grind.strength;
        field(menu, "Pop-out strength", "How far to the side a trigger exit pops. 100% is the full exit; lower it for rails next to a wall.");
        ImGui::BeginDisabled(!grind_console);
        if (slider_int("##grind-strength", &strength, 10, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
            edited = std::clamp((strength + 2) / 5 * 5, 10, 100);
            edited_at = ImGui::GetTime();
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && edited >= 0) {
            send_console(menu, callbacks, "grindexit strength " + std::to_string(edited));
            edited_at = ImGui::GetTime();
        }
        if (edited >= 0 && !ImGui::IsItemActive() && (grind.strength == edited || ImGui::GetTime() - edited_at > 2.0)) edited = -1;
        ImGui::EndDisabled();
    }
    end_card();
}

void skater_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    // (Grind exits are an addon: Addons > Grind Exits.)
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
