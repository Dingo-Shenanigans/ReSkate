#include "skate_menu_internal.h"
#include "multiplayer_menu_internal.h"
#include "role_badge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

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

namespace {
// The colours the D-pad steps through, where the mouse has the full picker beside them. Sent
// as picked on screen (developer_hoodie_material.h: material_color makes them the material's).
struct Swatch {
    const char* name;
    std::array<std::uint8_t, 3> rgb;
};
constexpr std::array<Swatch, 18> swatches{{
    {"White", {0xff, 0xff, 0xff}}, {"Light grey", {0xc0, 0xc0, 0xc0}}, {"Grey", {0x80, 0x80, 0x80}},
    {"Charcoal", {0x40, 0x40, 0x40}}, {"Black", {0x10, 0x10, 0x10}}, {"Red", {0xe0, 0x20, 0x20}},
    {"Orange", {0xff, 0x80, 0x10}}, {"Yellow", {0xff, 0xe0, 0x30}}, {"Lime", {0x9c, 0xe0, 0x30}},
    {"Green", {0x20, 0xa0, 0x40}}, {"Teal", {0x20, 0xb0, 0xa0}}, {"Sky blue", {0x50, 0xb8, 0xf0}},
    {"Blue", {0x20, 0x50, 0xe0}}, {"Navy", {0x18, 0x24, 0x60}}, {"Purple", {0x80, 0x40, 0xe0}},
    {"Pink", {0xff, 0x60, 0xb0}}, {"Brown", {0x80, 0x50, 0x20}}, {"Tan", {0xd2, 0xb4, 0x8c}}}};
// One colour as a row whose left and right step through the swatches; "Custom" while it is one
// the mouse picked. True when a swatch was stepped to, into `colour`.
bool swatch_row(SkateMenu& menu, const char* label, std::array<float, 3>& colour, bool enabled) {
    std::vector<std::string> names;
    int selected = -1;
    for (std::size_t i = 0; i < swatches.size(); ++i) {
        names.emplace_back(swatches[i].name);
        bool same = true;
        for (std::size_t part = 0; part < 3; ++part)
            same = same && std::abs(colour[part] * 255.0f - static_cast<float>(swatches[i].rgb[part])) < 1.0f;
        if (same) selected = static_cast<int>(i);
    }
    if (selected < 0) {
        names.emplace_back("Custom");
        selected = static_cast<int>(swatches.size());
    }
    if (!option_row(menu, label, "Left and right step through the colors. With the mouse, the swatch below picks any color.",
                    selected, names, enabled) ||
        selected < 0 || selected >= static_cast<int>(swatches.size()))
        return false;
    for (std::size_t part = 0; part < 3; ++part)
        colour[part] = static_cast<float>(swatches[static_cast<std::size_t>(selected)].rgb[part]) / 255.0f;
    return true;
}

// Skater > STYLE: how this player looks to everyone they skate with. A player on one of the
// backend's lists has their tag, and on several picks which one shows; then everyone colours
// what they wear, item by item, with what is theirs to pick (style_offered): off and a solid
// colour for anyone; their list's own colours, a gradient and the board for a listed player;
// the rainbow for the staff. It goes out with their appearance (session_send.cpp) and is kept
// in their profile.
void style_tab(SkateMenu& menu, const Model& model, const CallbacksV3&) {
    const auto& mp = model.multiplayer;
    std::array<char, 65> unused{};
    const auto send = [&](const char* action, const std::string& argument) {
        multiplayer_detail::send_private(menu, action, argument, unused, false);
    };
    const bool listed = !mp.identity_animation.empty();
    if (!mp.identity_tag.empty()) {
        begin_card(menu, "style-tag", "TAG");
        {
            // Drawn the way chat and nametags draw it (role_badge.h), and faint while it is off.
            field(menu, "Looks like", "Your tag and name, as chat and your nametag show them.");
            const std::string name = mp.local_name.empty() ? std::string("You") : mp.local_name;
            const float size = px(18), height = ImGui::GetFrameHeight(), alpha = mp.identity_tag_shown ? 1.0f : .3f;
            auto* draw = ImGui::GetWindowDrawList();
            const auto at = ImGui::GetCursorScreenPos();
            const float badge = overlay::detail::role_badge_width(menu.bold, size, mp.identity_tag) + px(6);
            overlay::detail::draw_role_badge(draw, menu.bold, size, at, height, mp.identity_tag, mp.identity_tag_colour, alpha);
            const auto extent = menu.bold->CalcTextSizeA(size, FLT_MAX, 0, name.c_str());
            const ImVec2 name_at(at.x + badge, at.y + (height - extent.y) * .5f);
            const int name_vertices = draw->VtxBuffer.Size;
            draw->PushClipRect(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + height), true);
            draw->AddText(menu.bold, size, name_at,
                          (mp.identity_tag_colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(255.0f * alpha) << IM_COL32_A_SHIFT),
                          name.c_str());
            draw->PopClipRect();
            overlay::detail::shade_nametag_gradient(draw, name_vertices, name_at.x, extent.x, mp.identity_tag_colour, ImGui::GetTime());
            ImGui::Dummy(ImVec2(std::min(badge + extent.x, ImGui::GetContentRegionAvail().x), height));
        }
        bool tag = mp.identity_tag_shown;
        if (toggle_row(menu, "Show my tag",
                       "Your tag and the color of your name, in chat and on your nametag. "
                       "Off: your name looks like any other player's, to yourself and to everyone you skate with.",
                       tag))
            send("mark-tag", tag ? "on" : "off");
        // Only the lists they are on. Their items keep what their first list gives, whichever shows.
        if (mp.identity_tag_lists.size() > 1) {
            std::vector<std::string> names;
            int selected = -1;
            for (std::size_t i = 0; i < mp.identity_tag_lists.size(); ++i) {
                names.push_back(mp.identity_tag_lists[i].tag);
                if (mp.identity_tag_lists[i].list == mp.identity_tag_list) selected = static_cast<int>(i);
            }
            if (option_row(menu, "Tag", "Which of your tags shows. Your items keep the colors they have.", selected, names,
                           mp.identity_tag_shown) &&
                selected >= 0)
                send("mark-tag-list", mp.identity_tag_lists[static_cast<std::size_t>(selected)].list);
        }
        end_card();
    }

