#include "overlay_internal.h"
#include "skate_style.h"
#include "animation_studio_images.h"
#include "Extension/AnimationStudio/anim_studio.h"
#include "Extension/AnimationStudio/native_asset_names.h"
#include "Extension/AnimationStudio/trick_batch.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
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
    bool rows_bar_held{};  // the joint list's scroll bar is being dragged
    bool list_bar_held{};  // the menu list's scroll bar is being dragged
    std::uint16_t rows_followed{0xffff}; // the joint the list last scrolled to
    // Channels: joints whose rows are opened in the timeline into rotation X, Y, Z (and location X, Y, Z).
    std::vector<std::uint16_t> opened;
    std::uint16_t rows_reveal{0xffff};  // a joint just opened: the timeline scrolls to show its channels
    double held_said_until{};           // Joint limits holding an edit back was said until then
    struct ChannelField {               // a channel's value box while it is dragged or typed into
        std::uint16_t joint{0xffff};
        int channel{-1};
        bool active{}, began{};
        float value{};                  // the number asked for (an angle read back can come out another way)
        studio::Offset base;            // the edit it started from
    } channel_field;
    bool bracket_began{};               // the held bracket has moved: its undo step is taken
    std::uint16_t channel_joint{0xffff};
    int channel{-1};                    // the channel row chosen last (-1 none)
    struct ChannelDrag {
        bool on{}, began{};
        int mode{}; // 0 not decided yet, 1 up and down (the value), 2 sideways (the time)
        std::uint16_t joint{};
        int channel{};
        float time{}, start{};
        studio::Offset base;
        ImVec2 from{};
    } channel_drag;
    // What the timeline is showing: free (zoomed by hand), the whole trick, or the custom animation's
    // own window. The last one is kept, so it follows the brackets as they are moved.
    enum class View { free, fit, region };
    View timeline_view{View::fit};
    int bracket_drag{};    // 1 the start bracket is held, 2 the end bracket
    float bracket_dragged_to{-1}; // the frame the held bracket was last moved to
    bool dragging_playhead{}, dragging_gizmo{}, orbiting{}, panning{};
    // A key being dragged along its row.
    std::optional<std::uint16_t> key_joint;
    float key_from{}, key_to{};
    bool key_moved{};
    // A box dragged with the right button over the rows, and the keys it was drawn around.
    bool band_held{};
    ImVec2 band_from{};
    // Keys chosen together (Ctrl + A, Shift + click, a box): dragged, moved and deleted as one.
    std::vector<std::pair<std::uint16_t, float>> picked;
    // The joint's state when a turn (mouse drag or stick) began, and how far it has gone since.
    studio::Offset drag_offset;
    studio::Vec3 drag_field{}; // a hand's or a foot's POSITION fields when the drag began
    studio::Quat drag_world{0, 0, 0, 1}, drag_parent{0, 0, 0, 1};
    float drag_parent_scale{1};
    ImVec2 drag_start{};
    int axis{-1}; // -1 free, 0 X, 1 Y, 2 Z
    bool local_axes{true}, move_mode{};
    std::uint16_t joint_row_for{0xffff}; // the joint JOINTS last showed as chosen
    // The gizmo on the chosen joint: three arrows to move along an axis, three rings to turn about one.
    int gizmo_handle{-1};      // the one being dragged: 0 to 2 the arrows, 3 to 5 the rings, -1 none
    int gizmo_hot{-1};         // the one under the mouse
    studio::Vec3 drag_point{}; // where the joint was when the drag began
    float ring_from{};         // the angle round the ring where a ring drag began
    bool drag_moved{};         // the mouse has gone far enough for the drag to count
    bool snap{};               // a foot moved in the air lands on the deck

    std::array<char, 64> name{}, author{};
    std::array<char, 64> pack_name{};
    int pack_row{}; // the row of the pack that was opened in PACKS, to come back to
    std::uint64_t names_for{};
    std::string message;
    double message_until{};
    std::string status_said;  // the studio's last line, and when it has been up long enough to have been read
    double status_until{};
    std::string confirm_delete;
    // Controller.
    bool pad_mode{}, playstation{};
    unsigned buttons{};
    double held_since[16]{};
    double repeat_at[16]{};
    bool l3_combo{}, stick_turning{}, stick_zoom{}, scrubbing{};
    bool shoulder_shifted{}; // a shoulder was held and the stick moved a bracket, so it is not a tab tap
    float bracket_at[2]{};   // where each bracket is being carried to, while a stick carries it
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
    bool hide_joints{};          // J: the dots and the gizmo are out of the way
    bool hide_ui{};              // H: nothing but the skater and the room, for filming and pictures
    bool anti_clip{true};        // a joint being placed is kept out of the body and the deck
    float clear_force{1.0f};     // how wide the body is taken to be: 0 nothing, 1 the full shapes
    bool shove_limbs{true};      // a limb that is run into gives way as well, not only what you move
    int camera_shot{};           // which of the shots the camera is set to, 0 while it is yours
    std::uint64_t clip_seen{};   // the clip the overlay last started clean on
    int warning_choice{};
    bool zoom_to_playhead{};
    std::array<float, 4> list_scroll{};
    std::array<int, 4> list_followed{-1, -1, -1, -1}; // the row the list last scrolled to, per tab
    std::string open_trick;    // TRICKS: the trick whose pops are shown ("" = the list of tricks)
    std::string trick_category; // TRICKS: the kind of trick being looked through ("" = the kinds)
    bool browse_assets{};
    bool asset_ready_only{}, asset_inspect{};
    std::array<char, 64> asset_query{};
    std::string asset_filter;
    std::string asset_document, asset_json_path;
    std::vector<std::pair<std::string, std::string>> asset_history;
    std::size_t asset_return_page{};
    std::string asset_category;
    std::size_t asset_page{};
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
    // The keys a bracket puts on every joint are the brackets' own: they are not chosen (as a box leaves them).
    const float end = v.bracket_end < 0 ? v.duration : v.bracket_end;
    for (const auto& t : v.tracks)
        if (!t.locked)
            for (const auto& key : t.keys)
                if (std::abs(key.time - v.bracket_start) >= 0.5f / fps && std::abs(key.time - end) >= 0.5f / fps)
                    u.picked.emplace_back(t.joint, key.time);
    say(u.picked.empty() ? "There are no keys yet." : std::to_string(u.picked.size()) + " keys chosen: drag one to move them all, Delete takes them off.");
}
// The chosen keys moved by `by` seconds, as one undo step: the far end first, so none lands on another.
void move_picked(float by, const studio::View& v) {
    auto& u = ui();
    if (u.picked.empty()) return;
    // The group moves as one. Inside the brackets it stays inside them (a key never lands on a bracket's own
    // key), outside them it stops at the ends of the clip.
    float first = u.picked.front().second, last = first;
    for (const auto& [joint, time] : u.picked) first = std::min(first, time), last = std::max(last, time);
    const float end = v.bracket_end < 0 ? v.duration : v.bracket_end;
    const float low = first > v.bracket_start + 0.5f / fps ? v.bracket_start + 1 / fps : 0.0f;
    const float high = last < end - 0.5f / fps ? end - 1 / fps : v.duration;
    by = std::clamp(by, std::min(0.0f, low - first), std::max(0.0f, high - last));
    if (std::abs(by) < 1e-4f) return;
    auto keys = u.picked;
    std::sort(keys.begin(), keys.end(), [by](const auto& a, const auto& b) { return by > 0 ? a.second > b.second : a.second < b.second; });
    studio::begin_change();
    for (auto& [joint, time] : keys) {
        const float to = time + by;
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
// ---- channels ------------------------------------------------------------------------------------
// A joint opened in the timeline shows a row for each number of its keys: the turn about X, Y and Z and,
// for what can be moved (the hips, the board), the move along X, Y and Z.
int channel_count(std::uint16_t joint) {
    // A hand or a foot is placed by the limb above it bending to reach (its position fields in the card), not
    // by a move of its own, so it has its turn channels only.
    const auto* e = studio::editable(joint);
    return e && e->moves && studio::limb_chain(joint) == 0 ? 6 : 3;
}
bool channels_open(std::uint16_t joint) {
    const auto& o = ui().opened;
    return std::find(o.begin(), o.end(), joint) != o.end();
}
// A channel edit that Joint limits held back is said, once in a while, so a number that will not move is
// explained rather than looking broken.
void note_held(const studio::View& v, std::uint16_t joint, const studio::Offset& requested) {
    if (!v.limits) return;
    // The turns compared, not one angle read back from them (that can come out another way round).
    const auto got = studio::offset_of(joint).rotation;
    const float d = std::abs(got[0] * requested.rotation[0] + got[1] * requested.rotation[1] + got[2] * requested.rotation[2] +
                             got[3] * requested.rotation[3]);
    if (2 * std::acos(std::clamp(d, 0.0f, 1.0f)) * 57.29578f <= 0.5f) return;
    auto& u = ui();
    if (ImGui::GetTime() < u.held_said_until) return;
    u.held_said_until = ImGui::GetTime() + 4;
    say("Held by Joint limits: switch them off in JOINTS to push it further.");
}
// Opens or closes a joint's channels. One joint at a time, unless `keep_others` (Ctrl + click).
void toggle_channels(std::uint16_t joint, bool keep_others) {
    auto& u = ui();
    if (channels_open(joint)) {
        std::erase(u.opened, joint);
        return;
    }
    if (!keep_others) u.opened.clear();
    u.opened.push_back(joint);
    u.rows_reveal = joint;
}
// Where each key was drawn this frame, for the box that picks them out.
struct DrawnKey {
    std::uint16_t joint;
    float time;
    ImVec2 at;
};
std::vector<DrawnKey>& drawn_keys() {
    static std::vector<DrawnKey> keys;
    return keys;
}
// The chosen keys are smoothed: each eases into its neighbours and shows round. This is the chosen keys
// and nothing else: KEYS has Smooth keys for tidying a whole track, which also drops keys and moves them
// onto frames.
void smooth_picked() {
    auto& u = ui();
    if (u.picked.empty()) return;
    studio::begin_change();
    std::string m;
    studio::smooth_chosen_keys(u.picked, m);
    say(m);
}
// A key marker: round when it has been smoothed, a diamond otherwise (a Linear or Step key is never round).
bool round_key(const studio::KeyView& key) noexcept { return key.smooth && key.ease == studio::Ease::smooth; }
void key_mark(ImDrawList* d, ImVec2 c, float r, ImU32 colour, bool smooth) {
    if (smooth) d->AddCircleFilled(c, r * 0.92f, colour, 18);
    else diamond(d, c, r, colour);
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
// A typed field in the studio's hand: its own type, the rough edge the buttons and rows have, and the
// same height as the button beside it, instead of the stock box in the stock font.
bool text_field(const Fonts& f, const char* id, std::array<char, 64>& buffer, float width, const char* hint = "Type a trick name") {
    const float k = f.k;
    const float h = 30 * k;
    // The field's own box stays, painted the shade the rows and buttons use: the studio draws on the
    // foreground list, so a frame drawn there would land on top of what is being typed.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(28, 29, 32, 240));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(40, 42, 46, 240));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(46, 48, 54, 245));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, blue_dim);
    ImGui::PushStyleColor(ImGuiCol_Text, white);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * k, (h - 14 * k) * 0.5f));
    ImGui::PushFont(f.bold);
    ImGui::SetNextItemWidth(width);
    const bool done = ImGui::InputTextWithHint(id, hint, buffer.data(), buffer.size());
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    return done;
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

// The joints that can be put somewhere: the hands, the feet, and the knees and elbows between them. The
// limb above bends to reach, so these are moved with the IK rather than by shifting the bone itself, and
// their POSITION fields are where they are from their rest. A knee or an elbow turns only the bone above
// it, so placing one never swings the hips or the chest (anim_studio's limb_chain).
bool limb_end(std::uint16_t joint) { return studio::limb_chain(joint) != 0; }
float dot3(const studio::Vec3& a, const studio::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// ---- turning the chosen joint (mouse drag or left stick) ---------------------------------------------
void begin_turn(const studio::View& v) {
    auto& u = ui();
    const auto* sel = joint_view(v, u.selected);
    if (!sel) return;
    u.drag_offset = studio::offset_of(u.selected);
    u.drag_point = sel->position;
    u.drag_field = limb_end(u.selected) ? studio::limb_position(u.selected) : studio::Vec3{};
    u.drag_world = sel->rotation;
    u.drag_parent = sel->parent_rotation;
    u.drag_parent_scale = sel->parent_scale;
    studio::begin_change();
    studio::set_playing(false);
}
// Drags the selected limb: the point under the mouse, on the camera's plane through the joint, is where it goes.
// The board's up, from its turn (a quaternion rotating the axis Y).
studio::Vec3 up_of(const studio::Quat& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)};
}
// ---- snapping ---------------------------------------------------------------------------------------
// Snapping holds a foot on the deck: it keeps its place along and across the board, but sits
// `feet_above_board` up the board's up, the height the game holds the feet at while riding.
//
// Wherever the board is, which is the point: on the ground that is the board the game has already put
// under the lower foot (board_on_feet), in the air the one the clip and its keys put there. Snapping a
// foot onto it is right either way, so this asks nothing about the trick but whether the board is known.
bool snapping_now(const studio::View& v) { return ui().snap && v.has_clip; }
studio::Vec3 on_the_deck(const studio::View& v, std::uint16_t joint, studio::Vec3 target) {
    if ((joint != 10 && joint != 343) || !snapping_now(v)) return target;
    const auto* board = joint_view(v, studio::board_joint);
    if (!board) return target;
    const auto up = up_of(board->rotation);
    const studio::Vec3 from{target[0] - board->position[0], target[1] - board->position[1], target[2] - board->position[2]};
    const float over = dot3(from, up) - studio::feet_above_board;
    for (int i = 0; i < 3; ++i) target[i] -= up[i] * over;
    return target;
}

// ---- keeping the skater out of himself ------------------------------------------------------------
// The body is taken as a handful of capsules - a line between two joints with a thickness - and the deck
// as a thin flat box. A joint being placed is pushed back out of any of them it has got inside, the
// shortest way out, which is enough to keep the meshes off one another without the pose being dragged
// about. His own limb is left out of the test, or it would push against itself.
//
// This holds what is being placed. Turning a bone can still swing the limb below it into the body: that
// would want the whole pose solved at once, which is a different thing from this.
struct Capsule {
    std::uint16_t from, to;
    float radius;
};
// The arms and the legs are a pair of capsules each; the torso and the head one each.
constexpr std::array<Capsule, 11> body_shape{{
    {7, 44, 0.150f},    // hips to chest
    {44, 101, 0.130f},  // chest to neck
    {101, 103, 0.105f}, // neck to head
    {276, 277, 0.052f}, {277, 278, 0.046f}, // left arm, forearm
    {47, 48, 0.052f},   {48, 49, 0.046f},   // right arm, forearm
    {341, 342, 0.075f}, {342, 343, 0.062f}, // left thigh, shin
    {8, 9, 0.075f},     {9, 10, 0.062f},    // right thigh, shin
}};
// How thick the joint being placed is taken to be.
float joint_radius(std::uint16_t joint) {
    switch (joint) {
    case 49: case 278: return 0.045f;  // a hand
    case 10: case 343: return 0.055f;  // a foot
    default: return 0.050f;            // a knee or an elbow
    }
}
// Which limb a joint belongs to, so its own bones are not tested against it. 0 is none.
int limb_of(std::uint16_t joint) {
    switch (joint) {
    case 275: case 276: case 277: case 278: return 1;
    case 46: case 47: case 48: case 49: return 2;
    case 341: case 342: case 343: case 344: return 3;
    case 8: case 9: case 10: case 11: return 4;
    default: return 0;
    }
}
// The point on the segment a to b nearest p, and how far p is from it.
float near_segment(const studio::Vec3& p, const studio::Vec3& a, const studio::Vec3& b, studio::Vec3& on) {
    studio::Vec3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = dot3(ab, ab);
    float t = 0;
    if (len > 1e-8f) t = std::clamp(((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1] + (p[2] - a[2]) * ab[2]) / len, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) on[i] = a[i] + ab[i] * t;
    return std::hypot(std::hypot(p[0] - on[0], p[1] - on[1]), p[2] - on[2]);
}
studio::Vec3 keep_clear(const studio::View& v, std::uint16_t joint, studio::Vec3 target) {
    const float force = std::clamp(ui().clear_force, 0.0f, 1.0f);
    if (!ui().anti_clip || !v.has_clip || force <= 0.01f) return target;
    // Against his own body the joint is as thick as the slider says; against the deck it is always its
    // whole self, since a shoe inside the board is wrong at every setting.
    const float r = joint_radius(joint) * force, deck_r = joint_radius(joint);
    const int mine = limb_of(joint);
    // Two passes: pushing out of one shape can put it inside another, and twice settles it in practice.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& c : body_shape) {
            if (limb_of(c.from) == mine && limb_of(c.to) == mine) continue; // his own limb
            const auto* a = joint_view(v, c.from);
            const auto* b = joint_view(v, c.to);
            if (!a || !b) continue;
            studio::Vec3 on{};
            const float away = near_segment(target, a->position, b->position, on);
            const float want = c.radius * force + r;
            if (away >= want) continue;
            if (away < 1e-4f) {
                target[1] += want; // dead centre of a bone: straight up, so there is a way out at all
                continue;
            }
            for (int i = 0; i < 3; ++i) target[i] = on[i] + (target[i] - on[i]) / away * want;
        }
        // The deck: a thin flat box about the board's mount, pushed out whichever of its faces is nearest.
        if (const auto* board = joint_view(v, studio::board_joint)) {
            constexpr std::array<float, 3> half{0.105f, 0.020f, 0.430f}; // across, thick, along
            // A deck is not a slab: it kicks up at the nose and the tail. Past the trucks the top of it
            // rises towards the tip, so a foot put on the tail sits on the kick and a hand reaching the
            // nose meets it where it really is.
            constexpr float flat_to = 0.165f, kick_rise = 0.075f;
            std::array<studio::Vec3, 3> axes{};
            for (int i = 0; i < 3; ++i) {
                studio::Vec3 e{};
                e[static_cast<std::size_t>(i)] = 1;
                axes[static_cast<std::size_t>(i)] = studio::rotate(board->rotation, e);
            }
            const studio::Vec3 from{target[0] - board->position[0], target[1] - board->position[1], target[2] - board->position[2]};
            std::array<float, 3> along{};
            for (int i = 0; i < 3; ++i) along[static_cast<std::size_t>(i)] = dot3(from, axes[static_cast<std::size_t>(i)]);
            // How far up the kick is here: nothing between the trucks, rising to the tips.
            const float out_along = std::clamp((std::abs(along[2]) - flat_to) / std::max(half[2] - flat_to, 1e-3f), 0.0f, 1.0f);
            const float kick = out_along * out_along * kick_rise;
            bool inside = std::abs(along[0]) < half[0] + deck_r && std::abs(along[2]) < half[2] + deck_r &&
                          along[1] < half[1] + deck_r + kick && along[1] > -half[1] - deck_r;
            if (!inside) continue;
            // A foot comes out of the top of the deck and nowhere else. He stands on his board: pushing it
            // out by whichever face is nearest put the foot under the deck whenever it had got a little
            // below the middle of it, and then it had to be dragged back through to get on top again.
            if (joint == 10 || joint == 343) {
                // Just under the height the game holds the feet at while riding, so a foot sits on the
                // deck rather than in its top face, and snapping can still take it the rest of the way.
                constexpr float on_top = studio::feet_above_board - 0.012f;
                const float up = on_top + kick - along[1]; // on the tail, that is up on the kick
                if (up > 0) for (int i = 0; i < 3; ++i) target[i] += axes[1][i] * up;
                continue;
            }
            // A hand can be under the board, holding it, so that one leaves by its shallowest face.
            int face = 0;
            float least = 1e9f;
            for (int i = 0; i < 3; ++i) {
                const float reach = half[static_cast<std::size_t>(i)] + deck_r + (i == 1 ? kick : 0.0f);
                const float out = reach - std::abs(along[static_cast<std::size_t>(i)]);
                if (out < least) least = out, face = i;
            }
            const float sign = along[static_cast<std::size_t>(face)] < 0 ? -1.0f : 1.0f;
            for (int i = 0; i < 3; ++i) target[i] += axes[static_cast<std::size_t>(face)][i] * least * sign;
        }
    }
    // The ground. Nothing goes under it at any setting: a hand or a foot through the floor shows in the
    // world, and no pose wants it. The joint's own thickness is kept clear of it, as with the deck.
    if (v.floor_valid) {
        const float lowest = v.floor + joint_radius(joint) * 0.6f;
        if (target[1] < lowest) target[1] = lowest;
    }
    return target;
}

