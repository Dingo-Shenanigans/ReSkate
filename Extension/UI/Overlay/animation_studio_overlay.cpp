#include "overlay_internal.h"
#include "skate_style.h"
#include "animation_studio_images.h"
#include "Extension/AnimationStudio/anim_studio.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// ReSkate Animator Studio's screen: the skater in the middle on the studio's own camera, a timeline of
// the body's joints along the bottom, the saved animations on the left and the chosen joint on the
// right. Drawn over the game while the studio is open (F8, or L3 + R3 held on a controller); the game
// gets no input meanwhile. Everything works from the keyboard and mouse or from a controller: the
// prompts follow whichever was used last.

using namespace dingosdk::overlay::detail;
namespace studio = dingosdk::anim_studio;

namespace dingosdk::overlay::detail {
namespace {
// skate.'s look: rough-edged dark tiles, white type, grey labels, the game's blue for what is chosen.
constexpr ImU32 panel_fill = IM_COL32(16, 17, 19, 228);
constexpr ImU32 panel_fill_solid = IM_COL32(18, 19, 21, 246);
constexpr ImU32 row_fill = IM_COL32(44, 45, 48, 222);
constexpr ImU32 row_fill_alt = IM_COL32(37, 38, 41, 222);
constexpr ImU32 row_selected = IM_COL32(62, 66, 74, 236);
constexpr ImU32 header_fill = IM_COL32(9, 9, 10, 244);
constexpr ImU32 white = skate_theme::white;
constexpr ImU32 soft = IM_COL32(168, 170, 176, 255);
constexpr ImU32 faint = IM_COL32(112, 114, 120, 255);
constexpr ImU32 blue = skate_theme::blue;
constexpr ImU32 blue_dim = IM_COL32(1, 131, 255, 110);
constexpr ImU32 blue_light = IM_COL32(90, 180, 255, 255);
constexpr float fps = 30.0f;

// XInput button bits (read_gamepad keeps them for PlayStation pads too).
constexpr unsigned pad_up = 0x0001, pad_down = 0x0002, pad_left = 0x0004, pad_right = 0x0008, pad_menu = 0x0010,
                   pad_view = 0x0020, pad_l3 = 0x0040, pad_r3 = 0x0080, pad_lb = 0x0100, pad_rb = 0x0200, pad_a = 0x1000,
                   pad_b = 0x2000, pad_x = 0x4000, pad_y = 0x8000;

struct Ui {
    std::uint16_t selected{studio::hips_joint};
    bool library_open{true}, inspector_open{true};
    float timeline_zoom{1}, timeline_scroll{}, rows_scroll{};
    bool dragging_playhead{}, dragging_gizmo{}, orbiting{}, panning{};
    // A key being dragged along its row.
    std::optional<std::uint16_t> key_joint;
    float key_from{}, key_to{};
    bool key_moved{};
    // Keys chosen together (Ctrl + A, Shift + click): dragged, moved and deleted as one.
    std::vector<std::pair<std::uint16_t, float>> picked;
    // The joint's state when a turn (mouse drag or stick) began, and how far it has gone since.
    studio::Offset drag_offset;
    studio::Quat drag_world{0, 0, 0, 1}, drag_parent{0, 0, 0, 1};
    float drag_parent_scale{1};
    ImVec2 drag_start{};
    int axis{-1}; // -1 free, 0 X, 1 Y, 2 Z
    bool local_axes{true}, move_mode{};
    bool limb_drag{}; // the mouse drags the selected limb to where it is pointed (I toggles it)
    std::array<char, 64> name{}, trick{};
    std::uint64_t names_for{};
    std::string message;
    double message_until{};
    std::string confirm_delete;
    // Controller.
    bool pad_mode{}, playstation{};
    unsigned buttons{};
    double held_since[16]{};
    double repeat_at[16]{};
    bool l3_combo{}, stick_turning{}, stick_zoom{}, scrubbing{};
    // The sticks, filtered over time: a light touch and a hard push both settle, with no jump at the dead zone's edge.
    float filtered[4]{};
    float scrub_time{};
    bool combo_armed{true}; // false while the hold that opened the studio is still on
    float stick_dx{}, stick_dy{};
    double stick_idle_since{};
    bool menu_open{}; // kept false: the menu is the list itself now
    int menu_index{};
    int tab{0}, row{1};
    bool menu_hidden{};
    int warning_choice{};
    bool zoom_to_playhead{};
    std::array<float, 5> list_scroll{};
    std::array<int, 5> list_followed{-1, -1, -1, -1, -1}; // the row the list last scrolled to, per tab
    std::string open_trick; // TRICKS: the trick whose pops are shown ("" = the list of tricks)
    int trick_row{1};       // its row in the list of tricks, to come back to
    ImVec2 last_mouse{};
};
Ui& ui() {
    static auto* u = new Ui;
    return *u;
}
void say(std::string text) {
    ui().message = std::move(text);
    ui().message_until = ImGui::GetTime() + 4.0;
}

const studio::JointView* joint_view(const studio::View& v, std::uint16_t joint) {
    for (const auto& j : v.joints) if (j.joint == joint) return &j;
    return nullptr;
}
const studio::TrackView* track_view(const studio::View& v, std::uint16_t joint) {
    for (const auto& t : v.tracks) if (t.joint == joint) return &t;
    return nullptr;
}
bool is_picked(std::uint16_t joint, float time) {
    for (const auto& [j, t] : ui().picked)
        if (j == joint && std::abs(t - time) < 1e-4f) return true;
    return false;
}
// Every key of every track that is not locked.
void pick_all(const studio::View& v) {
    auto& u = ui();
    u.picked.clear();
    for (const auto& t : v.tracks)
        if (!t.locked)
            for (const auto& key : t.keys) u.picked.emplace_back(t.joint, key.time);
    say(u.picked.empty() ? "There are no keys yet." : std::to_string(u.picked.size()) + " keys chosen: drag one to move them all, Delete takes them off.");
}
// The chosen keys moved by `by` seconds, as one undo step: the far end first, so none lands on another.
void move_picked(float by) {
    auto& u = ui();
    if (std::abs(by) < 1e-4f || u.picked.empty()) return;
    auto keys = u.picked;
    std::sort(keys.begin(), keys.end(), [by](const auto& a, const auto& b) { return by > 0 ? a.second > b.second : a.second < b.second; });
    studio::begin_change();
    for (auto& [joint, time] : keys) {
        const float to = std::max(0.0f, time + by);
        studio::move_key(joint, time, to);
        time = to;
    }
    u.picked = keys;
}
void delete_picked() {
    auto& u = ui();
    if (u.picked.empty()) return;
    studio::begin_change();
    for (const auto& [joint, time] : u.picked) studio::delete_key(joint, time);
    say(std::to_string(u.picked.size()) + " keys taken off.");
    u.picked.clear();
}
std::string label_of(std::uint16_t joint) {
    if (const auto* e = studio::editable(joint)) return std::string(e->label);
    return "Joint " + std::to_string(joint);
}
std::string upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
std::string title_of(const studio::View& v) {
    return upper(v.name.empty() ? (v.trick.empty() ? "Untitled" : v.trick) : v.name);
}
const studio::KeyView* key_here(const studio::View& v, std::uint16_t joint) {
    if (const auto* t = track_view(v, joint))
        for (const auto& k : t->keys)
            if (std::abs(k.time - v.time) < 0.5f / fps) return &k;
    return nullptr;
}

// The studio camera's projection, as the nametags place a world point (nametag_overlay.cpp).
struct Projector {
    studio::Vec3 right{}, up{}, back{}, origin{};
    float focal{}, cx{}, cy{};
    bool valid{};
    std::optional<ImVec2> operator()(const studio::Vec3& p) const {
        if (!valid) return std::nullopt;
        const studio::Vec3 d{p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
        const float depth = -(d[0] * back[0] + d[1] * back[1] + d[2] * back[2]);
        if (depth < 0.05f) return std::nullopt;
        const float side = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        const float height = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        return ImVec2(cx + side * focal / depth, cy - height * focal / depth);
    }
};
Projector projector(const studio::View& v, ImVec2 display) {
    Projector p;
    if (!v.camera_valid || !(v.fov > 1 && v.fov < 175)) return p;
    const auto& m = v.camera;
    p.right = {m[0], m[1], m[2]};
    p.up = {m[4], m[5], m[6]};
    p.back = {m[8], m[9], m[10]};
    p.origin = {m[12], m[13], m[14]};
    p.focal = display.y / (2.0f * std::tan(v.fov * 3.14159265f / 360.0f));
    p.cx = display.x * 0.5f;
    p.cy = display.y * 0.5f;
    p.valid = true;
    return p;
}

// ---- drawing ---------------------------------------------------------------------------------------
struct Fonts {
    ImFont *body, *bold, *heading, *title;
    float k;
};
ImVec2 text_size(ImFont* font, float size, const char* text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text); }
void text(ImDrawList* d, ImFont* font, float size, ImVec2 at, ImU32 colour, const std::string& value) {
    d->AddText(font, size, at, colour, value.c_str());
}
unsigned seed_of(ImVec2 a) { return static_cast<unsigned>(a.x * 7 + a.y * 13); }
// A tile with skate.'s rough, hand-cut edge and a little screen-print in its corner.
void tile(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 colour, float k, bool print = true) {
    skate_theme::rough_rect(d, a, b, colour, seed_of(a), k);
    if (print && b.x - a.x > 120 * k && b.y - a.y > 60 * k)
        theme::halftone(d, ImVec2(b.x - 58 * k, a.y + 8 * k), 8, 3, IM_COL32(255, 255, 255, 16));
}
// A flat button: rough tile, white type, blue when on. Returns whether it was clicked.
bool button(ImDrawList* d, const Fonts& f, const char* id, ImVec2 at, ImVec2 size, const std::string& label, bool on = false,
            bool enabled = true) {
    ImGui::SetCursorScreenPos(at);
    const bool clicked = ImGui::InvisibleButton(id, size) && enabled;
    const bool hovered = ImGui::IsItemHovered() && enabled;
    const ImU32 fill = on ? blue : hovered ? IM_COL32(64, 66, 72, 240) : IM_COL32(34, 35, 38, 240);
    skate_theme::rough_rect(d, at, ImVec2(at.x + size.x, at.y + size.y), fill, seed_of(at), f.k * 0.7f);
    const float fs = 14 * f.k;
    const auto t = text_size(f.bold, fs, label.c_str());
    d->AddText(f.bold, fs, ImVec2(at.x + (size.x - t.x) * 0.5f, at.y + (size.y - t.y) * 0.5f), enabled ? white : faint, label.c_str());
    return clicked;
}
void diamond(ImDrawList* d, ImVec2 c, float r, ImU32 colour) {
    d->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), colour);
}
// The box on the chosen joint, glowing the game's blue.
// The chosen joint: a small blue ring around it, softly glowing.
void glow_box(ImDrawList* d, ImVec2 c, float half, float k) {
    (void)half;
    const float r = 11 * k;
    const float pulse = 0.8f + 0.2f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.2f);
    for (int i = 3; i >= 1; --i)
        d->AddCircle(c, r + i * 2.2f * k, IM_COL32(1, 131, 255, static_cast<int>(40 * (4 - i) * pulse)), 32, 2.4f * k);
    d->AddCircle(c, r, IM_COL32(60, 165, 255, 255), 32, 2.6f * k);
    d->AddCircleFilled(c, 3 * k, IM_COL32(230, 240, 255, 255), 12);
}

// Controller glyphs, drawn: Xbox letters or PlayStation shapes, all in white like the game's prompts.
enum class Glyph { a, b, x, y, lb, rb, lt, rt, ls, rs, l3, r3, dpad_ud, dpad_lr, menu, view };
float glyph(ImDrawList* d, const Fonts& f, ImVec2 at, Glyph g, bool playstation) {
    const float k = f.k, r = 11 * k;
    const ImVec2 c(at.x + r, at.y + r);
    const auto label = [&](const char* s, float w) {
        const float fs = 11 * k;
        const auto t = text_size(f.bold, fs, s);
        d->AddRectFilled(ImVec2(at.x, at.y + 2 * k), ImVec2(at.x + w, at.y + 2 * r - 2 * k), white, 5 * k);
        d->AddText(f.bold, fs, ImVec2(at.x + (w - t.x) * 0.5f, c.y - t.y * 0.5f), skate_theme::black, s);
        return w;
    };
    switch (g) {
    case Glyph::a: case Glyph::b: case Glyph::x: case Glyph::y: {
        d->AddCircleFilled(c, r, white);
        if (!playstation) {
            const char* s = g == Glyph::a ? "A" : g == Glyph::b ? "B" : g == Glyph::x ? "X" : "Y";
            const auto t = text_size(f.bold, 14 * k, s);
            d->AddText(f.bold, 14 * k, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), skate_theme::black, s);
        } else {
            const float s = r * 0.48f, w = 2.2f * k;
            const ImU32 ink = skate_theme::black;
            if (g == Glyph::a) {
                d->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), ink, w);
                d->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), ink, w);
            } else if (g == Glyph::b) d->AddCircle(c, s, ink, 0, w);
            else if (g == Glyph::x) d->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), ink, 0, 0, w);
            else d->AddTriangle(ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y + s * 0.75f), ImVec2(c.x - s, c.y + s * 0.75f), ink, w);
        }
        return 2 * r;
    }
    case Glyph::lb: return label(playstation ? "L1" : "LB", 30 * k);
    case Glyph::rb: return label(playstation ? "R1" : "RB", 30 * k);
    case Glyph::lt: return label(playstation ? "L2" : "LT", 30 * k);
    case Glyph::rt: return label(playstation ? "R2" : "RT", 30 * k);
    case Glyph::l3: return label("L3", 30 * k);
    case Glyph::r3: return label("R3", 30 * k);
    case Glyph::menu: return label(playstation ? "OPT" : "MENU", 40 * k);
    case Glyph::view: return label(playstation ? "SHARE" : "VIEW", 44 * k);
    case Glyph::ls: case Glyph::rs: {
        d->AddCircle(c, r, white, 0, 2 * k);
        const char* s = g == Glyph::ls ? "L" : "R";
        const auto t = text_size(f.bold, 12 * k, s);
        d->AddText(f.bold, 12 * k, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), white, s);
        return 2 * r;
    }
    case Glyph::dpad_ud: case Glyph::dpad_lr: {
        const float a = r * 0.9f, t = r * 0.34f;
        d->AddRectFilled(ImVec2(c.x - t, c.y - a), ImVec2(c.x + t, c.y + a), g == Glyph::dpad_ud ? white : IM_COL32(255, 255, 255, 90));
        d->AddRectFilled(ImVec2(c.x - a, c.y - t), ImVec2(c.x + a, c.y + t), g == Glyph::dpad_lr ? white : IM_COL32(255, 255, 255, 90));
        return 2 * r;
    }
    }
    return 0;
}
// One prompt: the glyph (or a key cap) and what it does. Returns the width taken.
float prompt(ImDrawList* d, const Fonts& f, ImVec2 at, std::optional<Glyph> g, const char* key, const char* what, bool playstation) {
    float w;
    if (g) w = glyph(d, f, at, *g, playstation);
    else {
        const auto cap = text_size(f.bold, 11 * f.k, key);
        d->AddRectFilled(ImVec2(at.x, at.y + 2 * f.k), ImVec2(at.x + cap.x + 12 * f.k, at.y + 20 * f.k), white, 3 * f.k);
        d->AddText(f.bold, 11 * f.k, ImVec2(at.x + 6 * f.k, at.y + 11 * f.k - cap.y * 0.5f), skate_theme::black, key);
        w = cap.x + 12 * f.k;
    }
    const float fs = 13 * f.k;
    const auto t = text_size(f.bold, fs, what);
    d->AddText(f.bold, fs, ImVec2(at.x + w + 7 * f.k, at.y + 11 * f.k - t.y * 0.5f), white, what);
    return w + 7 * f.k + t.x + 20 * f.k;
}