    begin_card(menu, "style-items", "ITEM COLORS");
    bool shown = mp.identity_items_shown;
    if (toggle_row(menu, "Color my items",
                   "The colors you pick below, on whatever you wear. "
                   "Off: it all keeps its own colors, for you and for everyone you skate with.",
                   shown))
        send("mark-items", shown ? "on" : "off");
    note(listed ? "Each one on its own: it colors whatever you have on in that slot, and everyone you skate with sees it."
                : "Give anything you wear a solid color of your own: it colors whatever you have on in that slot, and everyone "
                  "you skate with on this version of ReSkate or newer sees it.");
    end_card();

    // The colours being picked: held here while the mouse has them, and after it lets go until
    // the saved ones catch up, so a swatch never jumps back to the colour it had.
    struct Picking {
        std::array<float, 3> from{}, to{};
        bool changed{};
        int waiting{}; // frames
    };
    static std::vector<Picking> picking;
    picking.resize(mp.identity_styles.size());
    // The options in the order shown, as the mark-style command's modes: 0 what the list gives,
    // 1 off, 2 a gradient between the two colours, 3 the one colour, 4 the rainbow (the staff's).
    std::vector<int> modes{1, 3};
    std::vector<std::string> names{"OFF", "SOLID"};
    if (listed) {
        modes = {0, 2, 3, 1}, names = {mp.identity_animation, "GRADIENT", "SOLID", "OFF"};
        if (mp.identity_rainbow) modes.insert(modes.begin() + 1, 4), names.insert(names.begin() + 1, "RAINBOW");
    }
    for (int item = 0; item < static_cast<int>(mp.identity_styles.size()); ++item) {
        const auto& style = mp.identity_styles[static_cast<std::size_t>(item)];
        auto& picked = picking[static_cast<std::size_t>(item)];
        const auto same = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            return std::abs(a[0] - b[0]) < .003f && std::abs(a[1] - b[1]) < .003f && std::abs(a[2] - b[2]) < .003f;
        };
        if (picked.waiting && same(picked.from, style.from) && same(picked.to, style.to)) picked.waiting = 0;
        else if (picked.waiting) --picked.waiting;
        if (!picked.changed && !picked.waiting) picked.from = style.from, picked.to = style.to;
        const auto set = [&](int mode, int speed) {
            const auto hex = [](const std::array<float, 3>& colour) {
                unsigned value{};
                for (const float part : colour) value = value << 8 | static_cast<unsigned>(std::clamp(part, 0.0f, 1.0f) * 255.0f + 0.5f);
                return value;
            };
            send("mark-style", std::format("{} {} {:06x} {:06x} {}", item, mode, hex(picked.from), hex(picked.to), speed));
        };
        // The card is headed by the cosmetic: whatever is in that slot is what gets coloured.
        std::string title = style.name;
        for (auto& letter : title) letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
        ImGui::PushID(item);
        begin_card(menu, title.c_str(), title.c_str());
        int option = static_cast<int>(std::find(modes.begin(), modes.end(), style.mode) - modes.begin());
        if (option >= static_cast<int>(modes.size())) option = -1;
        if (option_row(menu, "Color", listed ? "What colors it: your tag's own colors, colors you pick, or none."
                                             : "A color you pick, or its own.",
                       option, names, shown) &&
            option >= 0)
            set(modes[static_cast<std::size_t>(option)], style.speed);
        if (style.mode == 2 || style.mode == 3) {
            const bool gradient = style.mode == 2;
            // The D-pad steps through the swatches; the mouse has the full picker under them.
            if (swatch_row(menu, gradient ? "First color###from" : "Pick###from", picked.from, shown)) {
                picked.waiting = 120;
                set(style.mode, style.speed);
            }
            if (gradient && swatch_row(menu, "Second color###to", picked.to, shown)) {
                picked.waiting = 120;
                set(style.mode, style.speed);
            }
            field(menu, gradient ? "Any colors" : "Any color",
                  gradient ? "Click either end to pick any color for it." : "Click the swatch to pick any color.");
            ImGui::BeginDisabled(!shown);
            constexpr auto flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha;
            if (ImGui::ColorEdit3("##from", picked.from.data(), flags)) picked.changed = true;
            // Beside the swatch, what it gives: the one colour, or all that lies between the two.
            ImGui::SameLine(0, px(6));
            const auto at = ImGui::GetCursorScreenPos();
            const float swatch = gradient ? ImGui::GetFrameHeight() + px(6) : 0.0f;
            const ImVec2 bar(std::max(px(20), ImGui::GetContentRegionAvail().x - swatch), ImGui::GetFrameHeight());
            const auto solid = [&](const std::array<float, 3>& colour) {
                return ImGui::ColorConvertFloat4ToU32(ImVec4(colour[0], colour[1], colour[2], shown ? 1.0f : .4f));
            };
            const auto left = solid(picked.from), right = solid(gradient ? picked.to : picked.from);
            ImGui::GetWindowDrawList()->AddRectFilledMultiColor(at, ImVec2(at.x + bar.x, at.y + bar.y), left, right, right, left);
            ImGui::Dummy(bar);
            if (gradient) {
                ImGui::SameLine(0, px(6));
                if (ImGui::ColorEdit3("##to", picked.to.data(), flags)) picked.changed = true;
            }
            ImGui::EndDisabled();
            // One save when the mouse lets go, not one for every shade dragged through.
            if (picked.changed && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                picked.changed = false;
                picked.waiting = 120;
                set(style.mode, style.speed);
            }
        }
        // A colour that stands still has no speed.
        if (style.mode == 0 || style.mode == 2 || style.mode == 4) {
            int speed = style.speed;
            if (option_row(menu, "Speed", "How fast it moves through its colors.", speed, {"NORMAL", "SLOW", "FAST"}, shown))
                set(style.mode, speed);
        }
        end_card();
        ImGui::PopID();
    }
}
} // namespace

void skater_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.skater_tab, {"FREECAM", "FIRST PERSON", "CHEATS", "STYLE"}, "skater-tabs");
    ImGui::PushID(menu.skater_tab);
    ImGui::BeginChild("skater-tab", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    switch (menu.skater_tab) {
    case 0: camera_controls(menu, model, callbacks); break;
    case 1: first_person_controls(menu, model, callbacks); break;
    case 2: cheats_tab(menu, model, callbacks); break;
    default: style_tab(menu, model, callbacks); break;
    }
    ImGui::EndChild();
    ImGui::PopID();
}
}