// The nearest points on two segments, and how far apart they are. Clamped, so segments that pass each
// other give the pair at the ends rather than off the line.
float near_segments(const studio::Vec3& a0, const studio::Vec3& a1, const studio::Vec3& b0, const studio::Vec3& b1,
                    studio::Vec3& on_a, studio::Vec3& on_b) {
    studio::Vec3 da{a1[0] - a0[0], a1[1] - a0[1], a1[2] - a0[2]}, db{b1[0] - b0[0], b1[1] - b0[1], b1[2] - b0[2]};
    const studio::Vec3 r{a0[0] - b0[0], a0[1] - b0[1], a0[2] - b0[2]};
    const float aa = dot3(da, da), bb = dot3(db, db), rb = dot3(r, db), ra = dot3(r, da), ab = dot3(da, db);
    float s_t = 0, t_t = 0;
    const float denom = aa * bb - ab * ab;
    if (aa > 1e-8f && bb > 1e-8f) {
        s_t = denom > 1e-8f ? std::clamp((ab * rb - ra * bb) / denom, 0.0f, 1.0f) : 0.0f;
        t_t = std::clamp((ab * s_t + rb) / bb, 0.0f, 1.0f);
        s_t = std::clamp((ab * t_t - ra) / std::max(aa, 1e-8f), 0.0f, 1.0f);
    }
    for (int i = 0; i < 3; ++i) {
        on_a[i] = a0[i] + da[i] * s_t;
        on_b[i] = b0[i] + db[i] * t_t;
    }
    return std::hypot(std::hypot(on_a[0] - on_b[0], on_a[1] - on_b[1]), on_a[2] - on_b[2]);
}
// The force field proper: when the limb you are moving runs into another one, that other limb is shoved
// out of the way rather than the two being left through one another. The limb's end - its hand or its
// foot - is moved by the deepest overlap, and the IK bends the rest of it to follow, so an arm swings
// aside the way an arm would. The worst one each frame, which settles over a drag without a fight.
//
// The torso and the head are not shoved: his body is where it is, and it is the limb that gives way.
void shove_clear(const studio::View& v, std::uint16_t moving) {
    auto& u = ui();
    const int mine = limb_of(moving);
    const float force = std::clamp(u.clear_force, 0.0f, 1.0f);
    if (!u.shove_limbs || !u.anti_clip || !v.has_clip || !mine || force <= 0.01f) return;
    // Each limb by the joint on its end, which is the one that is moved to shift it.
    constexpr std::array<std::uint16_t, 4> ends{278, 49, 343, 10};
    studio::Vec3 best_push{};
    std::uint16_t best_end = 0;
    float worst = 0;
    const bool mine_is_leg = mine == 3 || mine == 4;
    for (const auto end : ends) {
        const int other = limb_of(end);
        if (other == mine) continue;
        // His legs live close together in any crouch, so one leg never shoves the other: that was the
        // whole body being thrown about while a foot was being placed.
        if (mine_is_leg && (other == 3 || other == 4)) continue;
        if (const auto* t = track_view(v, end); t && t->locked) continue;
        const auto* end_at = joint_view(v, end);
        if (!end_at) continue;
        for (const auto& theirs : body_shape) {
            if (limb_of(theirs.from) != other || limb_of(theirs.to) != other) continue;
            const auto* ta = joint_view(v, theirs.from);
            const auto* tb = joint_view(v, theirs.to);
            if (!ta || !tb) continue;
            for (const auto& ours : body_shape) {
                if (limb_of(ours.from) != mine || limb_of(ours.to) != mine) continue;
                const auto* oa = joint_view(v, ours.from);
                const auto* ob = joint_view(v, ours.to);
                if (!oa || !ob) continue;
                studio::Vec3 on_theirs{}, on_ours{};
                const float away = near_segments(ta->position, tb->position, oa->position, ob->position, on_theirs, on_ours);
                const float want = (theirs.radius + ours.radius) * force;
                if (away >= want) continue;
                // How much of it the end joint should answer for: a touch at the hand moves the hand, a
                // touch up at the shoulder barely moves it at all. Moving the end by a push felt at the
                // top of the limb is what sent it off to odd places.
                const float to_end = std::hypot(std::hypot(on_theirs[0] - end_at->position[0], on_theirs[1] - end_at->position[1]),
                                                on_theirs[2] - end_at->position[2]);
                const float share = std::clamp(1.0f - to_end / 0.55f, 0.0f, 1.0f);
                const float depth = (want - away) * share;
                if (depth <= worst) continue;
                worst = depth;
                best_end = end;
                if (away > 1e-4f)
                    for (int i = 0; i < 3; ++i) best_push[i] = (on_theirs[i] - on_ours[i]) / away * depth;
                else best_push = {0, depth, 0}; // dead centre: up, so there is a way out at all
            }
        }
    }
    if (!best_end || worst < 0.004f) return;
    const auto* at = joint_view(v, best_end);
    if (!at) return;
    // Eased, not snapped: a shove a frame is enough to clear a drag and keeps the arm's motion its own.
    studio::Vec3 target{};
    for (int i = 0; i < 3; ++i) target[i] = at->position[i] + best_push[i] * 0.5f;
    studio::drag_limb(best_end, target);
}

// ---- the gizmo -------------------------------------------------------------------------------------
// Unreal's handles, drawn the game's way: three arrows out of the chosen joint to move it along one axis,
// three rings round it to turn it about one. X is red, Y green, Z blue, and the handle under the mouse
// goes white. Dragging the joint itself is still a free turn, as it always was.
//
// The arrows show on the joints that can be moved: the board, the hips, and the hands and feet (those are
// moved with the limb bending to follow). The rings show on every joint.
constexpr int ring_points = 48;
struct Gizmo {
    bool valid{};
    ImVec2 centre{};
    bool arrows{};
    std::array<studio::Vec3, 3> axis{};                    // the axes a ring turns about, in the world
    std::array<ImVec2, 3> tip{};                           // where each arrow ends on screen
    std::array<float, 3> pixels_per_metre{};               // along each arrow
    std::array<std::array<ImVec2, ring_points>, 3> ring{}; // each ring, on screen
    std::array<bool, 3> ring_drawn{}, arrow_drawn{};
};
// The axis a handle works along, in the world. Moving goes along the frame the offsets are kept in: the
// joint's parent, and for the board the ground it starts on. Turning goes about the joint's own axes -
// except the board's, whose keys turn it about that same ground frame. Either can be the world's instead.
studio::Vec3 handle_axis(const studio::View& v, const studio::JointView& sel, std::uint16_t joint, int a, bool move, bool local) {
    studio::Vec3 unit{};
    unit[static_cast<std::size_t>(a)] = 1;
    if (!local) return unit;
    // A hand or a foot is moved by its POSITION fields, which are measured in the clip's own space: its
    // arrows point the way those numbers go, so pulling the X arrow moves X and nothing else.
    if (move && limb_end(joint)) return v.field_axes_valid ? studio::rotate(v.field_axes, unit) : unit;
    const bool parent_frame = move || joint == studio::board_joint;
    return studio::rotate(parent_frame ? sel.parent_rotation : sel.rotation, unit);
}
Gizmo gizmo_of(const studio::View& v, const Projector& p, float k) {
    Gizmo g;
    auto& u = ui();
    const auto* sel = joint_view(v, u.selected);
    if (!sel || !p.valid || p.focal <= 0 || !v.has_clip) return g;
    const auto at = p(sel->position);
    if (!at) return g;
    const studio::Vec3 d{sel->position[0] - p.origin[0], sel->position[1] - p.origin[1], sel->position[2] - p.origin[2]};
    const float depth = -dot3(d, p.back);
    if (depth < 0.05f) return g;
    const auto* e = studio::editable(u.selected);
    g.valid = true;
    g.centre = *at;
    g.arrows = (e && e->moves) || limb_end(u.selected);
    // A metre of the world is this many pixels at the joint, so the handles keep their size on screen.
    const float scale = p.focal / depth;
    const float arrow = 54 * k / scale, radius = 78 * k / scale;
    for (int a = 0; a < 3; ++a) {
        const auto index = static_cast<std::size_t>(a);
        g.axis[index] = handle_axis(v, *sel, u.selected, a, false, u.local_axes);
        if (g.arrows) {
            const auto move_axis = handle_axis(v, *sel, u.selected, a, true, u.local_axes);
            studio::Vec3 end{};
            for (int i = 0; i < 3; ++i) end[i] = sel->position[i] + move_axis[i] * arrow;
            if (const auto tip = p(end)) {
                g.tip[index] = *tip;
                const float on_screen = std::hypot(tip->x - at->x, tip->y - at->y);
                g.pixels_per_metre[index] = on_screen / arrow;
                // An arrow pointing almost at the camera is a dot: it cannot be dragged.
                g.arrow_drawn[index] = on_screen > 18 * k;
            }
        }
        // The ring: a circle round the joint across the axis, projected point by point, so it leans with
        // the camera the way a real circle would.
        const auto& n = g.axis[index];
        studio::Vec3 side = std::abs(n[2]) < 0.9f ? studio::Vec3{-n[1], n[0], 0} : studio::Vec3{0, -n[2], n[1]};
        const float len = std::sqrt(dot3(side, side));
        if (len < 1e-4f) continue;
        for (auto& c : side) c /= len;
        const studio::Vec3 other{n[1] * side[2] - n[2] * side[1], n[2] * side[0] - n[0] * side[2], n[0] * side[1] - n[1] * side[0]};
        bool whole = true;
        for (int i = 0; i < ring_points; ++i) {
            const float t = static_cast<float>(i) * 6.2831853f / ring_points;
            studio::Vec3 point{};
            for (int c = 0; c < 3; ++c) point[c] = sel->position[c] + (side[c] * std::cos(t) + other[c] * std::sin(t)) * radius;
            if (const auto on = p(point)) g.ring[index][static_cast<std::size_t>(i)] = *on;
            else whole = false;
        }
        g.ring_drawn[index] = whole;
    }
    return g;
}
// The handle under the mouse: the nearest arrow or ring within a few pixels, the arrows first.
int gizmo_pick(const Gizmo& g, ImVec2 mouse, float k) {
    if (!g.valid) return -1;
    int best = -1;
    float best_dist = 10 * k;
    for (int a = 0; a < 3; ++a) {
        const auto index = static_cast<std::size_t>(a);
        if (!g.arrow_drawn[index]) continue;
        const ImVec2 line{g.tip[index].x - g.centre.x, g.tip[index].y - g.centre.y};
        const float length = line.x * line.x + line.y * line.y;
        if (length < 1) continue;
        const float t = std::clamp(((mouse.x - g.centre.x) * line.x + (mouse.y - g.centre.y) * line.y) / length, 0.0f, 1.0f);
        if (t < 0.25f) continue; // close to the joint the free drag has it
        const float dist = std::hypot(g.centre.x + line.x * t - mouse.x, g.centre.y + line.y * t - mouse.y);
        if (dist < best_dist) best_dist = dist, best = a;
    }
    if (best >= 0) return best;
    for (int a = 0; a < 3; ++a) {
        const auto index = static_cast<std::size_t>(a);
        if (!g.ring_drawn[index]) continue;
        for (int i = 0; i < ring_points; ++i) {
            const auto point = g.ring[index][static_cast<std::size_t>(i)];
            const float dist = std::hypot(point.x - mouse.x, point.y - mouse.y);
            if (dist < best_dist) best_dist = dist, best = 3 + a;
        }
    }
    return best;
}
void draw_gizmo(ImDrawList* d, const Gizmo& g, int hot, float k) {
    if (!g.valid) return;
    constexpr std::array<ImU32, 3> colours{IM_COL32(236, 76, 72, 230), IM_COL32(118, 212, 96, 230), IM_COL32(74, 150, 255, 230)};
    constexpr ImU32 lit = IM_COL32(255, 248, 190, 255);
    for (int a = 0; a < 3; ++a) {
        const auto index = static_cast<std::size_t>(a);
        if (!g.ring_drawn[index]) continue;
        const bool on = hot == 3 + a;
        d->AddPolyline(g.ring[index].data(), ring_points, on ? lit : colours[index], ImDrawFlags_Closed, (on ? 3.4f : 2.0f) * k);
    }
    for (int a = 0; a < 3; ++a) {
        const auto index = static_cast<std::size_t>(a);
        if (!g.arrow_drawn[index]) continue;
        const bool on = hot == a;
        const auto colour = on ? lit : colours[index];
        const auto tip = g.tip[index];
        d->AddLine(g.centre, tip, colour, (on ? 3.4f : 2.2f) * k);
        // The head: a little triangle pointing the way the arrow goes.
        const float dx = tip.x - g.centre.x, dy = tip.y - g.centre.y;
        const float len = std::hypot(dx, dy);
        if (len < 1) continue;
        const float ux = dx / len, uy = dy / len, size = (on ? 9.0f : 7.0f) * k;
        d->AddTriangleFilled(tip, ImVec2(tip.x - ux * size * 2 - uy * size, tip.y - uy * size * 2 + ux * size),
                             ImVec2(tip.x - ux * size * 2 + uy * size, tip.y - uy * size * 2 - ux * size), colour);
    }
}
// The board never goes into the ground. Its lowest point is the bottom of a wheel, or the tip of a kick
// when it is stood on its tail, so the whole shape is measured rather than its mount: the eight corners of
// the deck and the wheels hanging under it, whichever sits lowest once the board is turned the way it is.
// The board is moved by its own keys rather than by the IK, so keeping clear never saw it.
float board_lowest_under(const studio::Quat& turn) {
    std::array<studio::Vec3, 3> axes{};
    for (int i = 0; i < 3; ++i) {
        studio::Vec3 e{};
        e[static_cast<std::size_t>(i)] = 1;
        axes[static_cast<std::size_t>(i)] = studio::rotate(turn, e);
    }
    constexpr float across = 0.105f, over = 0.020f, under = 0.087f, along = 0.430f;
    float lowest = 0;
    for (const float sx : {-across, across})
        for (const float sy : {-under, over})
            for (const float sz : {-along, along})
                lowest = std::min(lowest, axes[0][1] * sx + axes[1][1] * sy + axes[2][1] * sz);
    return lowest;
}
// Lifts the board's keys by however much of it has gone under the floor.
void keep_board_up(const studio::View& v, studio::Offset& next) {
    auto& u = ui();
    if (!v.floor_valid || !u.anti_clip) return;
    const auto* sel = joint_view(v, studio::board_joint);
    if (!sel) return;
    studio::Vec3 moved{};
    for (int i = 0; i < 3; ++i) moved[i] = next.offset[i] - u.drag_offset.offset[i];
    auto at = studio::rotate(u.drag_parent, moved); // its keys move it in the frame it starts in
    for (int i = 0; i < 3; ++i) at[i] += u.drag_point[i];
    const float lowest = at[1] + board_lowest_under(sel->rotation);
    if (lowest >= v.floor) return;
    const auto lift = studio::rotate(studio::conjugate(u.drag_parent), studio::Vec3{0, v.floor - lowest, 0});
    for (int i = 0; i < 3; ++i) next.offset[i] += lift[i];
}

// One axis of the gizmo: `amount` is metres to move along it or radians to turn about it, measured off the
// mouse by drag_gizmo.
void apply_axis(const studio::View& v, int a, float amount, bool move) {
    auto& u = ui();
    const auto* e = studio::editable(u.selected);
    const auto index = static_cast<std::size_t>(a);
    studio::Vec3 unit{};
    unit[index] = 1;
    if (move && limb_end(u.selected)) {
        // A hand or a foot is moved by its POSITION fields, so the arrow sets those numbers and the limb
        // above it bends to reach: pull the X arrow a centimetre and the X field goes up by 0.01.
        studio::Vec3 field = u.drag_field;
        if (u.local_axes) field[index] += amount;
        else {
            // A world arrow: the step is brought into the axes the fields are measured in.
            const auto step = v.field_axes_valid ? studio::rotate(studio::conjugate(v.field_axes), unit) : unit;
            for (int i = 0; i < 3; ++i) field[i] += step[i] * amount;
        }
        // Snapping works where the board is, so the foot's place is taken out to the world, put on the
        // deck, and brought back to the fields.
        if (snapping_now(v) && (u.selected == 10 || u.selected == 343) && v.field_axes_valid) {
            studio::Vec3 world{};
            for (int i = 0; i < 3; ++i) world[i] = field[i] - u.drag_field[i];
            world = studio::rotate(v.field_axes, world);
            for (int i = 0; i < 3; ++i) world[i] += u.drag_point[i];
            const auto landed = on_the_deck(v, u.selected, world);
            studio::Vec3 back{};
            for (int i = 0; i < 3; ++i) back[i] = landed[i] - u.drag_point[i];
            back = studio::rotate(studio::conjugate(v.field_axes), back);
            for (int i = 0; i < 3; ++i) field[i] = u.drag_field[i] + back[i];
        }
        // Out of the body and the deck, the same way snapping is done: taken to the world, pushed clear,
        // and brought back to the fields.
        if (u.anti_clip && v.field_axes_valid) {
            studio::Vec3 world{};
            for (int i = 0; i < 3; ++i) world[i] = field[i] - u.drag_field[i];
            world = studio::rotate(v.field_axes, world);
            for (int i = 0; i < 3; ++i) world[i] += u.drag_point[i];
            const auto clear = keep_clear(v, u.selected, world);
            studio::Vec3 back{};
            for (int i = 0; i < 3; ++i) back[i] = clear[i] - u.drag_point[i];
            back = studio::rotate(studio::conjugate(v.field_axes), back);
            for (int i = 0; i < 3; ++i) field[i] = u.drag_field[i] + back[i];
        }
        studio::set_limb_position(u.selected, field);
        return;
    }
    studio::Offset next = u.drag_offset;
    if (move && e && e->moves) {
        // A world arrow is brought into the frame the offsets are kept in; a local one is that frame.
        const auto step = u.local_axes ? unit : studio::rotate(studio::conjugate(u.drag_parent), unit);
        for (int i = 0; i < 3; ++i) next.offset[i] += step[i] * amount / std::max(u.drag_parent_scale, 0.01f);
    } else {
        const auto q = studio::axis_angle(unit, amount);
        const auto turn = u.local_axes ? q : studio::multiply(studio::multiply(studio::conjugate(u.drag_world), q), u.drag_world);
        next.rotation = studio::normalised(studio::multiply(u.drag_offset.rotation, turn));
        // The board's keys turn it about the axes of the ground it starts on, so its turn goes on the left,
        // in that frame: a local ring is already in it, a world one is brought into it.
        if (u.selected == studio::board_joint) {
            const auto ground = u.local_axes ? q : studio::multiply(studio::multiply(studio::conjugate(u.drag_parent), q), u.drag_parent);
            next.rotation = studio::normalised(studio::multiply(ground, u.drag_offset.rotation));
        }
    }
    if (u.selected == studio::board_joint) keep_board_up(v, next);
    studio::set_offset(u.selected, next);
}
// The mouse against the handle it holds: along an arrow, how far it has gone in metres; round a ring, the
// angle it has swept in radians.
void drag_gizmo(const studio::View& v, const Projector& p, const Gizmo& g, ImVec2 mouse) {
    auto& u = ui();
    const int handle = u.gizmo_handle;
    if (handle < 0 || !g.valid) return;
    if (handle < 3) {
        const auto index = static_cast<std::size_t>(handle);
        const float dx = g.tip[index].x - g.centre.x, dy = g.tip[index].y - g.centre.y;
        const float len = std::hypot(dx, dy);
        if (len < 1 || g.pixels_per_metre[index] < 1e-3f) return;
        const float along = ((mouse.x - u.drag_start.x) * dx + (mouse.y - u.drag_start.y) * dy) / len;
        apply_axis(v, handle, along / g.pixels_per_metre[index], true);
        return;
    }
    const int a = handle - 3;
    float swept = std::atan2(mouse.y - g.centre.y, mouse.x - g.centre.x) - u.ring_from;
    while (swept > 3.14159265f) swept -= 6.2831853f;
    while (swept < -3.14159265f) swept += 6.2831853f;
    // Screen y runs down, so a sweep that reads positive turns the axis pointing into the screen; an axis
    // pointing back at the camera turns the other way.
    apply_axis(v, a, dot3(g.axis[static_cast<std::size_t>(a)], p.back) < 0 ? swept : -swept, false);
}