float glyph_width(const Fonts& f, Glyph g) {
    switch (g) {
    case Glyph::lb: case Glyph::rb: case Glyph::lt: case Glyph::rt: case Glyph::l3: case Glyph::r3: return 30 * f.k;
    case Glyph::menu: return 40 * f.k;
    case Glyph::view: return 44 * f.k;
    default: return 22 * f.k;
    }
}
float prompt_width(const Fonts& f, std::optional<Glyph> g, const char* key, const char* what) {
    const float w = g ? glyph_width(f, *g) : text_size(f.bold, 11 * f.k, key).x + 12 * f.k;
    return w + 7 * f.k + text_size(f.bold, 13 * f.k, what).x + 20 * f.k;
}

// A labelled number that drags left and right and takes typing on double click.
bool number(const Fonts& f, const char* id, const char* label, float& value, float speed, const char* format, float width) {
    ImGui::BeginGroup();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(34, 35, 38, 240));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(56, 58, 64, 240));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 84, 170, 240));
    ImGui::PushStyleColor(ImGuiCol_Text, white);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2 * f.k);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * f.k, 4 * f.k));
    ImGui::TextColored(ImColor(soft), "%s", label);
    ImGui::SameLine(26 * f.k);
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::DragFloat("##v", &value, speed, 0, 0, format);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    ImGui::EndGroup();
    return changed;
}
bool text_field(const Fonts& f, const char* id, std::array<char, 64>& buffer, float width) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(34, 35, 38, 240));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(46, 47, 52, 240));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(46, 47, 52, 240));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, blue_dim);
    ImGui::PushStyleColor(ImGuiCol_Text, white);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2 * f.k);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * f.k, 6 * f.k));
    ImGui::SetNextItemWidth(width);
    const bool done = ImGui::InputText(id, buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    const bool left = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    return done || left;
}

// The stick's response: a dead zone, then a curve that starts gently and reaches full speed at the rim (so a
// small push is fine control and a full push is fast, with no step at the dead zone's edge).
float stick_response(float raw) {
    constexpr float dead = 0.14f;
    const float a = std::abs(raw);
    if (a <= dead) return 0.0f;
    const float n = std::min((a - dead) / (1.0f - dead), 1.0f);
    return std::copysign(n * n, raw);
}
// The stick filtered towards its target over the frame time (about 1/20 s): removes the jitter of the pad.
float smooth_stick(float& state, float target, float dt) {
    state += (target - state) * (1.0f - std::exp(-dt * 20.0f));
    return std::abs(state) < 0.002f ? 0.0f : state;
}

// ---- turning the chosen joint (mouse drag or left stick) ---------------------------------------------
void begin_turn(const studio::View& v) {
    auto& u = ui();
    const auto* sel = joint_view(v, u.selected);
    if (!sel) return;
    u.drag_offset = studio::offset_of(u.selected);
    u.drag_world = sel->rotation;
    u.drag_parent = sel->parent_rotation;
    u.drag_parent_scale = sel->parent_scale;
    studio::begin_change();
    studio::set_playing(false);
}
// Drags the selected limb: the point under the mouse, on the camera's plane through the joint, is where it goes.
void drag_limb_to(const studio::View& v, const Projector& p, ImVec2 mouse) {
    const auto* sel = joint_view(v, ui().selected);
    if (!sel || !p.valid) return;
    const studio::Vec3 d{sel->position[0] - p.origin[0], sel->position[1] - p.origin[1], sel->position[2] - p.origin[2]};
    const float depth = -(d[0] * p.back[0] + d[1] * p.back[1] + d[2] * p.back[2]);
    if (depth < 0.05f || p.focal <= 0) return;
    const float side = (mouse.x - p.cx) * depth / p.focal, height = -(mouse.y - p.cy) * depth / p.focal;
    studio::Vec3 target{};
    for (int i = 0; i < 3; ++i) target[i] = p.origin[i] - p.back[i] * depth + p.right[i] * side + p.up[i] * height;
    studio::drag_limb(ui().selected, target);
}
// `dx`, `dy`: how far the turn has gone, in screen pixels at 1080 lines.
void apply_turn(const Projector& p, float dx, float dy, bool move) {
    auto& u = ui();
    const auto* e = studio::editable(u.selected);
    studio::Offset next = u.drag_offset;
    const float radians = 0.0085f;
    if (move && e && e->moves) {
        const float metres = 1.0f / 400;
        studio::Vec3 world{};
        for (int i = 0; i < 3; ++i) world[i] = p.right[i] * dx * metres - p.up[i] * dy * metres;
        if (u.axis >= 0) for (int i = 0; i < 3; ++i) if (i != u.axis) world[i] = 0;
        const auto local = studio::rotate(studio::conjugate(u.drag_parent), world);
        for (int i = 0; i < 3; ++i) next.offset[i] += local[i] / std::max(u.drag_parent_scale, 0.01f);
    } else {
        studio::Quat turn;
        if (u.axis >= 0) {
            studio::Vec3 axis{};
            axis[static_cast<std::size_t>(u.axis)] = 1;
            const auto q = studio::axis_angle(axis, (dx - dy) * radians);
            turn = u.local_axes ? q : studio::multiply(studio::multiply(studio::conjugate(u.drag_world), q), u.drag_world);
        } else {
            const auto world = studio::multiply(studio::axis_angle(p.up, dx * radians), studio::axis_angle(p.right, dy * radians));
            turn = studio::multiply(studio::multiply(studio::conjugate(u.drag_world), world), u.drag_world);
        }
        next.rotation = studio::normalised(studio::multiply(u.drag_offset.rotation, turn));
        if (u.selected == studio::board_joint) {
            // The board's keys turn it about the ground's axes (drag_parent is the ground's turn).
            studio::Quat ground_turn;
            if (u.axis >= 0) {
                studio::Vec3 axis{};
                axis[static_cast<std::size_t>(u.axis)] = 1;
                ground_turn = studio::axis_angle(axis, (dx - dy) * radians);
            } else {
                const auto world = studio::multiply(studio::axis_angle(p.up, dx * radians), studio::axis_angle(p.right, dy * radians));
                ground_turn = studio::multiply(studio::multiply(studio::conjugate(u.drag_parent), world), u.drag_parent);
            }
            next.rotation = studio::normalised(studio::multiply(ground_turn, u.drag_offset.rotation));
        }
    }
    studio::set_offset(u.selected, next);
}

// ---- actions shared by keys, buttons and the controller ---------------------------------------------
void key_joint() {
    studio::begin_change();
    studio::set_offset(ui().selected, studio::offset_of(ui().selected));
}
void delete_key(const studio::View& v) {
    if (const auto* k = key_here(v, ui().selected)) {
        studio::begin_change();
        studio::delete_key(ui().selected, k->time);
    }
}
void step_frame(const studio::View& v, int frames) { studio::set_time(std::round(v.time * fps + frames) / fps); }
// The playhead to the chosen joint's previous or next key.
void jump_key(const studio::View& v, int direction) {
    const auto* t = track_view(v, ui().selected);
    if (!t || t->keys.empty()) return;
    std::optional<float> best;
    for (const auto& k : t->keys) {
        if (direction > 0 && k.time > v.time + 0.5f / fps && (!best || k.time < *best)) best = k.time;
        if (direction < 0 && k.time < v.time - 0.5f / fps && (!best || k.time > *best)) best = k.time;
    }
    if (best) studio::set_time(*best);
}
void select_row(int delta) {
    auto& u = ui();
    const auto joints = studio::editable_joints();
    int index = 0;
    for (std::size_t i = 0; i < joints.size(); ++i) if (joints[i].index == u.selected) index = static_cast<int>(i);
    index = std::clamp(index + delta, 0, static_cast<int>(joints.size()) - 1);
    u.selected = joints[static_cast<std::size_t>(index)].index;
}
void save_now() {
    std::string m;
    studio::save(m);
    say(m);
}

