#include "skate_menu_internal.h"
#include "multiplayer_menu_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <vector>

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
    if (toggle_row(menu, "First person", "Attach the camera to the skater's head.", first_person,
            debug.available && debug.camera_available && callbacks.queue_debug))
        debug_request(menu, callbacks, {DebugAction::set_first_person, first_person});
    field(menu, "Field of view");
    int fov = static_cast<int>(std::lround(debug.first_person_fov > 0 ? debug.first_person_fov : debug.camera_fov));
    ImGui::BeginDisabled(!debug.first_person || !callbacks.queue_debug);
    if (ImGui::SliderInt("##first-person-fov", &fov, 40, 120, "%d degrees", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_first_person_fov, false, static_cast<float>(fov)});
    ImGui::EndDisabled();
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
    begin_card(menu, "movement", "MOVEMENT");
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
    if (ImGui::SliderFloat("##forward-velocity", &forward_velocity, 1.0f, 300.0f, "+%.1f", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_forward_velocity_speed, false, forward_velocity});
    field(menu, "Up boost");
    float up_velocity = debug.up_velocity_speed;
    if (ImGui::SliderFloat("##up-velocity", &up_velocity, 1.0f, 25.0f, "+%.1f", ImGuiSliderFlags_AlwaysClamp))
        debug_request(menu, callbacks, {DebugAction::set_up_velocity_speed, false, up_velocity});
    ImGui::EndDisabled();
    note("Controller: left stick moves, right stick looks, RT / LT rise and fall, click the left stick to boost.");
    note("Keyboard: WASD / Q E, Shift to boost. Close the menu to fly.");
    end_card();
}

void wardrobe_colour_controls(SkateMenu& menu, const Model& model) {
    using namespace multiplayer_detail;
    const auto& mp = model.multiplayer;
    if (mp.identity_styles.empty()) {
        note("Wardrobe colours are still loading. Enter a map, then reopen this tab.");
        return;
    }

    note("Pick any colour with the hue/saturation picker or type an exact HEX value. Changes save to your local ReSkate profile.");

    struct Picking {
        std::array<float, 3> from{}, to{};
        bool dirty{};
        int waiting{};
    };
    static std::vector<Picking> picking;
    picking.resize(mp.identity_styles.size());
    std::array<char, 65> unused{};

    const auto same = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::abs(a[0] - b[0]) < .003f && std::abs(a[1] - b[1]) < .003f && std::abs(a[2] - b[2]) < .003f;
    };
    const auto hex = [](const std::array<float, 3>& colour) {
        unsigned value{};
        for (float part : colour)
            value = (value << 8) | static_cast<unsigned>(std::clamp(part, 0.0f, 1.0f) * 255.0f + 0.5f);
        return value;
    };

    for (int item = 0; item < static_cast<int>(mp.identity_styles.size()); ++item) {
        const auto& style = mp.identity_styles[static_cast<std::size_t>(item)];
        auto& picked = picking[static_cast<std::size_t>(item)];

        if (picked.waiting && same(picked.from, style.from) && same(picked.to, style.to)) picked.waiting = 0;
        else if (picked.waiting) --picked.waiting;
        if (!picked.dirty && !picked.waiting) {
            picked.from = style.from;
            picked.to = style.to;
        }

        const auto send = [&](int mode, int speed) {
            send_private(menu, "mark-style",
                         std::format("{} {} {:06x} {:06x} {}", item, mode, hex(picked.from), hex(picked.to), speed),
                         unused, false);
            picked.waiting = 120;
        };

        std::string title = style.name;
        for (auto& letter : title) letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
        ImGui::PushID(item);
        begin_card(menu, title.c_str(), title.c_str());

        // ReSkate modes: 0 original/role default, 1 off, 2 gradient, 3 solid.
        static constexpr std::array<int, 4> modes{0, 3, 2, 1};
        const auto found = std::find(modes.begin(), modes.end(), style.mode);
        int option = found == modes.end() ? 0 : static_cast<int>(found - modes.begin());
        field(menu, "Mode");
        if (choice(menu, "wardrobe-mode", option, {"ORIGINAL", "SOLID", "GRADIENT", "OFF"}))
            send(modes[static_cast<std::size_t>(option)], style.speed);

        const int selectedMode = modes[static_cast<std::size_t>(option)];
        if (style.mode == 2 || style.mode == 3 || selectedMode == 2 || selectedMode == 3) {
            const bool gradient = selectedMode == 2 || (selectedMode == style.mode && style.mode == 2);
            field(menu, gradient ? "Colours" : "Colour",
                  gradient ? "Choose the two colours to blend between." : "Click the swatch for hue/saturation/brightness, or type a HEX value.");

            constexpr auto flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_DisplayHex |
                                   ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_InputRGB;
            if (ImGui::ColorEdit3("##wardrobe-from", picked.from.data(), flags)) picked.dirty = true;
            if (gradient) {
                ImGui::SameLine(0, px(8));
                if (ImGui::ColorEdit3("##wardrobe-to", picked.to.data(), flags)) picked.dirty = true;
            }

            // Save once the user lets go/closes the picker; avoids flooding the request queue.
            if (picked.dirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsItemActive()) {
                picked.dirty = false;
                send(selectedMode, style.speed);
            }
        }

        if (selectedMode == 2) {
            int speed = style.speed;
            field(menu, "Gradient speed");
            if (choice(menu, "wardrobe-speed", speed, {"NORMAL", "SLOW", "FAST"})) send(2, speed);
        }

        end_card();
        ImGui::PopID();
    }
}


void skater_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.skater_tab, {"CAMERA", "MOVEMENT", "COLOURS"}, "skater-tabs");
    ImGui::PushID(menu.skater_tab);
    ImGui::BeginChild("skater-tab", ImVec2(0, page_body_height(menu)));
    if (menu.skater_tab == 0) camera_controls(menu, model, callbacks);
    else if (menu.skater_tab == 1) movement_controls(menu, model, callbacks);
    else wardrobe_colour_controls(menu, model);
    ImGui::EndChild();
    ImGui::PopID();
}
}
