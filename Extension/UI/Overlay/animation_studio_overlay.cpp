#include "overlay_internal.h"
#include "skate_style.h"
#include "animation_studio_images.h"
#include "Extension/AnimationStudio/anim_studio.h"
#include "Extension/AnimationStudio/native_asset_names.h"
#include "Extension/AnimationStudio/native_trick_parts.h"
#include "Extension/AnimationStudio/marketplace.h"
#include "Extension/AnimationStudio/trick_batch.h"
#include "studio_cover_maker.h"
#include "studio_market_art.h"
#include "studio_torn.h"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// ReSkate Animator Studio's screen. Two parts: a full-screen menu in skate.'s own style (TRICKS to pick a trick,
// PACKS to keep your packs, SETTINGS), and the editor - the skater in the middle on the studio's own camera, the tools
// down the left, the chosen joint's card on the right and a timeline of the body's joints along the bottom. Drawn over
// the game while the studio is open (F8, or L3 + R3 held on a controller); the game gets no input meanwhile. Everything
// works from the keyboard and mouse or from a controller: the prompts follow whichever was used last.

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
// The tools down the left: how wide (times the scale), and where they start from the left edge.
constexpr float tool_strip_width = 148, tool_strip_margin = 20;
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
    float timeline_zoom{1}, timeline_scroll{}, rows_scroll{};
    bool rows_bar_held{};  // the joint list's scroll bar is being dragged
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
    // The right button over the timeline's rows: dragged, a box that chooses the keys inside it; clicked, the round
    // menu of what can be done with the chosen keys.
    bool band_held{}, band_moved{};
    ImVec2 band_from{};
    // Keys chosen together (a click, Shift + click adds, Ctrl + click takes one off, a box, Ctrl + A): dragged, moved,
    // copied and deleted as one.
    std::vector<std::pair<std::uint16_t, float>> picked;
    // The round menu round the mouse, and what COPY took of the chosen keys (Ctrl + V or PASTE puts them at the playhead).
    bool radial{};
    ImVec2 radial_at{};
    double radial_since{};
    struct CopiedKey {
        std::uint16_t joint{};
        float time{};
        studio::Quat rotation{0, 0, 0, 1};
        studio::Vec3 offset{};
        studio::Ease ease{};
    };
    std::vector<CopiedKey> copied_keys;
    bool copied_are_keys{}; // the last copy was keys, not the pose
    // The joint's state when a turn (mouse drag or stick) began, and how far it has gone since.
    studio::Offset drag_offset;
    studio::Vec3 drag_field{}; // a hand's or a foot's POSITION fields when the drag began
    studio::Quat drag_world{0, 0, 0, 1}, drag_parent{0, 0, 0, 1};
    float drag_parent_scale{1};
    ImVec2 drag_start{};
    int axis{-1}; // -1 free, 0 X, 1 Y, 2 Z
    bool local_axes{true}, move_mode{};
    // The gizmo on the chosen joint: three arrows to move along an axis, three rings to turn about one.
    int gizmo_handle{-1};      // the one being dragged: 0 to 2 the arrows, 3 to 5 the rings, -1 none
    int gizmo_hot{-1};         // the one under the mouse
    studio::Vec3 drag_point{}; // where the joint was when the drag began
    float ring_from{};         // the angle round the ring where a ring drag began
    bool drag_moved{};         // the mouse has gone far enough for the drag to count
    bool snap{};               // a foot moved in the air lands on the deck
    bool feet_lock{};          // FEET+BOARD: the feet are locked to the deck (lock_feet_to_deck)
    bool own_lead_right{};     // the skater's own stance leads with the right foot (View::own_lead_right), for the stance tags
    // More joints chosen with the one the gizmo is on (Ctrl + click): they are turned and moved with it (move_group).
    std::vector<std::uint16_t> also;
    struct GroupStart {
        std::uint16_t joint{};
        studio::Offset offset;
        studio::Vec3 point{};
        studio::Quat world{0, 0, 0, 1}, parent{0, 0, 0, 1};
        float scale{1};
    };
    std::vector<GroupStart> group_start; // each of them as it was when the turn began
    studio::Vec3 board_point{};          // the board's mount then: the feet on it turn about it

    std::array<char, 64> name{}, author{};
    std::array<char, 64> pack_name{}; // PACKS: the chosen pack's name as it is typed (RENAME)
    std::string pack_name_for;        // the pack it was filled for
    std::array<char, 64> label{};     // a saved trick's name as it is typed
    std::string label_for;            // the trick it was filled for
    std::array<char, 64> your_name{}; // SETTINGS > Tricks: MADE BY
    bool your_name_filled{};
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
    bool shoulder_shifted{}; // a shoulder was held and the stick moved a bracket, so it is not a tap
    float bracket_at[2]{};   // where each bracket is being carried to, while a stick carries it
    // The sticks, filtered over time: a light touch and a hard push both settle, with no jump at the dead zone's edge.
    float filtered[4]{};
    float scrub_time{};
    bool combo_armed{true}; // false while the hold that opened the studio is still on
    float stick_dx{}, stick_dy{};
    double stick_idle_since{};

    // ---- the screen ----
    // The full-screen menu (TRICKS, PACKS, SETTINGS), or the editor with the skater. The menu is up when the studio
    // opens with nothing to edit, and whenever MENU (Tab, View on a pad) is pressed.
    bool menu{true};
    bool was_open{};  // the studio was open last frame
    int menu_tab{0};  // 0 TRICKS, 1 PACKS, 2 MARKET, 3 SETTINGS
    // MARKET, the shop: the part shown (featured, browse, crew, liked, installed, mine), BROWSE's order and whether it
    // shows only the crew's, the search, the pack whose page is open, the crew member whose page is open, the spotlight's
    // pick and since when, the wall's fade, and the cards over it (sign in, share, liked).
    std::string market_list{"featured"};
    std::string market_sort{"top"};
    bool market_crew_only{};
    std::array<char, 64> market_query{};
    std::string market_query_seen;
    std::string market_open;
    std::string market_crew;
    float market_scroll{}, market_detail_scroll{}, market_lists_scroll{};
    int spotlight{};
    double spotlight_since{};
    float market_fade{};
    std::string like_pack; // the LIKED! card's pack ("" closed)
    // The cover designer: the pack it is for ("" closed), the cover as it is being made, its title as typed.
    std::string cover_pack;
    CoverDesign cover;
    std::array<char, 64> cover_title{};
    float cover_scroll{};
    bool sign_in{};
    std::array<char, 64> ts_team{};
    std::array<char, 512> ts_token{};
    std::string share_pack;                   // the pack the SHARE (or UPDATE) card is for ("" closed)
    std::array<char, 2001> share_description{};
    std::string share_description_for;
    std::array<char, 64> share_version{};     // the version it goes up as
    std::string share_version_for;
    float menu_fade{}; // eased in as it comes up
    bool blocked{};   // a card or the round menu is over the editor: the skater and the timeline take no clicks
    // TRICKS: the kind shown ("*mine" MY TRICKS, "*assets" the game's animation tree, or a category) and the saved trick
    // whose card is open beside the grid.
    std::string trick_group{"*mine"};
    std::string trick_page;
    std::string piece_card; // a card of the game's pieces (a grind, a grab) open beside the grid, by its key
    float groups_scroll{}, cards_scroll{}, drawer_scroll{}, list_scroll{};
    int row{};        // the game's animation tree: the row last chosen
    // PACKS
    bool pack_page{};         // a pack is open on its own page (else MY PACKS, every pack)
    float hub_scroll{};       // MY PACKS' scroll
    float pack_tricks_scroll{}, rules_scroll{};
    bool pack_adding{};       // ADD TRICKS is open over the pack
    float adding_scroll{};
    bool conflicts_open{};    // the duplicate tricks card
    float conflicts_scroll{};
    // SETTINGS: -1 the tiles; 0 Studio, 1 Skater, 2 Tricks, 3 Packs.
    int settings_page{-1};
    float settings_scroll{};
    // Choosing a picture for a pack, or a folder for exports or imports, inside the studio: Windows' own boxes cannot
    // be clicked while the game has the mouse.
    struct Browser {
        bool open{};
        int mode{}; // 0 a pack's picture, 1 the export folder, 2 the import folder
        std::string pack;
        std::filesystem::path dir;
        std::filesystem::path listed; // the folder `entries` were read from
        struct Entry {
            std::string name;
            std::filesystem::path path;
            bool folder{};
        };
        std::vector<Entry> entries;
        std::string error;
        float scroll{};
    } browser;
    // Saving: the card after a trick is saved the first time (name it, test it, put it in a pack), and the line the studio
    // says when you are back from testing one.
    bool save_card{};
    std::string save_for;
    std::string test_line, test_line_for;
    // The tool strip's card about the tool under the mouse.
    std::string tool_tip, tool_tip_text;
    ImVec2 tool_tip_at{};

    bool hide_joints{};          // J: the dots and the gizmo are out of the way
    bool hide_ui{};              // H: nothing but the skater and the room, for filming and pictures
    bool anti_clip{true};        // a joint being placed is kept out of the body and the deck
    float clear_force{1.0f};     // how wide the body is taken to be: 0 nothing, 1 the full shapes
    float ground_force{0.6f};    // the ground force field (SETTINGS): how much of a joint's thickness is kept above the floor; 0 off
    float crossfade_slider{};    // the Crossfade setting on its slider, 0 to 1 (0.1 to 0.8 s), from the studio each frame
    bool controls_open{true};    // the shortcuts panel out (its tab at its side, or F1, slides it in and out)
    float controls_t{1};         // how far out it is, eased
    bool shove_limbs{true};      // a limb that is run into gives way as well, not only what you move
    int camera_shot{};           // which of the shots the camera is set to, 0 while it is yours
    bool fisheye{true};          // FILMER looks through a fisheye, as a filmer's VX does
    std::uint64_t clip_seen{};   // the clip the overlay last started clean on
    int warning_choice{};
    bool zoom_to_playhead{};
    // The game's animation tree (TRICKS > GAME ANIMATIONS).
    bool browse_assets{};
    bool asset_ready_only{}, asset_inspect{};
    std::array<char, 64> asset_query{}; // the search box over TRICKS
    std::array<char, 64> pack_query{};  // the search box over MY PACKS
    bool pack_settings{};               // PACKS > SETTINGS is open (the playlist)
    float pack_settings_scroll{}, playlist_pick_scroll{};
    // Watching a saved trick (preview mode): the pack it is from, and where to go back to.
    std::string preview_pack, preview_name;
    bool preview_from_pack{};
    // A trick just put in a pack: the card asking for its other stances in that pack.
    std::string stance_pack, stance_name;
    // TRICKS' hover preview: the card under the mouse, since when, and the skater doing its trick (studio::peek_...).
    struct Hover {
        std::string key;
        double since{-1};
        ImVec2 a{}, b{};
        std::function<bool()> start;
        bool tried{}, peeking{}, done{};
        float duration{};
        studio::Orbit orbit; // the camera as it was, put back after
    } hover;
    std::string hover_candidate, hover_candidate_title, hover_title;
    ImVec2 hover_candidate_a{}, hover_candidate_b{};
    std::function<bool()> hover_candidate_start;
    // The window the skater is seen through while a trick is looked at: the menu leaves it clear (last frame's).
    bool peek_hole{};
    ImVec2 peek_a{}, peek_b{};
    ImVec2 grid_a{}, grid_b{}; // where TRICKS' cards are, for the window to sit in
    std::string asset_filter;
    std::string asset_document, asset_json_path;
    std::vector<std::pair<std::string, std::string>> asset_history;
    std::size_t asset_return_page{};
    std::string asset_category;
    std::size_t asset_page{};
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

// FILMER's fisheye: the game's frame drawn again bent, as a fisheye bends it. A point `r` (as a share of half the
// screen's width) out from the middle shows what was `scale * r * (1 + bend * r * r)` out: the middle comes closer, the
// edges are squeezed in, and the corners, which would show what is off the frame, go black.
struct Lens {
    bool on{};
    float scale{0.80f}, bend{0.25f};
};
Lens& lens() {
    static Lens l;
    return l;
}
// The studio camera's projection, as the nametags place a world point (nametag_overlay.cpp), through the lens when it
// is on, so the dots and the gizmo sit on the skater as he is shown.
struct Projector {
    studio::Vec3 right{}, up{}, back{}, origin{};
    float focal{}, cx{}, cy{};
    bool valid{};
    Lens bent{};
    std::optional<ImVec2> operator()(const studio::Vec3& p) const {
        if (!valid) return std::nullopt;
        const studio::Vec3 d{p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
        const float depth = -(d[0] * back[0] + d[1] * back[1] + d[2] * back[2]);
        if (depth < 0.05f) return std::nullopt;
        const float side = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        const float height = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        return through_lens(ImVec2(cx + side * focal / depth, cy - height * focal / depth));
    }
    // Where a point of the straight picture is shown through the lens (the lens's own sum, undone by a few steps).
    ImVec2 through_lens(ImVec2 q) const {
        if (!bent.on || cx <= 0) return q;
        const float rs = std::hypot(q.x - cx, q.y - cy) / cx;
        if (rs < 1e-5f) return q;
        float r = rs / bent.scale;
        for (int i = 0; i < 8; ++i) {
            const float f = bent.scale * r * (1 + bent.bend * r * r) - rs, slope = bent.scale * (1 + 3 * bent.bend * r * r);
            r -= f / slope;
        }
        return ImVec2(cx + (q.x - cx) * r / rs, cy + (q.y - cy) * r / rs);
    }
    // And the other way: what point of the straight picture shows at `p` (the mouse, for a drag).
    ImVec2 straight(ImVec2 p) const {
        if (!bent.on || cx <= 0) return p;
        const float r = std::hypot(p.x - cx, p.y - cy) / cx, f = bent.scale * (1 + bent.bend * r * r);
        return ImVec2(cx + (p.x - cx) * f, cy + (p.y - cy) * f);
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
    p.bent = lens();
    return p;
}

// ---- drawing ---------------------------------------------------------------------------------------
struct Fonts {
    ImFont *body, *bold, *heading, *title;
    float k;
    ImFont *display{}, *display_big{}, *display_small{}; // Anton, for the MARKET's headlines
    ImFont* marker_small{};                              // the brush face, small
    std::array<ImFont*, 4> covers{};   // the cover designer's fonts: Bebas Neue, Rubik Dirt, Sedgwick Ave Display, Knewave
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
    say("Held by Joint limits: switch them off (LIMITS, on the left) to push it further.");
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

studio::Vec3 up_of(const studio::Quat& q); // below
// The deck's own directions in the world: its up, along it nose to tail, and across it.
struct Deck {
    studio::Vec3 up{0, 1, 0}, along{0, 0, 1}, across{1, 0, 0};
};
Deck deck_of(const studio::View& v, const studio::JointView& board);                                       // below
studio::Vec3 deck_point(const studio::Vec3& centre, const Deck& deck, studio::Vec3 target, bool inside); // below
// ---- several joints at once ------------------------------------------------------------------------
// Ctrl + click adds a joint to the chosen ones (or takes it off). A turn or a move of the one the gizmo is on is done
// to all of them by the same amount in the world, each about its own joint. With the board among them, the feet
// among them stay where they stand on the deck: they go with the board, the legs bending to keep them there - tweak
// the board and the feet stay snapped to it.
bool in_group(std::uint16_t joint) {
    const auto& u = ui();
    return joint == u.selected || std::find(u.also.begin(), u.also.end(), joint) != u.also.end();
}
void toggle_in_group(std::uint16_t joint) {
    auto& u = ui();
    if (joint == u.selected) {
        // The gizmo's own joint taken off: the next of the others carries the gizmo.
        if (u.also.empty()) return;
        u.selected = u.also.front();
        u.also.erase(u.also.begin());
        return;
    }
    if (const auto at = std::find(u.also.begin(), u.also.end(), joint); at != u.also.end()) u.also.erase(at);
    else u.also.push_back(joint);
}
// FEET+BOARD: the feet locked to the deck. They are put on it now (the deck brought under them first when it is too
// far for the legs to reach), they go with it when it is moved or turned, and a foot moved slides over the deck and
// stays inside its outline - it never leaves the deck - until the lock is let go (B, the tool again, or Esc).
// `bring_deck`: the deck may be moved to the feet (when the lock is put on), else only the feet move.
void snap_feet_to_deck(const studio::View& v, bool bring_deck) {
    const auto* board = joint_view(v, studio::board_joint);
    const auto* left = joint_view(v, 343);
    const auto* right = joint_view(v, 10);
    if (!board || !left || !right) return;
    studio::Vec3 centre = board->position;
    const auto deck = deck_of(v, *board);
    const auto& up = deck.up;
    const studio::Vec3 middle{(left->position[0] + right->position[0]) * 0.5f, (left->position[1] + right->position[1]) * 0.5f,
                              (left->position[2] + right->position[2]) * 0.5f};
    const studio::Vec3 off{middle[0] - centre[0], middle[1] - centre[1], middle[2] - centre[2]};
    const float gap = dot3(off, up) - studio::feet_above_board;
    if (bring_deck && std::abs(gap) > 0.12f) {
        // Too far for the legs: the deck comes up (or down) under the feet, its middle under theirs.
        studio::Vec3 shift{};
        for (int i = 0; i < 3; ++i) shift[i] = middle[i] - up[i] * studio::feet_above_board - centre[i];
        auto next = studio::offset_of(studio::board_joint);
        const auto local = studio::rotate(studio::conjugate(board->parent_rotation), shift);
        for (int i = 0; i < 3; ++i) next.offset[i] += local[i] / std::max(board->parent_scale, 0.01f);
        studio::set_offset(studio::board_joint, next);
        for (int i = 0; i < 3; ++i) centre[i] += shift[i];
    }
    for (const auto* foot : {left, right})
        studio::drag_limb(foot->joint, deck_point(centre, deck, foot->position, true));
}
void lock_feet_to_deck(const studio::View& v) {
    auto& u = ui();
    if (!v.has_clip || !joint_view(v, studio::board_joint)) {
        say("No board in this trick to lock the feet to.");
        return;
    }
    studio::begin_change();
    snap_feet_to_deck(v, true);
    u.feet_lock = true;
    u.selected = studio::board_joint;
    u.also = {343, 10}; // the left and right foot
    say("Feet locked to the deck: move or turn the board and they stay on it; a foot slides over the deck but cannot leave it. B lets go.");
}
void unlock_feet() {
    auto& u = ui();
    u.feet_lock = false;
    u.also.clear();
    say("Feet free of the deck.");
}
void begin_group(const studio::View& v) {
    auto& u = ui();
    u.group_start.clear();
    if (u.feet_lock && u.selected == studio::board_joint) {
        for (const std::uint16_t foot : {std::uint16_t{343}, std::uint16_t{10}})
            if (std::find(u.also.begin(), u.also.end(), foot) == u.also.end()) u.also.push_back(foot);
        snap_feet_to_deck(v, false); // on this frame too, before it moves
    }
    if (const auto* board = joint_view(v, studio::board_joint)) u.board_point = board->position;
    for (const auto joint : u.also) {
        const auto* view = joint_view(v, joint);
        if (!view || joint == u.selected) continue;
        u.group_start.push_back({joint, studio::offset_of(joint), view->position, view->rotation, view->parent_rotation, view->parent_scale});
    }
}
// `turn`: how the chosen joint turned in the world since the turn began; `shift`: how far it moved in the world.
void move_group(const studio::Quat& turn, const studio::Vec3& shift) {
    auto& u = ui();
    if (u.group_start.empty()) return;
    const bool board = in_group(studio::board_joint);
    for (const auto& g : u.group_start) {
        const bool foot = g.joint == 343 || g.joint == 10; // the left and right foot
        if (foot && board) {
            // On the deck: carried round the board's mount with it, and the leg bends to reach.
            studio::Vec3 from{};
            for (int i = 0; i < 3; ++i) from[i] = g.point[i] - u.board_point[i];
            const auto turned = studio::rotate(turn, from);
            studio::Vec3 to{};
            for (int i = 0; i < 3; ++i) to[i] = u.board_point[i] + turned[i] + shift[i];
            studio::drag_limb(g.joint, to);
            continue;
        }
        if (limb_end(g.joint) && (shift[0] != 0 || shift[1] != 0 || shift[2] != 0)) {
            studio::Vec3 to{};
            for (int i = 0; i < 3; ++i) to[i] = g.point[i] + shift[i];
            studio::drag_limb(g.joint, to);
            continue;
        }
        studio::Offset next = g.offset;
        if (g.joint == studio::board_joint)
            // The board's keys turn it in the ground frame (on the left), about its mount.
            next.rotation = studio::normalised(studio::multiply(studio::multiply(studio::multiply(studio::conjugate(g.parent), turn), g.parent), g.offset.rotation));
        else
            next.rotation = studio::normalised(studio::multiply(g.offset.rotation, studio::multiply(studio::multiply(studio::conjugate(g.world), turn), g.world)));
        const auto* e = studio::editable(g.joint);
        if ((e && e->moves) || g.joint == studio::board_joint) {
            const auto local = studio::rotate(studio::conjugate(g.parent), shift);
            for (int i = 0; i < 3; ++i) next.offset[i] += local[i] / std::max(g.scale, 0.01f);
        }
        studio::set_offset(g.joint, next);
    }
}
// How the chosen joint moved in the world, from its offset now against the one it had when the turn began.
void move_group_after(const studio::Offset& next) {
    auto& u = ui();
    if (u.group_start.empty()) return;
    studio::Quat turn{0, 0, 0, 1};
    if (u.selected == studio::board_joint)
        turn = studio::multiply(studio::multiply(u.drag_parent, studio::multiply(next.rotation, studio::conjugate(u.drag_offset.rotation))),
                                studio::conjugate(u.drag_parent));
    else
        turn = studio::multiply(studio::multiply(u.drag_world, studio::multiply(studio::conjugate(u.drag_offset.rotation), next.rotation)),
                                studio::conjugate(u.drag_world));
    studio::Vec3 local{};
    for (int i = 0; i < 3; ++i) local[i] = (next.offset[i] - u.drag_offset.offset[i]) * std::max(u.drag_parent_scale, 0.01f);
    move_group(studio::normalised(turn), studio::rotate(u.drag_parent, local));
}

// ---- turning the chosen joint (mouse drag or left stick) ---------------------------------------------
void begin_turn(const studio::View& v) {
    auto& u = ui();
    const auto* sel = joint_view(v, u.selected);
    if (!sel) return;
    begin_group(v);
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
bool snapping_now(const studio::View& v) { return (ui().snap || ui().feet_lock) && v.has_clip; }
// The deck's directions: which of the board's own axes is its up and which runs along it differ from clip to clip (the
// studio finds them per clip too, BoardShape), so they are read off the board as it lies - up the axis nearest the
// world's up, along the one of the other two nearest the line between his feet.
Deck deck_of(const studio::View& v, const studio::JointView& board) {
    std::array<studio::Vec3, 3> axes{};
    for (int i = 0; i < 3; ++i) {
        studio::Vec3 e{};
        e[static_cast<std::size_t>(i)] = 1;
        axes[static_cast<std::size_t>(i)] = studio::rotate(board.rotation, e);
    }
    std::size_t up = 1;
    for (std::size_t i = 0; i < 3; ++i)
        if (std::abs(axes[i][1]) > std::abs(axes[up][1])) up = i;
    studio::Vec3 feet{0, 0, 1};
    if (const auto* left = joint_view(v, 343), *right = joint_view(v, 10); left && right)
        feet = {left->position[0] - right->position[0], left->position[1] - right->position[1], left->position[2] - right->position[2]};
    std::size_t along = up == 0 ? 1 : 0, other = 3 - up - along;
    if (std::abs(dot3(axes[other], feet)) > std::abs(dot3(axes[along], feet))) std::swap(along, other);
    Deck deck;
    deck.up = axes[up];
    if (deck.up[1] < 0) deck.up = {-deck.up[0], -deck.up[1], -deck.up[2]};
    deck.along = axes[along];
    deck.across = axes[other];
    return deck;
}
// A point on the deck of a board at `centre`: at the feet's height over it, and - with the feet locked to it - inside its
// outline (along it short of the kicks, across it inside its edges).
studio::Vec3 deck_point(const studio::Vec3& centre, const Deck& deck, studio::Vec3 target, bool inside) {
    const auto& up = deck.up;
    const auto& along = deck.along;
    const auto& across = deck.across;
    const studio::Vec3 from{target[0] - centre[0], target[1] - centre[1], target[2] - centre[2]};
    float a = dot3(from, along), c = dot3(from, across);
    const float u_over = studio::feet_above_board;
    if (inside) {
        a = std::clamp(a, -0.34f, 0.34f);
        c = std::clamp(c, -0.085f, 0.085f);
    }
    for (int i = 0; i < 3; ++i) target[i] = centre[i] + along[i] * a + across[i] * c + up[i] * u_over;
    return target;
}
studio::Vec3 on_the_deck(const studio::View& v, std::uint16_t joint, studio::Vec3 target) {
    if ((joint != 10 && joint != 343) || !snapping_now(v)) return target;
    const auto* board = joint_view(v, studio::board_joint);
    if (!board) return target;
    return deck_point(board->position, deck_of(v, *board), target, ui().feet_lock);
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
// The ground force field (SETTINGS > Posing): nothing you move goes under the floor - as much of the joint's own
// thickness as the slider says is kept clear of it. Off (0) lets it through. Apart from Keep clear, which is his
// body and the deck.
studio::Vec3 keep_off_ground(const studio::View& v, std::uint16_t joint, studio::Vec3 target) {
    const float g = std::clamp(ui().ground_force, 0.0f, 1.0f);
    if (!v.has_clip || !v.floor_valid || g <= 0.005f) return target;
    const float lowest = v.floor + joint_radius(joint) * g;
    if (target[1] < lowest) target[1] = lowest;
    return target;
}
studio::Vec3 keep_clear(const studio::View& v, std::uint16_t joint, studio::Vec3 target) {
    const float force = std::clamp(ui().clear_force, 0.0f, 1.0f);
    if (!ui().anti_clip || !v.has_clip || force <= 0.01f) return keep_off_ground(v, joint, target);
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
    // The ground: its own force field (keep_off_ground).
    return keep_off_ground(v, joint, target);
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
// The axis a handle works along, in the world. Moving goes along the frame the offsets are kept in (the joint's
// parent); turning goes about the joint's own axes. The board's handles follow the board itself as it is turned now,
// rings and arrows alike, so OBJECT means its nose, its side and its grip; apply_axis brings what they do back into the
// ground frame its keys are kept in. Either can be the world's instead.
studio::Vec3 handle_axis(const studio::View& v, const studio::JointView& sel, std::uint16_t joint, int a, bool move, bool local) {
    studio::Vec3 unit{};
    unit[static_cast<std::size_t>(a)] = 1;
    if (!local) return unit;
    // A hand or a foot is moved by its POSITION fields, which are measured in the clip's own space: its
    // arrows point the way those numbers go, so pulling the X arrow moves X and nothing else.
    if (move && limb_end(joint)) return v.field_axes_valid ? studio::rotate(v.field_axes, unit) : unit;
    const bool parent_frame = move && joint != studio::board_joint;
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
    if (!v.floor_valid || u.ground_force <= 0.005f) return; // the ground force field off lets it through
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
        if (u.anti_clip && v.field_axes_valid && !(u.feet_lock && (u.selected == 10 || u.selected == 343))) {
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
        if (!u.group_start.empty() && v.field_axes_valid) {
            studio::Vec3 moved{};
            for (int i = 0; i < 3; ++i) moved[i] = field[i] - u.drag_field[i];
            move_group({0, 0, 0, 1}, studio::rotate(v.field_axes, moved));
        }
        return;
    }
    studio::Offset next = u.drag_offset;
    // The board as it is turned, in the ground frame its keys are kept in: its own axes, as they lie in that frame.
    const auto board_turn = studio::multiply(studio::conjugate(u.drag_parent), u.drag_world);
    if (move && e && e->moves) {
        // A world arrow is brought into the frame the offsets are kept in; a local one is that frame - except the
        // board's, which goes along the board's own axes.
        const auto step = !u.local_axes ? studio::rotate(studio::conjugate(u.drag_parent), unit)
                          : u.selected == studio::board_joint ? studio::rotate(board_turn, unit) : unit;
        for (int i = 0; i < 3; ++i) next.offset[i] += step[i] * amount / std::max(u.drag_parent_scale, 0.01f);
    } else {
        const auto q = studio::axis_angle(unit, amount);
        const auto turn = u.local_axes ? q : studio::multiply(studio::multiply(studio::conjugate(u.drag_world), q), u.drag_world);
        next.rotation = studio::normalised(studio::multiply(u.drag_offset.rotation, turn));
        // The board's keys turn it about the axes of the ground it starts on, so its turn goes on the left, in that
        // frame: a local ring (about the board's own axis, as it is turned now) and a world one are both brought into it.
        if (u.selected == studio::board_joint) {
            const auto ground = u.local_axes ? studio::multiply(studio::multiply(board_turn, q), studio::conjugate(board_turn))
                                             : studio::multiply(studio::multiply(studio::conjugate(u.drag_parent), q), u.drag_parent);
            next.rotation = studio::normalised(studio::multiply(ground, u.drag_offset.rotation));
        }
    }
    if (u.selected == studio::board_joint) keep_board_up(v, next);
    studio::set_offset(u.selected, next);
    move_group_after(next);
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
    mouse = p.straight(mouse);
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
    const bool locked_foot = ui().feet_lock && (ui().selected == 10 || ui().selected == 343);
    const auto placed = on_the_deck(v, ui().selected, target);
    studio::drag_limb(ui().selected, locked_foot ? placed : keep_clear(v, ui().selected, placed));
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
                // One axis: the board's own (OBJECT) or the world's, brought into the ground frame.
                studio::Vec3 axis{};
                axis[static_cast<std::size_t>(u.axis)] = 1;
                const auto q = studio::axis_angle(axis, (dx - dy) * radians);
                const auto board_turn = studio::multiply(studio::conjugate(u.drag_parent), u.drag_world);
                ground_turn = u.local_axes ? studio::multiply(studio::multiply(board_turn, q), studio::conjugate(board_turn))
                                           : studio::multiply(studio::multiply(studio::conjugate(u.drag_parent), q), u.drag_parent);
            } else {
                const auto world = studio::multiply(studio::axis_angle(p.up, dx * radians), studio::axis_angle(p.right, dy * radians));
                ground_turn = studio::multiply(studio::multiply(studio::conjugate(u.drag_parent), world), u.drag_parent);
            }
            next.rotation = studio::normalised(studio::multiply(ground_turn, u.drag_offset.rotation));
        }
    }
    if (u.selected == studio::board_joint) keep_board_up(v, next);
    studio::set_offset(u.selected, next);
    move_group_after(next);
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

// ---- choosing with the pad and the arrow keys --------------------------------------------------------
// Everything that can be pressed on the menu and the cards over it is put down by hit(), which notes where it is.
// The D-pad and the arrow keys go to the nearest one that way (from what the last frame drew), A and Enter press it,
// and the mouse takes over again the moment it moves.
struct Spot {
    ImGuiID id{};
    ImVec2 a{}, b{};
    int layer{}, region{};
    bool sideways{}; // left and right change its value (a setting) rather than leaving it
};
struct Nav {
    std::vector<Spot> drawn, last; // this frame's, and the last frame's
    ImGuiID focus{};               // the one chosen
    bool keys{};                   // the pad or the arrow keys are choosing: the chosen one is drawn lit
    bool press{};                  // A or Enter this frame: the chosen one is pressed
    int lr{};                      // left (-1) or right (1) this frame, for a setting's value
    int layer{};                   // what is being drawn: 0 the page, 1 a card over it
    int region{};                  // the scrolled list being drawn (0 none)
    ImVec2 clip_a{}, clip_b{};     // and its box: what is outside it cannot be pressed
    bool clipped{};
    int top{};                     // the highest layer drawn last frame: only that one can be chosen
    std::unordered_map<int, float> region_end; // each scrolled list's furthest scroll, as it was last frame
};
Nav& nav() {
    static auto* n = new Nav;
    return *n;
}
ImVec2 centre_of(const Spot& s) { return ImVec2((s.a.x + s.b.x) * 0.5f, (s.a.y + s.b.y) * 0.5f); }
const Spot* spot_of(ImGuiID id) {
    for (const auto& s : nav().last) if (s.id == id) return &s;
    return nullptr;
}
// A step of the D-pad or an arrow key: the nearest spot that way, things in line with it first.
void nav_step(int dx, int dy) {
    auto& n = nav();
    n.keys = true;
    const Spot* from = spot_of(n.focus);
    if (from && from->layer != n.top) from = nullptr;
    if (from && from->sideways && dx != 0 && dy == 0) {
        n.lr = dx; // the setting takes it
        return;
    }
    if (!from) {
        const Spot* first = nullptr;
        for (const auto& s : n.last)
            if (s.layer == n.top && (!first || s.a.y < first->a.y - 1 || (std::abs(s.a.y - first->a.y) <= 1 && s.a.x < first->a.x))) first = &s;
        if (first) n.focus = first->id;
        return;
    }
    const ImVec2 c = centre_of(*from);
    const Spot* best = nullptr;
    float best_score = FLT_MAX;
    for (const auto& s : n.last) {
        if (s.layer != n.top || s.id == from->id) continue;
        const ImVec2 o = centre_of(s);
        const float along = dx ? (o.x - c.x) * static_cast<float>(dx) : (o.y - c.y) * static_cast<float>(dy);
        if (along <= 2) continue;
        // In line: their edges overlap across the way it goes.
        const bool in_line = dx ? std::min(s.b.y, from->b.y) - std::max(s.a.y, from->a.y) > 0
                                : std::min(s.b.x, from->b.x) - std::max(s.a.x, from->a.x) > 0;
        const float across = in_line ? 0.0f : (dx ? std::abs(o.y - c.y) : std::abs(o.x - c.x));
        const float score = along + across * 3.0f;
        if (score < best_score) best_score = score, best = &s;
    }
    if (best) n.focus = best->id;
}
// Starts a frame: what was drawn last is what the pad chooses between.
void nav_frame() {
    auto& n = nav();
    n.last.swap(n.drawn);
    n.drawn.clear();
    n.top = 0;
    for (const auto& s : n.last) n.top = std::max(n.top, s.layer);
    n.layer = 0;
    n.region = 0;
    n.clipped = false;
    // The chosen one went (a card closed, a page changed): the first of what is there is chosen at the next step.
    if (n.focus && !spot_of(n.focus)) n.focus = 0;
}
void nav_end_frame() {
    auto& n = nav();
    n.press = false;
    n.lr = 0;
}
// Something that can be pressed: a click on it, or A / Enter while it is chosen. `lit` says whether to draw it lit (the
// mouse over it, or chosen with the pad).
bool hit(const char* id, ImVec2 a, ImVec2 b, bool* lit = nullptr, bool enabled = true, bool sideways = false) {
    auto& n = nav();
    const ImGuiID gid = ImGui::GetID(id);
    const ImVec2 size(std::max(1.0f, b.x - a.x), std::max(1.0f, b.y - a.y));
    bool clicked = false, over = false;
    const bool visible = !n.clipped || (b.y > n.clip_a.y && a.y < n.clip_b.y && b.x > n.clip_a.x && a.x < n.clip_b.x);
    if (visible) {
        ImGui::SetCursorScreenPos(a);
        clicked = ImGui::InvisibleButton(id, size) && enabled;
        over = ImGui::IsItemHovered();
        const auto& io = ImGui::GetIO();
        if (over && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            n.focus = gid;
            n.keys = false;
        }
    }
    n.drawn.push_back({gid, a, b, n.layer, n.region, sideways});
    if (n.keys && n.focus == gid && n.layer >= n.top) {
        over = true;
        if (n.press && enabled) {
            clicked = true;
            n.press = false;
        }
    }
    if (lit) *lit = over && enabled;
    return clicked;
}
// Whether the pad or the keys have this one chosen (for a field that wants the keyboard when it is pressed).
bool nav_chosen(const char* id) {
    const auto& n = nav();
    return n.keys && n.focus == ImGui::GetID(id);
}
// A scrolled list: the wheel scrolls it while the mouse is over it, the pad's choice is scrolled into it, and what is
// outside its box cannot be pressed.
void begin_region(int id, ImVec2 a, ImVec2 b, float& scroll, float k) {
    auto& n = nav();
    if (n.keys)
        if (const Spot* s = spot_of(n.focus); s && s->region == id) {
            const float pad = 12 * k;
            if (s->a.y < a.y) scroll -= a.y - s->a.y + pad;
            else if (s->b.y > b.y) scroll += s->b.y - b.y + pad;
        }
    const auto& io = ImGui::GetIO();
    if (io.MousePos.x >= a.x && io.MousePos.x <= b.x && io.MousePos.y >= a.y && io.MousePos.y <= b.y && io.MouseWheel != 0 && !io.KeyCtrl)
        scroll -= io.MouseWheel * 90 * k;
    // Held at its end before it is drawn: scrolled past it, the list was drawn too far for a frame and then snapped
    // back, which flickered and cut it off at the bottom.
    scroll = std::max(0.0f, scroll);
    if (const auto end = n.region_end.find(id); end != n.region_end.end()) scroll = std::min(scroll, end->second);
    n.region = id;
    n.clip_a = a;
    n.clip_b = b;
    n.clipped = true;
    ImGui::PushClipRect(a, b, true);
}
// Ends it: `content` is how tall what was in it came to. A thin bar shows where it is when it is longer than its box.
void end_region(ImDrawList* d, ImVec2 a, ImVec2 b, float content, float& scroll, float k) {
    auto& n = nav();
    ImGui::PopClipRect();
    const float room = b.y - a.y;
    n.region_end[n.region] = std::max(0.0f, content - room);
    n.region = 0;
    n.clipped = false;
    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, content - room));
    if (content > room + 1) {
        const float h = std::max(30 * k, room * room / content);
        const float y = a.y + (room - h) * (scroll / std::max(1.0f, content - room));
        d->AddRectFilled(ImVec2(b.x - 4 * k, a.y), ImVec2(b.x - 1 * k, b.y), IM_COL32(255, 255, 255, 22), 2 * k);
        d->AddRectFilled(ImVec2(b.x - 5 * k, y), ImVec2(b.x, y + h), IM_COL32(235, 235, 240, 190), 2 * k);
    }
}

// ---- the pieces the pages are made of -------------------------------------------------------------
std::string elide(ImFont* font, float size, const std::string& value, float width) {
    if (width <= 0) return {};
    if (text_size(font, size, value.c_str()).x <= width) return value;
    std::string cut = value;
    while (!cut.empty() && text_size(font, size, (cut + "...").c_str()).x > width) cut.pop_back();
    return cut + "...";
}
// The one the pad has chosen: a white outline round it, as the game draws its own.
void chosen_outline(ImDrawList* d, ImVec2 a, ImVec2 b, float k) {
    d->AddRect(ImVec2(a.x - 3 * k, a.y - 3 * k), ImVec2(b.x + 3 * k, b.y + 3 * k), white, 0, 0, 2.4f * k);
}
enum class Look { dark, blue, paper, key, danger, ghost };
// A button: a rough tile with its label, lit when the mouse or the pad is on it.
bool action(ImDrawList* d, const Fonts& f, const char* id, ImVec2 a, ImVec2 size, const std::string& label, Look look = Look::dark,
            bool enabled = true, bool on = false) {
    const float k = f.k;
    const ImVec2 b(a.x + size.x, a.y + size.y);
    bool lit = false;
    const bool clicked = hit(id, a, b, &lit, enabled);
    ImU32 fill = IM_COL32(34, 35, 38, 245), ink = white;
    switch (look) {
    case Look::dark: fill = on ? blue : lit ? IM_COL32(62, 64, 70, 250) : IM_COL32(34, 35, 38, 245); break;
    case Look::blue: fill = lit ? IM_COL32(40, 152, 255, 255) : blue; break;
    case Look::paper: fill = lit ? IM_COL32(255, 255, 255, 255) : IM_COL32(232, 232, 230, 250), ink = IM_COL32(18, 18, 20, 255); break;
    case Look::key: fill = lit ? IM_COL32(250, 230, 128, 255) : IM_COL32(236, 214, 96, 255), ink = IM_COL32(18, 18, 20, 255); break;
    case Look::danger: fill = lit ? IM_COL32(220, 70, 70, 255) : IM_COL32(120, 34, 36, 245); break;
    case Look::ghost: fill = lit ? IM_COL32(255, 255, 255, 30) : IM_COL32(255, 255, 255, 0); break;
    }
    if (!enabled) fill = IM_COL32(30, 31, 34, 200), ink = faint;
    if (look == Look::ghost) {
        d->AddRectFilled(a, b, fill, 3 * k);
        d->AddRect(a, b, IM_COL32(255, 255, 255, lit ? 150 : 70), 3 * k, 0, 1.4f * k);
    } else skate_theme::rough_rect(d, a, b, fill, seed_of(a), k * 0.7f);
    if (lit && nav().keys) chosen_outline(d, a, b, k);
    const float fs = std::min(16 * k, size.y * 0.42f);
    const auto shown = elide(f.bold, fs, label, size.x - 12 * k);
    const auto t = text_size(f.bold, fs, shown.c_str());
    text(d, f.bold, fs, ImVec2(a.x + (size.x - t.x) * 0.5f, a.y + (size.y - t.y) * 0.5f), ink, shown);
    return clicked;
}
// A small rounded tag: a pop, PLAYS, IN 2 PACKS. Returns its width.
float tag(ImDrawList* d, const Fonts& f, ImVec2 at, const std::string& label, ImU32 fill, ImU32 ink = white) {
    const float k = f.k, fs = 12 * k;
    const auto t = text_size(f.bold, fs, label.c_str());
    const ImVec2 b(at.x + t.x + 14 * k, at.y + 20 * k);
    d->AddRectFilled(at, b, fill, 10 * k);
    text(d, f.bold, fs, ImVec2(at.x + 7 * k, at.y + (20 * k - t.y) * 0.5f), ink, label);
    return b.x - at.x;
}
float tag_width(const Fonts& f, const std::string& label) { return text_size(f.bold, 12 * f.k, label.c_str()).x + 14 * f.k; }
// A chip that can be pressed: the pops on a trick's card, a pack a trick can go in. Returns whether it was pressed;
// `width` is how wide it came out.
bool chip(ImDrawList* d, const Fonts& f, const char* id, ImVec2 at, const std::string& label, bool on, float* width = nullptr,
          float height = 0, bool enabled = true) {
    const float k = f.k, fs = 13 * k, h = height > 0 ? height : 28 * k;
    const auto t = text_size(f.bold, fs, label.c_str());
    const ImVec2 b(at.x + t.x + 24 * k, at.y + h);
    bool lit = false;
    const bool clicked = hit(id, at, b, &lit, enabled);
    const ImU32 fill = !enabled ? IM_COL32(30, 31, 34, 200) : on ? (lit ? IM_COL32(40, 152, 255, 255) : blue)
                                                             : lit ? IM_COL32(70, 72, 78, 255) : IM_COL32(40, 41, 46, 255);
    d->AddRectFilled(at, b, fill, h * 0.5f);
    if (!on) d->AddRect(at, b, IM_COL32(255, 255, 255, lit ? 120 : 50), h * 0.5f, 0, 1.2f * k);
    if (lit && nav().keys) d->AddRect(ImVec2(at.x - 3 * k, at.y - 3 * k), ImVec2(b.x + 3 * k, b.y + 3 * k), white, h * 0.5f + 3 * k, 0, 2 * k);
    text(d, f.bold, fs, ImVec2(at.x + 12 * k, at.y + (h - t.y) * 0.5f), enabled ? white : faint, label);
    if (width) *width = b.x - at.x;
    return clicked;
}
float chip_width(const Fonts& f, const std::string& label) { return text_size(f.bold, 13 * f.k, label.c_str()).x + 24 * f.k; }
// A row of choices of which one is taken (FIXED / CYCLE, a pack's pop rule). Returns the one pressed, or -1.
int segmented(ImDrawList* d, const Fonts& f, const char* id, ImVec2 at, float width, float height, const std::vector<std::string>& labels,
              int chosen, bool enabled = true) {
    const float k = f.k;
    const float w = width / static_cast<float>(std::max<std::size_t>(1, labels.size()));
    int pressed = -1;
    d->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(24, 25, 28, 250), 4 * k);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const ImVec2 a(at.x + w * static_cast<float>(i), at.y), b(a.x + w, at.y + height);
        ImGui::PushID(static_cast<int>(i));
        bool lit = false;
        if (hit(id, a, b, &lit, enabled)) pressed = static_cast<int>(i);
        ImGui::PopID();
        const bool on = static_cast<int>(i) == chosen;
        if (on) d->AddRectFilled(ImVec2(a.x + 2 * k, a.y + 2 * k), ImVec2(b.x - 2 * k, b.y - 2 * k), blue, 3 * k);
        else if (lit) d->AddRectFilled(ImVec2(a.x + 2 * k, a.y + 2 * k), ImVec2(b.x - 2 * k, b.y - 2 * k), IM_COL32(62, 64, 70, 255), 3 * k);
        if (lit && nav().keys) d->AddRect(ImVec2(a.x - 1 * k, a.y - 1 * k), ImVec2(b.x + 1 * k, b.y + 1 * k), white, 3 * k, 0, 2 * k);
        const float fs = std::min(14 * k, height * 0.45f);
        const auto shown = elide(f.bold, fs, labels[i], w - 8 * k);
        const auto t = text_size(f.bold, fs, shown.c_str());
        text(d, f.bold, fs, ImVec2(a.x + (w - t.x) * 0.5f, a.y + (height - t.y) * 0.5f), on || lit ? white : enabled ? soft : faint, shown);
    }
    return pressed;
}
// An ON / OFF switch. Returns whether it was pressed.
bool toggle(ImDrawList* d, const Fonts& f, const char* id, ImVec2 at, bool on, float scale = 1) {
    const float k = f.k * scale, w = 64 * k, h = 30 * k;
    const ImVec2 b(at.x + w, at.y + h);
    bool lit = false;
    const bool clicked = hit(id, at, b, &lit);
    d->AddRectFilled(at, b, on ? (lit ? IM_COL32(40, 152, 255, 255) : blue) : lit ? IM_COL32(70, 72, 78, 255) : IM_COL32(46, 47, 52, 255), h * 0.5f);
    const float r = h * 0.5f - 4 * k;
    d->AddCircleFilled(ImVec2(on ? b.x - h * 0.5f : at.x + h * 0.5f, at.y + h * 0.5f), r, white, 20);
    const char* word = on ? "ON" : "OFF";
    const auto t = text_size(f.bold, 11 * k, word);
    text(d, f.bold, 11 * k, ImVec2(on ? at.x + 9 * k : b.x - t.x - 9 * k, at.y + (h - t.y) * 0.5f), on ? white : soft, word);
    if (lit && nav().keys) d->AddRect(ImVec2(at.x - 3 * k, at.y - 3 * k), ImVec2(b.x + 3 * k, b.y + 3 * k), white, h * 0.5f + 3 * k, 0, 2 * k);
    return clicked;
}
// A typed field, chosen like anything else; pressing it (A, Enter) puts the keyboard in it. Returns true on Enter.
bool field(const Fonts& f, const char* id, std::array<char, 64>& buffer, ImVec2 at, float width, const char* hint, bool* edited = nullptr) {
    const float k = f.k;
    const ImVec2 b(at.x + width, at.y + 34 * k);
    auto& n = nav();
    const ImGuiID gid = ImGui::GetID(id);
    n.drawn.push_back({gid, at, b, n.layer, n.region, false});
    const bool chosen = n.keys && n.focus == gid && n.layer >= n.top;
    if (chosen && n.press) {
        ImGui::SetKeyboardFocusHere();
        n.press = false;
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(28, 29, 32, 250));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(40, 42, 46, 250));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(46, 48, 54, 255));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, blue_dim);
    ImGui::PushStyleColor(ImGuiCol_Text, white);
    ImGui::PushStyleColor(ImGuiCol_Border, chosen ? white : IM_COL32(255, 255, 255, 40));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, chosen ? 2 * k : 1 * k);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * k, 9 * k));
    ImGui::PushFont(f.bold);
    ImGui::SetNextItemWidth(width);
    const bool done = ImGui::InputTextWithHint(id, hint, buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (edited) *edited = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) {
        n.focus = gid;
        n.keys = false;
    }
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);
    return done;
}
// A heading over a group: small capitals with a line along under them.
void heading(ImDrawList* d, const Fonts& f, ImVec2 at, float width, const std::string& label, const std::string& value = {}) {
    const float k = f.k;
    text(d, f.heading, 15 * k, at, white, upper(label));
    if (!value.empty()) {
        const auto t = text_size(f.bold, 13 * k, value.c_str());
        text(d, f.bold, 13 * k, ImVec2(at.x + width - t.x, at.y + 2 * k), soft, value);
    }
    d->AddRectFilled(ImVec2(at.x, at.y + 22 * k), ImVec2(at.x + width, at.y + 23.5f * k), IM_COL32(255, 255, 255, 34));
}
// Wrapped text; returns how tall it came out.
float paragraph(ImDrawList* d, ImFont* font, float size, ImVec2 at, float width, ImU32 colour, const std::string& value) {
    d->AddText(font, size, at, colour, value.c_str(), nullptr, width);
    return font->CalcTextSizeA(size, FLT_MAX, width, value.c_str()).y;
}

// ---- the menu's lists: the game's animation tree (TRICKS > GAME ANIMATIONS) --------------------------
struct Row {
    std::string label, value, about;
    std::function<void()> act;
    bool enabled{true}, on{};
    bool info{}; // information: it can be chosen to read its help, and does nothing
};
Row info_row(std::string label, std::string value, std::string about) {
    Row row{std::move(label), std::move(value), std::move(about), {}, false};
    row.info = true;
    return row;
}
bool selectable(const Row& row) noexcept { return static_cast<bool>(row.act) || row.info; }
// Back, one step, in the game's animation tree. True when it went back.
bool go_back_in_assets() {
    auto& u = ui();
    if (!u.asset_document.empty()) {
        if (!u.asset_json_path.empty()) {
            const auto slash = u.asset_json_path.rfind('/');
            u.asset_json_path = slash == std::string::npos ? "" : u.asset_json_path.substr(0, slash);
        } else if (!u.asset_history.empty()) {
            u.asset_document = u.asset_history.back().first;
            u.asset_json_path = u.asset_history.back().second;
            u.asset_history.pop_back();
        } else u.asset_document.clear();
        u.asset_page = 0;
        return true;
    }
    if (!u.asset_category.empty()) {
        const auto slash = u.asset_category.rfind('/');
        u.asset_category = slash == std::string::npos ? "" : u.asset_category.substr(0, slash);
        u.asset_page = 0;
        return true;
    }
    return false;
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
    // Set from his body where it is when the shot is taken, rather than by `yaw`: 1 facing his chest, 2 his left side
    // (the board's whole length), 3 behind him, 4 from above.
    int body{};
};
constexpr std::array<Shot, 8> shots{{
    {"FREE", 0, 0, 0, studio::all_joints}, // stays where it is left; the rest travel with him
    {"FRONT", 0, 0.06f, 2.9f, studio::all_joints, 1},
    {"SIDE", 0, 0.06f, 2.9f, studio::all_joints, 2},
    {"BACK", 0, 0.06f, 2.9f, studio::all_joints, 3},
    {"TOP", 0, 1.38f, 3.3f, studio::all_joints, 4},
    {"BOARD", 0.90f, 0.13f, 1.25f, studio::board_joint},
    {"CINEMATIC", 1.15f, -0.04f, 4.00f, studio::all_joints},
    {"FILMER", 2.50f, 0.03f, 1.30f, studio::board_joint},
}};
constexpr int filmer_shot = 7;
// The way his chest faces, flat on the ground: across his shoulders (left upper arm from the right) turned a quarter
// about up. Nothing when his arms are not in the view.
std::optional<studio::Vec3> chest_facing(const studio::View& v) {
    const auto* left = joint_view(v, 276);
    const auto* right = joint_view(v, 47);
    if (!left || !right) return std::nullopt;
    const studio::Vec3 across{left->position[0] - right->position[0], 0, left->position[2] - right->position[2]};
    studio::Vec3 facing{-across[2], 0, across[0]};
    const float length = std::hypot(facing[0], facing[2]);
    if (length < 1e-4f) return std::nullopt;
    facing[0] /= length;
    facing[2] /= length;
    return facing;
}
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
    // A view of his body: the camera put on that side of him as he stands now, and kept there.
    if (shot.body)
        if (const auto facing = chest_facing(v)) {
            studio::Vec3 from = *facing;
            if (shot.body == 2) from = {facing->at(2), 0, -facing->at(0)}; // his left
            if (shot.body == 3) from = {-facing->at(0), 0, -facing->at(2)};
            orbit.yaw = std::atan2(from[0], from[2]) - v.base_yaw;
        }
    studio::set_orbit(orbit);
    studio::set_focus(shot.on);
    say(std::string(shot.name) + (u.camera_shot == filmer_shot && u.fisheye ? " camera, through the fisheye." : " camera."));
}
void next_shot(const studio::View& v, int by) {
    const int count = static_cast<int>(shots.size());
    take_shot(v, (ui().camera_shot + by % count + count) % count);
}
// The camera's shot in the editor's top bar: the name between two arrows, clicked or stepped with V.
void shot_switch(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 centre) {
    auto& u = ui();
    const float k = f.k;
    const auto& shot = shots[static_cast<std::size_t>(std::clamp(u.camera_shot, 0, static_cast<int>(shots.size()) - 1))];
    const float w = 250 * k, h = 40 * k;
    const ImVec2 a(centre.x - w * 0.5f, centre.y - h * 0.5f), b(a.x + w, a.y + h);
    skate_theme::rough_rect(d, a, b, IM_COL32(12, 12, 14, 200), 41, k * 0.7f);
    text(d, f.bold, 10 * k, ImVec2(a.x + 40 * k, a.y + 3 * k), IM_COL32(255, 255, 255, 90), "CAMERA (V)");
    const float fs = 16 * k;
    const auto t = text_size(f.heading, fs, shot.name);
    text(d, f.heading, fs, ImVec2(centre.x - t.x * 0.5f, a.y + 15 * k), u.camera_shot ? blue_light : white, shot.name);
    for (const int by : {-1, 1}) {
        const ImVec2 aa(by < 0 ? a.x : b.x - 36 * k, a.y), bb(aa.x + 36 * k, b.y);
        ImGui::PushID(by);
        bool lit = false;
        if (hit("##shot", aa, bb, &lit)) next_shot(v, by);
        ImGui::PopID();
        const char* glyph_text = by < 0 ? "<" : ">";
        const auto g = text_size(f.heading, 20 * k, glyph_text);
        text(d, f.heading, 20 * k, ImVec2((aa.x + bb.x - g.x) * 0.5f, (aa.y + bb.y - g.y) * 0.5f), lit ? white : IM_COL32(255, 255, 255, 120), glyph_text);
    }
}

// The rows of the game's animation tree: its folders, the clips in them, and a clip's own fields.
std::vector<Row> asset_rows(const studio::View& v) {
    (void)v;
    auto& u = ui();
    std::vector<Row> rows;
    const auto tricks = studio::game_tricks();
    const auto& list = *tricks;
    const auto assets = studio::native_assets();
    if (!u.asset_document.empty()) {
        const auto document = studio::native_asset_fields(u.asset_document, u.asset_json_path, u.asset_page);
        rows.push_back({"< Back", "", "Return to the parent field, referenced document, or native tree.", [] { go_back_in_assets(); }});
        rows.push_back({"NATIVE DATA", document.title, "Read-only native authoring data. References are static dependencies, not proof of runtime execution order.", {}, false});
        if (!u.asset_json_path.empty()) rows.push_back({"Field path", u.asset_json_path, "Objects and arrays can be expanded; values are not modified.", {}, false});
        if (!document.error.empty()) rows.push_back({"Data unavailable", document.error, document.error, {}, false});
        if (document.pages > 1)
            rows.push_back({"Next page", std::to_string(u.asset_page + 1) + "/" + std::to_string(document.pages), "Next 50 fields or array values.",
                            [pages = document.pages] { ui().asset_page = (ui().asset_page + 1) % pages; }});
        for (const auto& field_row : document.fields) {
            rows.push_back({field_row.label, field_row.value, field_row.expandable ? "Expand this native object or array." : field_row.value,
                            [path = field_row.path] { ui().asset_json_path = path; ui().asset_page = 0; }, field_row.expandable});
            if (!field_row.reference.empty())
                rows.push_back({"Open reference: " + field_row.label, ">", "Follow the referenced native partition; missing documents remain explicit.", [ref = field_row.reference] {
                                    auto& s = ui();
                                    s.asset_history.emplace_back(s.asset_document, s.asset_json_path);
                                    s.asset_document = ref;
                                    s.asset_json_path.clear();
                                    s.asset_page = 0;
                                }});
        }
        return rows;
    }
    if (!u.asset_category.empty() || !u.asset_filter.empty())
        rows.push_back({"< Back", "", "Browse the parent folder, or clear the search.", [] {
                            auto& state = ui();
                            if (!state.asset_filter.empty()) {
                                state.asset_filter.clear();
                                state.asset_query.fill(0);
                            } else go_back_in_assets();
                            state.asset_page = 0;
                        }});
    rows.push_back({!u.asset_filter.empty() ? "SEARCH RESULTS / ALL FOLDERS" : u.asset_category.empty() ? "ANIMATION FOLDERS" : upper(u.asset_category), "",
                    "Folders and readable names are inferred from native names; the original identity remains in the description.", {}, false});
    rows.push_back({"Show", u.asset_ready_only ? "Editable clips" : "All assets", "Filter the whole tree to clips with an available preview.",
                    [] { ui().asset_ready_only = !ui().asset_ready_only; ui().asset_page = 0; }});
    rows.push_back({"Selection action", u.asset_inspect ? "Inspect native data" : "Open preview",
                    "Inspect data opens the full typed field tree and references, including for editable clips.", [] { ui().asset_inspect = !ui().asset_inspect; }});
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
            ++counts.first;
            counts.second += ready;
        }
    }
    std::sort(leaves.begin(), leaves.end(), [](const auto* a, const auto* b) { return a->label == b->label ? a->asset < b->asset : a->label < b->label; });
    if (!u.asset_filter.empty())
        rows.push_back({std::to_string(leaves.size()) + " results", "",
                        "Search includes readable names, aliases, original names and types across every folder. All search words must match.", {}, false});
    if (assets->empty())
        rows.push_back(info_row("No animation index on this install", "",
                                "The native animation tree is built from a dump of the game's own asset names (contrib/animator-studio). "
                                "Without it there is nothing here to browse or search; the tricks you can edit are in the other kinds."));
    else if (leaves.empty() && children.empty())
        rows.push_back(info_row("No animations found", "", "Try fewer words, another trick name, or Show: All assets."));
    for (const auto& [child, counts] : children)
        rows.push_back({child, std::to_string(counts.second) + " editable / " + std::to_string(counts.first) + " >",
                        "Browse this branch of the native animation inventory.", [path = prefix + child] {
                            ui().asset_category = path;
                            ui().asset_page = 0;
                        }});
    constexpr std::size_t page_size = 50;
    const auto pages = (leaves.size() + page_size - 1) / page_size;
    if (pages) u.asset_page = std::min(u.asset_page, pages - 1);
    if (pages > 1) {
        rows.push_back({"Next page", std::to_string(u.asset_page + 1) + "/" + std::to_string(pages), "Show the next 50 assets (wraps to the first page).",
                        [pages] { ui().asset_page = (ui().asset_page + 1) % pages; }});
        rows.push_back({"Previous page", "", "Show the previous 50 assets.", [pages] { ui().asset_page = (ui().asset_page + pages - 1) % pages; }});
    }
    for (std::size_t i = u.asset_page * page_size; i < std::min(leaves.size(), (u.asset_page + 1) * page_size); ++i) {
        const auto& asset = *leaves[i];
        const auto ready = available.find(asset.asset);
        const auto description = asset.context + ". Folder: " + asset.category + ". Source: " + asset.asset + ". " + asset.detail;
        if (ready != available.end() && !u.asset_inspect)
            rows.push_back({asset.label, "Editable", description, [t = *ready->second] {
                                std::string m;
                                studio::load_game_trick(t, m);
                                say(m);
                                ui().names_for = 0;
                            }});
        else
            rows.push_back({asset.label,
                            ready != available.end() ? "Editable / data" : asset.decode_state == "metadata" ? "Metadata"
                            : asset.decode_state == "unsupported" ? "Unsupported preview" : asset.decode_state == "error" ? "Decode failed"
                            : asset.decode_state == "pending" ? "Queued" : "Indexed",
                            description, [name = asset.asset] {
                                auto& s = ui();
                                s.asset_return_page = s.asset_page;
                                s.asset_document = name;
                                s.asset_json_path.clear();
                                s.asset_history.clear();
                                s.asset_page = 0;
                            }});
    }
    rows.push_back({"Reload the trick library", std::to_string(list.size()) + " clips",
                    "Reads the trick library from disk again. To make it again from the game, use SETTINGS > TRICKS > Trick library.", [] {
                        std::string m;
                        studio::refresh_game_tricks(m);
                        say(m);
                    }});
    return rows;
}

void zoom_timeline(float factor);
// ---- what is done with the chosen keys (the round menu, the selection bar, the keys) -----------------
void copy_keys(const studio::View& v) {
    auto& u = ui();
    u.copied_keys.clear();
    for (const auto& [joint, time] : u.picked)
        if (const auto* t = track_view(v, joint))
            for (const auto& key : t->keys)
                if (std::abs(key.time - time) < 1e-4f) u.copied_keys.push_back({joint, key.time, key.rotation, key.offset, key.ease});
    u.copied_are_keys = !u.copied_keys.empty();
    say(u.copied_keys.empty() ? "No keys chosen to copy." : std::to_string(u.copied_keys.size()) + " keys copied: PASTE (Ctrl + V) puts them at the playhead.");
}
// The copied keys again, the first of them on the playhead and the rest as far after it as they were. They are chosen.
void paste_keys(const studio::View& v) {
    auto& u = ui();
    if (u.copied_keys.empty() || !v.has_clip) return;
    float first = u.copied_keys.front().time;
    for (const auto& c : u.copied_keys) first = std::min(first, c.time);
    const float at = std::round(v.time * fps) / fps;
    studio::begin_change();
    u.picked.clear();
    for (const auto& c : u.copied_keys) {
        const float t = std::clamp(std::round((at + c.time - first) * fps) / fps, 0.0f, v.duration);
        studio::set_time(t);
        studio::set_offset(c.joint, {c.rotation, c.offset});
        studio::set_ease(c.joint, t, c.ease);
        u.picked.emplace_back(c.joint, t);
    }
    studio::set_time(at);
    say(std::to_string(u.copied_keys.size()) + " keys pasted at frame " + std::to_string(static_cast<int>(std::round(at * fps))) + ".");
}
void ease_picked(studio::Ease ease) {
    auto& u = ui();
    if (u.picked.empty()) return;
    studio::begin_change();
    for (const auto& [joint, time] : u.picked) studio::set_ease(joint, time, ease);
    say(std::to_string(u.picked.size()) + (ease == studio::Ease::linear ? " keys go straight to the next." : " keys hold until the next."));
}
// The chosen keys back to the game's own pose (each is kept, with nothing added).
void reset_picked(const studio::View& v) {
    auto& u = ui();
    if (u.picked.empty()) return;
    const float at = v.time;
    studio::begin_change();
    for (const auto& [joint, time] : u.picked) {
        studio::set_time(time);
        studio::set_offset(joint, studio::Offset{});
    }
    studio::set_time(at);
    say(std::to_string(u.picked.size()) + " keys back to the game's own pose.");
}
// The next or previous joint, in the timeline's order (the D-pad, the arrow keys).
void step_joint(int by) {
    const auto joints = studio::editable_joints();
    if (joints.empty()) return;
    auto& u = ui();
    std::size_t i = 0;
    for (std::size_t j = 0; j < joints.size(); ++j)
        if (joints[j].index == u.selected) i = j;
    const auto n = static_cast<int>(joints.size());
    u.selected = joints[static_cast<std::size_t>((static_cast<int>(i) + by + n) % n)].index;
}

// ---- the screen's state: the menu, the cards over it ----------------------------------------------
bool any_card_open(const studio::View& v) {
    const auto& u = ui();
    return u.browser.open || u.save_card || !v.test_returned.empty() || u.conflicts_open || u.pack_adding || !u.stance_pack.empty() || u.sign_in ||
           !u.share_pack.empty() || !u.like_pack.empty() || !u.cover_pack.empty();
}
void open_menu(int tab = -1) {
    auto& u = ui();
    u.menu = true;
    u.radial = false;
    if (tab >= 0) u.menu_tab = tab;
}
void switch_menu_tab(int by) {
    auto& u = ui();
    u.menu_tab = (u.menu_tab + by + 4) % 4;
}
// Every duplicate trick not decided yet is left to take turns at random: the card asked once, and was answered.
void conflicts_done(const studio::View& v) {
    for (const auto& c : v.conflicts)
        if (c.choice.empty()) {
            std::string m;
            studio::set_conflict_choice(c.slot, "*", m);
        }
    ui().conflicts_open = false;
}
// Closes whatever card is on top. True when there was one.
bool close_card(const studio::View& v) {
    auto& u = ui();
    if (u.browser.open) u.browser.open = false;
    else if (u.pack_adding) u.pack_adding = false;
    else if (u.conflicts_open) conflicts_done(v);
    else if (!u.stance_pack.empty()) u.stance_pack.clear();
    else if (u.sign_in) u.sign_in = false;
    else if (!u.like_pack.empty()) u.like_pack.clear();
    else if (!u.cover_pack.empty()) {
        u.cover_pack.clear();
        release_cover_art();
    } else if (!u.share_pack.empty()) u.share_pack.clear();
    else if (u.save_card) u.save_card = false;
    else if (!v.test_returned.empty()) studio::clear_test_returned();
    else return false;
    return true;
}
// B, Esc: a step back. Cards first, then out of what is open on the page, then back to the editor.
void menu_back(const studio::View& v) {
    auto& u = ui();
    if (close_card(v)) return;
    if (u.menu_tab == 0) {
        if (!u.trick_page.empty()) {
            u.trick_page.clear();
            return;
        }
        if (!u.piece_card.empty()) {
            u.piece_card.clear();
            return;
        }
        if (u.trick_group == "*assets" && go_back_in_assets()) return;
        if (u.asset_query[0]) {
            u.asset_query.fill(0);
            u.asset_filter.clear();
            return;
        }
    }
    if (u.menu_tab == 3 && u.settings_page >= 0) {
        u.settings_page = -1;
        return;
    }
    if (u.menu_tab == 2) {
        if (!u.market_open.empty()) {
            u.market_open.clear();
            return;
        }
        if (!u.market_crew.empty()) {
            u.market_crew.clear();
            u.market_scroll = 0;
            return;
        }
        if (u.market_query[0]) {
            u.market_query.fill(0);
            return;
        }
        if (u.market_list != "featured") {
            u.market_list = "featured";
            u.market_scroll = 0;
            return;
        }
    }
    if (u.menu_tab == 1 && u.pack_page) {
        u.pack_page = false;
        return;
    }
    if (v.has_clip) u.menu = false;
}
// Saving. The first time a trick is saved a card comes up to name it, test it or put it in a pack.
void save_now(const studio::View& v) {
    if (!v.has_clip) return;
    auto& u = ui();
    const bool first = v.name.empty() || std::find(v.library.begin(), v.library.end(), v.name) == v.library.end();
    std::string m;
    const bool saved = studio::save(m);
    say(m);
    if (saved && first) {
        u.save_card = true;
        u.label_for.clear();
    }
}
// Test in game: saved first when it has changes, then the studio closes and that trick plays until it is opened again.
void test_now(const studio::View& v) {
    if (!v.has_clip) return;
    std::string m;
    const bool known = !v.name.empty() && std::find(v.library.begin(), v.library.end(), v.name) != v.library.end();
    if ((v.dirty || !known) && !studio::save(m)) {
        say(m);
        return;
    }
    const auto now = studio::view();
    auto& u = ui();
    u.save_card = false;
    studio::clear_test_returned();
    studio::test_in_game(now.name, m);
    say(m);
}

// ---- the controller --------------------------------------------------------------------------------
bool repeats(unsigned bit, unsigned buttons, unsigned pressed, double now) {
    auto& u = ui();
    const int i = std::countr_zero(bit);
    if (pressed & bit) {
        u.repeat_at[i] = now + 0.38;
        return true;
    }
    if ((buttons & bit) && now >= u.repeat_at[i]) {
        u.repeat_at[i] = now + 0.09;
        return true;
    }
    return false;
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
    const bool card = any_card_open(v);
    // The menu and the cards: the D-pad chooses, A presses, B goes back, LB and RB change tab, View goes to the editor.
    if (u.menu || card) {
        if (repeats(pad_up, buttons, pressed, now)) nav_step(0, -1);
        if (repeats(pad_down, buttons, pressed, now)) nav_step(0, 1);
        if (repeats(pad_left, buttons, pressed, now)) nav_step(-1, 0);
        if (repeats(pad_right, buttons, pressed, now)) nav_step(1, 0);
        if (pressed & pad_a) {
            nav().press = true;
            nav().keys = true;
        }
        if (pressed & pad_b) menu_back(v);
        if (!card && u.menu && (pressed & pad_lb)) switch_menu_tab(-1);
        if (!card && u.menu && (pressed & pad_rb)) switch_menu_tab(1);
        if (!card && (pressed & (pad_view | pad_menu)) && v.has_clip) u.menu = !u.menu;
        u.filtered[0] = u.filtered[1] = u.filtered[2] = u.filtered[3] = 0;
        u.stick_turning = false;
        u.shoulder_shifted = true; // a shoulder let go of here is not a tap for the editor
        return;
    }
    if (u.radial) return; // the round menu is the mouse's
    // LB and RB zoom the timeline on a tap. Held with a stick they carry a bracket instead (below).
    if (pressed & (pad_lb | pad_rb)) u.shoulder_shifted = false;
    if ((released & pad_lb) && !u.shoulder_shifted) zoom_timeline(1 / 1.5f);
    if ((released & pad_rb) && !u.shoulder_shifted) zoom_timeline(1.5f);
    if (pressed & pad_a) key_joint();
    if (pressed & pad_x) {
        if (!u.picked.empty()) delete_picked();
        else delete_key(v);
    }
    if (pressed & pad_y) studio::set_playing(!v.playing);
    if (pressed & pad_b) {
        if (!u.picked.empty()) u.picked.clear();
        else studio::undo();
    }
    if (pressed & pad_view) open_menu();
    if (pressed & pad_menu) save_now(v);
    // L3: turn about any axis, X, Y, Z, then move (the hips and the board).
    if ((released & pad_l3) && was_held(pad_l3) < 0.5) {
        if (u.move_mode) u.move_mode = false, u.axis = -1;
        else if (u.axis >= 2) u.move_mode = true, u.axis = -1;
        else ++u.axis;
    }
    // R3: the right stick orbits, or zooms (up / down) and pans (left / right).
    if ((released & pad_r3) && was_held(pad_r3) < 0.5) u.stick_zoom = !u.stick_zoom;
    if (repeats(pad_up, buttons, pressed, now)) step_joint(-1);
    if (repeats(pad_down, buttons, pressed, now)) step_joint(1);
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
    // the timeline does not zoom when it has been used.
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
    // Left stick turns the chosen joint; a turn is one undo step.
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

    // Right stick orbits; after R3 it zooms (up / down) and pans (left / right).
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
        if (u.camera_shot) {
            u.camera_shot = 0;
            studio::set_camera_follow(false);
        }
    }
}

// ---- the keyboard ---------------------------------------------------------------------------------
// In the menu and its cards: the arrows choose, Enter presses, Esc goes back, Q and E change tab, Tab goes to the editor.
void menu_keyboard(const studio::View& v) {
    auto& u = ui();
    auto& io = ImGui::GetIO();
    const bool moved = std::hypot(io.MousePos.x - u.last_mouse.x, io.MousePos.y - u.last_mouse.y) > 4;
    u.last_mouse = io.MousePos;
    if (moved || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) u.pad_mode = false;
    if (io.WantTextInput) return;
    const bool card = any_card_open(v);
    const auto pressed = [&](ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); };
    if (pressed(ImGuiKey_UpArrow, true)) nav_step(0, -1);
    if (pressed(ImGuiKey_DownArrow, true)) nav_step(0, 1);
    if (pressed(ImGuiKey_LeftArrow, true)) nav_step(-1, 0);
    if (pressed(ImGuiKey_RightArrow, true)) nav_step(1, 0);
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
        nav().press = true;
        nav().keys = true;
    }
    if (pressed(ImGuiKey_Escape)) menu_back(v);
    if (!card && u.menu && pressed(ImGuiKey_Q)) switch_menu_tab(-1);
    if (!card && u.menu && pressed(ImGuiKey_E)) switch_menu_tab(1);
    if (!card && pressed(ImGuiKey_Tab) && v.has_clip) u.menu = !u.menu;
    if (!card && io.KeyCtrl && pressed(ImGuiKey_S)) save_now(v);
}
// In the editor.
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
    if (pressed(ImGuiKey_Escape)) {
        // Esc backs out: the round menu, then the chosen keys, then to the menu.
        if (u.radial) u.radial = false;
        else if (!u.picked.empty()) u.picked.clear();
        else if (u.feet_lock) unlock_feet();
        else if (!u.also.empty()) u.also.clear();
        else open_menu();
        return;
    }
    if (u.radial) return;
    if (pressed(ImGuiKey_Tab)) {
        open_menu();
        return;
    }
    if (pressed(ImGuiKey_Space)) studio::set_playing(!v.playing);
    if (pressed(ImGuiKey_LeftArrow, true)) step_frame(v, -1);
    if (pressed(ImGuiKey_RightArrow, true)) step_frame(v, 1);
    if (pressed(ImGuiKey_UpArrow, true)) step_joint(-1);
    if (pressed(ImGuiKey_DownArrow, true)) step_joint(1);
    if ((pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) && v.has_clip) key_joint();
    if (pressed(ImGuiKey_B) && v.has_clip) {
        if (u.feet_lock) unlock_feet();
        else lock_feet_to_deck(v);
    }
    if (pressed(ImGuiKey_Home)) studio::set_time(0);
    if (pressed(ImGuiKey_End)) studio::set_time(v.duration);
    if (ctrl && pressed(ImGuiKey_Z, true)) studio::undo();
    if (ctrl && pressed(ImGuiKey_Y, true)) studio::redo();
    if (ctrl && pressed(ImGuiKey_C)) {
        if (!u.picked.empty()) copy_keys(v);
        else {
            studio::copy_pose();
            u.copied_are_keys = false;
            say("Pose copied: Ctrl + V keys it on another frame.");
        }
    }
    if (ctrl && pressed(ImGuiKey_V)) {
        if (u.copied_are_keys && !u.copied_keys.empty()) paste_keys(v);
        else {
            studio::begin_change();
            studio::paste_pose();
        }
    }
    if (ctrl && pressed(ImGuiKey_S)) save_now(v);
    if (ctrl && pressed(ImGuiKey_A)) pick_all(v);
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
        if (!u.picked.empty()) delete_picked();
        else delete_key(v);
    }
    if (!ctrl && pressed(ImGuiKey_X)) u.axis = u.axis == 0 ? -1 : 0;
    if (!ctrl && pressed(ImGuiKey_Y)) u.axis = u.axis == 1 ? -1 : 1;
    if (!ctrl && pressed(ImGuiKey_Z)) u.axis = u.axis == 2 ? -1 : 2;
    if (!ctrl && pressed(ImGuiKey_R)) u.move_mode = false;
    if (!ctrl && pressed(ImGuiKey_G)) u.move_mode = true;
    if (!ctrl && pressed(ImGuiKey_L)) u.local_axes = !u.local_axes;
    if (!ctrl && pressed(ImGuiKey_O)) studio::set_solo(!v.solo);
    if (!ctrl && pressed(ImGuiKey_M)) {
        studio::set_joint_limits(!v.limits);
        say(v.limits ? "Limits off: joints turn freely." : "Limits on: joints stay in the game's own range.");
    }
    if (!ctrl && pressed(ImGuiKey_N)) {
        u.snap = !u.snap;
        say(u.snap ? "Snapping on: a foot you move lands on the deck." : "Snapping off.");
    }
    if (pressed(ImGuiKey_F1)) u.controls_open = !u.controls_open;
    auto orbit = v.orbit;
    // WASD turns the view while held, as Blender's view keys do: A and D round him, W and S over and under; Shift
    // faster. (Ctrl+S and Ctrl+A are save and choose every key, as before.)
    if (!ctrl) {
        const float dt = std::min(io.DeltaTime, 0.1f), speed = io.KeyShift ? 2.4f : 1.2f;
        float round = 0, over = 0;
        if (ImGui::IsKeyDown(ImGuiKey_A)) round -= 1;
        if (ImGui::IsKeyDown(ImGuiKey_D)) round += 1;
        if (ImGui::IsKeyDown(ImGuiKey_W)) over += 1;
        if (ImGui::IsKeyDown(ImGuiKey_S)) over -= 1;
        if (round != 0 || over != 0) {
            any = true;
            orbit.yaw += round * speed * 1.6f * dt;
            orbit.pitch += over * speed * dt;
            studio::set_orbit(orbit);
            if (u.camera_shot) {
                u.camera_shot = 0;
                studio::set_camera_follow(false);
            }
        }
    }
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

// ---- the menu -------------------------------------------------------------------------------------
// skate.'s own full-screen menus, in the studio's colours: the tabs in the middle at the top (Q / E, LB / RB), the
// page under them, and what the buttons do along the bottom. TRICKS is where a trick is picked to edit, PACKS where
// your packs are kept, SETTINGS how the studio works.
constexpr std::array<const char*, 4> menu_tabs{"TRICKS", "PACKS", "MARKET", "SETTINGS"};
const studio::Preset* preset_named(const studio::View& v, const std::string& name) {
    const auto found = std::find_if(v.presets.begin(), v.presets.end(), [&](const auto& p) { return p.name == name; });
    return found == v.presets.end() ? nullptr : &*found;
}
const studio::View::PackView* pack_named(const studio::View& v, const std::string& name) {
    const auto found = std::find_if(v.packs.begin(), v.packs.end(), [&](const auto& p) { return p.name == name; });
    return found == v.packs.end() ? nullptr : &*found;
}
bool saved_trick(const studio::View& v, const std::string& name) { return std::find(v.library.begin(), v.library.end(), name) != v.library.end(); }
// What a saved trick is shown by: its name as you gave it, or its file's.
std::string shown_name(const studio::View& v, const std::string& name) {
    const auto* p = preset_named(v, name);
    return p && !p->label.empty() ? p->label : name;
}
std::string pop_words(const std::string& pop) { return pop.empty() ? "ANY POP" : upper(pop) + " POP"; }
// A saved trick's tags: the pop it plays on, the stance it was made in and how it was done - what to do in game to see
// it play. Drawn leftwards from `right`, which is left where they stop.
std::vector<std::pair<std::string, ImU32>> tags_of(const studio::Preset& p, const std::string& pop_rule = {}) {
    std::vector<std::pair<std::string, ImU32>> tags;
    const std::string pop = pop_rule.empty() ? p.pop : pop_rule == "any" ? std::string{} : pop_rule;
    tags.emplace_back(pop.empty() ? "ANY POP" : upper(pop) + " POP", blue);
    // Its stance as you ride: nothing for your own, else fakie, switch or switch fakie.
    static constexpr std::array<const char*, 4> stances{"", "FAKIE", "SWITCH", "SWITCH FAKIE"};
    if (const auto version = studio::stance_version(p.stance, p.switch_stance, p.fakie, ui().own_lead_right); version > 0)
        tags.emplace_back(stances[static_cast<std::size_t>(version)], IM_COL32(196, 104, 0, 255));
    if (p.entry == "rail") tags.emplace_back("OFF A RAIL", IM_COL32(36, 150, 76, 255));
    return tags;
}
void trick_tags(ImDrawList* d, const Fonts& f, const studio::Preset& p, const std::string& pop_rule, float& right, float y) {
    const auto tags = tags_of(p, pop_rule);
    for (auto it = tags.rbegin(); it != tags.rend(); ++it) {
        right -= tag_width(f, it->first);
        tag(d, f, ImVec2(right, y), it->first, it->second);
        right -= 6 * f.k;
    }
}
// A piece of the game's animation ("native:<asset>") by the trick card it is on: "Boardslide (FS LOOP)".
std::string piece_label(const std::string& trick) {
    if (!trick.starts_with("native:")) return {};
    const auto parts = studio::native_trick_parts(std::string_view(trick).substr(7));
    if (parts.empty()) return {};
    const auto& p = parts.front();
    return p.trick + " (" + (p.side.empty() ? p.part : p.side + " " + p.part) + ")";
}
// The game's name for a trick (Kickflip for kickflip), from its catalog.
std::string trick_label(const std::string& trick) {
    if (auto piece = piece_label(trick); !piece.empty()) return piece;
    const auto tricks = studio::game_tricks();
    for (const auto& t : *tricks)
        if (t.trick == trick && !t.label.empty()) return t.label;
    std::string out = trick;
    for (auto& c : out) if (c == '_') c = ' ';
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}
// The kind of trick it is, for sorting a pack: the catalog's own kind, or a good guess from its name.
std::string kind_of(const std::string& trick) {
    if (trick.starts_with("native:"))
        if (const auto parts = studio::native_trick_parts(std::string_view(trick).substr(7)); !parts.empty()) return parts.front().kind;
    const auto tricks = studio::game_tricks();
    for (const auto& t : *tricks)
        if (t.trick == trick && !t.category.empty() && !t.preview_only) return t.category;
    const auto key = studio::native_search_key(trick);
    const auto has = [&](const char* word) { return key.find(word) != std::string::npos; };
    if (has("grind") || has("50") || has("crook") || has("smith") || has("feeble") || has("salad") || has("suski") || has("willy")) return "Grinds";
    if (has("slide") || has("blunt")) return "Slides";
    if (has("manual")) return "Manuals";
    if (has("grab") || has("indy") || has("melon") || has("mute") || has("stalefish") || has("air")) return "Grabs";
    if (has("shuv") || has("shove")) return "Shove-its";
    if (has("flip") || has("heel") || has("varial") || has("tre")) return "Flip tricks";
    if (has("ollie") || has("nollie")) return "Ollies";
    return "Other";
}
// A name for a new pack no pack has yet.
std::string unique_pack_name(const studio::View& v) {
    for (int n = 1;; ++n) {
        const auto name = "Pack " + std::to_string(n);
        if (std::none_of(v.packs.begin(), v.packs.end(), [&](const auto& p) { return _stricmp(p.name.c_str(), name.c_str()) == 0; })) return name;
    }
}

void menu_backdrop(ImDrawList* d, const Fonts& f, ImVec2 display, float alpha) {
    const float k = f.k;
    const auto a8 = [&](int value) { return static_cast<int>(static_cast<float>(value) * alpha); };
    // Dark enough that nothing of the room shows through; clear only where a trick is being looked at.
    const ImU32 fill = IM_COL32(10, 10, 12, a8(252));
    const auto& u = ui();
    if (u.peek_hole) {
        d->AddRectFilled(ImVec2(0, 0), ImVec2(display.x, u.peek_a.y), fill);
        d->AddRectFilled(ImVec2(0, u.peek_b.y), display, fill);
        d->AddRectFilled(ImVec2(0, u.peek_a.y), ImVec2(u.peek_a.x, u.peek_b.y), fill);
        d->AddRectFilled(ImVec2(u.peek_b.x, u.peek_a.y), ImVec2(display.x, u.peek_b.y), fill);
    } else d->AddRectFilled(ImVec2(0, 0), display, fill);
    draw_studio_picture(d, StudioPicture::grain, ImVec2(0, 0), display, IM_COL32(255, 255, 255, a8(22)));
    // A cold light from the top, as the game's own pages have, and a little print in the corners.
    const ImU32 lit = IM_COL32(1, 131, 255, a8(26)), none = IM_COL32(1, 131, 255, 0);
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, display.y * 0.35f), lit, lit, none, none);
    theme::halftone(d, ImVec2(display.x - 150 * k, display.y - 120 * k), 12, 6, IM_COL32(255, 255, 255, a8(14)));
    theme::halftone(d, ImVec2(30 * k, 110 * k), 8, 10, IM_COL32(255, 255, 255, a8(10)));
}
// The tabs, in the middle at the top whatever the screen's width, with Q and E (LB and RB) either side.
void menu_tabs_row(ImDrawList* d, const Fonts& f, float centre_x, float y) {
    auto& u = ui();
    const float k = f.k, fs = 30 * k, side = 30 * k, gap = 14 * k, h = 54 * k;
    float names = 0;
    for (const char* name : menu_tabs) names += text_size(f.heading, fs, name).x + 2 * side;
    names += gap * static_cast<float>(menu_tabs.size() - 1);
    const float cap_w = 44 * k;
    float x = centre_x - names * 0.5f;
    const auto cap = [&](float at, const char* key, Glyph g) {
        const ImVec2 c(at, y + h * 0.5f - 11 * k);
        if (u.pad_mode) glyph(d, f, c, g, u.playstation);
        else {
            d->AddRect(ImVec2(c.x, c.y), ImVec2(c.x + 24 * k, c.y + 24 * k), IM_COL32(255, 255, 255, 170), 3 * k, 0, 2 * k);
            const auto t = text_size(f.bold, 13 * k, key);
            text(d, f.bold, 13 * k, ImVec2(c.x + 12 * k - t.x * 0.5f, c.y + 12 * k - t.y * 0.5f), white, key);
        }
    };
    cap(x - cap_w, "Q", Glyph::lb);
    for (int i = 0; i < static_cast<int>(menu_tabs.size()); ++i) {
        const auto t = text_size(f.heading, fs, menu_tabs[static_cast<std::size_t>(i)]);
        const ImVec2 a(x, y), b(x + t.x + 2 * side, y + h);
        ImGui::PushID(i);
        bool lit = false;
        if (hit("##menutab", a, b, &lit)) u.menu_tab = i;
        ImGui::PopID();
        const bool chosen = u.menu_tab == i;
        if (chosen) {
            if (!draw_studio_brush(d, ImVec2(a.x - 6 * k, a.y + 4 * k), ImVec2(b.x + 6 * k, b.y - 2 * k), blue))
                skate_theme::rough_rect(d, a, b, blue, seed_of(a), k);
        } else if (lit) skate_theme::rough_rect(d, a, b, IM_COL32(255, 255, 255, 22), seed_of(a), k);
        if (lit && nav().keys) chosen_outline(d, a, b, k);
        text(d, f.heading, fs, ImVec2(a.x + side, a.y + (h - t.y) * 0.5f), chosen || lit ? white : IM_COL32(170, 172, 178, 255),
             menu_tabs[static_cast<std::size_t>(i)]);
        x = b.x + gap;
    }
    cap(x - gap + 20 * k, "E", Glyph::rb);
}
// Along the bottom: what the studio last said on the left, what the buttons do on the right.
void menu_footer(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display, float left, float right, const std::string& line) {
    auto& u = ui();
    const float k = f.k, y = display.y - 50 * k;
    d->AddRectFilled(ImVec2(0, y - 14 * k), ImVec2(display.x, display.y), IM_COL32(0, 0, 0, 120));
    if (!line.empty()) text(d, f.bold, 15 * k, ImVec2(left + 48 * k, y + 2 * k), v.failure.empty() ? white : skate_theme::danger,
                            elide(f.bold, 15 * k, line, (right - left) * 0.5f));
    using P = std::tuple<std::optional<Glyph>, const char*, const char*>;
    std::vector<P> list;
    if (u.pad_mode) list = {{Glyph::dpad_ud, "", "Choose"}, {Glyph::a, "", "Select"}, {Glyph::b, "", "Back"}, {Glyph::lb, "", "Tabs"}};
    else list = {{std::nullopt, "ARROWS", "Choose"}, {std::nullopt, "ENTER", "Select"}, {std::nullopt, "ESC", "Back"}, {std::nullopt, "Q / E", "Tabs"}};
    if (v.has_clip) list.emplace_back(u.pad_mode ? std::optional<Glyph>(Glyph::view) : std::nullopt, "TAB", "Editor");
    float w = 0;
    for (const auto& [g, key, what] : list) w += prompt_width(f, g, key, what);
    float x = right - 48 * k - w;
    for (const auto& [g, key, what] : list) x += prompt(d, f, ImVec2(x, y), g, key, what, u.playstation);
}
// The top of the menu: the studio's name on the left, the tabs in the middle, back to the editor on the right.
void market_header(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display, float right); // below
void menu_header(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display, float left, float right) {
    if (ui().menu_tab == 2) {
        market_header(d, f, v, display, right);
        return;
    }
    const float k = f.k;
    text(d, f.bold, 14 * k, ImVec2(left + 50 * k, 26 * k), blue_light, "RESKATE");
    text(d, f.title, 36 * k, ImVec2(left + 48 * k, 40 * k), white, "ANIMATOR STUDIO");
    menu_tabs_row(d, f, display.x * 0.5f, 24 * k);
    float x = right - 48 * k;
    {
        const float w = 150 * k;
        x -= w;
        if (action(d, f, "##closestudio", ImVec2(x, 30 * k), ImVec2(w, 42 * k), "CLOSE (F8)", Look::ghost)) studio::request_close();
    }
    if (v.has_clip) {
        const float w = 200 * k;
        x -= w + 12 * k;
        if (action(d, f, "##toeditor", ImVec2(x, 30 * k), ImVec2(w, 42 * k), "BACK TO EDITOR", Look::paper)) ui().menu = false;
    }
    d->AddRectFilled(ImVec2(left + 48 * k, 98 * k), ImVec2(right - 48 * k, 99.5f * k), IM_COL32(255, 255, 255, 30));
}
// A page's title: big, brushed, with what it is for under it.
void page_heading(ImDrawList* d, const Fonts& f, ImVec2 at, const std::string& title, const std::string& about) {
    const float k = f.k;
    text(d, f.title, 46 * k, at, white, title);
    if (!about.empty()) text(d, f.bold, 15 * k, ImVec2(at.x + 2 * k, at.y + 50 * k), soft, about);
}

// ---- TRICKS ---------------------------------------------------------------------------------------
struct TrickCard {
    std::string trick, label, category;
    std::array<const studio::GameTrick*, 4> pops{}; // low, mid, high, and the one that is any pop
    // A card of the game's pieces - a grind, a grab, a manual (native_trick_parts.h): every piece of it, way by way.
    struct Piece {
        std::string side, part;
        int order{};
        const studio::GameTrick* clip{};
    };
    std::vector<Piece> pieces;
    std::vector<std::string> sides; // the ways it is done, in order ("" when there is one way)
    std::string search;             // its name, kind, ways and pieces, for SEARCH
};
// The ways a trick is done in a sensible order: one way, FS, BS, straight, then the same fakie and nollie, then grabs.
int side_rank(std::string_view side) {
    if (side.empty()) return 0;
    if (side == "FS") return 1;
    if (side == "BS") return 2;
    if (side == "STRAIGHT") return 3;
    if (side.starts_with("FAKIE")) return 10 + side_rank(side.substr(std::min<std::size_t>(side.size(), 6)));
    if (side.starts_with("NOLLIE")) return 20 + side_rank(side.substr(std::min<std::size_t>(side.size(), 7)));
    return 50;
}
// The game's tricks as cards: its whole tricks by their pops, then its pieces gathered into the tricks they are part
// of. Made once for each list (kept alive with it), not every frame: the pieces' names are read here.
const std::vector<TrickCard>& trick_cards(const std::shared_ptr<const std::vector<studio::GameTrick>>& tricks) {
    static std::shared_ptr<const std::vector<studio::GameTrick>> made_for;
    static std::vector<TrickCard> out;
    if (made_for == tricks) return out;
    made_for = tricks;
    out.clear();
    if (!tricks) return out;
    std::unordered_map<std::string, std::size_t> at;
    for (const auto& t : *tricks) {
        if (t.preview_only) continue;
        const auto [it, fresh] = at.emplace(t.trick, out.size());
        if (fresh) out.push_back({t.trick, t.label, t.category, {}});
        auto& c = out[it->second];
        const std::size_t slot = t.pop == "low" ? 0 : t.pop == "mid" ? 1 : t.pop == "high" ? 2 : 3;
        if (!c.pops[slot]) c.pops[slot] = &t;
    }
    for (auto& c : out) c.search = studio::native_search_key(c.label + " " + c.trick + " " + c.category);
    struct Made {
        TrickCard card;
        int kind_order{}, trick_order{};
    };
    std::vector<Made> pieces;
    std::unordered_map<std::string, std::size_t> piece_at;
    for (const auto& t : *tricks) {
        if (!t.preview_only || t.recorded || t.asset.empty()) continue;
        for (auto& part : studio::native_trick_parts(t.asset)) {
            const auto key = "*piece:" + part.kind + "/" + part.trick;
            const auto [it, fresh] = piece_at.emplace(key, pieces.size());
            if (fresh) {
                Made made;
                made.card.trick = key;
                made.card.label = part.trick;
                made.card.category = part.kind;
                made.kind_order = part.kind_order;
                made.trick_order = part.trick_order;
                pieces.push_back(std::move(made));
            }
            pieces[it->second].card.pieces.push_back({std::move(part.side), std::move(part.part), part.part_order, &t});
        }
    }
    std::stable_sort(pieces.begin(), pieces.end(), [](const Made& a, const Made& b) {
        return std::tie(a.kind_order, a.trick_order, a.card.label) < std::tie(b.kind_order, b.trick_order, b.card.label);
    });
    for (auto& made : pieces) {
        auto& c = made.card;
        std::stable_sort(c.pieces.begin(), c.pieces.end(), [](const TrickCard::Piece& a, const TrickCard::Piece& b) {
            const int ra = side_rank(a.side), rb = side_rank(b.side);
            return std::tie(ra, a.side, a.order, a.part) < std::tie(rb, b.side, b.order, b.part);
        });
        std::string words = c.label + " " + c.category;
        for (const auto& piece : c.pieces) {
            if (c.sides.empty() || c.sides.back() != piece.side) c.sides.push_back(piece.side), words += " " + piece.side;
            words += " " + piece.part;
        }
        c.search = studio::native_search_key(words);
        out.push_back(std::move(c));
    }
    return out;
}
// The clip a piece is, as the studio names it once it is open ("native:<asset>").
std::string piece_trick(const TrickCard::Piece& piece) { return "native:" + piece.clip->asset; }
void open_game_trick(const studio::GameTrick& t) {
    std::string m;
    studio::load_game_trick(t, m);
    say(m);
    ui().names_for = 0;
}
void open_saved(const std::string& name) {
    std::string m;
    studio::load(name, m);
    say(m);
    ui().names_for = 0;
}
// Watching a saved trick (preview mode): the studio plays it with nothing else on screen but a card about it and the
// buttons to watch it by. `pack`: the pack it was opened from, shown on the card ("" none).
void preview_trick(const std::string& pack, const std::string& name, bool from_pack) {
    auto& u = ui();
    std::string m;
    if (!studio::preview_saved(name, m)) {
        say(m);
        return;
    }
    u.preview_pack = pack;
    u.preview_name = name;
    u.preview_from_pack = from_pack;
    u.menu = false;
    u.names_for = 0;
}
// Opens a saved trick to edit, or - one locked to its maker - to watch.
void open_or_preview(const studio::View& v, const std::string& name, const std::string& pack = {}) {
    const auto* p = preset_named(v, name);
    if (p && p->locked) preview_trick(pack, name, !pack.empty());
    else open_saved(name);
}
// A tick box: ticked or not, drawn (the fonts have no tick).
void tick_box(ImDrawList* d, ImVec2 c, float half, bool ticked, ImU32 colour, float k) {
    d->AddRect(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), colour, 2 * k, 0, 2 * k);
    if (!ticked) return;
    d->AddLine(ImVec2(c.x - half * 0.6f, c.y), ImVec2(c.x - half * 0.1f, c.y + half * 0.5f), colour, 2.4f * k);
    d->AddLine(ImVec2(c.x - half * 0.1f, c.y + half * 0.5f), ImVec2(c.x + half * 0.75f, c.y - half * 0.6f), colour, 2.4f * k);
}
// The stance versions of a trick: your own stance, fakie, switch and switch fakie, ticked where there is one (among
// `among`, or all your saved tricks when empty). A ticked one opens it; an empty one makes it from `name` and opens it.
// Returns the height it took.
float stance_versions(ImDrawList* d, const Fonts& f, const studio::View& v, const std::string& name, ImVec2 at, float w,
                      const std::vector<std::string>& among = {}, const std::string& pack = {}, bool* acted = nullptr) {
    const auto* p = preset_named(v, name);
    if (!p || p->trick.starts_with("native:")) return 0;
    const float k = f.k;
    std::array<std::string, 4> have{};
    for (const auto& q : v.presets) {
        if (q.trick != p->trick || q.pop != p->pop) continue;
        if (!among.empty() && std::find(among.begin(), among.end(), q.name) == among.end()) continue;
        const auto version = static_cast<std::size_t>(studio::stance_version(q.stance, q.switch_stance, q.fakie, v.own_lead_right));
        if (have[version].empty() || q.name == name) have[version] = q.name;
    }
    static constexpr std::array<const char*, 4> labels{"OWN STANCE", "FAKIE", "SWITCH", "SWITCH FAKIE"};
    float x = at.x, y = at.y;
    text(d, f.bold, 11 * k, ImVec2(x, y + 9 * k), faint, "STANCES");
    x += 70 * k;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const float cw = text_size(f.bold, 12 * k, labels[i]).x + 40 * k;
        if (x + cw > at.x + w) x = at.x + 70 * k, y += 34 * k;
        const ImVec2 a(x, y), b(x + cw, y + 30 * k);
        ImGui::PushID(static_cast<int>(i));
        bool lit = false;
        const bool done = !have[i].empty();
        if (hit("##stance", a, b, &lit, !p->locked || done)) {
            if (acted) *acted = true;
            if (done) open_or_preview(v, have[i], pack);
            else {
                std::string made, m;
                if (studio::make_stance_version(name, static_cast<int>(i), made, m)) open_saved(made);
                say(m);
            }
        }
        d->AddRectFilled(a, b, lit ? IM_COL32(60, 62, 70, 240) : done ? IM_COL32(18, 52, 92, 240) : IM_COL32(28, 29, 33, 240), 4 * k);
        tick_box(d, ImVec2(a.x + 14 * k, a.y + 15 * k), 6 * k, done, done ? blue_light : soft, k);
        text(d, f.bold, 12 * k, ImVec2(a.x + 28 * k, a.y + 8 * k), done ? white : soft, labels[i]);
        ImGui::PopID();
        x += cw + 6 * k;
    }
    return y + 34 * k - at.y;
}
// A trick was put in a pack: the card asks for its other stances there (only for a whole trick, and only when the pack
// is missing one).
void ask_stances(const studio::View& v, const std::string& pack, const std::string& name) {
    const auto* p = preset_named(v, name);
    if (!p || p->trick.starts_with("native:")) return;
    auto& u = ui();
    u.stance_pack = pack;
    u.stance_name = name;
}
// One of the game's tricks: its name and kind, how many of yours there are of it, and its pops to open.
void game_trick_card(ImDrawList* d, const Fonts& f, const studio::View& v, const TrickCard& c, ImVec2 a, ImVec2 size, int saved) {
    const float k = f.k, strip = 46 * k;
    const ImVec2 b(a.x + size.x, a.y + size.y);
    ImGui::PushID(c.trick.c_str());
    bool lit = false;
    const bool open = hit("##card", a, ImVec2(b.x, b.y - strip), &lit);
    const bool current = v.has_clip && v.trick == c.trick;
    skate_theme::rough_rect(d, a, b, lit ? IM_COL32(48, 50, 56, 250) : IM_COL32(27, 28, 31, 248), seed_of(a), k);
    theme::halftone(d, ImVec2(b.x - 52 * k, a.y + 10 * k), 6, 3, IM_COL32(255, 255, 255, 18));
    if (current) d->AddRectFilled(ImVec2(a.x, a.y + 6 * k), ImVec2(a.x + 5 * k, b.y - 6 * k), blue);
    float right_edge = b.x - 14 * k;
    if (saved) {
        const auto label = std::to_string(saved) + " OF YOURS";
        right_edge -= tag_width(f, label);
        tag(d, f, ImVec2(right_edge, a.y + 14 * k), label, blue);
        right_edge -= 6 * k;
    }
    if (current) {
        right_edge -= tag_width(f, "OPEN");
        tag(d, f, ImVec2(right_edge, a.y + 14 * k), "OPEN", IM_COL32(255, 255, 255, 40));
    }
    text(d, f.bold, 21 * k, ImVec2(a.x + 18 * k, a.y + 14 * k), white, elide(f.bold, 21 * k, c.label, right_edge - a.x - 26 * k));
    text(d, f.bold, 12 * k, ImVec2(a.x + 18 * k, a.y + 42 * k), soft, upper(c.category));
    if (lit && nav().keys) chosen_outline(d, a, ImVec2(b.x, b.y - strip), k);
    float x = a.x + 14 * k;
    static constexpr std::array<const char*, 4> names{"LOW", "MID", "HIGH", "CLIP"};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!c.pops[i]) continue;
        float w = 0;
        ImGui::PushID(static_cast<int>(i));
        if (chip(d, f, "##pop", ImVec2(x, b.y - strip + 6 * k), names[i], current && v.pop == c.pops[i]->pop, &w, 30 * k)) open_game_trick(*c.pops[i]);
        ImGui::PopID();
        x += w + 6 * k;
    }
    if (open) {
        for (const std::size_t i : {1, 2, 0, 3})
            if (c.pops[i]) {
                open_game_trick(*c.pops[i]);
                break;
            }
    }
    ImGui::PopID();
}
// The piece of a way of doing it that a card's chip opens: its loop or hold if it has one, else its first.
const TrickCard::Piece* main_piece(const TrickCard& c, const std::string& side) {
    const TrickCard::Piece* first = nullptr;
    for (const auto& piece : c.pieces) {
        if (piece.side != side) continue;
        if (!first) first = &piece;
        if (piece.part == "LOOP" || piece.part == "HOLD" || piece.part == "WHOLE" || piece.part == "POP" || piece.part == "TRICK") return &piece;
    }
    return first;
}
// A card of the game's pieces (a grind, a grab): its name and kind, how many pieces, and a chip for each way it is
// done that opens its loop. The card itself opens its side panel, with every piece.
void piece_card(ImDrawList* d, const Fonts& f, const studio::View& v, const TrickCard& c, ImVec2 a, ImVec2 size, int saved) {
    auto& u = ui();
    const float k = f.k, strip = 46 * k;
    const ImVec2 b(a.x + size.x, a.y + size.y);
    ImGui::PushID(c.trick.c_str());
    bool lit = false;
    if (hit("##card", a, ImVec2(b.x, b.y - strip), &lit)) {
        u.piece_card = u.piece_card == c.trick ? std::string{} : c.trick;
        u.trick_page.clear();
        u.drawer_scroll = 0;
    }
    const TrickCard::Piece* open = nullptr;
    if (v.has_clip)
        for (const auto& piece : c.pieces)
            if (v.trick == piece_trick(piece)) open = &piece;
    const bool panel = u.piece_card == c.trick;
    skate_theme::rough_rect(d, a, b, panel ? IM_COL32(30, 52, 86, 250) : lit ? IM_COL32(48, 50, 56, 250) : IM_COL32(27, 28, 31, 248), seed_of(a), k);
    theme::halftone(d, ImVec2(b.x - 52 * k, a.y + 10 * k), 6, 3, IM_COL32(255, 255, 255, 18));
    if (open) d->AddRectFilled(ImVec2(a.x, a.y + 6 * k), ImVec2(a.x + 5 * k, b.y - 6 * k), blue);
    float right_edge = b.x - 14 * k;
    if (saved) {
        const auto label = std::to_string(saved) + " OF YOURS";
        right_edge -= tag_width(f, label);
        tag(d, f, ImVec2(right_edge, a.y + 14 * k), label, blue);
        right_edge -= 6 * k;
    }
    if (open) {
        right_edge -= tag_width(f, "OPEN");
        tag(d, f, ImVec2(right_edge, a.y + 14 * k), "OPEN", IM_COL32(255, 255, 255, 40));
    }
    text(d, f.bold, 21 * k, ImVec2(a.x + 18 * k, a.y + 14 * k), white, elide(f.bold, 21 * k, c.label, right_edge - a.x - 26 * k));
    const auto about = upper(c.category) + "  /  " + std::to_string(c.pieces.size()) + (c.pieces.size() == 1 ? " PIECE" : " PIECES");
    text(d, f.bold, 12 * k, ImVec2(a.x + 18 * k, a.y + 42 * k), soft, elide(f.bold, 12 * k, about, size.x - 36 * k));
    if (lit && nav().keys) chosen_outline(d, a, ImVec2(b.x, b.y - strip), k);
    // A chip for each way, as many as fit; the rest are in the side panel.
    float x = a.x + 14 * k;
    const float more_w = chip_width(f, "+99");
    for (std::size_t i = 0; i < c.sides.size(); ++i) {
        const auto& side = c.sides[i];
        const auto label = side.empty() ? std::string("OPEN") : side;
        const float w = chip_width(f, label);
        const bool last = i + 1 == c.sides.size();
        if (x + w + (last ? 0 : more_w + 6 * k) > b.x - 14 * k) {
            const auto more = "+" + std::to_string(c.sides.size() - i);
            if (chip(d, f, "##more", ImVec2(x, b.y - strip + 6 * k), more, false, nullptr, 30 * k)) {
                u.piece_card = c.trick;
                u.trick_page.clear();
                u.drawer_scroll = 0;
            }
            break;
        }
        ImGui::PushID(static_cast<int>(i));
        float used = 0;
        if (chip(d, f, "##side", ImVec2(x, b.y - strip + 6 * k), label, open && open->side == side, &used, 30 * k))
            if (const auto* piece = main_piece(c, side)) open_game_trick(*piece->clip);
        ImGui::PopID();
        x += used + 6 * k;
    }
    ImGui::PopID();
}
// Beside the grid: every piece of a card of the game's pieces, way by way, each to open in the editor.
void piece_drawer(ImDrawList* d, const Fonts& f, const studio::View& v, const TrickCard& c, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    const float k = f.k;
    skate_theme::rough_rect(d, a, b, IM_COL32(18, 19, 22, 250), seed_of(a), k);
    if (action(d, f, "##piececlose", ImVec2(b.x - 52 * k, a.y + 14 * k), ImVec2(38 * k, 34 * k), "X", Look::ghost)) {
        u.piece_card.clear();
        return;
    }
    const float x = a.x + 26 * k, w = b.x - a.x - 52 * k;
    float y = a.y + 18 * k;
    text(d, f.title, 34 * k, ImVec2(x, y), white, elide(f.title, 34 * k, upper(c.label), w - 50 * k));
    y += 42 * k;
    text(d, f.bold, 14 * k, ImVec2(x, y), soft, c.category + ", " + std::to_string(c.pieces.size()) + " pieces of the game's own animation");
    y += 24 * k;
    y += paragraph(d, f.bold, 13 * k, ImVec2(x, y), w, faint,
                   "Open a piece, change it and save it. In game your version plays whenever the game plays that piece: the studio "
                   "knows it by the skater's pose.");
    y += 14 * k;
    begin_region(32, ImVec2(a.x, y), ImVec2(b.x, b.y - 10 * k), u.drawer_scroll, k);
    const float top = y;
    y -= u.drawer_scroll;
    std::size_t i = 0;
    while (i < c.pieces.size()) {
        const auto& side = c.pieces[i].side;
        std::size_t end = i;
        while (end < c.pieces.size() && c.pieces[end].side == side) ++end;
        heading(d, f, ImVec2(x, y), w, side.empty() ? std::string("Pieces") : side, std::to_string(end - i));
        y += 34 * k;
        float cx = x;
        for (; i < end; ++i) {
            const auto& piece = c.pieces[i];
            const float cw = chip_width(f, piece.part);
            if (cx + cw > x + w) cx = x, y += 36 * k;
            ImGui::PushID(static_cast<int>(i));
            if (chip(d, f, "##piece", ImVec2(cx, y), piece.part, v.has_clip && v.trick == piece_trick(piece))) open_game_trick(*piece.clip);
            ImGui::PopID();
            cx += cw + 8 * k;
        }
        y += 48 * k;
    }
    end_region(d, ImVec2(a.x, top), ImVec2(b.x, b.y - 10 * k), y + u.drawer_scroll - top, u.drawer_scroll, k);
}
// One of your saved tricks: the name you gave it, what it is, whether it plays, and EDIT / TEST.
void my_trick_card(ImDrawList* d, const Fonts& f, const studio::View& v, const std::string& name, ImVec2 a, ImVec2 size) {
    auto& u = ui();
    const float k = f.k, strip = 46 * k;
    const ImVec2 b(a.x + size.x, a.y + size.y);
    const auto* p = preset_named(v, name);
    ImGui::PushID(name.c_str());
    bool lit = false;
    if (hit("##mine", a, ImVec2(b.x, b.y - strip), &lit)) {
        u.trick_page = name;
        u.piece_card.clear();
        u.label_for.clear();
        u.confirm_delete.clear();
    }
    const bool page = u.trick_page == name;
    skate_theme::rough_rect(d, a, b, page ? IM_COL32(30, 52, 86, 250) : lit ? IM_COL32(48, 50, 56, 250) : IM_COL32(27, 28, 31, 248), seed_of(a), k);
    theme::halftone(d, ImVec2(b.x - 52 * k, a.y + 10 * k), 6, 3, IM_COL32(255, 255, 255, 18));
    if (name == v.name && v.has_clip) d->AddRectFilled(ImVec2(a.x, a.y + 6 * k), ImVec2(a.x + 5 * k, b.y - 6 * k), blue);
    float right_edge = b.x - 14 * k;
    if (p && p->active) {
        right_edge -= tag_width(f, "PLAYS");
        tag(d, f, ImVec2(right_edge, a.y + 14 * k), "PLAYS", IM_COL32(36, 150, 76, 255));
        right_edge -= 6 * k;
    }
    if (p && p->locked) {
        right_edge -= tag_width(f, "LOCKED");
        tag(d, f, ImVec2(right_edge, a.y + 14 * k), "LOCKED", IM_COL32(120, 34, 36, 255));
        right_edge -= 6 * k;
    }
    std::size_t packs = 0;
    for (const auto& pack : v.packs) packs += std::find(pack.members.begin(), pack.members.end(), name) != pack.members.end();
    if (packs) {
        const auto label = std::to_string(packs) + (packs == 1 ? " PACK" : " PACKS");
        right_edge -= tag_width(f, label);
        tag(d, f, ImVec2(right_edge, a.y + 14 * k), label, IM_COL32(255, 255, 255, 36));
    }
    text(d, f.bold, 21 * k, ImVec2(a.x + 18 * k, a.y + 14 * k), white, elide(f.bold, 21 * k, shown_name(v, name), right_edge - a.x - 26 * k));
    std::string what = p ? upper(trick_label(p->trick)) : std::string("A PIECE OF THE GAME'S ANIMATION");
    if (p)
        for (const auto& [label, colour] : tags_of(*p)) what += "  /  " + label;
    text(d, f.bold, 12 * k, ImVec2(a.x + 18 * k, a.y + 42 * k), soft, elide(f.bold, 12 * k, what, size.x - 36 * k));
    if (lit && nav().keys) chosen_outline(d, a, ImVec2(b.x, b.y - strip), k);
    const float bw = (size.x - 28 * k - 8 * k) / 2;
    if (p && p->locked) {
        if (action(d, f, "##edit", ImVec2(a.x + 14 * k, b.y - strip + 6 * k), ImVec2(bw, 30 * k), "PREVIEW", Look::blue)) preview_trick({}, name, false);
    } else if (action(d, f, "##edit", ImVec2(a.x + 14 * k, b.y - strip + 6 * k), ImVec2(bw, 30 * k), "EDIT", Look::blue))
        open_saved(name);
    if (action(d, f, "##test", ImVec2(a.x + 22 * k + bw, b.y - strip + 6 * k), ImVec2(bw, 30 * k), "TEST IN GAME")) {
        std::string m;
        studio::clear_test_returned();
        studio::test_in_game(name, m);
        say(m);
    }
    ImGui::PopID();
}
// Beside the grid: one saved trick's own card - its name, its pop, its packs, and what can be done with it.
void trick_drawer(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    const float k = f.k;
    const auto name = u.trick_page;
    skate_theme::rough_rect(d, a, b, IM_COL32(18, 19, 22, 250), seed_of(a), k);
    if (action(d, f, "##drawerclose", ImVec2(b.x - 52 * k, a.y + 14 * k), ImVec2(38 * k, 34 * k), "X", Look::ghost)) {
        u.trick_page.clear();
        return;
    }
    const float x = a.x + 26 * k, w = b.x - a.x - 52 * k;
    if (!saved_trick(v, name)) {
        text(d, f.heading, 20 * k, ImVec2(x, a.y + 22 * k), white, "THIS TRICK IS GONE");
        return;
    }
    const auto* p = preset_named(v, name);
    float y = a.y + 18 * k;
    text(d, f.title, 34 * k, ImVec2(x, y), white, elide(f.title, 34 * k, upper(shown_name(v, name)), w - 50 * k));
    y += 42 * k;
    const std::string what = p ? trick_label(p->trick) + ", " + (p->pop.empty() ? std::string("any pop") : p->pop + " pop") : std::string("a piece");
    text(d, f.bold, 14 * k, ImVec2(x, y), soft, what + (shown_name(v, name) != name ? "   (file: " + name + ")" : std::string{}));
    y += 20 * k;
    if (p && !p->made_by.empty()) {
        text(d, f.bold, 13 * k, ImVec2(x, y), faint, "Made by " + p->made_by);
        y += 18 * k;
    }
    y += 12 * k;
    begin_region(31, ImVec2(a.x, y), ImVec2(b.x, b.y - 10 * k), u.drawer_scroll, k);
    const float top = y;
    y -= u.drawer_scroll;
    heading(d, f, ImVec2(x, y), w, "Name it");
    y += 32 * k;
    if (u.label_for != name && !ImGui::IsAnyItemActive()) {
        std::snprintf(u.label.data(), u.label.size(), "%s", p ? p->label.c_str() : "");
        u.label_for = name;
    }
    bool edited = false;
    const bool entered = field(f, "##label", u.label, ImVec2(x, y), w - 96 * k, "e.g. Steezy kickflip", &edited);
    if (action(d, f, "##setlabel", ImVec2(x + w - 86 * k, y), ImVec2(86 * k, 34 * k), "SET") || entered || edited) {
        std::string m;
        studio::set_label(name, u.label.data(), m);
        say(m);
    }
    y += 50 * k;
    heading(d, f, ImVec2(x, y), w, "Plays on");
    y += 32 * k;
    if (p && p->trick.starts_with("native:")) {
        // A piece of the game's animation (a grind's loop, a grab's hold) is not popped: it plays when the game plays it.
        y += paragraph(d, f.bold, 14 * k, ImVec2(x, y), w, soft,
                       "Whenever the game plays this piece, " + piece_label(p->trick) + ": the studio knows it by the skater's pose. "
                       "Pieces have no pop, so there is nothing to copy to low, mid and high.");
        y += 18 * k;
    } else {
        const std::string pop = p ? p->pop : std::string{};
        const int chosen = pop.empty() ? 0 : pop == "low" ? 1 : pop == "mid" ? 2 : 3;
        const int picked = segmented(d, f, "##poprule", ImVec2(x, y), w, 36 * k, {"ANY POP", "LOW", "MID", "HIGH"}, chosen, p != nullptr);
        if (picked >= 0 && picked != chosen) {
            static constexpr std::array<const char*, 4> pops{"", "low", "mid", "high"};
            std::string m;
            studio::set_saved_pop(name, pops[static_cast<std::size_t>(picked)], m);
            say(m);
        }
        y += 44 * k;
        if (action(d, f, "##copypops", ImVec2(x, y), ImVec2(w, 36 * k), "COPY TO LOW, MID AND HIGH", Look::dark, p != nullptr)) {
            std::string m;
            studio::copy_saved_to_pops(name, m);
            say(m);
        }
        y += 44 * k;
        y += paragraph(d, f.bold, 13 * k, ImVec2(x, y), w, faint,
                       "Any pop: it plays on every pop the game does, unless you have one of this trick saved for that exact pop. "
                       "Copy makes three copies, one for each pop height, to change each on its own.");
        y += 16 * k;
    }
    heading(d, f, ImVec2(x, y), w, "In your packs");
    y += 32 * k;
    {
        float cx = x;
        if (v.packs.empty()) {
            y += paragraph(d, f.bold, 14 * k, ImVec2(x, y), w, soft, "No packs yet.") + 6 * k;
        }
        for (const auto& pack : v.packs) {
            const bool in = std::find(pack.members.begin(), pack.members.end(), name) != pack.members.end();
            const float cw = chip_width(f, pack.name);
            if (cx + cw > x + w) cx = x, y += 36 * k;
            ImGui::PushID(pack.name.c_str());
            if (chip(d, f, "##inpack", ImVec2(cx, y), pack.name, in)) {
                std::string m;
                if (studio::set_pack_member(pack.name, name, !in, m) && !in) ask_stances(v, pack.name, name);
                say(m);
            }
            ImGui::PopID();
            cx += cw + 8 * k;
        }
        const float nw = chip_width(f, "+ NEW PACK");
        if (cx + nw > x + w) cx = x, y += 36 * k;
        if (chip(d, f, "##newpackwith", ImVec2(cx, y), "+ NEW PACK", false)) {
            const auto pack = unique_pack_name(v);
            std::string m;
            if (studio::create_preset_pack(m, pack) && studio::set_pack_member(pack, name, true, m)) ask_stances(v, pack, name);
            say(m);
        }
        y += 48 * k;
    }
    if (p && !p->trick.starts_with("native:")) {
        heading(d, f, ImVec2(x, y), w, "Its stances");
        y += 32 * k;
        y += stance_versions(d, f, v, name, ImVec2(x, y), w) + 4 * k;
        y += paragraph(d, f.bold, 12 * k, ImVec2(x, y), w, faint,
                       "A version for each stance, to change on its own: with Trick matching on Exact stance each plays only in its "
                       "stance, so every way you do the trick can look its own.") + 14 * k;
    }
    if (p && p->locked) {
        y += paragraph(d, f.bold, 13 * k, ImVec2(x, y), w, skate_theme::warning,
                       "Locked to its maker" + (p->made_by.empty() ? std::string{} : " (" + p->made_by + ")") +
                           ": it plays, tests and previews, but cannot be opened to edit, copied or shared again.") + 12 * k;
    }
    const float half = (w - 10 * k) / 2;
    if (p && p->locked) {
        if (action(d, f, "##drawedit", ImVec2(x, y), ImVec2(half, 42 * k), "PREVIEW", Look::blue)) preview_trick({}, name, false);
    } else if (action(d, f, "##drawedit", ImVec2(x, y), ImVec2(half, 42 * k), "OPEN IN EDITOR", Look::blue))
        open_saved(name);
    if (action(d, f, "##drawtest", ImVec2(x + half + 10 * k, y), ImVec2(half, 42 * k), "TEST IN GAME", Look::paper)) {
        std::string m;
        studio::clear_test_returned();
        studio::test_in_game(name, m);
        say(m);
    }
    y += 52 * k;
    if (action(d, f, "##drawdelete", ImVec2(x, y), ImVec2(w, 38 * k), u.confirm_delete == name ? "PRESS AGAIN TO DELETE" : "DELETE THIS TRICK", Look::danger)) {
        if (u.confirm_delete != name) u.confirm_delete = name;
        else {
            std::string m;
            studio::remove(name, m);
            say(m);
            u.confirm_delete.clear();
            u.trick_page.clear();
        }
    }
    y += 50 * k;
    end_region(d, ImVec2(a.x, top), ImVec2(b.x, b.y - 10 * k), y + u.drawer_scroll - top, u.drawer_scroll, k);
}
// The game's animation tree, as a list (it is a tree of folders, not cards).
void asset_list(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    const float k = f.k, row_h = 46 * k, gap = 6 * k;
    const auto rows = asset_rows(v);
    const float about_h = 96 * k;
    const ImVec2 la(a.x, a.y), lb(b.x, b.y - about_h - 12 * k);
    begin_region(41, la, lb, u.list_scroll, k);
    std::string about, about_title;
    float y = la.y - u.list_scroll;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const ImVec2 ra(la.x, y), rb(lb.x - 14 * k, y + row_h);
        y += row_h + gap;
        if (!selectable(row)) {
            const float fs = 15 * k;
            text(d, f.heading, fs, ImVec2(ra.x + 8 * k, rb.y - fs - 6 * k), white, upper(row.label));
            d->AddRectFilled(ImVec2(ra.x + 8 * k, rb.y - 3 * k), ImVec2(rb.x - 8 * k, rb.y - 1.5f * k), IM_COL32(255, 255, 255, 36));
            if (!row.value.empty()) {
                const auto t = text_size(f.bold, 13 * k, row.value.c_str());
                text(d, f.bold, 13 * k, ImVec2(rb.x - t.x - 10 * k, rb.y - fs - 4 * k), soft, row.value);
            }
            continue;
        }
        ImGui::PushID(static_cast<int>(i));
        bool lit = false;
        const bool pressed = hit("##assetrow", ra, rb, &lit, row.enabled);
        ImGui::PopID();
        if (lit) {
            skate_theme::rough_rect(d, ra, rb, blue, seed_of(ra), k);
            about = row.about;
            about_title = row.label;
        } else skate_theme::rough_rect(d, ra, rb, IM_COL32(24, 24, 26, 246), seed_of(ra), k);
        if (row.on && !lit) d->AddRectFilled(ImVec2(ra.x, ra.y + 6 * k), ImVec2(ra.x + 4 * k, rb.y - 6 * k), blue);
        const ImU32 ink = row.info ? soft : row.enabled ? white : faint;
        const float value_w = row.value.empty() ? 0 : std::min(text_size(f.bold, 15 * k, row.value.c_str()).x, (rb.x - ra.x) * 0.4f);
        text(d, f.bold, 19 * k, ImVec2(ra.x + 22 * k, ra.y + row_h * 0.5f - 11 * k), ink, elide(f.bold, 19 * k, row.label, rb.x - ra.x - value_w - 60 * k));
        if (!row.value.empty()) {
            const auto shown = elide(f.bold, 15 * k, row.value, (rb.x - ra.x) * 0.4f);
            const auto t = text_size(f.bold, 15 * k, shown.c_str());
            text(d, f.bold, 15 * k, ImVec2(rb.x - t.x - 18 * k, ra.y + row_h * 0.5f - 9 * k), lit ? white : soft, shown);
        }
        if (pressed && row.act) {
            u.row = static_cast<int>(i);
            row.act();
        }
    }
    end_region(d, la, lb, y + u.list_scroll - la.y, u.list_scroll, k);
    // What the row under the mouse (or the pad) is.
    const ImVec2 ha(a.x, b.y - about_h), hb(b.x, b.y);
    skate_theme::rough_rect(d, ha, hb, IM_COL32(18, 19, 22, 240), seed_of(ha), k * 0.8f);
    if (!about.empty()) {
        text(d, f.heading, 13 * k, ImVec2(ha.x + 18 * k, ha.y + 12 * k), blue_light, elide(f.heading, 13 * k, upper(about_title), hb.x - ha.x - 36 * k));
        paragraph(d, f.bold, 14 * k, ImVec2(ha.x + 18 * k, ha.y + 34 * k), hb.x - ha.x - 36 * k, IM_COL32(222, 222, 226, 255), about);
    } else
        text(d, f.bold, 14 * k, ImVec2(ha.x + 18 * k, ha.y + 14 * k), faint,
             "The game's whole animation tree: clips, graph states, transitions, grind phases. Indexed does not mean editable.");
}
// ---- the trick library, built on this PC ---------------------------------------------------------
// The game's tricks come from the player's own copy of the game: nothing of the game's ships with the studio. With no
// library yet, a panel to build one and how far the build has got; with one being rebuilt, or built from another
// version of the game, a strip saying so. Returns how much room it took at the top of `a`..`b`.
float library_panel(ImDrawList* d, const Fonts& f, ImVec2 a, ImVec2 b) {
    const float k = f.k, w = b.x - a.x;
    const auto build = studio::library_build();
    const auto bar = [&](ImVec2 at, float width, float h) {
        d->AddRectFilled(at, ImVec2(at.x + width, at.y + h), IM_COL32(255, 255, 255, 34), h * 0.5f);
        d->AddRectFilled(at, ImVec2(at.x + std::max(h, width * std::clamp(build.fraction, 0.0f, 1.0f)), at.y + h), blue, h * 0.5f);
    };
    const auto percent = std::to_string(static_cast<int>(std::clamp(build.fraction, 0.0f, 1.0f) * 100)) + "%";
    if (!build.exists) {
        const float h = 288 * k;
        const ImVec2 pa = a, pb(b.x, a.y + h);
        skate_theme::rough_rect(d, pa, pb, IM_COL32(20, 21, 24, 246), seed_of(pa), k);
        draw_studio_picture(d, StudioPicture::grain, pa, pb, IM_COL32(255, 255, 255, 34));
        // A strip of blue tape across the corner, as on the loading screen.
        d->AddQuadFilled(ImVec2(pb.x - 250 * k, pa.y), ImVec2(pb.x - 150 * k, pa.y), ImVec2(pb.x, pa.y + 120 * k), ImVec2(pb.x, pa.y + 210 * k), blue);
        theme::halftone(d, ImVec2(pb.x - 120 * k, pa.y + 24 * k), 8, 5, IM_COL32(255, 255, 255, 50));
        const float x = pa.x + 32 * k, text_w = std::min(w - 300 * k, 820 * k);
        text(d, f.title, 42 * k, ImVec2(x, pa.y + 26 * k), white, "BUILD YOUR TRICK LIBRARY");
        float y = pa.y + 84 * k;
        y += paragraph(d, f.bold, 16 * k, ImVec2(x, y), text_w, IM_COL32(222, 222, 226, 255),
                       "The studio makes its tricks from your own copy of skate.: every flip trick, grind, grab and manual the game "
                       "has, ready to open and change. It only reads the game's files, never changes them. Takes about a minute and "
                       "1.6 GB on this PC; do it once, and again after a game update.");
        y += 24 * k;
        if (build.running) {
            text(d, f.heading, 14 * k, ImVec2(x, y), blue_light, upper(build.step));
            const float bar_w = std::min(w - 260 * k, 820 * k);
            const auto t = text_size(f.bold, 15 * k, percent.c_str());
            text(d, f.bold, 15 * k, ImVec2(x + bar_w - t.x, y - 2 * k), white, percent);
            bar(ImVec2(x, y + 26 * k), bar_w, 12 * k);
            if (action(d, f, "##stop_library", ImVec2(x + bar_w + 28 * k, y + 6 * k), ImVec2(140 * k, 44 * k), "STOP", Look::ghost))
                studio::stop_library_build();
        } else {
            if (action(d, f, "##build_library", ImVec2(x, y), ImVec2(280 * k, 52 * k), build.failed ? "TRY AGAIN" : "BUILD THE LIBRARY", Look::blue))
                studio::start_library_build();
            if (!build.message.empty())
                paragraph(d, f.bold, 14 * k, ImVec2(x + 304 * k, y + 6 * k), std::max(100 * k, text_w - 304 * k),
                          build.failed ? skate_theme::warning : soft, build.message);
        }
        return h + 24 * k;
    }
    if (!build.running && !build.outdated) return 0;
    const float h = 62 * k;
    const ImVec2 pa = a, pb(b.x, a.y + h);
    skate_theme::rough_rect(d, pa, pb, IM_COL32(20, 21, 24, 246), seed_of(pa), k);
    d->AddRectFilled(pa, ImVec2(pa.x + 6 * k, pb.y), build.running ? blue : skate_theme::warning);
    const float x = pa.x + 24 * k;
    if (build.running) {
        text(d, f.heading, 14 * k, ImVec2(x, pa.y + 10 * k), blue_light, "REBUILDING THE TRICK LIBRARY");
        text(d, f.bold, 13 * k, ImVec2(x, pa.y + 34 * k), soft, build.step + "   " + percent);
        const float bar_x = x + 420 * k, bar_w = std::max(60 * k, pb.x - bar_x - 170 * k);
        bar(ImVec2(bar_x, pa.y + h * 0.5f - 5 * k), bar_w, 10 * k);
        if (action(d, f, "##stop_library", ImVec2(pb.x - 140 * k, pa.y + 11 * k), ImVec2(120 * k, 40 * k), "STOP", Look::ghost))
            studio::stop_library_build();
    } else {
        text(d, f.heading, 14 * k, ImVec2(x, pa.y + 10 * k), skate_theme::warning, "YOUR GAME WAS UPDATED");
        text(d, f.bold, 13 * k, ImVec2(x, pa.y + 34 * k), soft,
             elide(f.bold, 13 * k, "The trick library was built from an older version: rebuild it so the tricks match the game.", pb.x - x - 200 * k));
        if (action(d, f, "##rebuild_library", ImVec2(pb.x - 170 * k, pa.y + 11 * k), ImVec2(150 * k, 40 * k), "REBUILD", Look::blue))
            studio::start_library_build();
    }
    return h + 16 * k;
}
// Once a build ends, what came of it is said, whichever page is open.
void library_news() {
    static bool was_running = false;
    const auto build = studio::library_build();
    if (was_running && !build.running && !build.message.empty()) say(build.message);
    was_running = build.running;
}
// ---- the hover preview ----------------------------------------------------------------------------
// With PREVIEW on (TRICKS, top right), a trick card held under the mouse a moment has the skater himself do its trick:
// the menu leaves a window clear beside the card, the camera holds on him and his board from the side, and he does it
// once. Nothing is opened: what is open in the studio stays as it is, and the camera goes back as it was.
void note_hover(const std::string& key, ImVec2 a, ImVec2 b, std::function<bool()> start, const std::string& title = {}) {
    if (!ImGui::IsMouseHoveringRect(a, b) || !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
    auto& u = ui();
    u.hover_candidate = key;
    u.hover_candidate_a = a;
    u.hover_candidate_b = b;
    u.hover_candidate_start = std::move(start);
    u.hover_candidate_title = title;
}
// Whether a card lies under the preview's window (it is not drawn then: the skater is seen there).
bool under_peek(ImVec2 a, ImVec2 b) {
    const auto& u = ui();
    return u.peek_hole && a.x < u.peek_b.x && b.x > u.peek_a.x && a.y < u.peek_b.y && b.y > u.peek_a.y;
}
void stop_hover_peek() {
    auto& h = ui().hover;
    if (h.peeking) {
        studio::end_peek();
        studio::set_orbit(h.orbit);
        studio::set_camera_follow(ui().camera_shot != 0);
    }
    h.peeking = false;
    ui().peek_hole = false;
}
// The window was already left clear in the menu this frame: it is filled as the menu is, so the camera going back is not
// seen through it.
void cover_peek_hole() {
    const auto& u = ui();
    if (u.peek_hole) ImGui::GetWindowDrawList()->AddRectFilled(u.peek_a, u.peek_b, IM_COL32(10, 10, 12, 252));
}
// Leaving TRICKS (for the editor, another tab) ends a preview at once. Left playing, he went on doing the trick in the
// editor with the camera held at his side, and the menu came back with the old window still cut in it.
void end_hover_preview() {
    auto& u = ui();
    if (!u.hover.peeking && !u.peek_hole && u.hover.key.empty()) return;
    stop_hover_peek();
    u.hover = {};
}
void hover_preview(const Fonts& f, const studio::View& v) {
    auto& u = ui();
    auto& h = u.hover;
    const double now = ImGui::GetTime();
    if (!v.hover_preview || u.hover_candidate.empty() || any_card_open(v) || !u.menu || u.menu_tab != 0) {
        cover_peek_hole();
        stop_hover_peek();
        h = {};
        return;
    }
    if (h.key != u.hover_candidate) {
        cover_peek_hole();
        stop_hover_peek();
        h = {};
        h.key = u.hover_candidate;
        h.since = now;
        h.a = u.hover_candidate_a;
        h.b = u.hover_candidate_b;
        h.start = u.hover_candidate_start;
        u.hover_title = u.hover_candidate_title;
    }
    if (h.done) return;
    if (!h.tried) {
        if (now - h.since < 0.35) return; // a moment held on it, not passed over
        h.tried = true;
        if (!h.start || !h.start()) return;
        h.peeking = true;
        h.since = now;
        h.duration = std::max(0.3f, studio::peek_duration());
        h.orbit = v.orbit;
        // From his side, far enough back for the whole trick and the board, held on him.
        studio::Orbit orbit = v.orbit;
        orbit.pitch = 0.08f;
        orbit.distance = 4.6f;
        orbit.pan = {};
        if (const auto facing = chest_facing(v)) orbit.yaw = std::atan2(facing->at(2), -facing->at(0)) - v.base_yaw;
        studio::set_camera_follow(true);
        studio::set_focus(studio::all_joints);
        studio::set_orbit(orbit);
    }
    if (!h.peeking) return;
    const float k = f.k, played = static_cast<float>(now - h.since);
    if (played > h.duration + 0.9f) { // done, and the window closes until another card is held
        cover_peek_hole();
        stop_hover_peek();
        h.done = true;
        return;
    }
    // The window: beside the card, inside the cards' area, as tall as a skater in the air needs.
    const float w = std::min(460 * k, (u.grid_b.x - u.grid_a.x) * 0.48f), hgt = std::min(560 * k, u.grid_b.y - u.grid_a.y);
    ImVec2 a(h.b.x + 16 * k, h.a.y - 60 * k);
    if (a.x + w > u.grid_b.x) a.x = h.a.x - 16 * k - w;
    if (a.x < u.grid_a.x) a.x = std::min(u.grid_b.x - w, h.b.x + 16 * k);
    a.y = std::clamp(a.y, u.grid_a.y, std::max(u.grid_a.y, u.grid_b.y - hgt));
    const ImVec2 b(a.x + w, a.y + hgt);
    u.peek_hole = true;
    u.peek_a = a;
    u.peek_b = b;
    // The camera aims so he stands in the middle of the window.
    const auto display = ImGui::GetIO().DisplaySize;
    const ImVec2 middle((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + 40 * k);
    studio::set_view_shift((display.y * 0.5f - middle.y) / display.y, (middle.x - display.x * 0.5f) / display.y);
    auto* d = ImGui::GetForegroundDrawList();
    const float fade_in = std::clamp(played / 0.25f, 0.0f, 1.0f);
    // The first moments and the last are covered, fading: the camera takes a frame or two to get to his side, and he
    // goes back after. The window opens on him already in place and is closed again before anything moves back.
    const float opening = std::clamp((played - 0.12f) / 0.18f, 0.0f, 1.0f);
    const float closing = std::clamp((h.duration + 0.9f - played) / 0.2f, 0.0f, 1.0f);
    if (const float cover = 1 - std::min(opening, closing); cover > 0)
        d->AddRectFilled(a, b, IM_COL32(10, 10, 12, static_cast<int>(252 * cover)), 6 * k);
    // Its frame: the name across the top, how far it has got along the bottom.
    d->AddRect(a, b, IM_COL32(1, 131, 255, static_cast<int>(200 * fade_in)), 6 * k, 0, 3 * k);
    d->AddRectFilled(ImVec2(a.x, a.y), ImVec2(b.x, a.y + 40 * k), IM_COL32(12, 12, 14, 235), 6 * k, ImDrawFlags_RoundCornersTop);
    text(d, f.heading, 11 * k, ImVec2(a.x + 14 * k, a.y + 6 * k), blue_light, "PREVIEW");
    text(d, f.bold, 15 * k, ImVec2(a.x + 14 * k, a.y + 19 * k), white, elide(f.bold, 15 * k, u.hover_title, w - 28 * k));
    const float at = std::clamp(played / h.duration, 0.0f, 1.0f);
    d->AddRectFilled(ImVec2(a.x, b.y - 10 * k), b, IM_COL32(12, 12, 14, 235), 6 * k, ImDrawFlags_RoundCornersBottom);
    d->AddRectFilled(ImVec2(a.x + 10 * k, b.y - 7 * k), ImVec2(a.x + 10 * k + (w - 20 * k) * at, b.y - 3 * k), blue, 2 * k);
}
void tricks_page(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    u.hover_candidate.clear();
    const float k = f.k;
    const auto tricks = studio::game_tricks();
    const auto& cards = trick_cards(tricks);
    const auto assets = studio::native_assets();
    // The kinds, in the catalog's order, each with how many tricks are in it.
    std::vector<std::pair<std::string, int>> kinds;
    for (const auto& c : cards) {
        const auto found = std::find_if(kinds.begin(), kinds.end(), [&](const auto& kind) { return kind.first == c.category; });
        if (found == kinds.end()) kinds.emplace_back(c.category, 1);
        else ++found->second;
    }
    std::unordered_map<std::string, int> saved_of;
    for (const auto& name : v.library)
        if (const auto* p = preset_named(v, name)) ++saved_of[p->trick];
    // ---- the left: search and the kinds ----
    const float col_w = 340 * k;
    text(d, f.heading, 14 * k, ImVec2(a.x, a.y), blue_light, "SEARCH");
    field(f, "##search", u.asset_query, ImVec2(a.x, a.y + 22 * k), col_w, "Trick name, e.g. hardflip");
    {
        const std::string typed = u.asset_query.data();
        u.asset_filter = studio::native_search_terms(typed).empty() ? "" : typed;
    }
    struct Group {
        std::string key, label;
        std::size_t count;
    };
    std::vector<Group> groups{{"*mine", "MY TRICKS", v.library.size()}};
    for (const auto& [kind, count] : kinds) groups.push_back({kind, upper(kind), static_cast<std::size_t>(count)});
    groups.push_back({"*assets", "GAME ANIMATIONS", assets->size()});
    const ImVec2 ga(a.x, a.y + 76 * k), gb(a.x + col_w, b.y);
    begin_region(21, ga, gb, u.groups_scroll, k);
    float y = ga.y - u.groups_scroll;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (i == 1 || i + 1 == groups.size()) y += 10 * k; // your own first, the game's tree last
        const ImVec2 ra(ga.x, y), rb(gb.x - 12 * k, y + 46 * k);
        y += 52 * k;
        ImGui::PushID(static_cast<int>(i));
        bool lit = false;
        if (hit("##group", ra, rb, &lit)) {
            u.trick_group = g.key;
            u.trick_page.clear();
            u.cards_scroll = 0;
            u.browse_assets = g.key == "*assets";
        }
        ImGui::PopID();
        const bool chosen = u.trick_group == g.key;
        if (chosen) {
            skate_theme::rough_rect(d, ra, rb, blue, seed_of(ra), k);
            theme::halftone(d, ImVec2(rb.x - 60 * k, ra.y + 8 * k), 7, 4, IM_COL32(255, 255, 255, 40));
        } else skate_theme::rough_rect(d, ra, rb, lit ? IM_COL32(48, 50, 56, 248) : IM_COL32(24, 25, 28, 240), seed_of(ra), k);
        if (lit && nav().keys) chosen_outline(d, ra, rb, k);
        text(d, f.bold, 17 * k, ImVec2(ra.x + 18 * k, ra.y + 12 * k), white, elide(f.bold, 17 * k, g.label, rb.x - ra.x - 80 * k));
        const auto count = std::to_string(g.count);
        const auto t = text_size(f.bold, 14 * k, count.c_str());
        text(d, f.bold, 14 * k, ImVec2(rb.x - t.x - 16 * k, ra.y + 14 * k), chosen ? white : soft, count);
    }
    end_region(d, ga, gb, y + u.groups_scroll - ga.y, u.groups_scroll, k);

    // ---- the right: the kind's cards ----
    const ImVec2 ra(a.x + col_w + 36 * k, a.y), rb = b;
    const bool searching = !u.asset_filter.empty() && u.trick_group != "*assets";
    const auto terms = studio::native_search_terms(u.asset_filter);
    std::string title, about;
    if (searching) title = "RESULTS", about = "Tricks and your saved ones whose name has every word you typed.";
    else if (u.trick_group == "*mine")
        title = "MY TRICKS", about = v.library.empty() ? "Nothing saved yet: open one of the game's tricks, change it and save it."
                                                       : "Everything you saved. EDIT opens it, TEST IN GAME lets you skate it, the card holds the rest.";
    else if (u.trick_group == "*assets") title = "GAME ANIMATIONS", about = "Every piece of the game's animation: setups, loops, exits, grind phases.";
    else {
        title = upper(u.trick_group);
        const bool pieces = std::any_of(cards.begin(), cards.end(), [&](const TrickCard& c) { return c.category == u.trick_group && !c.pieces.empty(); });
        about = pieces ? "The game's own pieces of each trick. A chip opens that way of doing it; the card shows every piece (into, loop, out...)."
                       : "Pick a pop to open it in the editor. The card opens its mid pop.";
    }
    page_heading(d, f, ra, title, about);
    {
        const char* label = v.hover_preview ? "PREVIEW ON" : "PREVIEW OFF";
        const float pw = chip_width(f, label);
        if (chip(d, f, "##hoverpreview", ImVec2(rb.x - pw - 14 * k, ra.y + 8 * k), label, v.hover_preview))
            studio::set_hover_preview(!v.hover_preview);
    }
    ImVec2 area_a(ra.x, ra.y + 84 * k);
    area_a.y += library_panel(d, f, area_a, rb);
    if (u.trick_group == "*assets" && !searching) {
        asset_list(d, f, v, area_a, rb);
        return;
    }
    // My tricks that are shown (all of them, or those the search finds), then the game's.
    std::vector<std::string> mine;
    if (u.trick_group == "*mine" || searching)
        for (const auto& name : v.library) {
            if (searching) {
                const auto* p = preset_named(v, name);
                const auto key = studio::native_search_key(shown_name(v, name) + " " + name + " " + (p ? p->trick + " " + trick_label(p->trick) : std::string{}));
                if (!studio::native_search_matches(key, terms)) continue;
            }
            mine.push_back(name);
        }
    std::vector<const TrickCard*> game;
    if (u.trick_group != "*mine" || searching)
        for (const auto& c : cards) {
            if (searching) {
                if (!studio::native_search_matches(c.search, terms)) continue;
            } else if (c.category != u.trick_group) continue;
            game.push_back(&c);
        }
    // The side panel: one of your tricks, or every piece of one of the game's grinds, grabs...
    const TrickCard* pieces_open = nullptr;
    if (!u.piece_card.empty() && u.trick_group != "*mine")
        for (const auto* c : game)
            if (c->trick == u.piece_card) pieces_open = c;
    const bool drawer = (!u.trick_page.empty() && (u.trick_group == "*mine" || searching)) || pieces_open;
    const float drawer_w = 470 * k;
    const ImVec2 grid_a = area_a, grid_b(drawer ? rb.x - drawer_w - 24 * k : rb.x, rb.y);
    const float gap = 16 * k, min_w = 300 * k, card_h = 132 * k;
    const float grid_w = grid_b.x - grid_a.x - 14 * k;
    const int columns = std::max(1, static_cast<int>((grid_w + gap) / (min_w + gap)));
    const float card_w = (grid_w - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    begin_region(22, grid_a, grid_b, u.cards_scroll, k);
    float top = grid_a.y - u.cards_scroll;
    int i = 0;
    const auto place = [&](int index) {
        return ImVec2(grid_a.x + static_cast<float>(index % columns) * (card_w + gap), top + static_cast<float>(index / columns) * (card_h + gap));
    };
    if (searching && !mine.empty()) {
        heading(d, f, ImVec2(grid_a.x, top), grid_w, "Yours", std::to_string(mine.size()));
        top += 34 * k;
    }
    u.grid_a = grid_a;
    u.grid_b = grid_b;
    for (const auto& name : mine) {
        const auto at = place(i++);
        if (under_peek(at, ImVec2(at.x + card_w, at.y + card_h))) continue;
        my_trick_card(d, f, v, name, at, ImVec2(card_w, card_h));
        note_hover("mine|" + name, at, ImVec2(at.x + card_w, at.y + card_h), [name] { return studio::peek_saved(name); }, shown_name(v, name));
    }
    if (!mine.empty()) {
        top += static_cast<float>((i + columns - 1) / columns) * (card_h + gap) + 10 * k;
        i = 0;
    }
    if (searching && !game.empty()) {
        heading(d, f, ImVec2(grid_a.x, top), grid_w, "The game's", std::to_string(game.size()));
        top += 34 * k;
    }
    for (const auto* c : game) {
        const auto at = place(i);
        const ImVec2 end(at.x + card_w, at.y + card_h);
        if (under_peek(at, end)) {
            ++i;
            continue;
        }
        if (c->pieces.empty()) {
            game_trick_card(d, f, v, *c, place(i++), ImVec2(card_w, card_h), saved_of.contains(c->trick) ? saved_of[c->trick] : 0);
            // Its mid pop, or the one it has.
            for (const std::size_t pop : {1, 0, 2, 3})
                if (c->pops[pop]) {
                    const studio::GameTrick trick = *c->pops[pop];
                    note_hover("game|" + trick.file, at, end, [trick] { return studio::peek_game_trick(trick); }, c->label + " (" + trick.pop + " pop)");
                    break;
                }
            continue;
        }
        if (!c->sides.empty())
            if (const auto* piece = main_piece(*c, c->sides.front()); piece && piece->clip) {
                const studio::GameTrick trick = *piece->clip;
                note_hover("piece|" + trick.file, at, end, [trick] { return studio::peek_game_trick(trick); },
                           c->label + " (" + (piece->side.empty() ? piece->part : piece->side + " " + piece->part) + ")");
            }
        int saved = 0;
        for (const auto& piece : c->pieces)
            if (const auto found = saved_of.find(piece_trick(piece)); found != saved_of.end()) saved += found->second;
        piece_card(d, f, v, *c, place(i++), ImVec2(card_w, card_h), saved);
    }
    top += static_cast<float>((i + columns - 1) / columns) * (card_h + gap);
    if (mine.empty() && game.empty())
        text(d, f.bold, 17 * k, ImVec2(grid_a.x, grid_a.y + 10 * k), soft,
             searching ? "Nothing of that name. Try fewer letters, or another word for it." : "Nothing here yet.");
    end_region(d, grid_a, grid_b, top + u.cards_scroll - grid_a.y, u.cards_scroll, k);
    if (pieces_open && (u.trick_page.empty() || !(u.trick_group == "*mine" || searching)))
        piece_drawer(d, f, v, *pieces_open, ImVec2(rb.x - drawer_w, area_a.y), rb);
    else if (drawer) trick_drawer(d, f, v, ImVec2(rb.x - drawer_w, area_a.y), rb);
    hover_preview(f, v);
}

// ---- PACKS ----------------------------------------------------------------------------------------
// MY PACKS along the top as a row of covers to scroll through, any number of them on at once; the chosen one under
// them: its name and picture, its rules, and its tricks sorted by kind.
std::filesystem::path from_utf8(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
std::string to_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
void open_browser(const studio::View& v, int mode, const std::string& pack);
void open_cover_designer(const studio::View& v, const std::string& pack); // below
void preview_cover(const CoverDesign& design);                             // below
// A pack's own colour, for its drawn cover: the same pack always gets the same one.
ImU32 accent_of(const std::string& name) {
    static constexpr std::array<ImU32, 6> colours{IM_COL32(1, 131, 255, 255), IM_COL32(255, 140, 0, 255), IM_COL32(46, 184, 92, 255),
                                                  IM_COL32(170, 92, 255, 255), IM_COL32(235, 70, 80, 255), IM_COL32(240, 200, 40, 255)};
    unsigned hash = 2166136261u;
    for (const char c : name) hash = (hash ^ static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)))) * 16777619u;
    return colours[hash % colours.size()];
}
// Its picture, or a cover drawn for it: its initials on a strip of tape in its colour.
// A pack's package on Thunderstore while it is up there: the one it was published as, else one of the linked team's named
// after it (a pack uploaded by hand). nullptr when it is not up, or the listing does not have it yet.
const studio::market::Pack* live_package(const studio::market::View& m, const studio::View::PackView& pack) {
    for (const auto& p : m.packs)
        if (!pack.pack.published_as.empty() ? _stricmp(p.full_name.c_str(), pack.pack.published_as.c_str()) == 0
                                             : p.mine && p.name == studio::market::package_name(pack.name))
            return &p;
    return nullptr;
}
// Whether the pack is on Thunderstore already: SHARE is UPDATE then.
bool pack_is_published(const studio::market::View& m, const studio::View::PackView& pack) {
    return !pack.pack.published.empty() || !pack.pack.published_as.empty() || live_package(m, pack) != nullptr;
}
// The version an update goes up as: one past the highest out, published from here or up on Thunderstore.
std::string next_update_version(const studio::market::View& m, const studio::View::PackView& pack) {
    std::string last = pack.pack.published;
    if (const auto* live = live_package(m, pack); live && studio::market::version_after(live->version, last)) last = live->version;
    return last.empty() ? pack.pack.version : studio::market::next_version(last, last);
}
void pack_cover(ImDrawList* d, const Fonts& f, const studio::View::PackView& pack, ImVec2 a, ImVec2 b) {
    ImTextureID texture{};
    ImVec2 uv0{}, uv1{};
    if (!pack.thumbnail.empty() && detail::studio_pack_picture(pack.name + "|" + std::to_string(pack.thumbnail_stamp), texture, uv0, uv1)) {
        d->AddImage(texture, a, b, uv0, uv1);
        return;
    }
    const float w = b.x - a.x, h = b.y - a.y;
    d->AddRectFilled(a, b, IM_COL32(22, 22, 25, 255));
    draw_studio_picture(d, StudioPicture::grain, a, b, IM_COL32(255, 255, 255, 40));
    const ImU32 accent = accent_of(pack.name);
    d->AddQuadFilled(ImVec2(a.x, a.y + h * 0.56f), ImVec2(b.x, a.y + h * 0.32f), ImVec2(b.x, a.y + h * 0.54f), ImVec2(a.x, a.y + h * 0.78f), accent);
    theme::halftone(d, ImVec2(a.x + 12 * f.k, a.y + 12 * f.k), 6, 6, IM_COL32(255, 255, 255, 40));
    std::string initials;
    bool next = true;
    for (const char c : pack.name) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '_' || c == '-') next = true;
        else if (next && initials.size() < 2) {
            initials += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            next = false;
        }
    }
    if (initials.empty()) initials = "?";
    const float fs = h * 0.40f;
    const auto t = text_size(f.title, fs, initials.c_str());
    const ImVec2 at(a.x + (w - t.x) * 0.5f, a.y + (h - t.y) * 0.42f);
    text(d, f.title, fs, ImVec2(at.x + 4 * f.k, at.y + 5 * f.k), IM_COL32(0, 0, 0, 150), initials);
    text(d, f.title, fs, at, white, initials);
}
void want_pack_pictures(const studio::View& v) {
    std::vector<std::pair<std::string, std::string>> wanted;
    for (const auto& pack : v.packs)
        if (!pack.thumbnail.empty()) wanted.emplace_back(pack.name + "|" + std::to_string(pack.thumbnail_stamp), pack.thumbnail);
    detail::want_studio_pack_pictures(wanted);
}
std::size_t saved_count(const studio::View& v, const studio::View::PackView& pack) {
    std::size_t count = 0;
    for (const auto& member : pack.members) count += saved_trick(v, member);
    return count;
}
void make_pack(const studio::View& v) {
    std::string m;
    studio::create_preset_pack(m, unique_pack_name(v));
    say(m);
    ui().pack_name_for.clear();
}
void open_pack_page(const std::string& name) {
    auto& u = ui();
    studio::open_pack(name);
    u.pack_page = true;
    u.pack_tricks_scroll = 0;
    u.pack_name_for.clear();
    u.confirm_delete.clear();
}
// The tricks in a pack, by kind; where it has more than one of a trick and pop, which plays (FIXED) or that they take
// turns (CYCLE).
float pack_tricks(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::View::PackView& pack, ImVec2 a, float width) {
    const float k = f.k;
    float y = a.y;
    struct Slot {
        std::string slot, trick, pop;
        std::vector<std::string> members;
    };
    std::vector<std::pair<std::string, std::vector<Slot>>> kinds; // in the order they turn up
    for (const auto& [slot, names] : pack.pack.slots) {
        Slot s{slot, {}, {}, {}};
        for (const auto& name : names) if (saved_trick(v, name)) s.members.push_back(name);
        if (s.members.empty()) continue;
        if (const auto* p = preset_named(v, s.members.front())) s.trick = p->trick, s.pop = p->pop;
        const auto kind = kind_of(s.trick);
        auto found = std::find_if(kinds.begin(), kinds.end(), [&](const auto& e) { return e.first == kind; });
        if (found == kinds.end()) {
            kinds.emplace_back(kind, std::vector<Slot>{});
            found = kinds.end() - 1;
        }
        found->second.push_back(std::move(s));
    }
    if (kinds.empty()) {
        y += paragraph(d, f.bold, 15 * k, ImVec2(a.x, y), width, soft, "No tricks in it yet. ADD TRICKS puts your saved tricks in it.");
        return y - a.y;
    }
    // At the end of each row: watch it, switch it off in this pack (it stays in it), take it out.
    const auto out_button = [&](const std::string& member, float row_y, float row_h) {
        ImGui::PushID(member.c_str());
        const float by = row_y + (row_h - 30 * k) * 0.5f;
        if (action(d, f, "##out", ImVec2(a.x + width - 70 * k, by), ImVec2(62 * k, 30 * k), "OUT", Look::ghost)) {
            std::string m;
            studio::set_pack_member(pack.name, member, false, m);
            say(m);
        }
        const bool on = !pack.pack.off.contains(member);
        if (toggle(d, f, "##trickon", ImVec2(a.x + width - 136 * k, by + 2 * k), on, 0.8f)) {
            std::string m;
            studio::set_pack_trick_on(pack.name, member, !on, m);
            say(m);
        }
        if (action(d, f, "##preview", ImVec2(a.x + width - 236 * k, by), ImVec2(92 * k, 30 * k), "PREVIEW", Look::dark))
            preview_trick(pack.name, member, true);
        ImGui::PopID();
    };
    // Under each trick: its stance versions in this pack, ticked, and the missing ones to make.
    const auto stances_line = [&](const Slot& s, float& row_y) {
        if (s.trick.starts_with("native:")) return;
        ImGui::PushID("stances");
        row_y += stance_versions(d, f, v, s.members.front(), ImVec2(a.x + 16 * k, row_y), width - 40 * k, s.members, pack.name) + 4 * k;
        ImGui::PopID();
    };
    const std::string forced = pack.pack.pop_rule;
    for (const auto& [kind, slots] : kinds) {
        std::size_t tricks = 0;
        for (const auto& s : slots) tricks += s.members.size();
        heading(d, f, ImVec2(a.x, y), width - 8 * k, upper(kind), std::to_string(tricks));
        y += 34 * k;
        for (const auto& s : slots) {
            ImGui::PushID(s.slot.c_str());
            const std::string pop_label = forced.empty() ? pop_words(s.pop) : forced == "any" ? "ANY POP (RULE)" : upper(forced) + " (RULE)";
            if (s.members.size() == 1) {
                const float row_h = 46 * k;
                const auto& member = s.members.front();
                bool lit = false;
                if (hit("##one", ImVec2(a.x, y), ImVec2(a.x + width - 246 * k, y + row_h), &lit)) open_or_preview(v, member, pack.name);
                skate_theme::rough_rect(d, ImVec2(a.x, y), ImVec2(a.x + width - 8 * k, y + row_h), lit ? IM_COL32(48, 50, 56, 248) : IM_COL32(26, 27, 30, 240),
                                        seed_of(ImVec2(a.x, y)), k * 0.7f);
                if (lit && nav().keys) chosen_outline(d, ImVec2(a.x, y), ImVec2(a.x + width - 80 * k, y + row_h), k);
                float right = a.x + width - 250 * k;
                const bool member_on = !pack.pack.off.contains(member);
                if (const auto* p = preset_named(v, member)) trick_tags(d, f, *p, forced, right, y + 13 * k);
                text(d, f.bold, 17 * k, ImVec2(a.x + 16 * k, y + 12 * k), member_on ? white : faint,
                     elide(f.bold, 17 * k, shown_name(v, member) + (member_on ? "" : "  (OFF)"), right - a.x - 30 * k));
                out_button(member, y, row_h);
                y += row_h + 4 * k;
                stances_line(s, y);
                y += 4 * k;
                ImGui::PopID();
                continue;
            }
            // More than one of this trick and pop.
            const bool cycle = pack.pack.alternates(s.slot);
            const float head_h = 44 * k;
            skate_theme::rough_rect(d, ImVec2(a.x, y), ImVec2(a.x + width - 8 * k, y + head_h), IM_COL32(32, 34, 40, 246), seed_of(ImVec2(a.x, y)), k * 0.7f);
            text(d, f.bold, 16 * k, ImVec2(a.x + 16 * k, y + 12 * k), white,
                 elide(f.bold, 16 * k, upper(trick_label(s.trick)) + "  /  " + pop_label + "  x" + std::to_string(s.members.size()), width - 260 * k));
            const int mode = segmented(d, f, "##mode", ImVec2(a.x + width - 230 * k, y + 6 * k), 218 * k, 32 * k, {"FIXED", "CYCLE"}, cycle ? 1 : 0);
            if (mode >= 0 && (mode == 1) != cycle) {
                std::string m;
                studio::set_pack_mode(pack.name, s.slot, mode == 1, m);
                say(m);
            }
            y += head_h + 4 * k;
            for (std::size_t i = 0; i < s.members.size(); ++i) {
                const auto& member = s.members[i];
                const float row_h = 40 * k;
                ImGui::PushID(member.c_str());
                const ImVec2 ra(a.x + 26 * k, y), rb(a.x + width - 246 * k, y + row_h);
                bool lit = false;
                const bool in_cycle = !pack.pack.unlinked.contains(member);
                const bool plays = cycle ? in_cycle : i == 0;
                if (hit("##member", ra, rb, &lit)) {
                    std::string m;
                    if (cycle) studio::set_pack_linked(pack.name, member, !in_cycle, m);
                    else studio::set_pack_fixed(pack.name, member, m);
                    say(m);
                }
                skate_theme::rough_rect(d, ra, ImVec2(a.x + width - 8 * k, rb.y), lit ? IM_COL32(48, 50, 56, 248) : IM_COL32(22, 23, 26, 240), seed_of(ra), k * 0.6f);
                if (lit && nav().keys) chosen_outline(d, ra, rb, k);
                // A dot for the one that plays (FIXED), a tick box for the ones in the cycle (CYCLE).
                const ImVec2 c(ra.x + 18 * k, ra.y + row_h * 0.5f);
                if (cycle) {
                    d->AddRect(ImVec2(c.x - 8 * k, c.y - 8 * k), ImVec2(c.x + 8 * k, c.y + 8 * k), plays ? blue_light : soft, 2 * k, 0, 2 * k);
                    if (plays) {
                        d->AddLine(ImVec2(c.x - 5 * k, c.y), ImVec2(c.x - 1 * k, c.y + 4 * k), blue_light, 2.4f * k);
                        d->AddLine(ImVec2(c.x - 1 * k, c.y + 4 * k), ImVec2(c.x + 6 * k, c.y - 5 * k), blue_light, 2.4f * k);
                    }
                } else {
                    d->AddCircle(c, 8 * k, plays ? blue_light : soft, 20, 2 * k);
                    if (plays) d->AddCircleFilled(c, 4.5f * k, blue_light, 16);
                }
                const char* state = cycle ? (in_cycle ? "IN THE CYCLE" : "LEFT OUT") : (i == 0 ? "PLAYS" : "");
                float right = rb.x - 12 * k;
                if (*state) {
                    const auto t = text_size(f.bold, 12 * k, state);
                    right -= t.x;
                    text(d, f.bold, 12 * k, ImVec2(right, ra.y + 13 * k), plays ? blue_light : faint, state);
                    right -= 14 * k;
                }
                if (const auto* p = preset_named(v, member)) trick_tags(d, f, *p, forced, right, ra.y + 10 * k);
                text(d, f.bold, 15 * k, ImVec2(ra.x + 38 * k, ra.y + 11 * k), plays ? white : soft,
                     elide(f.bold, 15 * k, shown_name(v, member), right - ra.x - 50 * k));
                out_button(member, y, row_h);
                ImGui::PopID();
                y += row_h + 4 * k;
            }
            stances_line(s, y);
            y += 6 * k;
            ImGui::PopID();
        }
        y += 8 * k;
    }
    return y - a.y;
}
// What duplicates of this pack's tricks the other packs that are on have, and what plays them.
float pack_duplicates(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::View::PackView& pack, ImVec2 at, float w) {
    const float k = f.k;
    std::vector<const studio::PackConflict*> mine;
    for (const auto& c : v.conflicts)
        if (std::any_of(c.packs.begin(), c.packs.end(), [&](const auto& e) { return e.first == pack.name; })) mine.push_back(&c);
    if (mine.empty()) return 0;
    float y = at.y;
    heading(d, f, ImVec2(at.x, y), w, "Also in other packs that are on", std::to_string(mine.size()));
    y += 34 * k;
    for (const auto* c : mine) {
        ImGui::PushID(c->slot.c_str());
        text(d, f.bold, 15 * k, ImVec2(at.x, y + 7 * k), white, elide(f.bold, 15 * k, upper(trick_label(c->trick)) + "  /  " + pop_words(c->pop), 300 * k));
        float cx = at.x + 320 * k;
        const bool random = c->choice.empty() || c->choice == "*";
        float cw = 0;
        if (chip(d, f, "##random", ImVec2(cx, y), "TAKE TURNS", random, &cw)) {
            std::string m;
            studio::set_conflict_choice(c->slot, "*", m);
            say(m);
        }
        cx += cw + 6 * k;
        for (const auto& [other, names] : c->packs) {
            const float ow = chip_width(f, other);
            if (cx + ow > at.x + w) cx = at.x + 320 * k, y += 34 * k;
            ImGui::PushID(other.c_str());
            if (chip(d, f, "##winner", ImVec2(cx, y), other, c->choice == other)) {
                std::string m;
                studio::set_conflict_choice(c->slot, other, m);
                say(m);
            }
            ImGui::PopID();
            cx += ow + 6 * k;
        }
        y += 40 * k;
        ImGui::PopID();
    }
    return y + 10 * k - at.y;
}
// One pack's own page: its cover and what can be done with it on the left; its name, its rules and its tricks on the
// right, each trick with the pop and stance it plays on.
void pack_page(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::View::PackView& pack, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    const float k = f.k;
    if (action(d, f, "##backtopacks", a, ImVec2(190 * k, 36 * k), "< MY PACKS", Look::ghost)) {
        u.pack_page = false;
        return;
    }
    const float col = std::min(380 * k, (b.y - a.y) * 0.42f);
    // ---- the pack ----
    {
        float y = a.y + 54 * k;
        const ImVec2 ca(a.x, y), cb(a.x + col, y + col);
        if (pack.on) d->AddRect(ImVec2(ca.x - 5 * k, ca.y - 5 * k), ImVec2(cb.x + 5 * k, cb.y + 5 * k), IM_COL32(1, 131, 255, 140), 4 * k, 0, 6 * k);
        pack_cover(d, f, pack, ca, cb);
        if (pack.on) tag(d, f, ImVec2(ca.x + 12 * k, ca.y + 12 * k), "ON", blue);
        y = cb.y + 18 * k;
        const float x = a.x, w = col;
        if (u.pack_name_for != pack.name && !ImGui::IsAnyItemActive()) {
            std::snprintf(u.pack_name.data(), u.pack_name.size(), "%s", pack.name.c_str());
            u.pack_name_for = pack.name;
        }
        bool edited = false;
        const bool entered = field(f, "##packname", u.pack_name, ImVec2(x, y), w - 106 * k, "Pack name", &edited);
        const bool changed = std::string(u.pack_name.data()) != pack.name;
        if ((action(d, f, "##rename", ImVec2(x + w - 96 * k, y), ImVec2(96 * k, 34 * k), "RENAME", Look::dark, changed) || entered || edited) && changed) {
            std::string m;
            if (studio::rename_pack(pack.name, u.pack_name.data(), m)) u.pack_name_for.clear();
            say(m);
        }
        y += 48 * k;
        text(d, f.bold, 16 * k, ImVec2(x, y + 6 * k), white, "PLAYS IN GAME");
        if (toggle(d, f, "##detailon", ImVec2(x + w - 64 * k, y), pack.on)) {
            std::string m;
            studio::set_pack_on(pack.name, !pack.on, m);
            say(m);
        }
        y += 44 * k;
        if (!pack.installed) {
            text(d, f.bold, 16 * k, ImVec2(x, y + 6 * k), white, "OTHERS CAN EDIT");
            if (toggle(d, f, "##editable", ImVec2(x + w - 64 * k, y), pack.pack.editable)) {
                std::string m;
                studio::set_pack_editable(pack.name, !pack.pack.editable, m);
                say(m);
            }
            y += 34 * k;
            y += paragraph(d, f.bold, 12 * k, ImVec2(x, y), w, faint,
                           pack.pack.editable ? "Shared, its tricks can be opened, changed and copied by whoever installs it."
                                              : "Shared, its tricks play and preview for others but cannot be opened, copied or shared again.") + 10 * k;
        } else {
            text(d, f.bold, 14 * k, ImVec2(x, y + 6 * k), pack.pack.editable ? blue_light : skate_theme::warning,
                 pack.pack.editable ? "ITS MAKER LETS YOU EDIT IT" : "LOCKED BY ITS MAKER: PLAY AND PREVIEW");
            y += 36 * k;
        }
        const bool making = detail::studio_pack_picture_making();
        const float half = (w - 8 * k) / 2;
        if (action(d, f, "##picture", ImVec2(x, y), ImVec2(pack.thumbnail.empty() ? w : half, 38 * k),
                   making ? "MAKING IT..." : pack.thumbnail.empty() ? "MAKE A COVER" : "NEW COVER", Look::dark, !making))
            open_cover_designer(v, pack.name);
        if (!pack.thumbnail.empty() && action(d, f, "##nopicture", ImVec2(x + half + 8 * k, y), ImVec2(half, 38 * k), "NO PICTURE", Look::ghost)) {
            std::string m;
            studio::set_pack_thumbnail(pack.name, "", m);
            say(m);
        }
        y += 46 * k;
        const bool published = pack_is_published(studio::market::view(), pack);
        if (action(d, f, "##export", ImVec2(x, y), ImVec2(half, 38 * k), published ? "UPDATE" : "SHARE", published ? Look::blue : Look::paper)) {
            u.share_pack = pack.name;
            u.share_description_for.clear();
            u.share_version_for.clear();
        }
        const std::string confirm = "pack:" + pack.name;
        if (action(d, f, "##deletepack", ImVec2(x + half + 8 * k, y), ImVec2(half, 38 * k), u.confirm_delete == confirm ? "AGAIN TO DELETE" : "DELETE", Look::danger)) {
            if (u.confirm_delete != confirm) u.confirm_delete = confirm;
            else {
                std::string m;
                studio::delete_pack(pack.name, m);
                say(m);
                u.confirm_delete.clear();
                u.pack_page = false;
            }
        }
        y += 46 * k;
        paragraph(d, f.bold, 12 * k, ImVec2(x, y), w, faint, "Deleting a pack keeps its tricks: they stay in MY TRICKS.");
    }
    // ---- its name, rules and tricks ----
    const float x = a.x + col + 48 * k, w = b.x - x;
    float y = a.y + 42 * k;
    text(d, f.title, 54 * k, ImVec2(x, y), white, elide(f.title, 54 * k, upper(pack.name), w));
    y += 60 * k;
    const auto count = saved_count(v, pack);
    text(d, f.bold, 15 * k, ImVec2(x + 2 * k, y), soft,
         std::to_string(count) + (count == 1 ? " trick" : " tricks") + (pack.on ? ", on: they play in game" : ", off") +
             (pack.made_by.empty() ? std::string{} : ".   Made by " + pack.made_by));
    y += 40 * k;
    {
        text(d, f.heading, 15 * k, ImVec2(x, y + 10 * k), white, "EVERY TRICK PLAYS ON");
        static constexpr std::array<const char*, 5> rules{"", "any", "low", "mid", "high"};
        int chosen = 0;
        for (std::size_t i = 0; i < rules.size(); ++i)
            if (pack.pack.pop_rule == rules[i]) chosen = static_cast<int>(i);
        const float sx = x + 240 * k, sw = std::min(560 * k, w - 240 * k);
        const int picked = segmented(d, f, "##poprule", ImVec2(sx, y), sw, 38 * k, {"OWN POP", "ANY", "LOW", "MID", "HIGH"}, chosen);
        if (picked >= 0 && picked != chosen) {
            std::string m;
            studio::set_pack_pop_rule(pack.name, rules[static_cast<std::size_t>(picked)], m);
            say(m);
        }
        const char* about = chosen == 0 ? "Each trick plays on the pop it was saved for (its tag below)."
                            : chosen == 1 ? "Every trick in the pack plays on any pop: low, mid or high."
                                          : "Every trick in the pack plays on that pop only; the game's own plays on the others.";
        text(d, f.bold, 13 * k, ImVec2(sx + sw + 18 * k, y + 11 * k), faint, elide(f.bold, 13 * k, about, x + w - (sx + sw + 18 * k)));
        y += 50 * k;
        // What the pops are, so a pack's tags can be read as what they will look like in game.
        const ImVec2 ha(x, y), hb(x + w, y + 92 * k);
        skate_theme::rough_rect(d, ha, hb, IM_COL32(18, 22, 30, 236), seed_of(ha), k * 0.6f);
        d->AddRectFilled(ha, ImVec2(ha.x + 5 * k, hb.y), blue);
        static constexpr std::array<std::pair<const char*, const char*>, 4> pops{{
            {"LOW", "a quick, short flick: a little hop, about a third of a second in the air. Tricks look tight and fast."},
            {"MID", "an ordinary flick: the height most tricks are done at, and what most tricks are made on."},
            {"HIGH", "a long, hard flick: a big pop that floats, more air time, so the trick has time to stretch out."},
            {"ANY", "plays on every pop the game does that has no trick of its own for that height in your packs."},
        }};
        float py = ha.y + 8 * k;
        text(d, f.heading, 12 * k, ImVec2(ha.x + 18 * k, py), blue_light, "POP HEIGHT: HOW HARD YOU POP IN GAME (THE GAME GRADES EVERY POP)");
        py += 20 * k;
        const float half_w = (w - 40 * k) / 2;
        for (std::size_t i = 0; i < pops.size(); ++i) {
            const float px = ha.x + 18 * k + static_cast<float>(i % 2) * (half_w + 10 * k), row = py + static_cast<float>(i / 2) * 32 * k;
            text(d, f.bold, 12 * k, ImVec2(px, row), white, pops[i].first);
            paragraph(d, f.bold, 11 * k, ImVec2(px + 42 * k, row), half_w - 46 * k, IM_COL32(200, 204, 212, 255), pops[i].second);
        }
        y += 100 * k;
    }
    y += pack_duplicates(d, f, v, pack, ImVec2(x, y), w);
    heading(d, f, ImVec2(x, y + 10 * k), w - 230 * k, "Tricks in this pack", std::to_string(count));
    if (action(d, f, "##addtricks", ImVec2(x + w - 210 * k, y), ImVec2(210 * k, 40 * k), "+ ADD TRICKS", Look::blue)) {
        u.pack_adding = true;
        u.adding_scroll = 0;
    }
    y += 46 * k;
    text(d, f.bold, 13 * k, ImVec2(x, y), faint,
         v.front_foot ? "To play one: do its trick with its pop. Trick matching is on Front foot, so it plays in any stance, mirrored when the other foot leads."
                      : "To play one: do its trick with its pop, in the stance on its tags (Trick matching is on Exact stance).");
    y += 28 * k;
    const ImVec2 la(x, y), lb(b.x, b.y);
    begin_region(51, la, lb, u.pack_tricks_scroll, k);
    const float h = pack_tricks(d, f, v, pack, ImVec2(x, la.y - u.pack_tricks_scroll), w - 14 * k);
    end_region(d, la, lb, h, u.pack_tricks_scroll, k);
}
// MY PACKS: every pack as a cover with its name and switch, NEW PACK at the end. Pressing one opens its page.
void packs_hub(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    const float k = f.k;
    std::string on;
    std::size_t on_count = 0;
    for (const auto& pack : v.packs)
        if (pack.on) on += (on.empty() ? "" : ", ") + pack.name, ++on_count;
    page_heading(d, f, a, "MY PACKS",
                 v.packs.empty() ? std::string("Packs group your tricks to switch on together and share. Make one, then put your tricks in it.")
                 : on_count ? std::to_string(on_count) + " on (" + on + "): their tricks play in game. Open a pack to see and change what is in it."
                            : std::string("None on: every trick you saved plays in game. Open a pack to see and change what is in it."));
    {
        float x = b.x;
        const auto button = [&](const char* id, float w, const char* label, Look look) {
            x -= w;
            const bool pressed = action(d, f, id, ImVec2(x, a.y + 6 * k), ImVec2(w, 42 * k), label, look);
            x -= 10 * k;
            return pressed;
        };
        if (button("##exportall", 220 * k, "EXPORT ALL TRICKS", Look::ghost)) {
            std::string m;
            studio::export_pack("", m);
            say(m);
        }
        if (button("##import", 140 * k, "IMPORT", Look::dark)) {
            std::string m;
            studio::import_packs(m);
            say(m);
        }
        if (button("##newpack", 170 * k, "+ NEW PACK", Look::blue)) {
            make_pack(v);
            u.pack_page = true;
        }
        if (button("##packsettings", 150 * k, "SETTINGS", Look::paper)) {
            u.pack_settings = true;
            u.pack_settings_scroll = 0;
        }
    }
    float y = a.y + 84 * k;
    // The search, and the playlist's state when it is on (it decides what plays then, not the packs that are on).
    field(f, "##packsearch", u.pack_query, ImVec2(a.x, y), std::min(420 * k, b.x - a.x), "Search packs: name, maker, a trick in it");
    if (v.playlist_on) {
        const std::string line = "PLAYLIST ON (" + std::to_string(v.playlist.size()) + "): it decides what plays in game. PACKS > SETTINGS to change it.";
        text(d, f.bold, 13 * k, ImVec2(a.x + std::min(420 * k, b.x - a.x) + 20 * k, y + 9 * k), blue_light, elide(f.bold, 13 * k, line, b.x - a.x - 460 * k));
    }
    y += 50 * k;
    if (!v.conflicts.empty()) {
        const ImVec2 ca(a.x, y), cb(b.x, y + 52 * k);
        skate_theme::rough_rect(d, ca, cb, IM_COL32(56, 38, 8, 240), seed_of(ca), k * 0.7f);
        d->AddRectFilled(ca, ImVec2(ca.x + 6 * k, cb.y), skate_theme::warning);
        const auto count = v.conflicts.size();
        text(d, f.heading, 17 * k, ImVec2(ca.x + 22 * k, ca.y + 7 * k), white,
             std::to_string(count) + (count == 1 ? " TRICK IS" : " TRICKS ARE") + " IN MORE THAN ONE PACK THAT IS ON");
        text(d, f.bold, 13 * k, ImVec2(ca.x + 22 * k, ca.y + 30 * k), IM_COL32(230, 210, 170, 255), "Choose which pack plays each, or let them take turns at random.");
        if (action(d, f, "##review", ImVec2(cb.x - 150 * k, ca.y + 9 * k), ImVec2(136 * k, 34 * k), "CHOOSE", Look::paper)) {
            u.conflicts_open = true;
            u.conflicts_scroll = 0;
        }
        y += 66 * k;
    }
    // The packs to show: those the search finds, starred ones first.
    std::vector<const studio::View::PackView*> shown;
    {
        const auto terms = studio::native_search_terms(u.pack_query.data());
        for (const auto& pack : v.packs) {
            if (!terms.empty()) {
                std::string words = pack.name + " " + pack.made_by;
                for (const auto& member : pack.members) words += " " + shown_name(v, member);
                if (!studio::native_search_matches(studio::native_search_key(words), terms)) continue;
            }
            shown.push_back(&pack);
        }
        std::stable_sort(shown.begin(), shown.end(), [](const auto* x, const auto* y) { return x->favorite && !y->favorite; });
    }
    if (shown.empty() && !v.packs.empty())
        text(d, f.bold, 16 * k, ImVec2(a.x, y + 10 * k), soft, "No pack of that name. Try fewer letters.");
    // The covers, in rows, in the middle of the page.
    const float card_w = 290 * k, info_h = 92 * k, card_h = card_w + info_h, gap = 26 * k;
    const float room = b.x - a.x - 14 * k;
    const int columns = std::max(1, static_cast<int>((room + gap) / (card_w + gap)));
    const std::size_t count = shown.size() + 1;
    const int used = static_cast<int>(std::min<std::size_t>(count, static_cast<std::size_t>(columns)));
    const float grid_w = static_cast<float>(used) * card_w + static_cast<float>(used - 1) * gap;
    const float x0 = a.x + std::max(0.0f, (room - grid_w) * 0.5f);
    const ImVec2 ga(a.x, y + 8 * k), gb = b;
    begin_region(55, ga, gb, u.hub_scroll, k);
    const float top = ga.y + 10 * k - u.hub_scroll;
    auto& n = nav();
    for (std::size_t i = 0; i < count; ++i) {
        const ImVec2 ca(x0 + static_cast<float>(i % static_cast<std::size_t>(columns)) * (card_w + gap),
                        top + static_cast<float>(i / static_cast<std::size_t>(columns)) * (card_h + gap));
        const ImVec2 cover_b(ca.x + card_w, ca.y + card_w), cb(ca.x + card_w, ca.y + card_h);
        ImGui::PushID(static_cast<int>(i));
        if (i == shown.size()) {
            bool lit = false;
            if (hit("##newpackcard", ca, cover_b, &lit)) {
                make_pack(v);
                u.pack_page = true;
            }
            const ImU32 line = lit ? white : IM_COL32(255, 255, 255, 90);
            for (float t = 0; t < 1; t += 0.05f) { // a dashed outline
                const float e = t + 0.025f;
                d->AddLine(ImVec2(ca.x + card_w * t, ca.y), ImVec2(ca.x + card_w * e, ca.y), line, 2 * k);
                d->AddLine(ImVec2(ca.x + card_w * t, cover_b.y), ImVec2(ca.x + card_w * e, cover_b.y), line, 2 * k);
                d->AddLine(ImVec2(ca.x, ca.y + card_w * t), ImVec2(ca.x, ca.y + card_w * e), line, 2 * k);
                d->AddLine(ImVec2(cover_b.x, ca.y + card_w * t), ImVec2(cover_b.x, ca.y + card_w * e), line, 2 * k);
            }
            const ImVec2 c(ca.x + card_w * 0.5f, ca.y + card_w * 0.42f);
            d->AddLine(ImVec2(c.x - 30 * k, c.y), ImVec2(c.x + 30 * k, c.y), line, 5 * k);
            d->AddLine(ImVec2(c.x, c.y - 30 * k), ImVec2(c.x, c.y + 30 * k), line, 5 * k);
            const auto t = text_size(f.heading, 20 * k, "NEW PACK");
            text(d, f.heading, 20 * k, ImVec2(c.x - t.x * 0.5f, c.y + 48 * k), line, "NEW PACK");
            if (lit && n.keys) chosen_outline(d, ca, cover_b, k);
            ImGui::PopID();
            continue;
        }
        const auto& pack = *shown[i];
        ImGui::PushID(pack.name.c_str());
        bool lit = false, info_lit = false;
        // The star: pressed, the pack is starred and comes first.
        const ImVec2 star_a(cover_b.x - 46 * k, ca.y + 8 * k), star_b(cover_b.x - 8 * k, ca.y + 46 * k);
        bool star_lit = false;
        if (hit("##star", star_a, star_b, &star_lit)) studio::set_pack_favorite(pack.name, !pack.favorite);
        bool open = hit("##hubcover", ca, cover_b, &lit);
        open |= hit("##hubinfo", ImVec2(ca.x, cover_b.y), ImVec2(cb.x - 84 * k, cb.y), &info_lit);
        lit |= info_lit;
        if (pack.on) d->AddRect(ImVec2(ca.x - 5 * k, ca.y - 5 * k), ImVec2(cover_b.x + 5 * k, cover_b.y + 5 * k), IM_COL32(1, 131, 255, 140), 4 * k, 0, 6 * k);
        pack_cover(d, f, pack, ca, cover_b);
        if (!lit) d->AddRectFilled(ca, cover_b, IM_COL32(0, 0, 0, 50));
        if (pack.on) tag(d, f, ImVec2(ca.x + 12 * k, ca.y + 12 * k), "ON", blue);
        if (pack.installed) tag(d, f, ImVec2(ca.x + 12 * k, ca.y + (pack.on ? 38 : 12) * k), "INSTALLED", IM_COL32(0, 0, 0, 170));
        {
            // A five-pointed star, filled when starred.
            const ImVec2 c((star_a.x + star_b.x) * 0.5f, (star_a.y + star_b.y) * 0.5f);
            std::array<ImVec2, 10> points{};
            for (int s = 0; s < 10; ++s) {
                const float r = (s % 2 ? 7.0f : 16.0f) * k, angle = -1.5707963f + static_cast<float>(s) * 0.6283185f;
                points[static_cast<std::size_t>(s)] = ImVec2(c.x + std::cos(angle) * r, c.y + std::sin(angle) * r);
            }
            d->AddCircleFilled(c, 19 * k, IM_COL32(0, 0, 0, star_lit ? 200 : 140));
            if (pack.favorite) d->AddConcavePolyFilled(points.data(), 10, IM_COL32(255, 196, 0, 255));
            d->AddPolyline(points.data(), 10, pack.favorite ? IM_COL32(255, 220, 90, 255) : IM_COL32(255, 255, 255, star_lit ? 255 : 170), ImDrawFlags_Closed, 2 * k);
        }
        skate_theme::rough_rect(d, ImVec2(ca.x, cover_b.y), cb, lit ? IM_COL32(236, 236, 234, 252) : IM_COL32(22, 23, 26, 248), seed_of(ca), k * 0.6f);
        const ImU32 ink = lit ? IM_COL32(18, 18, 20, 255) : white, ink_soft = lit ? IM_COL32(80, 80, 86, 255) : soft;
        text(d, f.bold, 19 * k, ImVec2(ca.x + 14 * k, cover_b.y + 12 * k), ink, elide(f.bold, 19 * k, pack.name, card_w - 100 * k));
        const auto tricks = saved_count(v, pack);
        const std::string rule = pack.pack.pop_rule.empty() ? "" : pack.pack.pop_rule == "any" ? "  /  ANY POP" : "  /  " + upper(pack.pack.pop_rule) + " POP";
        text(d, f.bold, 12 * k, ImVec2(ca.x + 14 * k, cover_b.y + 40 * k), ink_soft, std::to_string(tricks) + (tricks == 1 ? " TRICK" : " TRICKS") + rule);
        if (!pack.made_by.empty())
            text(d, f.bold, 12 * k, ImVec2(ca.x + 14 * k, cover_b.y + 60 * k), ink_soft, elide(f.bold, 12 * k, "BY " + upper(pack.made_by), card_w - 110 * k));
        if (toggle(d, f, "##packon", ImVec2(cb.x - 76 * k, cover_b.y + 30 * k), pack.on)) {
            std::string m;
            studio::set_pack_on(pack.name, !pack.on, m);
            say(m);
        }
        if (lit && n.keys) chosen_outline(d, ca, cb, k);
        if (open) open_pack_page(pack.name);
        ImGui::PopID();
        ImGui::PopID();
    }
    const float rows = static_cast<float>((count + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
    end_region(d, ga, gb, rows * (card_h + gap) + 20 * k, u.hub_scroll, k);
}
// PACKS > SETTINGS: the playlist - whole packs and single tricks, in order, and how the same trick from more than one of
// them takes turns. On, it decides what plays in game; off, the packs that are on do.
void pack_settings_page(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    const float k = f.k;
    if (action(d, f, "##backfromsettings", a, ImVec2(190 * k, 36 * k), "< MY PACKS", Look::ghost)) {
        u.pack_settings = false;
        return;
    }
    text(d, f.title, 46 * k, ImVec2(a.x, a.y + 44 * k), white, "PACK SETTINGS");
    const float col = std::min(760 * k, (b.x - a.x) * 0.52f);
    const float x = a.x, w = col;
    float y = a.y + 116 * k;
    // ---- the playlist itself ----
    heading(d, f, ImVec2(x, y), w - 90 * k, "Trick playlist", std::to_string(v.playlist.size()));
    if (toggle(d, f, "##playliston", ImVec2(x + w - 70 * k, y - 2 * k), v.playlist_on)) {
        std::string m;
        studio::set_playlist_on(!v.playlist_on, m);
        say(m);
    }
    y += 36 * k;
    y += paragraph(d, f.bold, 13 * k, ImVec2(x, y), w, soft,
                   v.playlist_on ? "On: what is in it plays in game, from the top down, and nothing else of yours. The packs' own ON switches wait until it is off."
                                 : "Off: the packs that are on play, as before. Put whole packs or single tricks in it, then switch it on.") + 14 * k;
    {
        text(d, f.heading, 14 * k, ImVec2(x, y + 9 * k), white, "THE SAME TRICK MORE THAN ONCE");
        const int chosen = static_cast<int>(v.playlist_mode);
        const int picked = segmented(d, f, "##playlistmode", ImVec2(x + 300 * k, y), w - 300 * k, 36 * k, {"FIXED", "IN TURNS", "SHUFFLE"}, chosen);
        if (picked >= 0 && picked != chosen) {
            std::string m;
            studio::set_playlist_mode(static_cast<studio::PlaylistMode>(picked), m);
            say(m);
        }
        y += 44 * k;
        y += paragraph(d, f.bold, 12 * k, ImVec2(x, y), w, faint,
                       chosen == 0 ? "Fixed: the one highest in the playlist plays every time."
                       : chosen == 1 ? "In turns: one after another, top to bottom, attempt by attempt."
                                     : "Shuffle: any of them at random, never the same one twice running.") + 14 * k;
    }
    const ImVec2 la(x, y), lb(x + w, b.y);
    begin_region(57, la, lb, u.pack_settings_scroll, k);
    float ly = la.y - u.pack_settings_scroll;
    if (v.playlist.empty())
        ly += paragraph(d, f.bold, 15 * k, ImVec2(x, ly), w - 14 * k, soft, "Empty. Add whole packs or single tricks from the right.") + 10 * k;
    for (std::size_t i = 0; i < v.playlist.size(); ++i) {
        const auto& e = v.playlist[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 ra(x, ly), rb(x + w - 14 * k, ly + 48 * k);
        skate_theme::rough_rect(d, ra, rb, IM_COL32(26, 27, 30, 244), seed_of(ra), k * 0.6f);
        text(d, f.bold, 14 * k, ImVec2(ra.x + 14 * k, ra.y + 15 * k), faint, std::to_string(i + 1));
        const std::string what = e.animation.empty() ? "PACK" : "TRICK";
        tag(d, f, ImVec2(ra.x + 44 * k, ra.y + 14 * k), what, e.animation.empty() ? blue : IM_COL32(255, 255, 255, 46));
        const std::string label = e.animation.empty() ? e.pack : shown_name(v, e.animation) + (e.pack.empty() ? "" : "   from " + e.pack);
        text(d, f.bold, 16 * k, ImVec2(ra.x + 110 * k, ra.y + 13 * k), white, elide(f.bold, 16 * k, label, rb.x - ra.x - 300 * k));
        if (action(d, f, "##up", ImVec2(rb.x - 176 * k, ra.y + 8 * k), ImVec2(50 * k, 32 * k), "UP", Look::ghost, i > 0)) studio::playlist_move(i, -1);
        if (action(d, f, "##down", ImVec2(rb.x - 120 * k, ra.y + 8 * k), ImVec2(56 * k, 32 * k), "DOWN", Look::ghost, i + 1 < v.playlist.size()))
            studio::playlist_move(i, 1);
        if (action(d, f, "##remove", ImVec2(rb.x - 58 * k, ra.y + 8 * k), ImVec2(50 * k, 32 * k), "X", Look::danger)) studio::playlist_remove(i);
        ImGui::PopID();
        ly += 54 * k;
    }
    end_region(d, la, lb, ly + u.pack_settings_scroll - la.y, u.pack_settings_scroll, k);
    // ---- what can go in it: each pack whole, or any one of its tricks; and your own tricks ----
    const float px = x + w + 40 * k, pw = b.x - px;
    float py = a.y + 116 * k;
    heading(d, f, ImVec2(px, py), pw, "Add to the playlist");
    py += 36 * k;
    const ImVec2 pa(px, py), pb(b.x, b.y);
    begin_region(58, pa, pb, u.playlist_pick_scroll, k);
    float qy = pa.y - u.playlist_pick_scroll;
    const auto in_playlist = [&](const studio::PlaylistEntry& e) { return std::find(v.playlist.begin(), v.playlist.end(), e) != v.playlist.end(); };
    const auto add_row = [&](const studio::PlaylistEntry& e, const std::string& label, bool whole) {
        ImGui::PushID((e.pack + "|" + e.animation).c_str());
        const bool have = in_playlist(e);
        const ImVec2 ra(px + (whole ? 0 : 22 * k), qy), rb(px + pw - 14 * k, qy + (whole ? 44 : 36) * k);
        skate_theme::rough_rect(d, ra, rb, whole ? IM_COL32(30, 32, 38, 244) : IM_COL32(22, 23, 26, 240), seed_of(ra), k * 0.6f);
        text(d, whole ? f.heading : f.bold, (whole ? 15 : 14) * k, ImVec2(ra.x + 14 * k, ra.y + (whole ? 13 : 9) * k), have ? faint : white,
             elide(f.bold, 15 * k, label, rb.x - ra.x - 140 * k));
        if (action(d, f, "##add", ImVec2(rb.x - 110 * k, ra.y + 4 * k), ImVec2(100 * k, rb.y - ra.y - 8 * k), have ? "ADDED" : "+ ADD", have ? Look::ghost : Look::blue, !have)) {
            std::string m;
            studio::playlist_add(e, m);
            say(m);
        }
        ImGui::PopID();
        qy = rb.y + 6 * k;
    };
    for (const auto& pack : v.packs) {
        add_row({pack.name, ""}, upper(pack.name) + "  (WHOLE PACK, " + std::to_string(pack.members.size()) + ")", true);
        for (const auto& member : pack.members) add_row({pack.name, member}, shown_name(v, member), false);
        qy += 8 * k;
    }
    {
        // Your own tricks that are in no pack.
        bool any = false;
        for (const auto& p : v.presets) {
            if (std::any_of(v.packs.begin(), v.packs.end(), [&](const auto& pack) { return pack.pack.has(p.name); })) continue;
            if (!any) {
                heading(d, f, ImVec2(px, qy), pw - 14 * k, "Your tricks in no pack");
                qy += 34 * k;
                any = true;
            }
            add_row({"", p.name}, shown_name(v, p.name), false);
        }
    }
    end_region(d, pa, pb, qy + u.playlist_pick_scroll - pa.y, u.playlist_pick_scroll, k);
}
// ---- MARKET: the shop ----------------------------------------------------------------------------
// The approved mockup (contrib/animator-studio/market_mockup) built into the studio: a landing page (a welcome, the top of
// the month in a spotlight and a ranked row, the crew on worn paper, new releases, a call to share, the all-time
// favorites), BROWSE, THE CREW, LIKED, INSTALLED, MY UPLOADS, a search always at the top, a page for each pack, and the
// cover designer. Its paper is torn paper (studio_torn.h) cut from the textures the market carries
// (studio_market_art.h), over the market's own wall; its headlines are Anton.
using MarketPack = studio::market::Pack;
using PackList = std::vector<const MarketPack*>;
constexpr float shop_pi = 3.14159265f;
constexpr ImU32 shop_ink = IM_COL32(11, 12, 15, 255);
constexpr ImU32 shop_white = IM_COL32(243, 243, 239, 255);
std::string thousands(std::uint64_t n) {
    auto text = std::to_string(n);
    for (int i = static_cast<int>(text.size()) - 3; i > 0; i -= 3) text.insert(static_cast<std::size_t>(i), ",");
    return text;
}
// A count as shops write it: 999, 1.2K, 12K, 1.2M.
std::string short_count(std::uint64_t n) {
    char buffer[32]{};
    if (n < 1000) return std::to_string(n);
    if (n < 10000) {
        std::snprintf(buffer, sizeof buffer, "%.1fK", static_cast<double>(n) / 1000.0);
        return buffer;
    }
    if (n < 1000000) return std::to_string(n / 1000) + "K";
    std::snprintf(buffer, sizeof buffer, "%.1fM", static_cast<double>(n) / 1000000.0);
    return buffer;
}

// ---- type ----
// Anton at the size it is drawn: the big face for big words (scaled up it would blur), the small one for the rest.
ImFont* anton(const Fonts& f, float px) {
    if (px > 72 && f.display_big) return f.display_big;
    if (px <= 30 && f.display_small) return f.display_small;
    return f.display ? f.display : f.heading;
}
// The brush face at the size it is drawn: the small one for stamps, the big one for the rest.
ImFont* marker(const Fonts& f, float px) { return px <= 34 && f.marker_small ? f.marker_small : f.title; }
// Where capitals sit in a line of `font` drawn `size` high: their top and their foot below the line's top.
std::pair<float, float> caps_of(ImFont* font, float size) {
    const auto* g = font->FindGlyph('H');
    if (!g || font->FontSize <= 0) return {size * 0.2f, size * 0.8f};
    const float s = size / font->FontSize;
    return {g->Y0 * s, g->Y1 * s};
}
float spaced_width(ImFont* font, float size, std::string_view words, float spacing) {
    float w = 0;
    char one[2]{};
    for (const char c : words) {
        one[0] = c;
        w += font->CalcTextSizeA(size, FLT_MAX, 0, one).x + spacing;
    }
    return words.empty() ? 0 : w - spacing;
}
// Words with room between their letters (the shop's small capitals). Returns how wide they came out.
float text_spaced(ImDrawList* d, ImFont* font, float size, ImVec2 at, ImU32 colour, std::string_view words, float spacing) {
    float x = at.x;
    char one[2]{};
    for (const char c : words) {
        one[0] = c;
        d->AddText(font, size, ImVec2(x, at.y), colour, one);
        x += font->CalcTextSizeA(size, FLT_MAX, 0, one).x + spacing;
    }
    return words.empty() ? 0 : x - at.x - spacing;
}
// What was drawn since `start` (vertices of the list), turned `angle` about `centre`: a label's words with its paper.
void rotate_since(ImDrawList* d, int start, float angle, ImVec2 centre) {
    const float c = std::cos(angle), s = std::sin(angle);
    for (int i = start; i < d->VtxBuffer.Size; ++i) {
        auto& p = d->VtxBuffer[i].pos;
        const float x = p.x - centre.x, y = p.y - centre.y;
        p = ImVec2(centre.x + x * c - y * s, centre.y + x * s + y * c);
    }
}

// ---- torn paper, drawn ----
enum class PaperKind { blue, white, grip };
MarketArt art_of(PaperKind kind) { return kind == PaperKind::blue ? MarketArt::paper_blue : kind == PaperKind::white ? MarketArt::paper_white : MarketArt::grip; }
ImU32 rim_colour(PaperKind kind) {
    return kind == PaperKind::blue ? IM_COL32(231, 238, 251, 255) : kind == PaperKind::white ? IM_COL32(251, 250, 246, 255) : IM_COL32(142, 144, 151, 255);
}
ImU32 plain_colour(PaperKind kind) { // the paper's colour, while its texture is still on its way
    return kind == PaperKind::blue ? IM_COL32(4, 118, 236, 255) : kind == PaperKind::white ? IM_COL32(230, 227, 218, 255) : IM_COL32(22, 23, 26, 255);
}
// Triangles gathered and handed to the draw list in one go.
struct Mesh {
    std::vector<ImDrawVert> verts;
    std::vector<unsigned> idx;
    unsigned vert(ImVec2 pos, ImVec2 uv, ImU32 col) {
        verts.push_back({pos, uv, col});
        return static_cast<unsigned>(verts.size() - 1);
    }
    void tri(unsigned a, unsigned b, unsigned c) { idx.insert(idx.end(), {a, b, c}); }
    void flush(ImDrawList* d) {
        if (verts.empty()) return;
        d->PrimReserve(static_cast<int>(idx.size()), static_cast<int>(verts.size()));
        const unsigned base = d->_VtxCurrentIdx;
        for (const auto& v : verts) d->PrimWriteVtx(v.pos, v.uv, v.col);
        for (const auto i : idx) d->PrimWriteIdx(static_cast<ImDrawIdx>(base + i));
    }
};
// Where a piece is put: at `at`, turned about `pivot` (in its own coordinates).
struct PaperPlace {
    ImVec2 at;
    float c{1}, s{};
    ImVec2 pivot;
    ImVec2 xf(float x, float y) const {
        const float dx = x - pivot.x, dy = y - pivot.y;
        return ImVec2(at.x + pivot.x + dx * c - dy * s, at.y + pivot.y + dx * s + dy * c);
    }
};
// The piece's shape: a strip of quads between its tops and its feet, a fan at each ragged end. `uv` gives each corner its
// texture coordinates (from where it is before it is put), `tile` the tile its strip quad starts in.
template <class Uv> void paper_mesh(Mesh& m, const torn::Paper& p, const PaperPlace& at, ImVec2 off, ImU32 col, Uv uv, float tile_w, float origin_x,
                                   float scale) {
    if (p.top.size() < 2) return;
    const auto tile_of = [&](float x) { return tile_w > 0 ? std::floor((x - origin_x) / scale / tile_w) : 0.0f; };
    const auto put = [&](torn::Point q, float tile) { return m.vert(at.xf(q.x + off.x, q.y + off.y), uv(q.x, q.y, tile), col); };
    const auto quad = [&](torn::Point a, torn::Point b, torn::Point bf, torn::Point af, float tile) {
        const auto i0 = put(a, tile), i1 = put(b, tile), i2 = put(bf, tile), i3 = put(af, tile);
        m.tri(i0, i1, i2);
        m.tri(i0, i2, i3);
    };
    for (std::size_t i = 0; i + 1 < p.top.size(); ++i) {
        const auto a = p.top[i], b = p.top[i + 1], af = p.foot[i], bf = p.foot[i + 1];
        const float tile = tile_of(a.x);
        const float edge = origin_x + (tile + 1) * tile_w * scale;
        if (tile_w <= 0 || b.x <= edge) quad(a, b, bf, af, tile);
        else {
            // It crosses into the next tile: two quads, each in its own.
            const float f = (edge - a.x) / std::max(0.001f, b.x - a.x);
            const torn::Point mt{edge, a.y + (b.y - a.y) * f}, mf{edge, af.y + (bf.y - af.y) * f};
            quad(a, mt, mf, af, tile);
            quad(mt, b, bf, mf, tile + 1);
        }
    }
    const auto fan = [&](torn::Point top, const std::vector<torn::Point>& ends, torn::Point foot) {
        if (ends.empty()) return;
        const torn::Point centre{top.x, (top.y + foot.y) * 0.5f};
        const float tile = tile_of(centre.x);
        const auto c = put(centre, tile);
        auto prev = put(top, tile);
        for (const auto& e : ends) {
            const auto cur = put(e, tile);
            m.tri(c, prev, cur);
            prev = cur;
        }
        m.tri(c, prev, put(foot, tile));
    };
    fan(p.top.front(), p.left, p.foot.front());
    fan(p.top.back(), p.right, p.foot.back());
}
// A piece of torn paper: its shadow, its white core showing at the tear, and the paper cut from `kind`'s texture.
void draw_paper(ImDrawList* d, const Fonts& f, const torn::Paper& shape, ImVec2 at, float angle, ImVec2 pivot, PaperKind kind, std::uint32_t seed,
                float shadow = 0.42f, ImVec2 shadow_off = ImVec2(7, 9), float rim = 3.2f) {
    const float k = f.k;
    const PaperPlace place{at, std::cos(angle), std::sin(angle), pivot};
    const ImVec2 white_uv = ImGui::GetFontTexUvWhitePixel();
    const auto flat = [&](float, float, float) { return white_uv; };
    Mesh solid;
    if (shadow > 0) paper_mesh(solid, shape, place, ImVec2(shadow_off.x * k, shadow_off.y * k), IM_COL32(0, 0, 0, static_cast<int>(255 * shadow)), flat, 0, 0, 1);
    paper_mesh(solid, torn::rim(shape, seed, rim * k), place, ImVec2(0, 0), rim_colour(kind), flat, 0, 0, 1);
    ArtPlace art;
    const bool ready = market_art(art_of(kind), art) && art.width > 0;
    if (!ready) paper_mesh(solid, shape, place, ImVec2(0, 0), plain_colour(kind), flat, 0, 0, 1);
    solid.flush(d);
    if (!ready) return;
    // Cut from the texture a texel a pixel (on the 1080p page), somewhere along it of its own.
    float min_x = 1e9f, min_y = 1e9f, max_y = -1e9f;
    for (const auto* side : {&shape.top, &shape.foot, &shape.left, &shape.right})
        for (const auto& q : *side) min_x = std::min(min_x, q.x), min_y = std::min(min_y, q.y), max_y = std::max(max_y, q.y);
    float scale = k;
    const float room = art.width + art.border - 48;
    if ((max_y - min_y) / scale > room) scale = (max_y - min_y) / room;
    torn::Random r(seed + 7);
    const ImVec2 origin(min_x - r() * 600 * scale, min_y - r() * 12 * scale);
    const ImVec2 per((art.uv1.x - art.uv0.x) / art.width, (art.uv1.y - art.uv0.y) / art.height);
    const auto uv = [&](float x, float y, float tile) {
        return ImVec2(art.uv0.x + ((x - origin.x) / scale - tile * art.width) * per.x, art.uv0.y + ((y - origin.y) / scale) * per.y);
    };
    Mesh paper;
    paper_mesh(paper, shape, place, ImVec2(0, 0), IM_COL32_WHITE, uv, art.width, origin.x, scale);
    d->PushTextureID(art.texture);
    paper.flush(d);
    d->PopTextureID();
}
// Words on a torn paper label, its top left at `at` before it is turned `deg` about its middle. Returns its size.
ImVec2 paper_label(ImDrawList* d, const Fonts& f, ImFont* font, float size, ImVec2 at, std::string_view words, PaperKind kind, std::uint32_t seed,
                   float deg, ImU32 ink, ImVec2 pad, float spacing = 0, float edge = 2.6f, float ends = 10) {
    const float k = f.k;
    const auto [cap_top, cap_foot] = caps_of(font, size);
    const float tw = spacing > 0 ? spaced_width(font, size, words, spacing) : font->CalcTextSizeA(size, FLT_MAX, 0, words.data(), words.data() + words.size()).x;
    const ImVec2 sz(tw + pad.x * 2, (cap_foot - cap_top) + pad.y * 2);
    const auto shape = torn::piece(sz.x, sz.y, seed, edge * k, ends * k, 7 * k);
    const float angle = deg * shop_pi / 180;
    const int start = d->VtxBuffer.Size;
    draw_paper(d, f, shape, at, 0, ImVec2(0, 0), kind, seed);
    const ImVec2 tp(at.x + pad.x, at.y + pad.y - cap_top);
    if (spacing > 0) text_spaced(d, font, size, tp, ink, words, spacing);
    else d->AddText(font, size, tp, ink, words.data(), words.data() + words.size());
    rotate_since(d, start, angle, ImVec2(at.x + sz.x * 0.5f, at.y + sz.y * 0.5f));
    return sz;
}
ImVec2 label_size(const Fonts& f, ImFont* font, float size, std::string_view words, ImVec2 pad, float spacing = 0) {
    const auto [cap_top, cap_foot] = caps_of(font, size);
    const float tw = spacing > 0 ? spaced_width(font, size, words, spacing) : font->CalcTextSizeA(size, FLT_MAX, 0, words.data(), words.data() + words.size()).x;
    (void)f;
    return ImVec2(tw + pad.x * 2, (cap_foot - cap_top) + pad.y * 2);
}
// The "crew certified" stamp: worn paper, marker. Returns its size.
ImVec2 crew_stamp(ImDrawList* d, const Fonts& f, ImVec2 at, float size, std::uint32_t seed, float deg = -3, PaperKind kind = PaperKind::white) {
    const float k = f.k;
    (void)k;
    return paper_label(d, f, marker(f, size), size, at, "crew certified", kind, seed, deg, shop_ink, ImVec2(size * 0.75f, size * 0.42f), 0, 1.8f, 6);
}
// A band of torn paper right across the screen, `top` to `top + h`.
void paper_band(ImDrawList* d, const Fonts& f, float width, float top, float h, PaperKind kind, std::uint32_t seed, float shadow = 0.5f, float edge = 14) {
    const float k = f.k;
    const auto shape = torn::band(width, h, seed, edge * k);
    draw_paper(d, f, shape, ImVec2(0, top), 0, ImVec2(0, 0), kind, seed, shadow, ImVec2(0, 10), 3.2f);
}

void heart_icon(ImDrawList* d, ImVec2 c, float s, ImU32 colour, bool filled); // below
// ---- the shop's own buttons: flat, as the mockup's ----
enum class ShopLook { blue, line, paper, black };
bool shop_button(ImDrawList* d, const Fonts& f, const char* id, ImVec2 at, ImVec2 size, std::string_view label, ShopLook look = ShopLook::line,
                 bool enabled = true, int icon = 0 /* 1 heart, 2 heart filled, 3 arrow */) {
    const float k = f.k;
    const ImVec2 b(at.x + size.x, at.y + size.y);
    bool lit = false;
    const bool pressed = hit(id, at, b, &lit, enabled);
    ImU32 fill = 0, ink = IM_COL32_WHITE;
    switch (look) {
    case ShopLook::blue: fill = lit ? IM_COL32(40, 152, 255, 255) : blue; break;
    case ShopLook::paper: fill = lit ? IM_COL32(255, 255, 255, 255) : IM_COL32(233, 230, 221, 255), ink = shop_ink; break;
    case ShopLook::black: fill = lit ? IM_COL32(40, 42, 48, 255) : shop_ink; break;
    case ShopLook::line: fill = lit ? IM_COL32(255, 255, 255, 26) : IM_COL32(5, 6, 8, 90); break;
    }
    if (!enabled) fill = IM_COL32(40, 42, 46, 200), ink = IM_COL32(150, 152, 158, 255);
    d->AddRectFilled(at, b, fill);
    if (look == ShopLook::line) d->AddRect(at, b, enabled ? IM_COL32(255, 255, 255, lit ? 230 : 190) : IM_COL32(255, 255, 255, 60), 0, 0, 2 * k);
    if (lit && nav().keys) chosen_outline(d, at, b, k);
    const float fs = std::min(15 * k, size.y * 0.3f);
    const float tw = spaced_width(f.heading, fs, label, 0.5f * k);
    const float iw = icon ? 26 * k : 0;
    float x = at.x + (size.x - tw - iw) * 0.5f;
    if (icon == 1 || icon == 2) {
        heart_icon(d, ImVec2(x + 9 * k, at.y + size.y * 0.5f), 18 * k, ink, icon == 2);
        x += iw;
    } else if (icon == 4) {
        const ImVec2 c(x + 8 * k, at.y + size.y * 0.5f);
        d->AddLine(ImVec2(c.x - 7 * k, c.y - 7 * k), ImVec2(c.x + 7 * k, c.y + 7 * k), ink, 2 * k);
        d->AddLine(ImVec2(c.x + 7 * k, c.y - 7 * k), ImVec2(c.x - 7 * k, c.y + 7 * k), ink, 2 * k);
        x += iw;
    }
    const auto [ct, cf] = caps_of(f.heading, fs);
    text_spaced(d, f.heading, fs, ImVec2(x, at.y + (size.y - (cf - ct)) * 0.5f - ct), ink, label, 0.5f * k);
    if (icon == 3) {
        const float ax = x + tw + 12 * k, ay = at.y + size.y * 0.5f;
        d->AddLine(ImVec2(ax, ay), ImVec2(ax + 12 * k, ay), ink, 2.2f * k);
        d->AddTriangleFilled(ImVec2(ax + 15 * k, ay), ImVec2(ax + 9 * k, ay - 5 * k), ImVec2(ax + 9 * k, ay + 5 * k), ink);
    }
    return pressed && enabled;
}
float shop_button_width(const Fonts& f, std::string_view label, float height, int icon = 0) {
    const float k = f.k, fs = std::min(15 * k, height * 0.3f);
    return spaced_width(f.heading, fs, label, 0.5f * k) + (icon ? 26 * k : 0) + 2 * (height > 48 * k ? 26 : 18) * k;
}

// ---- little drawings ----
void heart_icon(ImDrawList* d, ImVec2 c, float s, ImU32 colour, bool filled);
void download_icon(ImDrawList* d, ImVec2 c, float s, ImU32 colour) {
    const float t = std::max(1.4f, s * 0.13f);
    d->AddLine(ImVec2(c.x, c.y - s * 0.45f), ImVec2(c.x, c.y + s * 0.02f), colour, t);
    d->AddTriangleFilled(ImVec2(c.x - s * 0.3f, c.y - s * 0.04f), ImVec2(c.x + s * 0.3f, c.y - s * 0.04f), ImVec2(c.x, c.y + s * 0.3f), colour);
    d->AddLine(ImVec2(c.x - s * 0.42f, c.y + s * 0.45f), ImVec2(c.x + s * 0.42f, c.y + s * 0.45f), colour, t);
}
void heart_icon(ImDrawList* d, ImVec2 c, float s, ImU32 colour, bool filled) {
    const float r = s * 0.27f, cy = c.y - s * 0.1f;
    const ImVec2 left(c.x - r * 0.95f, cy), right(c.x + r * 0.95f, cy), tip(c.x, c.y + s * 0.42f);
    if (filled) {
        d->AddCircleFilled(left, r, colour, 18);
        d->AddCircleFilled(right, r, colour, 18);
        d->AddTriangleFilled(ImVec2(left.x - r * 0.81f, cy + r * 0.59f), ImVec2(right.x + r * 0.81f, cy + r * 0.59f), tip, colour);
        d->AddTriangleFilled(ImVec2(left.x - r * 0.97f, cy + r * 0.2f), ImVec2(right.x + r * 0.97f, cy + r * 0.2f), ImVec2(c.x, cy + r * 1.4f), colour);
        return;
    }
    d->PathArcTo(left, r, shop_pi * 0.8f, shop_pi * 1.9f, 14);
    d->PathArcTo(right, r, shop_pi * 1.1f, shop_pi * 2.2f, 14);
    d->PathLineTo(tip);
    d->PathStroke(colour, ImDrawFlags_Closed, std::max(1.4f, s * 0.1f));
}
void search_icon(ImDrawList* d, ImVec2 c, float s, ImU32 colour) {
    const float t = std::max(1.5f, s * 0.12f);
    d->AddCircle(ImVec2(c.x - s * 0.08f, c.y - s * 0.08f), s * 0.3f, colour, 20, t);
    d->AddLine(ImVec2(c.x + s * 0.14f, c.y + s * 0.14f), ImVec2(c.x + s * 0.42f, c.y + s * 0.42f), colour, t * 1.3f);
}
void layers_icon(ImDrawList* d, ImVec2 c, float s, ImU32 colour) {
    const float t = std::max(1.2f, s * 0.11f), h = s * 0.5f;
    d->AddQuad(ImVec2(c.x, c.y - h), ImVec2(c.x + h, c.y - h * 0.5f), ImVec2(c.x, c.y), ImVec2(c.x - h, c.y - h * 0.5f), colour, t);
    d->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x, c.y + h * 0.5f), colour, t);
    d->AddLine(ImVec2(c.x, c.y + h * 0.5f), ImVec2(c.x + h, c.y), colour, t);
}
void stance_icon(ImDrawList* d, ImVec2 c, float s, ImU32 colour) {
    const float t = std::max(1.2f, s * 0.11f);
    d->AddCircle(ImVec2(c.x, c.y - s * 0.28f), s * 0.16f, colour, 12, t);
    d->AddLine(ImVec2(c.x - s * 0.32f, c.y + s * 0.45f), ImVec2(c.x - s * 0.18f, c.y), colour, t);
    d->AddLine(ImVec2(c.x - s * 0.18f, c.y), ImVec2(c.x + s * 0.18f, c.y), colour, t);
    d->AddLine(ImVec2(c.x + s * 0.18f, c.y), ImVec2(c.x + s * 0.32f, c.y + s * 0.45f), colour, t);
    d->AddLine(ImVec2(c.x - s * 0.42f, c.y + s * 0.12f), ImVec2(c.x + s * 0.42f, c.y + s * 0.12f), colour, t);
}

// ---- what the shop knows of a pack ----
std::uint64_t score_of(const MarketPack& p) { return p.downloads + 25ull * static_cast<std::uint64_t>(std::max(0, p.rating)); }
long long days_old(const MarketPack& p) {
    if (p.sample) return p.sample_days;
    const auto today = std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now().time_since_epoch()).count() / 24;
    const auto day = studio::market::day_number(p.created.empty() ? p.updated : p.created);
    return day < 0 ? 1000000 : today - day;
}
bool is_new(const MarketPack& p) { return days_old(p) <= 7; }
PackList sorted(PackList list, const std::string& order) {
    if (order == "new") std::stable_sort(list.begin(), list.end(), [](const auto* a, const auto* b) { return days_old(*a) < days_old(*b); });
    else if (order == "liked")
        std::stable_sort(list.begin(), list.end(), [](const auto* a, const auto* b) { return a->rating != b->rating ? a->rating > b->rating : a->downloads > b->downloads; });
    else if (order == "az") std::stable_sort(list.begin(), list.end(), [](const auto* a, const auto* b) { return _stricmp(a->title.c_str(), b->title.c_str()) < 0; });
    else if (order == "downloads") std::stable_sort(list.begin(), list.end(), [](const auto* a, const auto* b) { return a->downloads > b->downloads; });
    else std::stable_sort(list.begin(), list.end(), [](const auto* a, const auto* b) { return score_of(*a) > score_of(*b); });
    return list;
}
bool same_team(const std::string& a, std::string_view b) { return a.size() == b.size() && _strnicmp(a.c_str(), b.data(), b.size()) == 0; }
std::string maker_of(const MarketPack& p) {
    if (const auto* member = studio::market::crew_member(p.owner)) return std::string(member->name);
    return p.owner;
}
// Installed from the MARKET, but its pack is no longer in the library (deleted in PACKS): it can be put back.
bool removed_here(const studio::View& v, const MarketPack& p) {
    if (p.installed.empty()) return false;
    if (!p.installed_as.empty()) return pack_named(v, p.installed_as) == nullptr;
    for (const auto& pack : v.packs)
        if (studio::market::package_name(pack.name) == p.name || _stricmp(pack.name.c_str(), p.title.c_str()) == 0) return false;
    return true;
}
struct GetButton {
    std::string label;
    bool enabled{};
};
GetButton get_button(const studio::View& v, const studio::market::View& m, const MarketPack& p) {
    if (p.sample) return {"SAMPLE PACK", false};
    if (m.job.running)
        return {m.job.what.find(p.title) != std::string::npos ? std::to_string(static_cast<int>(m.job.fraction * 100)) + "%" : "BUSY...", false};
    if (p.installed.empty()) return {"GET IT", true};
    if (removed_here(v, p)) return {"REINSTALL", true};
    if (p.installed != p.version) return {"UPDATE TO " + p.version, true};
    return {"INSTALLED", false};
}
bool pack_matches(const MarketPack& p, const std::vector<std::string>& terms) {
    if (terms.empty()) return true;
    std::string words = p.title + " " + p.owner + " " + maker_of(p) + " " + p.description + " " + p.about + " " + p.stance;
    for (const auto& trick : p.tricks) words += " " + trick;
    return studio::native_search_matches(studio::native_search_key(words), terms);
}
// The covers the page drew: their icons fetched, and up to 30 of them shown (the picture texture holds 32).
struct Covers {
    std::vector<std::string> names;
    std::vector<std::pair<std::string, std::string>> pictures;
    void add(const MarketPack& p) {
        if (!p.sample && std::find(names.begin(), names.end(), p.full_name) == names.end()) names.push_back(p.full_name);
        if (p.icon_file.empty() || pictures.size() >= 30) return;
        auto key = "market|" + p.full_name + "|" + p.version;
        for (const auto& x : pictures)
            if (x.first == key) return;
        pictures.emplace_back(std::move(key), p.icon_file);
    }
    void ask() {
        studio::market::want_icons(names);
        std::sort(pictures.begin(), pictures.end()); // the same covers in another order are the same ask
        detail::want_studio_pack_pictures(pictures);
    }
};
void market_icon(ImDrawList* d, const Fonts& f, const MarketPack& pack, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE) {
    ImTextureID texture{};
    ImVec2 uv0{}, uv1{};
    if (!pack.icon_file.empty() && detail::studio_pack_picture("market|" + pack.full_name + "|" + pack.version, texture, uv0, uv1)) {
        d->AddImage(texture, a, b, uv0, uv1, tint);
        return;
    }
    studio::View::PackView drawn;
    drawn.name = pack.title;
    pack_cover(d, f, drawn, a, b);
}
void open_market_pack(const MarketPack& p) {
    auto& u = ui();
    u.market_open = p.full_name;
    u.market_detail_scroll = 0;
    if (!p.sample) studio::market::want_readmes({p.full_name});
}
// The heart: liked here (LIKED), and, once liked, the offer to like it on Thunderstore too.
void like_pack(const studio::market::View& m, const MarketPack& p) {
    studio::market::set_favorite(p.full_name, !p.favorite);
    if (!p.favorite && m.like_prompt && !p.sample) ui().like_pack = p.full_name;
    else say(p.favorite ? p.title + " is out of LIKED." : "Liked " + p.title + ": it is in LIKED.");
}

// ---- a photo print, taped up ----
// A print: its white border round a picture (drawn by `picture` into the box it is given), a caption under it in marker,
// a strip of tape on its top edge, its shadow; all of it turned `deg` about its middle. `size` is the picture's.
template <class Picture> void print_card(ImDrawList* d, const Fonts& f, ImVec2 at, ImVec2 size, float deg, std::string_view caption, bool tape, Picture picture,
                                        float caption_size = 22) {
    const float k = f.k, side = 14 * k, foot = 46 * k;
    const ImVec2 b(at.x + size.x + side * 2, at.y + size.y + side + foot);
    const int start = d->VtxBuffer.Size;
    d->AddRectFilled(ImVec2(at.x + 10 * k, at.y + 14 * k), ImVec2(b.x + 10 * k, b.y + 14 * k), IM_COL32(0, 0, 0, 115));
    d->AddRectFilled(at, b, IM_COL32(241, 239, 232, 255));
    picture(ImVec2(at.x + side, at.y + side), ImVec2(at.x + side + size.x, at.y + side + size.y));
    if (!caption.empty())
        d->AddText(marker(f, caption_size * k), caption_size * k, ImVec2(at.x + side + 2 * k, b.y - foot + 6 * k), shop_ink, caption.data(),
                   caption.data() + caption.size());
    if (tape) {
        const float tw = 130 * k, th = 34 * k, tx = at.x + (b.x - at.x - tw) * 0.5f, ty = at.y - 16 * k;
        d->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), IM_COL32(236, 232, 214, 200));
    }
    rotate_since(d, start, deg * shop_pi / 180, ImVec2((at.x + b.x) * 0.5f, (at.y + b.y) * 0.5f));
}

// ---- a pack's card ----
float shop_card_height(const Fonts& f, float w) { return w + 100 * f.k; }
// Its cover (the heart on it), and under it on a dark strip: its name in Anton, its maker (and the crew's stamp), how many
// tricks, its stance and its downloads. Hovered, it lifts and its cover is outlined blue; pressed, its page opens.
void shop_card(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::market::View& m, const MarketPack& p, ImVec2 a, float w, Covers& covers) {
    const float k = f.k, h = shop_card_height(f, w), hr = 19 * k;
    covers.add(p);
    ImGui::PushID(p.full_name.c_str());
    const ImVec2 hc(a.x + w - 12 * k - hr, a.y + 12 * k + hr);
    bool heart_lit = false, lit = false;
    // The heart first: it lies on the card, and of two things on top of each other the first takes the mouse.
    const bool hearted = hit("##heart", ImVec2(hc.x - hr, hc.y - hr), ImVec2(hc.x + hr, hc.y + hr), &heart_lit);
    const bool pressed = hit("##card", a, ImVec2(a.x + w, a.y + h), &lit);
    const float lift = lit || heart_lit ? 5 * k : 0;
    const ImVec2 ca(a.x, a.y - lift), cover_b(a.x + w, a.y - lift + w);
    if (lift > 0) d->AddRectFilled(ImVec2(ca.x + 6 * k, ca.y + 10 * k), ImVec2(cover_b.x + 6 * k, a.y - lift + h + 10 * k), IM_COL32(0, 0, 0, 120));
    d->AddRectFilled(ca, cover_b, IM_COL32(17, 17, 17, 255));
    market_icon(d, f, p, ca, cover_b);
    if (lift > 0) d->AddRect(ImVec2(ca.x - 1.5f * k, ca.y - 1.5f * k), ImVec2(cover_b.x + 1.5f * k, cover_b.y + 1.5f * k), blue, 0, 0, 3 * k);
    // Got already: a marker sticker on the cover's foot.
    if (!p.installed.empty()) {
        const bool gone = removed_here(v, p), behind = !gone && p.installed != p.version;
        paper_label(d, f, marker(f, 13 * k), 13 * k, ImVec2(ca.x + 10 * k, cover_b.y - 36 * k), gone ? "removed" : behind ? "update" : "installed",
                    gone ? PaperKind::white : behind ? PaperKind::blue : PaperKind::grip, static_cast<std::uint32_t>(p.full_name.size() * 17 + 5), -3,
                    gone ? shop_ink : IM_COL32_WHITE, ImVec2(10 * k, 6 * k), 0, 1.6f, 5);
    }
    const ImVec2 hcl(hc.x, hc.y - lift);
    d->AddCircleFilled(hcl, hr, heart_lit ? IM_COL32(0, 0, 0, 220) : IM_COL32(0, 0, 0, 155), 24);
    heart_icon(d, ImVec2(hcl.x, hcl.y + 1 * k), hr * 1.0f, p.favorite ? blue_light : white, p.favorite);
    // The strip under it.
    const ImVec2 ia(a.x, cover_b.y), ib(a.x + w, a.y - lift + h);
    d->AddRectFilled(ia, ib, IM_COL32(16, 17, 21, 255));
    d->AddRectFilled(ia, ImVec2(ib.x, ia.y + 3 * k), blue);
    const float x = ia.x + 14 * k, tw = w - 28 * k;
    ImFont* title_font = anton(f, 24 * k);
    const auto [ct, cf] = caps_of(title_font, 24 * k);
    d->AddText(title_font, 24 * k, ImVec2(x, ia.y + 15 * k - ct), shop_white, elide(title_font, 24 * k, upper(p.title), tw).c_str());
    const std::string by = "by " + maker_of(p);
    const float by_w = text_size(f.heading, 12 * k, by.c_str()).x;
    d->AddText(f.heading, 12 * k, ImVec2(x, ia.y + 45 * k), p.crew ? blue_light : soft, by.c_str());
    if (p.crew) crew_stamp(d, f, ImVec2(x + by_w + 10 * k, ia.y + 42 * k), 12 * k, static_cast<std::uint32_t>(p.full_name.size() * 31 + 300));
    // tricks | stance                downloads
    const float my = ia.y + 74 * k;
    const ImU32 meta = IM_COL32(243, 243, 239, 190);
    float mx = x;
    layers_icon(d, ImVec2(mx + 7 * k, my + 6 * k), 14 * k, soft);
    mx += 18 * k;
    const auto count = std::to_string(p.tricks.empty() ? 0 : p.tricks.size()) + " TRICKS";
    mx += text_spaced(d, f.heading, 11 * k, ImVec2(mx, my), meta, p.tricks.empty() ? "PACK" : count, 0.4f * k) + 10 * k;
    if (!p.stance.empty()) {
        d->AddRectFilled(ImVec2(mx, my), ImVec2(mx + 1 * k, my + 12 * k), IM_COL32(255, 255, 255, 64));
        mx += 10 * k;
        stance_icon(d, ImVec2(mx + 7 * k, my + 6 * k), 14 * k, soft);
        mx += 18 * k;
        text_spaced(d, f.heading, 11 * k, ImVec2(mx, my), meta, p.stance, 0.4f * k);
    }
    const auto dl = short_count(p.downloads);
    const float dw = text_size(f.heading, 11 * k, dl.c_str()).x;
    d->AddText(f.heading, 11 * k, ImVec2(ib.x - 14 * k - dw, my), soft, dl.c_str());
    download_icon(d, ImVec2(ib.x - 22 * k - dw, my + 6 * k), 12 * k, soft);
    if ((lit || heart_lit) && nav().keys) chosen_outline(d, ca, ib, k);
    ImGui::PopID();
    if (hearted) like_pack(m, p);
    else if (pressed) open_market_pack(p);
}
// Five cards in a row across `width`; with `ranked`, each beside its big number, Top-10 style.
float shop_row(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::market::View& m, const PackList& packs, ImVec2 at, float width, bool ranked,
               float top, float bottom, Covers& covers) {
    const float k = f.k, gap = (ranked ? 20 : 26) * k;
    const float slot = (width - gap * 4) / 5, num_w = ranked ? 74 * k : 0, w = slot - num_w, h = shop_card_height(f, w);
    if (at.y > bottom || at.y + h < top) return h;
    for (std::size_t i = 0; i < packs.size() && i < 5; ++i) {
        const float x = at.x + static_cast<float>(i) * (slot + gap);
        if (ranked) {
            ImFont* font = anton(f, 190 * k);
            const auto number = std::to_string(i + 1);
            const auto [ct, cf] = caps_of(font, 190 * k);
            const ImVec2 np(x - 6 * k, at.y + h - 84 * k - cf);
            d->AddText(font, 190 * k, ImVec2(np.x + 7 * k, np.y + 7 * k), blue, number.c_str());
            d->AddText(font, 190 * k, np, shop_white, number.c_str());
        }
        shop_card(d, f, v, m, *packs[i], ImVec2(x + num_w, at.y), w, covers);
    }
    return h;
}
// A section's heading: its torn paper label, words after it, SEE ALL at the right.
bool shop_head(ImDrawList* d, const Fonts& f, ImVec2 at, float width, std::string_view title, PaperKind kind, std::uint32_t seed, std::string_view sub,
               std::string_view sub_bold, const char* see_all_id) {
    const float k = f.k;
    ImFont* font = anton(f, 40 * k);
    const auto size = paper_label(d, f, font, 40 * k, at, title, kind, seed, -1.2f, kind == PaperKind::white ? shop_ink : IM_COL32_WHITE,
                                  ImVec2(26 * k, 13 * k), 1 * k);
    float sx = at.x + size.x + 22 * k;
    const float sy = at.y + size.y * 0.5f - 7 * k;
    if (!sub_bold.empty()) sx += text_spaced(d, f.heading, 13 * k, ImVec2(sx, sy), IM_COL32_WHITE, sub_bold, 2.5f * k) + 14 * k;
    if (!sub.empty()) text_spaced(d, f.heading, 13 * k, ImVec2(sx, sy), soft, sub, 2.5f * k);
    if (!see_all_id) return false;
    const float bw = shop_button_width(f, "SEE ALL", 40 * k, 3);
    return shop_button(d, f, see_all_id, ImVec2(at.x + width - bw, at.y + size.y * 0.5f - 20 * k), ImVec2(bw, 40 * k), "SEE ALL", ShopLook::line, true, 3);
}

// ---- the landing page ----
struct ShopColumn {
    float x0, width; // the 1920 column (on the 1080p page), centred on the screen
    float inner() const { return x0 + 48; }
};
void shop_wall(ImDrawList* d, ImVec2 display, float fade) {
    if (fade <= 0) return;
    ArtPlace wall;
    if (market_art(MarketArt::wallpaper, wall)) {
        // Filled over the screen with its own shape kept: what does not fit is cropped evenly.
        const float pw = wall.width, ph = wall.height, shape = pw / ph, screen = display.x / display.y;
        float u = 1, vv = 1;
        if (shape > screen) u = screen / shape;
        else vv = shape / screen;
        const ImVec2 uv0((1 - u) * 0.5f, (1 - vv) * 0.5f), uv1(1 - (1 - u) * 0.5f, 1 - (1 - vv) * 0.5f);
        d->AddImage(wall.texture, ImVec2(0, 0), display, uv0, uv1, IM_COL32(255, 255, 255, static_cast<int>(255 * fade)));
    } else d->AddRectFilled(ImVec2(0, 0), display, IM_COL32(13, 15, 19, static_cast<int>(255 * fade)));
    // Darker under the tabs and at the foot, so the shop reads over it.
    const auto a = [&](float x) { return static_cast<int>(255 * x * fade); };
    const float y1 = display.y * 0.24f, y2 = display.y * 0.8f;
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, y1), IM_COL32(5, 6, 8, a(0.78f)), IM_COL32(5, 6, 8, a(0.78f)), IM_COL32(5, 6, 8, a(0.35f)),
                               IM_COL32(5, 6, 8, a(0.35f)));
    d->AddRectFilled(ImVec2(0, y1), ImVec2(display.x, y2), IM_COL32(5, 6, 8, a(0.35f)));
    d->AddRectFilledMultiColor(ImVec2(0, y2), display, IM_COL32(5, 6, 8, a(0.35f)), IM_COL32(5, 6, 8, a(0.35f)), IM_COL32(5, 6, 8, a(0.7f)),
                               IM_COL32(5, 6, 8, a(0.7f)));
}
float welcome_section(ImDrawList* d, const Fonts& f, const studio::View& v, const PackList& all, const ShopColumn& col, float y) {
    auto& u = ui();
    const float k = f.k, h = 430 * k;
    const float cx = col.x0 * k + 64 * k;
    // The skater's print on the right, a splash behind it.
    {
        ArtPlace splat, skater;
        if (market_art(MarketArt::splatter_a, splat)) {
            const float w = 560 * k, sh = w * splat.height / splat.width;
            const float x = (col.x0 + col.width - 40 - 560) * k;
            d->AddImage(splat.texture, ImVec2(x, y - 60 * k), ImVec2(x + w, y - 60 * k + sh), splat.uv0, splat.uv1, IM_COL32(255, 255, 255, 242));
        }
        const ImVec2 size(600 * k, 330 * k);
        const ImVec2 at((col.x0 + col.width - 170) * k - size.x - 28 * k, y + 26 * k);
        const bool have = market_art(MarketArt::skater, skater);
        print_card(d, f, at, size, 3, "the studio, 2026", true, [&](ImVec2 a, ImVec2 b) {
            if (!have) {
                d->AddRectFilled(a, b, IM_COL32(4, 60, 140, 255));
                return;
            }
            // Cover-fit, held at 30% across and 40% down, as the mockup's.
            const float shape = skater.width / skater.height, box = (b.x - a.x) / (b.y - a.y);
            float uw = 1, vh = 1;
            if (shape > box) uw = box / shape;
            else vh = shape / box;
            const float u0 = (1 - uw) * 0.3f, v0 = (1 - vh) * 0.4f;
            const ImVec2 span(skater.uv1.x - skater.uv0.x, skater.uv1.y - skater.uv0.y);
            d->AddImage(skater.texture, a, b, ImVec2(skater.uv0.x + span.x * u0, skater.uv0.y + span.y * v0),
                        ImVec2(skater.uv0.x + span.x * (u0 + uw), skater.uv0.y + span.y * (v0 + vh)));
        });
    }
    // WELCOME TO THE on worn paper, MARKETPLACE clean and big.
    float ty = y + 22 * k;
    const auto label = paper_label(d, f, anton(f, 30 * k), 30 * k, ImVec2(cx, ty), "WELCOME TO THE", PaperKind::white, 11, -2, shop_ink,
                                   ImVec2(22 * k, 11 * k), 3 * k);
    ty += label.y + 8 * k;
    ImFont* big = anton(f, 168 * k);
    const auto [bt, bf] = caps_of(big, 168 * k);
    d->AddText(big, 168 * k, ImVec2(cx + 4 * k, ty - bt), IM_COL32_WHITE, "MARKETPLACE");
    ty += (bf - bt) + 28 * k;
    ty += paragraph(d, f.body, 16 * k, ImVec2(cx, ty), 640 * k, IM_COL32(243, 243, 239, 220),
                    "Animation packs made in the Animator Studio, by skaters, for skaters. Find a trick, hit GET, and it is in your game. Made one "
                    "yourself? Put it on the shelf.") +
          20 * k;
    // What is in the shop.
    std::size_t tricks = 0;
    std::uint64_t downloads = 0;
    for (const auto* p : all) tricks += p->tricks.size(), downloads += p->downloads;
    const std::array<std::pair<std::string, const char*>, 4> stats{{{std::to_string(all.size()), "PACKS"},
                                                                    {std::to_string(tricks), "TRICKS"},
                                                                    {std::to_string(studio::market::crew_members.size()), "CREW"},
                                                                    {short_count(downloads), "DOWNLOADS"}}};
    float sx = cx;
    ImFont* num = anton(f, 28 * k);
    const auto [nt, nf] = caps_of(num, 28 * k);
    for (const auto& [value, word] : stats) {
        d->AddText(num, 28 * k, ImVec2(sx, ty - nt), IM_COL32_WHITE, value.c_str());
        const float ww = text_spaced(d, f.heading, 11 * k, ImVec2(sx, ty + (nf - nt) + 8 * k), soft, word, 1.2f * k);
        sx += std::max(ww, text_size(num, 28 * k, value.c_str()).x) + 30 * k;
    }
    ty += (nf - nt) + 34 * k;
    const float bh = 56 * k;
    const float b1 = shop_button_width(f, "BROWSE ALL PACKS", bh);
    if (shop_button(d, f, "##welcomebrowse", ImVec2(cx, ty), ImVec2(b1, bh), "BROWSE ALL PACKS", ShopLook::blue)) {
        u.market_list = "browse";
        u.market_scroll = 0;
    }
    const float b2 = shop_button_width(f, "SHARE YOUR PACK", bh);
    if (shop_button(d, f, "##welcomeshare", ImVec2(cx + b1 + 12 * k, ty), ImVec2(b2, bh), "SHARE YOUR PACK", ShopLook::line)) {
        u.menu_tab = 1;
        u.pack_page = false;
        say("Open one of your packs and press SHARE (or UPDATE) on its page: its cover is made there too.");
    }
    (void)v;
    return h;
}
// The top of the month, one at a time, on grip tape across the screen: its name huge, its maker, its numbers, and its
// cover taped up beside the next ones.
float spotlight_section(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::market::View& m, const PackList& picks, const ShopColumn& col,
                        ImVec2 display, float y, float top, float bottom, Covers& covers) {
    auto& u = ui();
    const float k = f.k, h = 650 * k;
    if (y > bottom || y + h < top) return h;
    paper_band(d, f, display.x, y + 34 * k, 582 * k, PaperKind::grip, 7);
    if (picks.empty()) return h;
    const double now = ImGui::GetTime();
    if (u.spotlight >= static_cast<int>(picks.size()) || u.spotlight < 0) u.spotlight = 0;
    const ImVec2 ia(col.x0 * k, y + 34 * k), ib((col.x0 + col.width) * k, y + 34 * k + 582 * k);
    if (ImGui::IsMouseHoveringRect(ia, ib) || u.spotlight_since <= 0) u.spotlight_since = now;
    else if (picks.size() > 1 && now - u.spotlight_since > 7) {
        u.spotlight = (u.spotlight + 1) % static_cast<int>(picks.size());
        u.spotlight_since = now;
    }
    const auto& p = *picks[static_cast<std::size_t>(u.spotlight)];
    // The posters: the pick big and taped, the next two pinned behind it, darker.
    const float px = ib.x - 170 * k - 560 * k, py = ia.y + 40 * k;
    for (const int off : {2, 1, 0}) {
        const auto& q = *picks[static_cast<std::size_t>((u.spotlight + off) % static_cast<int>(picks.size()))];
        covers.add(q);
        const float size = (off ? 270 : 340) * k;
        const float rot = off == 0 ? -4.0f : off == 1 ? 5.0f : -2.0f;
        const ImVec2 at(px + (off == 0 ? 150 : off == 1 ? 320 : 0) * k, py + (off == 0 ? 30 : off == 1 ? 60 : 0) * k);
        const std::string caption = off ? "" : "#" + std::string(u.spotlight < 9 ? "0" : "") + std::to_string(u.spotlight + 1) + " of the month";
        print_card(d, f, at, ImVec2(size, size), rot, caption, off == 0, [&](ImVec2 a, ImVec2 b) {
            market_icon(d, f, q, a, b, off ? IM_COL32(107, 107, 107, 255) : IM_COL32_WHITE);
        });
        if (off == 0 && hit("##spotposter", at, ImVec2(at.x + size + 28 * k, at.y + size + 60 * k))) open_market_pack(p);
    }
    // Its words.
    const float x = ia.x + 112 * k;
    float ty = ia.y + 44 * k;
    const auto tag = paper_label(d, f, anton(f, 26 * k), 26 * k, ImVec2(x, ty), "TOP OF THE MONTH", PaperKind::blue, 81 + static_cast<std::uint32_t>(u.spotlight),
                                 -1.5f, IM_COL32_WHITE, ImVec2(20 * k, 10 * k), 1 * k);
    ty += tag.y + 18 * k;
    const auto rank = "#" + std::string(u.spotlight < 9 ? "0" : "") + std::to_string(u.spotlight + 1) + " THIS MONTH";
    text_spaced(d, anton(f, 15 * k), 15 * k, ImVec2(x, ty), blue_light, rank, 3 * k);
    ty += 30 * k;
    // The name, in two lines: its first word over the rest.
    const auto title = upper(p.title);
    const auto space = title.find(' ');
    const std::array<std::string, 2> lines{title.substr(0, space), space == std::string::npos ? std::string{} : title.substr(space + 1)};
    ImFont* big = anton(f, 104 * k);
    const auto [bt, bf] = caps_of(big, 104 * k);
    for (const auto& line : lines) {
        if (line.empty()) continue;
        d->AddText(big, 104 * k, ImVec2(x, ty - bt), shop_white, elide(big, 104 * k, line, 860 * k).c_str());
        ty += (bf - bt) + 20 * k;
    }
    ty += 6 * k;
    d->AddText(f.heading, 20 * k, ImVec2(x, ty), IM_COL32_WHITE, "by");
    const float byw = text_size(f.heading, 20 * k, "by ").x;
    const auto maker = upper(maker_of(p));
    d->AddText(f.heading, 20 * k, ImVec2(x + byw + 4 * k, ty), blue_light, maker.c_str());
    if (p.crew) crew_stamp(d, f, ImVec2(x + byw + 18 * k + text_size(f.heading, 20 * k, maker.c_str()).x, ty - 2 * k), 17 * k, 91 + static_cast<std::uint32_t>(u.spotlight));
    ty += 38 * k;
    std::string meta = std::to_string(p.tricks.size()) + " TRICKS";
    if (!p.stance.empty()) meta += "  /  " + p.stance;
    meta += "  /  " + short_count(p.downloads) + " DOWNLOADS  /  " + std::to_string(std::max(0, p.rating)) + " LIKES";
    text_spaced(d, f.heading, 13 * k, ImVec2(x, ty), IM_COL32(243, 243, 239, 205), meta, 4 * k);
    ty += 30 * k;
    const auto about = p.about.empty() ? p.description : p.about;
    ImGui::PushClipRect(ImVec2(x, ty), ImVec2(x + 620 * k, ty + 46 * k), true);
    paragraph(d, f.body, 15 * k, ImVec2(x, ty), 620 * k, IM_COL32(243, 243, 239, 205), about);
    ImGui::PopClipRect();
    ty += 60 * k;
    const float bh = 56 * k;
    float bx = x;
    const float w1 = shop_button_width(f, "VIEW PACK", bh, 3);
    if (shop_button(d, f, "##spotview", ImVec2(bx, ty), ImVec2(w1, bh), "VIEW PACK", ShopLook::blue, true, 3)) open_market_pack(p);
    bx += w1 + 12 * k;
    const auto g = get_button(v, m, p);
    const float w2 = shop_button_width(f, g.label, bh);
    if (shop_button(d, f, "##spotget", ImVec2(bx, ty), ImVec2(w2, bh), g.label, ShopLook::line, g.enabled)) studio::market::install(p.full_name);
    bx += w2 + 12 * k;
    const float w3 = shop_button_width(f, p.favorite ? "LIKED" : "LIKE", bh, 1);
    if (shop_button(d, f, "##spotlike", ImVec2(bx, ty), ImVec2(w3, bh), p.favorite ? "LIKED" : "LIKE", ShopLook::line, true, p.favorite ? 2 : 1)) like_pack(m, p);
    // Arrows at its sides, and which of them this is.
    const auto arrow = [&](const char* id, float ax, bool next) {
        const ImVec2 c(ax, (ia.y + ib.y) * 0.5f);
        bool lit = false;
        const bool pressed = hit(id, ImVec2(c.x - 30 * k, c.y - 30 * k), ImVec2(c.x + 30 * k, c.y + 30 * k), &lit);
        d->AddCircleFilled(c, 30 * k, lit ? IM_COL32(255, 255, 255, 30) : IM_COL32(5, 6, 8, 100), 32);
        d->AddCircle(c, 30 * k, IM_COL32(255, 255, 255, 180), 32, 2 * k);
        const float s = next ? 1.0f : -1.0f;
        d->AddLine(ImVec2(c.x - 4 * k * s, c.y - 8 * k), ImVec2(c.x + 4 * k * s, c.y), IM_COL32_WHITE, 3 * k);
        d->AddLine(ImVec2(c.x + 4 * k * s, c.y), ImVec2(c.x - 4 * k * s, c.y + 8 * k), IM_COL32_WHITE, 3 * k);
        return pressed;
    };
    const int n = static_cast<int>(picks.size());
    if (n > 1 && arrow("##spotprev", ia.x + 60 * k, false)) u.spotlight = (u.spotlight + n - 1) % n, u.spotlight_since = now;
    if (n > 1 && arrow("##spotnext", ib.x - 60 * k, true)) u.spotlight = (u.spotlight + 1) % n, u.spotlight_since = now;
    {
        const auto count = std::string(u.spotlight < 9 ? "0" : "") + std::to_string(u.spotlight + 1);
        const auto total = " / " + std::string(n < 10 ? "0" : "") + std::to_string(n);
        ImFont* font = anton(f, 22 * k);
        const float tw = text_size(font, 22 * k, (count + total).c_str()).x;
        float qx = ib.x - 120 * k - tw - 16 * k - static_cast<float>(n) * 44 * k;
        const float qy = ib.y - 34 * k - 22 * k;
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            bool lit = false;
            if (hit("##spotbar", ImVec2(qx, qy + 6 * k), ImVec2(qx + 36 * k, qy + 18 * k), &lit)) u.spotlight = i, u.spotlight_since = now;
            ImGui::PopID();
            d->AddRectFilled(ImVec2(qx, qy + 9 * k), ImVec2(qx + 36 * k, qy + 14 * k), i == u.spotlight ? blue : IM_COL32(255, 255, 255, lit ? 150 : 90));
            qx += 44 * k;
        }
        qx += 8 * k;
        d->AddText(font, 22 * k, ImVec2(qx, qy), IM_COL32_WHITE, count.c_str());
        d->AddText(font, 22 * k, ImVec2(qx + text_size(font, 22 * k, count.c_str()).x, qy), soft, total.c_str());
    }
    return h;
}
// A member of the crew: their newest pack's cover across the top, their name, what they do, a few words, their numbers.
void member_card(ImDrawList* d, const Fonts& f, const studio::market::CrewMember& member, const PackList& all, ImVec2 a, float w, Covers& covers) {
    auto& u = ui();
    const float k = f.k, ph = 230 * k, h = ph + 210 * k;
    PackList theirs;
    std::uint64_t downloads = 0;
    int likes = 0;
    for (const auto* p : all)
        if (same_team(p->owner, member.team)) theirs.push_back(p), downloads += p->downloads, likes += std::max(0, p->rating);
    theirs = sorted(std::move(theirs), "new");
    ImGui::PushID(std::string(member.team).c_str());
    bool lit = false;
    const bool pressed = hit("##member", a, ImVec2(a.x + w, a.y + h), &lit);
    ImGui::PopID();
    const float lift = lit ? 4 * k : 0;
    const ImVec2 ca(a.x, a.y - lift), cb(a.x + w, a.y + h - lift);
    d->AddRectFilled(ImVec2(ca.x + 8 * k, ca.y + 10 * k), ImVec2(cb.x + 8 * k, cb.y + 10 * k), IM_COL32(0, 0, 0, 90));
    d->AddRectFilled(ca, cb, IM_COL32(14, 15, 18, 255));
    // The photo: their newest cover, its middle across.
    if (!theirs.empty()) {
        const auto& p = *theirs.front();
        covers.add(p);
        ImTextureID texture{};
        ImVec2 uv0{}, uv1{};
        const ImVec2 pa = ca, pb(cb.x, ca.y + ph);
        if (!p.icon_file.empty() && detail::studio_pack_picture("market|" + p.full_name + "|" + p.version, texture, uv0, uv1)) {
            const float box = (pb.x - pa.x) / (pb.y - pa.y), vh = 1 / box; // the square cut to the box's shape, its middle kept
            const float v0 = (1 - vh) * 0.5f;
            d->AddImage(texture, pa, pb, ImVec2(uv0.x, uv0.y + (uv1.y - uv0.y) * v0), ImVec2(uv1.x, uv0.y + (uv1.y - uv0.y) * (v0 + vh)));
        } else market_icon(d, f, p, pa, pb);
    } else d->AddRectFilled(ca, ImVec2(cb.x, ca.y + ph), IM_COL32(4, 60, 140, 255));
    float y = ca.y + ph + 14 * k;
    ImFont* nf = anton(f, 38 * k);
    const auto [nt, nb] = caps_of(nf, 38 * k);
    d->AddText(nf, 38 * k, ImVec2(ca.x + 18 * k, y - nt), IM_COL32_WHITE, upper(std::string(member.name)).c_str());
    y += (nb - nt) + 12 * k;
    text_spaced(d, f.heading, 12 * k, ImVec2(ca.x + 18 * k, y), blue_light, upper(std::string(member.role)), 1.8f * k);
    y += 24 * k;
    ImGui::PushClipRect(ImVec2(ca.x + 18 * k, y), ImVec2(cb.x - 18 * k, y + 60 * k), true);
    paragraph(d, f.body, 13 * k, ImVec2(ca.x + 18 * k, y), w - 36 * k, IM_COL32(243, 243, 239, 190), std::string(member.about));
    ImGui::PopClipRect();
    y = cb.y - 52 * k;
    float sx = ca.x + 18 * k;
    ImFont* num = anton(f, 22 * k);
    const auto [st, sb] = caps_of(num, 22 * k);
    const std::array<std::pair<std::string, const char*>, 3> stats{
        {{std::to_string(theirs.size()), "PACKS"}, {short_count(static_cast<std::uint64_t>(likes)), "LIKES"}, {short_count(downloads), "DOWNLOADS"}}};
    for (const auto& [value, word] : stats) {
        d->AddText(num, 22 * k, ImVec2(sx, y - st), IM_COL32_WHITE, value.c_str());
        const float ww = text_spaced(d, f.heading, 10 * k, ImVec2(sx, y + (sb - st) + 6 * k), soft, word, 1.2f * k);
        sx += std::max(ww, text_size(num, 22 * k, value.c_str()).x) + 18 * k;
    }
    if (lit && nav().keys) chosen_outline(d, ca, cb, k);
    if (pressed) {
        u.market_list = "crew";
        u.market_crew = std::string(member.team);
        u.market_scroll = 0;
    }
}
float crew_section(ImDrawList* d, const Fonts& f, const PackList& all, const ShopColumn& col, ImVec2 display, float y, float top, float bottom, Covers& covers) {
    auto& u = ui();
    const float k = f.k, h = 700 * k;
    if (y > bottom || y + h < top) return h;
    // Worn white paper across the screen, a blue strip torn under it.
    {
        const auto under = torn::band(display.x, 654 * k, 14, 16 * k);
        draw_paper(d, f, under, ImVec2(0, y + 44 * k), 0, ImVec2(0, 0), PaperKind::blue, 14, 0.3f, ImVec2(0, 10), 3.2f);
        paper_band(d, f, display.x, y + 40 * k, 620 * k, PaperKind::white, 9);
    }
    const float cx = (col.x0 + 64) * k;
    float ty = y + 92 * k;
    ImFont* big = anton(f, 112 * k);
    const auto [bt, bf] = caps_of(big, 112 * k);
    d->AddText(big, 112 * k, ImVec2(cx, ty - bt), shop_ink, "FROM");
    ty += (bf - bt) + 14 * k;
    d->AddText(big, 112 * k, ImVec2(cx, ty - bt), shop_ink, "THE CREW");
    ty += (bf - bt) + 26 * k;
    const auto stamp = paper_label(d, f, marker(f, 30 * k), 30 * k, ImVec2(cx, ty), "crew certified", PaperKind::blue, 31, -4, shop_ink, ImVec2(22 * k, 10 * k));
    ty += stamp.y + 22 * k;
    ty += paragraph(d, f.body, 16 * k, ImVec2(cx, ty), 520 * k, IM_COL32(42, 43, 48, 255),
                    "The testers who broke the studio so you would not have to. Every pack of theirs carries the CREW CERTIFIED stamp, all over the shop.") +
          22 * k;
    const float bw = shop_button_width(f, "MEET THE CREW", 46 * k, 3);
    if (shop_button(d, f, "##meetcrew", ImVec2(cx, ty), ImVec2(bw, 46 * k), "MEET THE CREW", ShopLook::black, true, 3)) {
        u.market_list = "crew";
        u.market_crew.clear();
        u.market_scroll = 0;
    }
    // The crew, a card each.
    const float mx = (col.x0 + 700) * k, mw = (col.x0 + col.width - 48) * k - mx, gap = 22 * k;
    const auto count = static_cast<float>(studio::market::crew_members.size());
    const float w = (mw - gap * (count - 1)) / count;
    float x = mx;
    for (const auto& member : studio::market::crew_members) {
        member_card(d, f, member, all, ImVec2(x, y + 110 * k), w, covers);
        x += w + gap;
    }
    return h;
}
float cta_section(ImDrawList* d, const Fonts& f, const ShopColumn& col, ImVec2 display, float y, float top, float bottom) {
    auto& u = ui();
    const float k = f.k, h = 300 * k;
    if (y > bottom || y + h < top) return h;
    paper_band(d, f, display.x, y + 30 * k, 240 * k, PaperKind::blue, 13);
    const float cx = (col.x0 + 64) * k;
    ImFont* big = anton(f, 78 * k);
    const auto [bt, bf] = caps_of(big, 78 * k);
    d->AddText(big, 78 * k, ImVec2(cx, y + 70 * k - bt), IM_COL32_WHITE, "GOT A TRICK?");
    d->AddText(big, 78 * k, ImVec2(cx, y + 70 * k + (bf - bt) + 14 * k - bt), IM_COL32_WHITE, "PUT IT ON THE SHELF.");
    const std::array<std::array<const char*, 3>, 3> steps{{{"01", "MAKE IT", "Pose it, key it, test it in game, in the studio."},
                                                           {"02", "PACK IT", "Put your tricks in a pack, and give it a cover."},
                                                           {"03", "SHARE IT", "Publish it from here. It is in everyone's shop in a minute."}}};
    float sx = (col.x0 + 900) * k;
    for (const auto& step : steps) {
        ImFont* num = anton(f, 54 * k);
        const auto [nt, nb] = caps_of(num, 54 * k);
        float sy = y + 74 * k;
        d->AddText(num, 54 * k, ImVec2(sx, sy - nt), shop_ink, step[0]);
        sy += (nb - nt) + 12 * k;
        ImFont* word = anton(f, 26 * k);
        const auto [wt, wb] = caps_of(word, 26 * k);
        text_spaced(d, word, 26 * k, ImVec2(sx, sy - wt), IM_COL32_WHITE, step[1], 1 * k);
        sy += (wb - wt) + 10 * k;
        paragraph(d, f.body, 13 * k, ImVec2(sx, sy), 220 * k, IM_COL32(255, 255, 255, 230), step[2]);
        sx += 260 * k;
    }
    const float bw = shop_button_width(f, "SHARE A PACK", 56 * k, 3);
    if (shop_button(d, f, "##ctashare", ImVec2((col.x0 + col.width - 64) * k - bw, y + 120 * k), ImVec2(bw, 56 * k), "SHARE A PACK", ShopLook::paper, true, 3)) {
        u.menu_tab = 1;
        u.pack_page = false;
        say("Open one of your packs and press SHARE (or UPDATE) on its page: its cover is made there too.");
    }
    return h;
}
void home_page(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::market::View& m, const PackList& all, ImVec2 display, float top,
               float bottom, Covers& covers) {
    auto& u = ui();
    const float k = f.k;
    const ShopColumn col{std::max(0.0f, display.x / k * 0.5f - 960), std::min(1920.0f, display.x / k)};
    begin_region(72, ImVec2(0, top), ImVec2(display.x, bottom), u.market_scroll, k);
    float y = top + 6 * k - u.market_scroll;
    const float ix = col.inner() * k, iw = (col.width - 96) * k;
    PackList month, crew, fresh;
    for (const auto* p : all) {
        if (days_old(*p) <= 31) month.push_back(p);
        if (p->crew) crew.push_back(p);
    }
    month = sorted(std::move(month), "top");
    if (month.size() > 5) month.resize(5);
    if (y + 430 * k > top) welcome_section(d, f, v, all, col, y);
    y += 430 * k + 10 * k;
    y += spotlight_section(d, f, v, m, month, col, display, y, top, bottom, covers);
    const auto head = [&](std::string_view title, PaperKind kind, std::uint32_t seed, std::string_view sub, std::string_view sub_bold, const char* id) {
        y += 44 * k;
        const float lh = 76 * k;
        bool pressed = false;
        if (y < bottom && y + lh > top) pressed = shop_head(d, f, ImVec2(ix, y), iw, title, kind, seed, sub, sub_bold, id);
        y += lh + 22 * k;
        return pressed;
    };
    const auto row = [&](const char* id, const PackList& list, bool ranked) {
        ImGui::PushID(id);
        y += shop_row(d, f, v, m, list, ImVec2(ix, y), iw, ranked, top, bottom, covers);
        ImGui::PopID();
    };
    if (!month.empty()) {
        static const char* months[]{"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
        localtime_s(&local, &now);
        const auto when = std::string(months[local.tm_mon]) + " " + std::to_string(local.tm_year + 1900);
        if (head("TOP OF THE MONTH", PaperKind::blue, 21, "/  MOST DOWNLOADED AND LIKED", when, "##seemonth")) {
            u.market_list = "browse";
            u.market_sort = "top";
            u.market_scroll = 0;
        }
        row("month", month, true);
    }
    y += 80 * k;
    y += crew_section(d, f, all, col, display, y, top, bottom, covers);
    crew = sorted(std::move(crew), "new");
    if (!crew.empty()) {
        if (head("FROM THE CREW", PaperKind::white, 33, "CREW CERTIFIED PACKS", "", "##seecrew")) {
            u.market_list = "crew";
            u.market_crew.clear();
            u.market_scroll = 0;
        }
        row("crew", crew, false);
    }
    fresh = sorted(all, "new");
    if (head("NEW RELEASES", PaperKind::grip, 41, "JUST DROPPED", "", "##seenew")) {
        u.market_list = "browse";
        u.market_sort = "new";
        u.market_scroll = 0;
    }
    row("new", fresh, false);
    y += 70 * k;
    y += cta_section(d, f, col, display, y, top, bottom);
    if (head("ALL-TIME FAVORITES", PaperKind::white, 51, "THE MOST LIKED, EVER", "", "##seefav")) {
        u.market_list = "browse";
        u.market_sort = "liked";
        u.market_scroll = 0;
    }
    row("liked", sorted(all, "liked"), false);
    // The foot of the page.
    y += 60 * k;
    if (y < bottom) {
        float fx = ix;
        d->AddText(f.heading, 22 * k, ImVec2(fx, y), IM_COL32_WHITE, "reskate");
        fx += text_size(f.heading, 22 * k, "reskate").x;
        d->AddText(f.heading, 22 * k, ImVec2(fx, y), blue, ".");
        fx += 30 * k;
        fx += text_spaced(d, f.heading, 12 * k, ImVec2(fx, y + 6 * k), faint, "PACKS LIVE ON THUNDERSTORE", 1.5f * k) + 30 * k;
        text_spaced(d, f.heading, 12 * k, ImVec2(fx, y + 6 * k), faint, "BUILT BY 22SH, FOURTH, NORMS, STEEZ", 1.5f * k);
        if (studio::market::show_samples) {
            const float sw = spaced_width(f.heading, 12 * k, "SAMPLE PACKS SHOWN", 1.5f * k);
            text_spaced(d, f.heading, 12 * k, ImVec2(ix + iw - sw, y + 6 * k), faint, "SAMPLE PACKS SHOWN", 1.5f * k);
        }
    }
    y += 60 * k;
    end_region(d, ImVec2(0, top), ImVec2(display.x, bottom), y + u.market_scroll - top, u.market_scroll, k);
}

// ---- the grid pages: BROWSE, LIKED, INSTALLED, MY UPLOADS, a search's results, the crew ----
float shop_grid(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::market::View& m, const PackList& packs, ImVec2 at, float width, float top,
                float bottom, Covers& covers) {
    const float k = f.k, gap_x = 26 * k, gap_y = 34 * k;
    const std::size_t columns = static_cast<std::size_t>(std::max(1, static_cast<int>((width + gap_x) / (300 * k + gap_x))));
    const float w = (width - gap_x * static_cast<float>(columns - 1)) / static_cast<float>(columns), h = shop_card_height(f, w);
    for (std::size_t i = 0; i < packs.size(); ++i) {
        const ImVec2 ca(at.x + static_cast<float>(i % columns) * (w + gap_x), at.y + static_cast<float>(i / columns) * (h + gap_y));
        if (ca.y + h < top || ca.y > bottom) continue;
        shop_card(d, f, v, m, *packs[i], ca, w, covers);
    }
    return static_cast<float>((packs.size() + columns - 1) / columns) * (h + gap_y);
}
void grid_page(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::market::View& m, std::string_view heading, std::string_view about,
               PackList list, bool orders, std::string_view empty, ImVec2 display, float top, float bottom, Covers& covers) {
    auto& u = ui();
    const float k = f.k;
    const ShopColumn col{std::max(0.0f, display.x / k * 0.5f - 960), std::min(1920.0f, display.x / k)};
    const float ix = col.inner() * k, iw = (col.width - 96) * k;
    float y = top + 30 * k;
    ImFont* big = anton(f, 64 * k);
    const auto [bt, bf] = caps_of(big, 64 * k);
    d->AddText(big, 64 * k, ImVec2(ix, y - bt), IM_COL32_WHITE, std::string(heading).c_str());
    const float hw = text_size(big, 64 * k, std::string(heading).c_str()).x;
    if (orders) {
        static const std::array<std::pair<const char*, const char*>, 6> sorts{
            {{"top", "TOP"}, {"downloads", "MOST DOWNLOADED"}, {"new", "NEWEST"}, {"liked", "MOST LIKED"}, {"az", "A-Z"}, {"crew", "CREW ONLY"}}};
        float sx = ix + iw;
        for (auto it = sorts.rbegin(); it != sorts.rend(); ++it) {
            const float bw = shop_button_width(f, it->second, 40 * k);
            sx -= bw;
            const bool on = std::string_view(it->first) == "crew" ? u.market_crew_only : u.market_sort == it->first;
            ImGui::PushID(it->first);
            if (shop_button(d, f, "##sort", ImVec2(sx, y + (bf - bt) * 0.5f - 20 * k), ImVec2(bw, 40 * k), it->second, on ? ShopLook::blue : ShopLook::line)) {
                if (std::string_view(it->first) == "crew") u.market_crew_only = !u.market_crew_only;
                else u.market_sort = it->first;
                u.market_scroll = 0;
            }
            ImGui::PopID();
            sx -= 6 * k;
        }
        if (u.market_crew_only) std::erase_if(list, [](const MarketPack* p) { return !p->crew; });
        list = sorted(std::move(list), u.market_sort);
    }
    text_spaced(d, f.heading, 13 * k, ImVec2(ix + hw + 22 * k, y + (bf - bt) * 0.5f - 7 * k), soft,
                std::to_string(list.size()) + (list.size() == 1 ? " PACK" : " PACKS"), 2 * k);
    y += (bf - bt) + 36 * k;
    if (!about.empty()) y += paragraph(d, f.body, 15 * k, ImVec2(ix, y), iw, soft, std::string(about)) + 18 * k;
    if (list.empty()) {
        paragraph(d, f.body, 17 * k, ImVec2(ix, y + 10 * k), iw, soft, std::string(empty));
        return;
    }
    begin_region(72, ImVec2(0, y), ImVec2(display.x, bottom), u.market_scroll, k);
    const float h = shop_grid(d, f, v, m, list, ImVec2(ix, y + 8 * k - u.market_scroll), iw, y, bottom, covers);
    end_region(d, ImVec2(0, y), ImVec2(display.x, bottom), h + 40 * k, u.market_scroll, k);
}
void crew_page(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::market::View& m, const PackList& all, ImVec2 display, float top, float bottom,
               Covers& covers) {
    auto& u = ui();
    const float k = f.k;
    const ShopColumn col{std::max(0.0f, display.x / k * 0.5f - 960), std::min(1920.0f, display.x / k)};
    const float ix = col.inner() * k, iw = (col.width - 96) * k;
    begin_region(72, ImVec2(0, top), ImVec2(display.x, bottom), u.market_scroll, k);
    float y = top + 30 * k - u.market_scroll;
    ImFont* big = anton(f, 64 * k);
    const auto [bt, bf] = caps_of(big, 64 * k);
    d->AddText(big, 64 * k, ImVec2(ix, y - bt), IM_COL32_WHITE, "THE CREW");
    text_spaced(d, f.heading, 13 * k, ImVec2(ix + text_size(big, 64 * k, "THE CREW").x + 22 * k, y + (bf - bt) * 0.5f - 7 * k), soft,
                "THE TESTERS WHO SHAPED THE STUDIO", 2 * k);
    y += (bf - bt) + 40 * k;
    const float w = 420 * k, gap = 22 * k;
    float x = ix;
    for (const auto& member : studio::market::crew_members) {
        if (x + w > ix + iw + 1) x = ix, y += 470 * k;
        member_card(d, f, member, all, ImVec2(x, y), w, covers);
        x += w + gap;
    }
    y += 470 * k;
    const auto* chosen = studio::market::crew_member(u.market_crew);
    const std::string title = chosen ? "PACKS BY " + upper(std::string(chosen->name)) : "CREW CERTIFIED PACKS";
    const auto label = paper_label(d, f, anton(f, 40 * k), 40 * k, ImVec2(ix, y), title, PaperKind::blue, 61, -1.2f, IM_COL32_WHITE, ImVec2(26 * k, 13 * k), 1 * k);
    if (chosen) {
        const float bw = shop_button_width(f, "ALL THE CREW", 40 * k);
        if (shop_button(d, f, "##allcrew", ImVec2(ix + label.x + 22 * k, y + label.y * 0.5f - 20 * k), ImVec2(bw, 40 * k), "ALL THE CREW", ShopLook::line))
            u.market_crew.clear();
    }
    y += label.y + 30 * k;
    PackList theirs;
    for (const auto* p : all)
        if (chosen ? same_team(p->owner, chosen->team) : p->crew) theirs.push_back(p);
    y += shop_grid(d, f, v, m, sorted(std::move(theirs), "new"), ImVec2(ix, y), iw, top, bottom, covers);
    end_region(d, ImVec2(0, top), ImVec2(display.x, bottom), y + u.market_scroll - top + 20 * k, u.market_scroll, k);
}

// ---- a pack's page ----
void pack_page_market(ImDrawList* d, const Fonts& f, const studio::View& v, const studio::market::View& m, const MarketPack& p, ImVec2 display, float top,
                      float bottom, Covers& covers) {
    auto& u = ui();
    const float k = f.k;
    const ShopColumn col{std::max(0.0f, display.x / k * 0.5f - 960), std::min(1920.0f, display.x / k)};
    const float ix = col.inner() * k, iw = (col.width - 96) * k;
    covers.add(p);
    if (p.readme == 0 && !p.sample) studio::market::want_readmes({p.full_name});
    const float bw = shop_button_width(f, "BACK", 40 * k) + 20 * k;
    if (shop_button(d, f, "##packback", ImVec2(ix, top + 14 * k), ImVec2(bw, 40 * k), "< BACK", ShopLook::line)) {
        u.market_open.clear();
        return;
    }
    const float region_top = top + 70 * k;
    begin_region(71, ImVec2(0, region_top), ImVec2(display.x, bottom), u.market_detail_scroll, k);
    float y = region_top + 22 * k - u.market_detail_scroll;
    // The cover, taped up.
    const float side = 470 * k;
    print_card(d, f, ImVec2(ix + 10 * k, y + 20 * k), ImVec2(side, side), -2.5f, upper(maker_of(p)).substr(0, 24) + " / v" + p.version, true,
               [&](ImVec2 a, ImVec2 b) { market_icon(d, f, p, a, b); });
    const float x = ix + 520 * k + 70 * k, w = ix + iw - x;
    float ry = y;
    // Its stickers.
    {
        float sx = x;
        if (p.crew) sx += crew_stamp(d, f, ImVec2(sx, ry), 18 * k, 600 + static_cast<std::uint32_t>(p.title.size()), -2).x + 18 * k;
        if (is_new(p)) sx += paper_label(d, f, marker(f, 18 * k), 18 * k, ImVec2(sx, ry), "new drop", PaperKind::blue, 610, 2, shop_ink, ImVec2(14 * k, 7 * k)).x + 18 * k;
        if (!p.stance.empty()) {
            auto low = p.stance;
            for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            paper_label(d, f, marker(f, 18 * k), 18 * k, ImVec2(sx, ry), low, PaperKind::grip, 620, -1, IM_COL32_WHITE, ImVec2(14 * k, 7 * k));
        }
        ry += 44 * k;
    }
    // Its name, as big as fits in two lines.
    {
        float size = 112 * k;
        const auto title = upper(p.title);
        while (size > 48 * k && anton(f, size)->CalcTextSizeA(size, FLT_MAX, w, title.c_str()).y > size * 2.3f) size -= 6 * k;
        ImFont* big = anton(f, size);
        const auto [bt, bf] = caps_of(big, size);
        ry += 16 * k;
        // Line by line, so the lines sit as close as the mockup's.
        std::vector<std::string> lines;
        {
            std::string current;
            std::size_t start = 0;
            while (start <= title.size()) {
                auto end = title.find(' ', start);
                if (end == std::string::npos) end = title.size();
                const auto word = title.substr(start, end - start);
                const auto joined = current.empty() ? word : current + " " + word;
                if (!current.empty() && big->CalcTextSizeA(size, FLT_MAX, 0, joined.c_str()).x > w) {
                    lines.push_back(current);
                    current = word;
                } else current = joined;
                start = end + 1;
                if (end == title.size()) break;
            }
            if (!current.empty()) lines.push_back(current);
        }
        for (const auto& l : lines) {
            d->AddText(big, size, ImVec2(x, ry - bt), shop_white, l.c_str());
            ry += (bf - bt) + 14 * k;
        }
        ry += 6 * k;
    }
    // Its maker: one of the crew opens their page.
    {
        d->AddText(f.heading, 20 * k, ImVec2(x, ry), IM_COL32_WHITE, "by");
        const float byw = text_size(f.heading, 20 * k, "by ").x;
        const auto maker = upper(maker_of(p));
        const ImVec2 ma(x + byw + 4 * k, ry), mb(ma.x + text_size(f.heading, 20 * k, maker.c_str()).x, ry + 24 * k);
        bool lit = false;
        if (p.crew && hit("##packmaker", ma, mb, &lit)) {
            u.market_list = "crew";
            u.market_crew = p.owner;
            u.market_open.clear();
            u.market_scroll = 0;
        }
        d->AddText(f.heading, 20 * k, ma, lit ? IM_COL32_WHITE : blue_light, maker.c_str());
        ry += 46 * k;
    }
    // Its numbers.
    {
        ImFont* num = anton(f, 40 * k);
        const auto [nt, nb] = caps_of(num, 40 * k);
        const auto age = days_old(p);
        const std::array<std::pair<std::string, const char*>, 4> stats{{{std::to_string(p.tricks.size()), "TRICKS"},
                                                                         {short_count(static_cast<std::uint64_t>(std::max(0, p.rating))), "LIKES"},
                                                                         {short_count(p.downloads), "DOWNLOADS"},
                                                                         {age > 999 ? std::string("-") : std::to_string(age) + "D", "OUT SINCE"}}};
        float sx = x;
        for (const auto& [value, word] : stats) {
            d->AddText(num, 40 * k, ImVec2(sx, ry - nt), IM_COL32_WHITE, value.c_str());
            const float ww = text_spaced(d, f.heading, 11 * k, ImVec2(sx, ry + (nb - nt) + 8 * k), soft, word, 1.6f * k);
            sx += std::max(ww, text_size(num, 40 * k, value.c_str()).x) + 40 * k;
        }
        ry += (nb - nt) + 52 * k;
    }
    // GET, the heart, Thunderstore.
    {
        const float bh = 56 * k;
        const auto g = get_button(v, m, p);
        if (shop_button(d, f, "##packget", ImVec2(x, ry), ImVec2(240 * k, bh), g.label, ShopLook::blue, g.enabled)) studio::market::install(p.full_name);
        const float lw = shop_button_width(f, p.favorite ? "LIKED" : "LIKE", bh, 1);
        if (shop_button(d, f, "##packlike", ImVec2(x + 252 * k, ry), ImVec2(lw, bh), p.favorite ? "LIKED" : "LIKE", ShopLook::line, true, p.favorite ? 2 : 1))
            like_pack(m, p);
        const float tw = shop_button_width(f, "ON THUNDERSTORE", bh);
        if (!p.sample && shop_button(d, f, "##packts", ImVec2(x + 264 * k + lw, ry), ImVec2(tw, bh), "ON THUNDERSTORE", ShopLook::line))
            studio::market::open_page(p.full_name);
        ry += bh + 18 * k;
    }
    if (!m.job.message.empty() && m.job.what.find(p.title) != std::string::npos)
        ry += paragraph(d, f.body, 14 * k, ImVec2(x, ry), w, m.job.failed ? skate_theme::warning : soft, m.job.message) + 10 * k;
    if (!p.installed.empty() && !removed_here(v, p)) {
        const auto where = p.installed_as.empty() ? std::string("your library") : p.installed_as;
        ry += paragraph(d, f.body, 14 * k, ImVec2(x, ry), w - 200 * k, blue_light, "In " + where + ": switch it on, or preview its tricks, in PACKS.") + 8 * k;
        if (shop_button(d, f, "##packgopacks", ImVec2(x, ry), ImVec2(190 * k, 40 * k), "GO TO PACKS", ShopLook::line)) {
            u.menu_tab = 1;
            u.pack_page = false;
        }
        ry += 54 * k;
    } else if (removed_here(v, p))
        ry += paragraph(d, f.body, 14 * k, ImVec2(x, ry), w, skate_theme::warning, "It was installed here, and its pack has been deleted since: REINSTALL puts it back.") +
              14 * k;
    // About it, and what is in it.
    const auto about = p.about.empty() ? p.description : p.about;
    ry += 10 * k;
    ry += paragraph(d, f.body, 16 * k, ImVec2(x, ry), std::min(w, 820 * k), IM_COL32(243, 243, 239, 220), about.empty() ? "No description." : about) + 26 * k;
    {
        ImFont* head = anton(f, 26 * k);
        const auto [ht, hb] = caps_of(head, 26 * k);
        const float hw = text_spaced(d, head, 26 * k, ImVec2(x, ry - ht), IM_COL32_WHITE, "WHAT'S IN IT", 1.5f * k);
        d->AddRectFilled(ImVec2(x + hw + 14 * k, ry + (hb - ht) * 0.5f), ImVec2(x + w, ry + (hb - ht) * 0.5f + 2 * k), IM_COL32(255, 255, 255, 36));
        ry += (hb - ht) + 22 * k;
    }
    if (p.readme <= 1) ry += paragraph(d, f.body, 14 * k, ImVec2(x, ry), w, faint, "Reading what is in it...") + 8 * k;
    else if (p.readme == 3) ry += paragraph(d, f.body, 14 * k, ImVec2(x, ry), w, faint, "Its list could not be read.") + 8 * k;
    else {
        // Each trick a tag: its name, and in blue what goes with it ("mid pop").
        float tx = x;
        const float th = 36 * k;
        for (const auto& trick : p.tricks) {
            const auto open = trick.find(" (");
            const auto name = trick.substr(0, open);
            const auto rest = open == std::string::npos ? std::string{} : trick.substr(open + 2, trick.size() - open - 3);
            const float nw = text_size(f.heading, 13 * k, name.c_str()).x, rw = rest.empty() ? 0 : text_size(f.heading, 13 * k, rest.c_str()).x + 6 * k;
            const float tw = nw + rw + 28 * k;
            if (tx > x && tx + tw > x + std::min(w, 900 * k)) {
                tx = x;
                ry += th + 10 * k;
            }
            d->AddRectFilled(ImVec2(tx, ry), ImVec2(tx + tw, ry + th), IM_COL32(18, 19, 24, 255));
            d->AddRect(ImVec2(tx, ry), ImVec2(tx + tw, ry + th), IM_COL32(1, 131, 255, 140), 0, 0, 1 * k);
            d->AddText(f.heading, 13 * k, ImVec2(tx + 14 * k, ry + 11 * k), IM_COL32_WHITE, name.c_str());
            if (!rest.empty()) d->AddText(f.heading, 13 * k, ImVec2(tx + 14 * k + nw + 6 * k, ry + 11 * k), blue_light, rest.c_str());
            tx += tw + 10 * k;
        }
        ry += th + 10 * k;
    }
    const float bottom_of = std::max(y + side + 120 * k, ry) + 40 * k;
    end_region(d, ImVec2(0, region_top), ImVec2(display.x, bottom), bottom_of + u.market_detail_scroll - region_top, u.market_detail_scroll, k);
}

// ---- the sample covers, made once ----
void make_sample_covers() {
    static std::atomic<bool> started{};
    if (!studio::market::show_samples || started.exchange(true)) return;
    try {
        std::thread([] {
            std::error_code error;
            for (std::size_t i = 0; i < studio::market::samples.size(); ++i) {
                const auto path = studio::market::sample_cover(i);
                if (std::filesystem::is_regular_file(path, error)) continue;
                const auto& s = studio::market::samples[i];
                CoverDesign c;
                c.base = std::string(s.base);
                c.banner = std::string(s.banner);
                c.font = std::string(s.font);
                c.place = std::string(s.place);
                c.title = std::string(s.title);
                c.tag = std::string(s.stance);
                c.seed = static_cast<std::uint32_t>(i * 7 + 3);
                const auto rgba = make_cover(c, 512);
                const auto png = cover_png(rgba, 512);
                if (png.empty()) continue;
                std::filesystem::create_directories(path.parent_path(), error);
                std::ofstream out(path, std::ios::binary);
                out.write(png.data(), static_cast<std::streamsize>(png.size()));
            }
            release_cover_art();
        }).detach();
    } catch (...) {}
}

// ---- the shop ----
void market_page(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    const float k = f.k;
    const auto display = ImGui::GetIO().DisplaySize;
    want_market_art();
    make_sample_covers();
    studio::market::refresh(false);
    const auto m = studio::market::view();
    Covers covers;
    PackList all;
    for (const auto& p : m.packs) all.push_back(&p);
    // ---- the shop's bar: its parts, the search, the account ----
    const ShopColumn col{std::max(0.0f, display.x / k * 0.5f - 960), std::min(1920.0f, display.x / k)};
    const float ix = col.inner() * k, iw = (col.width - 96) * k;
    const float bar_y = 104 * k, bar_h = 52 * k;
    std::size_t liked = 0, installed = 0, mine = 0;
    for (const auto& p : m.packs) liked += p.favorite, installed += !p.installed.empty(), mine += p.mine;
    struct Part {
        const char* key;
        const char* label;
        std::size_t count;
        bool shown;
    };
    const std::array<Part, 6> parts{{{"featured", "FEATURED", 0, true},
                                     {"browse", "BROWSE", all.size(), true},
                                     {"crew", "THE CREW", studio::market::crew_members.size(), true},
                                     {"liked", "LIKED", liked, true},
                                     {"installed", "INSTALLED", installed, true},
                                     {"mine", "MY UPLOADS", mine, !m.team.empty()}}};
    float x = ix;
    for (const auto& part : parts) {
        if (!part.shown) continue;
        const float fs = 15 * k;
        const float tw = spaced_width(f.heading, fs, part.label, 0.6f * k);
        const auto count = part.count ? std::to_string(part.count) : std::string{};
        const float cw = count.empty() ? 0 : text_size(f.heading, 11 * k, count.c_str()).x + 14 * k;
        const ImVec2 pa(x, bar_y), pb(x + tw + 36 * k + (cw > 0 ? cw + 8 * k : 0), bar_y + bar_h);
        ImGui::PushID(part.key);
        bool lit = false;
        if (hit("##marketpart", pa, pb, &lit)) {
            u.market_list = part.key;
            u.market_open.clear();
            u.market_crew.clear();
            u.market_query.fill(0);
            u.market_scroll = 0;
        }
        ImGui::PopID();
        const bool chosen = u.market_list == part.key && u.market_open.empty() && !u.market_query[0];
        if (chosen) {
            const auto shape = torn::piece(pb.x - pa.x, bar_h - 12 * k, 17 + static_cast<std::uint32_t>(std::string_view(part.key).size()), 2 * k, 7 * k, 7 * k);
            draw_paper(d, f, shape, ImVec2(pa.x, pa.y + 6 * k), 0, ImVec2(0, 0), PaperKind::blue, 17);
        }
        if (lit && nav().keys) chosen_outline(d, pa, pb, k);
        const auto [ct, cf] = caps_of(f.heading, fs);
        text_spaced(d, f.heading, fs, ImVec2(pa.x + 18 * k, pa.y + (bar_h - (cf - ct)) * 0.5f - ct), chosen || lit ? IM_COL32_WHITE : IM_COL32(243, 243, 239, 178),
                    part.label, 0.6f * k);
        if (cw > 0) {
            const ImVec2 qa(pa.x + 18 * k + tw + 8 * k, pa.y + (bar_h - 18 * k) * 0.5f);
            d->AddRectFilled(qa, ImVec2(qa.x + cw, qa.y + 18 * k), chosen ? IM_COL32(0, 0, 0, 64) : IM_COL32(255, 255, 255, 36), 9 * k);
            d->AddText(f.heading, 11 * k, ImVec2(qa.x + 7 * k, qa.y + 3 * k), IM_COL32_WHITE, count.c_str());
        }
        x = pb.x + 4 * k;
    }
    {
        // REFRESH and the account at the right, the search before them: wide, and always there.
        float rx = ix + iw;
        const float h = 40 * k, ry = bar_y + (bar_h - h) * 0.5f;
        const float rw = shop_button_width(f, m.loading ? "LOADING" : "REFRESH", h);
        rx -= rw;
        if (shop_button(d, f, "##marketrefresh", ImVec2(rx, ry), ImVec2(rw, h), m.loading ? "LOADING" : "REFRESH", ShopLook::line)) studio::market::refresh(true);
        const std::string account = m.team.empty() ? "SIGN IN TO THUNDERSTORE" : "TEAM: " + upper(m.team);
        const float aw = shop_button_width(f, account, h);
        rx -= 12 * k + aw;
        if (shop_button(d, f, "##marketaccount", ImVec2(rx, ry), ImVec2(aw, h), account, m.team.empty() ? ShopLook::blue : ShopLook::line)) u.sign_in = true;
        const float sw = std::clamp(rx - 12 * k - (x + 40 * k), 220 * k, 470 * k), sx = rx - 12 * k - sw;
        search_icon(d, ImVec2(sx - 18 * k, bar_y + bar_h * 0.5f), 20 * k, u.market_query[0] ? blue_light : soft);
        if (field(f, "##marketsearch", u.market_query, ImVec2(sx, bar_y + (bar_h - 34 * k) * 0.5f), sw, "Search a trick, a pack, a maker...")) u.market_open.clear();
        if (std::string(u.market_query.data()) != u.market_query_seen) {
            u.market_query_seen = u.market_query.data();
            if (!u.market_query_seen.empty()) {
                u.market_open.clear();
                u.market_scroll = 0;
            }
        }
    }
    // ---- what is going on, then the part shown ----
    float top = bar_y + bar_h + 8 * k;
    const auto banner = [&](const std::string& line, ImU32 colour, float fraction) {
        const ImVec2 ba(ix, top), bb(ix + iw, top + 34 * k);
        d->AddRectFilled(ba, bb, IM_COL32(10, 11, 14, 235));
        d->AddText(f.heading, 13 * k, ImVec2(ba.x + 14 * k, ba.y + 10 * k), colour, elide(f.heading, 13 * k, line, bb.x - ba.x - 28 * k).c_str());
        if (fraction >= 0) d->AddRectFilled(ImVec2(ba.x, bb.y - 4 * k), ImVec2(ba.x + (bb.x - ba.x) * fraction, bb.y), blue);
        top += 42 * k;
    };
    if (!m.error.empty()) banner(m.error, skate_theme::warning, -1);
    if (m.job.running) banner(upper(m.job.what) + "  " + std::to_string(static_cast<int>(m.job.fraction * 100)) + "%", blue_light, m.job.fraction);
    const float bottom = b.y;
    const auto terms = studio::native_search_terms(u.market_query.data());
    const MarketPack* open = nullptr;
    for (const auto* p : all)
        if (p->full_name == u.market_open) open = p;
    if (!open) u.market_open.clear();
    if (open) pack_page_market(d, f, v, m, *open, display, top, bottom, covers);
    else if (!terms.empty()) {
        PackList found;
        for (const auto* p : all)
            if (pack_matches(*p, terms)) found.push_back(p);
        grid_page(d, f, v, m, "RESULTS", "For \"" + std::string(u.market_query.data()) + "\": names, makers, and the tricks in each pack.", std::move(found), true,
                  "Nothing matches. Try a trick's name (kickflip, nollie), a maker, or fewer words.", display, top, bottom, covers);
    } else if (u.market_list == "browse")
        grid_page(d, f, v, m, "ALL PACKS", "", all, true, m.loading ? "Reading Thunderstore..." : "No packs out yet. Be the first: open a pack in PACKS and press SHARE.",
                  display, top, bottom, covers);
    else if (u.market_list == "crew") crew_page(d, f, v, m, all, display, top, bottom, covers);
    else if (u.market_list == "liked") {
        PackList list;
        for (const auto* p : all)
            if (p->favorite) list.push_back(p);
        grid_page(d, f, v, m, "LIKED", "", sorted(std::move(list), "az"), false, "Nothing liked yet: press the heart on a pack you like.", display, top, bottom,
                  covers);
    } else if (u.market_list == "installed") {
        PackList list;
        for (const auto* p : all)
            if (!p->installed.empty()) list.push_back(p);
        grid_page(d, f, v, m, "INSTALLED", "Packs you got from the MARKET. One marked REMOVED was deleted in PACKS: open it and REINSTALL to put it back.",
                  sorted(std::move(list), "az"), false, "Nothing installed from the MARKET yet.", display, top, bottom, covers);
    } else if (u.market_list == "mine") {
        PackList list;
        for (const auto* p : all)
            if (p->mine) list.push_back(p);
        grid_page(d, f, v, m, "MY UPLOADS", "Your team's packs on Thunderstore. To put out a new version, open the pack in PACKS and press UPDATE.",
                  sorted(std::move(list), "new"), false, "Nothing out from " + m.team + " yet: open a pack in PACKS and press SHARE.", display, top, bottom, covers);
    } else home_page(d, f, v, m, all, display, top, bottom, covers);
    covers.ask();
    (void)a;
}

// The studio's tabs in the MARKET's dress: no name at the left, the tabs in Anton, the one open on torn blue paper, Q and E
// as keys, CLOSE and BACK TO EDITOR as the shop's buttons.
void market_header(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display, float right) {
    auto& u = ui();
    const float k = f.k, size = 34 * k, h = 52 * k, y = 20 * k, side = 28 * k, gap = 6 * k, cap = 34 * k, cap_gap = 14 * k;
    ImFont* font = anton(f, size);
    const auto [ct, cf] = caps_of(font, size);
    float total = 0;
    for (const char* name : menu_tabs) total += spaced_width(font, size, name, 1.5f * k) + side * 2 + gap;
    total += (cap + cap_gap * 2) * 2 - gap;
    float x = display.x * 0.5f - total * 0.5f;
    const auto key = [&](const char* label, Glyph g) {
        const ImVec2 c(x + cap_gap, y + (h - cap) * 0.5f);
        if (u.pad_mode) glyph(d, f, c, g, u.playstation);
        else {
            d->AddRectFilled(c, ImVec2(c.x + cap, c.y + cap), IM_COL32(5, 6, 8, 128));
            d->AddRect(c, ImVec2(c.x + cap, c.y + cap), IM_COL32(255, 255, 255, 204), 0, 0, 2 * k);
            ImFont* kf = anton(f, 18 * k);
            const auto [kt, kb] = caps_of(kf, 18 * k);
            const float tw = text_size(kf, 18 * k, label).x;
            d->AddText(kf, 18 * k, ImVec2(c.x + (cap - tw) * 0.5f, c.y + (cap - (kb - kt)) * 0.5f - kt), IM_COL32_WHITE, label);
        }
        x += cap + cap_gap * 2;
    };
    key("Q", Glyph::lb);
    for (int i = 0; i < static_cast<int>(menu_tabs.size()); ++i) {
        const char* name = menu_tabs[static_cast<std::size_t>(i)];
        const float tw = spaced_width(font, size, name, 1.5f * k);
        const ImVec2 a(x, y), b(x + tw + side * 2, y + h);
        ImGui::PushID(i);
        bool lit = false;
        if (hit("##markettab", a, b, &lit)) u.menu_tab = i;
        ImGui::PopID();
        const bool chosen = u.menu_tab == i;
        if (chosen) {
            const auto shape = torn::piece(b.x - a.x, h - 4 * k, 5, 2.4f * k, 9 * k, 7 * k);
            const int start = d->VtxBuffer.Size;
            draw_paper(d, f, shape, a, 0, ImVec2(0, 0), PaperKind::blue, 5);
            rotate_since(d, start, -2 * shop_pi / 180, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f));
        }
        if (lit && nav().keys) chosen_outline(d, a, b, k);
        text_spaced(d, font, size, ImVec2(a.x + side, a.y + (h - (cf - ct)) * 0.5f - ct), chosen || lit ? IM_COL32_WHITE : IM_COL32(243, 243, 239, 158), name,
                    1.5f * k);
        x = b.x + gap;
    }
    x -= gap;
    key("E", Glyph::rb);
    float rx = right - 40 * k;
    const float bh = 44 * k;
    const float cw = shop_button_width(f, "CLOSE (F8)", bh, 4);
    rx -= cw;
    if (shop_button(d, f, "##closestudio", ImVec2(rx, 24 * k), ImVec2(cw, bh), "CLOSE (F8)", ShopLook::line, true, 4)) studio::request_close();
    if (v.has_clip) {
        const float ew = shop_button_width(f, "BACK TO EDITOR", bh);
        rx -= 12 * k + ew;
        if (shop_button(d, f, "##toeditor", ImVec2(rx, 24 * k), ImVec2(ew, bh), "BACK TO EDITOR", ShopLook::paper)) u.menu = false;
    }
}

void packs_page(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    want_pack_pictures(v);
    if (u.pack_settings) {
        pack_settings_page(d, f, v, a, b);
        return;
    }
    if (u.pack_page)
        if (const auto* pack = pack_named(v, v.open_pack)) {
            pack_page(d, f, v, *pack, a, b);
            return;
        }
    u.pack_page = false;
    packs_hub(d, f, v, a, b);
}
// ---- SETTINGS -------------------------------------------------------------------------------------
// As the game's own: a tile for each part (a picture and what it holds), and in each the settings as rows, a value at
// the right with arrows either side or a bar to slide, and what the chosen one does in a box beside them.
struct Setting {
    enum class Kind { choice, slider, action, folder, name, info };
    Kind kind{};
    std::string label, value, about;
    std::function<void(int)> step;    // choice: one along, -1 or 1
    float amount{};                   // slider: 0 to 1
    std::function<void(float)> slide; // slider: set it
    std::function<void()> act;        // action
    bool exporting{};                 // folder: which one
    bool enabled{true};
};
constexpr std::array<const char*, 4> settings_pages{"STUDIO", "SKATER", "TRICKS", "PACKS"};
constexpr std::array<const char*, 4> settings_blurbs{"The room, the camera, playback and the studio's own tools.",
                                                     "How the skater is posed: limits, keeping clear, the ground, his stance.",
                                                     "How your tricks play in game: on or off, the crossfade, matching, your name.",
                                                     "Where packs are exported to and imported from, and bringing them in."};
Setting choice(std::string label, std::string value, std::string about, std::function<void(int)> step) {
    Setting s;
    s.kind = Setting::Kind::choice;
    s.label = std::move(label);
    s.value = std::move(value);
    s.about = std::move(about);
    s.step = std::move(step);
    return s;
}
Setting on_off(std::string label, bool on, std::string about, std::function<void(bool)> set) {
    return choice(std::move(label), on ? "ON" : "OFF", std::move(about), [on, set = std::move(set)](int) { set(!on); });
}
Setting slider(std::string label, float amount, std::string value, std::string about, std::function<void(float)> slide) {
    Setting s;
    s.kind = Setting::Kind::slider;
    s.label = std::move(label);
    s.amount = amount;
    s.value = std::move(value);
    s.about = std::move(about);
    s.slide = std::move(slide);
    return s;
}
Setting button_row(std::string label, std::string about, std::function<void()> act) {
    Setting s;
    s.kind = Setting::Kind::action;
    s.label = std::move(label);
    s.about = std::move(about);
    s.act = std::move(act);
    return s;
}
Setting info_setting(std::string label, std::string value, std::string about) {
    Setting s;
    s.kind = Setting::Kind::info;
    s.label = std::move(label);
    s.value = std::move(value);
    s.about = std::move(about);
    return s;
}
std::vector<Setting> settings_of(int page, const studio::View& v) {
    auto& u = ui();
    std::vector<Setting> rows;
    switch (page) {
    case 0: {
        rows.push_back(on_off("Studio room", v.room,
                              "On: the studio opens in the game's own lit studio room, as skate.'s Skaters page shows it. Off: it opens where "
                              "you are. Takes effect the next time it opens.",
                              [](bool on) { studio::set_room(on); }));
        rows.push_back(choice("Camera", shots[static_cast<std::size_t>(std::clamp(u.camera_shot, 0, static_cast<int>(shots.size()) - 1))].name,
                              "A set view to work in. FRONT, SIDE, BACK and TOP look at him square on from his chest, his left, behind "
                              "him and above, as he stands when you pick it; BOARD is close on the deck, CINEMATIC wide and low, FILMER "
                              "the filmer's follow. It travels with him while you pose and while it plays. Moving the camera yourself "
                              "goes back to free. Key: V.",
                              [&v](int by) { next_shot(v, by); }));
        rows.push_back(on_off("Fisheye on FILMER", u.fisheye,
                              "FILMER looks through a fisheye, as a filmer's camera does: wide, bent at the edges, black in the corners.",
                              [](bool on) { ui().fisheye = on; }));
        {
            static constexpr std::array<float, 7> speeds{0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
            std::size_t at = 3;
            for (std::size_t i = 0; i < speeds.size(); ++i)
                if (std::abs(speeds[i] - v.speed) < 0.01f) at = i;
            rows.push_back(choice("Preview speed", std::format("{:.2f}x", v.speed),
                                  "Slows the studio's playback down to look at a trick closely. In game the trick follows the game's own timing.",
                                  [at](int by) { studio::set_speed(speeds[static_cast<std::size_t>(std::clamp(static_cast<int>(at) + by, 0, 6))]); }));
        }
        rows.push_back(on_off("Loop", v.loop, "Plays the timeline round and round. A clip whose end differs from its start can visibly jump when it loops.",
                              [](bool on) { studio::set_loop(on); }));
        rows.push_back(on_off("Joints on the skater", !u.hide_joints, "The dots on his joints and the gizmo on the chosen one. Off shows the pose clean. Key: J.",
                              [](bool on) { ui().hide_joints = !on; }));
        rows.push_back(on_off("Shortcuts panel", u.controls_open, "The list of keys (or buttons) at the editor's lower right. Key: F1.",
                              [](bool on) { ui().controls_open = on; }));
        rows.push_back(button_row("Export diagnostics",
                                  "Reproduce a problem, open the studio, then export: the last 15 seconds of poses, stances, timings, the board and "
                                  "the tricks in play, as text files to send.",
                                  [] {
                                      std::string m;
                                      studio::export_diagnostics(m);
                                      say(m);
                                  }));
        rows.push_back(button_row("Close the studio", "Back to skating. The camera and the game's HUD come back. Key: F8.", [] { studio::request_close(); }));
        if (v.has_clip) {
            rows.push_back(info_setting("Source of the open trick",
                                        v.source_kind == "native_asset" ? (v.source_position_codec == "native-piec-v1-extent" ? "native clip" : "older native clip")
                                        : v.source_kind == "render_capture" ? "in-game capture" : "older / unknown",
                                        "Where the trick you are editing came from: one of the game's own clips, or one recorded in game."));
            if (v.source_pose_count > 1) {
                const float hz = v.duration > 0 ? static_cast<float>(v.source_pose_count - 1) / v.duration : 0;
                rows.push_back(info_setting("Its sampling", std::format("{} poses, {:.0f} Hz", v.source_pose_count, hz),
                                            "How many poses the source has. Slower playback does not add frames that are not there."));
            }
            rows.push_back(info_setting("Starts from", v.entry, "The ground or a rail. Ground and rail-exit tricks are kept apart."));
        }
        break;
    }
    case 1: {
        rows.push_back(choice("Stance", v.stance_choice == 1 ? "REGULAR" : v.stance_choice == 2 ? "GOOFY" : v.showing_goofy ? "THE GAME'S (GOOFY)" : "THE GAME'S (REGULAR)",
                              "Which foot leads while you edit. A trick made leading with the other foot is mirrored into this stance the way "
                              "the game mirrors its own, so the leg you grab is the leg that moves. The game's: the Stance setting in the "
                              "ReSkate menu, Addons > GRIND EXITS.",
                              [c = v.stance_choice](int by) { studio::set_stance((c + by + 3) % 3); }));
        rows.push_back(on_off("Hands grab the board", v.grab_board,
                              "A hand brought near the deck closes its fingers round it, more the nearer it gets. Shown only: the fingers are "
                              "not keyed or saved.",
                              [](bool on) { studio::set_grab(on); }));
        rows.push_back(on_off("Joint limits", v.limits,
                              "On: knees and elbows bend one way and every joint stays in the range the game's own animation uses, while you "
                              "pose and in game. Off: joints go wherever you push them.",
                              [](bool on) { studio::set_joint_limits(on); }));
        rows.push_back(slider("Keep clear", u.anti_clip ? u.clear_force : 0.0f,
                              !u.anti_clip || u.clear_force <= 0.005f ? "OFF" : std::format("{:.0f}%", u.clear_force * 100),
                              "What you move is kept out of his body and off the deck. How wide his body is taken to be: low keeps the limbs "
                              "close and stops only the worst, high holds them well apart. The deck is never softened. Key: C.",
                              [](float amount) {
                                  ui().clear_force = amount;
                                  ui().anti_clip = amount > 0.005f;
                              }));
        rows.push_back(on_off("Push limbs aside", u.shove_limbs,
                              "A limb the one you move runs into gives way too: its hand or foot moves by the overlap and the rest bends to "
                              "follow. Off: only what you move is kept clear, which is steadier.",
                              [](bool on) { ui().shove_limbs = on; }));
        rows.push_back(slider("Ground force field", u.ground_force, u.ground_force <= 0.005f ? "OFF" : std::format("{:.0f}%", u.ground_force * 100),
                              "Keeps what you move above the floor: a hand, a foot, a knee or the board stops on the ground. How far above: "
                              "how much of the joint's own thickness is kept clear. Off lets everything through.",
                              [](float amount) { ui().ground_force = amount; }));
        rows.push_back(on_off("Snapping", u.snap, "A foot you move lands on the deck, at the height the feet ride at, and slides along the board. Key: N.",
                              [](bool on) { ui().snap = on; }));
        rows.push_back(on_off("Solo", v.solo,
                              "A turn moves that joint alone: the joints hanging off it are turned back by as much, so the limb below keeps "
                              "the way it faces. Key: O.",
                              [](bool on) { studio::set_solo(on); }));
        rows.push_back(choice("Gizmo", u.local_axes ? "OBJECT" : "WORLD",
                              "Which way the gizmo's arrows and rings lie: along the joint's own axes, or the world's. Key: L.",
                              [](int) { ui().local_axes = !ui().local_axes; }));
        break;
    }
    case 2: {
        rows.push_back(on_off("Custom tricks in game", v.play_in_game,
                              "On: your saved tricks play in game. The game leads: it says which clip it plays and how far into it it is, and "
                              "your keys go onto its pose there, so the trick looks as the studio shows it and lands when the game lands.",
                              [](bool on) { studio::set_play_in_game(on); }));
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) u.crossfade_slider = std::clamp((v.crossfade - 0.1f) / 0.7f, 0.0f, 1.0f);
            const float seconds = 0.1f + u.crossfade_slider * 0.7f;
            rows.push_back(slider("Crossfade", u.crossfade_slider, std::format("{:.2f} s", seconds),
                                  "How smoothly your trick hands back to the game when it goes into its next move - a manual, a push, the "
                                  "next trick. Short is snappy, long is buttery.",
                                  [](float amount) {
                                      ui().crossfade_slider = amount;
                                      static float said = -1;
                                      const float s = std::round((0.1f + amount * 0.7f) * 100) / 100;
                                      if (std::abs(s - said) > 0.005f) {
                                          said = s;
                                          studio::set_crossfade(s);
                                      }
                                  }));
        }
        rows.push_back(choice("Trick matching", v.front_foot ? "FRONT FOOT" : "EXACT STANCE",
                              "Front foot: a trick plays in every stance, mirrored onto your front foot when the other foot leads, as the game "
                              "does. Exact stance: only the one made on the foot that leads now; the game's own otherwise.",
                              [on = v.front_foot](int) { studio::set_front_foot(!on); }));
        {
            const auto build = studio::library_build();
            Setting s = button_row("Trick library",
                                   "The game's tricks, made on this PC from your own copy of skate. (nothing of the game's comes with the "
                                   "studio). Rebuild it after a game update. It takes about a minute; your saved tricks and packs are not "
                                   "touched, and the old library stays until the new one is whole.",
                                   [running = build.running] { running ? studio::stop_library_build() : studio::start_library_build(); });
            s.value = build.running    ? "BUILDING " + std::to_string(static_cast<int>(build.fraction * 100)) + "%  -  STOP"
                      : !build.exists  ? std::string("NOT BUILT  -  BUILD")
                      : build.outdated ? std::string("OUT OF DATE  -  REBUILD")
                      : build.built.empty() ? std::string("REBUILD")
                                            : "BUILT " + build.built + "  -  REBUILD";
            rows.push_back(std::move(s));
        }
        {
            Setting s;
            s.kind = Setting::Kind::name;
            s.label = "Made by";
            s.about = "Your name. It goes with every trick you record and every pack you make, so a trick passed on says whose it is.";
            rows.push_back(std::move(s));
        }
        break;
    }
    default: {
        {
            Setting s;
            s.kind = Setting::Kind::folder;
            s.label = "Export folder";
            s.exporting = true;
            s.value = v.export_path;
            s.about = "Where EXPORT PACK and EXPORT ALL TRICKS put their zip. It opens by itself after an export. CHOOSE picks "
                      "another folder, DEFAULT goes back to ReSkate.Tricks\\exports.";
            rows.push_back(std::move(s));
        }
        {
            Setting s;
            s.kind = Setting::Kind::folder;
            s.label = "Import folder";
            s.exporting = false;
            s.value = v.import_path;
            s.about = "Where IMPORT looks for packs: put a pack's zip there (your Downloads folder works well) and press IMPORT. "
                      "Imported zips are moved into an imported folder inside it.";
            rows.push_back(std::move(s));
        }
        rows.push_back(button_row("Import packs now", "Takes in every pack zip in the import folder: checked first, names already used get the next number.", [] {
            std::string m;
            studio::import_packs(m);
            say(m);
        }));
        rows.push_back(button_row("Export all your tricks", "Every saved trick in one zip, in the export folder.", [] {
            std::string m;
            studio::export_pack("", m);
            say(m);
        }));
        break;
    }
    }
    return rows;
}
// The tiles' pictures, drawn: a film camera, the skater on his board, a board mid-flip, a stack of packs.
void tile_picture(ImDrawList* d, int which, ImVec2 a, ImVec2 b, bool lit) {
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const ImU32 line = lit ? white : IM_COL32(225, 226, 230, 255), accent = lit ? blue_light : blue;
    const float s = std::min(b.x - a.x, b.y - a.y) / 200.0f, w = 5 * s;
    const auto at = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
    switch (which) {
    case 0: // a film camera on its stand
        d->AddRect(at(-55, -20), at(35, 40), line, 6 * s, 0, w);
        d->AddCircle(at(-30, -42), 20 * s, line, 28, w);
        d->AddCircle(at(10, -42), 20 * s, line, 28, w);
        d->AddTriangleFilled(at(35, 0), at(70, -18), at(70, 38), accent);
        d->AddLine(at(-10, 40), at(-30, 75), line, w);
        d->AddLine(at(-10, 40), at(10, 75), line, w);
        d->AddCircleFilled(at(-38, 10), 7 * s, accent, 16);
        break;
    case 1: { // the skater on his board
        d->AddCircle(at(0, -62), 14 * s, line, 24, w);
        d->AddLine(at(0, -48), at(-6, 0), line, w);
        d->AddLine(at(-2, -36), at(-40, -20), line, w);
        d->AddLine(at(-2, -36), at(36, -46), line, w);
        d->AddLine(at(-6, 0), at(-30, 24), line, w);
        d->AddLine(at(-30, 24), at(-34, 46), line, w);
        d->AddLine(at(-6, 0), at(24, 20), line, w);
        d->AddLine(at(24, 20), at(30, 46), line, w);
        d->AddLine(at(-70, 50), at(70, 50), accent, 7 * s);
        d->AddCircleFilled(at(-44, 62), 8 * s, line, 16);
        d->AddCircleFilled(at(44, 62), 8 * s, line, 16);
        break;
    }
    case 2: { // a board turning over, with the lines it leaves
        const float angle = -0.45f;
        const ImVec2 dir(std::cos(angle), std::sin(angle)), up(-dir.y, dir.x);
        const auto p = [&](float along, float across) { return at(dir.x * along + up.x * across, dir.y * along + up.y * across); };
        d->AddQuadFilled(p(-72, -12), p(72, -12), p(72, 12), p(-72, 12), accent);
        d->AddCircleFilled(p(-72, 0), 12 * s, accent, 20);
        d->AddCircleFilled(p(72, 0), 12 * s, accent, 20);
        d->AddCircleFilled(p(-40, 20), 7 * s, line, 16);
        d->AddCircleFilled(p(40, 20), 7 * s, line, 16);
        for (int i = 0; i < 3; ++i) {
            const float r = (88 + i * 14) * s;
            d->PathArcTo(c, r, 3.6f + i * 0.08f, 4.5f, 18);
            d->PathStroke(IM_COL32(255, 255, 255, 120 - i * 30), 0, 3 * s);
        }
        break;
    }
    default: // packs, stacked
        for (int i = 2; i >= 0; --i) {
            const float o = static_cast<float>(i) * 16;
            d->AddRectFilled(at(-58 + o, -50 + o), at(42 + o, 50 + o), IM_COL32(22, 22, 25, 255), 6 * s);
            d->AddRect(at(-58 + o, -50 + o), at(42 + o, 50 + o), i == 0 ? line : IM_COL32(200, 200, 205, 140), 6 * s, 0, w);
        }
        d->AddQuadFilled(at(-58, 6), at(42, -14), at(42, 4), at(-58, 24), accent);
        break;
    }
}
// A tile as skate.'s own: its rough, hand-cut shape (the game's own tile when it is there), filled.
void rough_tile(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 colour, float k) {
    if (!draw_studio_tile(d, a, b, 18 * k, colour)) skate_theme::rough_rect(d, a, b, colour, seed_of(a), k);
}
// SETTINGS: a tile for each part, as the game's own settings page has them - the name at the top left, a big worn white
// icon running off the bottom right corner, and the chosen one flooded blue with its name in black.
void settings_tiles(ImDrawList* d, const Fonts& f, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    const float k = f.k, gap = 14 * k;
    const float top = a.y + 92 * k;
    const float tile_w = std::min(640 * k, (b.x - a.x - gap) / 2), tile_h = std::min(300 * k, (b.y - top - gap) / 2);
    // The grid in the middle of the page, the title over its left edge.
    const float grid_w = 2 * tile_w + gap;
    a.x += std::max(0.0f, (b.x - a.x - grid_w) * 0.5f);
    text(d, f.title, 64 * k, ImVec2(a.x, a.y - 8 * k), white, "SETTINGS");
    static constexpr std::array<StudioPicture, 4> icons{StudioPicture::icon_studio, StudioPicture::icon_skater, StudioPicture::icon_tricks,
                                                        StudioPicture::icon_packs};
    for (int i = 0; i < 4; ++i) {
        const ImVec2 ta(a.x + static_cast<float>(i % 2) * (tile_w + gap), top + static_cast<float>(i / 2) * (tile_h + gap));
        const ImVec2 tb(ta.x + tile_w, ta.y + tile_h);
        ImGui::PushID(i);
        bool lit = false;
        if (hit("##tile", ta, tb, &lit)) {
            u.settings_page = i;
            u.settings_scroll = 0;
        }
        ImGui::PopID();
        rough_tile(d, ta, tb, lit ? blue : IM_COL32(8, 8, 9, 246), k);
        // The icon, big, running off the corner: the tile is its frame.
        ImGui::PushClipRect(ImVec2(ta.x + 6 * k, ta.y + 6 * k), ImVec2(tb.x - 6 * k, tb.y - 6 * k), true);
        const float side = tile_h * 1.28f;
        const ImVec2 ia(tb.x - side * 0.80f, ta.y - tile_h * 0.04f);
        if (!draw_studio_picture(d, icons[static_cast<std::size_t>(i)], ia, ImVec2(ia.x + side, ia.y + side), white))
            tile_picture(d, i, ImVec2(tb.x - tile_h * 1.1f, ta.y + tile_h * 0.1f), ImVec2(tb.x, tb.y), lit);
        ImGui::PopClipRect();
        const ImU32 ink = lit ? IM_COL32(8, 8, 10, 255) : white;
        text(d, f.heading, 34 * k, ImVec2(ta.x + 26 * k, ta.y + 20 * k), ink, settings_pages[static_cast<std::size_t>(i)]);
        paragraph(d, f.bold, 14 * k, ImVec2(ta.x + 28 * k, ta.y + 66 * k), tile_w * 0.48f, lit ? IM_COL32(8, 8, 10, 220) : soft,
                  settings_blurbs[static_cast<std::size_t>(i)]);
        if (lit && nav().keys) chosen_outline(d, ta, tb, k);
    }
}
void settings_rows(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 a, ImVec2 b) {
    auto& u = ui();
    const float k = f.k;
    if (action(d, f, "##settingsback", a, ImVec2(160 * k, 34 * k), "< SETTINGS", Look::ghost)) {
        u.settings_page = -1;
        return;
    }
    const int page = std::clamp(u.settings_page, 0, 3);
    text(d, f.title, 46 * k, ImVec2(a.x, a.y + 44 * k), white, settings_pages[static_cast<std::size_t>(page)]);
    const auto rows = settings_of(page, v);
    const float rows_w = std::min(1000 * k, (b.x - a.x) * 0.62f);
    const ImVec2 ra(a.x, a.y + 116 * k), rb(a.x + rows_w, b.y);
    std::string about_title, about;
    begin_region(61, ra, rb, u.settings_scroll, k);
    float y = ra.y - u.settings_scroll;
    auto& n = nav();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const bool tall = row.kind == Setting::Kind::folder;
        const float row_h = tall ? 104 * k : 58 * k;
        const ImVec2 a1(ra.x, y), b1(rb.x - 14 * k, y + row_h);
        y += row_h + 8 * k;
        ImGui::PushID(static_cast<int>(i));
        const bool sideways = row.kind == Setting::Kind::choice || row.kind == Setting::Kind::slider;
        bool lit = false;
        const float control_x = b1.x - 420 * k;
        const ImVec2 hit_b = tall ? ImVec2(b1.x, a1.y + 52 * k) : b1;
        const bool pressed = row.kind == Setting::Kind::name ? false : hit("##setting", a1, hit_b, &lit, row.enabled, sideways);
        if (row.kind == Setting::Kind::name) lit = nav_chosen("##yourname");
        skate_theme::rough_rect(d, a1, b1, lit ? IM_COL32(1, 131, 255, 250) : IM_COL32(26, 27, 30, 246), seed_of(a1), k);
        if (lit) theme::halftone(d, ImVec2(control_x - 90 * k, a1.y + 8 * k), 9, 4, IM_COL32(255, 255, 255, 40));
        if (lit && n.keys) chosen_outline(d, a1, b1, k);
        text(d, f.bold, 20 * k, ImVec2(a1.x + 24 * k, a1.y + (tall ? 14 * k : row_h * 0.5f - 12 * k)), white, row.label);
        const float mid_y = a1.y + (tall ? 26 * k : row_h * 0.5f);
        switch (row.kind) {
        case Setting::Kind::choice: {
            // < value >: the arrows at either end; a click on the left half steps back, anywhere else on.
            if (pressed) {
                const bool back = ImGui::GetIO().MousePos.x < control_x + 60 * k && !n.keys;
                if (row.step) row.step(back ? -1 : 1);
            }
            if (lit && n.lr && row.step) {
                row.step(n.lr);
                n.lr = 0;
            }
            text(d, f.heading, 20 * k, ImVec2(control_x + 14 * k, mid_y - 12 * k), lit ? white : soft, "<");
            const auto t = text_size(f.heading, 20 * k, ">");
            text(d, f.heading, 20 * k, ImVec2(b1.x - 24 * k - t.x, mid_y - 12 * k), lit ? white : soft, ">");
            const auto shown = elide(f.bold, 17 * k, row.value, 340 * k);
            const auto vw = text_size(f.bold, 17 * k, shown.c_str());
            text(d, f.bold, 17 * k, ImVec2((control_x + b1.x - vw.x) * 0.5f, mid_y - 10 * k), white, shown);
            break;
        }
        case Setting::Kind::slider: {
            const float bar_a = control_x + 14 * k, bar_b = b1.x - 100 * k;
            float amount = row.amount;
            if (ImGui::IsItemActive() && !n.keys) {
                amount = std::clamp((ImGui::GetIO().MousePos.x - bar_a) / std::max(1.0f, bar_b - bar_a), 0.0f, 1.0f);
                if (row.slide) row.slide(amount);
            }
            if (lit && n.lr && row.slide) {
                amount = std::clamp(amount + 0.05f * static_cast<float>(n.lr), 0.0f, 1.0f);
                row.slide(amount);
                n.lr = 0;
            }
            d->AddRectFilled(ImVec2(bar_a, mid_y - 4 * k), ImVec2(bar_b, mid_y + 4 * k), IM_COL32(255, 255, 255, 50), 4 * k);
            d->AddRectFilled(ImVec2(bar_a, mid_y - 4 * k), ImVec2(bar_a + (bar_b - bar_a) * amount, mid_y + 4 * k), lit ? white : blue, 4 * k);
            d->AddCircleFilled(ImVec2(bar_a + (bar_b - bar_a) * amount, mid_y), 9 * k, white, 20);
            const auto vw = text_size(f.bold, 16 * k, row.value.c_str());
            text(d, f.bold, 16 * k, ImVec2(b1.x - 22 * k - vw.x, mid_y - 9 * k), white, row.value);
            break;
        }
        case Setting::Kind::action: {
            if (pressed && row.act) row.act();
            const auto t = text_size(f.heading, 20 * k, ">");
            text(d, f.heading, 20 * k, ImVec2(b1.x - 26 * k - t.x, mid_y - 12 * k), lit ? white : soft, ">");
            if (!row.value.empty()) {
                const auto shown = elide(f.bold, 15 * k, row.value, 400 * k);
                const auto vw = text_size(f.bold, 15 * k, shown.c_str());
                text(d, f.bold, 15 * k, ImVec2(b1.x - 46 * k - t.x - vw.x, mid_y - 9 * k), lit ? white : soft, shown);
            }
            break;
        }
        case Setting::Kind::info: {
            const auto shown = elide(f.bold, 16 * k, row.value, 400 * k);
            const auto t = text_size(f.bold, 16 * k, shown.c_str());
            text(d, f.bold, 16 * k, ImVec2(b1.x - 22 * k - t.x, mid_y - 9 * k), lit ? white : soft, shown);
            break;
        }
        case Setting::Kind::name: {
            if (!u.your_name_filled && !ImGui::IsAnyItemActive()) {
                std::snprintf(u.your_name.data(), u.your_name.size(), "%s", v.your_name.c_str());
                u.your_name_filled = true;
            }
            bool edited = false;
            if (field(f, "##yourname", u.your_name, ImVec2(control_x, mid_y - 17 * k), 400 * k, "your name", &edited) || edited) {
                studio::set_your_name(u.your_name.data());
                say(std::string("Made by ") + u.your_name.data() + " from now on.");
            }
            break;
        }
        case Setting::Kind::folder: {
            text(d, f.bold, 14 * k, ImVec2(control_x, a1.y + 18 * k), lit ? white : soft, elide(f.bold, 14 * k, row.value, b1.x - control_x - 20 * k));
            const float by = a1.y + 58 * k, bw = 118 * k;
            const bool custom = !(row.exporting ? v.export_folder : v.import_folder).empty();
            if (action(d, f, "##choosefolder", ImVec2(b1.x - 3 * (bw + 8 * k) - 6 * k, by), ImVec2(bw, 34 * k), "CHOOSE", Look::paper))
                open_browser(v, row.exporting ? 1 : 2, {});
            if (action(d, f, "##openfolder", ImVec2(b1.x - 2 * (bw + 8 * k) - 6 * k, by), ImVec2(bw, 34 * k), "OPEN")) studio::open_pack_folder(row.exporting);
            if (action(d, f, "##defaultfolder", ImVec2(b1.x - (bw + 8 * k) - 6 * k, by), ImVec2(bw, 34 * k), "DEFAULT", Look::ghost, custom))
                studio::set_pack_folder(row.exporting, "");
            if (pressed) open_browser(v, row.exporting ? 1 : 2, {});
            break;
        }
        }
        if (lit) {
            about_title = row.label;
            about = row.about;
        }
        ImGui::PopID();
    }
    end_region(d, ra, rb, y + u.settings_scroll - ra.y, u.settings_scroll, k);
    // What the chosen row does, beside the rows.
    const ImVec2 ha(rb.x + 36 * k, ra.y), hb(b.x, std::min(b.y, ra.y + 320 * k));
    skate_theme::rough_rect(d, ha, hb, IM_COL32(18, 19, 22, 240), seed_of(ha), k);
    theme::halftone(d, ImVec2(hb.x - 60 * k, ha.y + 12 * k), 6, 4, IM_COL32(255, 255, 255, 20));
    if (about.empty()) {
        about_title = settings_pages[static_cast<std::size_t>(page)];
        about = settings_blurbs[static_cast<std::size_t>(page)];
    }
    text(d, f.heading, 20 * k, ImVec2(ha.x + 24 * k, ha.y + 20 * k), blue_light, elide(f.heading, 20 * k, upper(about_title), hb.x - ha.x - 48 * k));
    paragraph(d, f.bold, 16 * k, ImVec2(ha.x + 24 * k, ha.y + 58 * k), hb.x - ha.x - 48 * k, IM_COL32(222, 222, 226, 255), about);
}
void settings_page(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 a, ImVec2 b) {
    // In the middle of the screen, no wider than a 16:9 page's worth however wide the screen is.
    const float w = std::min(b.x - a.x, 1720 * f.k);
    a.x += (b.x - a.x - w) * 0.5f;
    b.x = a.x + w;
    if (ui().settings_page < 0) settings_tiles(d, f, a, b);
    else settings_rows(d, f, v, a, b);
}

// ---- cards over the screen ------------------------------------------------------------------------
// A card is drawn in a window of its own over the studio's, so nothing behind it takes the mouse; the pad chooses only
// what is on it. Behind it the screen is dimmed.
void begin_card(ImVec2 display, bool dim = true) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##animation_studio_card", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    nav().layer = 1;
    if (dim) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 165));
}
void end_card() {
    nav().layer = 0;
    ImGui::End();
}
// The card itself, in the middle: dark, with its title on a strip of paper.
std::pair<ImVec2, ImVec2> card_box(ImDrawList* d, const Fonts& f, ImVec2 display, float w, float h, const std::string& title, ImU32 tape = blue) {
    const float k = f.k;
    w = std::min(w, display.x - 40 * k);
    h = std::min(h, display.y - 40 * k);
    const ImVec2 a((display.x - w) * 0.5f, (display.y - h) * 0.5f), b(a.x + w, a.y + h);
    skate_theme::rough_rect(d, ImVec2(a.x + 8 * k, a.y + 8 * k), ImVec2(b.x + 8 * k, b.y + 8 * k), IM_COL32(0, 0, 0, 150), 95, k);
    skate_theme::rough_rect(d, a, b, IM_COL32(18, 19, 22, 252), 97, k);
    theme::halftone(d, ImVec2(b.x - 90 * k, a.y + 16 * k), 9, 5, IM_COL32(255, 255, 255, 18));
    const auto t = text_size(f.title, 38 * k, title.c_str());
    const ImVec2 ta(a.x + 26 * k, a.y + 20 * k), tb(ta.x + t.x + 30 * k, ta.y + t.y + 10 * k);
    if (!draw_studio_brush(d, ta, tb, tape)) skate_theme::rough_rect(d, ta, tb, tape, seed_of(ta), k);
    text(d, f.title, 38 * k, ImVec2(ta.x + 15 * k, ta.y + 5 * k), white, title);
    return {a, b};
}

// ---- choosing a picture or a folder -----------------------------------------------------------------
std::filesystem::path known_folder(const KNOWNFOLDERID& id) {
    PWSTR found = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &found)) && found) out = found;
    if (found) CoTaskMemFree(found);
    return out;
}
void open_browser(const studio::View& v, int mode, const std::string& pack) {
    auto& br = ui().browser;
    br.open = true;
    br.mode = mode;
    br.pack = pack;
    br.scroll = 0;
    br.error.clear();
    br.listed.clear();
    std::filesystem::path start = mode == 0 ? known_folder(FOLDERID_Pictures) : from_utf8(mode == 1 ? v.export_path : v.import_path);
    std::error_code e;
    if (start.empty() || !std::filesystem::is_directory(start, e)) start = from_utf8(v.library_path);
    br.dir = start;
}
bool picture_file(const std::filesystem::path& path) {
    auto ext = path.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".gif" || ext == L".tif" || ext == L".tiff";
}
void read_folder(Ui::Browser& br) {
    if (br.listed == br.dir) return;
    br.listed = br.dir;
    br.entries.clear();
    br.error.clear();
    br.scroll = 0;
    std::error_code e;
    for (auto it = std::filesystem::directory_iterator(br.dir, std::filesystem::directory_options::skip_permission_denied, e);
         !e && it != std::filesystem::directory_iterator(); it.increment(e)) {
        if (br.entries.size() >= 800) break;
        const auto& path = it->path();
        const auto name = to_utf8(path.filename());
        if (name.empty() || name[0] == '.' || name[0] == '$') continue;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        std::error_code fe;
        const bool folder = it->is_directory(fe);
        if (!folder && ((br.mode != 0 && br.mode != 3) || !picture_file(path))) continue;
        br.entries.push_back({name, path, folder});
    }
    if (e) br.error = "That folder cannot be read.";
    std::sort(br.entries.begin(), br.entries.end(), [](const auto& a, const auto& b) {
        if (a.folder != b.folder) return a.folder;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}
void folder_icon(ImDrawList* d, ImVec2 at, float k, ImU32 colour) {
    d->AddRectFilled(ImVec2(at.x, at.y + 3 * k), ImVec2(at.x + 10 * k, at.y + 7 * k), colour, 2 * k);
    d->AddRectFilled(ImVec2(at.x, at.y + 6 * k), ImVec2(at.x + 24 * k, at.y + 22 * k), colour, 2 * k);
}
void picture_icon(ImDrawList* d, ImVec2 at, float k, ImU32 colour) {
    d->AddRect(ImVec2(at.x, at.y + 2 * k), ImVec2(at.x + 24 * k, at.y + 22 * k), colour, 2 * k, 0, 2 * k);
    d->AddTriangleFilled(ImVec2(at.x + 3 * k, at.y + 19 * k), ImVec2(at.x + 10 * k, at.y + 10 * k), ImVec2(at.x + 16 * k, at.y + 19 * k), colour);
    d->AddCircleFilled(ImVec2(at.x + 17 * k, at.y + 8 * k), 2.5f * k, colour, 10);
}
void browser_card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& br = ui().browser;
    const float k = f.k;
    read_folder(br);
    const std::string title = br.mode == 0   ? "A PICTURE FOR " + upper(br.pack)
                              : br.mode == 3 ? "A PICTURE FOR THE COVER"
                              : br.mode == 1 ? "WHERE PACKS GO"
                                             : "WHERE PACKS COME FROM";
    const auto [a, b] = card_box(d, f, display, 1180 * k, 780 * k, title);
    const float top = a.y + 92 * k;
    text(d, f.bold, 15 * k, ImVec2(a.x + 28 * k, top - 14 * k), soft,
         br.mode == 0 ? "Any picture: it is cut square from its middle and made 512 pixels a side, so the pack stays small."
                      : "Open the folder you want, then press USE THIS FOLDER.");
    // ---- places ----
    const float places_w = 250 * k;
    float y = top + 20 * k;
    heading(d, f, ImVec2(a.x + 28 * k, y), places_w - 20 * k, "Places");
    y += 32 * k;
    std::vector<std::pair<std::string, std::filesystem::path>> places{{"Desktop", known_folder(FOLDERID_Desktop)},
                                                                      {"Pictures", known_folder(FOLDERID_Pictures)},
                                                                      {"Downloads", known_folder(FOLDERID_Downloads)},
                                                                      {"Documents", known_folder(FOLDERID_Documents)},
                                                                      {"ReSkate.Tricks", from_utf8(v.library_path)}};
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (drives & (1u << i)) {
            const wchar_t root[4]{static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
            const UINT type = GetDriveTypeW(root);
            if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) places.emplace_back(std::string(1, static_cast<char>('A' + i)) + ":\\", std::filesystem::path(root));
        }
    for (std::size_t i = 0; i < places.size(); ++i) {
        if (places[i].second.empty()) continue;
        const ImVec2 pa(a.x + 28 * k, y), pb(a.x + places_w, y + 36 * k);
        ImGui::PushID(static_cast<int>(i));
        bool lit = false;
        if (hit("##place", pa, pb, &lit)) br.dir = places[i].second;
        ImGui::PopID();
        const bool here = br.dir == places[i].second;
        skate_theme::rough_rect(d, pa, pb, here ? blue : lit ? IM_COL32(48, 50, 56, 248) : IM_COL32(26, 27, 30, 240), seed_of(pa), k * 0.6f);
        if (lit && nav().keys) chosen_outline(d, pa, pb, k);
        folder_icon(d, ImVec2(pa.x + 10 * k, pa.y + 6 * k), k, here ? white : soft);
        text(d, f.bold, 15 * k, ImVec2(pa.x + 44 * k, pa.y + 9 * k), white, places[i].first);
        y += 40 * k;
    }
    // ---- the folder ----
    const float lx = a.x + places_w + 30 * k, rx = b.x - 28 * k;
    const float bottom = b.y - 76 * k;
    {
        const bool can_up = br.dir.has_parent_path() && br.dir.parent_path() != br.dir;
        if (action(d, f, "##up", ImVec2(lx, top + 14 * k), ImVec2(80 * k, 34 * k), "UP", Look::dark, can_up) && can_up) br.dir = br.dir.parent_path();
        const auto where = to_utf8(br.dir);
        // The end of a long path is what tells you where you are.
        std::string shown = where;
        const float room = rx - lx - 100 * k;
        if (text_size(f.bold, 15 * k, shown.c_str()).x > room) {
            while (shown.size() > 4 && text_size(f.bold, 15 * k, ("..." + shown).c_str()).x > room) shown.erase(0, 1);
            shown = "..." + shown;
        }
        text(d, f.bold, 15 * k, ImVec2(lx + 96 * k, top + 22 * k), white, shown);
    }
    const ImVec2 la(lx, top + 62 * k), lb(rx, bottom);
    skate_theme::rough_rect(d, la, lb, IM_COL32(12, 12, 14, 240), seed_of(la), k * 0.6f);
    begin_region(73, ImVec2(la.x + 6 * k, la.y + 6 * k), ImVec2(lb.x - 4 * k, lb.y - 6 * k), br.scroll, k);
    float ly = la.y + 6 * k - br.scroll;
    std::optional<std::filesystem::path> into;
    std::optional<std::filesystem::path> chosen_picture;
    if (!br.error.empty()) {
        text(d, f.bold, 15 * k, ImVec2(la.x + 16 * k, ly + 10 * k), skate_theme::danger, br.error);
        ly += 40 * k;
    } else if (br.entries.empty()) {
        text(d, f.bold, 15 * k, ImVec2(la.x + 16 * k, ly + 10 * k), soft, br.mode == 0 ? "No pictures or folders here." : "No folders in here.");
        ly += 40 * k;
    }
    for (std::size_t i = 0; i < br.entries.size(); ++i) {
        const auto& e = br.entries[i];
        const ImVec2 ea(la.x + 6 * k, ly), eb(lb.x - 18 * k, ly + 36 * k);
        ly += 38 * k;
        ImGui::PushID(static_cast<int>(i));
        bool lit = false;
        if (hit("##entry", ea, eb, &lit)) {
            if (e.folder) into = e.path;
            else chosen_picture = e.path;
        }
        ImGui::PopID();
        if (lit) d->AddRectFilled(ea, eb, IM_COL32(1, 131, 255, 200), 3 * k);
        if (lit && nav().keys) chosen_outline(d, ea, eb, k);
        if (e.folder) folder_icon(d, ImVec2(ea.x + 10 * k, ea.y + 6 * k), k, lit ? white : IM_COL32(240, 200, 90, 255));
        else picture_icon(d, ImVec2(ea.x + 10 * k, ea.y + 6 * k), k, lit ? white : blue_light);
        text(d, f.bold, 15 * k, ImVec2(ea.x + 46 * k, ea.y + 9 * k), white, elide(f.bold, 15 * k, e.name, eb.x - ea.x - 60 * k));
    }
    end_region(d, ImVec2(la.x + 6 * k, la.y + 6 * k), ImVec2(lb.x - 4 * k, lb.y - 6 * k), ly + br.scroll - (la.y + 6 * k), br.scroll, k);
    if (into) br.dir = *into;
    if (chosen_picture && br.mode == 3) {
        // For the cover designer: behind the banner, or the cover as it is.
        ui().cover.own = to_utf8(*chosen_picture);
        ui().cover.base = "own";
        preview_cover(ui().cover);
        br.open = false;
    } else if (chosen_picture) {
        detail::make_studio_pack_picture(br.pack, to_utf8(*chosen_picture));
        say("Making " + br.pack + "'s picture...");
        br.open = false;
    }
    // ---- the buttons ----
    const float by = b.y - 60 * k;
    if (action(d, f, "##cancel", ImVec2(rx - 160 * k, by), ImVec2(160 * k, 42 * k), "CANCEL", Look::ghost)) br.open = false;
    if (br.mode != 0 && action(d, f, "##usefolder", ImVec2(rx - 160 * k - 12 * k - 240 * k, by), ImVec2(240 * k, 42 * k), "USE THIS FOLDER", Look::blue)) {
        studio::set_pack_folder(br.mode == 1, to_utf8(br.dir));
        br.open = false;
    }
}

// ---- after saving, and back from testing ----------------------------------------------------------
// The packs a saved trick is in, as chips that put it in and take it out, and one more to make a new pack with it.
float pack_chips(ImDrawList* d, const Fonts& f, const studio::View& v, const std::string& name, ImVec2 at, float width) {
    const float k = f.k;
    float x = at.x, y = at.y;
    for (const auto& pack : v.packs) {
        const bool in = std::find(pack.members.begin(), pack.members.end(), name) != pack.members.end();
        const float w = chip_width(f, pack.name);
        if (x + w > at.x + width) x = at.x, y += 38 * k;
        ImGui::PushID(pack.name.c_str());
        if (chip(d, f, "##packchip", ImVec2(x, y), pack.name, in, nullptr, 32 * k)) {
            std::string m;
            if (studio::set_pack_member(pack.name, name, !in, m) && !in) ask_stances(v, pack.name, name);
            say(m);
        }
        ImGui::PopID();
        x += w + 8 * k;
    }
    const float w = chip_width(f, "+ NEW PACK");
    if (x + w > at.x + width) x = at.x, y += 38 * k;
    if (chip(d, f, "##packchipnew", ImVec2(x, y), "+ NEW PACK", false, nullptr, 32 * k)) {
        const auto pack = unique_pack_name(v);
        std::string m;
        if (studio::create_preset_pack(m, pack) && studio::set_pack_member(pack, name, true, m)) ask_stances(v, pack, name);
        say(m);
    }
    return y + 38 * k - at.y;
}
// ---- the MARKET's cards ------------------------------------------------------------------------------
// A text field that does not show what is typed (a token).
bool secret_field(const Fonts& f, const char* id, std::array<char, 512>& buffer, ImVec2 at, float width, const char* hint) {
    const float k = f.k;
    ImGui::SetCursorScreenPos(at);
    ImGui::PushItemWidth(width);
    ImGui::PushFont(f.bold);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * k, 8 * k));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(20, 21, 24, 255));
    const bool entered = ImGui::InputTextWithHint(id, hint, buffer.data(), buffer.size(), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::PopItemWidth();
    return entered;
}
// SIGN IN: Thunderstore has no sign-in for other apps, so a team links with a service account token of its own.
void sign_in_card(ImDrawList* d, const Fonts& f, const studio::View& /*v*/, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto market = studio::market::view();
    const auto [a, b] = card_box(d, f, display, 900 * k, 620 * k, "SIGN IN TO THUNDERSTORE");
    const float x = a.x + 30 * k, w = b.x - a.x - 60 * k;
    float y = a.y + 92 * k;
    y += paragraph(d, f.bold, 15 * k, ImVec2(x, y), w, soft,
                   "Your packs are published to Thunderstore (thunderstore.io/c/reskate) under your team. Thunderstore has no sign-in for other "
                   "apps, so your team gives the studio a token of its own:") + 12 * k;
    static constexpr std::array<const char*, 3> steps{
        "1  On thunderstore.io, sign in and open Settings > Teams (make a team if you have none: its name is your packs' maker).",
        "2  Open your team, then Service accounts > Add service account. Copy the token it shows (it is shown once).",
        "3  Type your team's name and paste the token below, then LINK.",
    };
    for (const char* step : steps) y += paragraph(d, f.bold, 14 * k, ImVec2(x, y), w, white, step) + 8 * k;
    y += 6 * k;
    if (action(d, f, "##opents", ImVec2(x, y), ImVec2(260 * k, 40 * k), "OPEN THUNDERSTORE", Look::dark))
        ShellExecuteW(nullptr, L"open", L"https://thunderstore.io/settings/teams/", nullptr, nullptr, SW_SHOWNORMAL);
    y += 56 * k;
    text(d, f.heading, 13 * k, ImVec2(x, y), blue_light, "TEAM NAME");
    text(d, f.heading, 13 * k, ImVec2(x + 330 * k, y), blue_light, "SERVICE ACCOUNT TOKEN");
    y += 22 * k;
    if (u.ts_team[0] == 0 && !market.team.empty()) std::snprintf(u.ts_team.data(), u.ts_team.size(), "%s", market.team.c_str());
    field(f, "##tsteam", u.ts_team, ImVec2(x, y), 310 * k, "YourTeam");
    const bool entered = secret_field(f, "##tstoken", u.ts_token, ImVec2(x + 330 * k, y), w - 330 * k, "tss_...");
    y += 52 * k;
    paragraph(d, f.bold, 12 * k, ImVec2(x, y), w, faint,
              "The token is kept encrypted on this PC for your Windows user and only ever sent to thunderstore.io, with your uploads. "
              "Delete the service account on Thunderstore to cancel it at any time.");
    const float bw = 200 * k, by = b.y - 70 * k;
    if (action(d, f, "##tslink", ImVec2(b.x - 30 * k - bw, by), ImVec2(bw, 46 * k), "LINK", Look::blue) || entered) {
        std::string m;
        if (studio::market::link_account(u.ts_team.data(), u.ts_token.data(), m)) {
            SecureZeroMemory(u.ts_token.data(), u.ts_token.size());
            u.sign_in = false;
        }
        say(m);
    }
    if (!market.team.empty() && action(d, f, "##tsunlink", ImVec2(b.x - 40 * k - 2 * bw, by), ImVec2(bw, 46 * k), "SIGN OUT", Look::danger)) {
        studio::market::unlink_account();
        u.sign_in = false;
        say("Signed out of Thunderstore: the token is gone from this PC.");
    }
    if (action(d, f, "##tscancel", ImVec2(x, by), ImVec2(160 * k, 46 * k), "CANCEL", Look::ghost)) u.sign_in = false;
}
// SHARE: a pack's description, and the ways to share it - a zip to send, a Thunderstore package, or published straight to
// the MARKET with a linked team.
// ---- the cover designer ---------------------------------------------------------------------------
// A pack's cover made from the shop's pieces (studio_cover_maker.h): what is behind it, its name on a banner in one of the
// shop's fonts, where the banner goes, a tag in a corner - or the player's own picture as it is. The preview is made off
// the render thread each time something changes (only the newest asked for is made), and USE THIS COVER makes it the
// pack's picture and keeps how it was made with the pack.
struct CoverWork {
    std::mutex mutex;
    CoverDesign want;
    unsigned wanted{}, made{};
    bool busy{}, saving{};
};
CoverWork& cover_work() {
    static auto* w = new CoverWork;
    return *w;
}
void preview_cover(const CoverDesign& design) {
    auto& w = cover_work();
    std::lock_guard lock(w.mutex);
    w.want = design;
    ++w.wanted;
    if (w.busy) return;
    w.busy = true;
    try {
        std::thread([] {
            auto& w = cover_work();
            for (;;) {
                CoverDesign design;
                unsigned number = 0;
                {
                    std::lock_guard lock(w.mutex);
                    if (w.made == w.wanted) {
                        w.busy = false;
                        return;
                    }
                    design = w.want;
                    number = w.wanted;
                }
                show_cover_preview(make_cover(design, 640), 640);
                std::lock_guard lock(w.mutex);
                w.made = number;
            }
        }).detach();
    } catch (...) { w.busy = false; }
}
void open_cover_designer(const studio::View& v, const std::string& pack) {
    auto& u = ui();
    u.cover_pack = pack;
    u.cover_scroll = 0;
    CoverDesign design;
    design.title = pack;
    if (const auto* p = pack_named(v, pack); p && !p->pack.cover.empty()) read_cover_design(p->pack.cover, design);
    u.cover = design;
    std::snprintf(u.cover_title.data(), u.cover_title.size(), "%s", design.title.c_str());
    preview_cover(design);
}
void cover_changed() {
    auto& u = ui();
    preview_cover(u.cover);
}
// A choice among others: its words (in `font`, when given), a blue edge when it is the one.
bool cover_choice(ImDrawList* d, const Fonts& f, const char* id, ImVec2 at, std::string_view label, bool on, float& width, ImFont* font = nullptr,
                  float size = 0) {
    const float k = f.k, h = 40 * k;
    ImFont* face = font ? font : f.heading;
    const float fs = size > 0 ? size : 12 * k;
    const float tw = font ? face->CalcTextSizeA(fs, FLT_MAX, 0, label.data(), label.data() + label.size()).x : spaced_width(face, fs, label, 0.5f * k);
    width = tw + 28 * k;
    const ImVec2 b(at.x + width, at.y + h);
    bool lit = false;
    const bool pressed = hit(id, at, b, &lit);
    d->AddRectFilled(at, b, on ? IM_COL32(1, 131, 255, 42) : IM_COL32(12, 13, 16, 255));
    d->AddRect(at, b, on ? blue : IM_COL32(255, 255, 255, lit ? 128 : 42), 0, 0, 2 * k);
    if (lit && nav().keys) chosen_outline(d, at, b, k);
    const auto [ct, cf] = caps_of(face, fs);
    const float ty = at.y + (h - (cf - ct)) * 0.5f - ct;
    if (font) d->AddText(face, fs, ImVec2(at.x + 14 * k, ty), IM_COL32_WHITE, label.data(), label.data() + label.size());
    else text_spaced(d, face, fs, ImVec2(at.x + 14 * k, ty), IM_COL32_WHITE, label, 0.5f * k);
    return pressed;
}
void cover_card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto* pack = pack_named(v, u.cover_pack);
    if (!pack) {
        u.cover_pack.clear();
        return;
    }
    want_market_art();
    const ImVec2 size(std::min(1500 * k, display.x - 40 * k), std::min(860 * k, display.y - 40 * k));
    const ImVec2 a((display.x - size.x) * 0.5f, (display.y - size.y) * 0.5f), b(a.x + size.x, a.y + size.y);
    d->AddRectFilled(ImVec2(a.x + 14 * k, a.y + 16 * k), ImVec2(b.x + 14 * k, b.y + 16 * k), IM_COL32(0, 0, 0, 128));
    d->AddRectFilled(a, b, IM_COL32(18, 19, 23, 255));
    paper_label(d, f, anton(f, 40 * k), 40 * k, ImVec2(a.x + 34 * k, a.y + 28 * k), "PACK COVER", PaperKind::blue, 71, -1.2f, IM_COL32_WHITE,
                ImVec2(22 * k, 12 * k), 1 * k);
    text_spaced(d, f.heading, 13 * k, ImVec2(a.x + 300 * k, a.y + 48 * k), soft, upper(pack->name), 2 * k);
    const float dw = shop_button_width(f, "DONE", 44 * k);
    if (shop_button(d, f, "##coverdone", ImVec2(b.x - 30 * k - dw, a.y + 30 * k), ImVec2(dw, 44 * k), "DONE", ShopLook::line)) {
        u.cover_pack.clear();
        release_cover_art();
        return;
    }
    // ---- the cover as it is ----
    const float side = std::min(600 * k, size.y - 200 * k);
    const ImVec2 pa(a.x + 34 * k, a.y + 120 * k), pb(pa.x + side, pa.y + side);
    d->AddRectFilled(ImVec2(pa.x + 10 * k, pa.y + 12 * k), ImVec2(pb.x + 10 * k, pb.y + 12 * k), IM_COL32(0, 0, 0, 128));
    ImTextureID preview{};
    if (cover_preview(preview)) d->AddImage(preview, pa, pb);
    else {
        d->AddRectFilled(pa, pb, IM_COL32(10, 11, 14, 255));
        text(d, f.heading, 15 * k, ImVec2(pa.x + 20 * k, pa.y + 20 * k), soft, "Making it...");
    }
    paragraph(d, f.body, 13 * k, ImVec2(pa.x, pb.y + 18 * k), side, soft,
              "This is the picture people see on your pack: in the MARKET, on Thunderstore, and in PACKS. Pick what is behind it, put its name on a banner "
              "in one of the shop's fonts, and stick a tag on it. Or use a picture of your own, as it is.");
    // ---- the choices ----
    const float ox = pb.x + 44 * k, ow = b.x - 34 * k - ox;
    const ImVec2 ra(ox, a.y + 120 * k), rb(b.x - 20 * k, b.y - 30 * k);
    begin_region(83, ra, rb, u.cover_scroll, k);
    float y = ra.y - u.cover_scroll;
    const auto group = [&](std::string_view title, std::string_view note) {
        ImFont* head = anton(f, 22 * k);
        const auto [ht, hb] = caps_of(head, 22 * k);
        const float w = text_spaced(d, head, 22 * k, ImVec2(ox, y - ht), IM_COL32_WHITE, title, 1.5f * k);
        if (!note.empty()) d->AddText(f.body, 12 * k, ImVec2(ox + w + 12 * k, y + (hb - ht) - 13 * k), faint, std::string(note).c_str());
        y += (hb - ht) + 14 * k;
    };
    const auto choices = [&](const char* id, const std::vector<std::pair<std::string, std::string>>& options, std::string& value, bool in_font = false) {
        float x = ox;
        for (const auto& [key, label] : options) {
            ImFont* font = nullptr;
            float size = 0;
            if (in_font) {
                font = key == "bebas" ? f.covers[0] : key == "dirt" ? f.covers[1] : key == "sedgwick" ? f.covers[2] : key == "knewave" ? f.covers[3] : anton(f, 24 * k);
                size = 24 * k;
            }
            float w = 0;
            ImGui::PushID((std::string(id) + key).c_str());
            const float probe = font ? font->CalcTextSizeA(size, FLT_MAX, 0, label.c_str()).x + 28 * k : spaced_width(f.heading, 12 * k, label, 0.5f * k) + 28 * k;
            if (x > ox && x + probe > ox + ow) {
                x = ox;
                y += 50 * k;
            }
            if (cover_choice(d, f, "##choice", ImVec2(x, y), label, value == key && !u.cover.as_is, w, font, size)) {
                value = key;
                u.cover.as_is = false;
                cover_changed();
            }
            ImGui::PopID();
            x += w + 10 * k;
        }
        y += 50 * k + 18 * k;
    };
    group("YOUR OWN PICTURE", "as it is, nothing put on it");
    {
        if (toggle(d, f, "##coverasis", ImVec2(ox, y), u.cover.as_is)) {
            if (u.cover.own.empty()) open_browser(v, 3, pack->name);
            else {
                u.cover.as_is = !u.cover.as_is;
                cover_changed();
            }
        }
        d->AddText(f.body, 13 * k, ImVec2(ox + 78 * k, y + 7 * k), soft,
                   u.cover.own.empty() ? "Use a picture of mine as the cover (pick one first)" : "Use my picture as the cover");
        y += 30 * k + 26 * k;
    }
    group("BEHIND IT", "the skater, a texture, or your picture with the banner on it");
    {
        std::vector<std::pair<std::string, std::string>> bases{{"skater", "SKATER"},   {"print", "PRINT"},     {"grip", "GRIP TAPE"},
                                                                {"concrete", "CONCRETE"}, {"paper", "WORN PAPER"}, {"blue", "BLUE PAPER"}};
        if (!u.cover.own.empty()) bases.emplace_back("own", "MY PICTURE");
        choices("base", bases, u.cover.base);
        y -= 18 * k;
        float w = 0;
        if (cover_choice(d, f, "##coverpick", ImVec2(ox, y), u.cover.own.empty() ? "+ PICK MY PICTURE" : "+ ANOTHER PICTURE", false, w)) open_browser(v, 3, pack->name);
        y += 50 * k + 18 * k;
    }
    group("PACK TITLE", "");
    {
        field(f, "##covertitle", u.cover_title, ImVec2(ox, y), std::min(ow, 700 * k), "The pack's name on its cover");
        if (std::string(u.cover_title.data()) != u.cover.title) {
            u.cover.title = u.cover_title.data();
            cover_changed();
        }
        y += 34 * k + 26 * k;
    }
    group("FONT", "five, all in the shop's style");
    choices("font", {{"anton", "ANTON"}, {"bebas", "BEBAS"}, {"dirt", "DIRT"}, {"sedgwick", "SEDGWICK"}, {"knewave", "KNEWAVE"}}, u.cover.font, true);
    group("BANNER", "what the title sits on");
    choices("banner", {{"blue", "BLUE TORN"}, {"white", "WORN PAPER"}, {"grip", "GRIP STRIP"}, {"tape", "TAPE"}, {"none", "NONE"}}, u.cover.banner);
    group("WHERE", "");
    choices("place", {{"top", "TOP"}, {"middle", "MIDDLE"}, {"bottom", "BOTTOM"}}, u.cover.place);
    group("TAG", "a sticker in the corner");
    {
        std::vector<std::pair<std::string, std::string>> tags{{"", "NONE"}};
        for (const char* t : {"ALL STANCES", "REGULAR", "GOOFY", "SWITCH", "FAKIE", "NOLLIE", "FLIPS", "GRINDS", "GRABS"}) tags.emplace_back(t, t);
        choices("tag", tags, u.cover.tag);
    }
    // ---- use it ----
    auto& work = cover_work();
    bool saving = false;
    {
        std::lock_guard lock(work.mutex);
        saving = work.saving;
    }
    const float bh = 56 * k;
    const float uw = shop_button_width(f, "USE THIS COVER", bh);
    if (shop_button(d, f, "##coveruse", ImVec2(ox, y), ImVec2(uw, bh), saving ? "SAVING..." : "USE THIS COVER", ShopLook::blue, !saving)) {
        const auto design = u.cover;
        const auto name = pack->name;
        {
            std::lock_guard lock(work.mutex);
            work.saving = true;
        }
        try {
            std::thread([design, name] {
                const auto png = cover_png(make_cover(design, static_cast<int>(studio::thumbnail_side)), static_cast<int>(studio::thumbnail_side));
                std::string message;
                if (png.empty()) say("The cover could not be made.");
                else if (studio::set_pack_thumbnail(name, png, message)) {
                    studio::set_pack_cover_design(name, cover_design_json(design), message);
                    say("The cover is " + name + "'s picture now.");
                } else say(message);
                auto& w = cover_work();
                std::lock_guard lock(w.mutex);
                w.saving = false;
            }).detach();
        } catch (...) {
            std::lock_guard lock(work.mutex);
            work.saving = false;
        }
    }
    const float sw = shop_button_width(f, "SHUFFLE THE TEAR", bh);
    if (shop_button(d, f, "##covershuffle", ImVec2(ox + uw + 12 * k, y), ImVec2(sw, bh), "SHUFFLE THE TEAR", ShopLook::line)) {
        u.cover.seed = (u.cover.seed * 31 + 7) % 997 + 1;
        cover_changed();
    }
    y += bh + 20 * k;
    end_region(d, ra, rb, y + u.cover_scroll - ra.y, u.cover_scroll, k);
}

// The heart's card: liked here; Thunderstore counts a like only made on its own site, so it offers to open the pack there.
void like_card(ImDrawList* d, const Fonts& f, const studio::View& /*v*/, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto m = studio::market::view();
    const MarketPack* p = nullptr;
    for (const auto& q : m.packs)
        if (q.full_name == u.like_pack) p = &q;
    if (!p) {
        u.like_pack.clear();
        return;
    }
    const auto [a, b] = card_box(d, f, display, 760 * k, 420 * k, "LIKED!");
    const float x = a.x + 30 * k, w = b.x - a.x - 60 * k;
    float y = a.y + 100 * k;
    heart_icon(d, ImVec2(b.x - 90 * k, a.y + 52 * k), 54 * k, blue, true);
    y += paragraph(d, f.heading, 18 * k, ImVec2(x, y), w - 60 * k, white, upper(p->title) + " IS IN YOUR LIKED LIST.") + 14 * k;
    y += paragraph(d, f.bold, 15 * k, ImVec2(x, y), w, soft,
                   "Thunderstore counts a like only when it is made on its own site, by someone signed in there, so the studio cannot add yours for you. "
                   "Open the pack's page and press its thumbs up to count it there too.") +
         22 * k;
    if (toggle(d, f, "##likeask", ImVec2(x, y), m.like_prompt)) studio::market::set_like_prompt(!m.like_prompt);
    text(d, f.bold, 14 * k, ImVec2(x + 76 * k, y + 6 * k), soft, "Ask every time I like a pack");
    if (action(d, f, "##likeopen", ImVec2(x, b.y - 70 * k), ImVec2(260 * k, 46 * k), "OPEN ITS PAGE", Look::blue)) {
        studio::market::open_page(p->full_name);
        u.like_pack.clear();
    }
    if (action(d, f, "##likelater", ImVec2(x + 272 * k, b.y - 70 * k), ImVec2(170 * k, 46 * k), "NOT NOW", Look::paper)) u.like_pack.clear();
}
void share_card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto* pack = pack_named(v, u.share_pack);
    if (!pack) {
        u.share_pack.clear();
        return;
    }
    const auto market = studio::market::view();
    const bool updating = pack_is_published(market, *pack);
    const auto [a, b] = card_box(d, f, display, 1100 * k, 700 * k, (updating ? "UPDATE " : "SHARE ") + upper(pack->name));
    const float x = a.x + 30 * k, w = b.x - a.x - 60 * k, cover = 230 * k;
    float y = a.y + 92 * k;
    pack_cover(d, f, *pack, ImVec2(x, y), ImVec2(x + cover, y + cover));
    if (shop_button(d, f, "##sharecover", ImVec2(x + 10 * k, y + cover - 50 * k), ImVec2(cover - 20 * k, 40 * k), pack->thumbnail.empty() ? "MAKE A COVER" : "COVER",
                    ShopLook::paper))
        open_cover_designer(v, pack->name);
    const float rx = x + cover + 30 * k, rw = w - cover - 30 * k;
    text(d, f.heading, 14 * k, ImVec2(rx, y), blue_light, "DESCRIPTION");
    text(d, f.bold, 12 * k, ImVec2(rx + 140 * k, y + 2 * k), faint,
         updating ? "Say what is new: it is what people see in the MARKET (the first line is its summary)."
                  : "What people see when they open it in the MARKET (the first line is its summary).");
    if (u.share_description_for != pack->name && !ImGui::IsAnyItemActive()) {
        std::snprintf(u.share_description.data(), u.share_description.size(), "%s", pack->pack.description.c_str());
        u.share_description_for = pack->name;
    }
    ImGui::SetCursorScreenPos(ImVec2(rx, y + 24 * k));
    ImGui::PushFont(f.bold);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * k, 8 * k));
    // A shade lighter than the card, as the studio's other typed fields are, so the box shows where to type.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(32, 34, 38, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(40, 42, 46, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(46, 48, 54, 255));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, blue_dim);
    ImGui::PushStyleColor(ImGuiCol_Text, white);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::InputTextMultiline("##sharedescription", u.share_description.data(), u.share_description.size(), ImVec2(rw, cover - 24 * k));
    const bool done_typing = ImGui::IsItemDeactivatedAfterEdit();
    if (u.share_description[0] == 0 && !ImGui::IsItemActive())
        text(d, f.bold, 14 * k, ImVec2(rx + 10 * k, y + 32 * k), faint, "Click here and say what is in the pack and how it skates.");
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    if (done_typing) {
        std::string m;
        studio::set_pack_description(pack->name, u.share_description.data(), m);
    }
    y += cover + 22 * k;
    // What goes with it, and as what version: for an update, one past what is up, and it goes to the same package.
    const auto& sp = pack->pack;
    const auto* live = live_package(market, *pack);
    if (u.share_version_for != pack->name) {
        const auto next = updating ? next_update_version(market, *pack) : studio::market::next_version(sp.version, sp.published);
        std::snprintf(u.share_version.data(), u.share_version.size(), "%s", next.c_str());
        u.share_version_for = pack->name;
    }
    const std::string version = u.share_version.data();
    const std::string last = live && studio::market::version_after(live->version, sp.published) ? live->version : sp.published;
    const bool version_ok = studio::market::plain_version(version) && std::count(version.begin(), version.end(), '.') == 2 &&
                            (last.empty() || studio::market::version_after(version, last));
    const std::string package = live ? live->full_name
                                     : !sp.published_as.empty() ? sp.published_as
                                                                : (market.team.empty() ? std::string{} : market.team + "-") + studio::market::package_name(pack->name);
    text(d, f.heading, 14 * k, ImVec2(x, y + 9 * k), blue_light, "VERSION");
    field(f, "##shareversion", u.share_version, ImVec2(x + 92 * k, y), 140 * k, "1.0.1");
    {
        const auto tricks = std::to_string(saved_count(v, *pack)) + " tricks";
        const std::string line = !version_ok ? (last.empty() ? "A version is three numbers with dots between them: 1.0.0."
                                                             : "It has to be higher than " + last + ", the version up now.")
                                 : updating ? tricks + ", as an update of " + package + (last.empty() ? "." : " (" + last + " is up now).")
                                            : tricks + ", on Thunderstore as " + package + ".";
        text(d, f.bold, 13 * k, ImVec2(x + 248 * k, y + 9 * k), version_ok ? white : skate_theme::warning,
             elide(f.bold, 13 * k, line, w - 248 * k - (live ? 150 * k : 0)));
    }
    if (live && action(d, f, "##sharelive", ImVec2(b.x - 30 * k - 140 * k, y), ImVec2(140 * k, 34 * k), "ITS PAGE", Look::ghost))
        studio::market::open_page(live->full_name);
    y += 46 * k;
    text(d, f.bold, 13 * k, ImVec2(x, y), sp.editable ? blue_light : soft,
         sp.editable ? "Others can open its tricks to edit and copy them (OTHERS CAN EDIT is on)." : "Locked for others: its tricks play and preview, but cannot be opened or copied.");
    y += 40 * k;
    // The three ways.
    const float bw = (w - 20 * k) / 3, bh = 120 * k;
    const auto way = [&](const char* id, float bx, const std::string& title, const std::string& about, Look look, bool enabled) {
        const ImVec2 wa(bx, y), wb(bx + bw, y + bh);
        bool lit = false;
        const bool pressed = hit(id, wa, wb, &lit, enabled);
        const ImU32 fill = !enabled ? IM_COL32(30, 31, 34, 220) : look == Look::blue ? (lit ? IM_COL32(40, 152, 255, 255) : blue)
                                                                                     : (lit ? IM_COL32(62, 64, 70, 250) : IM_COL32(34, 35, 38, 245));
        skate_theme::rough_rect(d, wa, wb, fill, seed_of(wa), k * 0.7f);
        text(d, f.heading, 18 * k, ImVec2(wa.x + 18 * k, wa.y + 16 * k), enabled ? white : faint, title);
        paragraph(d, f.bold, 12 * k, ImVec2(wa.x + 18 * k, wa.y + 48 * k), bw - 36 * k, enabled ? IM_COL32(230, 232, 236, 255) : faint, about);
        return pressed;
    };
    if (way("##sharezip", x, "SAVE A ZIP", "A zip to send to a friend: they put it in their import folder and press IMPORT.", Look::dark, true)) {
        std::string m;
        studio::export_pack(pack->name, m);
        say(m);
    }
    if (way("##sharets", x + bw + 10 * k, "THUNDERSTORE ZIP", "The pack as a Thunderstore package (manifest, icon, README, tricks), to upload on thunderstore.io yourself.",
            Look::dark, true)) {
        std::string m;
        studio::market::export_package(pack->name, m, version_ok ? version : std::string{});
        say(m);
    }
    const bool linked = !market.team.empty();
    if (way("##sharepublish", x + 2 * (bw + 10 * k), !linked ? "SIGN IN TO PUBLISH" : updating ? "PUBLISH UPDATE" : "PUBLISH TO MARKET",
            !linked    ? "Link your Thunderstore team once, and publish your packs from here."
            : updating ? "Puts " + version + " up as an update of " + package + ": everyone who has it gets UPDATE in their MARKET."
                       : "Uploads it to Thunderstore as your team, with everything set, and it shows in everyone's MARKET.",
            Look::blue, !market.job.running && (!linked || version_ok))) {
        if (!linked) u.sign_in = true;
        else {
            if (std::string(u.share_description.data()) != pack->pack.description) {
                std::string m;
                studio::set_pack_description(pack->name, u.share_description.data(), m);
            }
            studio::market::publish(pack->name, version, live ? live->full_name : std::string{});
        }
    }
    y += bh + 16 * k;
    // How the upload is going, or how it went.
    if (market.job.running || !market.job.message.empty()) {
        const auto line = market.job.running ? upper(market.job.what) + "  " + std::to_string(static_cast<int>(market.job.fraction * 100)) + "%" : market.job.message;
        paragraph(d, f.bold, 13 * k, ImVec2(x, y), w - 220 * k, market.job.failed ? skate_theme::warning : soft, line);
        if (market.job.running) {
            d->AddRectFilled(ImVec2(x, y + 22 * k), ImVec2(x + w - 220 * k, y + 28 * k), IM_COL32(255, 255, 255, 40), 3 * k);
            d->AddRectFilled(ImVec2(x, y + 22 * k), ImVec2(x + (w - 220 * k) * market.job.fraction, y + 28 * k), blue, 3 * k);
        } else if (!market.job.url.empty() && action(d, f, "##sharepage", ImVec2(b.x - 30 * k - 200 * k, y - 4 * k), ImVec2(200 * k, 36 * k), "OPEN ITS PAGE", Look::ghost))
            ShellExecuteW(nullptr, L"open", std::wstring(market.job.url.begin(), market.job.url.end()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (action(d, f, "##sharedone", ImVec2(b.x - 30 * k - 180 * k, b.y - 66 * k), ImVec2(180 * k, 44 * k), "DONE", Look::paper)) {
        if (std::string(u.share_description.data()) != pack->pack.description) {
            std::string m;
            studio::set_pack_description(pack->name, u.share_description.data(), m);
        }
        u.share_pack.clear();
    }
}
// A trick just put in a pack: its stance versions in that pack, ticked where the pack has them, the missing ones to make.
void stance_card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto* pack = pack_named(v, u.stance_pack);
    if (!pack || !preset_named(v, u.stance_name)) {
        u.stance_pack.clear();
        return;
    }
    const auto [a, b] = card_box(d, f, display, 860 * k, 430 * k, "ITS STANCES");
    const float x = a.x + 30 * k, w = b.x - a.x - 60 * k;
    float y = a.y + 92 * k;
    y += paragraph(d, f.bold, 16 * k, ImVec2(x, y), w, soft,
                   shown_name(v, u.stance_name) + " is in " + pack->name + ". Make its other stances for the pack too? Ticked: the pack has that "
                   "stance. An empty one makes it from this trick, puts it in the pack and opens it to change.") + 18 * k;
    bool acted = false;
    y += stance_versions(d, f, v, u.stance_name, ImVec2(x, y), w, pack->members, pack->name, &acted) + 10 * k;
    paragraph(d, f.bold, 12 * k, ImVec2(x, y), w, faint,
              "With Trick matching on Exact stance each version plays only in its own stance, so every way you do the trick can look its own. "
              "The pack shows these ticks under each trick, to make the rest later.");
    if (acted) u.stance_pack.clear();
    if (action(d, f, "##stancesdone", ImVec2(b.x - 30 * k - 200 * k, b.y - 70 * k), ImVec2(200 * k, 46 * k), "NOT NOW", Look::dark)) u.stance_pack.clear();
}
void save_card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto [a, b] = card_box(d, f, display, 860 * k, 560 * k, "SAVED");
    const float x = a.x + 30 * k, w = b.x - a.x - 60 * k;
    float y = a.y + 92 * k;
    const auto name = v.name;
    y += paragraph(d, f.bold, 16 * k, ImVec2(x, y), w, soft, "It is in MY TRICKS and plays in game on its trick. Give it a name you will know it by.") + 18 * k;
    heading(d, f, ImVec2(x, y), w, "Name it");
    y += 32 * k;
    if (u.label_for != name && !ImGui::IsAnyItemActive()) {
        const auto* p = preset_named(v, name);
        std::snprintf(u.label.data(), u.label.size(), "%s", p && !p->label.empty() ? p->label.c_str() : name.c_str());
        u.label_for = name;
    }
    bool edited = false;
    const bool entered = field(f, "##savelabel", u.label, ImVec2(x, y), w - 106 * k, "e.g. Steezy kickflip", &edited);
    if (action(d, f, "##savelabelset", ImVec2(x + w - 96 * k, y), ImVec2(96 * k, 34 * k), "SET") || entered || edited) {
        std::string m;
        studio::set_label(name, u.label.data(), m);
        say(m);
    }
    y += 54 * k;
    heading(d, f, ImVec2(x, y), w, "Put it in a pack");
    y += 34 * k;
    y += pack_chips(d, f, v, name, ImVec2(x, y), w) + 16 * k;
    const float bw = (w - 20 * k) / 3, by = b.y - 70 * k;
    if (action(d, f, "##savetest", ImVec2(x, by), ImVec2(bw, 46 * k), "TEST IN GAME", Look::blue)) test_now(v);
    if (action(d, f, "##savekeep", ImVec2(x + bw + 10 * k, by), ImVec2(bw, 46 * k), "KEEP EDITING", Look::dark)) u.save_card = false;
    if (action(d, f, "##savemine", ImVec2(x + 2 * (bw + 10 * k), by), ImVec2(bw, 46 * k), "TO MY TRICKS", Look::paper)) {
        u.save_card = false;
        open_menu(0);
        u.trick_group = "*mine";
        u.trick_page = name;
        u.label_for.clear();
    }
}
// ---- preview mode -----------------------------------------------------------------------------------
// Watching a saved trick - one from a pack, or another maker's: nothing of the editor, only a card about it on the left
// and the buttons to watch it by along the bottom. Space plays and pauses, R starts it again, 1-4 the views, Esc goes back.
void leave_preview() {
    auto& u = ui();
    studio::end_preview();
    if (u.preview_from_pack && !u.preview_pack.empty()) {
        open_menu(1);
        open_pack_page(u.preview_pack);
    } else open_menu(0);
    u.preview_pack.clear();
    u.preview_name.clear();
}
void preview_screen(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto* p = preset_named(v, u.preview_name.empty() ? v.name : u.preview_name);
    const auto* pack = u.preview_pack.empty() ? nullptr : pack_named(v, u.preview_pack);
    // ---- the card: the pack and the trick ----
    {
        const ImVec2 a(28 * k, 110 * k);
        const float w = 380 * k;
        float y = a.y + 18 * k;
        const float x = a.x + 20 * k, iw = w - 40 * k;
        // Its height is known once it is laid out: drawn into a channel under its contents.
        d->ChannelsSplit(2);
        d->ChannelsSetCurrent(1);
        if (pack) {
            const float cover = 120 * k;
            pack_cover(d, f, *pack, ImVec2(x, y), ImVec2(x + cover, y + cover));
            text(d, f.heading, 12 * k, ImVec2(x + cover + 16 * k, y + 4 * k), blue_light, "PACK");
            paragraph(d, f.title, 24 * k, ImVec2(x + cover + 16 * k, y + 22 * k), iw - cover - 16 * k, white, upper(pack->name));
            if (!pack->made_by.empty())
                text(d, f.bold, 12 * k, ImVec2(x + cover + 16 * k, y + cover - 18 * k), soft, elide(f.bold, 12 * k, "BY " + upper(pack->made_by), iw - cover - 16 * k));
            y += cover + 18 * k;
        }
        text(d, f.heading, 12 * k, ImVec2(x, y), blue_light, "TRICK");
        y += 20 * k;
        y += paragraph(d, f.title, 30 * k, ImVec2(x, y), iw, white, upper(shown_name(v, u.preview_name.empty() ? v.name : u.preview_name))) + 6 * k;
        if (p) {
            text(d, f.bold, 14 * k, ImVec2(x, y), soft, elide(f.bold, 14 * k, trick_label(p->trick) + (p->made_by.empty() ? "" : "   by " + p->made_by), iw));
            y += 26 * k;
            float right = x + iw;
            const auto tags = tags_of(*p, pack ? pack->pack.pop_rule : std::string{});
            float tx = x;
            for (const auto& [label, colour] : tags) {
                if (tx + tag_width(f, label) > right) tx = x, y += 26 * k;
                tx += tag(d, f, ImVec2(tx, y), label, colour) + 6 * k;
            }
            y += 30 * k;
            y += paragraph(d, f.bold, 12 * k, ImVec2(x, y), iw, faint,
                           v.front_foot ? "In game: do this trick with its pop. Trick matching is on Front foot: it plays in any stance."
                                        : "In game: do this trick with its pop, in the stance on its tags (Trick matching is on Exact stance).") + 8 * k;
            if (p->locked)
                y += paragraph(d, f.bold, 12 * k, ImVec2(x, y), iw, skate_theme::warning, "Locked by its maker: it plays and previews, it is not opened to edit.") + 8 * k;
        }
        d->ChannelsSetCurrent(0);
        skate_theme::rough_rect(d, a, ImVec2(a.x + w, y + 14 * k), IM_COL32(16, 17, 20, 236), seed_of(a), k);
        d->AddRectFilled(a, ImVec2(a.x + 6 * k, y + 14 * k), blue);
        d->ChannelsMerge();
    }
    // ---- the buttons ----
    const float bar_w = std::min(1320 * k, display.x - 80 * k), bar_h = 128 * k;
    const ImVec2 a((display.x - bar_w) * 0.5f, display.y - bar_h - 26 * k), b(a.x + bar_w, a.y + bar_h);
    skate_theme::rough_rect(d, a, b, IM_COL32(14, 14, 16, 230), seed_of(a), k);
    {
        // Where it is: a bar to click or drag along.
        const ImVec2 la(a.x + 24 * k, a.y + 18 * k), lb(b.x - 24 * k, a.y + 30 * k);
        const float at = v.duration > 0 ? std::clamp(v.time / v.duration, 0.0f, 1.0f) : 0.0f;
        d->AddRectFilled(la, lb, IM_COL32(255, 255, 255, 40), 6 * k);
        d->AddRectFilled(la, ImVec2(la.x + (lb.x - la.x) * at, lb.y), blue, 6 * k);
        d->AddCircleFilled(ImVec2(la.x + (lb.x - la.x) * at, (la.y + lb.y) * 0.5f), 9 * k, white, 20);
        ImGui::SetCursorScreenPos(ImVec2(la.x, la.y - 8 * k));
        ImGui::InvisibleButton("##previewscrub", ImVec2(lb.x - la.x, lb.y - la.y + 16 * k));
        if (ImGui::IsItemActive() && v.duration > 0) {
            studio::set_playing(false);
            studio::set_time(std::clamp((ImGui::GetIO().MousePos.x - la.x) / (lb.x - la.x), 0.0f, 1.0f) * v.duration);
        }
    }
    float x = a.x + 24 * k;
    const float by = a.y + 52 * k, bh = 50 * k;
    const auto button = [&](const char* id, float w, const std::string& label, Look look, bool on = false) {
        const bool pressed = action(d, f, id, ImVec2(x, by), ImVec2(w, bh), label, look, true, on);
        x += w + 10 * k;
        return pressed;
    };
    if (button("##previewplay", 150 * k, v.playing ? "PAUSE" : "PLAY", Look::blue)) studio::set_playing(!v.playing);
    if (button("##previewrestart", 130 * k, "RESTART", Look::dark)) {
        studio::set_time(0);
        studio::set_playing(true);
    }
    if (button("##previewloop", 110 * k, "LOOP", Look::dark, v.loop)) studio::set_loop(!v.loop);
    if (button("##previewslow", 110 * k, v.speed < 0.99f ? "0.5X" : "1X", Look::dark, v.speed < 0.99f)) studio::set_speed(v.speed < 0.99f ? 1.0f : 0.5f);
    x += 14 * k;
    static constexpr std::array<std::pair<const char*, int>, 4> views{{{"FRONT", 1}, {"SIDE", 2}, {"BACK", 3}, {"TOP", 4}}};
    for (const auto& [label, shot] : views) {
        ImGui::PushID(shot);
        if (button("##previewview", 90 * k, label, Look::ghost, u.camera_shot == shot)) take_shot(v, shot);
        ImGui::PopID();
    }
    if (action(d, f, "##previewexit", ImVec2(b.x - 24 * k - 230 * k, by), ImVec2(230 * k, bh), u.preview_from_pack ? "< BACK TO PACK" : "< BACK",
               Look::paper))
        leave_preview();
    // The keys.
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) studio::set_playing(!v.playing);
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) studio::set_time(0), studio::set_playing(true);
        for (const auto& [label, shot] : views)
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + shot), false)) take_shot(v, shot);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) leave_preview();
    }
}
// Something said when you are back from testing a trick: picked at random, one each time.
const char* test_line() {
    static constexpr std::array<const char*, 24> lines{
        "That one's going in the part.", "Clean. Do it again for the filmer.", "Bolts. Every single time.",
        "The homies would lose it over that.", "Steez levels: rising.", "Landed. Roll away like it was nothing.",
        "Okay, that was proper.", "Send that clip to the group chat.", "Buttery. Absolutely buttery.",
        "Your board is proud of you.", "That looked heavy from here.", "Pop, flick, catch, roll away. Textbook.",
        "That's the one you film twice.", "The spot just got a little more famous.", "Dialed. On to the next one.",
        "Someone call the filmer back.", "Session ender right there.", "Smooth enough to skate on glass.",
        "Big energy. Keep rolling.", "Certified banger.", "No bails, just good vibes.",
        "That trick has a future.", "Even the security guard nodded.", "Skate shop wall material."};
    static std::mt19937 random{static_cast<std::uint32_t>(GetTickCount64())};
    static std::size_t last = lines.size();
    std::size_t pick = std::uniform_int_distribution<std::size_t>(0, lines.size() - 1)(random);
    if (pick == last) pick = (pick + 1) % lines.size();
    last = pick;
    return lines[pick];
}
void test_card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto name = v.test_returned;
    if (u.test_line_for != name) {
        u.test_line = test_line();
        u.test_line_for = name;
    }
    const auto [a, b] = card_box(d, f, display, 920 * k, 560 * k, "BACK IN THE STUDIO");
    const float x = a.x + 30 * k, w = b.x - a.x - 60 * k;
    float y = a.y + 104 * k;
    // The line, big, on paper.
    {
        const float fs = 40 * k;
        const auto measured = f.title->CalcTextSizeA(fs, FLT_MAX, w - 40 * k, upper(u.test_line).c_str());
        const ImVec2 pa(x, y), pb(x + w, y + measured.y + 36 * k);
        skate_theme::rough_rect(d, pa, pb, IM_COL32(236, 236, 234, 252), seed_of(pa), k);
        d->AddText(f.title, fs, ImVec2(pa.x + 20 * k, pa.y + 18 * k), IM_COL32(18, 18, 20, 255), upper(u.test_line).c_str(), nullptr, w - 40 * k);
        y = pb.y + 18 * k;
    }
    y += paragraph(d, f.bold, 16 * k, ImVec2(x, y), w, soft,
                   "You skated " + shown_name(v, name) + ". Keep it as it is, put it in a pack, or go back and tweak it.") + 18 * k;
    if (saved_trick(v, name)) {
        heading(d, f, ImVec2(x, y), w, "Put it in a pack");
        y += 34 * k;
        y += pack_chips(d, f, v, name, ImVec2(x, y), w);
    }
    const float bw = (w - 20 * k) / 3, by = b.y - 70 * k;
    if (action(d, f, "##testagain", ImVec2(x, by), ImVec2(bw, 46 * k), "TEST AGAIN", Look::blue)) {
        std::string m;
        studio::clear_test_returned();
        studio::test_in_game(name, m);
        say(m);
    }
    if (action(d, f, "##testedit", ImVec2(x + bw + 10 * k, by), ImVec2(bw, 46 * k), "KEEP EDITING", Look::dark)) {
        studio::clear_test_returned();
        if (v.name != name) open_saved(name);
        u.menu = false;
    }
    if (action(d, f, "##testmine", ImVec2(x + 2 * (bw + 10 * k), by), ImVec2(bw, 46 * k), "TO MY TRICKS", Look::paper)) {
        studio::clear_test_returned();
        open_menu(0);
        u.trick_group = "*mine";
        u.trick_page = name;
        u.label_for.clear();
    }
}
// Packs that are on with their own animation of the same trick: which one plays it, or turns at random.
void conflicts_card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto count = v.conflicts.size();
    const auto [a, b] = card_box(d, f, display, 980 * k, 720 * k, count == 1 ? "DUPLICATE TRICK FOUND" : "DUPLICATE TRICKS FOUND", skate_theme::warning);
    const float x = a.x + 30 * k, w = b.x - a.x - 60 * k;
    float y = a.y + 96 * k;
    y += paragraph(d, f.bold, 16 * k, ImVec2(x, y), w, soft,
                   "Packs you have on each have their own animation of these. Pick the pack that always plays it, or let them take "
                   "turns at random, attempt by attempt. You can change this any time in PACKS.") + 16 * k;
    const ImVec2 la(x, y), lb(b.x - 20 * k, b.y - 84 * k);
    begin_region(81, la, lb, u.conflicts_scroll, k);
    float ly = la.y - u.conflicts_scroll;
    for (const auto& c : v.conflicts) {
        ImGui::PushID(c.slot.c_str());
        const float block_top = ly;
        text(d, f.heading, 20 * k, ImVec2(x + 18 * k, ly + 14 * k), white, upper(trick_label(c.trick)) + "  /  " + pop_words(c.pop));
        std::string in;
        for (const auto& [pack, names] : c.packs) in += (in.empty() ? "In " : ", ") + pack + " (" + std::to_string(names.size()) + ")";
        text(d, f.bold, 13 * k, ImVec2(x + 18 * k, ly + 42 * k), soft, elide(f.bold, 13 * k, in, w - 40 * k));
        ly += 66 * k;
        float cx = x + 18 * k;
        const bool random = c.choice.empty() || c.choice == "*";
        float cw = 0;
        if (chip(d, f, "##random", ImVec2(cx, ly), "TAKE TURNS AT RANDOM", random, &cw, 34 * k)) {
            std::string m;
            studio::set_conflict_choice(c.slot, "*", m);
        }
        cx += cw + 8 * k;
        for (const auto& [pack, names] : c.packs) {
            const auto label = pack + " ALWAYS";
            const float pw = chip_width(f, label);
            if (cx + pw > x + w - 20 * k) cx = x + 18 * k, ly += 42 * k;
            ImGui::PushID(pack.c_str());
            if (chip(d, f, "##always", ImVec2(cx, ly), label, c.choice == pack, nullptr, 34 * k)) {
                std::string m;
                studio::set_conflict_choice(c.slot, pack, m);
            }
            ImGui::PopID();
            cx += pw + 8 * k;
        }
        ly += 50 * k;
        d->AddRect(ImVec2(x, block_top), ImVec2(x + w - 14 * k, ly - 8 * k), IM_COL32(255, 255, 255, 30), 4 * k, 0, 1.4f * k);
        ly += 6 * k;
        ImGui::PopID();
    }
    end_region(d, la, lb, ly + u.conflicts_scroll - la.y, u.conflicts_scroll, k);
    if (action(d, f, "##conflictsdone", ImVec2(b.x - 30 * k - 200 * k, b.y - 66 * k), ImVec2(200 * k, 46 * k), "DONE", Look::blue)) conflicts_done(v);
}
// ADD TRICKS: every saved trick, by kind, to put in the pack or take out of it.
void adding_card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    const float k = f.k;
    const auto* pack = pack_named(v, v.open_pack);
    if (!pack) {
        u.pack_adding = false;
        return;
    }
    const auto [a, b] = card_box(d, f, display, 900 * k, 780 * k, "ADD TO " + upper(pack->name));
    const float x = a.x + 30 * k, w = b.x - a.x - 60 * k;
    float y = a.y + 96 * k;
    text(d, f.bold, 15 * k, ImVec2(x, y), soft, "Press a trick to put it in the pack, again to take it out.");
    y += 32 * k;
    std::vector<std::pair<std::string, std::vector<std::string>>> kinds;
    for (const auto& name : v.library) {
        const auto* p = preset_named(v, name);
        const auto kind = p ? kind_of(p->trick) : std::string("Pieces");
        auto found = std::find_if(kinds.begin(), kinds.end(), [&](const auto& e) { return e.first == kind; });
        if (found == kinds.end()) {
            kinds.emplace_back(kind, std::vector<std::string>{});
            found = kinds.end() - 1;
        }
        found->second.push_back(name);
    }
    const ImVec2 la(x, y), lb(b.x - 20 * k, b.y - 84 * k);
    begin_region(91, la, lb, u.adding_scroll, k);
    float ly = la.y - u.adding_scroll;
    if (kinds.empty()) {
        text(d, f.bold, 16 * k, ImVec2(x, ly), soft, "You have no saved tricks yet: open a trick in TRICKS, change it and save it.");
        ly += 40 * k;
    }
    for (const auto& [kind, names] : kinds) {
        heading(d, f, ImVec2(x, ly), w - 20 * k, upper(kind), std::to_string(names.size()));
        ly += 34 * k;
        for (const auto& name : names) {
            const bool in = std::find(pack->members.begin(), pack->members.end(), name) != pack->members.end();
            const ImVec2 ra(x, ly), rb(x + w - 20 * k, ly + 44 * k);
            ly += 50 * k;
            ImGui::PushID(name.c_str());
            bool lit = false;
            if (hit("##add", ra, rb, &lit)) {
                std::string m;
                if (studio::set_pack_member(pack->name, name, !in, m) && !in) ask_stances(v, pack->name, name);
                say(m);
            }
            ImGui::PopID();
            skate_theme::rough_rect(d, ra, rb, in ? IM_COL32(20, 60, 110, 248) : lit ? IM_COL32(48, 50, 56, 248) : IM_COL32(26, 27, 30, 240), seed_of(ra), k * 0.6f);
            if (lit && nav().keys) chosen_outline(d, ra, rb, k);
            const auto* p = preset_named(v, name);
            text(d, f.bold, 17 * k, ImVec2(ra.x + 16 * k, ra.y + 12 * k), white, elide(f.bold, 17 * k, shown_name(v, name), (rb.x - ra.x) * 0.42f));
            const char* state = in ? "IN THE PACK" : "ADD";
            const auto t = text_size(f.bold, 13 * k, state);
            text(d, f.bold, 13 * k, ImVec2(rb.x - t.x - 16 * k, ra.y + 15 * k), in ? blue_light : lit ? white : soft, state);
            float right = rb.x - t.x - 34 * k;
            if (p) trick_tags(d, f, *p, {}, right, ra.y + 12 * k);
        }
        ly += 8 * k;
    }
    end_region(d, la, lb, ly + u.adding_scroll - la.y, u.adding_scroll, k);
    if (action(d, f, "##addingdone", ImVec2(b.x - 30 * k - 200 * k, b.y - 66 * k), ImVec2(200 * k, 46 * k), "DONE", Look::blue)) u.pack_adding = false;
}
// Whichever card is up, over everything.
void draw_cards(const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    if (!any_card_open(v)) {
        // Packs that are on with the same trick, not decided yet: asked about once, over the menu.
        bool undecided = false;
        for (const auto& c : v.conflicts) undecided |= c.choice.empty();
        if (undecided && u.menu) u.conflicts_open = true;
        else return;
    }
    begin_card(display);
    auto* d = ImGui::GetWindowDrawList();
    if (u.browser.open) browser_card(d, f, v, display);
    else if (u.pack_adding) adding_card(d, f, v, display);
    else if (u.conflicts_open) conflicts_card(d, f, v, display);
    else if (!u.stance_pack.empty()) stance_card(d, f, v, display);
    else if (u.sign_in) sign_in_card(d, f, v, display);
    else if (!u.like_pack.empty()) like_card(d, f, v, display);
    else if (!u.cover_pack.empty()) cover_card(d, f, v, display);
    else if (!u.share_pack.empty()) share_card(d, f, v, display);
    else if (u.save_card) save_card(d, f, v, display);
    else if (!v.test_returned.empty()) test_card(d, f, v, display);
    end_card();
}

// ---- the editor -----------------------------------------------------------------------------------
// The skater in the middle; along the top MENU, the trick's name, the camera, and TEST IN GAME and SAVE; the tools down
// the left; the chosen joint's card on the right; the timeline along the bottom with KEY in it.
void editor_top(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 display, float left, float right) {
    const float k = f.k, h = 78 * k;
    const ImU32 dark = IM_COL32(8, 8, 10, 215), none = IM_COL32(8, 8, 10, 0);
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, h + 30 * k), dark, dark, none, none);
    const float y = 16 * k;
    if (action(d, f, "##menu", ImVec2(left + 20 * k, y), ImVec2(132 * k, 46 * k), "", Look::paper)) open_menu();
    {
        // Three bars and MENU, on the paper button.
        const ImU32 ink = IM_COL32(18, 18, 20, 255);
        const float bx = left + 38 * k, cy = y + 23 * k;
        for (int i = -1; i <= 1; ++i) d->AddRectFilled(ImVec2(bx, cy + i * 7 * k - 1.5f * k), ImVec2(bx + 20 * k, cy + i * 7 * k + 1.5f * k), ink);
        text(d, f.heading, 18 * k, ImVec2(bx + 32 * k, cy - 11 * k), ink, "MENU");
    }
    const float title_x = left + 172 * k;
    const float title_w = std::max(120 * k, display.x * 0.5f - 160 * k - title_x);
    text(d, f.title, 30 * k, ImVec2(title_x, y - 4 * k), white, elide(f.title, 30 * k, title_of(v), title_w));
    float tx = title_x;
    if (!v.trick.empty()) tx += tag(d, f, ImVec2(tx, y + 32 * k), upper(trick_label(v.trick)), IM_COL32(255, 255, 255, 36)) + 6 * k;
    tx += tag(d, f, ImVec2(tx, y + 32 * k), pop_words(v.pop), IM_COL32(255, 255, 255, 36)) + 6 * k;
    if (v.dirty) tag(d, f, ImVec2(tx, y + 32 * k), "NOT SAVED", skate_theme::warning, IM_COL32(18, 18, 20, 255));
    shot_switch(d, f, v, ImVec2(display.x * 0.5f, y + 23 * k));
    float x = right - 20 * k;
    const auto place = [&](float w) {
        x -= w;
        const ImVec2 at(x, y);
        x -= 10 * k;
        return at;
    };
    if (action(d, f, "##save", place(150 * k), ImVec2(150 * k, 46 * k), v.dirty ? "SAVE *" : "SAVE", Look::paper, v.has_clip)) save_now(v);
    if (action(d, f, "##testgame", place(200 * k), ImVec2(200 * k, 46 * k), "TEST IN GAME", Look::blue, v.has_clip)) test_now(v);
    if (action(d, f, "##redo", place(86 * k), ImVec2(86 * k, 46 * k), "REDO", Look::ghost, v.can_redo)) studio::redo();
    if (action(d, f, "##undo", place(86 * k), ImVec2(86 * k, 46 * k), "UNDO", Look::ghost, v.can_undo)) studio::undo();
}
// The tools, down the left in three groups - how a drag poses the joint, what helps it, what is shown - a row each,
// its name at the left and its key at the right. Each says what it is beside it while the mouse is over it.
void tool_strip(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 at) {
    auto& u = ui();
    const float k = f.k, w = tool_strip_width * k, h = 38 * k, row = h + 4 * k;
    float y = at.y;
    u.tool_tip.clear();
    const auto group = [&](const char* name, bool first = false) {
        if (!first) y += 14 * k;
        text(d, f.bold, 11 * k, ImVec2(at.x + 3 * k, y), faint, name);
        y += 19 * k;
    };
    const auto tool = [&](const char* id, const char* label, const char* key, bool on, const char* about, float width = 0, float x_off = 0) {
        const float bw = width > 0 ? width : w;
        const ImVec2 a(at.x + x_off, y), b(a.x + bw, y + h);
        bool lit = false;
        const bool pressed = hit(id, a, b, &lit);
        skate_theme::rough_rect(d, a, b, on ? (lit ? IM_COL32(40, 152, 255, 255) : blue) : lit ? IM_COL32(46, 48, 54, 245) : IM_COL32(20, 21, 24, 230),
                                seed_of(a), k * 0.6f);
        const auto t = text_size(f.heading, 14 * k, label);
        if (key[0]) {
            text(d, f.heading, 14 * k, ImVec2(a.x + 12 * k, a.y + (h - t.y) * 0.5f), white, label);
            // The key, on a little cap at the right.
            const auto kt = text_size(f.bold, 11 * k, key);
            const float cap_w = std::max(20 * k, kt.x + 10 * k), cap_h = 20 * k;
            const ImVec2 ca(b.x - 9 * k - cap_w, a.y + (h - cap_h) * 0.5f), cb(b.x - 9 * k, ca.y + cap_h);
            d->AddRectFilled(ca, cb, on ? IM_COL32(255, 255, 255, 46) : IM_COL32(255, 255, 255, 20), 4 * k);
            text(d, f.bold, 11 * k, ImVec2(ca.x + (cap_w - kt.x) * 0.5f, ca.y + (cap_h - kt.y) * 0.5f), on ? IM_COL32(235, 243, 255, 245) : faint, key);
        } else text(d, f.heading, 14 * k, ImVec2(a.x + (bw - t.x) * 0.5f, a.y + (h - t.y) * 0.5f), white, label);
        if (lit) {
            u.tool_tip = label;
            u.tool_tip_text = about;
            u.tool_tip_at = ImVec2(at.x + w + 14 * k, a.y);
        }
        return pressed;
    };
    group("POSE", true);
    if (tool("##turn", "TURN", "R", !u.move_mode, "A drag (or the left stick) turns the chosen joint.")) u.move_mode = false;
    y += row;
    if (tool("##move", "MOVE", "G", u.move_mode, "A drag moves the hips or the board, and pulls a hand or a foot to where you drag it, the limb bending to follow."))
        u.move_mode = true;
    y += row;
    {
        const float third = (w - 8 * k) / 3;
        static constexpr std::array<const char*, 3> names{"X", "Y", "Z"};
        static constexpr std::array<const char*, 3> ids{"##axisx", "##axisy", "##axisz"};
        for (int i = 0; i < 3; ++i)
            if (tool(ids[static_cast<std::size_t>(i)], names[static_cast<std::size_t>(i)], "", u.axis == i,
                     "Locks a drag to one axis. Press it again for any axis. Keys: X, Y, Z.", third, static_cast<float>(i) * (third + 4 * k)))
                u.axis = u.axis == i ? -1 : i;
    }
    y += row;
    if (tool("##axes", u.local_axes ? "OBJECT" : "WORLD", "L", false, "Which way the gizmo's arrows and rings lie: the joint's own axes, or the world's."))
        u.local_axes = !u.local_axes;
    y += row;
    group("HELPERS");
    if (tool("##solo", "SOLO", "O", v.solo, "A turn moves that joint alone: the limb below keeps the way it faces.")) studio::set_solo(!v.solo);
    y += row;
    if (tool("##snap", "SNAP", "N", u.snap, "A foot you move lands on the deck and slides along it.")) u.snap = !u.snap;
    y += row;
    if (tool("##feetboard", "FEET+BOARD", "B", u.feet_lock,
             "Locks the feet to the deck: they go with the board when you move or turn it, and slide along it but never leave it. "
             "B again lets go. Ctrl + click a joint to turn it too.")) {
        if (u.feet_lock) unlock_feet();
        else lock_feet_to_deck(v);
    }
    y += row;
    if (tool("##limits", "LIMITS", "M", v.limits, "Knees and elbows bend one way and joints stay in the range the game's own animation uses."))
        studio::set_joint_limits(!v.limits);
    y += row;
    if (tool("##clear", "CLEAR", "C", u.anti_clip, "What you move is kept out of his body and off the deck. How much: SETTINGS > SKATER.")) u.anti_clip = !u.anti_clip;
    y += row;
    group("VIEW");
    if (tool("##joints", "JOINTS", "J", !u.hide_joints, "The dots on his joints and the gizmo. Off shows the pose clean.")) u.hide_joints = !u.hide_joints;
    y += row;
    if (tool("##follow", "FOLLOW", "F", false, "The camera keeps the chosen joint in the middle. Again: the whole skater.")) studio::focus_joint(u.selected);
    if (!u.tool_tip.empty()) {
        const float tw = 300 * k;
        const auto measured = f.bold->CalcTextSizeA(14 * k, FLT_MAX, tw - 28 * k, u.tool_tip_text.c_str());
        const ImVec2 a = u.tool_tip_at, b(a.x + tw, a.y + measured.y + 50 * k);
        skate_theme::rough_rect(d, a, b, IM_COL32(236, 236, 234, 250), seed_of(a), k * 0.7f);
        text(d, f.heading, 15 * k, ImVec2(a.x + 14 * k, a.y + 10 * k), IM_COL32(18, 18, 20, 255), u.tool_tip);
        paragraph(d, f.bold, 14 * k, ImVec2(a.x + 14 * k, a.y + 34 * k), tw - 28 * k, IM_COL32(60, 60, 66, 255), u.tool_tip_text);
    }
}
// The chosen joint's card: its turn and place as numbers, and what can be done to its keys.
void joint_card(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 at, float width) {
    auto& u = ui();
    const float k = f.k;
    const auto* key = key_here(v, u.selected);
    const auto* track = track_view(v, u.selected);
    const bool locked = track && track->locked;
    const auto* e = studio::editable(u.selected);
    const bool placed = studio::limb_chain(u.selected) != 0;
    const bool has_position = (e && e->moves) || placed;
    const float h = (has_position ? 360 : 300) * k;
    const ImVec2 a = at, b(at.x + width, at.y + h);
    skate_theme::rough_rect(d, ImVec2(a.x + 5 * k, a.y + 5 * k), ImVec2(b.x + 5 * k, b.y + 5 * k), IM_COL32(0, 0, 0, 120), 71, k);
    skate_theme::rough_rect(d, a, b, IM_COL32(16, 17, 20, 236), 73, k);
    theme::halftone(d, ImVec2(b.x - 50 * k, a.y + 10 * k), 5, 4, IM_COL32(255, 255, 255, 20));
    const float x = a.x + 20 * k, w = width - 40 * k;
    float y = a.y + 16 * k;
    text(d, f.heading, 22 * k, ImVec2(x, y), white, elide(f.heading, 22 * k, upper(label_of(u.selected)), w - 110 * k));
    {
        const char* state = locked ? "LOCKED" : key ? "KEYED" : "NO KEY";
        const float tw = tag_width(f, state);
        tag(d, f, ImVec2(x + w - tw, y + 4 * k), state, locked ? IM_COL32(120, 34, 36, 255) : key ? blue : IM_COL32(255, 255, 255, 36));
    }
    y += 40 * k;
    // The numbers.
    {
        const auto offset = studio::offset_of(u.selected);
        auto euler = studio::to_euler(offset.rotation);
        auto position = placed ? studio::limb_position(u.selected) : offset.offset;
        const bool board_fields = u.selected == studio::board_joint && v.board_base_valid;
        if (board_fields) {
            for (std::size_t i = 0; i < 3; ++i) position[i] = v.board_base.position[i] + offset.offset[i];
            euler = studio::to_euler(studio::multiply(offset.rotation, v.board_base.rotation));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, white);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(36, 38, 44, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(52, 56, 64, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 84, 170, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * k, 5 * k));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3 * k);
        ImGui::PushFont(f.bold);
        static constexpr std::array<ImU32, 3> axis_colour{IM_COL32(232, 84, 84, 255), IM_COL32(110, 200, 96, 255), IM_COL32(80, 150, 245, 255)};
        static constexpr std::array<const char*, 3> axes{"X", "Y", "Z"};
        const float col_w = w / 3;
        bool turned = false, moved = false;
        const auto row = [&](const char* title, float row_y, auto& values, int id_base, float speed, const char* format, bool& changed) {
            text(d, f.bold, 12 * k, ImVec2(x, row_y), soft, title);
            for (int i = 0; i < 3; ++i) {
                const float cx = x + static_cast<float>(i) * col_w;
                d->AddRectFilled(ImVec2(cx, row_y + 20 * k), ImVec2(cx + 4 * k, row_y + 46 * k), axis_colour[static_cast<std::size_t>(i)], 2 * k);
                text(d, f.bold, 13 * k, ImVec2(cx + 9 * k, row_y + 25 * k), soft, axes[static_cast<std::size_t>(i)]);
                ImGui::SetCursorScreenPos(ImVec2(cx + 24 * k, row_y + 18 * k));
                ImGui::PushID(id_base + i);
                ImGui::SetNextItemWidth(col_w - 32 * k);
                changed |= ImGui::DragFloat("##value", &values[static_cast<std::size_t>(i)], speed, 0, 0, format);
                ImGui::PopID();
            }
        };
        row(board_fields ? "ROTATION  (0, 0, 0 = AS IT STARTS)" : "ROTATION", y, euler, 0, 0.4f, "%.1f", turned);
        y += 58 * k;
        if (has_position) {
            row(board_fields ? "POSITION  (0, 0, 0 = WHERE IT STARTS)" : "POSITION", y, position, 10, 0.002f, "%.3f", moved);
            y += 58 * k;
        }
        ImGui::PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        if ((turned || moved) && !locked && v.has_clip) {
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
    }
    y += 6 * k;
    // Its keys.
    const float bh = 40 * k, gap = 8 * k;
    {
        const float key_w = 128 * k, step_w = 48 * k, del_w = w - key_w - 2 * step_w - 3 * gap;
        float bx = x;
        if (action(d, f, "##key", ImVec2(bx, y), ImVec2(key_w, bh), "KEY  (K)", Look::key, v.has_clip && !locked)) key_joint();
        bx += key_w + gap;
        if (action(d, f, "##deletekey", ImVec2(bx, y), ImVec2(del_w, bh), "DELETE KEY", Look::dark, key && !locked)) delete_key(v);
        bx += del_w + gap;
        const bool has_keys = track && !track->keys.empty();
        if (action(d, f, "##prevkey", ImVec2(bx, y), ImVec2(step_w, bh), "<", Look::dark, has_keys)) jump_key(v, -1);
        bx += step_w + gap;
        if (action(d, f, "##nextkey", ImVec2(bx, y), ImVec2(step_w, bh), ">", Look::dark, has_keys)) jump_key(v, 1);
    }
    y += bh + gap;
    {
        const float q = (w - 3 * gap) / 4;
        float bx = x;
        if (action(d, f, "##channels", ImVec2(bx, y), ImVec2(q, 34 * k), "CURVES", Look::dark, v.has_clip, channels_open(u.selected)))
            toggle_channels(u.selected, false);
        bx += q + gap;
        if (action(d, f, "##mute", ImVec2(bx, y), ImVec2(q, 34 * k), "MUTE", Look::dark, track != nullptr, track && track->hidden))
            studio::set_track_flags(u.selected, !(track && track->hidden), locked);
        bx += q + gap;
        if (action(d, f, "##lock", ImVec2(bx, y), ImVec2(q, 34 * k), "LOCK", Look::dark, track != nullptr, locked))
            studio::set_track_flags(u.selected, track && track->hidden, !locked);
        bx += q + gap;
        if (action(d, f, "##resetjoint", ImVec2(bx, y), ImVec2(q, 34 * k), "CLEAR", Look::dark, track && !locked && !track->keys.empty())) {
            studio::begin_change();
            studio::reset_joint(u.selected);
            say("All of " + label_of(u.selected) + "'s keys taken off.");
        }
    }
    y += 34 * k + gap;
    {
        const float half = (w - gap) / 2;
        if (action(d, f, "##copypose", ImVec2(x, y), ImVec2(half, 34 * k), "COPY POSE", Look::ghost, v.has_clip)) {
            studio::copy_pose();
            u.copied_are_keys = false;
            say("Pose copied: PASTE POSE keys it on another frame.");
        }
        if (action(d, f, "##pastepose", ImVec2(x + half + gap, y), ImVec2(half, 34 * k), "PASTE POSE", Look::ghost, v.has_clip)) {
            studio::begin_change();
            studio::paste_pose();
        }
    }
}
// Over the timeline while keys are chosen: what can be done with them, in one place.
void selection_bar(ImDrawList* d, const Fonts& f, const studio::View& v, float centre_x, float bottom) {
    auto& u = ui();
    if (u.picked.empty()) return;
    const float k = f.k, bh = 34 * k, bw = 96 * k, gap = 6 * k;
    const std::string count = std::to_string(u.picked.size()) + (u.picked.size() == 1 ? " KEY" : " KEYS");
    const float count_w = text_size(f.heading, 17 * k, count.c_str()).x + 20 * k;
    const float hint_w = text_size(f.bold, 12 * k, "RIGHT-CLICK: MORE").x + 20 * k;
    const float w = count_w + 6 * (bw + gap) + hint_w + 20 * k;
    const ImVec2 a(centre_x - w * 0.5f, bottom - bh - 22 * k), b(a.x + w, bottom - 10 * k);
    skate_theme::rough_rect(d, a, b, IM_COL32(12, 12, 14, 230), seed_of(a), k * 0.7f);
    float x = a.x + 14 * k;
    const float y = a.y + (b.y - a.y - bh) * 0.5f;
    text(d, f.heading, 17 * k, ImVec2(x, y + 7 * k), blue_light, count);
    x += count_w;
    const auto button = [&](const char* id, const char* label, Look look = Look::dark) {
        const bool pressed = action(d, f, id, ImVec2(x, y), ImVec2(bw, bh), label, look);
        x += bw + gap;
        return pressed;
    };
    if (button("##selsmooth", "SMOOTH")) smooth_picked();
    if (button("##sellinear", "LINEAR")) ease_picked(studio::Ease::linear);
    if (button("##selhold", "HOLD")) ease_picked(studio::Ease::step);
    if (button("##selcopy", "COPY")) copy_keys(v);
    if (button("##seldelete", "DELETE", Look::danger)) delete_picked();
    if (button("##selclear", "LET GO", Look::ghost)) u.picked.clear();
    text(d, f.bold, 12 * k, ImVec2(x + 8 * k, y + 11 * k), faint, "RIGHT-CLICK: MORE");
}
// The round menu round the mouse: what can be done with the chosen keys, a slice each.
void radial_menu(const Fonts& f, const studio::View& v, ImVec2 display) {
    auto& u = ui();
    if (!u.radial) return;
    const float k = f.k;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##animation_studio_radial", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    auto* d = ImGui::GetWindowDrawList();
    // The whole screen is its: a click anywhere is the menu's.
    ImGui::SetCursorScreenPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##radialcatch", display, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const float grow = std::clamp(static_cast<float>(ImGui::GetTime() - u.radial_since) * 9.0f, 0.0f, 1.0f);
    const float ease = 1 - (1 - grow) * (1 - grow);
    const float outer = (78 + 72 * ease) * k, inner = 52 * k;
    const ImVec2 c(std::clamp(u.radial_at.x, outer + 10 * k, display.x - outer - 10 * k), std::clamp(u.radial_at.y, outer + 10 * k, display.y - outer - 10 * k));
    const bool any = !u.picked.empty();
    struct Slice {
        const char* label;
        bool enabled;
        std::function<void()> act;
    };
    const std::array<Slice, 8> slices{{
        {"COPY", any, [&] { copy_keys(v); }},
        {"PASTE", !u.copied_keys.empty(), [&] { paste_keys(v); }},
        {"SMOOTH", any, [] { smooth_picked(); }},
        {"LINEAR", any, [] { ease_picked(studio::Ease::linear); }},
        {"HOLD", any, [] { ease_picked(studio::Ease::step); }},
        {"RESET", any, [&] { reset_picked(v); }},
        {"DELETE", any, [] { delete_picked(); }},
        {"ALL", v.has_clip, [&] { pick_all(v); }},
    }};
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float dist = std::hypot(m.x - c.x, m.y - c.y);
    constexpr float pi = 3.14159265f;
    const float span = 2 * pi / static_cast<float>(slices.size());
    int hot = -1;
    if (dist > inner * 0.7f) {
        float angle = std::atan2(m.y - c.y, m.x - c.x) + pi * 0.5f + span * 0.5f; // the first slice straight up
        while (angle < 0) angle += 2 * pi;
        while (angle >= 2 * pi) angle -= 2 * pi;
        hot = static_cast<int>(angle / span) % static_cast<int>(slices.size());
    }
    d->AddCircleFilled(c, outer + 10 * k, IM_COL32(0, 0, 0, static_cast<int>(90 * ease)), 64);
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const float from = -pi * 0.5f - span * 0.5f + span * static_cast<float>(i) + 0.025f, to = from + span - 0.05f;
        const bool lit = hot == static_cast<int>(i) && slices[i].enabled;
        const ImU32 fill = lit ? blue : slices[i].enabled ? IM_COL32(24, 25, 29, 240) : IM_COL32(24, 25, 29, 150);
        // One shape per slice (out along the rim, back along the inside), so it fills clean with no seams across it.
        const float r_out = outer + (lit ? 8 * k : 0);
        d->PathArcTo(c, r_out, from, to, 24);
        d->PathArcTo(c, inner, to, from, 12);
        d->PathFillConcave(fill);
        const float mid = (from + to) * 0.5f, r = (inner + outer) * 0.5f + (lit ? 4 * k : 0);
        const auto t = text_size(f.heading, 14 * k, slices[i].label);
        text(d, f.heading, 14 * k, ImVec2(c.x + std::cos(mid) * r - t.x * 0.5f, c.y + std::sin(mid) * r - t.y * 0.5f),
             slices[i].enabled ? white : faint, slices[i].label);
    }
    d->AddCircleFilled(c, inner - 6 * k, IM_COL32(236, 236, 234, 250), 40);
    const std::string centre = any ? std::to_string(u.picked.size()) : "NO";
    const auto ct = text_size(f.title, 26 * k, centre.c_str());
    text(d, f.title, 26 * k, ImVec2(c.x - ct.x * 0.5f, c.y - ct.y * 0.5f - 7 * k), IM_COL32(18, 18, 20, 255), centre);
    const char* keys = any && u.picked.size() == 1 ? "KEY" : "KEYS";
    const auto kt = text_size(f.bold, 11 * k, keys);
    text(d, f.bold, 11 * k, ImVec2(c.x - kt.x * 0.5f, c.y + 10 * k), IM_COL32(70, 70, 76, 255), keys);
    // A click on a slice does it; a click anywhere else, or the right button, closes it.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetTime() - u.radial_since > 0.08) {
        if (hot >= 0 && slices[static_cast<std::size_t>(hot)].enabled) slices[static_cast<std::size_t>(hot)].act();
        u.radial = false;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::GetTime() - u.radial_since > 0.08) u.radial = false;
    ImGui::End();
}
// The shortcuts, at the lower right: they slide out to the side and back on the tab at their left edge (or F1).
void shortcuts(ImDrawList* d, const Fonts& f, const studio::View& v, ImVec2 corner, float clip_right) {
    auto& u = ui();
    const float k = f.k;
    using Line = std::tuple<std::optional<Glyph>, const char*, const char*>;
    std::vector<Line> list;
    const char* move = u.move_mode ? "Move (L3: turn)" : u.axis < 0 ? "Turn (L3: axis)" : u.axis == 0 ? "Turn X (L3)" : u.axis == 1 ? "Turn Y (L3)" : "Turn Z (L3)";
    if (u.pad_mode)
        list = {{Glyph::ls, "", move},
                {Glyph::rs, "", u.stick_zoom ? "Zoom / pan (R3)" : "Orbit (R3: zoom)"},
                {Glyph::rt, "", "Play forward"},
                {Glyph::lt, "", "Play back"},
                {Glyph::dpad_ud, "", "Joint"},
                {Glyph::dpad_lr, "", "One frame"},
                {Glyph::a, "", "Key"},
                {Glyph::x, "", "Delete key"},
                {Glyph::y, "", v.playing ? "Pause" : "Play"},
                {Glyph::b, "", "Undo"},
                {Glyph::menu, "", "Save"},
                {Glyph::view, "", "Menu"},
                {Glyph::lb, "", "Zoom timeline (RB)"},
                {Glyph::lb, "", "+ LS: start anim."},
                {Glyph::rb, "", "+ RS: end anim."},
                {Glyph::l3, "", "+ R3 hold: back"}};
    else
        list = {{std::nullopt, "LMB", "Pick / turn"},
                {std::nullopt, "RMB", "Orbit"},
                {std::nullopt, "W A S D", "Turn the view"},
                {std::nullopt, "MMB", "Pan"},
                {std::nullopt, "G / R", u.move_mode ? "Move" : "Turn"},
                {std::nullopt, "X Y Z", "Axis"},
                {std::nullopt, "K", "Key"},
                {std::nullopt, "DEL", "Delete key"},
                {std::nullopt, "UP DN", "Joint"},
                {std::nullopt, "< >", "One frame"},
                {std::nullopt, "SPACE", v.playing ? "Pause" : "Play"},
                {std::nullopt, "CTRL Z", "Undo"},
                {std::nullopt, "SHIFT", "+ click: add keys"},
                {std::nullopt, "CTRL", "+ click: drop keys"},
                {std::nullopt, "RMB", "Keys: round menu"},
                {std::nullopt, "CTRL C/V", "Copy / paste"},
                {std::nullopt, "TAB", "Menu"},
                {std::nullopt, "F8", "Back to skating"}};
    const float col_w = 205 * k, row_h = 26 * k, pad = 12 * k;
    const int rows = static_cast<int>((list.size() + 1) / 2);
    const float panel_w = 2 * col_w + 2 * pad, panel_h = static_cast<float>(rows) * row_h + 2 * pad;
    const ImVec2 pb = corner, pa(pb.x - panel_w, pb.y - panel_h);
    {
        const float target = u.controls_open ? 1.0f : 0.0f;
        u.controls_t += (target - u.controls_t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 10);
        if (std::abs(target - u.controls_t) < 0.002f) u.controls_t = target;
    }
    const float out = u.controls_t * u.controls_t * (3 - 2 * u.controls_t);
    const float slid = (1 - out) * (panel_w + 24 * k);
    const ImVec2 qa(pa.x + slid, pa.y), qb(pb.x + slid, pb.y);
    d->PushClipRect(ImVec2(0, 0), ImVec2(clip_right, ImGui::GetIO().DisplaySize.y), true);
    {
        const ImVec2 ta(qa.x - 30 * k, qb.y - 84 * k), tb(qa.x - 2 * k, qb.y);
        bool lit = false;
        if (hit("##controlstab", ta, tb, &lit)) u.controls_open = !u.controls_open;
        skate_theme::rough_rect(d, ta, tb, lit ? IM_COL32(40, 41, 46, 235) : IM_COL32(12, 12, 14, 215), 95, k * 0.7f);
        const char* arrow = u.controls_open ? ">" : "<";
        const auto t = text_size(f.heading, 20 * k, arrow);
        text(d, f.heading, 20 * k, ImVec2((ta.x + tb.x - t.x) * 0.5f, (ta.y + tb.y - t.y) * 0.5f), lit ? white : IM_COL32(200, 200, 205, 255), arrow);
        if (lit) ImGui::SetTooltip(u.controls_open ? "Hide the shortcuts (F1)" : "Show the shortcuts (F1)");
    }
    if (out > 0.01f) skate_theme::rough_rect(d, qa, qb, IM_COL32(12, 12, 14, 200), 93, k);
    for (std::size_t i = 0; i < list.size() && out > 0.01f; ++i) {
        const auto& [g, key, what] = list[i];
        const ImVec2 at(qa.x + pad + static_cast<float>(i % 2) * col_w, qa.y + pad + static_cast<float>(i / 2) * row_h);
        if (g) glyph(d, f, at, *g, u.playstation);
        else {
            const auto t = text_size(f.bold, 11 * k, key);
            d->AddRectFilled(ImVec2(at.x, at.y + 2 * k), ImVec2(at.x + t.x + 12 * k, at.y + 20 * k), white, 3 * k);
            text(d, f.bold, 11 * k, ImVec2(at.x + 6 * k, at.y + 11 * k - t.y * 0.5f), skate_theme::black, key);
        }
        text(d, f.bold, 13 * k, ImVec2(at.x + 74 * k, at.y + 11 * k - text_size(f.bold, 13 * k, what).y * 0.5f), white, what);
    }
    d->PopClipRect();
}

// A clip has arrived - a trick opened, an animation loaded, a take recorded - so nothing is carried over from the
// last one, and the editor comes up to work on it.
// The game's frame again, through FILMER's fisheye: a grid over the screen whose corners take the picture from where the
// lens says, darkening to black where that is off the frame.
void draw_fisheye(ImDrawList* d, ImVec2 display) {
    const ImTextureID frame = detail::studio_frame_texture();
    if (!frame || display.x < 2 || display.y < 2) return;
    const Lens& l = lens();
    constexpr int columns = 48, rows = 28;
    const float cx = display.x * 0.5f, cy = display.y * 0.5f, feather = display.y * 0.035f;
    d->PushTextureID(frame);
    d->PrimReserve(columns * rows * 6, (columns + 1) * (rows + 1));
    const ImDrawIdx first = static_cast<ImDrawIdx>(d->_VtxCurrentIdx);
    for (int y = 0; y <= rows; ++y)
        for (int x = 0; x <= columns; ++x) {
            const ImVec2 at(display.x * static_cast<float>(x) / columns, display.y * static_cast<float>(y) / rows);
            const float r = std::hypot(at.x - cx, at.y - cy) / cx, f = l.scale * (1 + l.bend * r * r);
            const ImVec2 from(cx + (at.x - cx) * f, cy + (at.y - cy) * f);
            const float inside = std::min({from.x, display.x - from.x, from.y, display.y - from.y});
            const auto shade = static_cast<int>(255 * std::clamp(inside / feather, 0.0f, 1.0f));
            auto* vertex = d->_VtxWritePtr++;
            vertex->pos = at;
            vertex->uv = ImVec2(std::clamp(from.x / display.x, 0.0f, 1.0f), std::clamp(from.y / display.y, 0.0f, 1.0f));
            vertex->col = IM_COL32(shade, shade, shade, 255);
        }
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x) {
            const auto a = static_cast<ImDrawIdx>(first + y * (columns + 1) + x), b = static_cast<ImDrawIdx>(a + 1);
            const auto c = static_cast<ImDrawIdx>(a + columns + 1), e = static_cast<ImDrawIdx>(c + 1);
            for (const ImDrawIdx i : {a, b, e, a, e, c}) *d->_IdxWritePtr++ = i;
        }
    d->_VtxCurrentIdx += static_cast<unsigned>((columns + 1) * (rows + 1));
    d->PopTextureID();
    // The lens's own edge: a soft dark ring as a VX's glass gives.
    const ImU32 clear = IM_COL32(0, 0, 0, 0), dark = IM_COL32(0, 0, 0, 110);
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, display.y * 0.12f), dark, dark, clear, clear);
    d->AddRectFilledMultiColor(ImVec2(0, display.y * 0.88f), display, clear, clear, dark, dark);
}
void on_new_clip(const studio::View& v) {
    auto& u = ui();
    if (u.clip_seen == v.clip_serial) return;
    u.clip_seen = v.clip_serial;
    u.selected = studio::hips_joint;
    u.picked.clear();
    u.key_joint.reset();
    u.gizmo_handle = -1;
    u.dragging_gizmo = u.dragging_playhead = false;
    u.bracket_drag = 0;
    u.axis = -1;
    u.timeline_view = Ui::View::fit;
    u.timeline_zoom = 1;
    u.timeline_scroll = u.rows_scroll = 0;
    u.channel_drag = {};
    u.channel = -1;
    u.rows_followed = 0xffff;
    u.radial = false;
    if (v.has_clip) u.menu = false;
    // The camera keeps whichever shot you chose, but is told again whether to travel with him, since the studio starts
    // out carrying him along and nothing has said otherwise yet.
    studio::set_camera_follow(u.camera_shot != 0);
}

// Adds a key to the chosen ones (Shift + click, a box) or takes it off (Ctrl + click, a box with Ctrl).
void pick_key(std::uint16_t joint, float time, bool add) {
    auto& picked = ui().picked;
    const auto found = std::find_if(picked.begin(), picked.end(), [&](const auto& p) { return p.first == joint && std::abs(p.second - time) < 1e-4f; });
    if (add && found == picked.end()) picked.emplace_back(joint, time);
    if (!add && found != picked.end()) picked.erase(found);
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
    // Header, left: KEY and DELETE KEY over the transport.
    {
        const auto* here = key_here(v, u.selected);
        const auto* chosen_track = track_view(v, u.selected);
        const bool locked = chosen_track && chosen_track->locked;
        if (action(d, f, "##tlkey", ImVec2(at.x + 20 * k, at.y + 8 * k), ImVec2(120 * k, 28 * k), "KEY  (K)", Look::key, v.has_clip && !locked)) key_joint();
        if (action(d, f, "##tldelkey", ImVec2(at.x + 146 * k, at.y + 8 * k), ImVec2(116 * k, 28 * k), "DELETE KEY", Look::dark, here && !locked)) delete_key(v);
    }
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
                // Outside the brackets the edits do nothing (the game's own animation plays), so the curve is 0 there.
                const float window_start = v.bracket_start, window_end = v.bracket_end < 0 ? v.duration : v.bracket_end;
                for (float x = ruler_left; x <= ruler_right + 0.5f * step; x += step) {
                    const float t = std::max(0.0f, t_of(x));
                    float value = t < window_start - 1e-4f || t > window_end + 1e-4f ? 0.0f : studio::channel_value(studio::evaluate(curve, t), c);
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
                    // Shift + click adds the key to the chosen ones, Ctrl + click takes it off.
                    if (ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl) {
                        pick_key(joint.index, key.time, ImGui::GetIO().KeyShift);
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
                if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) u.picked.clear();
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
                if (hit && !track->locked && (ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl)) {
                    // Shift + click adds the key to the chosen ones, Ctrl + click takes it off.
                    on_key = true;
                    pick_key(joint.index, key.time, ImGui::GetIO().KeyShift);
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
            if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) u.picked.clear();
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
    // The right button over the rows: dragged, a box that chooses every key inside it (Shift adds them, Ctrl takes them
    // off); clicked, the round menu of what can be done with the chosen keys, round the mouse (a key clicked that was not
    // chosen is chosen first). The right button is free down here; up in the view it orbits the camera.
    {
        auto& io = ImGui::GetIO();
        const ImVec2 m = io.MousePos;
        const bool over_rows = m.x >= ruler_left && m.x <= ruler_right && m.y >= rows_top && m.y <= at.y + size.y;
        if (over_rows && !u.blocked && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            u.band_held = true;
            u.band_moved = false;
            u.band_from = m;
        }
        if (u.band_held) {
            if (std::hypot(m.x - u.band_from.x, m.y - u.band_from.y) > 5 * k) u.band_moved = true;
            const ImVec2 a(std::min(u.band_from.x, m.x), std::min(u.band_from.y, m.y));
            const ImVec2 b(std::max(u.band_from.x, m.x), std::max(u.band_from.y, m.y));
            if (u.band_moved) {
                d->AddRectFilled(a, b, io.KeyCtrl ? IM_COL32(255, 92, 92, 36) : IM_COL32(1, 131, 255, 40));
                d->AddRect(a, b, io.KeyCtrl ? IM_COL32(255, 140, 140, 200) : IM_COL32(120, 190, 255, 200), 0, 0, 1.6f * k);
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                u.band_held = false;
                if (u.band_moved) {
                    if (!io.KeyShift && !io.KeyCtrl) u.picked.clear();
                    std::size_t caught = 0;
                    for (const auto& key : drawn_keys())
                        if (key.at.x >= a.x && key.at.x <= b.x && key.at.y >= a.y && key.at.y <= b.y) {
                            pick_key(key.joint, key.time, !io.KeyCtrl);
                            ++caught;
                        }
                    if (caught) say(std::to_string(u.picked.size()) + " keys chosen: drag one to move them all, right-click for more.");
                } else {
                    const DrawnKey* under = nullptr;
                    float nearest = 11 * k;
                    for (const auto& key : drawn_keys())
                        if (const float apart = std::hypot(key.at.x - m.x, key.at.y - m.y); apart < nearest) {
                            nearest = apart;
                            under = &key;
                        }
                    if (under && !is_picked(under->joint, under->time)) {
                        u.picked.clear();
                        u.picked.emplace_back(under->joint, under->time);
                    }
                    u.radial = true;
                    u.radial_at = m;
                    u.radial_since = ImGui::GetTime();
                }
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
        if (near_bracket && !u.blocked && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
    if (!u.blocked && mouse.x >= at.x && mouse.y >= at.y && mouse.x <= at.x + size.x && mouse.y <= at.y + size.y) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            if (ImGui::GetIO().KeyCtrl) zoom_timeline(wheel > 0 ? 1.25f : 1 / 1.25f);
            else if (ImGui::GetIO().KeyShift) u.timeline_scroll -= wheel * 60 * k;
            else u.rows_scroll -= wheel * (row_h + gap);
        }
    }
}


// The skater: dots on the joints that can be chosen, the glowing box on the chosen one, and the drag.
// The studio's rail (View::rail_a/b, its top): a round steel bar on two posts down to the floor. Drawn over the
// game's picture, under the dots and the gizmo; where it passes behind the camera it is left out.
void draw_rail(ImDrawList* d, const studio::View& v, const Projector& project) {
    if (!v.rail_valid || !project.valid) return;
    const float r = v.rail_radius;
    const auto at = [&](float t) {
        return studio::Vec3{v.rail_a[0] + (v.rail_b[0] - v.rail_a[0]) * t, v.rail_a[1] - r + (v.rail_b[1] - v.rail_a[1]) * t,
                            v.rail_a[2] + (v.rail_b[2] - v.rail_a[2]) * t};
    };
    const auto offset = [](const studio::Vec3& p, const studio::Vec3& by, float s) {
        return studio::Vec3{p[0] + by[0] * s, p[1] + by[1] * s, p[2] + by[2] * s};
    };
    // Its posts first, so the bar is over them: square steel down to the floor.
    const float floor = v.floor_valid ? v.floor : at(0.5f)[1] - 0.45f;
    for (const float t : {0.12f, 0.88f}) {
        const auto top = at(t);
        const studio::Vec3 bottom{top[0], floor, top[2]};
        const auto a = project(offset(top, project.right, -0.02f)), b = project(offset(top, project.right, 0.02f));
        const auto c = project(offset(bottom, project.right, 0.02f)), e = project(offset(bottom, project.right, -0.02f));
        if (a && b && c && e) {
            d->AddQuadFilled(*a, *b, *c, *e, IM_COL32(58, 60, 66, 235));
            d->AddLine(*a, *e, IM_COL32(150, 154, 162, 200), 1.0f);
        }
    }
    // A quad shaded from its first edge (a, b) to its second (c, e).
    const auto shade = [d](ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 e, ImU32 from, ImU32 to) {
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        const auto i = static_cast<ImDrawIdx>(d->_VtxCurrentIdx);
        d->PrimReserve(6, 4);
        d->PrimWriteIdx(i); d->PrimWriteIdx(static_cast<ImDrawIdx>(i + 1)); d->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
        d->PrimWriteIdx(i); d->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2)); d->PrimWriteIdx(static_cast<ImDrawIdx>(i + 3));
        d->PrimWriteVtx(a, uv, from); d->PrimWriteVtx(b, uv, from); d->PrimWriteVtx(c, uv, to); d->PrimWriteVtx(e, uv, to);
    };
    // The bar: steel, lit from above, in short pieces so the part behind the camera can be left out.
    constexpr int pieces = 32;
    for (int i = 0; i < pieces; ++i) {
        const float t0 = static_cast<float>(i) / pieces, t1 = static_cast<float>(i + 1) / pieces;
        const auto c0 = at(t0), c1 = at(t1);
        const auto top0 = project(offset(c0, project.up, r)), top1 = project(offset(c1, project.up, r));
        const auto bot0 = project(offset(c0, project.up, -r)), bot1 = project(offset(c1, project.up, -r));
        const auto mid0 = project(c0), mid1 = project(c1);
        if (!top0 || !top1 || !bot0 || !bot1 || !mid0 || !mid1) continue;
        shade(*top0, *top1, *mid1, *mid0, IM_COL32(205, 210, 218, 245), IM_COL32(120, 124, 132, 245));
        shade(*mid0, *mid1, *bot1, *bot0, IM_COL32(120, 124, 132, 245), IM_COL32(52, 54, 60, 245));
        d->AddLine(*top0, *top1, IM_COL32(245, 248, 252, 220), 1.5f);
    }
}
void viewport(ImDrawList* d, const Fonts& f, const studio::View& v, const Projector& project, ImVec2 area_min, ImVec2 area_max) {
    auto& u = ui();
    const float k = f.k;
    draw_rail(d, v, project);
    const auto mouse = ImGui::GetIO().MousePos;
    const bool inside = !u.blocked && mouse.x >= area_min.x && mouse.y >= area_min.y && mouse.x <= area_max.x && mouse.y <= area_max.y;
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
            if (std::find(u.also.begin(), u.also.end(), j.joint) != u.also.end()) {
                d->AddCircleFilled(*at, 6.5f * k, blue);
                d->AddCircle(*at, 11.0f * k, blue_light, 24, 2.0f * k);
                continue;
            }
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
    if (u.blocked || (!inside && u.gizmo_handle < 0 && !u.dragging_gizmo && !u.orbiting && !u.panning)) return;
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
        } else if (hovered && io.KeyCtrl) toggle_in_group(*hovered);
        else if (hovered && *hovered != u.selected) {
            u.selected = *hovered;
            u.also.clear();
        } else if (sel && can_edit) {
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
            "Click a dot on the skater, then drag it to turn the joint.",
            "The arrows on a joint move it along one axis, the rings turn it about one.",
            "G moves, R turns, X Y Z lock an axis, F follows the joint.",
            "S turns one joint alone. N keeps a foot on the deck. L swaps object and world.",
            "K keys the pose on this frame. Keys always sit on whole frames.",
            "G moves the hips or the board. X, Y and Z lock an axis.",
            "Save your trick and do it in game: your version plays on that trick.",
            "Right-click chosen keys in the timeline for copy, paste, smooth and more.",
            "Packs group your tricks: switch any number on, and share them, in PACKS.",
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
    auto& u = ui();
    if (!studio::is_open()) {
        u.was_open = false;
        return;
    }
    const auto v = studio::view();
    u.own_lead_right = v.own_lead_right;
    if (!u.was_open) {
        // Just opened: the menu when there is nothing to edit, the editor when there is.
        u.was_open = true;
        u.menu = !v.has_clip;
        u.radial = false;
        u.your_name_filled = false;
        nav().focus = 0;
    }
    if (!v.has_clip) u.menu = true;
    studio::advance(ImGui::GetIO().DeltaTime);
    const auto display = ImGui::GetIO().DisplaySize;
    Fonts f{s.menu.body ? s.menu.body : ImGui::GetFont(), s.menu.bold ? s.menu.bold : ImGui::GetFont(),
            s.menu.heading ? s.menu.heading : ImGui::GetFont(), s.menu.title_large ? s.menu.title_large : s.menu.title ? s.menu.title : ImGui::GetFont(),
            1.0f};
    f.display = s.menu.display;
    f.display_big = s.menu.display_big;
    f.display_small = s.menu.display_small;
    f.marker_small = s.menu.marker_small;
    f.covers = {s.menu.cover_bebas ? s.menu.cover_bebas : f.heading, s.menu.cover_dirt ? s.menu.cover_dirt : f.heading,
                s.menu.cover_sedgwick ? s.menu.cover_sedgwick : f.heading, s.menu.cover_knewave ? s.menu.cover_knewave : f.heading};
    // Any screen: laid out on a 1920 x 1080 page, scaled to fit the height and the game's menu frame (the whole width up
    // to 21:9, a 21:9 frame in the middle of anything wider).
    const float frame_w = std::min(display.x, display.y * 21.0f / 9.0f);
    const float left = (display.x - frame_w) * 0.5f, right = left + frame_w;
    f.k = std::clamp(std::min(display.y / 1080.0f, frame_w / 1700.0f), 0.45f, 3.0f);
    const float k = f.k;
    nav_frame();
    on_new_clip(v);
    {
        const bool filming = u.camera_shot == filmer_shot && u.fisheye && v.has_clip;
        studio::set_wanted_fov(filming ? 100.0f : 0.0f);
        if (filming && !u.menu) detail::want_studio_frame();
        lens().on = filming && !u.menu && detail::studio_frame_texture() != ImTextureID{};
    }
    const bool card = any_card_open(v) || v.confirm_close;
    u.blocked = card || u.radial || u.menu;
    const float timeline_h = std::min(display.y * 0.34f, 360 * k);
    const float side_w = 434 * k; // the joint's card and the shortcuts, at the right
    const ImVec2 view_min(u.hide_ui ? left : left + (tool_strip_margin + tool_strip_width + 22) * k, u.hide_ui ? 0 : 80 * k),
        view_max(u.hide_ui ? right : right - side_w - 40 * k, u.hide_ui ? display.y : display.y - timeline_h);
    // The skater stands in the middle of the free space.
    studio::set_view_shift((display.y * 0.5f - (view_min.y + view_max.y) * 0.5f) / display.y,
                           ((view_min.x + view_max.x) * 0.5f - display.x * 0.5f) / display.y);
    const auto project = projector(v, display);
    // The pad and the keys first, so a press this frame reaches what is drawn this frame.
    controller(v, project);
    if ((u.menu || any_card_open(v)) && !v.confirm_close) menu_keyboard(v);
    if (!u.menu || u.menu_tab != 0) end_hover_preview();
    // What the studio last said: held long enough to read, by its length, and then gone. A failure stays, because that
    // is something to act on.
    std::string line = v.failure.empty() ? v.status : v.failure;
    if (v.failure.empty()) {
        if (line != u.status_said) {
            u.status_said = line;
            u.status_until = ImGui::GetTime() + 4.0 + static_cast<double>(line.size()) * 0.045;
        }
        if (ImGui::GetTime() > u.status_until) line.clear();
    }
    if (ImGui::GetTime() < u.message_until) line = u.message;
    if (!v.pose_seen) line = "Waiting for the skater's pose...";

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##animation_studio", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    auto* d = ImGui::GetWindowDrawList();
    library_news();
    if (u.menu) {
        u.menu_fade = std::min(1.0f, u.menu_fade + ImGui::GetIO().DeltaTime * 8);
        menu_backdrop(d, f, display, 0.6f + 0.4f * u.menu_fade);
        // The MARKET has its own wall behind it, faded in as it is opened.
        u.market_fade = u.menu_tab == 2 ? std::min(1.0f, u.market_fade + ImGui::GetIO().DeltaTime * 4) : 0.0f;
        shop_wall(d, display, u.market_fade * (0.6f + 0.4f * u.menu_fade));
        menu_header(d, f, v, display, left, right);
        const ImVec2 ca(left + 48 * k, 124 * k), cb(right - 48 * k, display.y - 76 * k);
        switch (u.menu_tab) {
        case 0: tricks_page(d, f, v, ca, cb); break;
        case 1: packs_page(d, f, v, ca, cb); break;
        case 2: market_page(d, f, v, ca, cb); break;
        default: settings_page(d, f, v, ca, cb); break;
        }
        menu_footer(d, f, v, display, left, right, line);
    } else {
        u.menu_fade = 0;
        if (lens().on) draw_fisheye(d, display);
        if (v.previewing) {
            preview_screen(d, f, v, display);
            ImGui::End();
            ImGui::PopStyleVar(2);
            draw_cards(f, v, display);
            nav_end_frame();
            return;
        }
        viewport(d, f, v, project, view_min, view_max);
        if (u.hide_ui) {
            // Only the skater and the room he stands in: for filming a trick or taking a picture of it. The camera and the
            // playhead still answer, so a trick can be played and watched clean.
            if (!card) keyboard(v);
            ImGui::End();
            ImGui::PopStyleVar(2);
            nav_end_frame();
            return;
        }
        editor_top(d, f, v, display, left, right);
        tool_strip(d, f, v, ImVec2(left + tool_strip_margin * k, 98 * k));
        joint_card(d, f, v, ImVec2(right - 20 * k - side_w, 98 * k), side_w);
        timeline(d, f, v, ImVec2(left, display.y - timeline_h), ImVec2(frame_w, timeline_h));
        selection_bar(d, f, v, (left + right) * 0.5f - side_w * 0.25f, display.y - timeline_h);
        shortcuts(d, f, v, ImVec2(right - 20 * k, display.y - timeline_h - 14 * k), right);
        if (!line.empty()) {
            const auto w = text_size(f.bold, 15 * k, line.c_str());
            const ImVec2 at(view_min.x + 24 * k, 100 * k);
            skate_theme::rough_rect(d, ImVec2(at.x - 14 * k, at.y - 7 * k), ImVec2(at.x + w.x + 14 * k, at.y + w.y + 7 * k), IM_COL32(16, 16, 18, 230), 41, k * 0.6f);
            text(d, f.bold, 15 * k, at, v.failure.empty() ? white : skate_theme::danger, line);
        }
        if (!card) keyboard(v);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    // Over everything: a card when one is up, the close warning, the round menu.
    if (v.confirm_close) {
        begin_card(display, false);
        close_warning(ImGui::GetWindowDrawList(), f, v, display);
        end_card();
    } else draw_cards(f, v, display);
    if (!u.menu) radial_menu(f, v, display);
    nav_end_frame();
}
} // namespace dingosdk::overlay::detail