// ---- the menu: skate.'s own list menu (Trick List), with tabs ---------------------------------------
enum class Tab { tricks, animations, joints, keys, playback };
constexpr std::array<const char*, 5> tab_names{"TRICKS", "SAVED", "JOINTS", "KEYS", "PLAYBACK"};
struct Row {
    std::string label, value, about;
    std::function<void()> act;
    bool enabled{true}, on{};
    std::string library; // a saved animation's name (Delete takes it out)
};
std::vector<Row> rows_of(Tab tab, const studio::View& v) {
    auto& u = ui();
    std::vector<Row> rows;
    switch (tab) {
    case Tab::tricks: {
        static constexpr const char* record_about =
            "Skate and land the trick you want: you are back in the studio with it, the board where the game had it. Esc: back without one.";
        // The game's tricks by category, each once; choosing one shows its pops (low, mid, high).
        const auto tricks = studio::game_tricks();
        const auto& list = *tricks;
        if (list.empty()) {
            rows.push_back({"RECORD", "", "", {}, false});
            rows.push_back({"Record a trick", "", record_about, [] { studio::record_next_trick(); }});
            break;
        }
        if (!u.open_trick.empty()) {
            std::string label;
            for (const auto& t : list) if (t.trick == u.open_trick) label = t.label;
            rows.push_back({"< Back", "", "Back to the list of tricks.", [] {
                                auto& ui_state = ui();
                                ui_state.open_trick.clear();
                                ui_state.row = ui_state.trick_row;
                            }});
            rows.push_back({upper(label), "", "", {}, false});
            for (const auto& t : list) {
                if (t.trick != u.open_trick) continue;
                const std::string pop = t.pop == "low" ? "Low pop" : t.pop == "mid" ? "Mid pop" : "High pop";
                Row row{pop, "", "Opens the game's " + t.label + " with a " + t.pop + " pop to change it. Save it and it plays on your " + t.label + "s.",
                        [t] {
                            std::string m;
                            studio::load_game_trick(t, m);
                            say(m);
                            ui().names_for = 0;
                        }};
                row.on = v.trick == t.trick && v.pop == t.pop;
                rows.push_back(std::move(row));
            }
            break;
        }
        // Your own trick: you do it, and it comes into the studio with the board as the game drew it.
        rows.push_back({"RECORD", "", "", {}, false});
        rows.push_back({"Record a trick", "", record_about, [] { studio::record_next_trick(); }});
        // The game's own clips stay in the build but are not listed for now: their board is not where the
        // game draws it in a flip (the game places it with its physics), so a recording is the way in.
        constexpr bool show_game_clips = false;
        if (!show_game_clips) break;
        std::string category, trick;
        for (const auto& t : list) {
            if (t.category != category) {
                category = t.category;
                rows.push_back({upper(category), "", "", {}, false});
            }
            if (t.trick == trick) continue; // each trick once
            trick = t.trick;
            const int index = static_cast<int>(rows.size());
            Row row{t.label, ">", "Choose the pop next: low, mid or high.", [name = t.trick, index] {
                        auto& ui_state = ui();
                        ui_state.open_trick = name;
                        ui_state.trick_row = index;
                        ui_state.row = 2; // the first pop, under the trick's heading
                    }};
            row.on = v.trick == t.trick;
            rows.push_back(std::move(row));
        }
        break;
    }
    case Tab::animations:
        if (v.library.empty()) rows.push_back({"Nothing saved yet", "", "Pick a trick in TRICKS, change it and save it: it shows up here.", {}, false});
        for (const auto& name : v.library) {
            Row row{name, name == v.name ? "open" : "", "A saved animation: choose it to open it. Delete takes it out of the library (press it twice).",
                    [name] {
                        std::string m;
                        studio::load(name, m);
                        say(m);
                        ui().names_for = 0;
                    }};
            row.on = name == v.name;
            row.library = name;
            if (u.confirm_delete == name) row.value = "delete again";
            rows.push_back(std::move(row));
        }
        break;
    case Tab::joints:
        for (const auto& j : studio::editable_joints()) {
            const auto* t = track_view(v, j.index);
            const auto count = t ? t->keys.size() : 0;
            Row row{std::string(j.label), count ? std::to_string(count) + (count == 1 ? " key" : " keys") : "", "", [index = j.index] { ui().selected = index; }};
            if (t && t->hidden) row.value += "  off";
            if (t && t->locked) row.value += "  locked";
            rows.push_back(std::move(row));
        }
        break;
    case Tab::keys: {
        const auto* key = key_here(v, u.selected);
        const auto* track = track_view(v, u.selected);
        const bool locked = track && track->locked;
        const auto joint = upper(label_of(u.selected));
        rows.push_back({"Key this frame", joint, "Keys the joint as it is now on this frame.", [] { key_joint(); }, v.has_clip && !locked});
        rows.push_back({"Delete key", "", "Takes the joint's key on this frame off.", [v] { delete_key(v); }, key && !locked});
        rows.push_back({"Select all keys", u.picked.empty() ? "Ctrl A" : std::to_string(u.picked.size()),
                        "Chooses every key of every joint and the board: drag one to move them all, Delete takes them off. Shift + click adds or takes one key; Esc lets go.",
                        [v] { pick_all(v); }, v.has_clip});
        rows.push_back({"Delete chosen keys", "", "Takes every chosen key off.", [] { delete_picked(); }, !u.picked.empty()});
        rows.push_back({"Previous key", "", "The playhead to the joint's key before this frame.", [v] { jump_key(v, -1); }, track && !track->keys.empty()});
        rows.push_back({"Next key", "", "The playhead to the joint's key after this frame.", [v] { jump_key(v, 1); }, track && !track->keys.empty()});
        if (key) {
            const char* names[] = {"Smooth", "Linear", "Step"};
            const int ease = static_cast<int>(key->ease);
            const float time = key->time;
            rows.push_back({"Blend into the next key", names[ease],
                            "Smooth eases in and out, Linear goes straight, Step holds this key until the next.", [time, ease] {
                                studio::begin_change();
                                studio::set_ease(ui().selected, time, static_cast<studio::Ease>((ease + 1) % 3));
                            }, !locked});
        }
        rows.push_back({"Turn about", u.axis == 0 ? "X" : u.axis == 1 ? "Y" : u.axis == 2 ? "Z" : "any axis",
                        "Which axis a drag or the left stick turns the joint about.", [] { ui().axis = ui().axis >= 2 ? -1 : ui().axis + 1; }});
        rows.push_back({"Axes", u.local_axes ? "the joint's own" : "the world's", "X, Y and Z of the joint itself or of the world.",
                        [] { ui().local_axes = !ui().local_axes; }});
        rows.push_back({"Follow joint", "", "The camera keeps the joint in the middle (again: the whole skater).", [] { studio::focus_joint(ui().selected); }});
        rows.push_back({"Copy pose", "", "Remembers every joint's edit on this frame.", [] { studio::copy_pose(); }});
        rows.push_back({"Paste pose", "", "Keys the remembered pose on this frame.", [] {
                            studio::begin_change();
                            studio::paste_pose();
                        }, v.has_clip});
        rows.push_back({"Reset joint", joint, "Takes all of the joint's keys off.", [] {
                            studio::begin_change();
                            studio::reset_joint(ui().selected);
                        }, track && !locked});
        rows.push_back({"Hide joint's keys", track && track->hidden ? "hidden" : "", "Plays the trick as if the joint had no keys.",
                        [hidden = track && track->hidden, locked] { studio::set_track_flags(ui().selected, !hidden, locked); }});
        rows.push_back({"Lock joint", locked ? "locked" : "", "A locked joint's keys cannot be changed.",
                        [hidden = track && track->hidden, locked] { studio::set_track_flags(ui().selected, hidden, !locked); }});
        rows.push_back({"Undo", "", "", [] { studio::undo(); }, v.can_undo});
        rows.push_back({"Redo", "", "", [] { studio::redo(); }, v.can_redo});
        break;
    }
    case Tab::playback:
        rows.push_back({v.playing ? "Pause" : "Play", "", "Plays the trick with your keys.", [v] { studio::set_playing(!v.playing); }, v.has_clip});
        rows.push_back({"Loop", v.loop ? "on" : "off", "", [v] { studio::set_loop(!v.loop); }});
        rows.push_back({"Speed", std::format("{:.2f}x", v.speed), "How fast the trick plays, here and in game. Choose it to step through the speeds.", [v] {
                            const float speeds[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
                            float next = speeds[0];
                            for (const float s : speeds) if (s > v.speed + 0.01f) { next = s; break; }
                            studio::set_speed(next);
                        }});
        rows.push_back({"Custom tricks in game", v.play_in_game ? "on" : "off",
                        "On: when you do a trick you saved an animation for (its TRICK, as ollie or kickflip), yours plays instead of the game's.",
                        [v] { studio::set_play_in_game(!v.play_in_game); }});
        rows.push_back({"Studio room", v.room ? "on" : "off",
                        "On: the studio opens in the game's own lit studio room, as skate.'s Skaters page shows it. Off: it opens where you are. Takes effect the next time it opens.",
                        [v] { studio::set_room(!v.room); }});
        rows.push_back({"Pop", v.pop.empty() ? "any" : v.pop,
                        "Which pop this plays on: any, or only a low, mid or high pop (one of each can be saved for a trick). Choose it to step through.",
                        [v] { studio::set_pop(v.pop.empty() ? "low" : v.pop == "low" ? "mid" : v.pop == "mid" ? "high" : ""); }, v.has_clip});
        rows.push_back({"Copy to low, mid and high pop", "", "Saves this trick three times, one for each pop, to change each on its own. In game every pop is timed to its own air.",
                        [] {
                            std::string m;
                            studio::copy_to_pops(m);
                            say(m);
                        }, v.has_clip});
        rows.push_back({"Save", v.dirty ? "not saved" : "", "Saves the animation in the library.", [] { save_now(); }, v.has_clip});
        rows.push_back({"Close the studio", "", "Back to skating. The camera and the game's HUD come back.", [] { studio::request_close(); }});
        break;
    }
    return rows;
}

void zoom_timeline(float factor);
// ---- the controller --------------------------------------------------------------------------------
bool repeats(unsigned bit, unsigned buttons, unsigned pressed, double now) {
    auto& u = ui();
    const int i = std::countr_zero(bit);
    if (pressed & bit) {
        u.repeat_at[i] = now + 0.38;
        return true;
    }
    if ((buttons & bit) && now >= u.repeat_at[i]) {
        u.repeat_at[i] = now + 0.07;
        return true;
    }
    return false;
}
void switch_tab(int delta) {
    auto& u = ui();
    u.tab = (u.tab + delta + static_cast<int>(tab_names.size())) % static_cast<int>(tab_names.size());
    u.row = static_cast<Tab>(u.tab) == Tab::tricks ? (u.open_trick.empty() ? 1 : 2) : 0; // Record a trick, or the first pop
    u.confirm_delete.clear();
}
// Up / down in the list: in JOINTS the list is the joints themselves.
void move_row(const studio::View& v, int delta) {
    auto& u = ui();
    if (static_cast<Tab>(u.tab) == Tab::joints) {
        select_row(delta);
        return;
    }
    const auto rows = rows_of(static_cast<Tab>(u.tab), v);
    const int count = static_cast<int>(rows.size());
    // Headings (rows that do nothing) are stepped over.
    int at = u.row;
    for (int step = 0; step < count; ++step) {
        at = std::clamp(at + delta, 0, std::max(0, count - 1));
        if (rows[static_cast<std::size_t>(at)].act || at == 0 || at == count - 1) break;
    }
    if (count && !rows[static_cast<std::size_t>(at)].act) at = u.row; // only headings that way: stay
    u.row = at;
}
void activate_row(const studio::View& v) {
    auto& u = ui();
    if (static_cast<Tab>(u.tab) == Tab::joints) {
        key_joint();
        return;
    }
    auto rows = rows_of(static_cast<Tab>(u.tab), v);
    if (u.row >= 0 && u.row < static_cast<int>(rows.size()) && rows[static_cast<std::size_t>(u.row)].enabled && rows[static_cast<std::size_t>(u.row)].act)
        rows[static_cast<std::size_t>(u.row)].act();
}
void delete_row(const studio::View& v) {
    auto& u = ui();
    const auto rows = rows_of(static_cast<Tab>(u.tab), v);
    if (u.row < 0 || u.row >= static_cast<int>(rows.size())) return;
    const auto& name = rows[static_cast<std::size_t>(u.row)].library;
    if (name.empty()) return;
    if (u.confirm_delete == name) {
        std::string m;
        studio::remove(name, m);
        say(m);
        u.confirm_delete.clear();
    } else u.confirm_delete = name;
}
void controller(const studio::View& v, const Projector& p) {
    auto& u = ui();
    const auto pad = read_gamepad();
    if (!pad.valid) {
        u.buttons = 0;
        return;
    }
    u.playstation = pad.playstation;
    const double now = ImGui::GetTime();
    const unsigned buttons = pad.buttons, pressed = buttons & ~u.buttons, released = u.buttons & ~buttons;
    for (int i = 0; i < 16; ++i) if (pressed & (1u << i)) u.held_since[i] = now;
    const auto held_for = [&](unsigned bit) { return (buttons & bit) ? now - u.held_since[std::countr_zero(bit)] : 0.0; };
    const auto was_held = [&](unsigned bit) { return now - u.held_since[std::countr_zero(bit)]; };
    u.buttons = buttons;
    const bool sticks = std::abs(pad.left_x) > 0.35f || std::abs(pad.left_y) > 0.35f || std::abs(pad.right_x) > 0.35f ||
                        std::abs(pad.right_y) > 0.35f || pad.left_trigger > 0.4f || pad.right_trigger > 0.4f;
    if (pressed || sticks) u.pad_mode = true;
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);

    // L3 + R3 held: close the studio (the same hold opens it from the game).
    if ((buttons & pad_l3) && (buttons & pad_r3)) {
        u.l3_combo = true;
        if (u.combo_armed && held_for(pad_l3) > 0.5 && held_for(pad_r3) > 0.5) {
            studio::request_close();
            u.combo_armed = false;
        }
        return;
    }
    if (u.l3_combo) {
        if (!(buttons & (pad_l3 | pad_r3))) {
            u.l3_combo = false;
            u.combo_armed = true;
        }
        return;
    }
    // The close warning takes the buttons: A save and close, X close without saving, B back.
    if (v.confirm_close) return; // the close warning's card takes the buttons
    if (pressed & pad_lb) switch_tab(-1);
    if (pressed & pad_rb) switch_tab(1);
    if (pressed & pad_a) activate_row(v);
    if (pressed & pad_x) {
        if (static_cast<Tab>(u.tab) == Tab::animations) delete_row(v);
        else delete_key(v);
    }
    if (pressed & pad_y) studio::set_playing(!v.playing);
    if (pressed & pad_b) studio::undo();
    if (pressed & pad_view) {
        // The timeline: the whole trick, or zoomed in on the playhead.
        if (u.timeline_zoom > 1.01f) u.timeline_zoom = 1;
        else zoom_timeline(3);
    }
    if (pressed & pad_menu) save_now();
    // L3: turn about any axis, X, Y, Z, then move (the hips and the board).
    if ((released & pad_l3) && was_held(pad_l3) < 0.5) {
        if (u.move_mode) u.move_mode = false, u.axis = -1;
        else if (u.axis >= 2) u.move_mode = true, u.axis = -1;
        else ++u.axis;
    }
    // R3: the right stick orbits, or zooms (up / down) and pans (left / right).
    if ((released & pad_r3) && was_held(pad_r3) < 0.5) u.stick_zoom = !u.stick_zoom;
    if (repeats(pad_up, buttons, pressed, now)) move_row(v, -1);
    if (repeats(pad_down, buttons, pressed, now)) move_row(v, 1);
    // RT plays forward and LT backward, as fast as they are pressed (fully: real time), to watch it.
    if (const float scrub = pad.right_trigger - pad.left_trigger; std::abs(scrub) > 0.08f && v.has_clip) {
        const float sign = scrub > 0 ? 1.0f : -1.0f;
        const float amount = (std::abs(scrub) - 0.08f) / 0.92f;
        u.scrub_time = (u.scrubbing ? u.scrub_time : v.time) + sign * amount * amount * dt;
        u.scrub_time = std::clamp(u.scrub_time, 0.0f, v.duration);
        u.scrubbing = true;
        studio::set_playing(false);
        studio::set_time(u.scrub_time);
    } else if (u.scrubbing) {
        u.scrubbing = false;
        studio::set_time(std::round(v.time * fps) / fps); // lands on a frame
    }
    if (repeats(pad_left, buttons, pressed, now)) step_frame(v, -1);
    if (repeats(pad_right, buttons, pressed, now)) step_frame(v, 1);

    // Left stick turns the chosen joint (LT held: moves the hips); a turn is one undo step.
    const float lx = smooth_stick(u.filtered[0], stick_response(pad.left_x), dt), ly = smooth_stick(u.filtered[1], stick_response(pad.left_y), dt);
    if (lx != 0 || ly != 0) {
        const auto* t = track_view(v, u.selected);
        if (!u.stick_turning && v.has_clip && !(t && t->locked) && joint_view(v, u.selected)) {
            begin_turn(v);
            u.stick_turning = true;
            u.stick_dx = u.stick_dy = 0;
        }
        if (u.stick_turning && p.valid) {
            u.stick_dx += lx * 300.0f * dt;
            u.stick_dy -= ly * 300.0f * dt;
            apply_turn(p, u.stick_dx, u.stick_dy, u.move_mode);
        }
        u.stick_idle_since = now;
    } else if (u.stick_turning && now - u.stick_idle_since > 0.25) u.stick_turning = false;

    // Right stick orbits; with RT held it zooms (up / down) and pans (left / right).
    const float rx = smooth_stick(u.filtered[2], stick_response(pad.right_x), dt), ry = smooth_stick(u.filtered[3], stick_response(pad.right_y), dt);
    if (rx != 0 || ry != 0) {
        auto orbit = v.orbit;
        if (u.stick_zoom) {
            orbit.distance *= 1.0f - ry * 1.6f * dt;
            for (int i = 0; i < 3; ++i) orbit.pan[i] += p.right[i] * rx * orbit.distance * 0.5f * dt;
        } else {
            orbit.yaw -= rx * 2.2f * dt;
            orbit.pitch -= ry * 1.6f * dt;
        }
        studio::set_orbit(orbit);
    }
}

