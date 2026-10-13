#include "style_overlay.h"
#include "style_batch.h"
#include "Extension/Style/style_pose.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::overlay {
namespace {
using namespace menu;
constexpr double shown_s = 0.75;
constexpr double send_gap_s = 0.08;
constexpr double controls_gap_s = 0.04;
constexpr double pending_key_s = 1.5;
constexpr double second_click_s = 3.0;
constexpr double closing_s = 3.0;
constexpr double reopen_s = 1.0;
using style::Target;
// A value the menu set, shown in place of the model's until the game has it, and sent at most every send_gap_s.
template <class T> struct Unconfirmed {
    std::optional<T> value;
    double until{}, sent{};
    void set(T next, double now, double shown_for = shown_s) {
        value = std::move(next);
        until = now + shown_for;
    }
    [[nodiscard]] bool send_due(double now, bool final = false) {
        if (!final && now < sent + send_gap_s) return false;
        sent = now;
        return true;
    }
    [[nodiscard]] const T* shown(double now) const { return value && now < until ? &*value : nullptr; }
    void drop() {
        value.reset();
        until = 0;
    }
};
struct Armed {
    double until{};
    [[nodiscard]] bool armed(double now) const { return now < until; }
    bool click(double now) {
        const bool second = armed(now);
        until = second ? 0.0 : now + second_click_s;
        return second;
    }
};
struct KeyMove {
    std::size_t key{};
    float time{};
};
struct KeyBlend {
    std::size_t key{};
    float ms{};
};
struct JointEdit {
    int joint{};
    Target target;
    std::array<float, 3> degrees{};
};
struct Reselect {
    float time{};
    std::vector<float> before;
};
enum class TimelineDrag { none, playhead, keyframe, blend_out };
// The action that waits for the leave prompt.
enum class Leave { close, trick, preset };
struct ScreenState {
    int trick = 2;
    std::optional<std::size_t> key = 0; // the selected keyframe
    float time = 0.5f;
    double closing_until{}, drawn_at{};
    std::string asked;                    // the trick that the editor last asked to show
    std::uint8_t followed{};              // the game's last picked trick that the selection followed
    std::array<char, 41> preset_name{};
    Armed delete_click, discard_click;
    std::string delete_preset; // the preset in use when delete_click was last drawn
    TimelineDrag drag{};
    std::size_t drag_key{};
    Unconfirmed<KeyMove> moved;
    Unconfirmed<KeyBlend> blend;
    Unconfirmed<JointEdit> joint_edit;
    Unconfirmed<std::size_t> pending_key; // a new keyframe, selected when the game has it
    Unconfirmed<Reselect> reselect;       // after a reload, the selection follows its time
    std::array<float, 4> orbit{};         // camera turn, zoom and lift not yet sent
    int speed{};                          // index into the playback speeds
    std::optional<float> hold;            // a playhead move not yet sent
    double controls_sent{};
    bool grouping{};                      // a drag's undo group is open
    bool popup_open{};                    // a popup was open in the last frame
    std::optional<Leave> leave;
    int leave_trick{};
    std::string leave_command;
};
// Presentation thread only.
ScreenState styling;
// Undo, redo, discard and a keyframe delete show the game's values again, not the menu's own.
void drop_unconfirmed() {
    styling.moved.drop();
    styling.blend.drop();
    styling.joint_edit.drop();
    styling.pending_key.drop();
}
// A reload gives IDs by the file's order, so the selection follows its keyframe's time.
void reselect_after_reload(const Model& model) {
    auto times = style::keys_of(model.style.times, static_cast<std::uint8_t>(styling.trick));
    if (!styling.key || *styling.key >= times.size()) return;
    const float at = times[*styling.key];
    styling.reselect.set({at, std::move(times)}, ImGui::GetTime());
}
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
struct Editing {
    SkateMenu& menu;
    const Model& model;
    const CallbacksV3& callbacks;
    style::Playhead replay;
    std::uint8_t trick_id{};
    std::string trick;
    std::vector<float> times;
    std::vector<float> blend_outs; // ms for each keyframe. 0: the keyframe blends to the next keyframe
    style::Pace pace;
    double now{};
    bool standing_in{}; // the stand-in shows this trick's clip
    // send_controls sends the move.
    void hold(float time) const {
        styling.time = time;
        if (standing_in) styling.hold = time;
    }
    [[nodiscard]] float playhead() const { return standing_in ? replay.time : styling.time; }
};
void select_trick(Editing& e, int trick) {
    styling.trick = trick;
    styling.key = 0;
    styling.drag = TimelineDrag::none;
    drop_unconfirmed();
    quiet(e.callbacks, "style history clear");
}
Editing begin_editing(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto feed = playhead_feed.load();
    Editing e{menu, model, callbacks, feed ? feed() : style::Playhead{}};
    e.now = ImGui::GetTime();
    // A trick picked outside the screen, such as by a console command, becomes the selection.
    if (const auto& load = model.style.editor_load; load.trick && load.trick != styling.followed) {
        styling.followed = load.trick;
        if (load.trick <= style::last_capturable && load.trick != styling.trick) {
            select_trick(e, load.trick);
            // The game loads or shows it already: do not ask for it again.
            using Phase = style::EditorLoad::Phase;
            if (load.phase == Phase::loading || load.phase == Phase::ready) styling.asked = style::flip_trick_names[load.trick];
        }
    }
    styling.trick = std::clamp(styling.trick, 1, static_cast<int>(style::last_capturable));
    e.trick_id = static_cast<std::uint8_t>(styling.trick);
    e.trick = style::flip_trick_names[e.trick_id];
    e.times = style::keys_of(model.style.times, e.trick_id);
    if (const auto* pending = styling.pending_key.shown(e.now); pending && e.times.size() > *pending) {
        styling.key = *pending;
        styling.pending_key.drop();
    }
    if (const auto* r = styling.reselect.shown(e.now); r && e.times != r->before) {
        styling.key = style::key_at_time(e.times, r->time);
        styling.reselect.drop();
    }
    styling.key = e.times.empty() ? std::nullopt : std::optional<std::size_t>(std::min(styling.key.value_or(0), e.times.size() - 1));
    e.standing_in = e.replay.editor && e.replay.trick == e.trick_id;
    if (const auto* move = styling.moved.shown(e.now); move && move->key < e.times.size()) e.times[move->key] = move->time;
    e.blend_outs = style::blend_outs_of(model.style.blend_outs, e.trick_id, e.times.size());
    if (const auto* blend = styling.blend.shown(e.now); blend && blend->key < e.times.size()) e.blend_outs[blend->key] = blend->ms;
    e.pace = model.style.pace;
    if (e.standing_in) styling.time = e.replay.time;
    return e;
}
// At most every controls_gap_s: the game runs one command a tick.
void send_controls(Editing& e, bool now) {
    auto& s = styling;
    if (!now && e.now < s.controls_sent + controls_gap_s) return;
    bool sent{};
    if (const auto time = std::exchange(s.hold, std::nullopt)) {
        quiet(e.callbacks, std::format("style editor hold {:.4f}", std::clamp(*time, 0.0f, style::trick_end)));
        sent = true;
    }
    if (s.orbit != std::array<float, 4>{}) {
        const auto [yaw, pitch, distance, height] = s.orbit;
        const auto limit = [](float value) { return std::clamp(value, -style::max_orbit_step, style::max_orbit_step); };
        quiet(e.callbacks,
              std::format("style editor orbit {:.4f} {:.4f} {:.4f} {:.4f}", limit(yaw), limit(pitch), limit(distance), limit(height)));
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
// The edits between a press and its release are one undo step.
void begin_group(Editing& e) {
    if (!std::exchange(styling.grouping, true)) quiet(e.callbacks, "style group begin");
}
void end_group(Editing& e) {
    if (std::exchange(styling.grouping, false)) quiet(e.callbacks, "style group end");
}
// What the sliders read since `SliderEdit` was made: the press starts an undo group, the release ends it.
struct SliderEdit {
    bool released{}, pressed{}, let_go{};
    void read() {
        released |= ImGui::IsItemDeactivatedAfterEdit();
        pressed |= ImGui::IsItemActivated();
        let_go |= ImGui::IsItemDeactivated();
    }
};
float button_width(const char* text) { return ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2; }
void undo_or_redo(Editing& e, const char* command) {
    drop_unconfirmed();
    send_now(e, command);
}
void show_trick(Editing& e, int trick) {
    select_trick(e, trick);
    send_console(e.menu, e.callbacks, std::format("style editor show {}", style::flip_trick_names[static_cast<std::size_t>(trick)]));
}
// The screen draws one or two more frames after close, and must not ask for its trick again.
void close_screen(Editing& e) {
    styling.closing_until = e.now + closing_s;
    styling.grouping = false;
    send_console(e.menu, e.callbacks, "style editor close");
}
// Where a keyframe's blend out ends on the timeline.
struct BlendOut {
    float ms{}, end{};
    float room_ms{};           // the time to the next keyframe or the trick's end
    bool last{}, set{}, cut{}; // set: the keyframe has its own time. cut: the next keyframe or the trick's end comes first
};
std::optional<BlendOut> blend_out(const Editing& e, std::size_t key) {
    // A trick switch selects keyframe 0 before this frame's keyframes change.
    if (key >= e.times.size() || key >= e.blend_outs.size()) return std::nullopt;
    const float at = e.times[key];
    const float next = style::next_key_time(e.times, at);
    const bool last = style::last_key(e.times, key), set = e.blend_outs[key] > 0;
    const float ms = style::blend_out_ms(e.blend_outs[key], last);
    if (ms <= 0 || at >= style::trick_end) return std::nullopt;
    const float from = style::paced(at, e.pace), room = style::paced(next, e.pace) - from;
    return BlendOut{ms, style::timeline_at(from + std::min(ms, room), e.pace), room, last, set, ms >= room - 1e-3f};
}
void send_blend_out(Editing& e, std::size_t key, float ms, bool final) {
    styling.blend.set({key, ms}, e.now);
    if (!styling.blend.send_due(e.now, final)) return;
    quiet(e.callbacks, std::format("style key blendout {} {} {}", e.trick, key, ms > 0 ? std::format("{:.0f}", ms) : "off"));
}
void trick_picker(Editing& e) {
    constexpr int listed = style::last_capturable + 1;
    const auto nollie = [](int trick) { return trick >= 16 && trick <= 30; };
    const auto picked = std::format("Flip tricks  /  {}", style::flip_trick_titles[e.trick_id]);
    if (!ImGui::BeginCombo("##style-trick", picked.c_str(), ImGuiComboFlags_HeightLargest)) return;
    const auto soon = [](const char* text) {
        ImGui::BeginDisabled();
        ImGui::Selectable(text, false);
        ImGui::EndDisabled();
    };
    const auto group = [&](const char* title, bool wanted) {
        ImGui::TextDisabled("%s", title);
        ImGui::Indent();
        for (int i = 1; i < listed; ++i) {
            if (nollie(i) != wanted) continue;
            const bool edited =
                std::ranges::any_of(e.model.style.rotations, [&](const auto& r) { return r.target.trick && r.target.id == i; });
            const auto label = std::format("{}{}", style::flip_trick_titles[i], edited ? "  *" : "");
            if (!ImGui::Selectable(label.c_str(), i == styling.trick) || i == styling.trick) continue;
            if (e.model.style.unsaved) {
                styling.leave = Leave::trick;
                styling.leave_trick = i;
            } else {
                show_trick(e, i);
            }
        }
        ImGui::Unindent();
    };
    ImGui::SeparatorText("FLIP TRICKS");
    group("Regular", false);
    group("Nollie", true);
    soon("Quick ollie and quick nollie  (coming soon)");
    soon("Switch  (coming soon)");
    soon("Fakie  (coming soon)");
    ImGui::SeparatorText("MORE");
    soon("Grinds  (coming soon)");
    soon("Grabs  (coming soon)");
    soon("Manuals  (coming soon)");
    ImGui::EndCombo();
}
struct TimelineArea {
    ImVec2 origin;
    float width{}, label{}, height{}, radius{}, middle{};
    [[nodiscard]] float x_of(float time) const { return origin.x + time / style::trick_end * width; }
};
struct TimelineHit {
    float pointer_x{}, pointer_time{};
    std::optional<std::size_t> key;   // the nearest keyframe within reach
    std::optional<BlendOut> selected; // the selected keyframe's blend out
    bool on_handle{}, hovered{}, active{};
};
TimelineHit timeline_hit(const Editing& e, const TimelineArea& area) {
    TimelineHit hit;
    hit.hovered = ImGui::IsItemHovered();
    hit.active = ImGui::IsItemActive();
    hit.pointer_x = ImGui::GetIO().MousePos.x;
    hit.pointer_time = std::clamp((hit.pointer_x - area.origin.x) / area.width, 0.0f, 1.0f) * style::trick_end;
    hit.selected = styling.key ? blend_out(e, *styling.key) : std::nullopt;
    hit.on_handle = hit.selected && hit.hovered && std::abs(hit.pointer_x - area.x_of(hit.selected->end)) <= area.radius;
    for (std::size_t i = 0; i < e.times.size(); ++i) {
        const float gap = std::abs(hit.pointer_x - area.x_of(e.times[i]));
        if (gap <= area.radius * 1.5f && (!hit.key || gap < std::abs(hit.pointer_x - area.x_of(e.times[*hit.key])))) hit.key = i;
    }
    return hit;
}
void draw_timeline(const Editing& e, const TimelineArea& area, const TimelineHit& hit) {
    auto* draw = ImGui::GetWindowDrawList();
    const float top = area.origin.y + area.label, bottom = top + area.height, middle = area.middle, radius = area.radius;
    constexpr std::array labels{"POP", "FALL", "LANDING"};
    static_assert(labels.size() == style::part_names.size());
    for (std::size_t part = 0; part < labels.size(); ++part) {
        const float left = area.x_of(static_cast<float>(part)), right = area.x_of(static_cast<float>(part + 1));
        draw->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), part % 2 ? track_alternate : track_colour);
        draw->AddText(e.menu.body, px(12), ImVec2(left + px(4), area.origin.y), line_colour, labels[part]);
        if (part) draw->AddLine(ImVec2(left, top), ImVec2(left, bottom), line_colour, px(1));
    }
    for (std::size_t i = 0; i < e.times.size(); ++i)
        if (const auto blend = blend_out(e, i)) {
            const float x = area.x_of(e.times[i]);
            const ImU32 colour = blend->set && blend->cut ? cut_tail_colour : blend->set ? tail_colour : default_tail_colour;
            draw->AddTriangleFilled(ImVec2(x, middle - radius), ImVec2(area.x_of(blend->end), middle), ImVec2(x, middle + radius), colour);
        }
    for (std::size_t i = 0; i < e.times.size(); ++i) {
        const float x = area.x_of(e.times[i]);
        const ImU32 colour = styling.key == i ? selected_colour : key_colour;
        draw->AddQuadFilled(ImVec2(x, middle - radius), ImVec2(x + radius, middle), ImVec2(x, middle + radius), ImVec2(x - radius, middle),
                            colour);
    }
    if (const auto& selected = hit.selected) {
        const float handle = area.x_of(selected->end);
        draw->AddRectFilled(ImVec2(handle - px(2), middle - radius), ImVec2(handle + px(2), middle + radius),
                            selected->set && selected->cut ? cut_tail_colour | IM_COL32_A_MASK : selected_colour);
    }
    const float shown = hit.active && styling.drag == TimelineDrag::playhead ? hit.pointer_x : area.x_of(e.playhead());
    draw->AddLine(ImVec2(shown, top - px(3)), ImVec2(shown, bottom + px(3)), playhead_colour, px(2));
}
void start_timeline_drag(Editing& e, const TimelineHit& hit) {
    // The last drag's drop point does not apply.
    styling.moved.drop();
    if (hit.on_handle) {
        styling.drag = TimelineDrag::blend_out;
        begin_group(e);
        return;
    }
    if (hit.hovered && hit.key) {
        styling.drag = TimelineDrag::keyframe;
        styling.drag_key = *hit.key;
        styling.key = *hit.key;
        e.hold(e.times[*hit.key]);
        begin_group(e);
        return;
    }
    styling.drag = TimelineDrag::playhead;
    e.hold(hit.pointer_time);
}
void drag_blend_out(Editing& e, const TimelineHit& hit) {
    if (!hit.selected) return;
    const auto key = *styling.key;
    const float most = std::max(style::min_blend_out_ms, std::min(hit.selected->room_ms, style::max_blend_out_ms));
    const float pointed = style::paced(hit.pointer_time, e.pace) - style::paced(e.times[key], e.pace);
    const float ms = std::round(std::clamp(pointed, style::min_blend_out_ms, most));
    if (ms != e.blend_outs[key]) send_blend_out(e, key, ms, false);
}
void send_key_move(Editing& e, std::size_t key, float time) {
    quiet(e.callbacks, std::format("style key move {} {} {:.3f}", e.trick, key, time));
    e.hold(time);
}
void drag_keyframe(Editing& e, const TimelineHit& hit) {
    if (ImGui::GetIO().MouseDragMaxDistanceSqr[0] <= px(3) * px(3)) return;
    styling.moved.set({styling.drag_key, hit.pointer_time}, e.now);
    if (!styling.moved.send_due(e.now)) return;
    send_key_move(e, styling.drag_key, hit.pointer_time);
}
// The release sends the last value, which the send gap may have held back.
void end_timeline_drag(Editing& e) {
    const auto drag = std::exchange(styling.drag, TimelineDrag::none);
    const auto* blend = styling.blend.shown(e.now);
    if (drag == TimelineDrag::blend_out && blend && blend->key == styling.key)
        send_blend_out(e, blend->key, blend->ms, true);
    if (const auto* move = styling.moved.shown(e.now); drag == TimelineDrag::keyframe && move) send_key_move(e, move->key, move->time);
    end_group(e);
}
void timeline(Editing& e, float height) {
    TimelineArea area{ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, px(18), height, px(7)};
    area.middle = area.origin.y + area.label + height * 0.5f;
    ImGui::InvisibleButton("##timeline", ImVec2(area.width, area.label + height));
    const auto hit = timeline_hit(e, area);
    draw_timeline(e, area, hit);
    if (ImGui::IsItemActivated()) start_timeline_drag(e, hit);
    else if (hit.active && styling.drag == TimelineDrag::blend_out) drag_blend_out(e, hit);
    else if (hit.active && styling.drag == TimelineDrag::keyframe) drag_keyframe(e, hit);
    else if (hit.active && styling.drag == TimelineDrag::playhead) e.hold(hit.pointer_time);
    if (ImGui::IsItemDeactivated()) end_timeline_drag(e);
    if (!hit.hovered || hit.active) return;
    if (hit.on_handle) ImGui::SetTooltip("Drag to change the blend out (%.0f ms)", hit.selected->ms);
    else ImGui::SetTooltip(hit.key ? "Drag to move this keyframe" : "Click to show this moment");
}
void blend_slider(Editing& e) {
    if (!styling.key || *styling.key >= e.times.size()) return;
    const auto key = *styling.key;
    const auto blend = blend_out(e, key);
    const bool last = style::last_key(e.times, key);
    field(e.menu, "Blend out",
          "The time from this keyframe back to the game's pose. At the left end, the pose blends to the next keyframe.");
    int ms = static_cast<int>(std::lround(e.blend_outs[key]));
    const auto none = last ? std::format("Default ({:.0f} ms)", style::release_ms) : std::string("Next keyframe");
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::SliderInt("##blend-out", &ms, 0, static_cast<int>(style::max_blend_out_ms), ms ? "%d ms" : none.c_str(),
                                          ImGuiSliderFlags_AlwaysClamp);
    SliderEdit edit;
    edit.read();
    if (edit.pressed) begin_group(e);
    if (changed || edit.released)
        send_blend_out(e, key, ms > 0 ? std::max(static_cast<float>(ms), style::min_blend_out_ms) : 0.0f, edit.released);
    if (edit.let_go) end_group(e);
    if (blend && blend->set && blend->cut)
        note((last ? std::format("The trick ends {:.0f} ms after this keyframe, so the blend out ends with it.", blend->room_ms)
                   : std::format("The next keyframe comes {:.0f} ms after this one, so the pose blends straight to it.", blend->room_ms))
                 .c_str());
}
void add_keyframe(Editing& e) {
    send_console(e.menu, e.callbacks, std::format("style key add {} {:.3f}", e.trick, styling.time));
    styling.pending_key.set(e.times.size(), e.now, pending_key_s);
}
void delete_keyframe(Editing& e) {
    if (!styling.key) return;
    send_console(e.menu, e.callbacks, std::format("style key delete {} {}", e.trick, *styling.key));
    styling.key = *styling.key < e.times.size() ? style::selection_after_delete(e.times, *styling.key) : std::nullopt;
    drop_unconfirmed();
}
void joints(Editing& e) {
    if (!styling.key || *styling.key >= e.times.size()) return;
    const Target target{true, e.trick_id, static_cast<std::uint8_t>(*styling.key)};
    const float at = e.times[target.key];
    for (int joint = 0; joint < static_cast<int>(style::editable_joints.size()); ++joint) {
        ImGui::PushID(joint);
        field(e.menu, style::editable_joints[joint].data());
        const auto* mine = styling.joint_edit.shown(e.now);
        auto degrees = mine && mine->joint == joint && mine->target == target ? mine->degrees : saved(e.model, target, joint);
        const float reset = button_width("Reset");
        const float width = (ImGui::GetContentRegionAvail().x - reset - ImGui::GetStyle().ItemSpacing.x * 3) / 3;
        bool changed{};
        SliderEdit edit;
        for (int axis = 0; axis < 3; ++axis) {
            ImGui::PushID(axis);
            ImGui::SetNextItemWidth(width);
            constexpr std::array<const char*, 3> formats{"X %.0f", "Y %.0f", "Z %.0f"};
            changed |= ImGui::SliderFloat("##axis", &degrees[axis], -style::max_degrees, style::max_degrees, formats[axis],
                                          ImGuiSliderFlags_AlwaysClamp);
            edit.read();
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (edit.pressed) begin_group(e);
        if (ImGui::Button("Reset", ImVec2(reset, 0))) {
            degrees = {};
            changed = edit.released = true;
        }
        if (changed || edit.released) {
            // An edit of a keyframe shows that keyframe.
            if (!e.standing_in || e.replay.playing || std::abs(e.replay.time - at) > 0.05f) e.hold(at);
            styling.joint_edit.set({joint, target, degrees}, e.now);
            if (styling.joint_edit.send_due(e.now, edit.released))
                quiet(e.callbacks, std::format("style joint {} {} {:.1f} {:.1f} {:.1f} {}", e.trick, style::editable_joints[joint],
                                               degrees[0], degrees[1], degrees[2], target.key));
        }
        if (edit.let_go) end_group(e);
        ImGui::PopID();
    }
}
// Closing the editor always asks; a trick or preset switch asks only with unsaved changes.
void leave_prompt(Editing& e) {
    auto& s = styling;
    constexpr const char* title = "##style-leave";
    if (!s.leave) return;
    const bool unsaved = e.model.style.unsaved;
    const auto go = [&] {
        const auto what = *std::exchange(s.leave, std::nullopt);
        if (what == Leave::close) close_screen(e);
        else if (what == Leave::trick) show_trick(e, s.leave_trick);
        else send_console(e.menu, e.callbacks, s.leave_command);
    };
    const bool was_open = ImGui::IsPopupOpen(title);
    if (!unsaved && *s.leave != Leave::close) {
        if (was_open && ImGui::BeginPopupModal(title)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        go();
        return;
    }
    if (!was_open) ImGui::OpenPopup(title);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoNavInputs))
        return;
    bool done{};
    constexpr std::array<const char*, 3> questions{"Close the style editor?", "Switch to another trick?", "Switch to another preset?"};
    static_assert(questions.size() == static_cast<std::size_t>(Leave::preset) + 1);
    ImGui::TextUnformatted(questions[static_cast<std::size_t>(*s.leave)]);
    if (unsaved) {
        ImGui::TextDisabled("This preset has unsaved changes.");
        if (!e.model.style.save_issue.empty()) ImGui::TextDisabled("Not saved: %s", e.model.style.save_issue.c_str());
        if (ImGui::Button("Save", ImVec2(px(110), 0))) {
            quiet(e.callbacks, "style save");
            done = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(px(110), 0))) {
            reselect_after_reload(e.model);
            quiet(e.callbacks, "style reload");
            done = true;
        }
    } else if (ImGui::Button("Close", ImVec2(px(110), 0))) {
        done = true;
    }
    ImGui::SameLine();
    // The Escape press that opened the prompt does not also cancel it.
    if (ImGui::Button("Cancel", ImVec2(px(110), 0)) || (was_open && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        s.leave.reset();
        ImGui::CloseCurrentPopup();
    } else if (done) {
        go();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Discard reverts every unsaved edit, so it takes a second click within second_click_s.
const char* discard_label() { return styling.discard_click.armed(ImGui::GetTime()) ? "Click again to discard" : "Discard changes"; }
float save_row_width(const Model& model) {
    const auto& look = ImGui::GetStyle();
    float width = ImGui::GetFrameHeight() + look.ItemInnerSpacing.x + ImGui::CalcTextSize("Auto save").x;
    if (!model.style.auto_save) width += button_width("Save") + button_width(discard_label()) + look.ItemSpacing.x * 2;
    return width;
}
void save_row(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& current = model.style;
    bool auto_save = current.auto_save;
    if (ImGui::Checkbox("Auto save", &auto_save)) send_console(menu, callbacks, auto_save ? "style autosave 1" : "style autosave 0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: each edit saves %u ms after it is made. Off: edits wait for Save (Ctrl+S in the editor).",
                          style::autosave_delay_ms);
    if (current.auto_save) return;
    ImGui::SameLine();
    ImGui::BeginDisabled(!current.unsaved);
    if (ImGui::Button("Save")) send_console(menu, callbacks, "style save");
    ImGui::SameLine();
    if (ImGui::Button(discard_label()) && styling.discard_click.click(ImGui::GetTime())) {
        reselect_after_reload(model);
        send_console(menu, callbacks, "style reload");
        drop_unconfirmed();
    }
    ImGui::EndDisabled();
}
void preset_controls(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool manage) {
    const auto& current = model.style;
    // The editor has this row next to its Close button.
    if (manage) save_row(menu, model, callbacks);
    // A switch away from unsaved changes: the editor asks first, and this page waits for Save or Discard.
    const auto switch_to = [&](const std::string& command) {
        if (!current.unsaved) {
            send_console(menu, callbacks, command);
            return;
        }
        styling.leave = Leave::preset;
        styling.leave_command = command;
    };
    const bool locked = manage && current.unsaved;
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##style-preset", current.preset.c_str())) {
        for (const auto& name : current.presets)
            if (ImGui::Selectable(name.c_str(), name == current.preset) && name != current.preset) switch_to("style preset load " + name);
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (locked) note("Save or discard the changes to switch presets.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##style-preset-name", "Name for a new preset", styling.preset_name.data(), styling.preset_name.size());
    const std::string name(styling.preset_name.data());
    const bool valid = style::preset_name(name) &&
                       std::ranges::none_of(current.presets, [&](const std::string& p) { return console::equal(p, name); });
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::BeginDisabled(!valid || locked);
    if (ImGui::Button("New empty preset", ImVec2(half, 0))) {
        switch_to("style preset new " + name);
        // A switch that waits for the leave prompt keeps the name, in case of Cancel.
        if (!current.unsaved) styling.preset_name = {};
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!valid);
    ImGui::SameLine();
    if (ImGui::Button("Save a copy", ImVec2(half, 0))) {
        send_console(menu, callbacks, "style preset copy " + name);
        styling.preset_name = {};
    }
    if (ImGui::IsItemHovered() && current.unsaved)
        ImGui::SetTooltip("The copy gets the unsaved changes. This preset keeps its saved version.");
    ImGui::EndDisabled();
    if (!name.empty() && !valid) note("Use letters, digits, '-' and '_', and a name that no preset has.");
    if (!manage) return;
    // A click armed for another preset does not delete this one.
    if (std::exchange(styling.delete_preset, current.preset) != current.preset) styling.delete_click = {};
    const char* delete_label = styling.delete_click.armed(ImGui::GetTime()) ? "Click again to delete" : "Delete this preset";
    if (ImGui::Button(delete_label, ImVec2(half, 0)) && styling.delete_click.click(ImGui::GetTime()))
        send_console(menu, callbacks, "style preset delete " + current.preset);
    ImGui::SameLine();
    if (ImGui::Button("Open the presets folder", ImVec2(half, 0))) send_console(menu, callbacks, "style preset folder");
}
void record_button(Editing& e) {
    const bool solo = !e.model.multiplayer.active;
    ImGui::BeginDisabled(!solo);
    if (ImGui::Button("Record all my tricks")) send_console(e.menu, e.callbacks, "style editor record");
    ImGui::EndDisabled();
    if (!solo) warn(style::sentence(style::solo_recording_text).c_str());
    else if (const auto& to_record = e.model.style.to_record; to_record.any())
        note(style::record_text(to_record.count(), style::ms_per_trick).c_str());
}
void load_status(Editing& e) {
    using Phase = style::EditorLoad::Phase;
    const auto& load = e.model.style.editor_load;
    if (e.model.style.outdated_clips.test(e.trick_id)) {
        note(style::sentence(style::older_clip_text).c_str());
        record_button(e);
    }
    // The game has not seen the selection yet: a trick with a clip shows at once.
    if (load.trick != e.trick_id) {
        if (!e.model.style.clips.test(e.trick_id)) warn("Loading this trick...");
        return;
    }
    if (load.phase == Phase::unrecorded) {
        warn("This trick is not recorded yet.");
        record_button(e);
    } else if (load.phase == Phase::failed) {
        warn("This trick did not load.");
        note(style::sentence(load.reason).c_str());
        if (ImGui::Button("Try again")) send_console(e.menu, e.callbacks, "style editor show " + e.trick);
    } else if (load.phase == Phase::loading) {
        warn("Loading this trick...");
    }
}
// Panel sizes, and whether the editor's keys apply this frame.
struct ScreenLayout {
    float side{}, bottom{};
    bool keys{}, popup{};
};
// No keyboard navigation: Space and the arrow keys are the editor's own shortcuts, and must not also press the focused button.
constexpr auto fixed_window = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs;
// After the screen opens again, ask for the selected trick again. A close by another path can leave a prompt or a drag behind.
void forget_closed_screen(const CallbacksV3& callbacks) {
    const double now = ImGui::GetTime();
    if (now > styling.drawn_at + reopen_s) {
        styling.asked.clear();
        styling.leave.reset();
        styling.drag = TimelineDrag::none;
        // A drag that a long pause cut off still ends its undo group in the game.
        if (std::exchange(styling.grouping, false)) quiet(callbacks, "style group end");
        styling.popup_open = false;
        styling.hold.reset();
    }
    styling.drawn_at = now;
}
ScreenLayout screen_layout() {
    const auto& io = ImGui::GetIO();
    ScreenLayout layout{std::min(px(400), io.DisplaySize.x * 0.34f), px(176)};
    // The keyboard shortcuts wait while a popup is open, and in the frame after it closes on Escape.
    layout.popup = styling.popup_open || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    layout.keys = !io.WantTextInput && !layout.popup;
    return layout;
}
void side_panel(Editing& e, const ScreenLayout& layout) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(layout.side, ImGui::GetIO().DisplaySize.y - layout.bottom));
    if (ImGui::Begin("##style-editor-side", nullptr, fixed_window)) {
        ImGui::PushFont(e.menu.heading);
        ImGui::TextUnformatted("STYLE EDITOR");
        ImGui::PopFont();
        ImGui::TextDisabled("Preset");
        preset_controls(e.menu, e.model, e.callbacks, false);
        ImGui::Spacing();
        ImGui::TextDisabled("Trick");
        ImGui::SetNextItemWidth(-1);
        trick_picker(e);
        if (!e.standing_in) {
            if (e.now >= styling.closing_until && styling.asked != e.trick) {
                styling.asked = e.trick;
                send_console(e.menu, e.callbacks, "style editor show " + e.trick);
            }
            load_status(e);
        }
        if (styling.key) {
            section(e.menu, std::format("KEYFRAME {} OF {}", style::key_place(e.times, *styling.key) + 1, e.times.size()).c_str());
            blend_slider(e);
            ImGui::BeginChild("##style-editor-joints", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
            joints(e);
            ImGui::EndChild();
        } else if (e.standing_in) {
            note("This trick has no keyframes. Add one at the playhead.");
        }
    }
    ImGui::End();
}
void toggle_play(Editing& e) { send_now(e, e.replay.playing ? "style editor pause" : "style editor play"); }
void step_frames(Editing& e, int frames) { send_now(e, std::format("style editor step {}", frames)); }
void select_at(Editing& e, float at) {
    if (const auto key = style::key_at_time(e.times, at)) styling.key = key;
    e.hold(at);
}
void transport(Editing& e) {
    ImGui::BeginDisabled(!e.standing_in);
    if (ImGui::Button("|<")) e.hold(0);
    ImGui::SameLine();
    // Half a nudge: a playhead on a keyframe steps to its neighbour.
    if (ImGui::Button("< Key")) select_at(e, style::previous_key_time(e.times, e.playhead() - style::key_nudge / 2));
    ImGui::SameLine();
    if (ImGui::Button("< Frame")) step_frames(e, -1);
    ImGui::SameLine();
    if (ImGui::Button(e.replay.playing ? "Pause" : "Play", ImVec2(px(90), 0))) toggle_play(e);
    ImGui::SameLine();
    if (ImGui::Button("Frame >")) step_frames(e, 1);
    ImGui::SameLine();
    if (ImGui::Button("Key >")) select_at(e, style::next_key_time(e.times, e.playhead() + style::key_nudge / 2));
    ImGui::SameLine();
    if (ImGui::Button(">|")) e.hold(style::trick_end);
    ImGui::SameLine();
    constexpr std::array<std::pair<const char*, float>, 4> speeds{{{"1x", 1.0f}, {"3/4x", 0.75f}, {"1/2x", 0.5f}, {"1/4x", 0.25f}}};
    styling.speed = std::clamp(styling.speed, 0, static_cast<int>(speeds.size()) - 1);
    ImGui::SetNextItemWidth(px(84));
    if (ImGui::BeginCombo("##style-speed", speeds[static_cast<std::size_t>(styling.speed)].first)) {
        for (int i = 0; i < static_cast<int>(speeds.size()); ++i)
            if (ImGui::Selectable(speeds[static_cast<std::size_t>(i)].first, i == styling.speed)) {
                styling.speed = i;
                send_now(e, std::format("style editor speed {}", speeds[static_cast<std::size_t>(i)].second));
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed");
    ImGui::EndDisabled();
}
void keyframe_buttons(Editing& e) {
    ImGui::BeginDisabled(e.times.size() >= style::max_keys);
    if (ImGui::Button("Add keyframe")) add_keyframe(e);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!styling.key);
    if (ImGui::Button("Delete keyframe")) delete_keyframe(e);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset trick")) send_console(e.menu, e.callbacks, "style clear " + e.trick);
}
// Not during a drag: undo would split the drag's step and could change the dragged keyframe.
bool can_undo_or_redo(const std::string& name) { return !name.empty() && !styling.grouping; }
void history_button(Editing& e, const char* label, const std::string& name, const char* keys, const char* command) {
    ImGui::BeginDisabled(!can_undo_or_redo(name));
    if (ImGui::Button(label)) undo_or_redo(e, command);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", (name.empty() ? "Nothing to " + console::lower(label) : std::format("{}: {}  ({})", label, name, keys)).c_str());
    ImGui::EndDisabled();
}
void undo_redo(Editing& e) {
    history_button(e, "Undo", e.model.style.undo_name, "Ctrl+Z", "style undo");
    ImGui::SameLine();
    history_button(e, "Redo", e.model.style.redo_name, "Ctrl+Y or Ctrl+Shift+Z", "style redo");
}
void save_and_close(Editing& e) {
    const auto& look = ImGui::GetStyle();
    const float right = save_row_width(e.model) + look.ItemSpacing.x + button_width("Close");
    ImGui::SameLine(ImGui::GetWindowWidth() - right - look.WindowPadding.x);
    save_row(e.menu, e.model, e.callbacks);
    ImGui::SameLine();
    if (ImGui::Button("Close")) styling.leave = Leave::close;
}
void footer(const Model& model) {
    const auto saving = !model.style.save_issue.empty() ? "Not saved: " + model.style.save_issue
                        : model.style.unsaved            ? std::string("Unsaved changes")
                        : model.style.saved              ? std::string("Saved")
                                                         : std::string("Saving...");
    ImGui::TextDisabled("Drag: turn the camera.   Right drag: raise or lower the view.   Wheel: zoom (Shift: fine).   "
                        "Space: play / pause   Left / Right: frame   %s",
                        saving.c_str());
}
void shortcuts(Editing& e, bool keys) {
    if (!keys) return;
    if (e.standing_in) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggle_play(e);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) step_frames(e, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) step_frames(e, 1);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z) && can_undo_or_redo(e.model.style.undo_name)) undo_or_redo(e, "style undo");
    const bool redo =
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
    if (redo && can_undo_or_redo(e.model.style.redo_name)) undo_or_redo(e, "style redo");
    // The model can lag behind an edit that is still queued, so Ctrl+S always asks for a save.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && !e.model.style.auto_save) send_now(e, "style save");
}
void bottom_bar(Editing& e, const ScreenLayout& layout) {
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - layout.bottom));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, layout.bottom));
    if (ImGui::Begin("##style-editor-timeline", nullptr, fixed_window | ImGuiWindowFlags_NoScrollbar)) {
        transport(e);
        ImGui::SameLine(0, px(28));
        keyframe_buttons(e);
        ImGui::SameLine(0, px(28));
        undo_redo(e);
        save_and_close(e);
        timeline(e, px(52));
        footer(e.model);
        shortcuts(e, layout.keys);
    }
    ImGui::End();
}
void camera_drag(const ScreenLayout& layout) {
    if (layout.popup || styling.leave || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive()) return;
    const auto& io = ImGui::GetIO();
    const bool lifting = ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0);
    const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) || lifting;
    const float zoom = -io.MouseWheel * (io.KeyShift ? 0.1f : 0.35f);
    if (dragging) styling.orbit[0] -= io.MouseDelta.x * 0.008f;
    if (lifting) styling.orbit[3] += io.MouseDelta.y * 0.004f;
    else if (dragging) styling.orbit[1] += io.MouseDelta.y * 0.006f;
    styling.orbit[2] += zoom;
}
} // namespace