// Drags the selected limb: the mouse's ray meets the plane of the board (through the joint, facing the board's
// up), so the foot slides over the board as the board moves. Without a board in the view, the camera's plane.
void drag_limb_to(const studio::View& v, const Projector& p, ImVec2 mouse) {
    // Only when the mouse has moved: each update rebuilds the keys, so a still mouse costs nothing.
    static ImVec2 last{-1e9f, -1e9f};
    if (std::abs(mouse.x - last.x) < 0.5f && std::abs(mouse.y - last.y) < 0.5f) return;
    last = mouse;
    const auto* sel = joint_view(v, ui().selected);
    if (!sel || !p.valid || p.focal <= 0) return;
    const studio::Vec3 d{sel->position[0] - p.origin[0], sel->position[1] - p.origin[1], sel->position[2] - p.origin[2]};
    const float depth = -(d[0] * p.back[0] + d[1] * p.back[1] + d[2] * p.back[2]);
    if (depth < 0.05f) return;
    // The ray through the mouse: from the camera, along -back at the focal length, sideways by the pixel.
    const float sx = (mouse.x - p.cx) / p.focal, sy = -(mouse.y - p.cy) / p.focal;
    studio::Vec3 ray{};
    for (int i = 0; i < 3; ++i) ray[i] = -p.back[i] + p.right[i] * sx + p.up[i] * sy;
    studio::Vec3 normal{0, 0, 0};
    const auto* board = joint_view(v, studio::board_joint);
    if (board) normal = up_of(board->rotation);
    else normal = p.back; // the camera's plane
    const float facing = ray[0] * normal[0] + ray[1] * normal[1] + ray[2] * normal[2];
    studio::Vec3 target{};
    if (std::abs(facing) > 1e-4f) {
        const float t = ((sel->position[0] - p.origin[0]) * normal[0] + (sel->position[1] - p.origin[1]) * normal[1] +
                         (sel->position[2] - p.origin[2]) * normal[2]) / facing;
        if (t <= 0) return;
        for (int i = 0; i < 3; ++i) target[i] = p.origin[i] + ray[i] * t;
    } else {
        for (int i = 0; i < 3; ++i) target[i] = p.origin[i] - p.back[i] * depth + p.right[i] * (sx * depth) + p.up[i] * (sy * depth);
    }
    studio::drag_limb(ui().selected, keep_clear(v, ui().selected, on_the_deck(v, ui().selected, target)));
}
// `dx`, `dy`: how far the turn has gone, in screen pixels at 1080 lines.
void apply_turn(const studio::View& v, const Projector& p, float dx, float dy, bool move) {
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
    if (u.selected == studio::board_joint) keep_board_up(v, next);
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
void save_now() {
    std::string m;
    studio::save(m);
    say(m);
}

// ---- the menu: skate.'s own list menu (Trick List), with tabs ---------------------------------------
enum class Tab { tricks, joints, keys, packs, settings };
constexpr std::array<const char*, 5> tab_names{"TRICKS", "JOINTS", "KEYS", "PACKS", "SETTINGS"};
// Back, one step, in the TRICKS tab: out of a trick's pops, or up the native asset tree. True when it
// went back, so Esc and B only fall through to anything else when there was nowhere to go.
bool go_back_in_tricks() {
    auto& u = ui();
    if (static_cast<Tab>(u.tab) != Tab::tricks) return false;
    if (!u.open_trick.empty()) {
        u.open_trick.clear();
        u.row = u.trick_row;
        u.message.clear();
        return true;
    }
    if (!u.trick_category.empty()) {
        u.trick_category.clear();
        u.row = u.trick_row;
        u.message.clear();
        return true;
    }
    if (u.browse_assets) {
        if (u.asset_category.empty()) u.browse_assets = false;
        else {
            const auto slash = u.asset_category.rfind('/');
            u.asset_category = slash == std::string::npos ? "" : u.asset_category.substr(0, slash);
        }
        u.asset_page = 0;
        u.row = 0;
        u.message.clear();
        return true;
    }
    return false;
}
struct Row {
    std::string label, value, about;
    std::function<void()> act;
    bool enabled{true}, on{};
    std::string library; // a saved animation's name (Delete takes it out)
    std::optional<std::uint16_t> joint; // JOINTS: the joint this row chooses
    bool slider{};       // JOINTS: Keep clear, drawn as a slider
    bool info{};         // information: it can be chosen to read its help, and does nothing
};
// A row that only tells you something. Unlike a heading it can be chosen, so its help can be read.
Row info_row(std::string label, std::string value, std::string about) {
    Row row{std::move(label), std::move(value), std::move(about), {}, false};
    row.info = true;
    return row;
}
// What can be chosen in a list: anything but a heading.
bool selectable(const Row& row) noexcept { return static_cast<bool>(row.act) || row.info; }
// JOINTS: the highlight follows the chosen joint - on a joint's row, or when a joint was chosen elsewhere (a
// dot on the skater, the timeline) - and stays on an option row otherwise. Used by the list, the keys and the pad.
int joints_row(const std::vector<Row>& rows) {
    auto& u = ui();
    const bool joint_changed = u.joint_row_for != u.selected;
    u.joint_row_for = u.selected;
    const bool on_joint = u.row < 0 || u.row >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(u.row)].joint;
    if (on_joint || joint_changed)
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].joint && *rows[i].joint == u.selected) return static_cast<int>(i);
    return u.row;
}
// ---- the shots ------------------------------------------------------------------------------------
// Set camera angles to work in, the ones a skate part is actually shot from. Each is the orbit the studio
// already drives - turned and tilted about the skater, at a distance - and what it holds in the middle, so
// a shot follows him and stays on while you pose and while it plays. Moving the camera yourself puts it
// back to FREE without losing where you were.
struct Shot {
    const char* name;
    float yaw, pitch, distance;
    std::uint16_t on; // the joint it holds in the middle, all_joints for the whole skater
};
constexpr std::array<Shot, 6> shots{{
    {"FREE", 0, 0, 0, studio::all_joints}, // stays where it is left; the rest travel with him
    {"CLOSE", 0.60f, 0.14f, 1.60f, studio::hips_joint},
    {"BOARD", 0.90f, 0.13f, 1.25f, studio::board_joint},
    {"CINEMATIC", 1.15f, -0.04f, 4.00f, studio::all_joints},
    {"FILMER", 2.50f, 0.10f, 1.15f, studio::board_joint},
    {"SKATER", 3.00f, 0.22f, 0.95f, 103},
}};
void take_shot(const studio::View& v, int which) {
    auto& u = ui();
    u.camera_shot = std::clamp(which, 0, static_cast<int>(shots.size()) - 1);
    const auto& shot = shots[static_cast<std::size_t>(u.camera_shot)];
    // A set shot travels with him; the free camera stays where it is put and he goes past it.
    studio::set_camera_follow(u.camera_shot != 0);
    if (!u.camera_shot) {
        studio::set_focus(studio::all_joints);
        say("Free camera: it stays where you leave it.");
        return;
    }
    studio::Orbit orbit = v.orbit;
    orbit.yaw = shot.yaw;
    orbit.pitch = shot.pitch;
    orbit.distance = shot.distance;
    orbit.pan = {};
    studio::set_orbit(orbit);
    studio::set_focus(shot.on);
    say(std::string(shot.name) + " camera.");
}
void next_shot(const studio::View& v, int by) {
    const int count = static_cast<int>(shots.size());
    take_shot(v, (ui().camera_shot + by % count + count) % count);
}
// The strip that shows it, at the top of the view: the name between two arrows, clicked or scrolled with V.
void shot_strip(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 area_min, ImVec2 area_max) {
    auto& u = ui();
    const float k = f.k;
    const auto& shot = shots[static_cast<std::size_t>(std::clamp(u.camera_shot, 0, static_cast<int>(shots.size()) - 1))];
    const float w = 190 * k, h = 30 * k;
    const ImVec2 a(area_max.x - w - 22 * k, area_min.y + 18 * k), b(a.x + w, a.y + h);
    skate_theme::rough_rect(d, a, b, IM_COL32(12, 12, 14, 190), 41, k * 0.7f);
    const float fs = 13 * k;
    const auto t = text_size(f.bold, fs, shot.name);
    text(d, f.bold, fs, ImVec2((a.x + b.x) * 0.5f - t.x * 0.5f, (a.y + b.y) * 0.5f - t.y * 0.5f),
         u.camera_shot ? blue_light : IM_COL32(255, 255, 255, 150), shot.name);
    const auto arrow = [&](const char* glyph, float x, int by) {
        ImGui::SetCursorScreenPos(ImVec2(x, a.y));
        ImGui::PushID(by);
        const bool hit = ImGui::InvisibleButton("##shot", ImVec2(28 * k, h));
        const bool over = ImGui::IsItemHovered();
        ImGui::PopID();
        const auto g = text_size(f.bold, fs, glyph);
        text(d, f.bold, fs, ImVec2(x + 14 * k - g.x * 0.5f, (a.y + b.y) * 0.5f - g.y * 0.5f), over ? white : IM_COL32(255, 255, 255, 120), glyph);
        if (hit) next_shot(v, by);
    };
    arrow("<", a.x + 4 * k, -1);
    arrow(">", b.x - 32 * k, 1);
    text(d, f.bold, 10 * k, ImVec2(a.x, b.y + 4 * k), IM_COL32(255, 255, 255, 70), "V  CAMERA");
}

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
        const auto assets = studio::native_assets();
        if (u.browse_assets) {
            if (!u.asset_document.empty()) {
                const auto document = studio::native_asset_fields(u.asset_document, u.asset_json_path, u.asset_page);
                rows.push_back({"< Back", "", "Return to the parent field, referenced document, or native tree.", [] {
                    auto& s = ui();
                    if (!s.asset_json_path.empty()) {
                        const auto slash = s.asset_json_path.rfind('/');
                        s.asset_json_path = slash == std::string::npos ? "" : s.asset_json_path.substr(0, slash);
                        s.asset_page = 0;
                    } else if (!s.asset_history.empty()) {
                        s.asset_document = s.asset_history.back().first; s.asset_json_path = s.asset_history.back().second;
                        s.asset_history.pop_back(); s.asset_page = 0;
                    } else { s.asset_document.clear(); s.asset_page = s.asset_return_page; }
                    s.row = 0;
                }});
                rows.push_back({"NATIVE DATA", document.title, "Read-only native authoring data. References are static dependencies, not proof of runtime execution order.", {}, false});
                if (!u.asset_json_path.empty()) rows.push_back({"Field path", u.asset_json_path, "Objects and arrays can be expanded; values are not modified.", {}, false});
                if (!document.error.empty()) rows.push_back({"Data unavailable", document.error, document.error, {}, false});
                if (document.pages > 1) rows.push_back({"Next page", std::to_string(u.asset_page + 1) + "/" + std::to_string(document.pages), "Next 50 fields or array values.", [pages = document.pages] { ui().asset_page = (ui().asset_page + 1) % pages; ui().row = 0; }});
                for (const auto& field : document.fields) {
                    rows.push_back({field.label, field.value, field.expandable ? "Expand this native object or array." : field.value,
                        [path = field.path] { ui().asset_json_path = path; ui().asset_page = 0; ui().row = 0; }, field.expandable});
                    if (!field.reference.empty()) rows.push_back({"Open reference: " + field.label, ">", "Follow the referenced native partition; missing documents remain explicit.", [ref = field.reference] {
                        auto& s = ui(); s.asset_history.emplace_back(s.asset_document, s.asset_json_path);
                        s.asset_document = ref; s.asset_json_path.clear(); s.asset_page = 0; s.row = 0;
                    }});
                }
                break;
            }
            rows.push_back({"< Back", "", "Browse parent folder, or clear the global search.", [] {
                auto& state = ui();
                if (!state.asset_filter.empty()) { state.asset_filter.clear(); state.asset_query.fill(0); }
                else if (state.asset_category.empty()) state.browse_assets = false;
                else {
                    const auto slash = state.asset_category.rfind('/');
                    state.asset_category = slash == std::string::npos ? "" : state.asset_category.substr(0, slash);
                }
                state.asset_page = 0; state.row = 0;
            }});
            rows.push_back({!u.asset_filter.empty() ? "SEARCH RESULTS / ALL FOLDERS" : u.asset_category.empty() ? "ANIMATION FOLDERS" : upper(u.asset_category), "", "Folders and readable names are inferred from native names; the original identity remains in the description.", {}, false});
            rows.push_back({"Show", u.asset_ready_only ? "Editable clips" : "All assets", "Filter the whole tree to clips with an available preview.", [] { ui().asset_ready_only = !ui().asset_ready_only; ui().asset_page = 0; ui().row = 0; }});
            rows.push_back({"Selection action", u.asset_inspect ? "Inspect native data" : "Open preview", "Inspect data opens the full typed field tree and references, including for editable clips.", [] { ui().asset_inspect = !ui().asset_inspect; }});
            std::unordered_map<std::string_view, const studio::GameTrick*> available;
            for (const auto& trick : list) if (!trick.asset.empty()) available.emplace(trick.asset, &trick);
            std::map<std::string, std::pair<std::size_t, std::size_t>> children;
            std::vector<const studio::NativeAsset*> leaves;
            const auto prefix = u.asset_category.empty() ? "" : u.asset_category + "/";
            const auto terms = studio::native_search_terms(u.asset_filter);
            for (const auto& asset : *assets) {
                const bool ready = available.contains(asset.asset);
                if (u.asset_ready_only && !ready) continue;
                if (!u.asset_filter.empty()) {
                    if (!studio::native_search_matches(asset.search, terms)) continue;
                    leaves.push_back(&asset);
                    continue;
                }
                if (asset.category == u.asset_category) leaves.push_back(&asset);
                else if (asset.category.starts_with(prefix)) {
                    const auto rest = asset.category.substr(prefix.size());
                    auto& counts = children[rest.substr(0, rest.find('/'))];
                    ++counts.first; counts.second += ready;
                }
            }
            std::sort(leaves.begin(), leaves.end(), [](const auto* a, const auto* b) {
                return a->label == b->label ? a->asset < b->asset : a->label < b->label;
            });
            if (!u.asset_filter.empty()) rows.push_back({std::to_string(leaves.size()) + " results", "", "Search includes readable names, aliases, original names and types across every folder. All search words must match.", {}, false});
            if (assets->empty())
                rows.push_back({"No animation index on this install", "",
                                "The native animation tree is built from a dump of the game's own asset names (contrib/animator-studio). "
                                "Without it there is nothing here to browse or search; the tricks you can edit are in the list behind this one.",
                                {}, false});
            else if (leaves.empty() && children.empty())
                rows.push_back({"No animations found", "", "Try fewer words, another trick name, or Show: All assets.", {}, false});
            for (const auto& [child, counts] : children) {
                rows.push_back({child, std::to_string(counts.second) + " editable / " + std::to_string(counts.first) + " >", "Browse this branch of the native animation inventory.", [path = prefix + child] {
                    ui().asset_category = path; ui().asset_page = 0; ui().row = 0;
                }});
            }
            constexpr std::size_t page_size = 50;
            const auto pages = (leaves.size() + page_size - 1) / page_size;
            if (pages) u.asset_page = std::min(u.asset_page, pages - 1);
            if (pages > 1) {
                rows.push_back({"Next page", std::to_string(u.asset_page + 1) + "/" + std::to_string(pages), "Show the next 50 assets (wraps to the first page).", [pages] {
                    ui().asset_page = (ui().asset_page + 1) % pages; ui().row = 2;
                }});
                rows.push_back({"Previous page", "", "Show the previous 50 assets.", [pages] {
                    ui().asset_page = (ui().asset_page + pages - 1) % pages; ui().row = 2;
                }});
            }
            for (std::size_t i = u.asset_page * page_size; i < std::min(leaves.size(), (u.asset_page + 1) * page_size); ++i) {
                const auto& asset = *leaves[i];
                const auto ready = available.find(asset.asset);
                const auto& label = asset.label;
                const auto description = asset.context + ". Folder: " + asset.category + ". Source: " + asset.asset + ". " + asset.detail;
                if (ready != available.end() && !u.asset_inspect) rows.push_back({label, "Editable", description, [t = *ready->second] {
                    std::string m; studio::load_game_trick(t, m); say(m); ui().names_for = 0;
                }});
                else rows.push_back({label, ready != available.end() ? "Editable / data" : asset.decode_state == "metadata" ? "Metadata" :
                    asset.decode_state == "unsupported" ? "Unsupported preview" : asset.decode_state == "error" ? "Decode failed" : asset.decode_state == "pending" ? "Queued" : "Indexed",
                    description, [name = asset.asset] {
                    auto& s = ui(); s.asset_return_page = s.asset_page; s.asset_document = name;
                    s.asset_json_path.clear(); s.asset_history.clear(); s.asset_page = 0; s.row = 0;
                }});
            }
            break;
        }
        const auto asset_entry = [&] {
            if (!assets->empty()) rows.push_back({"Native asset tree", std::to_string(assets->size()) + " >", "Browse clips, graph states, transitions, grind phases and parameters. Indexed does not mean editable.", [] {
                ui().browse_assets = true; ui().asset_category.clear(); ui().asset_page = 0; ui().row = 0;
            }});
            rows.push_back({"Reload native library", std::to_string(list.size()) + " clips", "Refresh converted native clips after importing or extracting more animations.", [] {
                std::string m; studio::refresh_game_tricks(m); say(m);
            }});
        };
        const auto record_entries = [&] {
            rows.push_back({"RECORD", "", "", {}, false});
            rows.push_back({"Record a trick", "", record_about, [] { studio::record_next_trick(); }});
            rows.push_back({"Record all tricks", std::any_of(list.begin(), list.end(), [](const auto& t) { return t.recorded; }) ? "Has recorded takes" : "", "Records the game's trick gestures on a solo practice stage, with the real board physics. Esc/Back stops after the current take.", [] { studio::request_record_all(); }});
        };
        if (list.empty()) {
            record_entries();
            asset_entry();
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
            for (const char* want : {"high", "mid", "low", ""})
            for (const auto& t : list) {
                if (t.trick != u.open_trick || t.pop != want) continue;
                const std::string pop = t.preview_only ? "Edit this piece" : t.pop == "low" ? "Low pop" : t.pop == "mid" ? "Mid pop" : t.pop == "high" ? "High pop" : "Native clip";
                Row row{pop, t.preview_only ? "Piece" : t.recorded ? "Recorded" : "Native", t.preview_only ? "A piece of the game's own animation - a setup, a loop, an exit - rather than a whole trick. It opens and saves like any other and is kept by the clip it came from. In game your edit plays whenever the game plays this piece: the game never says which piece it plays, so the studio knows it by the skater's pose, about a quarter of a second in. A piece that barely moves (a held pose) cannot be told apart from standing still." :
                        "Opens the game's " + t.label + " with a " + t.pop + " pop to change it. Native board physics and graph constraints are not included.",
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
        // Searching: the tricks whose name, category or pop matches every word typed, and nothing else, so
        // the one you are after is one line away rather than down a folder.
        const auto trick_terms = studio::native_search_terms(u.asset_filter);
        if (!trick_terms.empty()) {
            rows.push_back({"< Clear search", "", "Back to the whole list of tricks.", [] {
                                auto& state = ui();
                                state.asset_filter.clear();
                                state.asset_query.fill(0);
                                state.row = 0;
                            }});
            std::size_t found = 0;
            std::string seen;
            for (const auto& t : list) {
                if (t.trick == seen) continue;
                if (!studio::native_search_matches(studio::native_search_key(t.label + " " + t.trick + " " + t.category), trick_terms)) continue;
                seen = t.trick;
                ++found;
                const int index = static_cast<int>(rows.size());
                Row row{t.label, upper(t.category), "Choose the pop next: low, mid or high.", [name = t.trick, index] {
                            auto& state = ui();
                            state.open_trick = name;
                            state.trick_row = index;
                            state.row = 2;
                        }};
                row.on = v.trick == t.trick;
                rows.push_back(std::move(row));
            }
            rows.insert(rows.begin() + 1, {found ? std::to_string(found) + (found == 1 ? " TRICK" : " TRICKS") : "NO TRICK OF THAT NAME", "", "", {}, false});
            if (!found)
                rows.push_back({"Search every animation", assets->empty() ? "no index" : std::to_string(assets->size()) + " assets",
                                assets->empty() ? "The native animation index has not been built on this install, so there is nothing beyond the tricks above to search. "
                                                  "It is made by contrib/animator-studio, from a dump of the game's own asset names."
                                                : "Look through the game's whole animation tree for these words, not only the tricks you can edit.",
                                [] { ui().browse_assets = true; ui().row = 0; }, !assets->empty()});
            break;
        }
        // The tricks in their own folders. Hundreds of them in one list is not a list anybody reads: the
        // kinds are the top of it, and a kind opens to the tricks in it.
        if (!u.trick_category.empty()) {
            rows.push_back({"< Back", "", "Back to the kinds of trick.", [] {
                                auto& state = ui();
                                state.trick_category.clear();
                                state.row = state.trick_row;
                            }});
            rows.push_back({upper(u.trick_category), "", "", {}, false});
            std::string trick;
            for (const auto& t : list) {
                if (t.preview_only || t.category != u.trick_category || t.trick == trick) continue; // each trick once
                trick = t.trick;
                const int index = static_cast<int>(rows.size());
                Row row{t.label, ">", "Choose the pop next: low, mid or high.", [name = t.trick, index] {
                            auto& state = ui();
                            state.open_trick = name;
                            state.trick_row = index;
                            state.row = 2; // the first pop, under the trick's heading
                        }};
                row.on = v.trick == t.trick;
                rows.push_back(std::move(row));
            }
            break;
        }
        record_entries();
        rows.push_back({"Tricks to edit", "", "", {}, false});
        // Each kind once, with how many tricks are in it, in the order the catalog has them.
        {
            // The ones that can be opened and changed. The native tree's thousands of previews are not
            // tricks to edit and have their own way in, below.
            std::vector<std::pair<std::string, std::size_t>> kinds;
            std::string seen_trick;
            for (const auto& t : list) {
                if (t.preview_only || t.trick == seen_trick) continue;
                seen_trick = t.trick;
                const auto found = std::find_if(kinds.begin(), kinds.end(), [&](const auto& kind) { return kind.first == t.category; });
                if (found == kinds.end()) kinds.emplace_back(t.category, 1);
                else ++found->second;
            }
            for (const auto& [name, count] : kinds) {
                Row row{upper(name), std::to_string(count) + " >", "Open this kind of trick.", [name, index = static_cast<int>(rows.size())] {
                            auto& state = ui();
                            state.trick_category = name;
                            state.trick_row = index;
                            state.row = 2; // the first trick, under the heading
                        }};
                row.on = !v.trick.empty() && std::any_of(list.begin(), list.end(),
                                                         [&](const auto& t) { return t.trick == v.trick && t.category == name; });
                rows.push_back(std::move(row));
            }
        }
        rows.push_back({"Pieces of the game's animation", "", "", {}, false});
        asset_entry();
        break;
    }
    case Tab::joints: {
        // How a drag or the stick poses the chosen joint, what keeps it clear, then the joints themselves.
        rows.push_back({"Posing", "", "", {}, false});
        rows.push_back({"Move or turn", u.move_mode ? "move" : "turn",
                        "Turn: a drag or the left stick turns the joint. Move: it moves the hips or the board, and pulls a hand or "
                        "a foot to where you drag it, the limb bending to follow. Keys: R turns, G moves.",
                        [] { ui().move_mode = !ui().move_mode; ui().axis = -1; }});
        rows.push_back({"Axis", u.axis == 0 ? "X" : u.axis == 1 ? "Y" : u.axis == 2 ? "Z" : "any",
                        "The one axis a drag or the left stick turns the joint about (or moves it along), or any. Keys: X, Y, Z.",
                        [] { ui().axis = ui().axis >= 2 ? -1 : ui().axis + 1; }});
        rows.push_back({"Gizmo alignment", u.local_axes ? "object" : "world",
                        "Which way the gizmo's arrows and rings lie: along the joint's own X, Y and Z (object), or the world's "
                        "(world). The board's own is the ground it starts on, because that is the frame its keys are in. Key: L.",
                        [] { ui().local_axes = !ui().local_axes; }});
        rows.push_back({"Solo", v.solo ? "on" : "off",
                        "On: a turn moves that joint alone. The joints hanging off it are turned back by as much, so the limb below "
                        "keeps the way it faces instead of swinging with its parent. Bones are rigid, so it still travels with the "
                        "joint. Key: S.",
                        [on = v.solo] { studio::set_solo(!on); }});
        rows.push_back({"Snapping", u.snap ? "on" : "off",
                        "On: a foot you move lands on the deck, at the height the feet ride at, and slides along the board "
                        "instead of through it or over it. Key: N.",
                        [] { ui().snap = !ui().snap; }});
        rows.push_back({"Joint limits", v.limits ? "on" : "off",
                        "On: knees and elbows bend one way and every joint stays in the range the game's own animation "
                        "uses, while you pose and in game. Off: joints go wherever you push them, in the studio and in "
                        "game alike. Kept between sessions.",
                        [on = v.limits] { studio::set_joint_limits(!on); }});
        rows.push_back({"Collisions", "", "", {}, false});
        {
            // Pressing it steps through off and the quarters; C turns it off and on where it stood.
            Row clear{"Keep clear", !u.anti_clip || u.clear_force <= 0.005f ? "off" : std::format("{:.0f}%", u.clear_force * 100),
                      "What you move is kept out of his body and off the deck. How wide his body and board are taken to be: "
                      "low keeps the limbs close and stops only the worst of it, high holds them well apart, off lets "
                      "everything through. Drag along it, or choose it to step through the quarters. The deck is never "
                      "softened: a shoe inside the board is wrong at any setting. Key: C turns it off and on.",
                      [] {
                          auto& state = ui();
                          const float steps[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
                          float next = steps[0];
                          const float now = state.anti_clip ? state.clear_force : 0.0f;
                          for (const float step : steps) if (step > now + 0.01f) { next = step; break; }
                          state.clear_force = next;
                          state.anti_clip = next > 0.005f;
                      }};
            clear.slider = true;
            rows.push_back(std::move(clear));
        }
        rows.push_back({"Push limbs aside", u.shove_limbs ? "on" : "off",
                        "On: a limb that the one you are moving runs into gives way too - its hand or foot moves by the overlap and "
                        "the rest bends to follow. Off, only what you move is kept clear, which is steadier. His two legs never push "
                        "each other, since they belong close together.",
                        [] { ui().shove_limbs = !ui().shove_limbs; }});
        const auto joints = studio::editable_joints();
        rows.push_back({"Joints", std::to_string(joints.size()), "", {}, false});
        for (const auto& j : joints) {
            const auto* t = track_view(v, j.index);
            const auto count = t ? t->keys.size() : 0;
            Row row{std::string(j.label), count ? std::to_string(count) + (count == 1 ? " key" : " keys") : "",
                    "Choose the joint, then drag it on the skater or use the left stick. Enter (A) keys it on this frame.",
                    [index = j.index] { ui().selected = index; }};
            if (t && t->hidden) row.value += "  muted";
            if (t && t->locked) row.value += "  locked";
            row.joint = j.index;
            rows.push_back(std::move(row));
        }
        break;
    }
    case Tab::keys: {
        const auto* key = key_here(v, u.selected);
        const auto* track = track_view(v, u.selected);
        const bool locked = track && track->locked;
        const auto joint = upper(label_of(u.selected));
        rows.push_back({"This frame", joint, "", {}, false});
        rows.push_back({"Key this frame", "", "Keys the chosen joint as it is now, on this frame. Key: K.", [] { key_joint(); }, v.has_clip && !locked});
        rows.push_back({"Delete key", "", "Takes the chosen joint's key on this frame off. Key: Del (X on a controller).", [&v] { delete_key(v); }, key && !locked});
        {
            // Always here, so the rows under it do not move as the playhead passes keys.
            const char* names[] = {"smooth", "linear", "step"};
            const int ease = key ? static_cast<int>(key->ease) : 0;
            const float time = key ? key->time : 0.0f;
            rows.push_back({"Blend into the next key", key ? names[ease] : "",
                            "How the key on this frame blends into the next: smooth curves through the keys, linear goes straight, "
                            "step holds this key until the next. A linear or step key is never round.", [time, ease] {
                                studio::begin_change();
                                studio::set_ease(ui().selected, time, static_cast<studio::Ease>((ease + 1) % 3));
                            }, key && !locked});
        }
        rows.push_back({"Previous key", "", "The playhead to the chosen joint's key before this frame.", [&v] { jump_key(v, -1); }, track && !track->keys.empty()});
        rows.push_back({"Next key", "", "The playhead to the chosen joint's key after this frame.", [&v] { jump_key(v, 1); }, track && !track->keys.empty()});
        rows.push_back({"Chosen keys", u.picked.empty() ? "" : std::to_string(u.picked.size()), "", {}, false});
        rows.push_back({"Select all keys", "", "Chooses every key of every joint and the board: drag one to move them all. Shift + click "
                        "adds or takes one key; a box dragged with the right button chooses a group. Key: Ctrl+A.",
                        [&v] { pick_all(v); }, v.has_clip});
        rows.push_back({"Smooth chosen keys", "", "The chosen keys ease into their neighbours (each is drawn a little towards them) and "
                        "show round. Double-click a key to smooth just that one.", [] { smooth_picked(); }, !u.picked.empty()});
        rows.push_back({"Delete chosen keys", "", "Takes every chosen key off. Key: Del.", [] { delete_picked(); }, !u.picked.empty()});
        rows.push_back({"Let go of chosen keys", "", "No key is chosen any more. Key: Esc.", [] {
                            ui().picked.clear();
                            say("No keys chosen.");
                        }, !u.picked.empty()});
        rows.push_back({"Joint's keys", joint, "", {}, false});
        rows.push_back({"Smooth joint's keys", "", "Smooths every key of the chosen joint: they all ease smoothly, keys that say nothing "
                        "are dropped, and the rest are drawn towards their neighbours and put on whole frames. Smoothed keys show round.",
                        [] {
                            studio::begin_change();
                            studio::smooth_keys(ui().selected);
                        }, v.has_clip && !locked});
        rows.push_back({"Smooth every joint", "", "The same, over every joint's keys at once.", [] {
                            studio::begin_change();
                            studio::smooth_keys(studio::all_joints);
                        }, v.has_clip});
        rows.push_back({"Delete joint's keys", "", "Takes all of the chosen joint's keys off.", [] {
                            studio::begin_change();
                            studio::reset_joint(ui().selected);
                        }, track && !locked});
        rows.push_back({"Channels", channels_open(u.selected) ? "shown" : "hidden",
                        "Opens the chosen joint in the timeline into its channels - rotation X, Y and Z and, for the hips and the "
                        "board, location X, Y and Z - each with its curve, to change one number of a key at a time: drag a key "
                        "up or down (Shift: finer), drag it sideways to move it in time, or type a value. Clicking a joint's name "
                        "in the timeline does the same; Ctrl + click keeps others open. Key: T.",
                        [] { toggle_channels(ui().selected, false); }, v.has_clip});
        rows.push_back({"Mute joint's keys", track && track->hidden ? "muted" : "off",
                        "Muted: the trick plays as if the chosen joint had no keys. They are kept, and come back when it is unmuted.",
                        [hidden = track && track->hidden, locked] { studio::set_track_flags(ui().selected, !hidden, locked); }, track != nullptr});
        rows.push_back({"Lock joint's keys", locked ? "locked" : "off", "Locked: the chosen joint's keys cannot be changed.",
                        [hidden = track && track->hidden, locked] { studio::set_track_flags(ui().selected, hidden, !locked); }, track != nullptr});
        rows.push_back({"Pose", "", "", {}, false});
        rows.push_back({"Copy pose", "", "Remembers every joint's edit on this frame. Key: Ctrl+C.", [] { studio::copy_pose(); }, v.has_clip});
        rows.push_back({"Paste pose", "", "Keys the remembered pose on this frame. Key: Ctrl+V.", [] {
                            studio::begin_change();
                            studio::paste_pose();
                        }, v.has_clip});
        rows.push_back({"History", "", "", {}, false});
        rows.push_back({"Undo", "", "Takes back the last change. Key: Ctrl+Z (B on a controller).", [] { studio::undo(); }, v.can_undo});
        rows.push_back({"Redo", "", "Puts back what Undo took back. Key: Ctrl+Y.", [] { studio::redo(); }, v.can_redo});
        break;
    }
    case Tab::packs: {
        // Everything about keeping tricks: saving the one open, the packs, and every saved trick.
        const auto preset_of = [&](const std::string& name) -> const studio::Preset* {
            const auto found = std::find_if(v.presets.begin(), v.presets.end(), [&](const auto& p) { return p.name == name; });
            return found == v.presets.end() ? nullptr : &*found;
        };
        const auto trick_of = [&](const std::string& name) {
            const auto* p = preset_of(name);
            return p ? p->trick + ", " + (p->pop.empty() ? std::string("any pop") : p->pop + " pop") : std::string("no trick yet");
        };
        const auto made_by = [&](const std::string& name) {
            const auto* p = preset_of(name);
            return p && !p->made_by.empty() ? " Made by " + p->made_by + "." : std::string{};
        };
        if (!v.open_pack.empty()) {
            const auto pack = std::find_if(v.packs.begin(), v.packs.end(), [&](const auto& p) { return p.name == v.open_pack; });
            rows.push_back({"< Back", "", "Back to your packs.", [] {
                                studio::open_pack("");
                                ui().row = ui().pack_row;
                                ui().confirm_delete.clear();
                            }});
            if (pack == v.packs.end()) {
                rows.push_back(info_row("This pack is gone", "", "It was deleted. Back takes you to your packs."));
                break;
            }
            const auto name = pack->name;
            rows.push_back({upper(name), pack->made_by.empty() ? "" : "by " + pack->made_by, "", {}, false});
            rows.push_back({"Switched on", pack->on ? "on" : "off",
                            "On: the tricks in this pack play in game. Several packs can be on at once (when two have the same "
                            "trick they take turns). With no pack on, all your saved tricks play.",
                            [name, on = pack->on] { std::string m; studio::set_pack_on(name, !on, m); say(m); }});
            rows.push_back({"Variation playback", pack->alternate ? "alternate" : "fixed",
                            "When the pack has more than one of a trick: Fixed plays the first one added, Alternate plays a "
                            "different one each attempt.",
                            [on = pack->alternate] { std::string m; studio::set_pack_alternate(!on, m); say(m); }});
            rows.push_back({"Save pack", pack->unsaved ? "not saved" : "saved",
                            "Keeps the tricks you put in or took out, and Variation playback. Until then the pack plays as it was "
                            "last saved.",
                            [] { std::string m; studio::save_pack(m); say(m); }, pack->unsaved});
            rows.push_back({"Export pack", name + ".zip",
                            "Saves the pack (with its tricks) as one zip in ReSkate.Tricks\\exports, to share. Whoever gets it puts "
                            "the zip in their ReSkate.Tricks\\import and chooses Import packs in PACKS.",
                            [name] { std::string m; studio::export_pack(name, m); say(m); }});
            rows.push_back({"Delete pack", u.confirm_delete == "pack:" + name ? "delete again" : "",
                            "Deletes the pack, not the tricks in it. Choose it twice.", [name] {
                                auto& state = ui();
                                if (state.confirm_delete != "pack:" + name) { state.confirm_delete = "pack:" + name; return; }
                                std::string m;
                                studio::delete_pack(name, m);
                                say(m);
                                state.confirm_delete.clear();
                                state.row = state.pack_row;
                            }});
            std::vector<std::string> in, out;
            for (const auto& trick : v.library)
                (std::find(pack->members.begin(), pack->members.end(), trick) != pack->members.end() ? in : out).push_back(trick);
            rows.push_back({"Tricks in this pack", std::to_string(in.size()), "", {}, false});
            if (in.empty()) rows.push_back(info_row("None yet", "", "Put your tricks in it from the list below."));
            for (const auto& trick : in) {
                Row row{trick, trick_of(trick), "In the pack: choose it to take it out (Save pack keeps that)." + made_by(trick),
                        [trick] { std::string m; studio::set_pack_member(trick, false, m); say(m); }};
                row.on = true;
                rows.push_back(std::move(row));
            }
            rows.push_back({"Add your tricks", std::to_string(out.size()), "", {}, false});
            if (out.empty()) rows.push_back(info_row("All your tricks are in it", "", "Every trick you saved is in this pack."));
            for (const auto& trick : out)
                rows.push_back({trick, trick_of(trick), "Choose it to put it in the pack (Save pack keeps that)." + made_by(trick),
                                [trick] { std::string m; studio::set_pack_member(trick, true, m); say(m); }, preset_of(trick) != nullptr});
            break;
        }
        rows.push_back({"This animation", v.has_clip ? title_of(v) : "nothing open", "", {}, false});
        rows.push_back({"Save", v.dirty ? "not saved" : "", "Saves the animation in your tricks, below. It plays in game on its trick - "
                        "unless packs are switched on and it is in none of them.", [] { save_now(); }, v.has_clip});
        rows.push_back({"Save as new variation", "", "Saves a separate copy of this animation and its edits, keeping the original.", [] {
                            std::string m; studio::save_preset_copy(m); say(m); ui().names_for = 0;
                        }, v.has_clip});
        rows.push_back({"Pop", v.pop.empty() ? "any" : v.pop,
                        "Which pop this plays on: any, or only a low, mid or high pop (one of each can be saved for a trick). Choose it to step through.",
                        [pop = v.pop] { studio::set_pop(pop.empty() ? "low" : pop == "low" ? "mid" : pop == "mid" ? "high" : ""); }, v.has_clip});
        rows.push_back({"Copy to low, mid and high pop", "", "Saves this animation with its keys three times, one for each pop height, so it plays on every pop.",
                        [] {
                            std::string m;
                            studio::copy_to_pops(m);
                            say(m);
                        }, v.has_clip});
        std::size_t packs_on = 0;
        for (const auto& pack : v.packs) packs_on += pack.on;
        rows.push_back({"Your packs", packs_on ? std::to_string(packs_on) + " on" : "none on: all your tricks play", "", {}, false});
        rows.push_back({"Create a new pack", u.pack_name[0] ? std::string(u.pack_name.data()) : "type its name below",
                        "Type the pack's name in NEW PACK NAME below (for example Steezy), then choose this. It starts empty "
                        "and off: put your tricks in it, Save pack, and switch it on.", [] {
                            std::string m;
                            if (studio::create_preset_pack(m, ui().pack_name.data())) {
                                ui().pack_name.fill(0);
                                ui().row = 2;
                            }
                            say(m);
                        }});
        rows.push_back({"Import packs", "", "Takes in other people's packs: put their zip in ReSkate.Tricks\\import first. Everything "
                        "in it is checked, a name already used gets the next number, and animations from older builds are "
                        "brought up to date.", [] { std::string m; studio::import_packs(m); say(m); }});
        if (v.packs.empty()) rows.push_back(info_row("No packs yet", "", "Create one above: type its name in NEW PACK NAME first."));
        for (const auto& pack : v.packs) {
            std::size_t count = 0;
            for (const auto& member : pack.members) count += std::find(v.library.begin(), v.library.end(), member) != v.library.end();
            const int index = static_cast<int>(rows.size());
            rows.push_back({pack.name, std::string(pack.on ? "on" : "off") + ", " + std::to_string(count) + (count == 1 ? " trick" : " tricks") +
                                           (pack.unsaved ? ", not saved" : "") + " >",
                            (pack.made_by.empty() ? std::string{} : "Made by " + pack.made_by + ". ") +
                                "Open it to switch it on or off, put your tricks in it, save it or export it.",
                            [name = pack.name, index] {
                                studio::open_pack(name);
                                ui().pack_row = index;
                                ui().row = 2; // Switched on
                            }});
        }
        rows.push_back({"Your tricks", std::to_string(v.library.size()), "", {}, false});
        rows.push_back({"Export all your tricks", "All saved animations.zip", "Every saved animation in one zip in "
                        "ReSkate.Tricks\\exports, to share or keep.", [] { std::string m; studio::export_pack("", m); say(m); }});
        if (v.library.empty()) rows.push_back(info_row("Nothing saved yet", "", "Pick a trick in TRICKS, change it and save it: it shows up here."));
        for (const auto& name : v.library) {
            std::size_t in = 0;
            for (const auto& pack : v.packs) in += std::find(pack.members.begin(), pack.members.end(), name) != pack.members.end();
            const auto* preset = preset_of(name);
            const bool plays = preset && preset->active;
            Row row{name, name == v.name ? "open" : trick_of(name) + (plays ? ", plays" : "") + (in ? ", " + std::to_string(in) + (in == 1 ? " pack" : " packs") : ""),
                    "One of your saved tricks: choose it to open it and change it. Del (X on a controller) twice deletes it. \"Plays\": the "
                    "game uses it for that trick and pop now - with no pack on, the one of each you saved last; with packs on, the "
                    "ones in them." + made_by(name),
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
    }
    case Tab::settings: {
        rows.push_back({"Playback", "", "", {}, false});
        rows.push_back({v.playing ? "Pause" : "Play", "", "Plays the trick with your keys. Key: Space (Y on a controller).", [on = v.playing] { studio::set_playing(!on); }, v.has_clip});
        rows.push_back({"Loop", v.loop ? "on" : "off", "Repeat the timeline. A source whose end differs from its start can visibly reset when looped.", [on = v.loop] { studio::set_loop(!on); }});
        rows.push_back({"Preview speed", std::format("{:.2f}x", v.speed), "Slows the studio preview down to edit. In game the trick is fitted to the air that happens.", [speed = v.speed] {
                            const float speeds[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
                            float next = speeds[0];
                            for (const float step : speeds) if (step > speed + 0.01f) { next = step; break; }
                            studio::set_speed(next);
                        }});
        rows.push_back({"View", "", "", {}, false});
        rows.push_back({"Camera", shots[static_cast<std::size_t>(std::clamp(u.camera_shot, 0, 5))].name,
                        "A set camera angle to work in: close, on the board, cinematic, the filmer's follow, or over the skater. "
                        "It follows him while you pose and while it plays. Moving the camera yourself goes back to free. Key: V "
                        "(1 to 5 and 0 look from fixed sides).",
                        [&v] { next_shot(v, 1); }});
        rows.push_back({"Follow joint", "", "The camera keeps the chosen joint in the middle (again: the whole skater). Key: F.",
                        [] { studio::focus_joint(ui().selected); }});
        rows.push_back({"Hide joints", u.hide_joints ? "hidden" : "shown", "Takes the dots and the gizmo off the skater, to see the "
                        "pose clean. Key: J.", [] { ui().hide_joints = !ui().hide_joints; }});
        rows.push_back({"Hide everything", "", "Takes the whole studio off the screen - tabs, timeline, dots and all - leaving the "
                        "skater and the room, for filming a trick or taking a picture of it. Key: H brings it back.",
                        [] { ui().hide_ui = true; say("Hidden. H brings it back."); }});
        rows.push_back({"Stance", v.stance_choice != 0 ? (v.stance_choice == 1 ? "regular" : "goofy")
                                  : v.game_stance == 0 ? (v.showing_goofy ? "the game's (not set: goofy)" : "the game's (not set: regular)")
                                  : v.showing_goofy ? "the game's (goofy)" : "the game's (regular)",
                        "Which foot leads while you edit: regular (left) or goofy (right). A trick made leading with the other foot "
                        "is mirrored into this stance the way the game mirrors its own animation, so the leg you grab is the leg "
                        "that moves. Where it plays once saved is up to Trick matching. The game's: the Stance setting in the "
                        "ReSkate menu, Addons > GRIND EXITS (taken as regular until it is set).",
                        [choice = v.stance_choice] { studio::set_stance((choice + 1) % 3); }});
        rows.push_back({"Hands grab the board", v.grab_board ? "on" : "off",
                        "On: a hand brought near the deck closes its fingers round it, more the nearer it gets. The fingers are not "
                        "keyed and not saved - it is the studio showing what the hand is doing. They cannot be posed by hand yet.",
                        [on = v.grab_board] { studio::set_grab(!on); }});
        rows.push_back({"In game", "", "", {}, false});
        rows.push_back({"Custom tricks in game", v.play_in_game ? "on" : "off",
                        "On: your saved tricks play in game in place of the game's own. The whole skater and the board play the "
                        "recording with your keys, as the studio shows it, and the trick is fitted to the air that happens: its "
                        "takeoff meets the recording's and its landing the game's. Off: every trick plays as the game has it.",
                        [on = v.play_in_game] { studio::set_play_in_game(!on); }});
        rows.push_back({"Trick matching", v.front_foot ? "front foot" : "exact stance",
                        "Front foot: plays in every stance (regular, goofy, switch, fakie), mirrored onto your front foot when "
                        "the other foot leads, as the game does. The closest recording wins: same pop, then rolling the same way, "
                        "then the same foot. Exact stance: only one made on the foot that leads now, rolling the same way; the "
                        "game's own animation otherwise. Nollie tricks need their own recording.",
                        [on = v.front_foot] { studio::set_front_foot(!on); }});
        rows.push_back({"Studio", "", "", {}, false});
        rows.push_back({"Studio room", v.room ? "on" : "off",
                        "On: the studio opens in the game's own lit studio room, as skate.'s Skaters page shows it. Off: it opens where you are. Takes effect the next time it opens.",
                        [on = v.room] { studio::set_room(!on); }});
        rows.push_back({"Export diagnostics", "Last 15 seconds", "Reproduce the bug, open F8, then export. Captures native/modified poses, switch/stance, timings, board and selected presets as local text files. Gameplay history freezes while the studio is open.", [] {
                            std::string m; studio::export_diagnostics(m); say(m);
                        }});
        rows.push_back({"Close the studio", "", "Back to skating. The camera and the game's HUD come back. Key: F8.", [] { studio::request_close(); }});
        rows.push_back({"About this animation", "", "", {}, false});
        rows.push_back(info_row("Source", v.source_kind == "native_asset" ? (v.source_position_codec == "native-piec-v1-extent" ? "native clip" : "unverified native clip")
                                          : v.source_kind == "render_capture" ? "in-game capture" : "legacy / unknown",
                                "Where this animation came from: one of the game's own clips, or a trick you recorded in game."));
        if (v.has_clip && v.source_pose_count > 1) {
            const float hz = v.duration > 0 ? static_cast<float>(v.source_pose_count - 1) / v.duration : 0;
            const bool sparse = v.source_max_sample_gap > 0.05f;
            rows.push_back(info_row("Source sampling", std::format("{} poses / {:.0f} Hz{}", v.source_pose_count, hz, sparse ? " (sparse)" : ""),
                            sparse ? "This source has widely spaced poses. Interpolation cannot restore missing motion; use a full-rate recording for a finished animation." :
                                     "The source poses available to the timeline. Slower playback does not add missing source frames."));
        }
        if (v.source_kind == "native_asset" && v.source_position_codec != "native-piec-v1-extent")
            rows.push_back(info_row("Source needs updating", "reopen from TRICKS", "This saved source predates the position decoder "
                                    "correction. Your saved keys remain intact; use the updated native library to inspect the corrected source."));
        rows.push_back(info_row("Entry", v.entry, "Where the trick starts from: the ground or a rail. Ground and rail-exit recordings are "
                                "kept apart, and the entry is taken when recording."));
        break;
    }
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
    // Record a trick, or the first pop; in JOINTS the chosen joint (list() finds its row).
    u.row = static_cast<Tab>(u.tab) == Tab::tricks ? (u.open_trick.empty() ? 1 : 2) : static_cast<Tab>(u.tab) == Tab::joints ? -1 : 0;
    u.confirm_delete.clear();
}
// Up / down in the list: in JOINTS the list is the joints themselves.
void move_row(const studio::View& v, int delta) {
    auto& u = ui();
    const auto rows = rows_of(static_cast<Tab>(u.tab), v);
    const int count = static_cast<int>(rows.size());
    if (static_cast<Tab>(u.tab) == Tab::joints) u.row = joints_row(rows);
    // Headings are stepped over. In JOINTS going onto a joint's row chooses that joint.
    int at = std::clamp(u.row, 0, std::max(0, count - 1));
    for (int step = 0; step < count; ++step) {
        at = std::clamp(at + delta, 0, std::max(0, count - 1));
        if (selectable(rows[static_cast<std::size_t>(at)]) || at == 0 || at == count - 1) break;
    }
    if (count && !selectable(rows[static_cast<std::size_t>(at)])) at = u.row; // only headings that way: stay
    u.row = at;
    if (at >= 0 && at < count && rows[static_cast<std::size_t>(at)].joint) u.selected = *rows[static_cast<std::size_t>(at)].joint;
}
void activate_row(const studio::View& v) {
    auto& u = ui();
    auto rows = rows_of(static_cast<Tab>(u.tab), v);
    if (static_cast<Tab>(u.tab) == Tab::joints) u.row = joints_row(rows);
    // On a joint's row, Enter (A) keys that joint on this frame.
    if (u.row >= 0 && u.row < static_cast<int>(rows.size()) && rows[static_cast<std::size_t>(u.row)].joint) {
        key_joint();
        return;
    }
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
    if (v.confirm_close) return; // the close warning's card takes the buttons
    // LB and RB change tab on a tap. Held with a stick they carry a bracket instead (below), so the tab
    // only changes when the shoulder is let go without having carried anything.
    if (pressed & (pad_lb | pad_rb)) u.shoulder_shifted = false;
    if ((released & pad_lb) && !u.shoulder_shifted) switch_tab(-1);
    if ((released & pad_rb) && !u.shoulder_shifted) switch_tab(1);
    if (pressed & pad_a) activate_row(v);
    if (pressed & pad_x) {
        if (static_cast<Tab>(u.tab) == Tab::packs) delete_row(v);
        else delete_key(v);
    }
    if (pressed & pad_y) studio::set_playing(!v.playing);
    if (pressed & pad_b) {
        if (!go_back_in_tricks()) studio::undo();
    }
    if (pressed & pad_view) {
        // The timeline: the whole trick, or zoomed in on the playhead.
        // View: the whole trick, the custom animation's window, then in on the playhead.
        if (u.timeline_view == Ui::View::fit) u.timeline_view = Ui::View::region;
        else if (u.timeline_view == Ui::View::region) zoom_timeline(3);
        else u.timeline_view = Ui::View::fit;
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

    // A shoulder and the stick under it carry a bracket in or out: LB and the left stick the start, RB and
    // the right stick the end (L1 and R1 on a PlayStation pad). Held, so it cannot be done by accident, and
    // the tab does not change when it has been used.
    const bool carrying_start = (buttons & pad_lb) && v.has_clip, carrying_end = (buttons & pad_rb) && v.has_clip;
    if (carrying_start || carrying_end) {
        const float raw = carrying_start ? stick_response(pad.left_x) : stick_response(pad.right_x);
        const int which = carrying_start ? 1 : 2;
        const float now_at = which == 1 ? v.bracket_start : (v.bracket_end < 0 ? v.duration : v.bracket_end);
        if (!u.shoulder_shifted) u.bracket_at[which - 1] = now_at;
        if (raw != 0) {
            if (!u.shoulder_shifted) {
                studio::begin_change();
                u.shoulder_shifted = true;
                u.bracket_at[which - 1] = now_at;
            }
            // A second of the trick a second of holding it over, so a short clip is still fine to aim.
            u.bracket_at[which - 1] = std::clamp(u.bracket_at[which - 1] + raw * dt, 0.0f, v.duration);
            studio::move_bracket(which, std::round(u.bracket_at[which - 1] * fps) / fps);
        }
        // The sticks are the brackets' while a shoulder is down: nothing is turned or orbited.
        u.filtered[0] = u.filtered[1] = u.filtered[2] = u.filtered[3] = 0;
        u.stick_turning = false;
        return;
    }
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
            apply_turn(v, p, u.stick_dx, u.stick_dy, u.move_mode);
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
void tab_row(ImDrawList* d, const Fonts& f, ImVec2 at, float width) {
    auto& u = ui();
    const float k = f.k, h = 36 * k;
    // The names shrink a little rather than run past the menu.
    float names = 0;
    for (const char* name : tab_names) names += text_size(f.heading, 25 * k, name).x;
    const float room = width - 2 * (22 + 14) * k - static_cast<float>(tab_names.size()) * 34 * k;
    const float fs = 25 * k * std::clamp(room / names, 0.6f, 1.0f);
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
            u.row = static_cast<Tab>(i) == Tab::joints ? -1 : 0; // list() moves off a heading
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
    int chosen = tab == Tab::joints ? joints_row(rows) : u.row;
    chosen = std::clamp(chosen, 0, std::max(0, static_cast<int>(rows.size()) - 1));
    // A heading cannot be chosen: the first row under it that does something is.
    if (!rows.empty() && !selectable(rows[static_cast<std::size_t>(chosen)]))
        for (int i = chosen; i < static_cast<int>(rows.size()); ++i)
            if (selectable(rows[static_cast<std::size_t>(i)])) { chosen = i; break; }
    u.row = chosen;
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
        // Keep clear: a paper row like the card below, filled across to how far it is turned up, dragged
        // anywhere along it.
        if (row.slider) {
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(9001);
            ImGui::InvisibleButton("##force", ImVec2(b.x - a.x, b.y - a.y));
            const bool over = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
            ImGui::PopID();
            const float inset = 20 * k;
            if (held) {
                const float along = (ImGui::GetIO().MousePos.x - (a.x + inset)) / std::max(1.0f, (b.x - a.x) - 2 * inset);
                u.clear_force = std::clamp(along, 0.0f, 1.0f);
                u.anti_clip = u.clear_force > 0.005f;
                u.row = static_cast<int>(i);
            }
            const bool on = u.anti_clip && u.clear_force > 0.005f;
            const float force = on ? u.clear_force : 0.0f;
            constexpr ImU32 paper = IM_COL32(234, 234, 232, 250), ink = IM_COL32(20, 20, 22, 255);
            skate_theme::rough_rect(d, a, b, over || held ? IM_COL32(246, 246, 244, 252) : paper, seed_of(a), k);
            if (force > 0.005f) {
                d->PushClipRect(a, ImVec2(a.x + inset + ((b.x - a.x) - 2 * inset) * force, b.y), true);
                skate_theme::rough_rect(d, a, b, blue, seed_of(a), k);
                d->PopClipRect();
            }
            const float fs = 21 * k;
            text(d, f.bold, fs, ImVec2(a.x + 30 * k, a.y + row_h * 0.5f - 12 * k), on && force > 0.25f ? white : ink, row.label);
            const std::string value = on ? std::format("{:.0f}%", force * 100) : "off";
            const auto vw = text_size(f.bold, 16 * k, value.c_str());
            text(d, f.bold, 16 * k, ImVec2(b.x - vw.x - 22 * k, a.y + row_h * 0.5f - 9 * k), on && force > 0.92f ? white : ink, value);
            continue;
        }
        // A row with nothing to do is a heading over the rows under it: the game's own settings pages are
        // read that way, and a heading that looks like a button is a button you cannot press.
        if (!row.act && !row.enabled && !row.info) {
            const float fs = 15 * k;
            text(d, f.heading, fs, ImVec2(a.x + 8 * k, b.y - fs - 6 * k), white, upper(row.label));
            d->AddRectFilled(ImVec2(a.x + 8 * k, b.y - 3 * k), ImVec2(b.x - 8 * k, b.y - 1.5f * k), IM_COL32(255, 255, 255, 36));
            if (!row.value.empty()) {
                const auto t = text_size(f.bold, 13 * k, row.value.c_str());
                text(d, f.bold, 13 * k, ImVec2(b.x - t.x - 10 * k, b.y - fs - 4 * k), soft, row.value);
            }
            continue;
        }
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
        const ImU32 ink = row.info ? soft : row.enabled ? white : faint;
        text(d, f.bold, 21 * k, ImVec2(a.x + 30 * k, a.y + row_h * 0.5f - 12 * k), ink, row.label);
        if (!row.value.empty()) {
            const auto t = text_size(f.bold, 16 * k, row.value.c_str());
            text(d, f.bold, 16 * k, ImVec2(b.x - t.x - 22 * k, a.y + row_h * 0.5f - 9 * k), here ? white : soft, row.value);
        }
        if (row.on && !here) d->AddRectFilled(ImVec2(a.x, a.y + 6 * k), ImVec2(a.x + 4 * k, b.y - 6 * k), blue);
        if (clicked) {
            u.row = static_cast<int>(i);
            if (row.enabled && row.act) row.act(); // a joint's row chooses its joint
        }
    }
    d->PopClipRect();
    // The thin scroll bar at the right, as the game's lists have.
    const float bar_x = at.x + size.x - 8 * k;
    d->AddRectFilled(ImVec2(bar_x, at.y), ImVec2(bar_x + 3 * k, at.y + size.y), IM_COL32(220, 220, 225, 120));
    if (content > size.y) {
        const float h = (std::max)(30 * k, size.y * size.y / content);
        const float travel = size.y - h;
        // It can be dragged, as well as showing where the list is.
        auto& io = ImGui::GetIO();
        const bool over = io.MousePos.x >= bar_x - 10 * k && io.MousePos.x <= bar_x + 13 * k && io.MousePos.y >= at.y &&
                          io.MousePos.y <= at.y + size.y;
        if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) u.list_bar_held = true;
        if (u.list_bar_held && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) u.list_bar_held = false;
        if (u.list_bar_held && travel > 0)
            scroll = std::clamp((io.MousePos.y - at.y - h * 0.5f) / travel, 0.0f, 1.0f) * (content - size.y);
        const float y = at.y + travel * std::clamp(scroll / (content - size.y), 0.0f, 1.0f);
        d->AddRectFilled(ImVec2(bar_x - 4 * k, y), ImVec2(bar_x + 7 * k, y + h),
                         u.list_bar_held || over ? IM_COL32(255, 255, 255, 255) : IM_COL32(235, 235, 240, 255), 2 * k);
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
        // A hand, foot, knee or elbow: its position fields are where it is from its rest, and the limb above
        // it bends to reach (studio::limb_chain). The others take a move of their own, or none at all.
        const bool placed = studio::limb_chain(u.selected) != 0;
        auto position = placed ? studio::limb_position(u.selected) : offset.offset;
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
        if ((e && e->moves) || placed) {
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
            } else if (placed) {
                if (moved) studio::set_limb_position(u.selected, position);
                if (turned) studio::set_offset(u.selected, {studio::from_euler(euler), {0, 0, 0}});
            } else studio::set_offset(u.selected, {studio::from_euler(euler), position});
        }
        return; // what the chosen row does is said beside the card, in help_box
    }
    if (tab == Tab::packs) {
        // The name and trick of the one being edited, typed here.
        if (u.names_for != v.revision && !ImGui::IsAnyItemActive()) {
            std::snprintf(u.name.data(), u.name.size(), "%s", v.name.c_str());
            std::snprintf(u.author.data(), u.author.size(), "%s", v.author.c_str());
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
        // The trick it plays on is the one the game said it was (recorded, or the game's own trick opened): shown,
        // not typed, since a name the game does not use would never play.
        {
            const ImVec2 box(at.x + 38 * k + w, y + 18 * k), end(box.x + w, box.y + ImGui::GetFrameHeight());
            d->AddRectFilled(box, end, IM_COL32(222, 222, 220, 255));
            const std::string shown = v.trick.empty() ? std::string("none - record it in TRICKS") : v.trick;
            const auto t = text_size(f.bold, ImGui::GetFontSize(), shown.c_str());
            text(d, f.bold, ImGui::GetFontSize(), ImVec2(box.x + 8 * k, box.y + (end.y - box.y - t.y) * 0.5f), v.trick.empty() ? ink_soft : ink, shown);
        }
        // Who made it. Kept with the trick and shared with it, so a trick passed on says whose it is.
        text(d, f.bold, 13 * k, ImVec2(at.x + 22 * k, y + 62 * k), ink_soft, "MADE BY");
        ImGui::SetCursorScreenPos(ImVec2(at.x + 22 * k, y + 80 * k));
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputTextWithHint("##author", "your name", u.author.data(), u.author.size(), ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit())
            studio::set_author(u.author.data());
        text(d, f.bold, 13 * k, ImVec2(at.x + 38 * k + w, y + 62 * k), ink_soft, "NEW PACK NAME");
        ImGui::SetCursorScreenPos(ImVec2(at.x + 38 * k + w, y + 80 * k));
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("##packname", "e.g. Steezy", u.pack_name.data(), u.pack_name.size());
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(5);
        y += 124 * k;
    }
}
// What the chosen row does, in its own box beside the card. It used to be written inside the card, which
// on the pages that show a joint's numbers meant straight over them.
float help_box(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 at, ImVec2 size) {
    auto& u = ui();
    const float k = f.k;
    const auto rows = rows_of(static_cast<Tab>(u.tab), v);
    if (u.row < 0 || u.row >= static_cast<int>(rows.size())) return at.y + size.y;
    const auto& row = rows[static_cast<std::size_t>(u.row)];
    if (row.about.empty()) return at.y + size.y;
    const float fs = 14 * k, wrap = size.x - 36 * k;
    const auto measured = f.bold->CalcTextSizeA(fs, FLT_MAX, wrap, row.about.c_str());
    const float tall = std::min(size.y, measured.y + 54 * k);
    const ImVec2 a(at.x, at.y + size.y - tall), b(at.x + size.x, at.y + size.y);
    skate_theme::rough_rect(d, a, b, IM_COL32(16, 16, 18, 238), seed_of(a), k * 0.8f);
    text(d, f.heading, 13 * k, ImVec2(a.x + 18 * k, a.y + 14 * k), blue_light, upper(row.label));
    d->AddText(f.bold, fs, ImVec2(a.x + 18 * k, a.y + 36 * k), IM_COL32(222, 222, 226, 255), row.about.c_str(), nullptr, wrap);
    return a.y;
}

// The bottom of the screen: the whole trick from start to end, one row of keys per joint with a row
// of every key on top, the playhead across all of them. FIT shows all of it; - and + (Ctrl + wheel,
// LT + up / down on a controller) zoom in on the playhead.
void zoom_timeline(float factor) {
    auto& u = ui();
    u.timeline_view = Ui::View::free; // zooming by hand is leaving FIT or REGION
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
        // REGION holds the timeline on the custom animation's window and follows the brackets; FIT shows
        // the whole trick. - and + zoom by hand, which leaves both.
        if (button(d, f, "##region", ImVec2(zx - 96 * k, at.y + 6 * k), ImVec2(86 * k, 24 * k), "REGION",
                   u.timeline_view == Ui::View::region, v.has_clip))
            u.timeline_view = u.timeline_view == Ui::View::region ? Ui::View::fit : Ui::View::region;
        if (button(d, f, "##zoomout", ImVec2(zx, at.y + 6 * k), ImVec2(30 * k, 24 * k), "-", false, u.timeline_zoom > 1)) zoom_timeline(1 / 1.5f);
        zx += 34 * k;
        if (button(d, f, "##fit", ImVec2(zx, at.y + 6 * k), ImVec2(30 * k, 24 * k), "FIT", u.timeline_view == Ui::View::fit)) {
            u.timeline_view = Ui::View::fit;
            u.timeline_zoom = 1;
        }
        zx += 34 * k;
        if (button(d, f, "##zoomin", ImVec2(zx, at.y + 6 * k), ImVec2(30 * k, 24 * k), "+", false, u.timeline_zoom < 16)) zoom_timeline(1.5f);
        const std::string time = v.has_clip ? std::format("{:.2f} / {:.2f} s   frame {}", v.time, v.duration, static_cast<int>(std::round(v.time * fps))) : "";
        const auto t = text_size(f.bold, 13 * k, time.c_str());
        text(d, f.bold, 13 * k, ImVec2(at.x + size.x - 16 * k - t.x, at.y + 36 * k), soft, time);
    }
    const float ruler_left = lane.x, ruler_right = at.x + size.x - 130 * k;
    const float ruler_w = ruler_right - ruler_left;
    const float region_start = v.bracket_start, region_end = v.bracket_end < 0 ? duration : v.bracket_end;
    // Held still while a bracket is being carried: the window is worked out from the brackets, so letting it
    // rescale under a drag would have the bracket running away from the mouse. It settles on letting go.
    const bool carrying = u.bracket_drag != 0 || u.shoulder_shifted;
    const bool in_region = u.timeline_view == Ui::View::region && v.has_clip && !carrying;
    if (in_region) {
        // Held on the window between the brackets, its start at the left: worked out again every frame, so
        // moving a bracket keeps the window filling the timeline.
        u.timeline_zoom = std::clamp(duration / std::max(region_end - region_start, 1.0f / fps), 1.0f, 16.0f);
        u.zoom_to_playhead = false;
    } else if (u.timeline_view == Ui::View::fit) u.timeline_zoom = 1;
    const float seconds_w = (ruler_w - 2 * pad) / duration * u.timeline_zoom;
    if (in_region) u.timeline_scroll = region_start * seconds_w;
    else if (u.zoom_to_playhead || u.timeline_zoom <= 1.001f) {
        u.timeline_scroll = v.time * seconds_w - (ruler_w - 2 * pad) * 0.5f;
        u.zoom_to_playhead = false;
    } else if (const float x = pad + v.time * seconds_w - u.timeline_scroll; v.playing && (x < pad || x > ruler_w - pad))
        u.timeline_scroll = v.time * seconds_w - (ruler_w - 2 * pad) * 0.5f;
    u.timeline_scroll = std::clamp(u.timeline_scroll, 0.0f, std::max(0.0f, duration * seconds_w - (ruler_w - 2 * pad)));
    const auto x_of = [&](float t) { return ruler_left + pad + t * seconds_w - u.timeline_scroll; };
    const auto t_of = [&](float x) { return std::clamp((x - ruler_left - pad + u.timeline_scroll) / seconds_w, 0.0f, v.duration); };
    // The keys a bracket puts on every joint are not shown: they work, but the brackets stand for them.
    const auto bracket_key = [&](float time) {
        if (!v.has_clip) return false;
        const float be = v.bracket_end < 0 ? v.duration : v.bracket_end;
        return std::abs(time - v.bracket_start) < 0.5f / fps || std::abs(time - be) < 0.5f / fps;
    };
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
    // The bar lives in this strip; the rows start after it.
    const float rows_left = 22 * k;
    skate_theme::rough_rect(d, ImVec2(at.x + rows_left, summary_y), ImVec2(lanes_right - 8 * k, summary_y + summary_h), IM_COL32(30, 31, 34, 240), 83, k * 0.6f);
    text(d, f.heading, 15 * k, ImVec2(at.x + 22 * k, summary_y + 6 * k), soft, "ALL KEYS");
    d->PushClipRect(ImVec2(ruler_left, summary_y), ImVec2(ruler_right, summary_y + summary_h), true);
    for (const auto& t : v.tracks)
        for (const auto& key : t.keys)
            if (!bracket_key(key.time))
                    key_mark(d, ImVec2(x_of(key.time), summary_y + summary_h * 0.5f), 6 * k, t.hidden ? faint : soft, round_key(key));
    d->PopClipRect();
    // Rows, one per joint, and under a joint that is opened (its name clicked, or T) a row for each of its
    // channels - rotation X, Y, Z and, for what can be moved, location X, Y, Z - each with its curve.
    const auto joints = studio::editable_joints();
    const float rows_top = summary_y + summary_h + 6 * k;
    const float rows_h = at.y + size.y - rows_top;
    const float ch_h = 30 * k, ch_gap = 3 * k;
    struct LaneRow {
        std::size_t r;
        int channel; // -1: the joint's own row
        float top;
    };
    std::vector<LaneRow> lanes;
    float content_h = 0;
    for (std::size_t r = 0; r < joints.size(); ++r) {
        lanes.push_back({r, -1, content_h});
        content_h += row_h + gap;
        if (channels_open(joints[r].index))
            for (int c = 0; c < channel_count(joints[r].index); ++c) {
                lanes.push_back({r, c, content_h});
                content_h += ch_h + ch_gap;
            }
    }
    // The list follows the chosen joint only when the choice has just changed (or a joint was just opened):
    // otherwise it would pull the view back every frame and the list could not be scrolled past it.
    if (u.selected != u.rows_followed || u.rows_reveal != 0xffff) {
        const std::uint16_t target = u.rows_reveal != 0xffff ? u.rows_reveal : u.selected;
        u.rows_followed = u.selected;
        u.rows_reveal = 0xffff;
        for (const auto& entry : lanes)
            if (entry.channel < 0 && joints[entry.r].index == target) {
                const float top = entry.top;
                float bottom = top + row_h;
                if (channels_open(target)) bottom += channel_count(target) * (ch_h + ch_gap);
                bottom = std::min(bottom, top + rows_h); // taller than the list: its name at the top
                if (top < u.rows_scroll) u.rows_scroll = top;
                if (bottom > u.rows_scroll + rows_h) u.rows_scroll = bottom - rows_h;
            }
    }
    u.rows_scroll = std::clamp(u.rows_scroll, 0.0f, std::max(0.0f, content_h - rows_h));
    drawn_keys().clear();
    d->PushClipRect(ImVec2(at.x, rows_top), ImVec2(lanes_right, at.y + size.y), true);
    static constexpr std::array<const char*, 6> channel_names{"ROTATION X", "ROTATION Y", "ROTATION Z", "LOCATION X", "LOCATION Y", "LOCATION Z"};
    static constexpr std::array<ImU32, 3> axis_colour{IM_COL32(232, 84, 84, 255), IM_COL32(110, 200, 96, 255), IM_COL32(80, 150, 245, 255)};
    for (const auto& entry : lanes) {
        const auto& joint = joints[entry.r];
        const std::size_t r = entry.r;
        const float y = rows_top + entry.top - u.rows_scroll;
        if (y + (entry.channel < 0 ? row_h : ch_h) < rows_top || y > at.y + size.y) continue;
        const bool chosen = joint.index == u.selected;
        const auto* track = track_view(v, joint.index);
        if (entry.channel >= 0) {
            // ---- a channel: its name and value on the left, its curve and keys on the lane --------------
            const int c = entry.channel;
            const bool turn = c < 3;
            const bool locked = track && track->locked;
            const ImVec2 a(at.x + rows_left + 18 * k, y), b(lanes_right - 8 * k, y + ch_h);
            const bool focused = u.channel_joint == joint.index && u.channel == c;
            skate_theme::rough_rect(d, a, ImVec2(at.x + rows_left + label_w - 12 * k, b.y), focused ? IM_COL32(40, 52, 72, 246) : IM_COL32(20, 20, 22, 246), seed_of(a), k * 0.6f);
            skate_theme::rough_rect(d, ImVec2(at.x + rows_left + label_w - 4 * k, a.y), b, IM_COL32(24, 25, 28, 225), seed_of(a) + 1, k * 0.6f);
            d->AddRectFilled(ImVec2(a.x + 8 * k, y + 7 * k), ImVec2(a.x + 12 * k, y + ch_h - 7 * k), axis_colour[static_cast<std::size_t>(c % 3)], 2 * k);
            text(d, f.bold, 13 * k, ImVec2(a.x + 20 * k, y + ch_h * 0.5f - 8 * k), focused ? white : soft, channel_names[static_cast<std::size_t>(c)]);
            // The joint's keys as a curve of this one number.
            studio::Track curve;
            curve.joint = joint.index;
            if (track) for (const auto& kv : track->keys) curve.keys.push_back({kv.time, kv.rotation, kv.offset, kv.ease});
            // Its value on this frame, to drag or type (a double-click types).
            {
                const float frame_time = std::round(v.time * fps) / fps; // the frame a change is keyed on
                auto& field = u.channel_field;
                const bool this_field = field.joint == joint.index && field.channel == c;
                float value = this_field && field.active ? field.value : studio::channel_value(studio::evaluate(curve, frame_time), c);
                ImGui::PushID(joint.index);
                ImGui::PushID(100 + c);
                ImGui::PushStyleColor(ImGuiCol_Text, locked ? faint : white);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(44, 46, 52, 255));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(60, 64, 72, 255));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(40, 80, 140, 255));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * k, 3 * k));
                ImGui::PushFont(f.bold);
                ImGui::SetCursorScreenPos(ImVec2(at.x + rows_left + label_w - 104 * k, y + 4 * k));
                ImGui::SetNextItemWidth(88 * k);
                const bool changed = ImGui::DragFloat("##value", &value, turn ? 0.25f : 0.002f, 0, 0, turn ? "%.1f" : "%.3f");
                if (ImGui::IsItemActivated() && !locked) {
                    field = {};
                    field.joint = joint.index;
                    field.channel = c;
                    field.active = true;
                    field.base = studio::evaluate(curve, frame_time);
                    field.value = value;
                    u.selected = joint.index;
                    u.rows_followed = joint.index; // chosen here: the rows stay where they are
                    u.channel_joint = joint.index;
                    u.channel = c;
                }
                if (field.joint == joint.index && field.channel == c) {
                    if (ImGui::IsItemActive()) field.value = value;
                    else field.active = false;
                    if (changed && !locked) {
                        if (!field.began) { // one undo step, taken when it first changes something
                            studio::begin_change();
                            field.began = true;
                        }
                        const auto requested = studio::with_channel(field.base, c, value);
                        studio::set_offset(joint.index, requested);
                        note_held(v, joint.index, requested);
                    }
                }
                ImGui::PopFont();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
                ImGui::PopID();
                ImGui::PopID();
            }
            ImGui::PushID(joint.index);
            ImGui::PushID(200 + c);
            ImGui::SetCursorScreenPos(ImVec2(ruler_left, y));
            ImGui::InvisibleButton("##channel", ImVec2(ruler_right - ruler_left, ch_h));
            const bool lane_hovered = ImGui::IsItemHovered();
            const bool lane_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            ImGui::PopID();
            ImGui::PopID();
            bool on_key = false;
            std::optional<float> nearest; // the key a click takes: the nearest in reach
            if (lane_clicked && track) {
                float reach = 8 * k;
                for (const auto& key : track->keys)
                    if (!bracket_key(key.time))
                        if (const float apart = std::abs(ImGui::GetIO().MousePos.x - x_of(key.time)); apart < reach) {
                            reach = apart;
                            nearest = key.time;
                        }
            }
            if (!curve.keys.empty()) {
                d->PushClipRect(ImVec2(ruler_left, std::max(rows_top, y)), ImVec2(ruler_right, std::min(at.y + size.y, y + ch_h)), true);
                // The curve, sampled across the lane; angles unwrapped so a turn past 180 does not jump.
                thread_local std::vector<ImVec2> points;
                thread_local std::vector<float> values;
                points.clear();
                values.clear();
                const float step = 3 * k;
                float lo = std::numeric_limits<float>::max(), hi = -lo, prev = 0;
                for (float x = ruler_left; x <= ruler_right + 0.5f * step; x += step) {
                    float value = studio::channel_value(studio::evaluate(curve, std::max(0.0f, t_of(x))), c);
                    if (turn && !values.empty()) {
                        while (value - prev > 180) value -= 360;
                        while (value - prev < -180) value += 360;
                    }
                    prev = value;
                    values.push_back(value);
                    lo = std::min(lo, value);
                    hi = std::max(hi, value);
                }
                const float least = turn ? 10.0f : 0.05f;
                if (hi - lo < least) {
                    const float middle = (hi + lo) * 0.5f;
                    lo = middle - least * 0.5f;
                    hi = middle + least * 0.5f;
                }
                const auto y_of = [&](float value) { return y + ch_h - 5 * k - (value - lo) / (hi - lo) * (ch_h - 10 * k); };
                for (std::size_t i = 0; i < values.size(); ++i) points.emplace_back(ruler_left + static_cast<float>(i) * step, y_of(values[i]));
                d->AddPolyline(points.data(), static_cast<int>(points.size()), track && track->hidden ? faint : axis_colour[static_cast<std::size_t>(c % 3)], 0, 1.6f * k);
                for (const auto& key : track->keys) {
                    if (bracket_key(key.time)) continue;
                    // While a key is dragged sideways it is shown where it is going, as on the joint's own row.
                    const bool dragged_with = u.key_joint && is_picked(joint.index, key.time) && is_picked(*u.key_joint, u.key_from);
                    const float kx = x_of(u.key_joint == joint.index && std::abs(key.time - u.key_from) < 1e-4f ? u.key_to
                                          : dragged_with ? key.time + (u.key_to - u.key_from) : key.time);
                    // Its value on the curve as drawn (unwrapped like the samples round it).
                    float value = studio::channel_value(studio::Offset{key.rotation, key.offset}, c);
                    if (turn && !values.empty()) {
                        const auto near_sample = static_cast<std::size_t>(std::clamp((kx - ruler_left) / step, 0.0f, static_cast<float>(values.size() - 1)));
                        while (value - values[near_sample] > 180) value -= 360;
                        while (value - values[near_sample] < -180) value += 360;
                    }
                    const ImVec2 cpos(kx, y_of(value));
                    const bool current = std::abs(key.time - v.time) < 0.5f / fps;
                    const bool close_to = lane_hovered && std::abs(ImGui::GetIO().MousePos.x - kx) < 8 * k;
                    const bool in_pick = is_picked(joint.index, key.time);
                    drawn_keys().push_back({joint.index, key.time, cpos});
                    if ((current && focused) || in_pick) key_mark(d, cpos, 9 * k, in_pick ? blue : blue_dim, round_key(key));
                    key_mark(d, cpos, (close_to ? 7.5f : 6) * k, track->hidden ? faint : in_pick || (current && focused) ? blue_light : white, round_key(key));
                    if (!lane_clicked || locked || !nearest || std::abs(key.time - *nearest) > 1e-4f) continue;
                    on_key = true;
                    if (ImGui::GetIO().KeyShift) {
                        auto& picked = u.picked;
                        const auto found = std::find_if(picked.begin(), picked.end(), [&](const auto& p) {
                            return p.first == joint.index && std::abs(p.second - key.time) < 1e-4f;
                        });
                        if (found != picked.end()) picked.erase(found);
                        else picked.emplace_back(joint.index, key.time);
                        continue;
                    }
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        studio::begin_change();
                        std::string m;
                        const std::array<std::pair<std::uint16_t, float>, 1> one{{{joint.index, key.time}}};
                        studio::smooth_chosen_keys(one, m);
                        say(m);
                        u.channel_drag.on = false;
                        continue;
                    }
                    // A press: this key's value can be dragged up and down, or the key moved sideways in time.
                    if (!in_pick) u.picked.clear();
                    u.selected = joint.index;
                    u.rows_followed = joint.index;
                    u.channel_joint = joint.index;
                    u.channel = c;
                    studio::set_time(key.time);
                    studio::set_playing(false);
                    u.channel_drag = {};
                    u.channel_drag.on = true;
                    u.channel_drag.joint = joint.index;
                    u.channel_drag.channel = c;
                    u.channel_drag.time = key.time;
                    u.channel_drag.base = studio::Offset{key.rotation, key.offset};
                    u.channel_drag.start = studio::channel_value(u.channel_drag.base, c);
                    u.channel_drag.from = ImGui::GetIO().MousePos;
                }
                d->PopClipRect();
            }
            if (lane_clicked && !on_key && v.has_clip) {
                u.picked.clear();
                u.selected = joint.index;
                u.rows_followed = joint.index;
                u.channel_joint = joint.index;
                u.channel = c;
                studio::set_time(std::round(t_of(ImGui::GetIO().MousePos.x) * fps) / fps);
                studio::set_playing(false);
            }
            continue;
        }
        // ---- a joint: its name (click: open or close its channels) and its keys ------------------------
        const ImVec2 a(at.x + rows_left, y), b(lanes_right - 8 * k, y + row_h);
        if (chosen) {
            skate_theme::rough_rect(d, a, ImVec2(a.x + label_w - 12 * k, b.y), blue, seed_of(a), k);
            theme::halftone(d, ImVec2(a.x + label_w - 70 * k, a.y + 6 * k), 8, 4, IM_COL32(255, 255, 255, 46));
            skate_theme::rough_rect(d, ImVec2(a.x + label_w - 4 * k, a.y), b, IM_COL32(44, 50, 62, 236), seed_of(a) + 1, k);
        } else {
            skate_theme::rough_rect(d, a, ImVec2(a.x + label_w - 12 * k, b.y), IM_COL32(24, 24, 26, 246), seed_of(a), k);
            skate_theme::rough_rect(d, ImVec2(a.x + label_w - 4 * k, a.y), b, r % 2 ? IM_COL32(30, 31, 34, 220) : IM_COL32(34, 35, 38, 220), seed_of(a) + 1, k);
        }
        const bool has_keys = track && !track->keys.empty();
        // The chevron: pointing down when the joint's channels are open.
        {
            const ImVec2 cc(a.x + 16 * k, y + row_h * 0.5f);
            const float h = 5 * k;
            const ImU32 ink = chosen || has_keys ? white : soft;
            if (channels_open(joint.index)) d->AddTriangleFilled(ImVec2(cc.x - h, cc.y - h * 0.6f), ImVec2(cc.x + h, cc.y - h * 0.6f), ImVec2(cc.x, cc.y + h * 0.8f), ink);
            else d->AddTriangleFilled(ImVec2(cc.x - h * 0.6f, cc.y - h), ImVec2(cc.x - h * 0.6f, cc.y + h), ImVec2(cc.x + h * 0.8f, cc.y), ink);
        }
        text(d, f.bold, 17 * k, ImVec2(a.x + 30 * k, y + row_h * 0.5f - 10 * k), chosen || has_keys ? white : soft, upper(std::string(joint.label)));
        ImGui::PushID(joint.index);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##label", ImVec2(label_w - 12 * k, row_h))) {
            u.selected = joint.index;
            toggle_channels(joint.index, ImGui::GetIO().KeyCtrl);
        }
        ImGui::SetCursorScreenPos(ImVec2(ruler_left, y));
        ImGui::InvisibleButton("##lane", ImVec2(ruler_right - ruler_left, row_h));
        const bool lane_hovered = ImGui::IsItemHovered();
        const bool lane_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool on_key = false;
        std::optional<float> nearest; // the key a click takes: the nearest in reach
        if (lane_clicked && track) {
            float reach = 9 * k;
            for (const auto& key : track->keys)
                if (!bracket_key(key.time))
                    if (const float apart = std::abs(ImGui::GetIO().MousePos.x - x_of(key.time)); apart < reach) {
                        reach = apart;
                        nearest = key.time;
                    }
        }
        if (track) {
            d->PushClipRect(ImVec2(ruler_left, rows_top), ImVec2(ruler_right, at.y + size.y), true);
            for (const auto& key : track->keys) {
                if (bracket_key(key.time)) continue;
                const bool dragged_with = u.key_joint && is_picked(joint.index, key.time) && is_picked(*u.key_joint, u.key_from);
                const float shown = u.key_joint == joint.index && std::abs(key.time - u.key_from) < 1e-4f ? u.key_to
                                    : dragged_with ? key.time + (u.key_to - u.key_from) : key.time;
                const ImVec2 c(x_of(shown), y + row_h * 0.5f);
                const bool current = std::abs(key.time - v.time) < 0.5f / fps;
                const bool close_to = lane_hovered && std::abs(ImGui::GetIO().MousePos.x - c.x) < 9 * k;
                const bool in_pick = is_picked(joint.index, key.time);
                const bool hit = lane_clicked && nearest && std::abs(key.time - *nearest) < 1e-4f;
                drawn_keys().push_back({joint.index, key.time, c});
                if ((current && chosen) || in_pick) key_mark(d, c, 13 * k, in_pick ? blue : blue_dim, round_key(key));
                key_mark(d, c, (close_to ? 10.5f : 9) * k, track->hidden ? faint : in_pick || (current && chosen) ? blue_light : white,
                         round_key(key));
                if (key.ease == studio::Ease::step) d->AddLine(ImVec2(c.x, c.y - 4 * k), ImVec2(c.x, c.y + 4 * k), IM_COL32(40, 40, 40, 255), 2 * k);
                if (hit && !track->locked && ImGui::GetIO().KeyShift) {
                    on_key = true;
                    auto& picked = u.picked;
                    const auto found = std::find_if(picked.begin(), picked.end(), [&](const auto& p) {
                        return p.first == joint.index && std::abs(p.second - key.time) < 1e-4f;
                    });
                    if (found != picked.end()) picked.erase(found);
                    else picked.emplace_back(joint.index, key.time);
                } else if (hit && !track->locked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    // Double-click: the key (or the chosen group it is in) is smoothed and shows round.
                    on_key = true;
                    studio::begin_change();
                    std::string m;
                    if (in_pick && u.picked.size() > 1) studio::smooth_chosen_keys(u.picked, m);
                    else {
                        const std::array<std::pair<std::uint16_t, float>, 1> one{{{joint.index, key.time}}};
                        studio::smooth_chosen_keys(one, m);
                    }
                    say(m);
                    u.key_joint.reset(); // not a drag
                } else if (hit && !track->locked) {
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
    // The joint list's scroll bar, down the left edge: it shows how far down the list is and can be dragged.
    if (content_h > rows_h + 1.0f) {
        const float bar_w = 10 * k, bar_x = at.x + 5 * k;
        const float shown = rows_h / content_h;
        const float thumb_h = (std::max)(rows_h * shown, 28 * k);
        const float travel = rows_h - thumb_h;
        const float where = u.rows_scroll / (std::max)(1.0f, content_h - rows_h);
        const float thumb_y = rows_top + travel * std::clamp(where, 0.0f, 1.0f);
        d->AddRectFilled(ImVec2(bar_x, rows_top), ImVec2(bar_x + bar_w, rows_top + rows_h), IM_COL32(255, 255, 255, 20), bar_w * 0.5f);
        auto& io = ImGui::GetIO();
        const bool over = io.MousePos.x >= bar_x - 4 * k && io.MousePos.x <= bar_x + bar_w + 4 * k && io.MousePos.y >= rows_top &&
                          io.MousePos.y <= rows_top + rows_h;
        if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) u.rows_bar_held = true;
        if (u.rows_bar_held && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) u.rows_bar_held = false;
        if (u.rows_bar_held && travel > 0)
            u.rows_scroll = std::clamp((io.MousePos.y - rows_top - thumb_h * 0.5f) / travel, 0.0f, 1.0f) * (content_h - rows_h);
        d->AddRectFilled(ImVec2(bar_x, thumb_y), ImVec2(bar_x + bar_w, thumb_y + thumb_h),
                         u.rows_bar_held || over ? blue : IM_COL32(255, 255, 255, 110), bar_w * 0.5f);
    }
    d->PopClipRect();
    if (u.channel_drag.on) {
        auto& drag = u.channel_drag;
        const auto& io = ImGui::GetIO();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag.on = false;
        else {
            const float dx = io.MousePos.x - drag.from.x, dy = io.MousePos.y - drag.from.y;
            if (drag.mode == 0 && std::hypot(dx, dy) > 4 * k) drag.mode = std::abs(dy) >= std::abs(dx) ? 1 : 2;
            if (drag.mode == 1) {
                if (!drag.began) {
                    studio::begin_change();
                    drag.began = true;
                }
                const float per_pixel = (drag.channel < 3 ? 0.25f : 0.0015f) * (io.KeyShift ? 0.2f : 1.0f);
                const auto requested = studio::with_channel(drag.base, drag.channel, drag.start - dy * per_pixel);
                studio::set_offset(drag.joint, requested);
                note_held(v, drag.joint, requested);
            } else if (drag.mode == 2) {
                u.key_joint = drag.joint;
                u.key_from = drag.time;
                u.key_to = drag.time;
                u.key_moved = false;
                drag.on = false;
            }
        }
    }
    if (u.key_joint) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float to = std::round(t_of(ImGui::GetIO().MousePos.x) * fps) / fps;
            if (std::abs(to - u.key_to) > 1e-4f) u.key_moved = true;
            u.key_to = to;
        } else {
            if (u.key_moved && std::abs(u.key_to - u.key_from) > 1e-4f) {
                if (is_picked(*u.key_joint, u.key_from) && u.picked.size() > 1) move_picked(u.key_to - u.key_from, v);
                else {
                    studio::begin_change();
                    studio::move_key(*u.key_joint, u.key_from, u.key_to);
                }
                studio::set_time(u.key_to);
            }
            u.key_joint.reset();
        }
    }
    // A box drawn with the right button over the rows picks out every key inside it. Shift adds to what
    // is already chosen. The right button is free down here; up in the view it orbits the camera.
    {
        auto& io = ImGui::GetIO();
        const ImVec2 m = io.MousePos;
        const bool over_rows = m.x >= ruler_left && m.x <= ruler_right && m.y >= rows_top && m.y <= at.y + size.y;
        if (over_rows && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            u.band_held = true;
            u.band_from = m;
        }
        if (u.band_held) {
            const ImVec2 a(std::min(u.band_from.x, m.x), std::min(u.band_from.y, m.y));
            const ImVec2 b(std::max(u.band_from.x, m.x), std::max(u.band_from.y, m.y));
            d->AddRectFilled(a, b, IM_COL32(1, 131, 255, 40));
            d->AddRect(a, b, IM_COL32(120, 190, 255, 200), 0, 0, 1.6f * k);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                u.band_held = false;
                if (!io.KeyShift) u.picked.clear();
                std::size_t caught = 0;
                for (const auto& key : drawn_keys())
                    if (key.at.x >= a.x && key.at.x <= b.x && key.at.y >= a.y && key.at.y <= b.y && !is_picked(key.joint, key.time)) {
                        u.picked.emplace_back(key.joint, key.time);
                        ++caught;
                    }
                if (caught) say(std::to_string(u.picked.size()) + " keys chosen: drag one to move them all.");
            }
        }
    }
    // The brackets: the window the animation works in, drawn over the rows, with the window shaded. Either
    // one retimes the animation into the new window. Each is labelled and has a grab area wide enough to
    // catch: a 3 px line was a hard thing to hit.
    if (v.has_clip) {
        const float bs = v.bracket_start, be = v.bracket_end < 0 ? v.duration : v.bracket_end;
        const float xs = x_of(bs), xe = x_of(be);
        const float grab = 22 * k; // how near the mouse counts as on a bracket
        auto& io = ImGui::GetIO();
        const bool in_ruler = io.MousePos.y >= at.y && io.MousePos.y <= at.y + header_h + 10 * k &&
                              io.MousePos.x >= ruler_left - grab && io.MousePos.x <= ruler_right + grab;
        // The nearer bracket when both are in reach, so they stay separable when the window is short.
        int near_bracket = 0;
        if (in_ruler) {
            const float ds = std::abs(io.MousePos.x - xs), de = std::abs(io.MousePos.x - xe);
            if (std::min(ds, de) < grab) near_bracket = ds <= de ? 1 : 2;
        }
        // The window the custom animation works in, barely there: a wash between the brackets over the rows
        // and a thin band along the top, both in time, so they stretch and slide with the zoom and the
        // scroll. Outside it the game's own animation plays, and that is left plain.
        d->AddRectFilled(ImVec2(xs, at.y + header_h), ImVec2(xe, at.y + size.y), IM_COL32(1, 131, 255, 14));
        d->AddRectFilled(ImVec2(xs, at.y + header_h - 3 * k), ImVec2(xe, at.y + header_h), IM_COL32(1, 131, 255, 70));
        // Its edges, where it hands over, a touch stronger than the middle.
        const float feather = std::min(26 * k, std::max(0.0f, (xe - xs) * 0.25f));
        if (feather > 1) {
            const ImU32 edge = IM_COL32(1, 131, 255, 30), none = IM_COL32(1, 131, 255, 0);
            d->AddRectFilledMultiColor(ImVec2(xs, at.y + header_h), ImVec2(xs + feather, at.y + size.y), edge, none, none, edge);
            d->AddRectFilledMultiColor(ImVec2(xe - feather, at.y + header_h), ImVec2(xe, at.y + size.y), none, edge, edge, none);
        }
        int which = 1;
        for (const auto [x, open_right] : {std::pair{xs, true}, std::pair{xe, false}}) {
            const float dir = open_right ? 1.0f : -1.0f;
            const bool hot = u.bracket_drag == which || (!u.bracket_drag && near_bracket == which);
            const ImU32 colour = hot ? IM_COL32(150, 205, 255, 255) : IM_COL32(1, 131, 255, 255);
            const float weight = hot ? 4.5f : 3.0f;
            // The grab area, shown while the mouse is on it so it is plain how big it is.
            if (hot) d->AddRectFilled(ImVec2(x - grab * 0.5f, at.y), ImVec2(x + grab * 0.5f, at.y + size.y), IM_COL32(1, 131, 255, 26));
            // The flag: the name on the marker itself, a little pennant off the top of its pole, pointing
            // into the window. The pole runs the height of the timeline.
            const char* name = open_right ? "START ANIM." : "END ANIM.";
            const float fs = 10 * k;
            const auto tw = text_size(f.bold, fs, name);
            const float flag_h = tw.y + 7 * k, flag_w = tw.x + 12 * k, top = at.y + 3 * k;
            const float tail = 5 * k; // the notch cut out of the flag's far end
            const ImVec2 a(x, top), b(x + dir * flag_w, top), c(x + dir * (flag_w - tail), top + flag_h * 0.5f);
            const ImVec2 e(x + dir * flag_w, top + flag_h), g(x, top + flag_h);
            const ImVec2 points[5]{a, b, c, e, g};
            d->AddConvexPolyFilled(points, 5, colour);
            text(d, f.bold, fs, ImVec2(open_right ? x + 6 * k : x - flag_w + 6 * k, top + (flag_h - tw.y) * 0.5f), IM_COL32(8, 14, 24, 255), name);
            d->AddLine(ImVec2(x, top), ImVec2(x, at.y + size.y - 4 * k), colour, weight * k);
            d->AddLine(ImVec2(x, at.y + size.y - 4 * k), ImVec2(x + dir * 7 * k, at.y + size.y - 4 * k), colour, weight * k);
            ++which;
        }
        // One drag of a bracket is one change to undo, and it only moves when it reaches another frame.
        if (near_bracket && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            u.bracket_drag = near_bracket;
            u.bracket_dragged_to = near_bracket == 1 ? v.bracket_start : v.bracket_end < 0 ? v.duration : v.bracket_end;
            u.bracket_began = false;
        }
        if (u.bracket_drag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) u.bracket_drag = 0;
        if (u.bracket_drag) {
            const float to = std::round(t_of(io.MousePos.x) * fps) / fps;
            if (std::abs(to - u.bracket_dragged_to) > 0.5f / fps) {
                if (!u.bracket_began) {
                    studio::begin_change();
                    u.bracket_began = true;
                }
                u.bracket_dragged_to = to;
                studio::move_bracket(u.bracket_drag, to);
            }
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
    // A clip has arrived - a trick opened, an animation loaded, a take recorded - so nothing is carried
    // over from the last one: the chosen joint, the picked keys, a drag in progress, the message that was
    // standing, and the timeline back to the whole trick.
    if (u.clip_seen != v.clip_serial) {
        u.clip_seen = v.clip_serial;
        u.selected = studio::hips_joint;
        u.picked.clear();
        u.key_joint.reset();
        u.gizmo_handle = -1;
        u.dragging_gizmo = u.dragging_playhead = u.bracket_drag = false;
        u.axis = -1;
        u.timeline_view = Ui::View::fit;
        u.timeline_zoom = 1;
        u.timeline_scroll = u.rows_scroll = 0;
        u.channel_drag = {};
        u.channel = -1;
        u.rows_followed = 0xffff;
        u.message.clear();
        // A trick is open: the joints are what you want next, not the list you picked it from.
        if (v.has_clip) {
            u.tab = static_cast<int>(Tab::joints);
            u.row = -1; // the chosen joint's row (list() finds it)
        }
        // The camera keeps whichever shot you chose, but is told again whether to travel with him, since
        // the studio starts out carrying him along and nothing has said otherwise yet.
        studio::set_camera_follow(u.camera_shot != 0);
    }
    const auto mouse = ImGui::GetIO().MousePos;
    const bool inside = !u.menu_open && mouse.x >= area_min.x && mouse.y >= area_min.y && mouse.x <= area_max.x && mouse.y <= area_max.y;
    // Out of the way while it plays or is scrubbed, so the motion can be watched, and on J whenever you
    // want the skater plain.
    const bool joints_hidden = u.hide_ui || u.hide_joints || v.playing || u.scrubbing || u.dragging_playhead || u.bracket_drag != 0;
    std::optional<std::uint16_t> hovered;
    float best = 22 * k;
    if (!u.pad_mode && !joints_hidden)
        for (const auto& j : v.joints)
            if (const auto at = project(j.position)) {
                const float dist = std::hypot(at->x - mouse.x, at->y - mouse.y);
                if (inside && dist < best) {
                    best = dist;
                    hovered = j.joint;
                }
            }
    if (!joints_hidden)
        for (const auto& j : v.joints) {
            const auto at = project(j.position);
            if (!at || j.joint == u.selected) continue;
            const bool hot = hovered && *hovered == j.joint;
            d->AddCircleFilled(*at, (hot ? 6.0f : 3.5f) * k, hot ? blue_light : IM_COL32(255, 255, 255, 90));
        }
    const auto* sel = joint_view(v, u.selected);
    std::optional<ImVec2> sel_at = sel ? project(sel->position) : std::nullopt;
    // The gizmo on the chosen joint. While a handle is held it is drawn from the frame the drag began on,
    // so the handle stays under the mouse as the joint moves.
    const auto* locked_track = track_view(v, u.selected);
    const bool can_edit = v.has_clip && !(locked_track && locked_track->locked) && !joints_hidden;
    static Gizmo held;
    const auto gizmo = u.gizmo_handle >= 0 ? held : gizmo_of(v, project, k);
    if (u.gizmo_handle < 0) held = gizmo;
    u.gizmo_hot = u.gizmo_handle >= 0 ? u.gizmo_handle
                  : (!u.pad_mode && inside && can_edit) ? gizmo_pick(gizmo, mouse, k) : -1;
    if (sel_at && !joints_hidden) {
        glow_box(d, *sel_at, 22 * k, k);
        if (u.pad_mode) text(d, f.bold, 13 * k, ImVec2(sel_at->x + 30 * k, sel_at->y - 8 * k), blue_light, upper(label_of(u.selected)));
    }
    if (can_edit && !u.pad_mode) {
        draw_gizmo(d, gizmo, u.gizmo_hot, k);
        // Which way the handles lie, beside them: the one thing you cannot read off the gizmo itself.
        if (gizmo.valid)
            text(d, f.bold, 12 * k, ImVec2(gizmo.centre.x + 14 * k, gizmo.centre.y + 10 * k), IM_COL32(255, 255, 255, 120),
                 u.local_axes ? "OBJECT" : "WORLD");
    }
    if (hovered && *hovered != u.selected)
        if (const auto at = project(joint_view(v, *hovered)->position))
            text(d, f.bold, 13 * k, ImVec2(at->x + 10 * k, at->y - 22 * k), white, upper(label_of(*hovered)));
    if (u.menu_open || (!inside && u.gizmo_handle < 0 && !u.dragging_gizmo && !u.orbiting && !u.panning)) return;
    auto& io = ImGui::GetIO();
    if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
        // Press and hold a joint: it is grabbed, and the limb above it follows the mouse. Shift turns the
        // selected joint with the gizmo instead.
        if (u.gizmo_hot >= 0 && can_edit) {
            u.gizmo_handle = u.gizmo_hot;
            u.axis = u.gizmo_hot % 3;
            u.drag_start = mouse;
            u.drag_moved = false;
            u.ring_from = std::atan2(mouse.y - gizmo.centre.y, mouse.x - gizmo.centre.x);
            begin_turn(v);
        } else if (hovered && *hovered != u.selected) u.selected = *hovered;
        else if (sel && can_edit) {
            u.dragging_gizmo = true;
            u.drag_start = mouse;
            u.drag_moved = false;
            begin_turn(v);
        }
    }
    // Pressing is not yet dragging. Until the mouse has really moved nothing is applied: a press that
    // writes a key would key the pose where it stands, and on a hand or a foot that is a fresh bend of
    // the whole limb, which threw the leg about as soon as a handle was touched.
    const auto drag_counts = [&] {
        if (!u.drag_moved && std::hypot(mouse.x - u.drag_start.x, mouse.y - u.drag_start.y) > 3 * k) u.drag_moved = true;
        return u.drag_moved;
    };
    if (u.gizmo_handle >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) u.gizmo_handle = -1;
        else if (project.valid && drag_counts()) drag_gizmo(v, project, gizmo, mouse);
    }
    // Whatever the drag is doing, anything it has run into is shoved clear afterwards.
    if ((u.gizmo_handle >= 0 || u.dragging_gizmo) && u.drag_moved) shove_clear(v, u.selected);
    if (u.dragging_gizmo) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) u.dragging_gizmo = false;
        else if (project.valid && drag_counts()) {
            // A hand or a foot in MOVE goes where the mouse is over the board, the limb bending to reach;
            // anything else turns, as a free drag always has.
            if (u.move_mode && limb_end(u.selected)) drag_limb_to(v, project, mouse);
            else apply_turn(v, project, (mouse.x - u.drag_start.x) / k, (mouse.y - u.drag_start.y) / k, u.move_mode);
        }
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
    if (camera_changed) {
        // Taking the camera yourself is leaving the shot: it stops travelling with him from here.
        if (u.camera_shot) {
            u.camera_shot = 0;
            studio::set_camera_follow(false);
        }
        studio::set_orbit(orbit);
    }
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
    if (ctrl && pressed(ImGuiKey_C)) studio::copy_pose();
    if (ctrl && pressed(ImGuiKey_V)) {
        studio::begin_change();
        studio::paste_pose();
    }
    if (ctrl && pressed(ImGuiKey_S)) save_now();
    if (ctrl && pressed(ImGuiKey_A)) pick_all(v);
    if (!ctrl && pressed(ImGuiKey_Escape)) {
        // Esc backs out: the chosen keys first, then a step back through the trick list.
        if (!u.picked.empty()) u.picked.clear();
        else go_back_in_tricks();
    }
    if (!ctrl && pressed(ImGuiKey_C)) {
        u.anti_clip = !u.anti_clip;
        say(u.anti_clip ? "Keep clear on: what you move stays out of the body and off the deck." : "Keep clear off.");
    }
    if (!ctrl && pressed(ImGuiKey_V)) next_shot(v, io.KeyShift ? -1 : 1);
    if (!ctrl && pressed(ImGuiKey_H)) {
        u.hide_ui = !u.hide_ui;
        if (u.hide_ui) say("Hidden. H brings it back.");
    }
    if (!ctrl && pressed(ImGuiKey_J)) {
        u.hide_joints = !u.hide_joints;
        say(u.hide_joints ? "Joints hidden. J shows them again." : "Joints shown.");
    }
    if (pressed(ImGuiKey_Equal) || pressed(ImGuiKey_KeypadAdd)) zoom_timeline(1.5f);
    if (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract)) zoom_timeline(1 / 1.5f);
    if (!ctrl && pressed(ImGuiKey_F)) studio::focus_joint(u.selected);
    if (!ctrl && pressed(ImGuiKey_T) && v.has_clip) toggle_channels(u.selected, false);
    if (!ctrl && pressed(ImGuiKey_K) && v.has_clip) key_joint();
    if (pressed(ImGuiKey_Delete)) {
        if (static_cast<Tab>(u.tab) == Tab::packs) delete_row(v);
        else if (!u.picked.empty()) delete_picked();
        else delete_key(v);
    }
    if (!ctrl && pressed(ImGuiKey_X)) u.axis = u.axis == 0 ? -1 : 0;
    if (!ctrl && pressed(ImGuiKey_Y)) u.axis = u.axis == 1 ? -1 : 1;
    if (!ctrl && pressed(ImGuiKey_Z)) u.axis = u.axis == 2 ? -1 : 2;
    if (!ctrl && pressed(ImGuiKey_R)) u.move_mode = false;
    if (!ctrl && pressed(ImGuiKey_G)) u.move_mode = true;
    if (!ctrl && pressed(ImGuiKey_L)) u.local_axes = !u.local_axes;
    if (!ctrl && pressed(ImGuiKey_S)) studio::set_solo(!v.solo);
    if (!ctrl && pressed(ImGuiKey_N)) {
        u.snap = !u.snap;
        say(u.snap ? "Snapping on: a foot you move lands on the deck." : "Snapping off.");
    }
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
    if (set) {
        studio::set_orbit(orbit);
        if (u.camera_shot) {
            u.camera_shot = 0;
            studio::set_camera_follow(false);
        }
    }
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
                {Glyph::lb, "", "+ LS: start anim."},
                {Glyph::rb, "", "+ RS: end anim."},
                {Glyph::l3, "", "+ R3 hold: back"}};
    else
        list = {{std::nullopt, "LMB", "Pick / turn"},
                {std::nullopt, "RMB", "Orbit"},
                {std::nullopt, "MMB", "Pan"},
                {std::nullopt, "G / R", u.move_mode ? "Move" : "Turn"},
                {std::nullopt, "X Y Z", "Axis"},
                {std::nullopt, "L", u.local_axes ? "Object axes" : "World axes"},
                {std::nullopt, "S", v.solo ? "Solo on" : "Solo off"},
                {std::nullopt, "N", u.snap ? "Snapping on" : "Snapping off"},
                {std::nullopt, "J", u.hide_joints ? "Joints hidden" : "Hide joints"},
                {std::nullopt, "H", "Hide everything"},
                {std::nullopt, "C", u.anti_clip ? "Keep clear on" : "Keep clear off"},
                {std::nullopt, "V", shots[static_cast<std::size_t>(std::clamp(u.camera_shot, 0, 5))].name},
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
    // The chosen keys: what can be done with a group chosen by a box dragged with the right button, or by
    // Shift + click. Under the shortcuts and over the timeline, where it is always seen and never covers a key.
    if (v.has_clip) {
        const bool any = !u.picked.empty();
        const float bh = 30 * k, gap_x = 8 * k, top = pb.y + (50 * k - bh) * 0.5f;
        float x = pb.x;
        const auto next = [&](float w) {
            x -= w;
            const ImVec2 at(x, top);
            x -= gap_x;
            return at;
        };
        if (button(d, f, "##groupclear", next(84 * k), ImVec2(84 * k, bh), "CLEAR", false, any)) {
            u.picked.clear();
            say("No keys chosen.");
        }
        if (button(d, f, "##groupdelete", next(96 * k), ImVec2(96 * k, bh), "DELETE", false, any)) delete_picked();
        if (button(d, f, "##groupsmooth", next(96 * k), ImVec2(96 * k, bh), "SMOOTH", false, any)) smooth_picked();
        if (button(d, f, "##groupall", next(64 * k), ImVec2(64 * k, bh), "ALL")) pick_all(v);
        const auto label = any ? std::to_string(u.picked.size()) + (u.picked.size() == 1 ? " KEY CHOSEN" : " KEYS CHOSEN")
                               : std::string("CHOSEN KEYS");
        const auto lw = text_size(f.bold, 13 * k, label.c_str());
        text(d, f.bold, 13 * k, ImVec2(x - lw.x - 6 * k, top + (bh - lw.y) * 0.5f), any ? blue_light : soft, label);
    }
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
    std::string packs;
    for (const auto& pack : v.packs)
        if (pack.unsaved) packs += (packs.empty() ? "" : ", ") + pack.name;
    const bool animation = v.dirty && v.has_clip;
    const std::string what = animation && !packs.empty() ? "\"" + title_of(v) + "\" and the pack " + packs + " have"
                             : animation ? "\"" + title_of(v) + "\" has" : "The pack " + packs + " has";
    const std::string question = what + " changes that are not saved. Save them before you go back to skating?";
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

// While Record all tricks runs: a strip across the top, as skate.'s own banners, with how far it is.
void draw_animation_studio_batch() {
    const auto p = studio::batch::progress();
    if (!p.active || studio::loading().alpha > 0.99f) return;
    auto& s = state();
    const auto display = ImGui::GetIO().DisplaySize;
    ImFont* heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    ImFont* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const float H = display.y;
    auto* d = ImGui::GetForegroundDrawList();
    const ImVec2 a(0, 0), b(display.x, 0.085f * H);
    d->AddRectFilled(a, b, IM_COL32(12, 12, 13, 225));
    d->AddRectFilled(ImVec2(0, b.y - 0.004f * H), ImVec2(display.x * (p.total ? static_cast<float>(p.place) / static_cast<float>(p.total) : 0.0f), b.y),
                     skate_theme::blue);
    const std::string title = p.total ? std::format("RECORDING YOUR TRICKS  {} OF {}", std::min(p.place + 1, p.total), p.total) : std::string("RECORDING YOUR TRICKS");
    text(d, heading, 0.026f * H, ImVec2(0.034f * display.x, 0.014f * H), skate_theme::white, title);
    text(d, bold, 0.019f * H, ImVec2(0.034f * display.x, 0.048f * H), IM_COL32(200, 200, 204, 255), p.status);
    const char* hint = p.stopping ? "STOPPING AFTER THIS TRICK" : "ESC / BACK: STOP AFTER THIS TRICK";
    const auto hw = text_size(heading, 0.019f * H, hint);
    text(d, heading, 0.019f * H, ImVec2(display.x - 0.034f * display.x - hw.x, 0.03f * H), p.stopping ? skate_theme::warning : skate_theme::white, hint);
}

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
bool animation_studio_loading_pending() { return studio::loading().alpha > 0 || studio::batch::active(); }
// While a trick is being recorded (the studio steps out so you can skate): a strip across the top in the
// studio's look, and Esc (Back on a controller) goes back to the studio without one.
bool animation_studio_recording_pending() {
    return !studio::batch::active() && !studio::is_open() && studio::recording_state() != studio::Recording::off;
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
    // The background is always opaque: the game's own loading art underneath must not show through while
    // this fades. Only the content fades.
    d->AddRectFilled(ImVec2(0, 0), display, IM_COL32(19, 19, 19, 255));
    // The loading picture, filled over the screen with its own shape kept. It carries the wordmark, the
    // blue tape and who built it already, so none of those are drawn over it and nothing is veiled: the
    // artwork is shown as it is, and only the sides are lost on a screen less wide than it.
    const bool backdrop_drawn = draw_studio_backdrop_cover(d, ImVec2(0, 0), display, IM_COL32(255, 255, 255, 255));
    if (backdrop_drawn) {
    } else if (!draw_studio_picture(d, StudioPicture::grain, ImVec2(0, 0), display, fade(IM_COL32(255, 255, 255, 150))))
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
    if (!backdrop_drawn) {
    const float word_size = 0.40f * H, word_track = -0.045f;
    const float word_w = tight_width("reskate.", word_size, word_track);
    const ImVec2 word_at(display.x * 0.5f - word_w * 0.5f, H * 0.215f);
    tight("reskate.", word_size, word_track, ImVec2(word_at.x + 5 * k, word_at.y + 7 * k), fade(IM_COL32(0, 0, 0, 140)), true);
    tight("reskate.", word_size, word_track, word_at, fade(paper), true);
    // Worn paper: the grain again, dark, over the letters (the dark concrete around hides it).
    draw_studio_picture(d, StudioPicture::grain, ImVec2(word_at.x - 20 * k, word_at.y), ImVec2(word_at.x + word_w + 20 * k, word_at.y + word_size * 1.1f),
                        fade(IM_COL32(30, 28, 25, 230)));

    // animator studio on a strip of blue tape under it.
    const float sub_size = 0.115f * H, sub_track = -0.03f;
    const float sub_w = tight_width("animator studio", sub_size, sub_track);
    const float tape_top = word_at.y + word_size * 0.99f, tape_h = 0.13f * H;
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

    }

    // Bottom left: a tip, a new one every few seconds, and a bar that fills while it waits.
    {
        static constexpr std::array<const char*, 11> tips{
            "Pick a trick in TRICKS, then its pop: low, mid or high.",
            "Click a dot on the skater, or choose a joint in JOINTS, then drag to turn it.",
            "The arrows on a joint move it along one axis, the rings turn it about one.",
            "G moves, R turns, X Y Z lock an axis, F follows the joint.",
            "S turns one joint alone. N keeps a foot on the deck. L swaps object and world.",
            "K keys the pose on this frame. Keys always sit on whole frames.",
            "G moves the hips or the board. X, Y and Z lock an axis.",
            "Save your trick and do it in game: your version plays on that trick.",
            "Copy to low, mid and high pop in PACKS to fit every pop.",
            "Packs group your tricks: switch them on and off, and share them, in PACKS.",
            "On a controller: the left stick turns, the D-pad steps frames, RT and LT scrub, A keys."};
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
        // The size and the place skate.'s own loading box has, so ours reads as the same thing.
        const float box_h = 0.088f * H, box_w = 0.300f * H;
        // Up off the bottom edge, with what is going on under it in the corner.
        const ImVec2 bb(display.x - 0.034f * display.x, H - 0.125f * H), ba(bb.x - box_w, bb.y - box_h);
        // skate.'s box: black, with a light rough edge.
        const float rim = 2.5f * k;
        if (!draw_studio_tile(d, ba, bb, 8 * k, fade(IM_COL32(205, 205, 205, 255)))) skate_theme::rough_rect(d, ba, bb, fade(IM_COL32(205, 205, 205, 255)), 5, 1.2f * k);
        const ImVec2 ia(ba.x + rim, ba.y + rim), ib(bb.x - rim, bb.y - rim);
        if (!draw_studio_tile(d, ia, ib, 8 * k, fade(IM_COL32(8, 8, 9, 255)))) skate_theme::rough_rect(d, ia, ib, fade(IM_COL32(8, 8, 9, 255)), 6, 1.2f * k);
        const char* dots[]{"Loading", "Loading.", "Loading..", "Loading..."};
        const auto loading_text = dots[static_cast<int>(l.seconds * 2.5f) % 4];
        const auto lw = text_size(bold, 0.030f * H, "Loading...");
        text(d, bold, 0.030f * H, ImVec2(ba.x + 0.030f * H, ba.y + (box_h - lw.y) * 0.5f), fade(skate_theme::white), loading_text);
        const ImVec2 fa(bb.x - box_h * 1.25f - 0.012f * H, ba.y + box_h * 0.1f), fb(bb.x - 0.012f * H, bb.y - box_h * 0.1f);
        if (!draw_studio_kickflip(d, fa, fb, l.seconds, fade(skate_theme::white))) {
            // Without the game's board: a wheel that spins.
            const ImVec2 c((fa.x + fb.x) * 0.5f, (fa.y + fb.y) * 0.5f);
            const float wr = (fb.y - fa.y) * 0.35f, t = static_cast<float>(ImGui::GetTime());
            d->AddCircle(c, wr, fade(skate_theme::white), 24, 3 * k);
            d->AddLine(c, ImVec2(c.x + std::cos(t * 6) * wr, c.y + std::sin(t * 6) * wr), fade(skate_theme::blue), 3 * k);
        }
        // What is going on, in the bottom right corner, under the box.
        const auto ww = text_size(heading, 0.026f * H, what);
        text(d, heading, 0.026f * H, ImVec2(bb.x - ww.x, H - 0.05f * H - ww.y * 0.5f), fade(skate_theme::white), what);
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
    // With everything hidden the skater has the whole screen and is aimed at the middle of it: nothing is
    // beside him or under him to leave room for, and a clip filmed this way wants him centred.
    const ImVec2 view_min(u.hide_ui || u.menu_hidden ? left : menu_x + menu_w + 30 * k, 0),
        view_max(right, u.hide_ui ? display.y : display.y - timeline_h);
    // The skater stands in the free space right of the menu.
    studio::set_view_shift((display.y * 0.5f - (view_min.y + view_max.y) * 0.5f) / display.y,
                           ((view_min.x + view_max.x) * 0.5f - display.x * 0.5f) / display.y);
    float help_top = display.y; // the top of the box that says what a row does, so nothing is written over it
    const auto project = projector(v, display);
    controller(v, project);
    viewport(d, f, v, project, view_min, view_max);
    if (u.hide_ui) {
        // Only the skater and the room he stands in: for filming a trick or taking a picture of it. The
        // camera and the playhead still answer, so a trick can be played and watched clean. The window is
        // closed the same way as below: leaving it open here left ImGui with a window it was never given
        // back, and it said so on the screen.
        keyboard(v);
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }
    shot_strip(d, f, v, view_min, view_max);
    if (!u.menu_hidden) {
        page_title(d, f, ImVec2(menu_x, 40 * k));
        tab_row(d, f, ImVec2(menu_x, 140 * k), menu_w + 10 * k);
        float list_top = 205 * k;
        const float card_h = 190 * k;
        if (static_cast<Tab>(u.tab) == Tab::tricks && u.asset_document.empty()) {
            text(d, f.heading, 15 * k, ImVec2(menu_x + 16 * k, list_top), blue_light, "SEARCH");
            ImGui::SetCursorScreenPos(ImVec2(menu_x + 16 * k, list_top + 22 * k));
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) ImGui::SetKeyboardFocusHere();
            const bool changed = text_field(f, "##nativequery", u.asset_query, menu_w - 104 * k, "Trick name, e.g. hardflip");
            const bool clear = button(d, f, "##clearquery", ImVec2(menu_x + menu_w - 86 * k, list_top + 22 * k), ImVec2(70 * k, 30 * k),
                                      "CLEAR", false, u.asset_query[0] != 0);
            if (changed || clear) {
                if (clear) u.asset_query.fill(0);
                u.asset_filter = studio::native_search_terms(u.asset_query.data()).empty() ? "" : u.asset_query.data();
                // The search filters whichever list you are in. It used to throw you into the native asset
                // tree, which on an install without that index built is empty, so every search read as
                // nothing found even when the trick was right there in the list.
                u.open_trick.clear();
                u.trick_category.clear(); // a search looks through every kind
                u.asset_page = 0; u.row = 0;
                u.list_scroll[static_cast<std::size_t>(u.tab)] = 0;
                u.list_followed[static_cast<std::size_t>(u.tab)] = -1;
            }
            list_top += 70 * k;
        }
        // The white card only where it has something: the joint's numbers (JOINTS, KEYS) and the
        // animation's name, trick and maker (PACKS). Elsewhere the list runs down to the timeline.
        const auto tab = static_cast<Tab>(u.tab);
        const bool has_card = tab == Tab::joints || tab == Tab::keys || tab == Tab::packs;
        const float card_top = display.y - timeline_h - card_h - 22 * k;
        const float list_bottom = has_card ? card_top - 18 * k : display.y - timeline_h - 24 * k;
        list(d, f, v, ImVec2(menu_x + 16 * k, list_top), ImVec2(menu_w - 16 * k, std::max(120 * k, list_bottom - list_top)));
        if (has_card) card(d, f, v, ImVec2(menu_x, card_top), ImVec2(menu_w + 10 * k, card_h));
        help_top = help_box(d, f, v, ImVec2(menu_x + menu_w + 26 * k, card_top), ImVec2(330 * k, card_h));
    }
    timeline(d, f, v, ImVec2(left, display.y - timeline_h), ImVec2(frame_w, timeline_h));
    // What the studio last said. It used to stand there for good, so the screen kept a line about a trick
    // opened minutes ago; it is held long enough to read, by its length, and then goes. A failure stays,
    // because that is something to act on.
    std::string line = v.failure.empty() ? v.status : v.failure;
    if (v.failure.empty()) {
        if (line != u.status_said) {
            u.status_said = line;
            u.status_until = ImGui::GetTime() + 4.0 + static_cast<double>(line.size()) * 0.045; // about a word a third of a second
        }
        if (ImGui::GetTime() > u.status_until) line.clear();
    }
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
        const ImVec2 at(view_min.x + 24 * k, std::min(view_max.y - 40 * k, help_top - w.y - 26 * k));
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