// ---- the parts of the screen --------------------------------------------------------------------
// The page title, brushed like the game's ("TRICK LIST"), with its spray of dots.
void page_title(ImDrawList* d, const Fonts& f, ImVec2 at) {
    const float k = f.k;
    theme::halftone(d, ImVec2(at.x - 18 * k, at.y + 6 * k), 3, 9, IM_COL32(255, 255, 255, 70));
    text(d, f.title, 74 * k, at, white, "ANIMATOR");
    const auto w = text_size(f.title, 74 * k, "ANIMATOR");
    text(d, f.title, 74 * k, ImVec2(at.x + w.x + 22 * k, at.y), white, "STUDIO");
    const auto w2 = text_size(f.title, 74 * k, "STUDIO");
    theme::halftone(d, ImVec2(at.x + w.x + w2.x + 30 * k, at.y + 10 * k), 3, 7, IM_COL32(255, 255, 255, 60));
    text(d, f.bold, 15 * k, ImVec2(at.x + 6 * k, at.y - 14 * k), blue_light, "RESKATE");
}
// The tab row: Q / E (LB / RB) either side, the chosen tab outlined with a blue brush stroke under it.
void tab_row(ImDrawList* d, const Fonts& f, ImVec2 at) {
    auto& u = ui();
    const float k = f.k, h = 36 * k, fs = 25 * k;
    float x = at.x;
    const auto cap = [&](const char* key, std::optional<Glyph> g) {
        if (u.pad_mode && g) x += glyph(d, f, ImVec2(x, at.y + 7 * k), *g, u.playstation) + 12 * k;
        else {
            d->AddRect(ImVec2(x, at.y + 7 * k), ImVec2(x + 22 * k, at.y + 29 * k), white, 0, 0, 2 * k);
            const auto t = text_size(f.bold, 13 * k, key);
            text(d, f.bold, 13 * k, ImVec2(x + 11 * k - t.x * 0.5f, at.y + 18 * k - t.y * 0.5f), white, key);
            x += 22 * k + 14 * k;
        }
    };
    cap("Q", Glyph::lb);
    for (int i = 0; i < static_cast<int>(tab_names.size()); ++i) {
        const auto t = text_size(f.heading, fs, tab_names[static_cast<std::size_t>(i)]);
        const ImVec2 a(x, at.y), b(x + t.x + 20 * k, at.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(b.x - a.x, b.y - a.y))) {
            u.tab = i;
            u.row = 0;
        }
        ImGui::PopID();
        const bool chosen = u.tab == i;
        skate_theme::rough_rect(d, a, b, chosen ? IM_COL32(22, 22, 24, 250) : IM_COL32(26, 26, 28, 236), seed_of(a), k * 0.6f);
        if (chosen) {
            d->AddRect(ImVec2(a.x - 3 * k, a.y - 3 * k), ImVec2(b.x + 3 * k, b.y + 3 * k), white, 0, 0, 2.6f * k);
            // The blue brush stroke under the chosen tab.
            d->AddBezierCubic(ImVec2(a.x - 2 * k, b.y + 6 * k), ImVec2(a.x + (b.x - a.x) * 0.3f, b.y + 9 * k),
                              ImVec2(a.x + (b.x - a.x) * 0.7f, b.y + 3 * k), ImVec2(b.x + 4 * k, b.y + 5 * k), blue, 4 * k);
        }
        text(d, f.heading, fs, ImVec2(a.x + 10 * k, a.y + (h - t.y) * 0.5f), white, tab_names[static_cast<std::size_t>(i)]);
        x = b.x + 14 * k;
    }
    cap("E", Glyph::rb);
}
// The list: dark rough rows; the chosen one a blue brush stroke with a screen-print fade at its end.
void list(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 at, ImVec2 size) {
    auto& u = ui();
    const float k = f.k, row_h = 50 * k, gap = 8 * k;
    const auto tab = static_cast<Tab>(u.tab);
    const auto rows = rows_of(tab, v);
    int chosen = u.row;
    if (tab == Tab::joints) {
        const auto joints = studio::editable_joints();
        for (std::size_t i = 0; i < joints.size(); ++i) if (joints[i].index == u.selected) chosen = static_cast<int>(i);
    }
    chosen = std::clamp(chosen, 0, std::max(0, static_cast<int>(rows.size()) - 1));
    u.row = tab == Tab::joints ? u.row : chosen;
    skate_theme::rough_rect(d, ImVec2(at.x - 18 * k, at.y - 10 * k), ImVec2(at.x + size.x + 18 * k, at.y + size.y + 10 * k), IM_COL32(48, 50, 56, 150), 61, k);
    const float content = rows.size() * (row_h + gap);
    float& scroll = u.list_scroll[static_cast<std::size_t>(u.tab)];
    // The list follows the chosen row when it changes (the D-pad, a click); the wheel scrolls freely.
    if (int& followed = u.list_followed[static_cast<std::size_t>(u.tab)]; followed != chosen) {
        followed = chosen;
        const float chosen_top = chosen * (row_h + gap);
        if (chosen_top < scroll) scroll = chosen_top;
        if (chosen_top + row_h > scroll + size.y) scroll = chosen_top + row_h - size.y;
    }
    const auto mouse = ImGui::GetIO().MousePos;
    if (mouse.x >= at.x && mouse.x <= at.x + size.x && mouse.y >= at.y && mouse.y <= at.y + size.y && ImGui::GetIO().MouseWheel != 0)
        scroll -= ImGui::GetIO().MouseWheel * (row_h + gap);
    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, content - size.y));
    d->PushClipRect(at, ImVec2(at.x + size.x + 4 * k, at.y + size.y), true);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const float y = at.y + i * (row_h + gap) - scroll;
        if (y + row_h < at.y || y > at.y + size.y) continue;
        const ImVec2 a(at.x, y), b(at.x + size.x - 24 * k, y + row_h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(static_cast<int>(i));
        const bool clicked = ImGui::InvisibleButton("##row", ImVec2(b.x - a.x, b.y - a.y)) && y >= at.y - 1;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool here = static_cast<int>(i) == chosen;
        if (here) {
            skate_theme::rough_rect(d, a, b, blue, seed_of(a), k * 1.2f);
            theme::halftone(d, ImVec2(b.x - 70 * k, a.y + 8 * k), 9, 5, IM_COL32(255, 255, 255, 46));
            skate_theme::scribble(d, ImVec2(a.x, a.y - 2 * k), ImVec2(a.x + 4 * k, b.y + 2 * k), 7 + static_cast<unsigned>(i), k);
        } else {
            skate_theme::rough_rect(d, a, b, hovered ? IM_COL32(40, 41, 44, 246) : IM_COL32(24, 24, 26, 246), seed_of(a), k);
            d->AddRectFilled(ImVec2(a.x + 2 * k, b.y), ImVec2(b.x - 2 * k, b.y + 3 * k), IM_COL32(0, 0, 0, 120)); // its shadow
        }
        const ImU32 ink = row.enabled ? white : faint;
        text(d, f.bold, 21 * k, ImVec2(a.x + 30 * k, a.y + row_h * 0.5f - 12 * k), ink, row.label);
        if (!row.value.empty()) {
            const auto t = text_size(f.bold, 16 * k, row.value.c_str());
            text(d, f.bold, 16 * k, ImVec2(b.x - t.x - 22 * k, a.y + row_h * 0.5f - 9 * k), here ? white : soft, row.value);
        }
        if (row.on && !here) d->AddRectFilled(ImVec2(a.x, a.y + 6 * k), ImVec2(a.x + 4 * k, b.y - 6 * k), blue);
        if (clicked) {
            if (tab == Tab::joints) u.selected = studio::editable_joints()[i].index;
            else {
                u.row = static_cast<int>(i);
                if (row.enabled && row.act) row.act();
            }
        }
    }
    d->PopClipRect();
    // The thin scroll bar at the right, as the game's lists have.
    const float bar_x = at.x + size.x - 8 * k;
    d->AddRectFilled(ImVec2(bar_x, at.y), ImVec2(bar_x + 3 * k, at.y + size.y), IM_COL32(220, 220, 225, 120));
    if (content > size.y) {
        const float h = std::max(30 * k, size.y * size.y / content);
        const float y = at.y + (size.y - h) * scroll / (content - size.y);
        d->AddRectFilled(ImVec2(bar_x - 4 * k, y), ImVec2(bar_x + 7 * k, y + h), IM_COL32(235, 235, 240, 255), 2 * k);
    }
}
// The white card under the list, as the game's trick description: the chosen joint's numbers, or what
// the highlighted row does.
void card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 at, ImVec2 size) {
    auto& u = ui();
    const float k = f.k;
    skate_theme::rough_rect(d, ImVec2(at.x + 6 * k, at.y + 6 * k), ImVec2(at.x + size.x + 6 * k, at.y + size.y + 6 * k), IM_COL32(0, 0, 0, 140), 71, k);
    skate_theme::rough_rect(d, at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(234, 234, 232, 252), 73, k);
    theme::halftone(d, ImVec2(at.x + size.x - 40 * k, at.y + 10 * k), 4, 10, IM_COL32(0, 0, 0, 22));
    const ImU32 ink = IM_COL32(20, 20, 22, 255), ink_soft = IM_COL32(90, 90, 96, 255);
    const auto tab = static_cast<Tab>(u.tab);
    float y = at.y + 16 * k;
    if (tab == Tab::joints || tab == Tab::keys) {
        text(d, f.heading, 22 * k, ImVec2(at.x + 22 * k, y), ink, upper(label_of(u.selected)));
        const auto* key = key_here(v, u.selected);
        const std::string state = key ? "KEY ON THIS FRAME" : "NO KEY ON THIS FRAME";
        const auto t = text_size(f.bold, 13 * k, state.c_str());
        text(d, f.bold, 13 * k, ImVec2(at.x + size.x - t.x - 22 * k, y + 6 * k), key ? blue : ink_soft, state);
        y += 40 * k;
        const auto offset = studio::offset_of(u.selected);
        auto euler = studio::to_euler(offset.rotation);
        auto position = offset.offset;
        // The board: where it is and how it is turned, from where it starts on the ground (its keys added to the trick's own).
        const bool board_fields = u.selected == studio::board_joint && v.board_base_valid;
        if (board_fields) {
            for (std::size_t i = 0; i < 3; ++i) position[i] = v.board_base.position[i] + offset.offset[i];
            euler = studio::to_euler(studio::multiply(offset.rotation, v.board_base.rotation));
        }
        const auto* track = track_view(v, u.selected);
        const bool locked = track && track->locked;
        const auto* e = studio::editable(u.selected);
        ImGui::PushStyleColor(ImGuiCol_Text, ink);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(205, 205, 204, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(190, 192, 196, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(150, 200, 255, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * k, 4 * k));
        ImGui::PushFont(f.bold);
        const float col_w = (size.x - 44 * k) / 3;
        bool turned = false, moved = false;
        const char* axes[] = {"X", "Y", "Z"};
        text(d, f.bold, 13 * k, ImVec2(at.x + 22 * k, y), ink_soft, board_fields ? "ROTATION (0, 0, 0 = AS IT STARTS)" : "ROTATION");
        for (int i = 0; i < 3; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(at.x + 22 * k + i * col_w, y + 18 * k));
            ImGui::PushID(i);
            ImGui::TextUnformatted(axes[i]);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(col_w - 34 * k);
            turned |= ImGui::DragFloat("##r", &euler[static_cast<std::size_t>(i)], 0.4f, 0, 0, "%.1f");
            ImGui::PopID();
        }
        if (e && e->moves) {
            text(d, f.bold, 13 * k, ImVec2(at.x + 22 * k, y + 52 * k), ink_soft, board_fields ? "POSITION (0, 0, 0 = WHERE IT STARTS, ON THE GROUND)" : "POSITION");
            for (int i = 0; i < 3; ++i) {
                ImGui::SetCursorScreenPos(ImVec2(at.x + 22 * k + i * col_w, y + 70 * k));
                ImGui::PushID(10 + i);
                ImGui::TextUnformatted(axes[i]);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(col_w - 34 * k);
                moved |= ImGui::DragFloat("##p", &position[static_cast<std::size_t>(i)], 0.002f, 0, 0, "%.3f");
                ImGui::PopID();
            }
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        if ((turned || moved) && !locked) {
            static double last_change = 0;
            if (ImGui::GetTime() - last_change > 0.4) studio::begin_change();
            last_change = ImGui::GetTime();
            if (board_fields) {
                studio::Offset next;
                for (std::size_t i = 0; i < 3; ++i) next.offset[i] = position[i] - v.board_base.position[i];
                next.rotation = studio::normalised(studio::multiply(studio::from_euler(euler), studio::conjugate(v.board_base.rotation)));
                studio::set_offset(u.selected, next);
            } else studio::set_offset(u.selected, {studio::from_euler(euler), position});
        }
        if (tab == Tab::keys) {
            const auto rows = rows_of(tab, v);
            if (u.row >= 0 && u.row < static_cast<int>(rows.size()))
                text(d, f.bold, 14 * k, ImVec2(at.x + 22 * k, at.y + size.y - 30 * k), ink_soft, rows[static_cast<std::size_t>(u.row)].about);
        }
        return;
    }
    if (tab == Tab::animations) {
        // The name and trick of the one being edited, typed here.
        if (u.names_for != v.revision && !ImGui::IsAnyItemActive()) {
            std::snprintf(u.name.data(), u.name.size(), "%s", v.name.c_str());
            std::snprintf(u.trick.data(), u.trick.size(), "%s", v.trick.c_str());
            u.names_for = v.revision;
        }
        const float w = (size.x - 60 * k) / 2;
        text(d, f.bold, 13 * k, ImVec2(at.x + 22 * k, y), ink_soft, "NAME");
        text(d, f.bold, 13 * k, ImVec2(at.x + 38 * k + w, y), ink_soft, "TRICK");
        ImGui::PushStyleColor(ImGuiCol_Text, ink);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(205, 205, 204, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(195, 197, 200, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(215, 230, 250, 255));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, blue_dim);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * k, 6 * k));
        ImGui::PushFont(f.bold);
        ImGui::SetCursorScreenPos(ImVec2(at.x + 22 * k, y + 18 * k));
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputText("##name", u.name.data(), u.name.size(), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
            studio::rename(u.name.data());
        ImGui::SetCursorScreenPos(ImVec2(at.x + 38 * k + w, y + 18 * k));
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputText("##trick", u.trick.data(), u.trick.size(), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
            studio::set_trick(u.trick.data());
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(5);
        y += 62 * k;
    }
    const auto rows = rows_of(tab, v);
    if (u.row >= 0 && u.row < static_cast<int>(rows.size())) {
        // Wrapped to the card's width, the game's body type in dark ink.
        const auto& about = rows[static_cast<std::size_t>(u.row)].about;
        d->AddText(f.bold, 19 * k, ImVec2(at.x + 22 * k, y), ink, about.c_str(), nullptr, size.x - 44 * k);
    }
}

// The bottom of the screen: the whole trick from start to end, one row of keys per joint with a row
// of every key on top, the playhead across all of them. FIT shows all of it; - and + (Ctrl + wheel,
// LT + up / down on a controller) zoom in on the playhead.
void zoom_timeline(float factor) {
    auto& u = ui();
    u.timeline_zoom = std::clamp(u.timeline_zoom * factor, 1.0f, 16.0f);
    u.zoom_to_playhead = true;
}
void timeline(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 at, ImVec2 size) {
    auto& u = ui();
    const float k = f.k;
    const float label_w = 280 * k, header_h = 70 * k, summary_h = 30 * k, row_h = 40 * k, gap = 4 * k;
    skate_theme::rough_rect(d, ImVec2(at.x - 10 * k, at.y), ImVec2(at.x + size.x + 10 * k, at.y + size.y + 10 * k), IM_COL32(14, 14, 16, 236), 81, k * 1.4f);
    // Header, left: the animation's name, brushed, and the transport.
    text(d, f.title, 30 * k, ImVec2(at.x + 20 * k, at.y + 4 * k), white, title_of(v));
    float bx = at.x + 20 * k;
    const float by = at.y + 42 * k;
    const ImVec2 cap(30 * k, 22 * k);
    if (button(d, f, "##start", ImVec2(bx, by), cap, "|<", false, v.has_clip)) studio::set_time(0);
    bx += cap.x + 4 * k;
    if (button(d, f, "##back", ImVec2(bx, by), cap, "<", false, v.has_clip)) step_frame(v, -1);
    bx += cap.x + 4 * k;
    if (button(d, f, "##play", ImVec2(bx, by), ImVec2(62 * k, cap.y), v.playing ? "PAUSE" : "PLAY", v.playing, v.has_clip)) studio::set_playing(!v.playing);
    bx += 62 * k + 4 * k;
    if (button(d, f, "##next", ImVec2(bx, by), cap, ">", false, v.has_clip)) step_frame(v, 1);
    bx += cap.x + 4 * k;
    if (button(d, f, "##end", ImVec2(bx, by), cap, ">|", false, v.has_clip)) studio::set_time(v.duration);
    bx += cap.x + 4 * k;
    if (button(d, f, "##loop", ImVec2(bx, by), ImVec2(40 * k, cap.y), "LOOP", v.loop)) studio::set_loop(!v.loop);

    const ImVec2 lane(at.x + label_w, at.y);
    const float duration = std::max(v.duration, 0.5f);
    const float pad = 22 * k;
    // Zoom: - FIT + at the header's right; the time under them.
    {
        float zx = at.x + size.x - 16 * k - 3 * 34 * k;
        if (button(d, f, "##zoomout", ImVec2(zx, at.y + 6 * k), ImVec2(30 * k, 24 * k), "-", false, u.timeline_zoom > 1)) zoom_timeline(1 / 1.5f);
        zx += 34 * k;
        if (button(d, f, "##fit", ImVec2(zx, at.y + 6 * k), ImVec2(30 * k, 24 * k), "FIT", u.timeline_zoom <= 1.001f)) u.timeline_zoom = 1;
        zx += 34 * k;
        if (button(d, f, "##zoomin", ImVec2(zx, at.y + 6 * k), ImVec2(30 * k, 24 * k), "+", false, u.timeline_zoom < 16)) zoom_timeline(1.5f);
        const std::string time = v.has_clip ? std::format("{:.2f} / {:.2f} s   frame {}", v.time, v.duration, static_cast<int>(std::round(v.time * fps))) : "";
        const auto t = text_size(f.bold, 13 * k, time.c_str());
        text(d, f.bold, 13 * k, ImVec2(at.x + size.x - 16 * k - t.x, at.y + 36 * k), soft, time);
    }
    const float ruler_left = lane.x, ruler_right = at.x + size.x - 130 * k;
    const float ruler_w = ruler_right - ruler_left;
    const float seconds_w = (ruler_w - 2 * pad) / duration * u.timeline_zoom;
    if (u.zoom_to_playhead || u.timeline_zoom <= 1.001f) {
        u.timeline_scroll = v.time * seconds_w - (ruler_w - 2 * pad) * 0.5f;
        u.zoom_to_playhead = false;
    } else if (const float x = pad + v.time * seconds_w - u.timeline_scroll; v.playing && (x < pad || x > ruler_w - pad))
        u.timeline_scroll = v.time * seconds_w - (ruler_w - 2 * pad) * 0.5f;
    u.timeline_scroll = std::clamp(u.timeline_scroll, 0.0f, std::max(0.0f, duration * seconds_w - (ruler_w - 2 * pad)));
    const auto x_of = [&](float t) { return ruler_left + pad + t * seconds_w - u.timeline_scroll; };
    const auto t_of = [&](float x) { return std::clamp((x - ruler_left - pad + u.timeline_scroll) / seconds_w, 0.0f, v.duration); };
    const float lanes_right = at.x + size.x;
    // The ruler: frame numbers.
    d->PushClipRect(ImVec2(ruler_left, at.y), ImVec2(ruler_right, at.y + size.y), true);
    const int frames = static_cast<int>(std::ceil(duration * fps));
    const float frame_w = seconds_w / fps;
    const int every = frame_w > 26 * k ? 1 : frame_w > 9 * k ? 5 : 10;
    for (int i = 0; i <= frames; ++i) {
        const float x = x_of(i / fps);
        if (x < ruler_left - 20 || x > ruler_right + 20) continue;
        const bool labelled = i % every == 0;
        d->AddLine(ImVec2(x, at.y + header_h - (labelled ? 16 : 7) * k), ImVec2(x, at.y + header_h - 3 * k), labelled ? white : faint, 1.6f * k);
        if (labelled) {
            const auto s = std::to_string(i);
            const auto w = text_size(f.bold, 16 * k, s.c_str());
            text(d, f.bold, 16 * k, ImVec2(x - w.x * 0.5f, at.y + 26 * k), white, s);
        }
    }
    for (const auto& [t, name] : {std::pair{v.takeoff, "POP"}, std::pair{v.landing, "LAND"}}) {
        if (t < 0 || !v.has_clip) continue;
        const float x = x_of(t);
        d->AddLine(ImVec2(x, at.y + header_h), ImVec2(x, at.y + size.y), IM_COL32(1, 131, 255, 80), 1.5f * k);
        text(d, f.bold, 12 * k, ImVec2(x + 4 * k, at.y + 8 * k), blue_light, name);
    }
    d->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(ruler_left, at.y));
    ImGui::InvisibleButton("##ruler", ImVec2(ruler_w, header_h));
    if (ImGui::IsItemActivated()) u.dragging_playhead = true;
    if (u.dragging_playhead) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && v.has_clip) {
            float t = t_of(ImGui::GetIO().MousePos.x);
            if (!ImGui::GetIO().KeyAlt) t = std::round(t * fps) / fps;
            studio::set_time(t);
            studio::set_playing(false);
        } else u.dragging_playhead = false;
    }
    // Every key of every joint, in one row: the whole animation at a glance.
    const float summary_y = at.y + header_h + 4 * k;
    skate_theme::rough_rect(d, ImVec2(at.x + 8 * k, summary_y), ImVec2(lanes_right - 8 * k, summary_y + summary_h), IM_COL32(30, 31, 34, 240), 83, k * 0.6f);
    text(d, f.heading, 15 * k, ImVec2(at.x + 22 * k, summary_y + 6 * k), soft, "ALL KEYS");
    d->PushClipRect(ImVec2(ruler_left, summary_y), ImVec2(ruler_right, summary_y + summary_h), true);
    for (const auto& t : v.tracks)
        for (const auto& key : t.keys) diamond(d, ImVec2(x_of(key.time), summary_y + summary_h * 0.5f), 6 * k, t.hidden ? faint : soft);
    d->PopClipRect();
    // Rows, one per joint.
    const auto joints = studio::editable_joints();
    const float rows_top = summary_y + summary_h + 6 * k;
    const float rows_h = at.y + size.y - rows_top;
    const float content_h = joints.size() * (row_h + gap);
    for (std::size_t r = 0; r < joints.size(); ++r)
        if (joints[r].index == u.selected) {
            const float top = r * (row_h + gap), bottom = top + row_h;
            if (top < u.rows_scroll) u.rows_scroll = top;
            if (bottom > u.rows_scroll + rows_h) u.rows_scroll = bottom - rows_h;
        }
    u.rows_scroll = std::clamp(u.rows_scroll, 0.0f, std::max(0.0f, content_h - rows_h));
    d->PushClipRect(ImVec2(at.x, rows_top), ImVec2(lanes_right, at.y + size.y), true);
    for (std::size_t r = 0; r < joints.size(); ++r) {
        const auto& joint = joints[r];
        const float y = rows_top + r * (row_h + gap) - u.rows_scroll;
        if (y + row_h < rows_top || y > at.y + size.y) continue;
        const bool chosen = joint.index == u.selected;
        const auto* track = track_view(v, joint.index);
        const ImVec2 a(at.x + 8 * k, y), b(lanes_right - 8 * k, y + row_h);
        if (chosen) {
            skate_theme::rough_rect(d, a, ImVec2(a.x + label_w - 12 * k, b.y), blue, seed_of(a), k);
            theme::halftone(d, ImVec2(a.x + label_w - 70 * k, a.y + 6 * k), 8, 4, IM_COL32(255, 255, 255, 46));
            skate_theme::rough_rect(d, ImVec2(a.x + label_w - 4 * k, a.y), b, IM_COL32(44, 50, 62, 236), seed_of(a) + 1, k);
        } else {
            skate_theme::rough_rect(d, a, ImVec2(a.x + label_w - 12 * k, b.y), IM_COL32(24, 24, 26, 246), seed_of(a), k);
            skate_theme::rough_rect(d, ImVec2(a.x + label_w - 4 * k, a.y), b, r % 2 ? IM_COL32(30, 31, 34, 220) : IM_COL32(34, 35, 38, 220), seed_of(a) + 1, k);
        }
        const bool has_keys = track && !track->keys.empty();
        text(d, f.bold, 17 * k, ImVec2(a.x + 20 * k, y + row_h * 0.5f - 10 * k), chosen || has_keys ? white : soft, upper(std::string(joint.label)));
        ImGui::PushID(joint.index);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##label", ImVec2(label_w - 12 * k, row_h))) u.selected = joint.index;
        ImGui::SetCursorScreenPos(ImVec2(ruler_left, y));
        ImGui::InvisibleButton("##lane", ImVec2(ruler_right - ruler_left, row_h));
        const bool lane_hovered = ImGui::IsItemHovered();
        const bool lane_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool on_key = false;
        if (track) {
            d->PushClipRect(ImVec2(ruler_left, rows_top), ImVec2(ruler_right, at.y + size.y), true);
            for (const auto& key : track->keys) {
                const bool dragged_with = u.key_joint && is_picked(joint.index, key.time) && is_picked(*u.key_joint, u.key_from);
                const float shown = u.key_joint == joint.index && std::abs(key.time - u.key_from) < 1e-4f ? u.key_to
                                    : dragged_with ? key.time + (u.key_to - u.key_from) : key.time;
                const ImVec2 c(x_of(shown), y + row_h * 0.5f);
                const bool current = std::abs(key.time - v.time) < 0.5f / fps;
                const bool close_to = lane_hovered && std::abs(ImGui::GetIO().MousePos.x - c.x) < 9 * k;
                const bool in_pick = is_picked(joint.index, key.time);
                if ((current && chosen) || in_pick) diamond(d, c, 13 * k, in_pick ? blue : blue_dim);
                diamond(d, c, (close_to ? 10.5f : 9) * k, track->hidden ? faint : in_pick || (current && chosen) ? blue_light : white);
                if (key.ease == studio::Ease::step) d->AddLine(ImVec2(c.x, c.y - 4 * k), ImVec2(c.x, c.y + 4 * k), IM_COL32(40, 40, 40, 255), 2 * k);
                if (close_to && lane_clicked && !track->locked && ImGui::GetIO().KeyShift) {
                    on_key = true;
                    auto& picked = u.picked;
                    const auto found = std::find_if(picked.begin(), picked.end(), [&](const auto& p) {
                        return p.first == joint.index && std::abs(p.second - key.time) < 1e-4f;
                    });
                    if (found != picked.end()) picked.erase(found);
                    else picked.emplace_back(joint.index, key.time);
                } else if (close_to && lane_clicked && !track->locked) {
                    on_key = true;
                    if (!in_pick) u.picked.clear();
                    u.key_joint = joint.index;
                    u.key_from = key.time;
                    u.key_to = key.time;
                    u.key_moved = false;
                    u.selected = joint.index;
                    studio::set_time(key.time);
                }
            }
            d->PopClipRect();
        }
        if (lane_clicked && !on_key && v.has_clip) {
            u.picked.clear();
            u.selected = joint.index;
            studio::set_time(std::round(t_of(ImGui::GetIO().MousePos.x) * fps) / fps);
            studio::set_playing(false);
        }
        ImGui::PopID();
    }
    d->PopClipRect();
    if (u.key_joint) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float to = std::round(t_of(ImGui::GetIO().MousePos.x) * fps) / fps;
            if (std::abs(to - u.key_to) > 1e-4f) u.key_moved = true;
            u.key_to = to;
        } else {
            if (u.key_moved && std::abs(u.key_to - u.key_from) > 1e-4f) {
                if (is_picked(*u.key_joint, u.key_from) && u.picked.size() > 1) move_picked(u.key_to - u.key_from);
                else {
                    studio::begin_change();
                    studio::move_key(*u.key_joint, u.key_from, u.key_to);
                }
                studio::set_time(u.key_to);
            }
            u.key_joint.reset();
        }
    }
    if (v.has_clip) {
        const float x = x_of(v.time);
        if (x >= ruler_left && x <= ruler_right) {
            d->AddLine(ImVec2(x, at.y + 4 * k), ImVec2(x, at.y + size.y), blue, 2.4f * k);
            d->AddTriangleFilled(ImVec2(x - 8 * k, at.y + 2 * k), ImVec2(x + 8 * k, at.y + 2 * k), ImVec2(x, at.y + 12 * k), blue);
        }
    }
    // Wheel: scroll the rows; Ctrl + wheel zooms; Shift + wheel scrolls the time.
    const auto mouse = ImGui::GetIO().MousePos;
    if (mouse.x >= at.x && mouse.y >= at.y && mouse.x <= at.x + size.x && mouse.y <= at.y + size.y) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            if (ImGui::GetIO().KeyCtrl) zoom_timeline(wheel > 0 ? 1.25f : 1 / 1.25f);
            else if (ImGui::GetIO().KeyShift) u.timeline_scroll -= wheel * 60 * k;
            else u.rows_scroll -= wheel * (row_h + gap);
        }
    }
}


