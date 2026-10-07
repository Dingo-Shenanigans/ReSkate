#include "skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <optional>
#include <utility>

// Style editing: the style editor screen with a keyframe timeline for each flip trick, and the STYLE page.
namespace dingosdk::overlay {
namespace {
using namespace menu;
using style::Target;
constexpr ImU32 track_colour = IM_COL32(38, 40, 46, 255), track_alternate = IM_COL32(48, 51, 58, 255),
                line_colour = IM_COL32(110, 114, 124, 255), key_colour = IM_COL32(236, 232, 220, 255),
                selected_colour = IM_COL32(70, 150, 255, 255), playhead_colour = IM_COL32(255, 196, 64, 255),
                tail_colour = IM_COL32(236, 232, 220, 80), default_tail_colour = IM_COL32(236, 232, 220, 36),
                cut_tail_colour = IM_COL32(255, 128, 72, 120);
std::atomic<StylePlayheadFeed> playhead_feed{};
// Queues a command without a reply line, because sliders and drags send many commands.
void quiet(const CallbacksV3& callbacks, const std::string& command) {
    if (!callbacks.queue_console_command) return;
    std::array<char, 512> result{};
    callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
}
std::array<float, 3> saved(const Model& model, Target target, int joint) {
    for (const auto& rotation : model.style.rotations)
        if (rotation.target == target && rotation.joint == joint) return rotation.degrees;
    return {};
}
// The edit state of this frame.
struct Editing {
    SkateMenu& menu;
    const Model& model;
    const CallbacksV3& callbacks;
    style::Playhead replay;
    std::uint8_t trick_id{};
    std::string trick;
    std::vector<float> times;
    std::vector<float> blend_outs; // ms for each keyframe. 0: the keyframe blends to the next keyframe
    style::Pace pace;              // the shown clip's
    double now{};
    bool replaying{}, standing_in{};
    // Moves the playhead, and the stand-in when it shows this trick. send_controls sends the move.
    void hold(float time) const {
        menu.styling.time = time;
        if (standing_in) menu.styling.hold = time;
    }
    [[nodiscard]] float playhead() const { return replaying ? replay.time : menu.styling.time; }
};
Editing begin_editing(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    // The timeline follows the clip on the stand-in.
    const auto feed = playhead_feed.load();
    Editing e{menu, model, callbacks, feed ? feed() : style::Playhead{}};
    menu.styling.trick = std::clamp(menu.styling.trick, 1, static_cast<int>(style::flip_trick_names.size()) - 1);
    e.trick_id = static_cast<std::uint8_t>(menu.styling.trick);
    e.trick = style::flip_trick_names[e.trick_id];
    const auto found = model.style.times.find(e.trick_id);
    e.times = found != model.style.times.end() ? found->second : std::vector<float>{};
    if (menu.styling.pending_key >= 0 && ImGui::GetTime() < menu.styling.pending_until) {
        if (static_cast<int>(e.times.size()) > menu.styling.pending_key) menu.styling.key = std::exchange(menu.styling.pending_key, -1);
    } else menu.styling.pending_key = -1;
    menu.styling.key = e.times.empty() ? -1 : std::clamp(menu.styling.key, 0, static_cast<int>(e.times.size()) - 1);
    e.now = ImGui::GetTime();
    e.replaying = e.replay.trick == e.trick_id;
    e.standing_in = e.replaying && e.replay.editor;
    // Once: the model shows the preview until the game has stopped it.
    if (e.replay.editor && model.style.preview && e.now >= menu.styling.preview_off_sent + 0.5) {
        menu.styling.preview_off_sent = e.now;
        quiet(callbacks, "style preview off");
    }
    // A dragged keyframe shows at the mouse position until the game confirms the move.
    if (menu.styling.drag_key >= 0 && menu.styling.drag_key < static_cast<int>(e.times.size()) && e.now < menu.styling.drag_until)
        e.times[static_cast<std::size_t>(menu.styling.drag_key)] = menu.styling.drag_time;
    e.blend_outs.assign(e.times.size(), 0.0f);
    for (const auto& [target, ms] : model.style.blend_outs)
        if (target.trick && target.id == e.trick_id && target.key < e.blend_outs.size()) e.blend_outs[target.key] = ms;
    // A changed blend out shows its own value until the game confirms it.
    if (menu.styling.blend_key >= 0 && menu.styling.blend_key < static_cast<int>(e.times.size()) && e.now < menu.styling.blend_until)
        e.blend_outs[static_cast<std::size_t>(menu.styling.blend_key)] = menu.styling.blend_ms;
    e.pace = model.style.pace;
    if (e.replaying) menu.styling.time = e.replay.time;
    return e;
}
// Sends the newest playhead move and the camera turn gathered since the last send.
// A drag changes them every frame, so they go at most every 40 ms: the game runs one command for each tick.
void send_controls(Editing& e, bool now) {
    auto& s = e.menu.styling;
    if (!now && e.now < s.controls_sent + 0.04) return;
    bool sent{};
    if (const auto time = std::exchange(s.hold, std::nullopt)) {
        quiet(e.callbacks, std::format("style editor hold {:.4f}", std::clamp(*time, 0.0f, style::trick_end)));
        sent = true;
    }
    if (s.orbit != std::array<float, 4>{}) {
        const auto [yaw, pitch, distance, height] = s.orbit;
        const auto limit = [](float value) { return std::clamp(value, -20.0f, 20.0f); };
        quiet(e.callbacks, std::format("style editor orbit {:.4f} {:.4f} {:.4f} {:.4f}", limit(yaw), limit(pitch), limit(distance), limit(height)));
        s.orbit = {};
        sent = true;
    }
    if (sent) s.controls_sent = e.now;
}
// Sends a playback command after the moves before it, so the game runs them in order.
void send_now(Editing& e, const std::string& command) {
    send_controls(e, true);
    quiet(e.callbacks, command);
}
// Where a keyframe's blend out ends on the timeline.
struct BlendOut {
    float ms{}, end{};
    float room_ms{};           // the time to the next keyframe or the trick's end
    bool last{}, set{}, cut{}; // set: the keyframe has its own time. cut: the next keyframe or the trick's end comes first
};
std::optional<BlendOut> blend_out(const Editing& e, std::size_t key) {
    const float at = e.times[key];
    float next = style::trick_end;
    for (const auto time : e.times)
        if (time > at && time < next) next = time;
    const bool last = next == style::trick_end, set = e.blend_outs[key] > 0;
    const float ms = set ? e.blend_outs[key] : last ? style::release_ms : 0.0f;
    if (ms <= 0 || at >= style::trick_end) return std::nullopt;
    const float from = style::paced(at, e.pace), room = style::paced(next, e.pace) - from;
    return BlendOut{ms, style::timeline_at(from + std::min(ms, room), e.pace), room, last, set, ms >= room - 1e-3f};
}
// Shows a keyframe's blend out at once and sends it, but not faster than the game accepts commands.
void send_blend_out(Editing& e, std::size_t key, float ms, bool final) {
    auto& menu = e.menu;
    menu.styling.blend_key = static_cast<int>(key);
    menu.styling.blend_ms = ms;
    menu.styling.blend_until = e.now + 0.75;
    if (!final && e.now < menu.styling.blend_sent + 0.08) return;
    menu.styling.blend_sent = e.now;
    quiet(e.callbacks, std::format("style key blendout {} {} {}", e.trick, key, ms > 0 ? std::format("{:.0f}", ms) : "off"));
}
void trick_picker(Editing& e) {
    auto& menu = e.menu;
    // Tricks 16 to 30 and 32 are the nollie versions of 1 to 15 and 31.
    constexpr int listed = static_cast<int>(style::flip_trick_titles.size());
    const auto nollie = [](int trick) { return (trick >= 16 && trick <= 30) || trick == 32; };
    const auto preview = std::format("Flip tricks  /  {}", style::flip_trick_titles[e.trick_id]);
    if (!ImGui::BeginCombo("##style-trick", preview.c_str(), ImGuiComboFlags_HeightLargest)) return;
    const auto soon = [](const char* label) {
        ImGui::BeginDisabled();
        ImGui::Selectable(label, false);
        ImGui::EndDisabled();
    };
    const auto group = [&](const char* title, bool wanted) {
        ImGui::TextDisabled("%s", title);
        ImGui::Indent();
        for (int i = 1; i < listed; ++i) {
            if (nollie(i) != wanted) continue;
            const bool edited = std::ranges::any_of(e.model.style.rotations, [&](const auto& r) { return r.target.trick && r.target.id == i; });
            const auto label = std::format("{}{}", style::flip_trick_titles[i], edited ? "  *" : "");
            if (ImGui::Selectable(label.c_str(), i == menu.styling.trick) && i != menu.styling.trick) {
                menu.styling.trick = i;
                menu.styling.key = 0;
                // The stand-in follows the selected trick.
                send_console(menu, e.callbacks, std::format("style editor show {}", style::flip_trick_names[i]));
            }
        }
        ImGui::Unindent();
    };
    ImGui::SeparatorText("FLIP TRICKS");
    group("Regular", false);
    group("Nollie", true);
    soon("Switch  (coming soon)");
    soon("Fakie  (coming soon)");
    ImGui::SeparatorText("MORE");
    soon("Grinds  (coming soon)");
    soon("Grabs  (coming soon)");
    soon("Manuals  (coming soon)");
    ImGui::EndCombo();
}
// The bar: the flick at the left edge, lines at the catch and the touchdown, a diamond for each keyframe, and the playhead.
void timeline(Editing& e, float height) {
    auto& menu = e.menu;
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, label = px(18), radius = px(7);
    const auto x_of = [&](float time) { return origin.x + time / style::trick_end * width; };
    ImGui::InvisibleButton("##timeline", ImVec2(width, label + height));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const float pointer = ImGui::GetIO().MousePos.x;
    const float mouse = std::clamp((pointer - origin.x) / width, 0.0f, 1.0f) * style::trick_end;
    auto* draw = ImGui::GetWindowDrawList();
    constexpr std::array<const char*, 3> parts{"POP", "CATCH", "LANDING"};
    for (int part = 0; part < 3; ++part) {
        const float left = x_of(static_cast<float>(part)), right = x_of(static_cast<float>(part + 1));
        draw->AddRectFilled(ImVec2(left, origin.y + label), ImVec2(right, origin.y + label + height), part % 2 ? track_alternate : track_colour);
        draw->AddText(menu.body, px(12), ImVec2(left + px(4), origin.y), line_colour, parts[part]);
        if (part) draw->AddLine(ImVec2(left, origin.y + label), ImVec2(left, origin.y + label + height), line_colour, px(1));
    }
    const float middle = origin.y + label + height * 0.5f;
    // Each blend out is a tail that narrows to the game's pose. It stops at the next keyframe or the trick's end.
    for (std::size_t i = 0; i < e.times.size(); ++i)
        if (const auto blend = blend_out(e, i)) {
            const float x = x_of(e.times[i]);
            const ImU32 colour = blend->set && blend->cut ? cut_tail_colour : blend->set ? tail_colour : default_tail_colour;
            draw->AddTriangleFilled(ImVec2(x, middle - radius), ImVec2(x_of(blend->end), middle), ImVec2(x, middle + radius), colour);
        }
    // The selected keyframe's blend out has a handle at its end.
    const auto selected = menu.styling.key >= 0 ? blend_out(e, static_cast<std::size_t>(menu.styling.key)) : std::nullopt;
    const float handle = selected ? x_of(selected->end) : 0.0f;
    const bool on_handle = selected && hovered && std::abs(pointer - handle) <= radius;
    int nearest = -1;
    for (int i = 0; i < static_cast<int>(e.times.size()); ++i) {
        const float x = x_of(e.times[static_cast<std::size_t>(i)]);
        if (std::abs(pointer - x) <= radius * 1.5f &&
            (nearest < 0 || std::abs(pointer - x) < std::abs(pointer - x_of(e.times[static_cast<std::size_t>(nearest)]))))
            nearest = i;
        const ImU32 colour = i == menu.styling.key ? selected_colour : key_colour;
        draw->AddQuadFilled(ImVec2(x, middle - radius), ImVec2(x + radius, middle), ImVec2(x, middle + radius), ImVec2(x - radius, middle), colour);
    }
    if (selected)
        draw->AddRectFilled(ImVec2(handle - px(2), middle - radius), ImVec2(handle + px(2), middle + radius),
                            selected->set && selected->cut ? cut_tail_colour | IM_COL32_A_MASK : selected_colour);
    const float shown = active && menu.styling.drag_key < 0 && !menu.styling.blend_drag ? pointer : x_of(e.playhead());
    draw->AddLine(ImVec2(shown, origin.y + label - px(3)), ImVec2(shown, origin.y + label + height + px(3)), playhead_colour, px(2));
    if (ImGui::IsItemActivated() && on_handle) {
        // A click on the handle drags the blend out. It does not go past the next keyframe or the trick's end.
        menu.styling.blend_drag = true;
        menu.styling.drag_key = -1;
    } else if (active && menu.styling.blend_drag) {
        if (selected) {
            const auto key = static_cast<std::size_t>(menu.styling.key);
            const float most = std::max(style::min_blend_out_ms, std::min(selected->room_ms, style::max_blend_out_ms));
            const float ms = std::round(std::clamp(style::paced(mouse, e.pace) - style::paced(e.times[key], e.pace), style::min_blend_out_ms, most));
            if (ms != e.blend_outs[key]) send_blend_out(e, key, ms, false);
        }
    } else if (ImGui::IsItemActivated()) {
        // A click on a keyframe selects it for a drag. A click elsewhere moves the playhead.
        menu.styling.drag_key = hovered ? nearest : -1;
        if (nearest >= 0) {
            menu.styling.key = nearest;
            e.hold(e.times[static_cast<std::size_t>(nearest)]);
        } else e.hold(mouse);
    } else if (active && menu.styling.drag_key >= 0 && ImGui::GetIO().MouseDragMaxDistanceSqr[0] > px(3) * px(3)) {
        menu.styling.drag_time = mouse;
        menu.styling.drag_until = e.now + 0.75;
        if (e.now >= menu.styling.edit_sent + 0.08) {
            menu.styling.edit_sent = e.now;
            quiet(e.callbacks, std::format("style key move {} {} {:.3f}", e.trick, menu.styling.drag_key, mouse));
            e.hold(mouse);
        }
    } else if (active && menu.styling.drag_key < 0) {
        e.hold(mouse);
    }
    if (ImGui::IsItemDeactivated() && std::exchange(menu.styling.blend_drag, false) && menu.styling.blend_key == menu.styling.key &&
        e.now < menu.styling.blend_until)
        send_blend_out(e, static_cast<std::size_t>(menu.styling.key), menu.styling.blend_ms, true);
    if (ImGui::IsItemDeactivated() && menu.styling.drag_key >= 0 && e.now < menu.styling.drag_until) {
        quiet(e.callbacks, std::format("style key move {} {} {:.3f}", e.trick, menu.styling.drag_key, menu.styling.drag_time));
        e.hold(menu.styling.drag_time);
    }
    if (hovered && !active) {
        if (on_handle) ImGui::SetTooltip("Drag to change the blend out (%.0f ms)", selected->ms);
        else ImGui::SetTooltip(nearest >= 0 ? "Drag to move this keyframe" : "Click to show this moment");
    }
}
// The selected keyframe's blend out: the ms back to the game's pose. At 0 the pose blends to the next keyframe.
void blend_slider(Editing& e) {
    auto& menu = e.menu;
    if (menu.styling.key < 0 || menu.styling.key >= static_cast<int>(e.times.size())) return;
    const auto key = static_cast<std::size_t>(menu.styling.key);
    const auto blend = blend_out(e, key);
    const bool last = std::ranges::none_of(e.times, [&](float time) { return time > e.times[key]; });
    field(menu, "Blend out", "The time from this keyframe back to the game's pose. At the left end, the pose blends to the next keyframe.");
    int ms = static_cast<int>(std::lround(e.blend_outs[key]));
    const auto none = last ? std::format("Default ({:.0f} ms)", style::release_ms) : std::string("Next keyframe");
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::SliderInt("##blend-out", &ms, 0, static_cast<int>(style::max_blend_out_ms), ms ? "%d ms" : none.c_str(),
                                          ImGuiSliderFlags_AlwaysClamp);
    const bool released = ImGui::IsItemDeactivatedAfterEdit();
    if (changed || released) send_blend_out(e, key, ms > 0 ? std::max(static_cast<float>(ms), style::min_blend_out_ms) : 0.0f, released);
    if (blend && blend->set && blend->cut)
        note((last ? std::format("The trick ends {:.0f} ms after this keyframe, so the blend out ends with it.", blend->room_ms)
                   : std::format("The next keyframe comes {:.0f} ms after this one, so the pose blends straight to it.", blend->room_ms))
                 .c_str());
}
void add_keyframe(Editing& e) {
    send_console(e.menu, e.callbacks, std::format("style key add {} {:.3f}", e.trick, e.menu.styling.time));
    e.menu.styling.pending_key = static_cast<int>(e.times.size());
    e.menu.styling.pending_until = e.now + 1.5;
}
void delete_keyframe(Editing& e) {
    send_console(e.menu, e.callbacks, std::format("style key delete {} {}", e.trick, e.menu.styling.key));
    e.menu.styling.key = std::max(0, e.menu.styling.key - 1);
}
// Three sliders for each joint of the selected keyframe.
void joints(Editing& e) {
    auto& menu = e.menu;
    if (menu.styling.key < 0 || menu.styling.key >= static_cast<int>(e.times.size())) return;
    const Target target{true, e.trick_id, static_cast<std::uint8_t>(menu.styling.key)};
    const float at = e.times[target.key];
    for (int joint = 0; joint < static_cast<int>(style::editable_joints.size()); ++joint) {
        ImGui::PushID(joint);
        field(menu, style::editable_joints[joint].data());
        // A dragged slider shows its own value until the game confirms it.
        const bool mine = menu.styling.edit_joint == joint && menu.styling.edit_target == target && e.now < menu.styling.edit_until;
        auto degrees = mine ? menu.styling.edit : saved(e.model, target, joint);
        const float reset = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
        const float width = (ImGui::GetContentRegionAvail().x - reset - ImGui::GetStyle().ItemSpacing.x * 3) / 3;
        bool changed{}, released{};
        for (int axis = 0; axis < 3; ++axis) {
            ImGui::PushID(axis);
            ImGui::SetNextItemWidth(width);
            constexpr std::array<const char*, 3> formats{"X %.0f", "Y %.0f", "Z %.0f"};
            changed |= ImGui::SliderFloat("##axis", &degrees[axis], -style::max_degrees, style::max_degrees, formats[axis],
                                          ImGuiSliderFlags_AlwaysClamp);
            released |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (ImGui::Button("Reset", ImVec2(reset, 0))) {
            degrees = {};
            changed = released = true;
        }
        if (changed || released) {
            // An edit of a keyframe shows that keyframe.
            if (!e.standing_in || e.replay.playing || std::abs(e.replay.time - at) > 0.05f) e.hold(at);
            menu.styling.edit = degrees;
            menu.styling.edit_joint = joint;
            menu.styling.edit_target = target;
            menu.styling.edit_until = e.now + 0.75;
            // Send during the drag, but not faster than the game accepts commands.
            if (released || e.now >= menu.styling.edit_sent + 0.08) {
                menu.styling.edit_sent = e.now;
                quiet(e.callbacks, std::format("style joint {} {} {:.1f} {:.1f} {:.1f} {}", e.trick, style::editable_joints[joint], degrees[0],
                                               degrees[1], degrees[2], target.key));
            }
        }
        ImGui::PopID();
    }
}
}
void set_style_playhead_feed(StylePlayheadFeed feed) noexcept { playhead_feed.store(feed); }
bool style_editor_wanted() noexcept {
    const auto feed = playhead_feed.load();
    return feed && feed().wanted;
}

// The style editor screen: the stand-in in the middle, the timeline at the bottom, the joints at the side.
// Presets: pick the one in use, make an empty one, or copy the one in use to a new name.
void preset_controls(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool manage) {
    const auto& style = model.style;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##style-preset", style.preset.c_str())) {
        for (const auto& name : style.presets)
            if (ImGui::Selectable(name.c_str(), name == style.preset) && name != style.preset) send_console(menu, callbacks, "style preset load " + name);
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##style-preset-name", "Name for a new preset", menu.styling.preset_name.data(), menu.styling.preset_name.size());
    const std::string name(menu.styling.preset_name.data());
    // The name is the file name: letters, digits, '-' and '_'.
    const bool valid = style::preset_name(name) && std::ranges::none_of(style.presets, [&](const std::string& p) { return style::same_text(p, name); });
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("New empty preset", ImVec2(half, 0))) {
        send_console(menu, callbacks, "style preset new " + name);
        menu.styling.preset_name = {};
    }
    ImGui::SameLine();
    if (ImGui::Button("Save a copy", ImVec2(half, 0))) {
        send_console(menu, callbacks, "style preset copy " + name);
        menu.styling.preset_name = {};
    }
    ImGui::EndDisabled();
    if (!name.empty() && !valid) note("Use letters, digits, '-' and '_', and a name that no preset has.");
    if (!manage) return;
    // The second click within three seconds deletes.
    const bool armed = ImGui::GetTime() < menu.styling.delete_until;
    if (ImGui::Button(armed ? "Click again to delete" : "Delete this preset", ImVec2(half, 0))) {
        if (armed) send_console(menu, callbacks, "style preset delete " + style.preset);
        menu.styling.delete_until = armed ? 0.0 : ImGui::GetTime() + 3.0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open the presets folder", ImVec2(half, 0))) send_console(menu, callbacks, "style preset folder");
}
void draw_style_editor(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool exit_requested) {
    auto& io = ImGui::GetIO();
    // After the screen opens again, ask for the selected trick again.
    if (ImGui::GetTime() > menu.styling.drawn_at + 1.0) menu.styling.asked.clear();
    menu.styling.drawn_at = ImGui::GetTime();
    menu::set_scale(std::clamp(model.menu_scale, min_menu_scale, max_menu_scale));
    const auto restore_font_scale = io.FontGlobalScale;
    io.FontGlobalScale = std::clamp(model.menu_scale, min_menu_scale, max_menu_scale);
    ImGui::PushFont(menu.body);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(10), px(7)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14), px(12)));
    const int colours = skate_theme::push_widget_colours();
    auto e = begin_editing(menu, model, callbacks);
    // The screen draws one or two more frames after close, and must not ask for its trick again.
    const auto close = [&] {
        menu.styling.closing_until = ImGui::GetTime() + 3.0;
        send_console(menu, callbacks, "style editor close");
    };
    const float side = std::min(px(400), io.DisplaySize.x * 0.34f), bottom = px(176);
    // No keyboard navigation: Space and the arrow keys are the editor's own shortcuts, and must not also press the focused button.
    constexpr auto fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs;
    const bool typing = io.WantTextInput;

    // The side panel: the trick and the selected keyframe.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(side, io.DisplaySize.y - bottom));
    if (ImGui::Begin("##style-editor-side", nullptr, fixed)) {
        ImGui::PushFont(menu.heading);
        ImGui::TextUnformatted("STYLE EDITOR");
        ImGui::PopFont();
        ImGui::TextDisabled("Preset");
        preset_controls(menu, model, callbacks, false);
        ImGui::Spacing();
        ImGui::TextDisabled("Trick");
        ImGui::SetNextItemWidth(-1);
        trick_picker(e);
        const bool has_clip = (model.style.clips >> e.trick_id & 1) != 0;
        if (!e.standing_in) {
            // A selected trick is shown. A trick without a clip is first fetched from Skatepedia.
            if (e.now >= menu.styling.closing_until && (menu.styling.asked != e.trick || e.now > menu.styling.asked_at + 20.0)) {
                menu.styling.asked = e.trick;
                menu.styling.asked_at = e.now;
                send_console(menu, callbacks, "style editor show " + e.trick);
            }
            if (!has_clip) {
                warn("Loading this trick...");
                note("About ten seconds, the first time only.");
            }
        }
        if (!model.style.editor_note.empty()) ImGui::TextDisabled("%s", model.style.editor_note.c_str());
        if (menu.styling.key >= 0) {
            section(menu, std::format("KEYFRAME {} OF {}", menu.styling.key + 1, e.times.size()).c_str());
            blend_slider(e);
            ImGui::BeginChild("##style-editor-joints", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
            joints(e);
            ImGui::EndChild();
        } else note("This trick has no keyframes. Add one at the playhead.");
    }
    ImGui::End();

    // The bottom bar: transport, keyframe controls and the timeline.
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - bottom));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, bottom));
    if (ImGui::Begin("##style-editor-timeline", nullptr, fixed | ImGuiWindowFlags_NoScrollbar)) {
        const float time = e.playhead();
        const auto previous = [&] {
            float best = 0;
            for (const auto key : e.times)
                if (key < time - 0.02f) best = std::max(best, key);
            return best;
        };
        const auto next = [&] {
            float best = style::trick_end;
            for (const auto key : e.times)
                if (key > time + 0.02f) best = std::min(best, key);
            return best;
        };
        const auto select_at = [&](float at) {
            for (int i = 0; i < static_cast<int>(e.times.size()); ++i)
                if (std::abs(e.times[static_cast<std::size_t>(i)] - at) < 0.001f) menu.styling.key = i;
            e.hold(at);
        };
        const auto toggle = [&] { send_now(e, e.replay.playing ? "style editor pause" : "style editor play"); };
        const auto step = [&](int frames) { send_now(e, std::format("style editor step {}", frames)); };
        ImGui::BeginDisabled(!e.standing_in);
        if (ImGui::Button("|<")) e.hold(0);
        ImGui::SameLine();
        if (ImGui::Button("< Key")) select_at(previous());
        ImGui::SameLine();
        if (ImGui::Button("< Frame")) step(-1);
        ImGui::SameLine();
        if (ImGui::Button(e.replay.playing ? "Pause" : "Play", ImVec2(px(90), 0))) toggle();
        ImGui::SameLine();
        if (ImGui::Button("Frame >")) step(1);
        ImGui::SameLine();
        if (ImGui::Button("Key >")) select_at(next());
        ImGui::SameLine();
        if (ImGui::Button(">|")) e.hold(style::trick_end);
        ImGui::SameLine();
        constexpr std::array<std::pair<const char*, float>, 4> speeds{{{"1x", 1.0f}, {"3/4x", 0.75f}, {"1/2x", 0.5f}, {"1/4x", 0.25f}}};
        menu.styling.speed = std::clamp(menu.styling.speed, 0, static_cast<int>(speeds.size()) - 1);
        ImGui::SetNextItemWidth(px(84));
        if (ImGui::BeginCombo("##style-speed", speeds[static_cast<std::size_t>(menu.styling.speed)].first)) {
            for (int i = 0; i < static_cast<int>(speeds.size()); ++i)
                if (ImGui::Selectable(speeds[static_cast<std::size_t>(i)].first, i == menu.styling.speed)) {
                    menu.styling.speed = i;
                    send_now(e, std::format("style editor speed {}", speeds[static_cast<std::size_t>(i)].second));
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed");
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(28));
        ImGui::BeginDisabled(e.times.size() >= style::max_keys);
        if (ImGui::Button("Add keyframe")) add_keyframe(e);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(menu.styling.key < 0);
        if (ImGui::Button("Delete keyframe")) delete_keyframe(e);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reset trick")) send_console(menu, callbacks, "style clear " + e.trick);
        const float closing = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetWindowWidth() - closing - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("Close")) close();
        timeline(e, px(52));
        ImGui::TextDisabled("Drag: turn the camera.   Right drag: raise or lower the view.   Wheel: zoom (Shift: fine).   Space: play / pause   Left / Right: frame   %s",
                            model.style.saved ? "Saved" : "Saving...");
        if (!typing && e.standing_in) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggle();
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) step(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) step(1);
        }
    }
    ImGui::End();

    // A drag in the view turns the camera around the stand-in.
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && !ImGui::IsAnyItemActive()) {
        // A left drag turns the camera. A right drag also raises or lowers the camera target.
        const bool lifting = ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0);
        const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) || lifting;
        const float zoom = -io.MouseWheel * (io.KeyShift ? 0.1f : 0.35f);
        if (dragging) menu.styling.orbit[0] -= io.MouseDelta.x * 0.008f;
        if (lifting) menu.styling.orbit[3] += io.MouseDelta.y * 0.004f;
        else if (dragging) menu.styling.orbit[1] += io.MouseDelta.y * 0.006f;
        menu.styling.orbit[2] += zoom;
    }
    send_controls(e, false);
    if (exit_requested || (!typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) close();
    ImGui::PopStyleColor(colours);
    ImGui::PopStyleVar(7);
    ImGui::PopFont();
    io.FontGlobalScale = restore_font_scale;
}