void set_style_playhead_feed(StylePlayheadFeed feed) noexcept { playhead_feed.store(feed); }
bool style_editor_wanted() noexcept {
    const auto feed = playhead_feed.load();
    return feed && feed().wanted;
}
void draw_style_editor(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks, bool exit_requested) {
    forget_closed_screen(callbacks);
    const MenuLook look(menu, model.menu_scale, ImVec2(14, 12));
    auto e = begin_editing(menu, model, callbacks);
    const auto layout = screen_layout();
    side_panel(e, layout);
    bottom_bar(e, layout);
    camera_drag(layout);
    send_controls(e, false);
    if (!styling.leave && (exit_requested || (layout.keys && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))) styling.leave = Leave::close;
    leave_prompt(e);
    styling.popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
}

namespace menu {
void style_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& current = model.style;
    ImGui::BeginChild("style-body", ImVec2(0, page_body_height(menu)));

    begin_card(menu, "style", "YOUR STYLE");
    bool enabled = current.enabled;
    if (toggle_row(menu, "Style", "Add your own body movement to the game's trick animations.", enabled, current.available, "Unavailable"))
        send_console(menu, callbacks, enabled ? "style 1" : "style 0");
    bool share = current.share;
    if (toggle_row(menu, "Show to other players", "Off: only you see your style.", share))
        send_console(menu, callbacks, share ? "style share 1" : "style share 0");
    note("Flip tricks for now. Grinds, grabs, manuals and pushing are coming soon.");
    end_card();

    begin_card(menu, "style-presets", "PRESETS", "A preset holds the style of every trick you edit in it.");
    preset_controls(menu, model, callbacks, true);
    {
        std::vector<std::uint8_t> tricks;
        for (const auto& rotation : current.rotations)
            if (rotation.target.trick && std::ranges::find(tricks, rotation.target.id) == tricks.end())
                tricks.push_back(rotation.target.id);
        info(menu, "Tricks styled", std::to_string(tricks.size()));
        info(menu, "Saved", current.saved ? "Yes" : "Not yet");
    }
    note("Each preset is one file in the presets folder. To share a preset, send that file. It works the same on every computer.");
    end_card();

    begin_card(menu, "style-editor", "EDITOR", "Edit each trick of the preset on a timeline.");
    if (primary_button(menu, "Open the style editor", style_editor_openable(model)))
        send_console(menu, callbacks, "style editor open");
    if (const auto blocked = style_editor_blocked_reason(model); !blocked.empty()) warn(blocked.c_str());
    else note("The editor records your tricks first, once: your skater does each trick on a practice stage.");
    end_card();
    ImGui::EndChild();
}
} // namespace menu
} // namespace dingosdk::overlay