// The skater: dots on the joints that can be chosen, the glowing box on the chosen one, and the drag.
void viewport(ImDrawList* d, const Fonts& f, const studio::View& v, const Projector& project, ImVec2 area_min, ImVec2 area_max) {
    auto& u = ui();
    const float k = f.k;
    const auto mouse = ImGui::GetIO().MousePos;
    const bool inside = !u.menu_open && mouse.x >= area_min.x && mouse.y >= area_min.y && mouse.x <= area_max.x && mouse.y <= area_max.y;
    std::optional<std::uint16_t> hovered;
    float best = 22 * k;
    if (!u.pad_mode)
        for (const auto& j : v.joints)
            if (const auto at = project(j.position)) {
                const float dist = std::hypot(at->x - mouse.x, at->y - mouse.y);
                if (inside && dist < best) {
                    best = dist;
                    hovered = j.joint;
                }
            }
    for (const auto& j : v.joints) {
        const auto at = project(j.position);
        if (!at || j.joint == u.selected) continue;
        const bool hot = hovered && *hovered == j.joint;
        d->AddCircleFilled(*at, (hot ? 6.0f : 3.5f) * k, hot ? blue_light : IM_COL32(255, 255, 255, 90));
    }
    const auto* sel = joint_view(v, u.selected);
    std::optional<ImVec2> sel_at = sel ? project(sel->position) : std::nullopt;
    if (sel_at) {
        glow_box(d, *sel_at, 22 * k, k);
        if (u.pad_mode) text(d, f.bold, 13 * k, ImVec2(sel_at->x + 30 * k, sel_at->y - 8 * k), blue_light, upper(label_of(u.selected)));
    }
    if (hovered && *hovered != u.selected)
        if (const auto at = project(joint_view(v, *hovered)->position))
            text(d, f.bold, 13 * k, ImVec2(at->x + 10 * k, at->y - 22 * k), white, upper(label_of(*hovered)));
    if (sel) {
        const auto off = studio::offset_of(u.selected);
        const auto e = studio::to_euler(off.rotation);
        const float x = area_min.x + 24 * k, y = area_max.y - 92 * k;
        if (studio::editable(u.selected) && studio::editable(u.selected)->moves) {
            text(d, f.heading, 15 * k, ImVec2(x, y - 44 * k), IM_COL32(255, 255, 255, 110), "POSITION");
            text(d, f.body, 13 * k, ImVec2(x, y - 24 * k), IM_COL32(255, 255, 255, 90),
                 std::format("X: {:.2f}    Y: {:.2f}    Z: {:.2f}", off.offset[0], off.offset[1], off.offset[2]));
        }
        text(d, f.heading, 15 * k, ImVec2(x, y), IM_COL32(255, 255, 255, 110), "ROTATION");
        text(d, f.body, 13 * k, ImVec2(x, y + 20 * k), IM_COL32(255, 255, 255, 90), std::format("X: {:.0f}    Y: {:.0f}    Z: {:.0f}", e[0], e[1], e[2]));
        const char* axis = u.axis == 0 ? "X" : u.axis == 1 ? "Y" : u.axis == 2 ? "Z" : "FREE";
        text(d, f.body, 12 * k, ImVec2(x, y + 42 * k), IM_COL32(120, 190, 255, 150),
             std::format("{}  axis {}  ({})", u.limb_drag ? "LIMB DRAG (I)" : u.move_mode ? "MOVE" : "TURN", axis, u.local_axes ? "local" : "world"));
    }
    if (u.menu_open || (!inside && !u.dragging_gizmo && !u.orbiting && !u.panning)) return;
    auto& io = ImGui::GetIO();
    if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
        if (hovered && *hovered != u.selected) u.selected = *hovered;
        else if (sel && v.has_clip) {
            const auto* track = track_view(v, u.selected);
            if (!(track && track->locked)) {
                u.dragging_gizmo = true;
                u.drag_start = mouse;
                begin_turn(v);
            }
        }
    }
    if (u.dragging_gizmo) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) u.dragging_gizmo = false;
        else if (u.limb_drag) drag_limb_to(v, project, mouse);
        else if (project.valid) apply_turn(project, (mouse.x - u.drag_start.x) / k, (mouse.y - u.drag_start.y) / k, u.move_mode);
    }
    auto orbit = v.orbit;
    bool camera_changed = false;
    if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) u.orbiting = true;
    if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) u.panning = true;
    if (u.orbiting) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) u.orbiting = false;
        else {
            orbit.yaw -= io.MouseDelta.x * 0.006f;
            orbit.pitch += io.MouseDelta.y * 0.005f;
            camera_changed = true;
        }
    }
    if (u.panning) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) u.panning = false;
        else if (project.valid) {
            const float metres = orbit.distance / (900 * k);
            for (int i = 0; i < 3; ++i) orbit.pan[i] += -project.right[i] * io.MouseDelta.x * metres + project.up[i] * io.MouseDelta.y * metres;
            camera_changed = true;
        }
    }
    if (inside && io.MouseWheel != 0) {
        orbit.distance *= io.MouseWheel > 0 ? 0.9f : 1.1f;
        camera_changed = true;
    }
    if (camera_changed) studio::set_orbit(orbit);
}