namespace menu {
void style_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& style = model.style;
    ImGui::BeginChild("style-body", ImVec2(0, page_body_height(menu)));

    begin_card(menu, "style", "YOUR STYLE");
    bool enabled = style.enabled;
    if (toggle_row(menu, "Style", "Add your own body movement to the game's trick animations.", enabled))
        send_console(menu, callbacks, enabled ? "style 1" : "style 0");
    bool share = style.share;
    if (toggle_row(menu, "Show to other players", "Off: only you see your style.", share))
        send_console(menu, callbacks, share ? "style share 1" : "style share 0");
    note("Flip tricks for now. Grinds, grabs, manuals and pushing are coming soon.");
    end_card();

    begin_card(menu, "style-presets", "PRESETS", "A preset holds the style of every trick you edit in it.");
    preset_controls(menu, model, callbacks, true);
    {
        // The tricks that the preset in use changes.
        std::vector<std::uint8_t> tricks;
        for (const auto& rotation : style.rotations)
            if (rotation.target.trick && std::ranges::find(tricks, rotation.target.id) == tricks.end()) tricks.push_back(rotation.target.id);
        info(menu, "Tricks styled", std::to_string(tricks.size()));
        info(menu, "Saved", style.saved ? "Yes" : "Not yet");
    }
    note("Each preset is one file in the presets folder. To share a preset, send that file. It works the same on every computer.");
    end_card();

    begin_card(menu, "style-editor", "EDITOR", "Edit each trick of the preset on a timeline.");
    if (primary_button(menu, "Open the style editor", true)) send_console(menu, callbacks, "style editor open");
    note("A trick that you open for the first time takes about ten seconds to load.");
    if (!style.editor_note.empty()) info(menu, "Last", style.editor_note);
    end_card();
    ImGui::EndChild();
}
}
}