void keyboard(const studio::View& v) {
    auto& u = ui();
    auto& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl;
    bool any = false;
    const auto pressed = [&](ImGuiKey key, bool repeat = false) {
        const bool p = ImGui::IsKeyPressed(key, repeat);
        any |= p;
        return p;
    };
    if (pressed(ImGuiKey_Space)) studio::set_playing(!v.playing);
    if (pressed(ImGuiKey_LeftArrow, true)) step_frame(v, -1);
    if (pressed(ImGuiKey_RightArrow, true)) step_frame(v, 1);
    if (pressed(ImGuiKey_UpArrow, true)) move_row(v, -1);
    if (pressed(ImGuiKey_DownArrow, true)) move_row(v, 1);
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) activate_row(v);
    if (!ctrl && pressed(ImGuiKey_Q)) switch_tab(-1);
    if (!ctrl && pressed(ImGuiKey_E)) switch_tab(1);
    if (pressed(ImGuiKey_Home)) studio::set_time(0);
    if (pressed(ImGuiKey_End)) studio::set_time(v.duration);
    if (ctrl && pressed(ImGuiKey_Z, true)) studio::undo();
    if (ctrl && pressed(ImGuiKey_Y, true)) studio::redo();
    if (!ctrl && pressed(ImGuiKey_I)) ui().limb_drag = !ui().limb_drag;
    if (ctrl && pressed(ImGuiKey_C)) studio::copy_pose();
    if (ctrl && pressed(ImGuiKey_V)) {
        studio::begin_change();
        studio::paste_pose();
    }
    if (ctrl && pressed(ImGuiKey_S)) save_now();
    if (ctrl && pressed(ImGuiKey_A)) pick_all(v);
    if (!ctrl && pressed(ImGuiKey_Escape) && !u.picked.empty()) u.picked.clear();
    if (pressed(ImGuiKey_Equal) || pressed(ImGuiKey_KeypadAdd)) zoom_timeline(1.5f);
    if (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract)) zoom_timeline(1 / 1.5f);
    if (!ctrl && pressed(ImGuiKey_F)) studio::focus_joint(u.selected);
    if (!ctrl && pressed(ImGuiKey_K) && v.has_clip) key_joint();
    if (pressed(ImGuiKey_Delete)) {
        if (static_cast<Tab>(u.tab) == Tab::animations) delete_row(v);
        else if (!u.picked.empty()) delete_picked();
        else delete_key(v);
    }
    if (!ctrl && pressed(ImGuiKey_X)) u.axis = u.axis == 0 ? -1 : 0;
    if (!ctrl && pressed(ImGuiKey_Y)) u.axis = u.axis == 1 ? -1 : 1;
    if (!ctrl && pressed(ImGuiKey_Z)) u.axis = u.axis == 2 ? -1 : 2;
    if (!ctrl && pressed(ImGuiKey_R)) u.move_mode = false;
    if (!ctrl && pressed(ImGuiKey_G)) u.move_mode = true;
    if (!ctrl && pressed(ImGuiKey_L)) u.local_axes = !u.local_axes;
    if (!ctrl && pressed(ImGuiKey_Tab)) u.menu_hidden = !u.menu_hidden;
    auto orbit = v.orbit;
    bool set = true;
    if (pressed(ImGuiKey_1)) orbit.yaw = 0, orbit.pitch = 0.1f;
    else if (pressed(ImGuiKey_2)) orbit.yaw = 3.14159f, orbit.pitch = 0.1f;
    else if (pressed(ImGuiKey_3)) orbit.yaw = 1.5708f, orbit.pitch = 0.1f;
    else if (pressed(ImGuiKey_4)) orbit.yaw = -1.5708f, orbit.pitch = 0.1f;
    else if (pressed(ImGuiKey_5)) orbit.yaw = 0.6f, orbit.pitch = 0.18f;
    else if (pressed(ImGuiKey_0)) orbit = studio::Orbit{};
    else set = false;
    if (set) studio::set_orbit(orbit);
    if (any || std::hypot(io.MousePos.x - u.last_mouse.x, io.MousePos.y - u.last_mouse.y) > 4 || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) u.pad_mode = false;
    u.last_mouse = io.MousePos;
}

// An action button like the game's "P UN-PIN": its key or button, then what it does.
bool action_button(ImDrawList* d, const Fonts& f, const char* id, ImVec2 a, ImVec2 size, std::optional<Glyph> g, const char* key, const char* what,
                   ImU32 fill, ImU32 fill_hot, ImU32 ink) {
    auto& u = ui();
    const float k = f.k;
    ImGui::SetCursorScreenPos(a);
    const bool clicked = ImGui::InvisibleButton(id, size);
    skate_theme::rough_rect(d, a, ImVec2(a.x + size.x, a.y + size.y), ImGui::IsItemHovered() ? fill_hot : fill, seed_of(a), k);
    float x = a.x + 18 * k;
    if (u.pad_mode && g) x += glyph(d, f, ImVec2(x, a.y + size.y * 0.5f - 11 * k), *g, u.playstation) + 12 * k;
    else {
        const auto t = text_size(f.bold, 14 * k, key);
        const float w = std::max(26 * k, t.x + 12 * k);
        d->AddRect(ImVec2(x, a.y + size.y * 0.5f - 13 * k), ImVec2(x + w, a.y + size.y * 0.5f + 13 * k), ink, 0, 0, 2.2f * k);
        text(d, f.bold, 14 * k, ImVec2(x + (w - t.x) * 0.5f, a.y + size.y * 0.5f - t.y * 0.5f), ink, key);
        x += w + 12 * k;
    }
    const auto t = text_size(f.heading, 22 * k, what);
    text(d, f.heading, 22 * k, ImVec2(x, a.y + size.y * 0.5f - t.y * 0.5f), ink, what);
    return clicked;
}
// The lower right: KEY and SAVE, and under them the controls in a tidy two-column panel.
void prompts(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 view_max) {
    auto& u = ui();
    const float k = f.k;
    using Row = std::tuple<std::optional<Glyph>, const char*, const char*>;
    std::vector<Row> list;
    const char* move = u.move_mode ? "Move (L3: turn)" : u.axis < 0 ? "Turn (L3: axis)" : u.axis == 0 ? "Turn X (L3)" : u.axis == 1 ? "Turn Y (L3)" : "Turn Z (L3)";
    if (u.pad_mode)
        list = {{Glyph::ls, "", move},
                {Glyph::rs, "", u.stick_zoom ? "Zoom / pan (R3)" : "Orbit (R3: zoom)"},
                {Glyph::rt, "", "Play forward"},
                {Glyph::lt, "", "Play back"},
                {Glyph::dpad_ud, "", "Choose"},
                {Glyph::dpad_lr, "", "One frame"},
                {Glyph::y, "", v.playing ? "Pause" : "Play"},
                {Glyph::x, "", "Delete key"},
                {Glyph::b, "", "Undo"},
                {Glyph::view, "", "Zoom timeline"},
                {Glyph::lb, "", "Tabs (RB)"},
                {Glyph::l3, "", "+ R3 hold: back"}};
    else
        list = {{std::nullopt, "LMB", "Pick / turn"},
                {std::nullopt, "RMB", "Orbit"},
                {std::nullopt, "MMB", "Pan"},
                {std::nullopt, "G / R", u.move_mode ? "Move" : "Turn"},
                {std::nullopt, "X Y Z", "Axis"},
                {std::nullopt, "UP DN", "Choose"},
                {std::nullopt, "< >", "One frame"},
                {std::nullopt, "SPACE", v.playing ? "Pause" : "Play"},
                {std::nullopt, "DEL", "Delete key"},
                {std::nullopt, "CTRL Z", "Undo"},
                {std::nullopt, "- / +", "Zoom timeline"},
                {std::nullopt, "F8", "Back"}};
    const float col_w = 205 * k, row_h = 30 * k, pad = 14 * k;
    const int rows = static_cast<int>((list.size() + 1) / 2);
    const float panel_w = 2 * col_w + 2 * pad, panel_h = rows * row_h + 2 * pad;
    const ImVec2 pb(view_max.x - 20 * k, view_max.y - 50 * k), pa(pb.x - panel_w, pb.y - panel_h);
    skate_theme::rough_rect(d, pa, pb, IM_COL32(12, 12, 14, 200), 93, k);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& [g, key, what] = list[i];
        const ImVec2 at(pa.x + pad + static_cast<float>(i % 2) * col_w, pa.y + pad + static_cast<float>(i / 2) * row_h);
        // Glyph or key cap in a fixed slot, so the labels line up.
        if (g) glyph(d, f, at, *g, u.playstation);
        else {
            const auto t = text_size(f.bold, 11 * k, key);
            d->AddRectFilled(ImVec2(at.x, at.y + 2 * k), ImVec2(at.x + t.x + 12 * k, at.y + 20 * k), white, 3 * k);
            text(d, f.bold, 11 * k, ImVec2(at.x + 6 * k, at.y + 11 * k - t.y * 0.5f), skate_theme::black, key);
        }
        text(d, f.bold, 13 * k, ImVec2(at.x + 66 * k, at.y + 11 * k - text_size(f.bold, 13 * k, what).y * 0.5f), white, what);
    }
    // KEY (yellow, as the game's main action) and SAVE over the panel.
    const ImVec2 size(panel_w * 0.5f - 6 * k, 48 * k);
    const float by = pa.y - 12 * k - size.y;
    if (action_button(d, f, "##save", ImVec2(pa.x, by), size, Glyph::menu, "CTRL S", v.dirty ? "SAVE *" : "SAVE", IM_COL32(236, 236, 234, 250),
                      IM_COL32(255, 255, 255, 255), skate_theme::black) && v.has_clip)
        save_now();
    if (action_button(d, f, "##keybutton", ImVec2(pa.x + size.x + 12 * k, by), size, Glyph::a, "K", "KEY", IM_COL32(234, 214, 96, 255),
                      IM_COL32(246, 226, 120, 255), skate_theme::black) && v.has_clip)
        key_joint();
}
// Closing with changes not saved: the game's white card asks first. Its three buttons are a row the
// D-pad, the arrow keys or the mouse move along; the chosen one is lit, A / Enter / a click takes it.
void close_warning(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    if (!v.confirm_close) return;
    auto& u = ui();
    const float k = f.k;
    d->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 150));
    const float w = 640 * k, h = 260 * k;
    const ImVec2 a((display.x - w) * 0.5f, (display.y - h) * 0.45f), b(a.x + w, a.y + h);
    skate_theme::rough_rect(d, ImVec2(a.x + 6 * k, a.y + 6 * k), ImVec2(b.x + 6 * k, b.y + 6 * k), IM_COL32(0, 0, 0, 160), 95, k);
    skate_theme::rough_rect(d, a, b, IM_COL32(236, 236, 234, 252), 97, k);
    const ImU32 ink = IM_COL32(20, 20, 22, 255);
    text(d, f.title, 40 * k, ImVec2(a.x + 30 * k, a.y + 18 * k), ink, "UNSAVED CHANGES");
    const std::string question = "\"" + title_of(v) + "\" has changes that are not saved. Save them before you go back to skating?";
    d->AddText(f.bold, 19 * k, ImVec2(a.x + 30 * k, a.y + 84 * k), IM_COL32(60, 60, 66, 255), question.c_str(), nullptr, w - 60 * k);
    constexpr std::array<const char*, 3> labels{"SAVE", "DON'T SAVE", "STAY"};
    constexpr std::array<studio::CloseAnswer, 3> answers{studio::CloseAnswer::save, studio::CloseAnswer::discard, studio::CloseAnswer::cancel};
    u.warning_choice = std::clamp(u.warning_choice, 0, 2);
    const ImVec2 size((w - 60 * k - 2 * 12 * k) / 3, 48 * k);
    const float y = b.y - 30 * k - size.y;
    std::optional<int> taken;
    for (int i = 0; i < 3; ++i) {
        const ImVec2 at(a.x + 30 * k + i * (size.x + 12 * k), y), end(at.x + size.x, at.y + size.y);
        ImGui::SetCursorScreenPos(at);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##answer", size);
        if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) u.warning_choice = i;
        ImGui::PopID();
        const bool chosen = u.warning_choice == i;
        // The chosen one: the game's blue brush stroke; the others dark.
        skate_theme::rough_rect(d, at, end, chosen ? blue : IM_COL32(32, 32, 35, 250), seed_of(at), k);
        if (chosen) theme::halftone(d, ImVec2(end.x - 44 * k, at.y + 6 * k), 6, 5, IM_COL32(255, 255, 255, 46));
        const auto t = text_size(f.heading, 22 * k, labels[static_cast<std::size_t>(i)]);
        text(d, f.heading, 22 * k, ImVec2(at.x + (size.x - t.x) * 0.5f, at.y + (size.y - t.y) * 0.5f), white, labels[static_cast<std::size_t>(i)]);
        if (clicked) taken = i;
    }
    // Controller: left / right choose, A takes it, B stays.
    if (const auto pad = read_gamepad(); pad.valid) {
        static unsigned last{};
        const unsigned fresh = pad.buttons & ~last;
        last = pad.buttons;
        if (fresh & pad_left) u.warning_choice = std::max(0, u.warning_choice - 1);
        if (fresh & pad_right) u.warning_choice = std::min(2, u.warning_choice + 1);
        if (fresh & pad_a) taken = u.warning_choice;
        if (fresh & pad_b) taken = 2;
    }
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) u.warning_choice = std::max(0, u.warning_choice - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) u.warning_choice = std::min(2, u.warning_choice + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false)) taken = u.warning_choice;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) taken = 2;
    }
    // Prompts under the card.
    float x = a.x + 30 * k;
    const float py = b.y + 16 * k;
    x += prompt(d, f, ImVec2(x, py), u.pad_mode ? std::optional<Glyph>(Glyph::dpad_lr) : std::nullopt, "< >", "Choose", u.playstation);
    x += prompt(d, f, ImVec2(x, py), u.pad_mode ? std::optional<Glyph>(Glyph::a) : std::nullopt, "ENTER", "Select", u.playstation);
    prompt(d, f, ImVec2(x, py), u.pad_mode ? std::optional<Glyph>(Glyph::b) : std::nullopt, "ESC", "Stay", u.playstation);
    if (taken) {
        studio::answer_close(answers[static_cast<std::size_t>(*taken)]);
        u.warning_choice = 0;
    }
}
} // namespace

bool animation_studio_open() { return studio::is_open(); }

// Closed, the studio still listens for L3 + R3 held on a controller, which opens it.
void poll_animation_studio_controller() {
    static double since = 0;
    static bool fired = false;
    const auto pad = read_gamepad();
    const bool both = pad.valid && (pad.buttons & pad_l3) && (pad.buttons & pad_r3);
    const double now = GetTickCount64() / 1000.0;
    if (!both) {
        since = 0;
        fired = false;
        return;
    }
    if (since == 0) since = now;
    if (!fired && now - since > 0.5) {
        fired = true;
        studio::request_open(true);
        auto& u = ui();
        u.pad_mode = true;
        u.l3_combo = true;
        u.buttons = pad.buttons; // still held: not a fresh press once the studio is up
        u.combo_armed = false;
    }
}

// ---- the loading screen ------------------------------------------------------------------------
// Over the walk to the studio's room and back: dark grainy concrete, the reskate. wordmark with
// animator studio on a strip of blue tape, who built it on a paper strip, a tip with a bar, and
// skate.'s own "Loading..." box with its kickflipping board (animation_studio_images.h).
bool animation_studio_loading_pending() { return studio::loading().alpha > 0; }
// While a trick is being recorded (the studio steps out so you can skate): a strip across the top in the
// studio's look, and Esc (Back on a controller) goes back to the studio without one.
bool animation_studio_recording_pending() {
    return !studio::is_open() && studio::view().recording != studio::Recording::off;
}
void draw_animation_studio_recording() {
    if (!animation_studio_recording_pending() || studio::loading().alpha > 0.99f) return;
    const auto v = studio::view();
    {
        constexpr unsigned escape_key = 1u << 3;
        ControllerInput pad{};
        DingoSDKOverlayReadControllerInput(&pad, true);
        if ((DingoSDKOverlayReadPromptKeys() & escape_key) != 0 || (pad.buttons & XINPUT_GAMEPAD_BACK) != 0) {
            studio::cancel_recording();
            studio::request_open(true);
            return;
        }
    }
    auto& s = state();
    const auto display = ImGui::GetIO().DisplaySize;
    ImFont* heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    ImFont* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const float H = display.y;
    auto* d = ImGui::GetForegroundDrawList();
    const ImVec2 b(display.x, 0.085f * H);
    d->AddRectFilled(ImVec2(0, 0), b, IM_COL32(12, 12, 13, 225));
    d->AddRectFilled(ImVec2(0, b.y - 0.004f * H), ImVec2(display.x, b.y), skate_theme::blue);
    const char* what = v.recording == studio::Recording::in_air ? "IN THE AIR..." : v.recording == studio::Recording::finishing ? "LANDED: BACK TO THE STUDIO" : "RECORD A TRICK";
    text(d, heading, 0.026f * H, ImVec2(0.034f * display.x, 0.014f * H), skate_theme::white, what);
    text(d, bold, 0.019f * H, ImVec2(0.034f * display.x, 0.048f * H), IM_COL32(200, 200, 204, 255),
         v.status.empty() ? std::string("Skate and land the trick you want.") : v.status);
    const char* hint = "ESC / BACK: BACK TO THE STUDIO";
    const auto hw = text_size(heading, 0.019f * H, hint);
    text(d, heading, 0.019f * H, ImVec2(display.x - 0.034f * display.x - hw.x, 0.03f * H), skate_theme::white, hint);
    // A red dot, as a camera's.
    const float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
    d->AddCircleFilled(ImVec2(display.x - 0.034f * display.x - hw.x - 0.022f * H, 0.03f * H + hw.y * 0.5f), 0.008f * H,
                       IM_COL32(235, 50, 50, static_cast<int>(255 * pulse)));
}
void draw_animation_studio_loading() {
    const auto l = studio::loading();
    if (l.alpha <= 0) return;
    auto& s = state();
    const auto display = ImGui::GetIO().DisplaySize;
    ImFont* logo = s.menu.logo ? s.menu.logo : s.menu.heading ? s.menu.heading : ImGui::GetFont();
    ImFont* heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    ImFont* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const float H = display.y, k = std::clamp(H / 1080.0f, 0.45f, 3.0f);
    const float a = l.alpha;
    const auto fade = [a](ImU32 c) {
        return (c & 0x00ffffffu) | (static_cast<ImU32>(static_cast<float>((c >> 24) & 0xff) * a) << 24);
    };
    constexpr ImU32 paper = IM_COL32(236, 233, 226, 255), ink = IM_COL32(17, 18, 21, 255);
    auto* d = ImGui::GetForegroundDrawList();
    // Concrete: near black, skate.'s grain and scratches over it, darker at the edges.
    d->AddRectFilled(ImVec2(0, 0), display, fade(IM_COL32(19, 19, 19, 255)));
    if (!draw_studio_picture(d, StudioPicture::grain, ImVec2(0, 0), display, fade(IM_COL32(255, 255, 255, 150))))
        for (unsigned i = 0; i < 2600; ++i) {
            const float x = skate_theme::noise(i * 3 + 1) * display.x, y = skate_theme::noise(i * 3 + 2) * display.y;
            const auto shade = static_cast<int>(26 + skate_theme::noise(i * 3 + 3) * 40);
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + 1.6f * k, y + 1.6f * k), fade(IM_COL32(shade, shade, shade, 120)));
        }
    const ImU32 clear = IM_COL32(0, 0, 0, 0), dark = fade(IM_COL32(0, 0, 0, 170));
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, H * 0.25f), dark, dark, clear, clear);
    d->AddRectFilledMultiColor(ImVec2(0, H * 0.75f), display, clear, clear, dark, dark);
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x * 0.15f, H), dark, clear, clear, dark);
    d->AddRectFilledMultiColor(ImVec2(display.x * 0.85f, 0), display, clear, dark, dark, clear);

    // The wordmark, set tight: each letter a little into the one before.
    const auto tight = [&](const char* word, float size, float track, ImVec2 at, ImU32 colour, bool heavy) {
        float x = at.x;
        for (const char* c = word; *c; ++c) {
            const char glyph[2]{*c, 0};
            // Drawn three times a hair apart: a heavier cut than the font has.
            if (heavy) {
                d->AddText(logo, size, ImVec2(x - size * 0.008f, at.y), colour, glyph);
                d->AddText(logo, size, ImVec2(x + size * 0.008f, at.y), colour, glyph);
            }
            d->AddText(logo, size, ImVec2(x, at.y), colour, glyph);
            x += logo->CalcTextSizeA(size, FLT_MAX, 0, glyph).x + track * size;
        }
        return x - track * size - at.x;
    };
    const auto tight_width = [&](const char* word, float size, float track) {
        float x = 0;
        for (const char* c = word; *c; ++c) {
            const char glyph[2]{*c, 0};
            x += logo->CalcTextSizeA(size, FLT_MAX, 0, glyph).x + track * size;
        }
        return x - track * size;
    };
    const float word_size = 0.30f * H, word_track = -0.045f;
    const float word_w = tight_width("reskate.", word_size, word_track);
    const ImVec2 word_at(display.x * 0.5f - word_w * 0.5f, H * 0.215f);
    tight("reskate.", word_size, word_track, ImVec2(word_at.x + 5 * k, word_at.y + 7 * k), fade(IM_COL32(0, 0, 0, 140)), true);
    tight("reskate.", word_size, word_track, word_at, fade(paper), true);
    // Worn paper: the grain again, dark, over the letters (the dark concrete around hides it).
    draw_studio_picture(d, StudioPicture::grain, ImVec2(word_at.x - 20 * k, word_at.y), ImVec2(word_at.x + word_w + 20 * k, word_at.y + word_size * 1.1f),
                        fade(IM_COL32(30, 28, 25, 230)));

    // animator studio on a strip of blue tape under it.
    const float sub_size = 0.085f * H, sub_track = -0.03f;
    const float sub_w = tight_width("animator studio", sub_size, sub_track);
    const float tape_top = word_at.y + word_size * 0.99f, tape_h = 0.098f * H;
    const ImVec2 tape_a(display.x * 0.5f - sub_w * 0.5f - 0.03f * H, tape_top), tape_b(display.x * 0.5f + sub_w * 0.5f + 0.03f * H, tape_top + tape_h);
    if (!draw_studio_brush(d, tape_a, tape_b, fade(skate_theme::blue))) skate_theme::rough_rect(d, tape_a, tape_b, fade(skate_theme::blue), 7, 2.0f * k);
    const float sub_h = logo->CalcTextSizeA(sub_size, FLT_MAX, 0, "a").y;
    tight("animator studio", sub_size, sub_track, ImVec2(display.x * 0.5f - sub_w * 0.5f, tape_top + (tape_h - sub_h) * 0.5f - 0.004f * H), fade(ink), true);

    // Who built it, on a strip of paper.
    {
        const float label_size = 0.020f * H, names_size = 0.026f * H;
        const char* names = "22SH, FOURTH STUDIO, NORMS";
        const auto sw = text_size(bold, label_size, "BUILT BY"), nw = text_size(heading, names_size, names);
        const float strip_w = sw.x + nw.x + 0.06f * H, strip_h = 0.06f * H;
        const ImVec2 pa(display.x * 0.5f - strip_w * 0.5f, tape_top + tape_h + 0.03f * H), pb(pa.x + strip_w, pa.y + strip_h);
        if (!draw_studio_tile(d, pa, pb, 6 * k, fade(paper))) skate_theme::rough_rect(d, pa, pb, fade(paper), 11, 1.2f * k);
        text(d, bold, label_size, ImVec2(pa.x + 0.022f * H, pa.y + (strip_h - sw.y) * 0.5f), fade(IM_COL32(40, 40, 44, 255)), "BUILT BY");
        text(d, heading, names_size, ImVec2(pa.x + 0.038f * H + sw.x, pa.y + (strip_h - nw.y) * 0.5f), fade(ink), names);
    }

    // Bottom left: a tip, a new one every few seconds, and a bar that fills while it waits.
    {
        static constexpr std::array<const char*, 7> tips{
            "Pick a trick in TRICKS, then its pop: low, mid or high.",
            "Click a dot on the skater, or choose a joint in JOINTS, then drag to turn it.",
            "K keys the pose on this frame. Keys always sit on whole frames.",
            "G moves the hips or the board. X, Y and Z lock an axis.",
            "Save your trick and do it in game: your version plays on that trick.",
            "Copy to low, mid and high pop in PLAYBACK to fit every pop.",
            "On a controller: the left stick turns, RT and LT step frames, A keys."};
        const auto tip = tips[static_cast<std::size_t>(l.seconds / 4.0f) % tips.size()];
        const ImVec2 ta(0.034f * display.x, H - 0.115f * H);
        text(d, heading, 0.022f * H, ta, fade(skate_theme::blue), "TIP");
        text(d, bold, 0.019f * H, ImVec2(ta.x, ta.y + 0.03f * H), fade(IM_COL32(225, 225, 228, 255)), tip);
        const ImVec2 ba(ta.x, H - 0.05f * H), bb(display.x * 0.415f, H - 0.04f * H);
        const float r = (bb.y - ba.y) * 0.5f;
        d->AddRectFilled(ba, bb, fade(IM_COL32(255, 255, 255, 38)), r);
        // It fills towards the end without reaching it while it waits, and runs out when done.
        const float fill = l.what == studio::Loading::none ? 1.0f : 0.92f * (1.0f - std::exp(-l.seconds / 3.0f));
        if (fill > 0.01f) d->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * fill, bb.y), fade(skate_theme::blue), r);
    }
    // Bottom right: what is going on, then skate.'s own loading box with its kickflipping board.
    {
        const char* what = l.what == studio::Loading::leaving ? "BACK TO THE STREETS" : "SETTING UP THE STUDIO";
        const float box_h = 0.062f * H, box_w = 0.205f * H;
        const ImVec2 bb(display.x - 0.034f * display.x, H - 0.045f * H), ba(bb.x - box_w, bb.y - box_h);
        // skate.'s box: black, with a light rough edge.
        const float rim = 2.5f * k;
        if (!draw_studio_tile(d, ba, bb, 8 * k, fade(IM_COL32(205, 205, 205, 255)))) skate_theme::rough_rect(d, ba, bb, fade(IM_COL32(205, 205, 205, 255)), 5, 1.2f * k);
        const ImVec2 ia(ba.x + rim, ba.y + rim), ib(bb.x - rim, bb.y - rim);
        if (!draw_studio_tile(d, ia, ib, 8 * k, fade(IM_COL32(8, 8, 9, 255)))) skate_theme::rough_rect(d, ia, ib, fade(IM_COL32(8, 8, 9, 255)), 6, 1.2f * k);
        const char* dots[]{"Loading", "Loading.", "Loading..", "Loading..."};
        const auto loading_text = dots[static_cast<int>(l.seconds * 2.5f) % 4];
        const auto lw = text_size(bold, 0.021f * H, "Loading...");
        text(d, bold, 0.021f * H, ImVec2(ba.x + 0.024f * H, ba.y + (box_h - lw.y) * 0.5f), fade(skate_theme::white), loading_text);
        const ImVec2 fa(bb.x - box_h * 1.25f - 0.012f * H, ba.y + box_h * 0.1f), fb(bb.x - 0.012f * H, bb.y - box_h * 0.1f);
        if (!draw_studio_kickflip(d, fa, fb, l.seconds, fade(skate_theme::white))) {
            // Without the game's board: a wheel that spins.
            const ImVec2 c((fa.x + fb.x) * 0.5f, (fa.y + fb.y) * 0.5f);
            const float wr = (fb.y - fa.y) * 0.35f, t = static_cast<float>(ImGui::GetTime());
            d->AddCircle(c, wr, fade(skate_theme::white), 24, 3 * k);
            d->AddLine(c, ImVec2(c.x + std::cos(t * 6) * wr, c.y + std::sin(t * 6) * wr), fade(skate_theme::blue), 3 * k);
        }
        const auto ww = text_size(heading, 0.021f * H, what);
        text(d, heading, 0.021f * H, ImVec2(ba.x - 0.022f * H - ww.x, ba.y + (box_h - ww.y) * 0.5f), fade(skate_theme::white), what);
    }
}

void draw_animation_studio() {
    auto& s = state();
    if (!studio::is_open()) return;
    const auto v = studio::view();
    auto& u = ui();
    studio::advance(ImGui::GetIO().DeltaTime);
    const auto display = ImGui::GetIO().DisplaySize;
    Fonts f{s.menu.body ? s.menu.body : ImGui::GetFont(), s.menu.bold ? s.menu.bold : ImGui::GetFont(),
            s.menu.heading ? s.menu.heading : ImGui::GetFont(), s.menu.title_large ? s.menu.title_large : s.menu.title ? s.menu.title : ImGui::GetFont(),
            1.0f};
    // Any screen: laid out on a 1920 x 1080 page, scaled to fit the height and the game's menu frame
    // (the whole width up to 21:9, a 21:9 frame in the middle of anything wider).
    const float frame_w = std::min(display.x, display.y * 21.0f / 9.0f);
    const float left = (display.x - frame_w) * 0.5f, right = left + frame_w;
    f.k = std::clamp(std::min(display.y / 1080.0f, frame_w / 1700.0f), 0.45f, 3.0f);
    const float k = f.k;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##animation_studio", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    auto* d = ImGui::GetWindowDrawList();
    const float timeline_h = std::min(display.y * 0.34f, 360 * k);
    const float menu_x = left + 60 * k, menu_w = 520 * k;
    const ImVec2 view_min(u.menu_hidden ? left : menu_x + menu_w + 30 * k, 0), view_max(right, display.y - timeline_h);
    // The skater stands in the free space right of the menu.
    studio::set_view_shift((display.y * 0.5f - (view_min.y + view_max.y) * 0.5f) / display.y,
                           ((view_min.x + view_max.x) * 0.5f - display.x * 0.5f) / display.y);
    const auto project = projector(v, display);
    controller(v, project);
    viewport(d, f, v, project, view_min, view_max);
    if (!u.menu_hidden) {
        page_title(d, f, ImVec2(menu_x, 40 * k));
        tab_row(d, f, ImVec2(menu_x, 140 * k));
        const float list_top = 205 * k, card_h = 190 * k;
        const float list_bottom = display.y - timeline_h - card_h - 40 * k;
        list(d, f, v, ImVec2(menu_x + 16 * k, list_top), ImVec2(menu_w - 16 * k, std::max(120 * k, list_bottom - list_top)));
        card(d, f, v, ImVec2(menu_x, list_bottom + 18 * k), ImVec2(menu_w + 10 * k, card_h));
    }
    timeline(d, f, v, ImVec2(left, display.y - timeline_h), ImVec2(frame_w, timeline_h));
    std::string line = v.failure.empty() ? v.status : v.failure;
    if (ImGui::GetTime() < u.message_until) line = u.message;
    if (!v.has_clip) {
        const char* first = "PICK A TRICK";
        const std::string second = "Choose a trick in TRICKS: it opens here to be changed.";
        const auto w1 = text_size(f.heading, 30 * k, first);
        const float bw = std::max(w1.x + 60 * k, 520 * k);
        // In the empty timeline, clear of the skater.
        const ImVec2 a(left + (frame_w - bw) * 0.5f, display.y - timeline_h * 0.5f - 30 * k), b(a.x + bw, a.y + 120 * k);
        skate_theme::rough_rect(d, a, b, IM_COL32(234, 234, 232, 250), 101, k);
        text(d, f.heading, 30 * k, ImVec2(a.x + (bw - w1.x) * 0.5f, a.y + 16 * k), IM_COL32(20, 20, 22, 255), first);
        d->AddText(f.bold, 17 * k, ImVec2(a.x + 30 * k, a.y + 60 * k), IM_COL32(60, 60, 66, 255), second.c_str(), nullptr, bw - 60 * k);
    }
    if (!v.pose_seen) line = "Waiting for the skater's pose...";
    if (!line.empty()) {
        const auto w = text_size(f.bold, 15 * k, line.c_str());
        const ImVec2 at(view_min.x + 24 * k, view_max.y - 40 * k);
        skate_theme::rough_rect(d, ImVec2(at.x - 14 * k, at.y - 7 * k), ImVec2(at.x + w.x + 14 * k, at.y + w.y + 7 * k), IM_COL32(16, 16, 18, 230), 41, k * 0.6f);
        text(d, f.bold, 15 * k, at, v.failure.empty() ? white : skate_theme::danger, line);
    }
    prompts(d, f, v, view_max);
    if (v.confirm_close) close_warning(d, f, v, display);
    else keyboard(v);
    ImGui::End();
    ImGui::PopStyleVar(2);
}
} // namespace dingosdk::overlay::detail
