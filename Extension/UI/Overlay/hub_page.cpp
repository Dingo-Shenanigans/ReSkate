#include "overlay_internal.h"
#include "skate_style.h"
#include "skate_menu_internal.h"
#include "cursor.h"
#include "input_capture.h"
#include "scripted_pad.h"
#include "Extension/UI/NativeMenu/native_menu.h"
#include "Extension/UI/NativeMenu/native_menu_view.h"
#include "Extension/UI/NativeMenu/native_tools_view.h"
#include "Extension/Boot/discord_presence.h"
#include "Extension/Assets/map_download.h"
#include "Extension/Assets/mod_store.h"
#include "Extension/Multiplayer/Steam/steam_avatars.h"
#include "Extension/Plugins/plugin_runtime.h"
#include "Extension/Music/local_music_playback.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Extension/Rendering/replay_export.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/Input/voice_input.h"
#include "Engine/Game/Multiplayer/object_placement.h"
#include "Engine/Game/Multiplayer/session_limits.h"
#include "Engine/Game/Multiplayer/tick_settings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// ReSkate's two pages of the game's own pause menu, Multiplayer and Mod Options, drawn by
// ReSkate: the game's menu widgets are a button, a text box and a line of text, which cannot
// make a list with columns, so the game keeps the frame around a page (its top bar, title and
// Back prompt) and this draws everything in it: the page's tabs and whichever is open (the
// server browser, hosting, the session, voice chat; travel, parks, the player and visuals).
//
// The game still owns the mouse and the controller while its menu is up, so this layer only
// watches them (State::hub_pointer). It keeps its own focus, moved with the D-pad or the arrow
// keys and pressed with A or Enter, so every control is reachable without a mouse. Typing in a
// text box is the exception: for that long the overlay takes the keyboard, as the chat box
// does (State::hub_typing).

namespace dingosdk::overlay {
namespace {
std::atomic<HubPageFeed> page_feed{};
std::atomic<UiSound> ui_sound{};
std::mutex hub_callbacks_mutex;
CallbacksV3 hub_callbacks; // under the mutex
} // namespace
void set_hub_page_feed(HubPageFeed feed) noexcept { page_feed.store(feed); }
void set_ui_sound(UiSound play) noexcept { ui_sound.store(play); }
void play_ui_sound(std::uint32_t sound) noexcept {
    if (const auto play = ui_sound.load()) play(sound);
}
void set_hub_callbacks(const CallbacksV3 &callbacks) noexcept {
    std::lock_guard lock(hub_callbacks_mutex);
    hub_callbacks = callbacks;
}
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
namespace {
namespace view = dingosdk::multiplayer::menu_view;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// The game's menu is laid out on a screen of 3840 by 2160 units, scaled to fit the window
// whole (so by its width in a tall window and its height in a wide one). What does not fit
// the 16:9 of that is given to the menu too: up to 4704 units across, centred in anything
// wider, and all of a taller window's height, where the top of the page moves down and the
// Back prompt up by a twentieth of the extra. Its pages keep 192 units clear at the sides.
// In those units, on a 16:9 screen: the page's tabs sit under its title at 572, its body
// starts at 770, and its Back prompt sits under 1905. (Measured on the game's own pages at
// 16:9, 21:9, 32:9, 11:7 and a window taller than wide.) ReSkate's pages have no title
// (native_menu.cpp writes a blank one), so their tabs go up into its place, under the icon bar
// that ends near 290, and the body follows them up.
constexpr float layout_width = 3840.f, layout_height = 2160.f, layout_widest = 4704.f, side_margin = 192.f, tabs_top = 340.f,
                tabs_height = 84.f, body_top = 520.f, body_bottom = 1905.f, body_most = 4400.f;
// skate.'s own menu colours (its Settings, Skaters and Skatepedia pages): near-black tiles, the one
// the D-pad or the keys are on in its blue with black text and halftone dots (theme::focus_fill,
// tile_rect), the one the pointer is over a lighter grey, and the main action on the light grey of
// its chosen tab (`primary_rest`), so it never reads as the focus. `accent` is the blue for marks.
constexpr ImU32 accent = skate_theme::blue, accent_hover = skate_theme::blue_hover, accent_off = IM_COL32(1, 131, 255, 90),
                accent_sub = IM_COL32(214, 234, 255, 255), focus_ink = IM_COL32(11, 11, 11, 255), row_hover = IM_COL32(56, 57, 61, 245),
                hint_ink = IM_COL32(110, 110, 110, 255), soft = IM_COL32(210, 210, 214, 255),
                primary_rest = IM_COL32(226, 227, 230, 255), primary_hover = IM_COL32(246, 246, 248, 255), primary_off = IM_COL32(226, 227, 230, 70);
// A mark in the blue (an ON tag, a bar, a switch) inside a row that may have the focus: black there,
// where the row is the blue itself.
constexpr ImU32 on_row(bool focused, ImU32 colour) { return focused && colour == skate_theme::blue ? skate_theme::black : colour; }

// The Multiplayer page's own look, on the SERVER BROWSER and FRIENDS tabs and its tab strip, and
// the other pages' since: the kit's dark rows, and a type plate on each server in the kit's tag
// colours (official blue, server grey, lobby green).
constexpr ImU32 tile_dark = IM_COL32(16, 16, 16, 242), card_light = IM_COL32(217, 219, 223, 255), card_light_hover = IM_COL32(196, 198, 203, 255),
                field_on_white = IM_COL32(228, 230, 233, 255), sub_ink = IM_COL32(138, 140, 146, 255), sub_ink_lit = IM_COL32(10, 40, 82, 255),
                track = IM_COL32(61, 62, 66, 255), track_lit = IM_COL32(201, 204, 209, 255), chip_ink = IM_COL32(185, 187, 192, 255),
                plate_grey = IM_COL32(61, 62, 66, 255), raised = IM_COL32(29, 29, 31, 245), raised_hover = IM_COL32(82, 83, 88, 255);

// (FRIENDS comes after SERVER BROWSER on the strip; its number here is only a name.)
enum class Tab { browser, host, session, voice, settings, ignored, friends };
// The game's own menu sounds, by their numbers in its list (UI/NativeMenu/ui_sound.h).
constexpr std::uint32_t sound_navigate = 3, sound_select = 7, sound_tab = 56;
enum class Kind { all, servers, lobbies };
enum class Look { tile, primary, plain, chosen };

constexpr std::uint32_t hash(std::string_view text, std::uint64_t salt = 0) {
    std::uint32_t value = 2166136261U ^ static_cast<std::uint32_t>(salt * 2654435761ULL);
    for (const char c : text) value = (value ^ static_cast<unsigned char>(c)) * 16777619U;
    return value ? value : 1;
}

struct Row {
    const MultiplayerLobby *lobby{};
    std::string map;
    bool joinable{}, here{}, full{}, own{}, favorite{};
    // Its map's picture: the level's asset when the map is installed here (menu_map_pictures.cpp),
    // else its Thunderstore package's icon URL when it is a known map package; neither: a plain tile.
    std::string asset, icon;
};
// A Steam friend in the game, on the FRIENDS tab: the public session they are in (when it is in the
// list), whether they are in the player's own, and whether the player can join them.
struct FriendRow {
    const MultiplayerFriend *who{};
    const MultiplayerLobby *where{};
    bool here{}, joinable{};
    // Their session's map, with its picture found as the server browser's rows find theirs (only
    // `map`, `asset` and `icon` are set).
    Row place;
};
struct Item {
    std::uint32_t id{};
    ImVec2 a, b;
    std::uint32_t list{}; // the scrolling list it is a row of, or 0
};

struct Hub {
    HubPage page;
    Clock::time_point next_page{}, next_model{};
    Model full;             // the game as the menus show it, read a few times a second
    MultiplayerModel model; // its multiplayer part
    CallbacksV3 callbacks;
    bool shown{}, dismissed{}, was_active{}, searched{};
    Clock::time_point dismissed_at{}; // when the front page over the HUB was dismissed
    // Mod Options: the same rows and actions the page has always had (native_tools_view.h).
    native_tools::State tools;
    int tools_tab{};
    // Recording a bind (the BINDS tab): which action (0: none), what has been pressed so far,
    // and when it gives up (GetTickCount64).
    int recording_bind{};
    ControllerComboCapture bind_capture;
    ULONGLONG recording_until{};
    Tab tab{Tab::browser};
    // The browser.
    std::vector<Row> rows;
    std::size_t listed{};
    int skaters{};
    bool rows_stale{true};
    std::string search, password, code, code_password;
    Kind kind{Kind::all};
    bool friends_only{}, hide_full{}, same_map{}, code_mode{};
    // Dedicated servers the player marked, by Steam ID: first in the list, saved with the profile.
    std::set<std::uint64_t> favorites;
    bool favorites_loaded{}, favorites_only{};
    view::Sort sort{view::Sort::players};
    bool sort_reverse{}; // the other way round: fewest skaters, Z to A
    std::uint64_t selected{};
    // The row whose actions are open under it (A on a row; 0: none).
    std::uint64_t open{};
    // The JOIN YOUR FRIENDS tile over the list: the session most friends are on.
    const MultiplayerLobby *crew{};
    // Thunderstore's map packages' icons by their squashed title (squashed()), for servers on a map
    // not installed here; made again when the listing changes.
    unsigned listing_serial = ~0U;
    std::map<std::string, std::string> package_icons;
    // The maps whose pictures the rows show (grid_levels), and whether they were asked for since
    // the model was last read.
    std::vector<const Level *> grid;
    bool pictures_asked{};
    // The FRIENDS tab.
    std::vector<FriendRow> friends;
    // Hosting.
    std::string host_name, host_password;
    bool host_seeded{}, host_named{}, public_lobby{true};
    unsigned capacity = static_cast<unsigned>(multiplayer_lobby_player_limit), tps = multiplayer_default_tps;
    // The session.
    std::string selected_player;
    // Voice: a change sent but not yet in the model, so a second press builds on the first.
    std::optional<VoiceSettings> voice_pending;
    ULONGLONG voice_pending_until{};
    // Focus, the keyboard and scrolling.
    std::uint32_t focus{}, fire{}, typing{};
    std::uint32_t focus_next{}; // a control that is only drawn from the next frame on, to focus then
    // The game's menu sound this frame calls for (its number; ~0: none), and the focus it was
    // last played for moving to.
    std::uint32_t sound = ~0U, focus_heard{};
    ULONGLONG sound_at{};
    bool ring{}, focus_moved{};
    std::vector<Item> items, last;
    std::map<std::uint32_t, float> scrolls;
    // The row of each list the focus was last on: where it goes back to when it returns to the
    // list, in place of the row that happens to be level with where it came from.
    std::map<std::uint32_t, std::uint32_t> list_focus;
    std::uint32_t pad_before{};
    std::array<bool, 8> keys_before{};
    ULONGLONG repeat_at{};
    std::string notice;
    std::string joining_name; // the session a join was asked for from this page, for its card
    // A join followed to its end, so one that did not get in can say why (failed_page).
    // join_asked: when one was asked for here and has not shown in the model yet (GetTickCount64).
    bool join_seen{}, join_fetching{}, join_cancelled{};
    ULONGLONG join_asked{};
    std::string failed, failed_name; // why the last join did not get in, and where to; empty: nothing to say
    ULONGLONG notice_until{};
};
Hub &hub() {
    static Hub value;
    return value;
}

// What one frame draws with.
struct Ui {
    Hub &h;
    ImDrawList *draw;
    ImFont *bold, *body, *heading;
    float u;
    ImVec2 mouse;
    bool input, clicked, doubled, moved;
    float wheel;
    const MultiplayerModel &mp;
    ControllerInput pad;
};

void set_typing(Hub &h, std::uint32_t id) {
    auto &s = state();
    if (h.typing == id) return;
    const bool was = h.typing != 0;
    h.typing = id;
    if (was == (id != 0)) return;
    s.hub_typing.store(id != 0);
    // The keyboard and the cursor change hands, as when the chat box opens and closes.
    sync_menu_cursor();
    std::lock_guard lock(s.input_mutex);
    s.input.clear();
}
void say(Hub &h, std::string text) {
    h.notice = std::move(text);
    h.notice_until = GetTickCount64() + 5000;
}
bool send(Hub &h, const char *action, const std::string &argument = {}, const std::string &password = {}) {
    if (queue_multiplayer_action(action, argument, password)) return true;
    say(h, "Couldn't do that just now. Try again.");
    return false;
}
// A join was asked for from this page.
void asked_join(Hub &h) {
    h.join_asked = GetTickCount64();
    h.join_cancelled = false;
    h.failed.clear();
}
void wipe(std::string &secret) {
    SecureZeroMemory(secret.data(), secret.size());
    secret.clear();
}

bool inside(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.x < b.x && p.y >= a.y && p.y < b.y; }
float width_of(ImFont *font, float size, const std::string &text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }
// One line of text inside a box, cut off at its right edge.
void label(ImDrawList *draw, ImFont *font, float size, ImVec2 at, float right, ImU32 colour, const std::string &text) {
    if (right <= at.x) return;
    draw->PushClipRect(ImVec2(at.x, at.y - size), ImVec2(right, at.y + size * 2), true);
    draw->AddText(font, size, at, colour, text.c_str());
    draw->PopClipRect();
}

// A control: takes its place in this frame's focus order and says what is happening to it.
// `clip`: the part of the screen it can be pointed at in (a row of a scrolling list).
struct Hit {
    bool hover{}, focus{}, press{};
};
Hit item(Ui &ui, std::uint32_t id, ImVec2 a, ImVec2 b, const Item *clip = nullptr) {
    auto &h = ui.h;
    h.items.push_back({id, a, b, clip ? clip->id : 0});
    if (clip && h.focus == id) h.list_focus[clip->id] = id;
    Hit hit;
    const bool over = ui.input && inside(ui.mouse, a, b) && (!clip || inside(ui.mouse, clip->a, clip->b));
    if (over && ui.moved && !h.typing) h.focus = id, h.ring = false;
    // Lit by the pointer only while the pointer is in charge: once the D-pad or the keys have
    // the focus, what the mouse was left resting on is not lit beside it. A click there still counts.
    hit.hover = over && !h.ring;
    hit.focus = h.focus == id && (h.ring || hit.hover);
    hit.press = ui.input && ((over && ui.clicked) || h.fire == id);
    if (hit.press) h.focus = id, h.sound = sound_select;
    return hit;
}

// A button in one of the game's looks. `enabled` off: drawn dim, and not a control at all.
bool button(Ui &ui, std::uint32_t id, ImVec2 a, ImVec2 b, const std::string &text, Look look = Look::tile, bool enabled = true,
            const std::string &value = {}, const Item *clip = nullptr) {
    const float u = ui.u, height = b.y - a.y, size = std::min(40 * u, height * .42f);
    const Hit hit = enabled ? item(ui, id, a, b, clip) : Hit{};
    const bool lit = hit.focus || hit.hover;
    ImU32 fill = tile_dark, ink = enabled ? theme::paper : theme::muted;
    // The main action: light grey with black text at rest, whiter under the pointer, and the focus
    // blue under the D-pad or the keys.
    if (look == Look::primary)
        fill = !enabled ? primary_off : hit.focus && !hit.hover ? theme::focus_fill : hit.hover ? primary_hover : primary_rest,
        ink = !enabled ? IM_COL32(245, 245, 245, 140) : focus_ink;
    // Focus as skate. draws it: its blue with black text under the D-pad or the keys, a lighter grey
    // under the pointer; the tiles themselves near black.
    else if (hit.focus && !hit.hover) fill = theme::focus_fill, ink = enabled ? focus_ink : theme::muted;
    else if (lit) fill = row_hover;
    else if (look == Look::plain) fill = raised;
    else if (look == Look::chosen) fill = theme::paper, ink = skate_theme::black;
    theme::tile_rect(ui.draw, a, b, fill, id & 0xfff, u * 2);
    if (value.empty()) {
        ui.draw->AddText(ui.bold, size, ImVec2(a.x + (b.x - a.x - width_of(ui.bold, size, text)) * .5f, a.y + (height - size) * .5f), ink, text.c_str());
    } else {
        // A setting: its name on the left, what it is now on the right.
        const float w = width_of(ui.bold, size, value);
        label(ui.draw, ui.body, size, ImVec2(a.x + 30 * u, a.y + (height - size) * .5f), b.x - w - 50 * u, ink, text);
        ui.draw->AddText(ui.bold, size, ImVec2(b.x - w - 30 * u, a.y + (height - size) * .5f), enabled && !lit && look != Look::chosen && look != Look::primary ? theme::blue : ink, value.c_str());
    }
    return hit.press;
}

// What was typed this frame goes into `text` (the overlay has the keyboard while typing).
void take_typing(std::string &text, std::size_t limit) {
    auto &io = ImGui::GetIO();
    for (const ImWchar c : io.InputQueueCharacters) {
        if (c < 32 || c == 127 || text.size() + 3 > limit) continue;
        if (c < 0x80) text += static_cast<char>(c);
        else if (c < 0x800) text += static_cast<char>(0xc0 | (c >> 6)), text += static_cast<char>(0x80 | (c & 0x3f));
        else text += static_cast<char>(0xe0 | (c >> 12)), text += static_cast<char>(0x80 | ((c >> 6) & 0x3f)), text += static_cast<char>(0x80 | (c & 0x3f));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && !text.empty()) {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back();
        if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xc0) text.pop_back();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
        if (const char *pasted = ImGui::GetClipboardText())
            for (; *pasted && text.size() < limit; ++pasted)
                if (static_cast<unsigned char>(*pasted) >= 32) text += *pasted;
}
// A text box in the game's white. Pressing it gives it the keyboard; Enter (returns true) or
// Escape gives the keyboard back, and so does a click anywhere else.
// `fill`: its colour (another white than the page's, on a white row); `clip` as for item().
bool field(Ui &ui, std::uint32_t id, ImVec2 a, ImVec2 b, std::string &text, const char *hint, bool secret = false, std::size_t limit = 64,
           ImU32 fill = theme::paper, const Item *clip = nullptr) {
    auto &h = ui.h;
    const float u = ui.u, size = 40 * u;
    const Hit hit = item(ui, id, a, b, clip);
    bool typing = h.typing == id, entered{};
    if (hit.press && !typing) set_typing(h, id), typing = true;
    else if (typing && ui.clicked && !hit.hover) set_typing(h, 0), typing = false;
    if (typing) {
        take_typing(text, limit);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) set_typing(h, 0), typing = false;
        else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) set_typing(h, 0), typing = false, entered = true;
    }
    theme::tile_rect(ui.draw, a, b, fill, id & 0xfff, u * 2);
    if (typing || hit.focus) ui.draw->AddRect(a, b, theme::blue, 0, 0, 6 * u);
    const bool empty = text.empty() && !typing;
    std::string shown = empty ? std::string(hint) : secret ? std::string(text.size(), '*') : text;
    if (typing && std::fmod(ImGui::GetTime(), 1.0) < .5) shown += '|';
    label(ui.draw, ui.body, size, ImVec2(a.x + 28 * u, a.y + (b.y - a.y - size) * .5f), b.x - 16 * u, empty ? hint_ink : skate_theme::black, shown);
    return entered;
}

// A column that scrolls: rows are placed with next(), which gives where the next one goes.
struct Scroller {
    Ui &ui;
    Item box;
    float &offset;
    float y;
    Scroller(Ui &owner, std::uint32_t id, ImVec2 a, ImVec2 b) : ui(owner), box{id, a, b}, offset(owner.h.scrolls[id]), y(a.y - owner.h.scrolls[id]) {
        ui.draw->PushClipRect(a, b, true);
    }
    // The top of a row `height` tall; `drawn` says whether any of it is in view.
    float next(float height, float gap, bool &drawn) {
        const float top = y;
        y += height + gap;
        drawn = top + height >= box.a.y && top <= box.b.y;
        return top;
    }
    // Keeps the focused row in view once the focus has moved to it.
    void reveal(std::uint32_t id, float top, float height) {
        if (ui.h.focus != id || !ui.h.focus_moved) return;
        if (top < box.a.y) offset -= box.a.y - top;
        else if (top + height > box.b.y) offset += top + height - box.b.y;
    }
    void end() {
        ui.draw->PopClipRect();
        const float view_height = box.b.y - box.a.y, content = y + offset - box.a.y, most = std::max(0.f, content - view_height);
        if (ui.input && inside(ui.mouse, box.a, box.b) && ui.wheel != 0) offset -= ui.wheel * 170 * ui.u;
        offset = std::clamp(offset, 0.f, most);
        if (most > 0) {
            const float thumb = std::max(60 * ui.u, view_height * view_height / content);
            const float at = box.a.y + (view_height - thumb) * (offset / most);
            ui.draw->AddRectFilled(ImVec2(box.b.x - 10 * ui.u, at), ImVec2(box.b.x - 2 * ui.u, at + thumb), theme::muted, 4 * ui.u);
        }
    }
};

// ---------------------------------------------------------------- the server browser

// Favourite servers. A dedicated server signed in with a login token keeps its Steam ID, so that
// is what is kept: "ServerFavorites" in the profile, the IDs with commas between. A player's
// lobby has a new ID each time and cannot be one.
constexpr std::size_t most_favorites = 256;
void load_favorites(Hub &h) {
    if (h.favorites_loaded) return;
    h.favorites_loaded = true;
    const auto saved = dingosdk::profile_runtime::local_value("ServerFavorites");
    if (!saved || !saved->is_string()) return;
    std::uint64_t id{};
    for (const char c : saved->string() + ",") {
        if (c >= '0' && c <= '9') id = id * 10 + static_cast<std::uint64_t>(c - '0');
        else {
            if (id && h.favorites.size() < most_favorites) h.favorites.insert(id);
            id = 0;
        }
    }
}
void toggle_favorite(Hub &h, const MultiplayerLobby &lobby) {
    if (!lobby.dedicated) return;
    if (!h.favorites.erase(lobby.id)) {
        if (h.favorites.size() >= most_favorites) return say(h, "That is as many favorites as there is room for.");
        h.favorites.insert(lobby.id);
    }
    std::string text;
    for (const auto id : h.favorites) text += (text.empty() ? "" : ",") + std::to_string(id);
    dingosdk::profile_runtime::set_local_values({{"ServerFavorites", dingosdk::Json(text)}});
    h.rows_stale = true;
}
// A five-pointed star, for a favorite's row.
void star(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour) {
    ImVec2 points[10];
    for (int i = 0; i < 10; ++i) {
        const float angle = -1.5707963f + static_cast<float>(i) * .62831853f, r = i % 2 ? radius * .42f : radius;
        points[i] = ImVec2(centre.x + std::cos(angle) * r, centre.y + std::sin(angle) * r);
    }
    for (int i = 0; i < 10; i += 2) draw->AddTriangleFilled(points[i], points[(i + 1) % 10], points[(i + 9) % 10], colour);
    const ImVec2 inner[5]{points[1], points[3], points[5], points[7], points[9]};
    draw->AddConvexPolyFilled(inner, 5, colour);
}

// Letters and digits only, in lower case: a server's map name and a Thunderstore package's title
// differ in spaces, underscores and case ("116th street", "116th_Street").
std::string squashed(std::string_view text) {
    std::string result;
    for (const char c : text)
        if (std::isalnum(static_cast<unsigned char>(c))) result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// The maps of Mod Options' MAPS grid, in its order: the game's, then the custom ones, each by name
// (not Tutorial Island: the game's first-time tutorial). The server browser asks for the pictures
// of the same maps, so the two never ask for different sets (want_menu_map_pictures keeps one).
std::vector<const Level *> grid_levels(std::span<const Level> levels) {
    std::vector<const Level *> listed;
    for (const bool custom : {false, true}) {
        const auto first = listed.size();
        for (const auto &entry : levels)
            if (native_tools::listed(entry) && entry.custom == custom && world_level_name(entry.asset) != "Tutorial Island") listed.push_back(&entry);
        std::sort(listed.begin() + static_cast<std::ptrdiff_t>(first), listed.end(),
                  [](const auto *x, const auto *y) { return native_tools::level_name(*x) < native_tools::level_name(*y); });
    }
    return listed;
}
void want_grid_pictures(const std::vector<const Level *> &listed) {
    std::vector<MenuMapWanted> wanted;
    for (const auto *entry : listed) wanted.push_back({entry->asset, native_tools::level_name(*entry)});
    want_menu_map_pictures(std::move(wanted));
}

void rebuild(Hub &h) {
    load_favorites(h);
    h.rows_stale = false;
    h.rows.clear();
    const auto &mp = h.model;
    const std::span<const Level> levels = h.full.levels;
    h.grid = grid_levels(levels);
    h.pictures_asked = false;
    const auto found = view::browse(mp, {h.search, h.same_map, h.sort, h.sort_reverse}, levels);
    h.listed = mp.lobbies.size();
    h.skaters = 0;
    for (const auto &lobby : mp.lobbies) h.skaters += std::max(0, lobby.players);
    // Each row's map picture. A player's lobby names its map by its level's asset, a dedicated
    // server by the map's name: an installed level is found by either. A map not installed here
    // has its Thunderstore package's icon, when the listing has a map package of that name (the
    // listing is only asked for then; reading it and the icons happen off this thread).
    std::map<std::string, const Level *> by_asset, by_name;
    for (const auto &entry : levels) {
        by_asset.emplace(view::fold(entry.asset), &entry);
        if (native_tools::listed(entry)) by_name.emplace(squashed(native_tools::level_name(entry)), &entry);
    }
    std::shared_ptr<const dingosdk::mod_store::Listing> listing;
    const auto picture = [&](Row &row) {
        const auto &map = row.lobby->map;
        if (const auto at = by_asset.find(view::fold(std::string(world_destination_asset(map)))); at != by_asset.end()) return void(row.asset = at->second->asset);
        const auto name = squashed(view::map_name(map, levels));
        if (const auto at = by_name.find(name); at != by_name.end()) return void(row.asset = at->second->asset);
        if (!listing) listing = dingosdk::mod_store::listing();
        if (!listing) return;
        if (listing->serial != h.listing_serial) {
            h.listing_serial = listing->serial;
            h.package_icons.clear();
            for (const auto &package : listing->packages)
                if (package.in_category("Maps") && !dingosdk::mod_store::nsfw(package) && !package.versions.empty() && !package.latest().icon.empty())
                    h.package_icons.emplace(squashed(package.title()), package.latest().icon);
        }
        if (const auto icon = h.package_icons.find(name); icon != h.package_icons.end()) row.icon = icon->second;
    };
    for (const auto *lobby : found.lobbies) {
        const bool full = lobby->players >= lobby->capacity;
        if ((h.kind == Kind::servers && !lobby->dedicated) || (h.kind == Kind::lobbies && lobby->dedicated) ||
            (h.friends_only && lobby->friends.empty()) || (h.hide_full && full))
            continue;
        const bool favorite = lobby->dedicated && h.favorites.contains(lobby->id);
        if (h.favorites_only && !favorite) continue;
        h.rows.push_back({lobby, view::caption(view::map_name(lobby->map, levels), 40), view::can_join(mp, *lobby),
                          mp.active && lobby->id == mp.public_lobby, full, lobby->owner == mp.local_id, favorite});
        picture(h.rows.back());
    }
    // Favorites come first, in the order the rest are in.
    std::stable_partition(h.rows.begin(), h.rows.end(), [](const Row &row) { return row.favorite; });
    if (std::none_of(h.rows.begin(), h.rows.end(), [&](const Row &row) { return row.lobby->id == h.selected; }))
        h.selected = h.rows.empty() ? 0 : h.rows.front().lobby->id;
    if (std::none_of(h.rows.begin(), h.rows.end(), [&](const Row &row) { return row.lobby->id == h.open; })) h.open = 0;
    // The JOIN YOUR FRIENDS tile, from every session whatever the filters hide: the session most
    // friends are on, one the player can join before one they cannot.
    h.crew = nullptr;
    for (const auto &lobby : mp.lobbies) {
        if (lobby.friends.empty()) continue;
        const bool can = view::can_join(mp, lobby), could = h.crew && view::can_join(mp, *h.crew);
        if (!h.crew || (can && !could) || (can == could && lobby.friends.size() > h.crew->friends.size())) h.crew = &lobby;
    }
    // The FRIENDS tab: those the player can join first, then those in a session, then by name.
    h.friends.clear();
    for (const auto &who : mp.friends_skating) {
        FriendRow row{&who};
        if (who.session)
            for (const auto &lobby : mp.lobbies)
                if (lobby.id == who.session) row.where = &lobby;
        row.here = mp.active && std::any_of(mp.roster.begin(), mp.roster.end(), [&](const auto &player) { return player.id == who.id && player.connected; });
        row.joinable = row.where && !row.here && view::can_join(mp, *row.where);
        // Their card's map picture: their session's, or the player's own when that is where they are
        // and it is not in the list (a private one).
        if (row.where || row.here) {
            MultiplayerLobby own;
            own.map = mp.map;
            Row probe{row.where ? row.where : &own};
            picture(probe);
            row.place.map = view::caption(view::map_name(probe.lobby->map, levels), 40);
            row.place.asset = std::move(probe.asset), row.place.icon = std::move(probe.icon);
        }
        h.friends.push_back(std::move(row));
    }
    std::stable_sort(h.friends.begin(), h.friends.end(), [](const FriendRow &x, const FriendRow &y) {
        if (x.joinable != y.joinable) return x.joinable;
        const bool x_in = x.where || x.here, y_in = y.where || y.here;
        if (x_in != y_in) return x_in;
        return view::fold(x.who->name) < view::fold(y.who->name);
    });
}
const Row *selected_row(const Hub &h) {
    for (const auto &row : h.rows)
        if (row.lobby->id == h.selected) return &row;
    return nullptr;
}
void join(Hub &h, const Row &row) {
    if (!row.joinable) return;
    if (row.lobby->password_required && h.password.empty()) {
        // (Its password box is in the actions under the row.)
        h.open = h.selected = row.lobby->id;
        h.focus_next = hash("password");
        return say(h, "Enter the password, then join.");
    }
    if (send(h, "join-lobby", std::to_string(row.lobby->id), row.lobby->password_required ? h.password : std::string())) {
        h.joining_name = view::caption(row.lobby->name, 40);
        asked_join(h);
        say(h, "Joining " + h.joining_name + "...");
        wipe(h.password);
    }
}
// Another of the Multiplayer page's tabs, from a control on this one.
void to_tab(Hub &h, Tab tab) {
    h.tab = tab;
    h.focus = 0;
    h.sound = sound_tab;
    set_typing(h, 0);
}
// A join asked for from somewhere other than the session's own row: a tile over the list, a friend.
// One that needs a password opens its row in the list instead (the filters cleared if they hide
// it), with the focus in its password box.
void join_lobby(Hub &h, const MultiplayerLobby &lobby) {
    if (!view::can_join(h.model, lobby)) return;
    if (lobby.password_required) {
        if (std::none_of(h.rows.begin(), h.rows.end(), [&](const Row &row) { return row.lobby->id == lobby.id; })) {
            h.kind = Kind::all;
            h.friends_only = h.hide_full = h.same_map = h.favorites_only = false;
            h.search.clear();
            h.rows_stale = true;
        }
        if (h.selected != lobby.id) wipe(h.password);
        h.selected = h.open = lobby.id;
        h.code_mode = false;
        if (h.tab != Tab::browser) to_tab(h, Tab::browser);
        h.focus_next = hash("password");
        return say(h, "Enter the password, then join.");
    }
    if (send(h, "join-lobby", std::to_string(lobby.id))) {
        h.joining_name = view::caption(lobby.name, 40);
        asked_join(h);
        say(h, "Joining " + h.joining_name + "...");
    }
}
// The lists are searched when the page first shows, as soon as the game-wide limit allows.
void first_browse(Hub &h, int wait) {
    if (!h.searched && !wait) h.searched = true, queue_multiplayer_action("browse", "");
}

// The Insert menu lights a control two ways: `focused`, under the D-pad or the keys (white, dark
// ink), and `pointed`, under the pointer (a lighter grey, or a lighter blue on a blue one). The
// pointer moves the focus too (item()), so a control it is over counts as pointed only.
struct Lit {
    bool focused{}, pointed{};
};
Lit lit_of(const Hit &hit) { return {hit.focus && !hit.hover, hit.hover}; }

// A chip of a bar of filters, as skate.'s tabs: a chosen one light grey with black text and a blue
// line along its foot, the focus blue, a lighter grey under the pointer.
// Drawn from `x`, between `top` and `bottom`, and `x` moves on past it. Returns whether it was pressed.
bool chip(Ui &ui, std::uint32_t id, float &x, float top, float bottom, const char *text, bool on) {
    const float u = ui.u, size = 32 * u, w = width_of(ui.bold, size, text) + 40 * u;
    const ImVec2 c(x, top), d(x + w, bottom);
    const Hit hit = item(ui, id, c, d);
    const auto [focused, pointed] = lit_of(hit);
    theme::tile_rect(ui.draw, c, d, focused ? theme::focus_fill : on ? (pointed ? primary_hover : primary_rest) : pointed ? row_hover : tile_dark,
                     hash(text) & 0xfff, u * 2);
    if (on) ui.draw->AddRectFilled(ImVec2(c.x + 10 * u, d.y - 7 * u), ImVec2(d.x - 10 * u, d.y), focused ? skate_theme::black : accent);
    ui.draw->AddText(ui.bold, size, ImVec2(c.x + 20 * u, c.y + (d.y - c.y - size) * .5f), focused || on ? focus_ink : pointed ? theme::paper : chip_ink, text);
    x = d.x + 10 * u;
    return hit.press;
}
// What a group of chips is, in small grey capitals before them; `x` moves on past it.
void chip_label(Ui &ui, float &x, float top, float bottom, const char *text) {
    const float u = ui.u, size = 24 * u;
    ui.draw->AddText(ui.bold, size, ImVec2(x, top + (bottom - top - size) * .5f), sub_ink, text);
    x += width_of(ui.bold, size, text) + 18 * u;
}

// A group's heading over its rows, in the Multiplayer page's small white capitals.
void group_heading(Ui &ui, ImVec2 at, const std::string &text) { ui.draw->AddText(ui.bold, 28 * ui.u, at, theme::paper, text.c_str()); }

// A tag on a row in one of the kit's tag colours (ON blue, OFF grey, problems amber or red), with
// its middle at `middle_y` and its right end at `right`. Returns its width.
float row_tag(Ui &ui, float right, float middle_y, const char *text, ImU32 fill, ImU32 ink) {
    const float u = ui.u, size = 26 * u, w = width_of(ui.bold, size, text) + 28 * u, tall = 44 * u;
    ui.draw->AddRectFilled(ImVec2(right - w, middle_y - tall * .5f), ImVec2(right, middle_y + tall * .5f), fill, 3 * u);
    ui.draw->AddText(ui.bold, size, ImVec2(right - w + 14 * u, middle_y - size * .5f), ink, text);
    return w;
}

// One of the three big tiles over the server list. `primary`: the main thing to do on the page now,
// on the blue. `open`: what it opens is showing, marked with the kit's blue edge.
bool hero(Ui &ui, std::uint32_t id, ImVec2 a, ImVec2 b, const char *title, const std::string &line, bool primary, bool enabled = true,
          bool open = false) {
    const float u = ui.u;
    const Hit hit = enabled ? item(ui, id, a, b) : Hit{};
    const auto [focused, pointed] = lit_of(hit);
    const ImU32 fill = focused ? theme::focus_fill : primary ? (pointed ? primary_hover : primary_rest) : pointed ? row_hover : tile_dark;
    const ImU32 ink = focused || primary ? focus_ink : enabled ? theme::paper : theme::muted;
    const ImU32 sub = focused ? sub_ink_lit : primary ? IM_COL32(70, 70, 74, 255) : sub_ink;
    theme::tile_rect(ui.draw, a, b, fill, id & 0xfff, u * 2);
    if (open) ui.draw->AddRectFilled(a, ImVec2(a.x + 10 * u, b.y), focused ? skate_theme::black : accent);
    const float inset = 44 * u, title_size = 64 * u, line_size = 32 * u, between = 16 * u;
    const float top = a.y + (b.y - a.y - title_size - between - line_size) * .5f;
    label(ui.draw, ui.heading, title_size, ImVec2(a.x + inset, top), b.x - inset, ink, title);
    label(ui.draw, ui.body, line_size, ImVec2(a.x + inset, top + title_size + between), b.x - inset, sub, line);
    return hit.press;
}

// A button in the Multiplayer page's look: `primary` on the blue; else a dark tile (`dark`) or, on
// a white row, a light grey one. Under the D-pad or the keys it goes white on the dark and black on
// the white (a primary one too: a blue focus would not show on blue).
bool action(Ui &ui, std::uint32_t id, ImVec2 a, ImVec2 b, const std::string &text, bool primary, bool dark, bool enabled = true,
            const Item *clip = nullptr) {
    const float u = ui.u, size = std::min(36 * u, (b.y - a.y) * .42f);
    const Hit hit = enabled ? item(ui, id, a, b, clip) : Hit{};
    const auto [focused, pointed] = lit_of(hit);
    ImU32 fill = tile_dark, ink = theme::paper;
    if (primary) {
        if (!enabled) fill = primary_off, ink = IM_COL32(245, 245, 245, 140);
        else fill = focused ? theme::focus_fill : pointed ? primary_hover : primary_rest, ink = focus_ink;
    } else if (dark) {
        fill = focused ? theme::focus_fill : pointed ? row_hover : tile_dark, ink = focused ? focus_ink : enabled ? theme::paper : theme::muted;
    } else {
        fill = focused ? theme::focus_fill : pointed ? card_light_hover : card_light, ink = enabled || focused ? skate_theme::black : sub_ink;
    }
    theme::tile_rect(ui.draw, a, b, fill, id & 0xfff, u * 2);
    const float w = width_of(ui.bold, size, text);
    label(ui.draw, ui.bold, size, ImVec2(a.x + std::max(16 * u, (b.x - a.x - w) * .5f), a.y + (b.y - a.y - size) * .5f), b.x - 16 * u, ink, text);
    return hit.press;
}

// A server's type, or a mark (YOU ARE HERE), on a small plate in one of the kit's tag colours.
// Returns its width.
float plate(Ui &ui, ImVec2 at, const char *text, ImU32 colour, ImU32 ink) {
    const float u = ui.u, size = 24 * u, w = width_of(ui.bold, size, text) + 20 * u;
    ui.draw->AddRectFilled(at, ImVec2(at.x + w, at.y + 36 * u), colour, 3 * u);
    ui.draw->AddText(ui.bold, size, ImVec2(at.x + 10 * u, at.y + 6 * u), ink, text);
    return w;
}

// A row's map picture, the middle of the square cut to the box's shape. With none (or while it is
// still on its way: nothing here waits for one), a plain tile with the map's name on it.
void map_picture(Ui &ui, ImVec2 a, ImVec2 b, const Row &row) {
    auto *draw = ui.draw;
    ImTextureID texture{};
    ImVec2 uv0(0, 0), uv1(1, 1);
    bool shown = !row.asset.empty() && menu_map_picture(row.asset, texture, uv0, uv1);
    if (!shown && !row.icon.empty()) {
        texture = store_icon(row.icon);
        uv0 = ImVec2(0, 0), uv1 = ImVec2(1, 1);
        shown = texture != ImTextureID{};
    }
    if (shown) {
        const float band = (uv1.y - uv0.y) * (1.f - (b.y - a.y) / (b.x - a.x)) * .5f;
        draw->AddImage(texture, a, b, ImVec2(uv0.x, uv0.y + band), ImVec2(uv1.x, uv1.y - band));
        return;
    }
    draw->AddRectFilled(a, b, (hash(row.map) & 1) ? IM_COL32(63, 84, 104, 255) : IM_COL32(74, 97, 119, 255));
    const float u = ui.u, size = 28 * u, inset = 18 * u, wrap = b.x - a.x - inset * 2;
    const float tall = ui.bold->CalcTextSizeA(size, FLT_MAX, wrap, row.map.c_str()).y;
    draw->PushClipRect(a, b, true);
    draw->AddText(ui.bold, size, ImVec2(a.x + inset, a.y + std::max(inset * .5f, (b.y - a.y - tall) * .5f)), theme::paper, row.map.c_str(), nullptr, wrap);
    draw->PopClipRect();
}

const char *sort_name(view::Sort sort, bool reverse) {
    switch (sort) {
    case view::Sort::name: return reverse ? "Name, Z to A" : "Name, A to Z";
    case view::Sort::map: return reverse ? "Map, Z to A" : "Map, A to Z";
    default: return reverse ? "Fewest skaters" : "Most skaters";
    }
}

// Joining with a code, in the list's place: a host's session code (and its password, if it has one).
void code_card(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    const float u = ui.u, bar = 96 * u, inset = 44 * u, tall = 100 * u;
    auto *draw = ui.draw;
    theme::tile_rect(draw, a, z, tile_dark, 0x77, u * 2);
    const float left = a.x + inset, middle = a.x + (z.x - a.x) * .42f, edge = z.x - inset;
    float y = a.y + inset;
    draw->AddText(ui.heading, 64 * u, ImVec2(left, y), theme::paper, "JOIN BY CODE");
    y += 100 * u;
    label(draw, ui.body, 32 * u, ImVec2(left, y), middle - inset, sub_ink, "Ask the host for their session code.");
    y += 50 * u;
    label(draw, ui.body, 32 * u, ImVec2(left, y), middle - inset, sub_ink, "You will travel to their map.");
    y += 80 * u;
    label(draw, ui.body, 30 * u, ImVec2(left, y), middle - inset, theme::muted, view::caption(!h.notice.empty() ? h.notice : mp.status, 96));
    y = a.y + inset;
    draw->AddText(ui.bold, 28 * u, ImVec2(middle, y), theme::muted, "JOIN CODE");
    y += 40 * u;
    bool go = field(ui, hash("code"), ImVec2(middle, y), ImVec2(edge, y + bar), h.code, "Paste a ReSkate join code", false, 120);
    y += bar + 28 * u;
    draw->AddText(ui.bold, 28 * u, ImVec2(middle, y), theme::muted, "PASSWORD");
    y += 40 * u;
    go |= field(ui, hash("code-password"), ImVec2(middle, y), ImVec2(edge, y + bar), h.code_password, "Only if it has one", true);
    y += bar + 36 * u;
    const bool idle = !mp.active && !mp.lobby_joining;
    const float split = middle + (edge - middle) * .55f;
    go |= action(ui, hash("join-code"), ImVec2(middle, y), ImVec2(split - 8 * u, y + tall), mp.lobby_joining ? "JOINING..." : idle ? "JOIN" : "LEAVE YOUR SESSION FIRST",
                 true, true, idle && !h.code.empty());
    if (go && idle && !h.code.empty() && send(h, "join", h.code, h.code_password)) h.joining_name.clear(), asked_join(h), say(h, "Joining..."), wipe(h.code_password);
    if (action(ui, hash("code-back"), ImVec2(split + 8 * u, y), ImVec2(edge, y + tall), "BACK TO THE LIST", false, true)) h.code_mode = false, h.focus_next = hash("hero-code");
}

// The keycaps under a list: what the pad_x's (or the keyboard's) buttons do here. Returns where they end.
struct Hint {
    std::uint32_t pad_button; // 0: none on the pad
    const char *key, *what;
};
float hints(Ui &ui, float x, float y, std::initializer_list<Hint> list) {
    for (const auto &hint : list) {
        const auto name = ui.pad.available ? (hint.pad_button ? controller_combo_label(hint.pad_button, ui.pad.style) : std::string()) : std::string(hint.key);
        if (!name.empty()) x += theme::prompt(ui.draw, ui.bold, ui.u * 2, ImVec2(x, y), name.c_str(), hint.what) + 16 * ui.u;
    }
    return x;
}

// The server browser: three big tiles (join the friends, join by code, host), the search and
// filters, then every session as a row with its map's picture and a column each for its map, the
// friends on it, its ping and its skaters. A on a row opens what it offers
// under it (JOIN, FAVORITE, DETAILS, its password); a double click joins.
void browser_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    const float u = ui.u, gap = 24 * u, bar = 96 * u, tiles = 190 * u, footer = 70 * u;
    auto *draw = ui.draw;
    // Refreshing waits out the game-wide limit (multiplayer_browse_interval_us); the button counts it down.
    const int wait = browse_wait(mp);
    first_browse(h, wait);
    // (Once a model read, and not while the overlay's own menu, with a map grid of its own, is over the page.)
    if (ui.input && !std::exchange(h.pictures_asked, true)) want_grid_pictures(h.grid);
    bool changed{};
    // ---- the tiles
    {
        const float w = (z.x - a.x - gap * 2) / 3;
        const auto at = [&](int i) { return a.x + (w + gap) * static_cast<float>(i); };
        std::string line = mp.friends_skating.empty() ? std::string("No friends skating")
                           : std::to_string(mp.friends_skating.size()) + (mp.friends_skating.size() == 1 ? " friend skating, not on a public server" : " friends skating, none on a public server");
        bool joinable{};
        if (const auto *crew = h.crew) {
            const auto who = view::caption(crew->friends.front(), 24) + (crew->friends.size() > 1 ? " and " + std::to_string(crew->friends.size() - 1) + " more" : "");
            joinable = view::can_join(mp, *crew);
            line = mp.active && crew->id == mp.public_lobby ? who + (crew->friends.size() > 1 ? " are" : " is") + " here with you"
                   : who + " on " + view::caption(crew->name, 40) + (crew->players >= crew->capacity ? " (full)" : "");
        }
        // With nobody to join, it goes to the FRIENDS tab instead.
        if (hero(ui, hash("hero-friends"), ImVec2(at(0), a.y), ImVec2(at(0) + w, a.y + tiles), "JOIN YOUR FRIENDS", line, joinable)) {
            if (joinable) join_lobby(h, *h.crew);
            else to_tab(h, Tab::friends);
        }
        // Joining with a code puts its card in the list's place; pressing the tile again (or View /
        // Select, or the card's own BACK TO THE LIST) brings the list back.
        if (hero(ui, hash("hero-code"), ImVec2(at(1), a.y), ImVec2(at(1) + w, a.y + tiles), "JOIN BY CODE",
                 h.code_mode ? "Back to the server list" : "Paste a friend's join code", false, true, h.code_mode)) {
            h.code_mode = !h.code_mode;
            if (h.code_mode) h.focus_next = hash("code");
        }
        const auto host = !mp.active ? "Your map, your rules, up to " + std::to_string(multiplayer_lobby_player_limit) + " skaters"
                          : std::string(mp.hosting ? "You are hosting a session" : "Leave your session to host your own");
        if (hero(ui, hash("hero-host"), ImVec2(at(2), a.y), ImVec2(at(2) + w, a.y + tiles), "HOST A LOBBY", host, false)) to_tab(h, Tab::host);
    }
    // ---- the bar: search, filters, the order, refresh
    const float bar_top = a.y + tiles + gap;
    {
        const float search_width = std::min(760 * u, (z.x - a.x) * .22f);
        const auto before = h.search;
        field(ui, hash("search"), ImVec2(a.x, bar_top), ImVec2(a.x + search_width, bar_top + bar), h.search, "Search servers, maps, friends");
        changed |= before != h.search;
        float x = a.x + search_width + 20 * u;
        const float c_top = bar_top + 12 * u, c_bottom = bar_top + bar - 12 * u;
        const auto filter = [&](const char *text, bool on) { return chip(ui, hash(text, 7), x, c_top, c_bottom, text, on); };
        if (filter("ALL", h.kind == Kind::all)) h.kind = Kind::all, changed = true;
        if (filter("SERVERS", h.kind == Kind::servers)) h.kind = Kind::servers, changed = true;
        if (filter("LOBBIES", h.kind == Kind::lobbies)) h.kind = Kind::lobbies, changed = true;
        x += 14 * u;
        if (filter("FRIENDS", h.friends_only)) h.friends_only = !h.friends_only, changed = true;
        if (filter("FAVORITES", h.favorites_only)) h.favorites_only = !h.favorites_only, changed = true;
        if (filter("HIDE FULL", h.hide_full)) h.hide_full = !h.hide_full, changed = true;
        if (filter("MY MAP", h.same_map)) h.same_map = !h.same_map, changed = true;
        const auto refresh = wait ? "REFRESH IN " + std::to_string(wait) : std::string(mp.lobby_searching ? "SEARCHING..." : "REFRESH");
        const float refresh_x = z.x - 380 * u;
        if (action(ui, hash("refresh"), ImVec2(refresh_x, bar_top + 12 * u), ImVec2(z.x, bar_top + bar - 12 * u), refresh, false, true, !wait))
            queue_multiplayer_action("browse", "");
        // The order the rows are in. Y goes to the next one; pressing this goes through each of them
        // and then the same the other way round.
        const std::string value = sort_name(h.sort, h.sort_reverse);
        const auto key = ui.pad.available ? "  (" + controller_combo_label(0x8000, ui.pad.style) + ")" : std::string();
        const float size = 32 * u, lead = width_of(ui.body, size, "Sort: "), w = lead + width_of(ui.bold, size, value) + width_of(ui.body, size, key) + 40 * u;
        const ImVec2 c(refresh_x - 16 * u - w, bar_top + 12 * u), d(refresh_x - 16 * u, bar_top + bar - 12 * u);
        if (c.x > x) {
            const Hit hit = item(ui, hash("sort"), c, d);
            const auto [lit, pointed] = lit_of(hit);
            if (lit || pointed) theme::tile_rect(draw, c, d, lit ? theme::focus_fill : row_hover, 0x5a, u * 2);
            const float y = c.y + (d.y - c.y - size) * .5f;
            draw->AddText(ui.body, size, ImVec2(c.x + 20 * u, y), lit ? sub_ink_lit : sub_ink, "Sort: ");
            draw->AddText(ui.bold, size, ImVec2(c.x + 20 * u + lead, y), lit ? focus_ink : theme::paper, value.c_str());
            draw->AddText(ui.body, size, ImVec2(c.x + 20 * u + lead + width_of(ui.bold, size, value), y), lit ? sub_ink_lit : sub_ink, key.c_str());
            if (hit.press) {
                if (!h.sort_reverse) h.sort_reverse = true;
                else h.sort = static_cast<view::Sort>((static_cast<int>(h.sort) + 1) % 3), h.sort_reverse = false;
                changed = true;
            }
        }
    }
    const bool fresh_input = ui.input && !h.typing;
    const std::uint32_t pressed = ui.pad.buttons & ~h.pad_before;
    if (fresh_input && (pressed & 0x4000) && !wait) queue_multiplayer_action("browse", ""); // X / Square
    if (fresh_input && (pressed & 0x8000))
        h.sort = static_cast<view::Sort>((static_cast<int>(h.sort) + 1) % 3), h.sort_reverse = false, changed = true; // Y / Triangle
    // View / Select: joining with a code, and back.
    if (fresh_input && (pressed & 0x20)) h.code_mode = !h.code_mode, h.focus_next = h.code_mode ? hash("code") : hash("hero-code");
    // F marks the chosen server as a favorite, or takes the mark away.
    if (fresh_input && !h.code_mode && ImGui::IsKeyPressed(ImGuiKey_F, false))
        if (const auto *row = selected_row(h); row && row->lobby->dedicated) toggle_favorite(h, *row->lobby), changed = true;
    if (changed) rebuild(h), h.scrolls[hash("servers")] = 0;

    // ---- the list, or joining with a code in its place
    const ImVec2 la(a.x, bar_top + bar + gap), lz(z.x, z.y - footer);
    if (h.code_mode) {
        code_card(ui, la, ImVec2(lz.x, std::min(lz.y, la.y + 560 * u)));
    } else {
        const float row_height = 120 * u, row_gap = 10 * u, picture_width = 213 * u, strip = 116 * u, pad_x = 30 * u, head = 46 * u;
        const float right = lz.x - 20 * u;
        // The columns, from the right: the skaters with a bar of how full it is, the ping, the
        // friends on it and its map; the name (with its type plate under it) has what is left. A
        // narrow page drops the friends column, then the map's, and the map goes beside the plates.
        const float column_gap = 40 * u, least_name = 700 * u;
        const float skaters_left = right - pad_x - 280 * u, ping_right = skaters_left - column_gap, ping_left = ping_right - 150 * u;
        const float name_left = la.x + picture_width + pad_x, spare = ping_left - column_gap - name_left;
        const float friends_width = std::clamp(spare * .24f, 340 * u, 620 * u), map_width = std::clamp(spare * .22f, 300 * u, 560 * u);
        const bool with_friends = spare - friends_width - map_width - column_gap * 2 >= least_name;
        const bool with_map = with_friends || spare - map_width - column_gap >= least_name;
        const float friends_left = ping_left - column_gap - friends_width, friends_right = ping_left - column_gap;
        const float map_right = with_friends ? friends_left - column_gap : ping_left - column_gap, map_left = map_right - map_width;
        const float name_right = with_map ? map_left - column_gap : ping_left - column_gap;
        {
            const float size = 24 * u, y = la.y + (head - size) * .5f - 4 * u;
            draw->AddText(ui.bold, size, ImVec2(name_left, y), sub_ink, "SERVER");
            if (with_map) draw->AddText(ui.bold, size, ImVec2(map_left, y), sub_ink, "MAP");
            if (with_friends) draw->AddText(ui.bold, size, ImVec2(friends_left, y), sub_ink, "FRIENDS");
            draw->AddText(ui.bold, size, ImVec2(ping_right - width_of(ui.bold, size, "PING"), y), sub_ink, "PING");
            draw->AddText(ui.bold, size, ImVec2(right - pad_x - width_of(ui.bold, size, "SKATERS"), y), sub_ink, "SKATERS");
        }
        Scroller list(ui, hash("servers"), ImVec2(la.x, la.y + head), lz);
        for (const auto &row : h.rows) {
            const auto &lobby = *row.lobby;
            const auto id = hash("server", lobby.id);
            const bool open = h.open == lobby.id;
            const float block = row_height + (open ? strip : 0);
            bool drawn{};
            const float top = list.next(block, row_gap, drawn);
            const ImVec2 p(la.x, top), q(right, top + row_height);
            const Hit hit = item(ui, id, p, q, &list.box);
            list.reveal(id, top, row_height);
            // The focus on another row closes the one that was open: one row is white at a time.
            if (hit.focus && (h.ring || ui.moved || ui.clicked) && h.selected != lobby.id) {
                wipe(h.password);
                h.selected = lobby.id;
                h.open = 0;
            }
            if (hit.press) {
                if (ui.doubled) join(h, row);
                else if (open) h.open = 0;
                else {
                    h.open = h.selected = lobby.id;
                    // From the pad_x or the keys, on to the first thing it offers.
                    if (h.ring && (row.joinable || lobby.dedicated)) h.focus_next = row.joinable ? hash("row-join") : hash("row-favorite");
                }
            }
            // White under the D-pad or the keys and while open, a lighter grey under the pointer.
            const auto [focused, pointed] = lit_of(hit);
            const bool lit = focused || open;
            const ImU32 ink = lit ? focus_ink : row.full ? theme::muted : theme::paper, sub = lit ? sub_ink_lit : sub_ink;
            if (drawn) {
                theme::tile_rect(draw, p, ImVec2(right, top + block), lit ? theme::focus_fill : pointed ? row_hover : tile_dark,
                                        0x900 + static_cast<unsigned>(lobby.id & 0xff), u * 2);
                map_picture(ui, p, ImVec2(p.x + picture_width, q.y), row);
                // The skaters, with a thin blue bar of how full it is.
                const float count_size = 40 * u;
                const auto count = row.full ? std::string("FULL") : std::to_string(lobby.players) + " / " + std::to_string(lobby.capacity);
                draw->AddText(ui.bold, count_size, ImVec2(right - pad_x - width_of(ui.bold, count_size, count), top + 20 * u), row.full ? skate_theme::danger : ink, count.c_str());
                const float fill = lobby.capacity > 0 ? std::clamp(static_cast<float>(lobby.players) / static_cast<float>(lobby.capacity), 0.f, 1.f) : 0.f;
                const ImVec2 t0(skaters_left, top + 80 * u), t1(right - pad_x, top + 90 * u);
                draw->AddRectFilled(t0, t1, lit ? track_lit : track);
                draw->AddRectFilled(t0, ImVec2(t0.x + (t1.x - t0.x) * fill, t1.y), row.full ? skate_theme::danger : on_row(lit, accent));
                const float cell_size = 32 * u, cell_y = top + (row_height - cell_size) * .5f;
                {
                    const auto ping = lobby.ping >= 0 ? std::to_string(lobby.ping) + " ms" : std::string("-");
                    draw->AddText(ui.body, cell_size, ImVec2(ping_right - width_of(ui.body, cell_size, ping), cell_y), lobby.ping >= 0 ? ink : sub, ping.c_str());
                }
                // The friends on it: their names while they fit, else how many.
                if (with_friends) {
                    std::string with = "-";
                    if (!lobby.friends.empty()) {
                        with = lobby_friends_text(lobby);
                        if (width_of(ui.body, cell_size, with) > friends_right - friends_left) with = lobby_friends_text(lobby, 1);
                        if (width_of(ui.body, cell_size, with) > friends_right - friends_left)
                            with = std::to_string(lobby.friends.size()) + (lobby.friends.size() == 1 ? " friend" : " friends");
                    }
                    label(draw, ui.body, cell_size, ImVec2(friends_left, cell_y), friends_right, lobby.friends.empty() ? sub
                          : lit ? IM_COL32(24, 120, 60, 255) : skate_theme::friends_text, with);
                }
                if (with_map) label(draw, ui.body, cell_size, ImVec2(map_left, cell_y), map_right, ink, row.map);
                // The name; under it its type (and a mark when it is the player's), whether it needs a
                // password, and the map when there is no column for it. With no friends column, the
                // friends on it come after.
                float x = name_left;
                if (row.favorite) star(draw, ImVec2(x + 18 * u, top + 40 * u), 20 * u, skate_theme::warning), x += 48 * u;
                label(draw, ui.bold, 40 * u, ImVec2(x, top + 18 * u), name_right, ink, view::caption(lobby.name, 64));
                const char *type = lobby.official ? "OFFICIAL" : lobby.dedicated ? "SERVER" : "LOBBY";
                const char *mark = row.here ? "YOU ARE HERE" : row.own ? "YOURS" : nullptr;
                const float plate_y = top + 68 * u;
                float px = name_left;
                px += plate(ui, ImVec2(px, plate_y), type, lobby.official ? skate_theme::blue : lobby.dedicated ? plate_grey : skate_theme::good,
                            lobby.official ? theme::paper : lobby.dedicated ? chip_ink : skate_theme::black) + 10 * u;
                if (mark) px += plate(ui, ImVec2(px, plate_y), mark, lit ? focus_ink : theme::paper, lit ? theme::paper : focus_ink) + 10 * u;
                if (lobby.password_required) px += plate(ui, ImVec2(px, plate_y), "PASSWORD", skate_theme::warning, skate_theme::black) + 10 * u;
                std::string rest = with_map ? std::string() : row.map;
                if (!with_friends && !lobby.friends.empty()) rest += (rest.empty() ? "with " : "  -  with ") + lobby_friends_text(lobby);
                if (!rest.empty()) label(draw, ui.body, 28 * u, ImVec2(px + 6 * u, plate_y + 4 * u), name_right, sub, rest);
            }
            if (!open) continue;
            // Under the open row, what it offers. Its controls keep their places in the focus order
            // whether they are in view or not, so the D-pad_x can reach them.
            const float y0 = top + row_height, y1 = y0 + strip - 28 * u;
            const float left = name_left;
            float x = left;
            const char *does = mp.lobby_joining ? "JOINING..." : row.here ? "YOU ARE HERE" : row.own ? "YOURS" : row.full ? "FULL"
                               : mp.active && mp.hosting ? "END YOUR SESSION FIRST" : "JOIN";
            const auto join_id = hash("row-join"), favorite_id = hash("row-favorite"), password_id = hash("password");
            list.reveal(join_id, top, block), list.reveal(favorite_id, top, block), list.reveal(password_id, top, block);
            const float join_width = width_of(ui.bold, 36 * u, does) + 120 * u;
            if (action(ui, join_id, ImVec2(x, y0), ImVec2(x + join_width, y1), does, true, false, row.joinable, &list.box)) join(h, row);
            x += join_width + 16 * u;
            if (lobby.dedicated) {
                const char *text = row.favorite ? "REMOVE FAVORITE" : "FAVORITE";
                const float w = width_of(ui.bold, 36 * u, text) + 70 * u;
                if (action(ui, favorite_id, ImVec2(x, y0), ImVec2(x + w, y1), text, false, false, true, &list.box)) toggle_favorite(h, lobby);
                x += w + 16 * u;
            }
            if (lobby.password_required && row.joinable) {
                const float w = std::min(640 * u, right - pad_x - x);
                if (w > 240 * u) {
                    if (field(ui, password_id, ImVec2(x, y0), ImVec2(x + w, y1), h.password, "Password", true, 64, field_on_white, &list.box)) join(h, row);
                    x += w + 16 * u;
                }
            }
            if (!drawn) continue;
            // What joining from here does, beside them.
            const auto status = !h.notice.empty() ? h.notice : mp.active && !mp.hosting && !mp.echo && row.joinable ? std::string("Joining leaves the session you are in.") : std::string();
            if (!status.empty()) {
                const float w = std::min(width_of(ui.body, 28 * u, status), std::max(0.f, right - pad_x - x));
                label(draw, ui.body, 28 * u, ImVec2(right - pad_x - w, y0 + (y1 - y0 - 28 * u) * .5f), right - pad_x, sub_ink_lit, view::caption(status, 96));
            }
        }
        list.end();
        if (h.rows.empty()) {
            const char *line = mp.lobby_searching ? "Finding skaters..." : !mp.lobby_searched ? "Looking for sessions..."
                               : h.listed ? "Nothing matches these filters." : "Nobody is hosting right now. Host a lobby and invite some skaters.";
            draw->AddText(ui.body, 40 * u, ImVec2(la.x + 30 * u, la.y + head + 40 * u), theme::muted, line);
            label(draw, ui.body, 30 * u, ImVec2(la.x + 30 * u, la.y + head + 100 * u), lz.x, theme::muted, view::caption(mp.browser_status, 96));
        }
    }

    // ---- under it: how many and what the buttons do (JOIN BY CODE is the tile over the list)
    {
        const float y = z.y - 52 * u;
        const auto total = std::to_string(h.rows.size()) + (h.rows.size() == h.listed ? "" : " of " + std::to_string(h.listed)) +
                           (h.listed == 1 ? " session, " : " sessions, ") + std::to_string(h.skaters) + " skating";
        const auto text = view::caption(!h.notice.empty() ? h.notice : total, 96);
        const float most = 1100 * u;
        label(draw, ui.body, 30 * u, ImVec2(a.x, y + 4 * u), a.x + most, theme::muted, text);
        const float x = a.x + std::min(width_of(ui.body, 30 * u, text), most) + 60 * u;
        if (ui.pad.available) hints(ui, x, y, {{0x1000, "", "Open / join"}, {0x4000, "", "Refresh"}, {0x8000, "", "Sort"}, {0x20, "", "Join by code"}});
        else hints(ui, x, y, {{0, "Enter", "Open / join"}, {0, "F", "Favorite"}});
    }
}

// ---------------------------------------------------------------- friends

// A friend's Steam avatar, made on one of store_icons.cpp's workers from its key, "steam-avatar:<id>".
bool make_friend_avatar(const std::string &key, CardPixels &pixels) {
    const auto colon = key.find(':');
    if (colon == std::string::npos) return false;
    dingosdk::multiplayer::SteamAvatar avatar;
    if (!dingosdk::multiplayer::steam_friend_avatar(std::strtoull(key.c_str() + colon + 1, nullptr, 10), avatar)) return false;
    pixels.rgba = std::move(avatar.rgba);
    pixels.width = avatar.width, pixels.height = avatar.height;
    return true;
}
// A friend's picture: their Steam avatar once it is on the GPU, and until then (or with no Steam,
// or no avatar) a square in a colour of their own with their initial.
void friend_avatar(Ui &ui, ImVec2 a, float side, const MultiplayerFriend &who) {
    auto *draw = ui.draw;
    const ImVec2 b(a.x + side, a.y + side);
    if (const auto texture = made_icon("steam-avatar:" + std::to_string(who.id), make_friend_avatar); texture != ImTextureID{}) {
        draw->AddImage(texture, a, b);
        return;
    }
    constexpr std::array<ImU32, 6> colours{IM_COL32(240, 122, 30, 255), IM_COL32(154, 124, 255, 255), IM_COL32(255, 210, 92, 255),
                                           IM_COL32(46, 184, 92, 255), IM_COL32(255, 92, 92, 255), IM_COL32(91, 143, 217, 255)};
    draw->AddRectFilled(a, b, colours[hash("avatar", who.id) % colours.size()]);
    const char first = who.name.empty() ? '?' : who.name.front();
    const char initial[2]{std::isalnum(static_cast<unsigned char>(first)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(first))) : '?', 0};
    const float size = side * .42f;
    draw->AddText(ui.bold, size, ImVec2(a.x + (side - width_of(ui.bold, size, initial)) * .5f, a.y + (side - size) * .5f), skate_theme::black, initial);
}
void view_profile(Hub &h, const MultiplayerFriend &who) {
    h.sound = sound_select;
    if (!dingosdk::multiplayer::steam_open_profile(who.id)) say(h, "Profiles open in the Steam overlay, which is off. Turn it on in Steam's settings.");
}

// The player's Steam friends in the game, as cards: four to a row (fewer when the page is narrower),
// those in a session first, then those skating on their own. A session's card has its map's picture
// across the top with how full it is, where they are under their avatar and name, and JOIN (A, or a
// click on the card); every card has VIEW PROFILE, their Steam profile in the Steam overlay (Y, P, or
// a click on it). Over the cards, only how many there are and a small REFRESH; joining by code is View
// / Select (or C) away, on the server browser's card. There is no Steam invite here: a friend finds a
// public lobby on this tab of their own, or through Steam's Join Game.
void friends_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    const float u = ui.u, gap = 24 * u, head = 72 * u, footer = 70 * u;
    auto *draw = ui.draw;
    const int wait = browse_wait(mp);
    first_browse(h, wait);
    // The cards' map pictures, asked for as the server browser asks for its rows'.
    if (ui.input && !std::exchange(h.pictures_asked, true)) want_grid_pictures(h.grid);
    // Joining by code is the server browser's card: View / Select (or C) opens it there.
    const auto join_by_code = [&] {
        to_tab(h, Tab::browser);
        h.code_mode = true;
        h.focus_next = hash("code");
    };
    const bool fresh_input = ui.input && !h.typing;
    const std::uint32_t pressed = ui.pad.buttons & ~h.pad_before;
    if (fresh_input && (pressed & 0x4000) && !wait) queue_multiplayer_action("browse", "");      // X / Square
    if (fresh_input && ((pressed & 0x20) || ImGui::IsKeyPressed(ImGuiKey_C, false))) join_by_code(); // View / Select, or C
    // Y / Triangle (or P) opens the profile of the friend whose card has the focus, found as it is drawn.
    const bool profile_asked = fresh_input && ((pressed & 0x8000) || ImGui::IsKeyPressed(ImGuiKey_P, false));
    // A friend in a public session in the list, or in the player's own. rebuild() puts them first,
    // so the two groups follow one another.
    const auto together = static_cast<std::size_t>(std::count_if(h.friends.begin(), h.friends.end(), [](const FriendRow &row) { return row.where || row.here; }));

    // ---- over the cards: how many, and refresh
    {
        const float size = 30 * u;
        // Refresh stays a control while it counts down, so the D-pad does not lose its place on it.
        const auto refresh = wait ? "REFRESH IN " + std::to_string(wait) : std::string(mp.lobby_searching ? "SEARCHING..." : "REFRESH");
        const float w = std::max(width_of(ui.bold, size, "REFRESH IN 00"), width_of(ui.bold, size, refresh)) + 60 * u;
        if (action(ui, hash("friends-refresh"), ImVec2(z.x - w, a.y), ImVec2(z.x, a.y + head), refresh, false, true) && !wait) queue_multiplayer_action("browse", "");
        const auto count = h.friends.empty() ? std::string("No friends skating right now")
                           : std::to_string(h.friends.size()) + " skating, " + std::to_string(together) + " in a session";
        label(draw, ui.body, size, ImVec2(a.x, a.y + (head - size) * .5f), z.x - w - 40 * u, theme::muted, count);
    }

    const ImVec2 la(a.x, a.y + head + gap), lz(z.x, z.y - footer);
    if (h.friends.empty()) {
        // Nobody to show: a card that says so, with the way to give them somewhere to go.
        const ImVec2 cz(lz.x, std::min(lz.y, la.y + 360 * u));
        theme::tile_rect(draw, la, cz, tile_dark, 0x7c, u * 2);
        const float inset = 44 * u, tall = 100 * u, left = la.x + inset;
        float y = la.y + inset;
        draw->AddText(ui.bold, 48 * u, ImVec2(left, y), theme::paper, "No friends skating right now");
        y += 80 * u;
        label(draw, ui.body, 32 * u, ImVec2(left, y), cz.x - inset, sub_ink,
              "Friends show here while they play, with where they are skating. Host a lobby and they can join you.");
        y += 70 * u;
        const char *text = "HOST A LOBBY FOR FRIENDS";
        const float w = width_of(ui.bold, 36 * u, text) + 120 * u;
        if (action(ui, hash("friends-empty-host"), ImVec2(left, y), ImVec2(left + w, y + tall), text, true, true)) to_tab(h, Tab::host);
    } else {
        // Four cards to a row while each is at least `least` wide (the page is 3456 units wide at 16:9
        // and wider on a wider screen), fewer when it is narrower. A session's card is taller than a
        // card for a friend skating on their own, so each group's rows are as tall as its cards.
        const float right = lz.x - 20 * u; // clear of the scroll bar
        const float least = 760 * u, card_gap = 24 * u;
        const auto columns = static_cast<std::size_t>(std::clamp(static_cast<int>((right - la.x + card_gap) / (least + card_gap)), 1, 4));
        const float card_width = (right - la.x - card_gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const float pad = 28 * u, side = 120 * u, strip = 250 * u, buttons = 76 * u, group_head = 64 * u, facts = 96 * u;
        const float session_height = strip + pad + side + 20 * u + facts + 20 * u + buttons + pad, solo_height = pad + side + 24 * u + buttons + pad;
        Scroller list(ui, hash("friends"), la, lz);
        const auto heading = [&](const char *title, std::size_t count) {
            bool drawn{};
            const float top = list.next(group_head, 0, drawn);
            if (drawn) group_heading(ui, ImVec2(a.x, top + group_head - 42 * u), std::string(title) + "  \xc2\xb7  " + std::to_string(count));
        };
        const auto card = [&](const FriendRow &row, ImVec2 p, ImVec2 q, bool drawn) {
            const auto &who = *row.who;
            const auto *lobby = row.where;
            const bool session = lobby || row.here, full = lobby && lobby->players >= lobby->capacity;
            const auto id = hash("friend", who.id);
            // Along its foot: JOIN (what A or a click anywhere else on the card does, on a session's
            // card only) and VIEW PROFILE, which a click on it or Y does.
            const float by = q.y - pad - buttons, split = p.x + pad + (q.x - p.x - pad * 3) * .55f;
            const ImVec2 join_a(p.x + pad, by), join_b(split, by + buttons);
            const ImVec2 profile_a(session ? split + pad : p.x + pad, by), profile_b(q.x - pad, by + buttons);
            const bool in_view = ui.input && inside(ui.mouse, list.box.a, list.box.b);
            const bool over_profile = in_view && inside(ui.mouse, profile_a, profile_b), over_join = session && in_view && inside(ui.mouse, join_a, join_b);
            const Hit hit = item(ui, id, p, q, &list.box);
            list.reveal(id, p.y, q.y - p.y);
            if (hit.press) {
                if (over_profile && ui.clicked) view_profile(h, who);
                else if (row.joinable) join_lobby(h, *row.where);
                else say(h, row.here ? "You are skating with them already." : full ? "Their session is full."
                            : lobby && mp.active && mp.hosting ? "End your session first, then join them."
                            : lobby && mp.lobby_joining ? "You are joining a session already."
                            : who.session ? "Their session is not in your list yet. Refresh to look again."
                                          : "They are not in a public session. Ask them for their join code, or host a lobby they can join.");
            }
            if (profile_asked && h.focus == id) view_profile(h, who);
            if (!drawn) return;
            // White under the D-pad or the keys, a lighter grey under the pointer, as the browser's rows.
            const auto [focused, pointed] = lit_of(hit);
            const ImU32 ink = focused ? focus_ink : theme::paper, sub = focused ? sub_ink_lit : sub_ink;
            theme::tile_rect(draw, p, q, focused ? theme::focus_fill : pointed ? row_hover : tile_dark, 0x900 + static_cast<unsigned>(who.id & 0xff), u * 2);
            float y = p.y + pad;
            if (session) {
                // The map's picture across the top (a plain tile with its name while there is none),
                // darker along its foot for the skaters, over a bar of how full the session is; the
                // session's type plate and marks in its corner.
                const ImVec2 sa(p.x, p.y), sb(q.x, p.y + strip);
                map_picture(ui, sa, sb, row.place);
                const ImU32 clear = IM_COL32(0, 0, 0, 0), shade = IM_COL32(0, 0, 0, 210);
                draw->AddRectFilledMultiColor(ImVec2(sa.x, sb.y - 130 * u), sb, clear, clear, shade, shade);
                const bool official = lobby && lobby->official, dedicated = lobby ? lobby->dedicated : mp.dedicated;
                float px = sa.x + pad;
                const float plate_y = sa.y + pad;
                px += plate(ui, ImVec2(px, plate_y), official ? "OFFICIAL" : dedicated ? "SERVER" : "LOBBY", official ? skate_theme::blue : dedicated ? plate_grey : skate_theme::good,
                            official ? theme::paper : dedicated ? chip_ink : skate_theme::black) + 10 * u;
                if (row.here) px += plate(ui, ImVec2(px, plate_y), "YOU ARE HERE", focus_ink, theme::paper) + 10 * u;
                if (lobby && lobby->password_required) plate(ui, ImVec2(px, plate_y), "PASSWORD", skate_theme::warning, skate_theme::black);
                const int players = lobby ? lobby->players : mp.players, capacity = lobby ? lobby->capacity : mp.capacity;
                const float count_size = 40 * u, tag_size = 24 * u, foot = sb.y - 24 * u;
                const auto count = full ? std::string("FULL") : std::to_string(players) + " / " + std::to_string(capacity);
                const float count_x = sb.x - pad - width_of(ui.bold, count_size, count);
                draw->AddText(ui.bold, count_size, ImVec2(count_x, foot - count_size), full ? skate_theme::danger : theme::paper, count.c_str());
                draw->AddText(ui.bold, tag_size, ImVec2(count_x - 14 * u - width_of(ui.bold, tag_size, "SKATERS"), foot - tag_size - 6 * u), chip_ink, "SKATERS");
                const float fill = capacity > 0 ? std::clamp(static_cast<float>(players) / static_cast<float>(capacity), 0.f, 1.f) : 0.f;
                draw->AddRectFilled(ImVec2(sa.x, sb.y - 8 * u), sb, track);
                draw->AddRectFilled(ImVec2(sa.x, sb.y - 8 * u), ImVec2(sa.x + (sb.x - sa.x) * fill, sb.y), full ? skate_theme::danger : on_row(focused, accent));
                y = sb.y + pad;
            }
            // Their avatar and name, and what they are doing: skating with the player (and in their
            // party), hosting, or where they are.
            friend_avatar(ui, ImVec2(p.x + pad, y), side, who);
            const float text_left = p.x + pad + side + 24 * u, text_right = q.x - pad;
            label(draw, ui.bold, 42 * u, ImVec2(text_left, y + 14 * u), text_right, ink, view::caption(who.name.empty() ? "A friend" : who.name, 40));
            std::string status;
            bool with{};
            if (row.here) {
                const auto me = std::find_if(mp.roster.begin(), mp.roster.end(), [&](const auto &player) { return player.id == who.id; });
                const bool party = mp.parties && me != mp.roster.end() && me->party_member;
                status = !party ? "Skating with you" : me->party_leader ? "Your party's leader, skating with you" : "In your party, skating with you";
                with = true;
            } else if (lobby && !lobby->dedicated && lobby->owner == who.id) {
                status = "Hosting this lobby";
                with = true;
            } else if (lobby) {
                status = lobby->dedicated ? "Skating on a server" : "Skating in a lobby";
            } else {
                status = who.session ? "In a session not in your list yet" : "Skating solo, or in a private session";
            }
            label(draw, ui.body, 30 * u, ImVec2(text_left, y + 72 * u), text_right, with ? (focused ? IM_COL32(24, 120, 60, 255) : skate_theme::friends_text) : sub, status);
            if (session) {
                // The session's name, then its map, the ping and the other friends there.
                y += side + 20 * u;
                const auto server = lobby ? view::caption(lobby->name, 64) : mp.lobby_name.empty() ? std::string("Your session") : view::caption(mp.lobby_name, 64);
                label(draw, ui.bold, 36 * u, ImVec2(p.x + pad, y), q.x - pad, ink, server);
                std::string line = row.place.map;
                if (lobby && lobby->ping >= 0) line += "  \xc2\xb7  " + std::to_string(lobby->ping) + " ms";
                if (lobby) {
                    std::vector<std::string> others;
                    for (const auto &name : lobby->friends)
                        if (name != who.name) others.push_back(name);
                    if (!others.empty()) {
                        line += "  \xc2\xb7  with " + view::caption(others.front(), 24);
                        if (others.size() == 2) line += " and " + view::caption(others[1], 24);
                        else if (others.size() > 2) line += " and " + std::to_string(others.size() - 1) + " more";
                    }
                }
                label(draw, ui.body, 28 * u, ImVec2(p.x + pad, y + 52 * u), q.x - pad, sub, line);
                // JOIN on the blue, or why not on grey.
                const char *does = mp.lobby_joining ? "JOINING..." : row.joinable ? "JOIN" : row.here ? "YOU ARE HERE" : full ? "FULL"
                                   : mp.active && mp.hosting ? "END YOUR SESSION FIRST" : "JOIN";
                theme::tile_rect(draw, join_a, join_b, row.joinable ? (focused ? skate_theme::black : over_join && !h.ring ? primary_hover : primary_rest) : focused ? track_lit : track, 0x5c, u * 2);
                const float size = 32 * u, w = width_of(ui.bold, size, does);
                label(draw, ui.bold, size, ImVec2(join_a.x + std::max(16 * u, (join_b.x - join_a.x - w) * .5f), join_a.y + (buttons - size) * .5f), join_b.x - 16 * u,
                      row.joinable ? (focused ? theme::paper : focus_ink) : focused ? focus_ink : chip_ink, does);
            }
            // VIEW PROFILE: a lighter tile under the pointer.
            {
                const bool lit = over_profile && !h.ring;
                theme::tile_rect(draw, profile_a, profile_b, focused ? (lit ? card_light_hover : card_light) : lit ? raised_hover : raised, 0x5d, u * 2);
                const float size = 32 * u, w = width_of(ui.bold, size, "VIEW PROFILE");
                label(draw, ui.bold, size, ImVec2(profile_a.x + std::max(16 * u, (profile_b.x - profile_a.x - w) * .5f), profile_a.y + (buttons - size) * .5f), profile_b.x - 16 * u,
                      focused ? skate_theme::black : theme::paper, "VIEW PROFILE");
            }
        };
        // A group's cards, `columns` to a row.
        const auto group = [&](std::size_t first, std::size_t end, float height) {
            for (std::size_t i = first; i < end; i += columns) {
                bool drawn{};
                const float top = list.next(height, card_gap, drawn);
                for (std::size_t j = i; j < std::min(end, i + columns); ++j) {
                    const float x = la.x + (card_width + card_gap) * static_cast<float>(j - i);
                    card(h.friends[j], ImVec2(x, top), ImVec2(x + card_width, top + height), drawn);
                }
            }
        };
        if (together) heading("IN A SESSION", together), group(0, together, session_height);
        if (together < h.friends.size()) heading("SKATING", h.friends.size() - together), group(together, h.friends.size(), solo_height);
        list.end();
    }

    // ---- under it: what happened last and what the buttons do
    {
        const float y = z.y - 52 * u;
        const auto text = view::caption(h.notice, 96);
        label(draw, ui.body, 30 * u, ImVec2(a.x, y + 4 * u), a.x + 1100 * u, theme::muted, text);
        const float x = a.x + (text.empty() ? 0 : std::min(width_of(ui.body, 30 * u, text), 1100 * u) + 60 * u);
        if (ui.pad.available) hints(ui, x, y, {{0x1000, "", "Join"}, {0x8000, "", "Steam profile"}, {0x4000, "", "Refresh"}, {0x20, "", "Join by code"}});
        else hints(ui, x, y, {{0, "Enter", "Join"}, {0, "P", "Steam profile"}, {0, "C", "Join by code"}});
    }
}

// The next (or the one before) of a setting's steps from where it is now; it stops at the ends.
template<std::size_t N> float stepped(float current, const std::array<float, N> &steps, int direction) {
    if (direction > 0) {
        for (const float step : steps)
            if (step > current + .001f) return step;
        return steps.back();
    }
    for (std::size_t i = steps.size(); i-- > 0;)
        if (steps[i] < current - .001f) return steps[i];
    return steps.front();
}
std::string whole(float value, const char *unit = "") { return std::to_string(static_cast<int>(value + .5f)) + unit; }

#include "mod_options_page.inl"

// ---------------------------------------------------------------- hosting

// HOST LOBBY: the map the lobby will be on as a big card on the left, with how the lobby will list in
// the server browser under it; on the right the lobby's settings as a list (Mod Options' rows, the
// D-pad's), skate.'s description card for the focused one under them, and HOST at the foot. The
// settings start from the ones used last time (saved_host).
void host_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    const float u = ui.u, gap = 36 * u;
    auto *draw = ui.draw;
    if (!h.host_seeded && mp.saved_host.loaded) {
        // Start from the host settings used last time.
        h.host_seeded = true;
        h.public_lobby = mp.saved_host.public_lobby;
        h.capacity = static_cast<unsigned>(mp.saved_host.capacity);
        h.tps = mp.saved_host.tps;
        if (!mp.saved_host.lobby_name.empty()) h.host_name = mp.saved_host.lobby_name, h.host_named = true;
    }
    if (!h.host_named && !mp.local_name.empty()) h.host_name = mp.local_name + "'s lobby", h.host_named = true;
    static bool password_on{};
    const bool idle = !mp.active && !mp.lobby_joining;
    const bool public_lobby = mp.hosting ? mp.public_host : h.public_lobby;
    const unsigned capacity = mp.hosting ? static_cast<unsigned>(mp.capacity) : h.capacity;
    const auto tps = mp.hosting ? mp.tps : h.tps;
    const std::string busy = mp.lobby_joining ? "A join is under way." : "You are in a session: leave it (Current session) to host your own.";

    // ---- left: the map, and how the lobby will list
    const float left_w = std::clamp((z.x - a.x) * .36f, 1100 * u, 1500 * u);
    const std::span<const Level> levels = h.full.levels;
    if (h.grid.empty()) h.grid = grid_levels(levels);
    if (ui.input && !std::exchange(h.pictures_asked, true)) want_grid_pictures(h.grid);
    Row here;
    here.map = view::map_name(mp.map, levels);
    for (const auto &entry : levels)
        if (view::fold(entry.asset) == view::fold(std::string(world_destination_asset(mp.map))) ||
            (native_tools::listed(entry) && squashed(native_tools::level_name(entry)) == squashed(here.map)))
            here.asset = entry.asset;
    {
        const float picture = left_w * .56f;
        const ImVec2 ca(a.x, a.y), cz(a.x + left_w, a.y + picture + 230 * u);
        theme::tile_rect(draw, ca, cz, skate_theme::tile, 0x7b1, u * 2);
        map_picture(ui, ImVec2(ca.x, ca.y), ImVec2(cz.x, ca.y + picture), here);
        const float tag_size = 26 * u, tag_w = width_of(ui.bold, tag_size, "THE MAP YOU ARE ON") + 28 * u;
        draw->AddRectFilled(ImVec2(ca.x + 24 * u, ca.y + 24 * u), ImVec2(ca.x + 24 * u + tag_w, ca.y + 24 * u + 44 * u), skate_theme::black);
        draw->AddText(ui.bold, tag_size, ImVec2(ca.x + 38 * u, ca.y + 24 * u + (44 * u - tag_size) * .5f), theme::paper, "THE MAP YOU ARE ON");
        std::string title = here.map.empty() ? std::string("No map loaded") : here.map;
        for (auto &c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        label(draw, ui.heading, 72 * u, ImVec2(ca.x + 40 * u, ca.y + picture + 36 * u), cz.x - 40 * u, theme::paper, title);
        draw->AddText(ui.body, 30 * u, ImVec2(ca.x + 40 * u, ca.y + picture + 130 * u), sub_ink,
                      "Everyone who joins loads it. To host on another map, travel there first (Mod Options, MAPS).", nullptr, left_w - 80 * u);
        // How it will list: a server browser row as it will read for everyone else.
        float y = cz.y + 44 * u;
        draw->AddText(ui.bold, 30 * u, ImVec2(a.x, y), soft, "HOW IT WILL LIST");
        y += 50 * u;
        const float row_h = 150 * u, pic_w = 230 * u;
        const ImVec2 ra(a.x, y), rz(a.x + left_w, y + row_h);
        theme::tile_rect(draw, ra, rz, skate_theme::tile, 0x7b2, u * 2);
        map_picture(ui, ra, ImVec2(ra.x + pic_w, rz.y), here);
        const std::string name = view::caption(h.host_name.empty() ? (mp.local_name.empty() ? std::string("Your lobby") : mp.local_name) : h.host_name, 40);
        const std::string count = "1 / " + std::to_string(capacity);
        const float count_w = width_of(ui.bold, 36 * u, count);
        const char *kind = public_lobby ? "LOBBY" : "JOIN CODE";
        const float kind_w = width_of(ui.bold, 24 * u, kind) + 24 * u;
        const float text_left = ra.x + pic_w + 30 * u, text_right = rz.x - count_w - kind_w - 70 * u;
        label(draw, ui.bold, 38 * u, ImVec2(text_left, ra.y + 34 * u), text_right, theme::paper, name);
        label(draw, ui.body, 28 * u, ImVec2(text_left, ra.y + 86 * u), text_right, sub_ink,
              (here.map.empty() ? std::string("No map") : here.map) + (password_on && !h.host_password.empty() ? "  \xc2\xb7  password" : ""));
        draw->AddRectFilled(ImVec2(rz.x - count_w - kind_w - 50 * u, ra.y + (row_h - 40 * u) * .5f), ImVec2(rz.x - count_w - 50 * u, ra.y + (row_h + 40 * u) * .5f),
                            public_lobby ? skate_theme::good : plate_grey, 3 * u);
        draw->AddText(ui.bold, 24 * u, ImVec2(rz.x - count_w - kind_w - 38 * u, ra.y + (row_h - 24 * u) * .5f), public_lobby ? skate_theme::black : theme::paper, kind);
        draw->AddText(ui.bold, 36 * u, ImVec2(rz.x - count_w - 30 * u, ra.y + (row_h - 36 * u) * .5f), theme::paper, count.c_str());
        if (!public_lobby)
            draw->AddText(ui.body, 26 * u, ImVec2(a.x, rz.y + 18 * u), sub_ink, "Not listed: this is how your friends see it once they use your code.", nullptr, left_w);
    }

    // ---- right: the lobby's settings, the card about the focused one, and HOST
    const float rx = a.x + left_w + gap, host_h = 130 * u, card_h = 300 * u;
    {
        draw->AddText(ui.bold, 34 * u, ImVec2(rx + 4 * u, a.y), theme::paper, "YOUR LOBBY");
        const float base = a.y + 54 * u;
        draw->AddQuadFilled(ImVec2(rx, base - 2 * u), ImVec2(z.x, base - 5 * u), ImVec2(z.x - 6 * u, base + 3 * u), ImVec2(rx + 4 * u, base + 5 * u), theme::paper);
    }
    Opts rows;
    const auto off = [&](Opt &row) -> Opt & { row.why_not = busy; return row; };
    off(opt_text(rows, "Lobby name", h.host_name, mp.local_name.empty() ? std::string("Your Steam name") : mp.local_name, idle,
                 "What the lobby is called in the server browser and in your friends' Steam lists. Up to 120 characters.", 120));
    off(opt_choice(rows, "Who can join", public_lobby ? "PUBLIC" : "JOIN CODE", idle,
                   "Public: listed in the server browser for anyone on a compatible build. Join code: not listed; friends join with the code "
                   "you share from Current session.",
                   [&h](int) { h.public_lobby = !h.public_lobby; }));
    off(opt_toggle(rows, "Password", password_on, idle, "Skaters must type a password to join, whichever way they find the lobby.",
                   [&h](bool on) {
                       password_on = on;
                       if (!on) wipe(h.host_password);
                   }));
    if (password_on)
        off(opt_text(rows, "The password", h.host_password, "Type one", idle, "What joining skaters must type. It is not saved.", 64, true));
    {
        static constexpr std::array<float, 5> limits{2.f, 4.f, 8.f, 16.f, static_cast<float>(multiplayer_lobby_player_limit)};
        auto &row = off(opt_number(rows, "Player limit", std::to_string(capacity), fraction_of(static_cast<float>(capacity), limits), idle,
                                   "How many skaters the lobby holds, you included.",
                                   [&h](int way) { h.capacity = static_cast<unsigned>(stepped(static_cast<float>(h.capacity), limits, way)); }));
        row.reset = [&h] { h.capacity = 16; };
        row.reset_does = "16 skaters";
    }
    {
        int at = 0;
        for (std::size_t i = 0; i < multiplayer_tick_rates.size(); ++i)
            if (multiplayer_tick_rates[i] == tps) at = static_cast<int>(i);
        off(opt_choice(rows, "Update rate", std::to_string(tps) + " TPS", idle,
                       "How often nearby skaters update: higher is smoother and uses more of your upload. Distant skaters update at 10 and 5.",
                       [&h, at](int way) {
                           h.tps = multiplayer_tick_rates[static_cast<std::size_t>(wrap_step(at, static_cast<int>(multiplayer_tick_rates.size()), way))];
                       }));
    }
    const float list_top = a.y + 80 * u, card_top = z.y - host_h - 24 * u - card_h;
    const auto focus = options_list(ui, hash("host-settings"), rows, ImVec2(rx, list_top), ImVec2(z.x, card_top - 20 * u), false);
    options_card(ui, hash("host-card"), focus, ImVec2(rx, card_top), ImVec2(z.x, card_top + card_h), .7f);

    // HOST, at the foot: what it hosts, or why it cannot.
    const bool ready = idle && mp.local_ready && (!password_on || !h.host_password.empty());
    const char *does = !idle ? "SESSION ALREADY ACTIVE" : !mp.local_ready ? "LOAD A MAP TO HOST" : password_on && h.host_password.empty() ? "TYPE A PASSWORD FIRST"
                     : h.public_lobby ? "HOST PUBLIC LOBBY" : "HOST WITH JOIN CODE";
    if (button(ui, hash("host"), ImVec2(rx, z.y - host_h), ImVec2(z.x, z.y), does, Look::primary, ready)) {
        if (h.host_name.size() > 128) say(h, "Lobby name is too long. Use a shorter name.");
        else if (send(h, "host-config", std::string(h.public_lobby ? "public " : "code ") + std::to_string(h.capacity) + " " + std::to_string(h.tps) + " " + h.host_name,
                      password_on ? h.host_password : std::string()))
            wipe(h.host_password);
    }
    if (!h.notice.empty() || !mp.status.empty())
        label(draw, ui.body, 28 * u, ImVec2(a.x, z.y + 20 * u), a.x + left_w, theme::muted, view::caption(!h.notice.empty() ? h.notice : mp.status, 110));
}

// ---------------------------------------------------------------- the session

void session_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    const float u = ui.u, gap = 28 * u, row = 96 * u, row_gap = 8 * u, list_width = (z.x - a.x) * .5f;
    auto *draw = ui.draw;
    label(draw, ui.heading, 52 * u, a, a.x + list_width, theme::paper, view::caption(mp.lobby_name.empty() ? "CURRENT SESSION" : mp.lobby_name, 64));
    draw->AddText(ui.bold, 28 * u, ImVec2(a.x, a.y + 76 * u), theme::muted, ("SKATERS  " + std::to_string(mp.players) + " / " + std::to_string(mp.capacity)).c_str());
    {
        // Who is here: a list to read through. The rows take the focus only so that the D-pad
        // can scroll it.
        Scroller list(ui, hash("players"), ImVec2(a.x, a.y + 128 * u), ImVec2(a.x + list_width, z.y));
        const auto person = [&](std::uint64_t id, const std::string &name, const std::string &role) {
            const auto key = hash("player", id);
            bool drawn{};
            const float top = list.next(row, row_gap, drawn);
            const ImVec2 p(a.x, top), q(a.x + list_width - 20 * u, top + row);
            const Hit hit = item(ui, key, p, q, &list.box);
            list.reveal(key, top, row);
            if (!drawn) return;
            theme::tile_rect(draw, p, q, hit.focus && h.ring ? row_hover : skate_theme::tile, key & 0xfff, u * 2);
            const float w = role.empty() ? 0 : width_of(ui.bold, 28 * u, role) + 30 * u;
            label(draw, ui.bold, 40 * u, ImVec2(p.x + 30 * u, top + (row - 40 * u) * .5f), q.x - w - 30 * u, theme::paper, view::caption(name, 48));
            if (!role.empty()) draw->AddText(ui.bold, 28 * u, ImVec2(q.x - w, top + (row - 28 * u) * .5f), theme::muted, role.c_str());
        };
        person(mp.local_id, mp.local_name, mp.hosting ? "YOU, HOST" : mp.parties && mp.party_leader ? "YOU, PARTY LEADER" : "YOU");
        for (const auto &player : mp.roster) {
            if (player.id == mp.local_id || !player.connected) continue;
            person(player.id, player.name, player.id == mp.host_id ? "HOST" : !mp.parties || !player.party_member ? "" : player.party_leader ? "PARTY LEADER" : "PARTY");
        }
        list.end();
    }

    // Beside the list: the session and its settings.
    const ImVec2 da(a.x + list_width + gap, a.y), dz(z.x, z.y);
    theme::tile_rect(draw, da, dz, skate_theme::tile, 0x7a, u * 2);
    const float inset = 36 * u, left = da.x + inset, edge = dz.x - inset - 14 * u, opt = 84 * u, opt_gap = 10 * u;
    Scroller side(ui, hash("session-side"), ImVec2(da.x, da.y + inset), ImVec2(dz.x, dz.y - inset));
    const auto text = [&](ImFont *font, float size, ImU32 colour, const std::string &line, float below) {
        bool drawn{};
        const float top = side.next(size, below, drawn);
        if (drawn) label(draw, font, size, ImVec2(left, top), edge, colour, line);
    };
    // A row of the panel: a setting (with `value`) or a plain action. Returns whether it was pressed.
    const auto option = [&](const std::string &name, const std::string &value, bool enabled, Look look = Look::plain) {
        const auto id = hash(name, 5);
        bool drawn{};
        const float top = side.next(opt, opt_gap, drawn);
        side.reveal(id, top, opt);
        // (Drawn or not, it keeps its place in the focus order, so the D-pad can reach it.)
        return button(ui, id, ImVec2(left, top), ImVec2(edge, top + opt), name, look, enabled, value, &side.box);
    };
    {
        // Leaving comes first: it is what most visits to this page are for.
        if (option(mp.hosting ? "End session" : "Leave server", {}, true, Look::primary)) send(h, "stop");
        bool drawn{};
        side.next(10 * u, 10 * u, drawn);
        text(ui.bold, 28 * u, theme::muted, "MAP", 10 * u);
        text(ui.bold, 40 * u, theme::paper, view::map_name(mp.map, ui.h.full.levels), 30 * u);
        if (!mp.invite.empty()) {
            text(ui.bold, 28 * u, theme::muted, "JOIN CODE", 10 * u);
            text(ui.bold, 36 * u, theme::paper, mp.invite, 16 * u);
            if (option("Copy join code", {}, true)) ImGui::SetClipboardText(mp.invite.c_str()), say(h, "Join code copied.");
        }
        if (mp.parties) {
            for (const auto &invite : mp.party_invites) {
                const auto from = std::to_string(invite.from);
                text(ui.body, 32 * u, theme::paper, view::caption(invite.name, 32) + " invited you to their party", 12 * u);
                if (option("Accept invite from " + view::caption(invite.name, 20), {}, true)) send(h, "party", "accept " + from);
                if (option("Decline " + view::caption(invite.name, 20), {}, true)) send(h, "party", "decline " + from);
            }
            if (mp.party) {
                if (mp.party_leader && option("Party", mp.party_open ? "Anyone can join" : "Invite only", true)) send(h, "party", mp.party_open ? "close" : "open");
                if (option("Leave party", {}, true)) send(h, "party", "leave");
            }
        }
        text(ui.body, 30 * u, theme::muted, "Tick rate: " + std::to_string(mp.tps), 16 * u);
    }
    text(ui.body, 30 * u, theme::muted, view::caption(!h.notice.empty() ? h.notice : mp.status, 110), 0);
    side.end();
}

// ---------------------------------------------------------------- voice chat

constexpr std::array voice_distances{10.f, 20.f, 35.f, 50.f, 75.f, 100.f, 150.f, 300.f};
constexpr std::array voice_volumes{0.f, .25f, .5f, .75f, 1.f, 1.5f, 2.f, 3.f, 5.f};
template<std::size_t N> float next_step(float current, const std::array<float, N> &steps) {
    const auto found = std::upper_bound(steps.begin(), steps.end(), current + 0.001f);
    return found == steps.end() ? steps.front() : *found;
}
std::string volume_label(float value) {
    auto text = std::to_string(value);
    return text.substr(0, text.find('.') + 3) + "x";
}

void voice_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    const float u = ui.u, gap = 28 * u, row = 96 * u, row_gap = 8 * u, list_width = (z.x - a.x) * .5f;
    auto *draw = ui.draw;
    if (h.voice_pending && (*h.voice_pending == mp.voice.settings || GetTickCount64() >= h.voice_pending_until)) h.voice_pending.reset();
    auto value = h.voice_pending.value_or(mp.voice.settings);
    draw->AddText(ui.heading, 52 * u, a, theme::paper, "SKATERS IN YOUR SESSION");
    {
        Scroller list(ui, hash("voices"), ImVec2(a.x, a.y + 96 * u), ImVec2(a.x + list_width, z.y));
        bool anyone{};
        for (const auto &player : mp.roster) {
            if (player.id == mp.local_id || !player.connected) continue;
            anyone = true;
            const auto id = std::to_string(player.id);
            const auto found = std::find_if(mp.voice.players.begin(), mp.voice.players.end(), [&](const auto &entry) { return entry.id == player.id; });
            const auto voice = found == mp.voice.players.end() ? VoicePlayer{player.id} : *found;
            bool drawn{};
            const float top = list.next(row, row_gap, drawn);
            const float right = a.x + list_width - 20 * u, volume_x = right - 330 * u, mute_x = volume_x - 12 * u - 260 * u,
                        ignore_x = mute_x - 12 * u - 260 * u;
            const auto mute = hash("mute", player.id), volume = hash("volume", player.id), ignore = hash("ignore", player.id);
            list.reveal(mute, top, row), list.reveal(volume, top, row), list.reveal(ignore, top, row);
            if (drawn) {
                theme::tile_rect(draw, ImVec2(a.x, top), ImVec2(ignore_x - 12 * u, top + row), skate_theme::tile, mute & 0xfff, u * 2);
                label(draw, ui.bold, 40 * u, ImVec2(a.x + 30 * u, top + (row - 40 * u) * .5f), ignore_x - 40 * u,
                      player.ignored ? theme::muted : mp.voice.allowed && voice.speaking && !voice.muted ? skate_theme::good : theme::paper,
                      view::caption(player.name, 40));
            }
            // Ignore: their skater, voice, chat and everything else of theirs, on this screen only.
            if (button(ui, ignore, ImVec2(ignore_x, top), ImVec2(mute_x - 12 * u, top + row), player.ignored ? "IGNORED" : "IGNORE", Look::plain, true, {}, &list.box))
                send(h, "ignore", id + (player.ignored ? " off" : " on"));
            if (button(ui, mute, ImVec2(mute_x, top), ImVec2(volume_x - 12 * u, top + row), voice.muted ? "MUTED" : "MUTE", Look::plain, true, {}, &list.box))
                send(h, "voice-mute", id + (voice.muted ? " off" : " on"));
            if (button(ui, volume, ImVec2(volume_x, top), ImVec2(right, top + row), "Volume", Look::plain, true, volume_label(voice.volume), &list.box))
                send(h, "voice-volume", id + " " + std::to_string(next_step(voice.volume, voice_volumes)));
        }
        list.end();
        if (!anyone) {
            draw->AddText(ui.body, 38 * u, ImVec2(a.x, a.y + 110 * u), theme::muted, mp.active ? "No other skaters in the session yet." : "You are not in a session yet.");
            draw->AddText(ui.body, 32 * u, ImVec2(a.x, a.y + 164 * u), theme::muted, "These voice settings apply to every session you join.");
        }
    }
    const ImVec2 sa(a.x + list_width + gap, a.y);
    const float right = std::min(z.x, sa.x + 1500 * u), opt = 92 * u;
    float y = sa.y;
    draw->AddText(ui.heading, 52 * u, ImVec2(sa.x, y), theme::paper, "VOICE SETTINGS");
    y += 84 * u;
    bool changed{};
    const auto setting = [&](const char *name, const std::string &shown, bool enabled = true) {
        const bool pressed = button(ui, hash(name, 4), ImVec2(sa.x, y), ImVec2(right, y + opt), name, Look::tile, enabled, shown);
        y += opt + 10 * u;
        return pressed;
    };
    if (setting("Voice chat", value.enabled ? "On" : "Off")) value.enabled = !value.enabled, changed = true;
    if (setting("Transmit", value.open_mic ? "Open mic" : "Push to talk")) value.open_mic = !value.open_mic, changed = true;
    if (!value.open_mic) {
        std::string binds = "Talk key: " + voice_key_name(value.push_to_talk);
        if (value.controller_combo) binds += "   Pad: " + controller_combo_label(value.controller_combo, ui.pad.style);
        label(draw, ui.body, 30 * u, ImVec2(sa.x + 30 * u, y), right, theme::muted, binds);
        y += 50 * u;
    }
    if (setting("Proximity voice", value.proximity ? "On" : "Off")) value.proximity = !value.proximity, changed = true;
    if (setting("Voice distance", std::to_string(static_cast<int>(value.distance + .5f)) + " m", value.proximity)) value.distance = next_step(value.distance, voice_distances), changed = true;
    if (setting("Listening volume", volume_label(value.volume))) value.volume = next_step(value.volume, voice_volumes), changed = true;
    if (setting("Microphone volume", volume_label(value.microphone))) value.microphone = next_step(value.microphone, voice_volumes), changed = true;
    if (changed) {
        const auto argument = std::to_string(value.enabled) + " " + std::to_string(value.proximity) + " " + std::to_string(value.push_to_talk) + " " +
                              std::to_string(value.distance) + " " + std::to_string(value.volume) + " " + std::to_string(value.open_mic) + " " +
                              std::to_string(value.controller_combo) + " " + std::to_string(value.microphone);
        if (send(h, "voice", argument)) h.voice_pending = value, h.voice_pending_until = GetTickCount64() + 2000;
    }
    if (mp.hosting) {
        if (setting("Lobby voice", mp.voice.allowed ? "Allowed" : "Disabled")) send(h, "voice-allow", mp.voice.allowed ? "off" : "on");
    } else if (mp.active && !mp.voice.allowed) {
        label(draw, ui.body, 30 * u, ImVec2(sa.x + 30 * u, y), right, skate_theme::warning, "The host has disabled voice chat for this lobby.");
        y += 50 * u;
    }
    label(draw, ui.body, 30 * u, ImVec2(sa.x + 30 * u, y + 8 * u), right, mp.voice.transmitting ? skate_theme::good : theme::muted,
          !h.notice.empty() ? h.notice : mp.voice.transmitting ? std::string("MICROPHONE TRANSMITTING") : mp.voice.status);
}

// A join under way, as a card over the middle of the page (which is dimmed behind it): where
// to, what is happening, and the way out.
void connecting_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    const float u = ui.u, width = 1500 * u, inset = 48 * u, tall = 100 * u;
    const float height = inset + 28 * u + 30 * u + 64 * u + 30 * u + 34 * u + 40 * u + 14 * u + 44 * u + tall + inset;
    const ImVec2 p(a.x + (z.x - a.x - width) * .5f, a.y + std::max(0.f, (z.y - a.y - height) * .4f)), q(p.x + width, p.y + height);
    auto *draw = ui.draw;
    draw->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 150));
    theme::tile_rect(draw, p, q, IM_COL32(26, 26, 26, 252), 0x4c0, u * 2);
    const float left = p.x + inset, edge = q.x - inset;
    float y = p.y + inset;
    draw->AddText(ui.bold, 28 * u, ImVec2(left, y), theme::blue, "CONNECTING");
    y += 28 * u + 30 * u;
    const auto name = !h.joining_name.empty() ? h.joining_name : !mp.lobby_name.empty() ? view::caption(mp.lobby_name, 40) : std::string("Joining the session");
    label(draw, ui.heading, 64 * u, ImVec2(left, y), edge, theme::paper, name);
    y += 64 * u + 30 * u;
    label(draw, ui.body, 34 * u, ImVec2(left, y), edge, theme::muted, view::caption(mp.status.empty() ? std::string("Connecting...") : mp.status, 100));
    y += 34 * u + 40 * u;
    // Not a measure of anything: a block going back and forth while it is under way.
    draw->AddRectFilled(ImVec2(left, y), ImVec2(edge, y + 14 * u), skate_theme::tile_light, 4 * u);
    const float at = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0)), span = edge - left;
    const float where = (at < 1.f ? at : 2.f - at) * span * .75f;
    draw->AddRectFilled(ImVec2(left + where, y), ImVec2(left + where + span * .25f, y + 14 * u), theme::blue, 4 * u);
    y += 14 * u + 44 * u;
    const auto cancel = hash("join-cancel");
    if (button(ui, cancel, ImVec2(left, y), ImVec2(edge, y + tall), "CANCEL", Look::plain) && send(h, "stop")) h.join_cancelled = true, say(h, "Cancelled.");
    if (!h.focus) h.focus_next = cancel;
}

// A join that did not get in, in the connecting card's place: where to, why, and a button that
// puts it away.
void failed_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const float u = ui.u, width = 1500 * u, inset = 48 * u, tall = 100 * u, size = 34 * u;
    const float column = width - inset * 2;
    const auto reason = view::caption(h.failed, 400);
    const float lines = std::min(ui.body->CalcTextSizeA(size, FLT_MAX, column, reason.c_str()).y, size * 6.2f);
    const float height = inset + 28 * u + 30 * u + 64 * u + 30 * u + lines + 44 * u + tall + inset;
    const ImVec2 p(a.x + (z.x - a.x - width) * .5f, a.y + std::max(0.f, (z.y - a.y - height) * .4f)), q(p.x + width, p.y + height);
    auto *draw = ui.draw;
    draw->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 150));
    theme::tile_rect(draw, p, q, IM_COL32(26, 26, 26, 252), 0x4c0, u * 2);
    const float left = p.x + inset, edge = q.x - inset;
    float y = p.y + inset;
    draw->AddText(ui.bold, 28 * u, ImVec2(left, y), skate_theme::danger, "COULDN'T CONNECT");
    y += 28 * u + 30 * u;
    label(draw, ui.heading, 64 * u, ImVec2(left, y), edge, theme::paper, h.failed_name.empty() ? std::string("The session") : h.failed_name);
    y += 64 * u + 30 * u;
    draw->PushClipRect(ImVec2(left, y), ImVec2(edge, y + lines), true);
    draw->AddText(ui.body, size, ImVec2(left, y), theme::muted, reason.c_str(), nullptr, column);
    draw->PopClipRect();
    y += lines + 44 * u;
    const auto ok = hash("join-failed-ok");
    if (button(ui, ok, ImVec2(left, y), ImVec2(edge, y + tall), "OK", Look::plain)) h.failed.clear();
    if (!h.focus) h.focus_next = ok;
}

// ---------------------------------------------------------------- Mod Options

// MAPS is the page's own grid; the others are lists of settings made for a controller
// (mod_options_page.inl), holding everything the Insert menu sets.
enum ToolsTab : int { official_tab, custom_tab, world_tab, parks_tab, skater_tab, camera_tab, style_tab, online_tab, settings_tab };


// A column of settings that scrolls, in the Multiplayer page's look: each setting a full-width row on
// the near-black tile with its name on the left and what it is now on the right (white under the
// D-pad or the keys), in groups under small headings.
struct Column {
    Ui &ui;
    Scroller list;
    float left, right, tall, between;
    bool placed{}; // anything in it yet: the first heading needs no room over it
    Column(Ui &owner, std::uint32_t id, ImVec2 a, ImVec2 b)
        : ui(owner), list(owner, id, a, b), left(a.x), right(b.x - 26 * owner.u), tall(96 * owner.u), between(10 * owner.u) {}
    // The top of a row `height` tall (see Scroller::next).
    float next(float height, float gap, bool &drawn) {
        placed = true;
        return list.next(height, gap, drawn);
    }
    // A row that is pressed: a switch, a choice that goes round, or an action (no value).
    bool setting(const std::string &name, const std::string &value, bool enabled, Look look = Look::tile) {
        const auto id = hash(name, 21);
        bool drawn{};
        const float top = next(tall, between, drawn);
        list.reveal(id, top, tall);
        return button(ui, id, ImVec2(left, top), ImVec2(right, top + tall), name, look, enabled, value, &list.box);
    }
    // A row with a number: less and more at its right end. Returns -1, 0 or 1.
    int adjust(const std::string &name, const std::string &value, bool enabled) {
        const float u = ui.u, side = 96 * u, readout = 300 * u;
        const auto less = hash(name, 22), more = hash(name, 23);
        bool drawn{};
        const float top = next(tall, between, drawn);
        list.reveal(less, top, tall), list.reveal(more, top, tall);
        const float more_x = right - side, less_x = more_x - readout - side;
        if (drawn) {
            theme::tile_rect(ui.draw, ImVec2(left, top), ImVec2(less_x - 10 * u, top + tall), tile_dark, less & 0xfff, u * 2);
            label(ui.draw, ui.body, 40 * u, ImVec2(left + 30 * u, top + (tall - 40 * u) * .5f), less_x - 30 * u, enabled ? theme::paper : theme::muted, name);
            theme::tile_rect(ui.draw, ImVec2(less_x + side + 6 * u, top), ImVec2(more_x - 6 * u, top + tall), tile_dark, more & 0xfff, u * 2);
            // (The value in the kit's blue, as button() and the Insert menu's option rows show one.)
            const float w = width_of(ui.bold, 40 * u, value);
            ui.draw->AddText(ui.bold, 40 * u, ImVec2(less_x + side + (readout - w) * .5f, top + (tall - 40 * u) * .5f), enabled ? theme::blue : theme::muted, value.c_str());
        }
        int result{};
        if (button(ui, less, ImVec2(less_x, top), ImVec2(less_x + side, top + tall), "-", Look::plain, enabled, {}, &list.box)) result = -1;
        if (button(ui, more, ImVec2(more_x, top), ImVec2(right, top + tall), "+", Look::plain, enabled, {}, &list.box)) result = 1;
        return result;
    }
    // A group's heading: small white capitals, with room over it after the group before.
    void heading(const std::string &text) {
        const float u = ui.u, size = 28 * u, height = placed ? 84 * u : 44 * u;
        bool drawn{};
        const float top = next(height, 14 * u, drawn);
        if (drawn) group_heading(ui, ImVec2(left, top + height - size - 4 * u), text);
    }
    void title(const std::string &text) { heading(text); }
    void note(const std::string &text, ImU32 colour = theme::muted) {
        if (text.empty()) return;
        const float size = 30 * ui.u, wrap = right - left;
        bool drawn{};
        const float top = next(ui.body->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str()).y + 6 * ui.u, 18 * ui.u, drawn);
        if (drawn) ui.draw->AddText(ui.body, size, ImVec2(left, top + 6 * ui.u), colour, text.c_str(), nullptr, wrap);
    }
    void end() { list.end(); }
};

// A settings tab's two columns: side by side on a page wide enough for two, so that short rows do not
// leave most of a wide page empty; else one after the other in a single column.
template<class Left, class Right> void two_columns(Ui &ui, std::uint32_t id, ImVec2 a, ImVec2 z, Left &&left_side, Right &&right_side) {
    const float u = ui.u, gap = 60 * u, room = z.x - a.x;
    if (room >= 2600 * u) {
        const float w = std::min((room - gap) * .5f, 1900 * u);
        Column first(ui, hash("left", id), a, ImVec2(a.x + w, z.y));
        left_side(first);
        first.end();
        Column second(ui, hash("right", id), ImVec2(a.x + w + gap, a.y), ImVec2(a.x + w * 2 + gap, z.y));
        right_side(second);
        second.end();
        return;
    }
    Column only(ui, id, a, ImVec2(a.x + std::min(room, 2100 * u), z.y));
    left_side(only);
    right_side(only);
    only.end();
}

// The page's tabs. What a control does, and when it can, is the same as on ReSkate's own menu
// (native_tools_view.h for travel, parks and the world; the same requests and commands that
// menu sends for the rest); how it is laid out is here, in the Multiplayer page's look: no title,
// groups of full-width rows, and a line under it with what happened last and what the buttons do.
void tools_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &m = h.full;
    const auto &mp = m.multiplayer;
    const auto &cb = h.callbacks;
    const float u = ui.u, gap = 28 * u, footer = 70 * u;
    auto *draw = ui.draw;
    native_tools::sync(h.tools, m);
    const auto changed = [&] { h.next_model = {}; }; // what it changed shows at once
    const auto act = [&](std::string_view command, const std::string &argument = {}) {
        native_tools::activate(h.tools, m, cb, command, argument);
        if (!h.tools.feedback.empty()) say(h, std::exchange(h.tools.feedback, {}));
        changed();
    };
    const bool console = cb.queue_console_command != nullptr, debugging = cb.queue_debug != nullptr;
    const auto run = [&](const std::string &line) {
        std::array<char, 512> result{};
        if (!console || !cb.queue_console_command(cb.user, line.c_str(), result.data(), result.size())) say(h, "Couldn't apply that change. Try again when the game is ready.");
        changed();
    };
    const auto debug = [&](DebugRequest request) {
        std::array<char, 512> result{};
        if (!debugging || !cb.queue_debug(cb.user, request, result.data(), result.size())) say(h, "Couldn't apply that change. Try again when the game is ready.");
        changed();
    };
    const auto online = [&](const char *action, const std::string &argument) {
        send(h, action, argument);
        changed();
    };
    const auto on_off = [](bool on) { return std::string(on ? "On" : "Off"); };
    const bool fresh_input = ui.input && !h.typing;
    const std::uint32_t pressed = fresh_input ? ui.pad.buttons & ~h.pad_before : 0;
    // The tabs draw down to here; the line under them is the page's foot.
    const ImVec2 bz(z.x, z.y - footer);
    // What the foot says when nothing has happened: the tab's own advice, if it has any.
    std::string advice;

    // MAPS: every map as a grid of cards, each with its picture (an official map's loading screen, a
    // custom map's mod icon: the same pictures as the ReSkate menu's map grid, menu_map_pictures.cpp)
    // and its name under it; pressing one travels there. (Not Tutorial Island: the game's first-time
    // tutorial.) Over it, the Insert menu's Map > Travel filters: which maps (all, the game's own or
    // the custom ones) and in what order (by name, or the custom maps first by when their mod was
    // installed, newest first, with a NEW tag on those from the last week). The choices are the Insert
    // menu's own (SkateMenu::map_show and map_sort), so the two agree for the rest of the run. When
    // each mod was installed comes with the pictures (map_icons.cpp reads it off this thread), so
    // nothing here reads the disk.
    const auto maps = [&] {
        auto &menu = state().menu;
        auto listed = grid_levels(m.levels);
        // (All of them, whatever is shown: the server browser asks for the same set.)
        want_grid_pictures(listed);
        const auto *root = native_tools::level(m, native_tools::root_asset);
        const bool travel = root && root->native_registered && root->can_load && m.can_queue_load && !multiplayer_controls_level(mp) && cb.queue_load;
        const auto list_id = hash("maps", 30);
        // ---- the filters, as chips: A or a click picks one; Y goes from one order to the other.
        const float bar = 96 * u, c_top = a.y + 12 * u, c_bottom = a.y + bar - 12 * u;
        float x = a.x;
        bool refilter{};
        chip_label(ui, x, c_top, c_bottom, "SHOW");
        constexpr std::array<const char *, 3> shows{"ALL", "STOCK", "CUSTOM"};
        for (int i = 0; i < static_cast<int>(shows.size()); ++i)
            if (chip(ui, hash(shows[static_cast<std::size_t>(i)], 33), x, c_top, c_bottom, shows[static_cast<std::size_t>(i)], menu.map_show == i))
                menu.map_show = i, refilter = true;
        x += 40 * u;
        chip_label(ui, x, c_top, c_bottom, "SORT");
        constexpr std::array<const char *, 2> sorts{"A TO Z", "NEWEST INSTALLED"};
        for (int i = 0; i < static_cast<int>(sorts.size()); ++i)
            if (chip(ui, hash(sorts[static_cast<std::size_t>(i)], 34), x, c_top, c_bottom, sorts[static_cast<std::size_t>(i)], menu.map_sort == i))
                menu.map_sort = i, refilter = true;
        if (pressed & 0x8000) menu.map_sort = menu.map_sort == 1 ? 0 : 1, refilter = true; // Y / Triangle
        if (refilter) h.scrolls[list_id] = 0;
        // Why a level cannot be loaded now: at the bar's other end.
        const std::string blocked = multiplayer_controls_level(mp) ? "Only the lobby host can change levels."
                                    : !m.can_queue_load ? "Travel is unavailable right now. Wait for the game to finish loading." : std::string();
        if (!blocked.empty()) {
            const float size = 30 * u, w = width_of(ui.body, size, blocked);
            label(draw, ui.body, size, ImVec2(std::max(x + 30 * u, bz.x - w), a.y + (bar - size) * .5f), bz.x, theme::muted, blocked);
        }
        // ---- which maps, in what order (as skate_menu_world.cpp's level_picker has them)
        const bool show_stock = menu.map_show != 2, show_custom = menu.map_show != 1, newest = menu.map_sort == 1;
        std::vector<std::pair<const Level *, std::int64_t>> shown;
        for (const auto *entry : listed)
            if (entry->custom ? show_custom : show_stock) shown.emplace_back(entry, newest && entry->custom ? menu_map_installed(entry->asset) : 0);
        // (Stable: the stock maps, and custom ones whose time is not known yet, keep their order by name.)
        if (newest)
            std::stable_sort(shown.begin(), shown.end(), [](const auto &x1, const auto &x2) {
                if (x1.first->custom != x2.first->custom) return x1.first->custom;
                return x1.second > x2.second;
            });
        const auto customs = static_cast<std::size_t>(std::count_if(shown.begin(), shown.end(), [](const auto &entry) { return entry.first->custom; }));
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        constexpr std::int64_t new_for = 7 * 24 * 60 * 60;
        // ---- the grid
        const float card_gap = 24 * u, room = bz.x - a.x - 16 * u, top = a.y + bar + gap;
        const int columns = std::max(1, static_cast<int>((room + card_gap) / (540 * u + card_gap)));
        const float card = (room - card_gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const float picture = card * 0.5625f, name_bar = 86 * u, card_tall = picture + name_bar;
        Scroller list(ui, list_id, ImVec2(a.x, top), bz);
        float row_top{};
        bool row_drawn{};
        std::size_t in_section{};
        int section = -1;
        for (const auto &[entry, installed] : shown) {
            // Each group (the game's maps, the custom ones) starts a row of its own under its heading.
            if (static_cast<int>(entry->custom) != section) {
                const float height = section < 0 ? 44 * u : 84 * u;
                section = static_cast<int>(entry->custom);
                in_section = 0;
                bool heading_drawn{};
                const float heading_top = list.next(height, 14 * u, heading_drawn);
                const auto count = entry->custom ? customs : shown.size() - customs;
                if (heading_drawn)
                    group_heading(ui, ImVec2(a.x, heading_top + height - 32 * u), std::string(entry->custom ? "CUSTOM MAPS" : "OFFICIAL MAPS") + "  \xc2\xb7  " + std::to_string(count));
            }
            const auto column = static_cast<int>(in_section++ % static_cast<std::size_t>(columns));
            if (!column) row_top = list.next(card_tall, card_gap, row_drawn);
            const auto id = hash("level", hash(entry->asset));
            const bool can = travel && entry->can_load && entry != root;
            const ImVec2 p(a.x + (card + card_gap) * static_cast<float>(column), row_top), q(p.x + card, row_top + card_tall);
            const Hit hit = item(ui, id, p, q, &list.box);
            list.reveal(id, row_top, card_tall);
            if (hit.press && can) h.tools.destination = entry->asset, act("load-level");
            if (!row_drawn) continue;
            // White under the D-pad or the keys, a lighter grey under the pointer, as the browser's rows.
            const auto [focused, pointed] = lit_of(hit);
            const bool lit = focused || pointed;
            theme::tile_rect(draw, p, q, focused ? theme::focus_fill : pointed ? row_hover : tile_dark, id & 0xfff, u * 2);
            // The picture: the middle band of its square, to the card's shape.
            const ImVec2 shot_a(p.x + 8 * u, p.y + 8 * u), shot_b(q.x - 8 * u, p.y + picture);
            ImTextureID texture{};
            ImVec2 uv0, uv1;
            if (menu_map_picture(entry->asset, texture, uv0, uv1)) {
                const float band = (uv1.y - uv0.y) * (1.0f - (shot_b.y - shot_a.y) / (shot_b.x - shot_a.x)) * 0.5f;
                draw->AddImage(texture, shot_a, shot_b, ImVec2(uv0.x, uv0.y + band), ImVec2(uv1.x, uv1.y - band),
                               can || lit ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 110));
            } else {
                draw->AddRectFilled(shot_a, shot_b, IM_COL32(30, 30, 33, 255));
                const char *initial = entry->custom ? "CUSTOM" : "MAP";
                const float size = 30 * u;
                draw->AddText(ui.bold, size, ImVec2(shot_a.x + (shot_b.x - shot_a.x - width_of(ui.bold, size, initial)) * .5f, shot_a.y + (shot_b.y - shot_a.y - size) * .5f),
                              theme::muted, initial);
            }
            // Installed in the last week, while they are in that order: the Insert menu's NEW tag in a corner.
            if (newest && entry->custom && installed && now - installed < new_for) {
                const float size = 26 * u, w = width_of(ui.bold, size, "NEW") + 28 * u;
                const ImVec2 t0(shot_b.x - 14 * u - w, shot_a.y + 14 * u), t1(shot_b.x - 14 * u, shot_a.y + 14 * u + 44 * u);
                draw->AddRectFilled(t0, t1, skate_theme::bar, 3 * u);
                draw->AddText(ui.bold, size, ImVec2(t0.x + 14 * u, t0.y + (t1.y - t0.y - size) * .5f), skate_theme::black, "NEW");
            }
            // The name under it, and on the one pointed at what pressing it does: LOAD on the blue, or
            // why not on grey.
            const float bar_top = p.y + picture, text_y = bar_top + (name_bar - 38 * u) * .5f;
            float name_right = q.x - 24 * u;
            if (lit) {
                const char *does = can ? "LOAD" : multiplayer_controls_level(mp) ? "HOST'S CHOICE" : "NOT NOW";
                const float w = width_of(ui.bold, 28 * u, does) + 36 * u;
                const ImVec2 c(q.x - w - 18 * u, bar_top + 18 * u), d(q.x - 18 * u, q.y - 18 * u);
                theme::tile_rect(draw, c, d, can ? (focused ? skate_theme::black : primary_rest) : focused ? track_lit : track, 0x52, u * 2);
                draw->AddText(ui.bold, 28 * u, ImVec2(c.x + 18 * u, c.y + (d.y - c.y - 28 * u) * .5f),
                              can ? (focused ? theme::paper : focus_ink) : focused ? focus_ink : theme::paper, does);
                name_right = c.x - 16 * u;
            }
            label(draw, ui.bold, 38 * u, ImVec2(p.x + 24 * u, text_y), name_right, focused ? focus_ink : can || pointed ? theme::paper : theme::muted,
                  native_tools::level_name(*entry));
        }
        list.end();
        if (listed.empty()) draw->AddText(ui.body, 38 * u, ImVec2(a.x, top + 20 * u), theme::muted, "Waiting for available levels...");
        else if (shown.empty()) draw->AddText(ui.body, 38 * u, ImVec2(a.x, top + 20 * u), theme::muted, show_custom ? "No custom maps to show." : "No stock maps to show.");
    };

    switch (h.tools_tab) {
    case official_tab:
    case custom_tab: maps(); break; // (custom maps are on the one tab now)
    default: {
        // A list of settings and the panel about the one the focus is on (mod_options_page.inl).
        OptionsContext context{h, m, cb, run, debug, online, act, console, debugging};
        Opts rows;
        switch (h.tools_tab) {
        case world_tab: world_rows(rows, context); break;
        case parks_tab: parks_rows(rows, context); break;
        case skater_tab: skater_rows(rows, context); break;
        case camera_tab: camera_rows(rows, context); break;
        case style_tab: style_rows(rows, context); break;
        case online_tab: online_rows(rows, context); break;
        default: settings_rows(rows, context); break;
        }
        // (Recording a bind keeps its own words in the foot.)
        if (h.recording_bind) advice = "Press the button, combination or key to use. Escape cancels.";
        options_page(ui, static_cast<std::uint32_t>(h.tools_tab), rows, a, bz, (pressed & 0x8000) != 0);
        break;
    }
    }

    // ---- under it: what happened last (else the tab's advice) and what the buttons do
    {
        const float y = z.y - 52 * u, most = 1900 * u;
        const bool on_maps = h.tools_tab == official_tab || h.tools_tab == custom_tab;
        const auto text = view::caption(h.recording_bind || h.notice.empty() ? advice : h.notice, 140);
        label(draw, ui.body, 30 * u, ImVec2(a.x, y + 4 * u), a.x + most, theme::muted, text);
        const float x = a.x + (text.empty() ? 0 : std::min(width_of(ui.body, 30 * u, text), most) + 60 * u);
        if (ui.pad.available) {
            if (on_maps) hints(ui, x, y, {{0x1000, "", "Load"}, {0x8000, "", "Sort"}});
            else hints(ui, x, y, {{0x1000, "", "Select"}, {0x4 | 0x8, "", "Change"}, {0x8000, "", "Default"}});
        } else {
            hints(ui, x, y, {{0, "Enter", on_maps ? "Load" : "Select"}, {0, "Left Right", "Change"}});
        }
    }
}

// ---------------------------------------------------------------- session settings

// What the session allows, for whoever runs it: the host of a lobby, or an admin of a
// dedicated server (whose changes go to the server). Nobody else has the tab.
void settings_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    Column list(ui, hash("session-settings"), a, ImVec2(a.x + std::min(z.x - a.x, 2100 * ui.u), z.y));
    list.heading("SESSION SETTINGS");
    // (Each is sent as the value it should have, the way a server's own console takes it.)
    list.note(mp.dedicated ? "You are an admin: these change for the whole server." : "You are the host: these change for everyone in your lobby.");
    const auto allowed = [](bool on) { return std::string(on ? "Allowed" : "Off"); };
    list.title("BUILDING");
    if (list.setting("Object placement", mp.dedicated && mp.object_placement == ObjectPlacement::host_only ? std::string("Admins only")
                                                                                                           : std::string(object_placement_name(mp.object_placement)), true))
        send(h, "object-placement", mp.object_placement == ObjectPlacement::everyone ? "host" : mp.object_placement == ObjectPlacement::host_only ? "nobody" : "everyone");
    {
        static constexpr std::array<unsigned, 7> limits{0, 10, 25, 50, 100, 250, 500};
        const auto found = std::find(limits.begin(), limits.end(), mp.object_limit);
        const auto next = limits[found == limits.end() ? 0 : static_cast<std::size_t>(found - limits.begin() + 1) % limits.size()];
        if (list.setting("Objects per player", mp.object_limit ? std::to_string(mp.object_limit) : std::string("No limit"), true))
            send(h, "object-limit", next ? std::to_string(next) : std::string("off"));
    }
    if (list.setting("Delete all guest objects", {}, true)) send(h, "clear-objects");
    list.title("WHAT GUESTS MAY USE");
    if (list.setting("Guest noclip", allowed(mp.guest_noclip), true)) send(h, "noclip-allow", mp.guest_noclip ? "off" : "on");
    if (list.setting("Guest No Bail", allowed(mp.guest_no_bail), true)) send(h, "nobail-allow", mp.guest_no_bail ? "off" : "on");
    if (list.setting("Guest boosts", allowed(mp.guest_boosts), true)) send(h, "boosts-allow", mp.guest_boosts ? "off" : "on");
    list.title("THE WORLD");
    if (list.setting("Physics tuning", mp.enforce_tuning ? (mp.dedicated ? "Game's" : "Host's") : "Everyone's own", true)) send(h, "tuning-enforce", mp.enforce_tuning ? "off" : "on");
    if (list.setting("World layer sync", mp.force_world_layers ? "On" : "Off", true)) send(h, "world-layer-sync", mp.force_world_layers ? "off" : "on");
    list.note(!h.notice.empty() ? h.notice : mp.status);
    list.end();
}

// ---------------------------------------------------------------- ignored players

// Everyone the player ignores, here or not, to stop ignoring; then the players here, to ignore one.
void ignored_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    Column list(ui, hash("ignored-players"), a, ImVec2(a.x + std::min(z.x - a.x, 2100 * ui.u), z.y));
    list.heading("IGNORED PLAYERS");
    list.note("Only for you: their skater, voice, chat, invites and plugins never show or are heard on your game, in any session. They are not told.");
    if (mp.ignored.empty()) list.note("You ignore nobody.");
    for (const auto &player : mp.ignored) {
        const bool here = std::any_of(mp.roster.begin(), mp.roster.end(), [&](const auto &p) { return p.id == player.id && p.connected; });
        const auto name = player.name.empty() ? std::to_string(player.id) : player.name;
        if (list.setting(view::caption(name, 40) + (here ? "  (here)" : ""), "UNIGNORE", true)) send(h, "ignore", std::to_string(player.id) + " off");
    }
    bool titled{};
    for (const auto &peer : mp.roster) {
        if (peer.id == mp.local_id || !peer.connected || peer.ignored || (mp.dedicated && peer.id == mp.host_id)) continue;
        if (!std::exchange(titled, true)) list.title("SKATERS HERE");
        if (list.setting(view::caption(peer.name, 40), "IGNORE", true)) send(h, "ignore", std::to_string(peer.id) + " on");
    }
    list.note(!h.notice.empty() ? h.notice : "/ignore <player> and /unignore <player> in chat do the same.");
    list.end();
}

// ---------------------------------------------------------------- focus

// Moves the focus one control over, by where the controls were drawn last frame.
#include "mod_manager_page.inl"

// `in_list`: up and down from a control in a scrolling list stay in that list while it has anything
// that way (the Mod Manager's rows, whose next row up can be out of view and further than the bar
// over the list).
void move_focus(Hub &h, int dx, int dy, bool in_list = false) {
    const Item *from{};
    for (const auto &entry : h.last)
        if (entry.id == h.focus) from = &entry;
    if (!from) {
        if (!h.last.empty()) h.focus = h.last.front().id;
        return;
    }
    const ImVec2 centre((from->a.x + from->b.x) * .5f, (from->a.y + from->b.y) * .5f);
    const Item *best{};
    float best_score = FLT_MAX;
    const auto same_list = [&](const Item &entry) { return from->list && entry.list == from->list; };
    bool list_only{};
    if (in_list && dy && from->list)
        for (const auto &entry : h.last)
            if (entry.id != from->id && same_list(entry) && (dy > 0 ? entry.a.y - from->b.y : from->a.y - entry.b.y) >= -2) list_only = true;
    for (const auto &entry : h.last) {
        if (entry.id == from->id || (list_only && !same_list(entry))) continue;
        // Measured from the nearest point of the other control, so a wide row is as close as it looks.
        const ImVec2 closest(std::clamp(centre.x, entry.a.x, entry.b.x), std::clamp(centre.y, entry.a.y, entry.b.y));
        const float along = dx ? (closest.x - centre.x) * static_cast<float>(dx) : (closest.y - centre.y) * static_cast<float>(dy);
        const float across = dx ? std::abs(closest.y - centre.y) : std::abs(closest.x - centre.x);
        const float edge = dx ? (dx > 0 ? entry.a.x - from->b.x : from->a.x - entry.b.x) : (dy > 0 ? entry.a.y - from->b.y : from->a.y - entry.b.y);
        if (along <= 0 || edge < -2) continue;
        const float score = along + across * 2.5f;
        if (score < best_score) best_score = score, best = &entry;
    }
    if (!best) return;
    h.focus = best->id;
    // Into a list from outside it: back to the row it was left on, if that row is still there.
    if (best->list && best->list != from->list)
        if (const auto kept = h.list_focus.find(best->list); kept != h.list_focus.end())
            for (const auto &entry : h.last)
                if (entry.id == kept->second && entry.list == best->list) h.focus = entry.id;
}
} // namespace

bool hub_page_pending() {
    auto &h = hub();
    auto &s = state();
    const auto now = Clock::now();
    if (now >= h.next_page) {
        h.next_page = now + 40ms;
        h.page = {};
        if (const auto feed = page_feed.load()) {
            try { h.page = feed(); } catch (...) {}
        }
        if (!h.page.visible) h.dismissed = false;
    }
    // B on the front page over the HUB dismisses it at once, taking for granted that the pause menu
    // closes. When the HUB is still there a moment later, B did something else of the game's (it took
    // a page opened from the HUB away, 2026-11-04 in game, and the front page stayed gone over the
    // empty HUB until the menu was opened again): the front page comes back.
    if (h.dismissed && h.page.visible && h.page.section == hub_home_section && now - h.dismissed_at > 1500ms) {
        h.dismissed = false;
        logging::log(logging::Level::info, logging::Channel::ui, "Front page: Back left the pause menu open; drawn again.");
    }
    const bool shown = h.page.visible && !h.dismissed;
    // The Mod Manager's changes that are not applied are asked about once the pause menu has closed
    // (not when another of its pages comes up), if it was open this time.
    if (auto &mods = mod_page(); mods.visited && !shown && !dingosdk::multiplayer::native_pause_menu_open()) {
        mods.visited = false;
        offer_apply(mods);
    }
    if (shown != h.shown) {
        h.shown = shown;
        h.next_model = {};
        h.searched = false;
        h.last.clear();
        if (!shown) {
            dingosdk::multiplayer::hold_native_menu_back(false);
            s.hub_nav.store(false);
            if (!mod_page().leaving) mod_page().declined = false;
            h.recording_bind = 0;
            set_typing(h, 0);
            wipe(h.password), wipe(h.code_password), wipe(h.host_password);
            h.notice.clear();
        } else {
            // What is held as the page comes up is not a press on it: the Back that closed the
            // game's side menu over it would dismiss the page as well.
            h.keys_before.fill(true);
            h.pad_before = ~0U;
        }
    }
    // With the overlay's own menu or console open the page stays where it is, behind them, and
    // takes no input: they have the pointer then.
    const bool others = s.visible.load() || s.console_visible.load() || s.editor_visible.load() || s.chat_visible.load();
    s.hub_pointer.store(shown && !others);
    if (shown && now >= h.next_model) {
        h.next_model = now + 200ms;
        {
            std::lock_guard lock(hub_callbacks_mutex);
            h.callbacks = hub_callbacks;
        }
        if (h.callbacks.read_model) {
            try {
                h.callbacks.read_model(h.callbacks.user, h.full);
                h.model = h.full.multiplayer;
            } catch (...) { h.full = {}, h.model = {}; }
            h.rows_stale = true;
        }
    }
    return shown;
}

// One frame of the pages, in `a`..`z` with their tabs at `tabs_y`, in the game's pause menu (draw_hub_page).
void hub_frame(Hub &h, ImDrawList *draw, float u, ImVec2 a, ImVec2 z, float tabs_y, bool input) {
    auto &s = state();
    auto &io = ImGui::GetIO();
    Ui ui{h, draw, s.menu.bold ? s.menu.bold : ImGui::GetFont(), s.menu.body ? s.menu.body : ImGui::GetFont(), nullptr, u, io.MousePos,
          input, false, false, false, 0, h.model, {}};
    ui.heading = s.menu.heading ? s.menu.heading : ui.bold;
    ui.clicked = ui.input && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    ui.doubled = ui.input && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ui.moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    ui.wheel = ui.input ? io.MouseWheel : 0;
    const auto &mp = h.model;
    if (h.rows_stale) rebuild(h);
    if (h.notice_until && GetTickCount64() >= h.notice_until) h.notice.clear(), h.notice_until = 0;
    // Going into a session shows it; leaving one goes back to the browser.
    // In a session, as far as the tabs go: not one that is still waiting on a map being fetched,
    // which the player may yet turn down.
    const bool in_session = mp.active && !mp.lobby_joining && !mp.map_fetching;
    // A join under way (connecting, the host's map loading): a card over the page says so until
    // the player is in, or calls it off. A map that has to be fetched first has a card of its
    // own (map_download_card.cpp), which takes this one's place.
    const bool connecting = !mp.hosting && !mp.echo && mp.lobby_joining && !mp.map_fetching;
    // A join is followed to its end. One that ends with the player not in, and not by their own
    // choice (the card's CANCEL, or a map they were asked about: its card says its own piece),
    // says why. One asked for here that never shows in the model was turned down at once.
    if (!mp.hosting && !mp.echo && (mp.lobby_joining || mp.map_fetching)) {
        if (!h.join_seen) h.failed.clear();
        h.join_seen = true;
        h.join_fetching = mp.map_fetching;
        h.join_asked = 0;
    } else if (mp.active) {
        h.join_seen = h.join_cancelled = false;
        h.join_asked = 0;
    } else if (h.join_seen || (h.join_asked && GetTickCount64() - h.join_asked > 2000)) {
        if (!h.join_cancelled && !(h.join_seen && h.join_fetching)) {
            const bool said = !mp.status.empty() && mp.status != MultiplayerModel{}.status;
            h.failed = said ? mp.status : std::string("The session did not answer. It may be full, closed or on another version of ReSkate.");
            h.failed_name = !h.joining_name.empty() ? h.joining_name : view::caption(mp.lobby_name, 40);
            h.notice.clear(), h.notice_until = 0;
            h.focus = 0;
        }
        h.join_seen = h.join_cancelled = false;
        h.join_asked = 0;
    }
    if (in_session != h.was_active) {
        h.was_active = in_session;
        h.tab = in_session ? Tab::session : Tab::browser;
        h.selected_player.clear();
    }
    if ((h.tab == Tab::session || h.tab == Tab::voice) && !in_session) h.tab = Tab::browser;
    const bool runs_session = in_session && (mp.hosting || mp.server_admin);
    if (h.tab == Tab::settings && !runs_session) h.tab = in_session ? Tab::session : Tab::browser;

    // ---- the controller and the keys, watched while the game has them
    if (ui.input) DingoSDKOverlayReadControllerInput(&ui.pad, true);
    if (!ui.pad.available) ui.pad.buttons = 0;
    const std::uint32_t pressed = ui.pad.buttons & ~h.pad_before;
    std::array<bool, 8> keys{};
    if (ui.input && !h.typing) {
        OverlayInputAccess access;
        constexpr std::array<int, 8> codes{VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, 'Z', 'C', VK_ESCAPE};
        for (std::size_t i = 0; i < codes.size(); ++i) keys[i] = (GetAsyncKeyState(codes[i]) & 0x8000) != 0;
    }
    const auto key_new = [&](std::size_t i) { return keys[i] && !h.keys_before[i]; };
    const bool typing_before = h.typing != 0;
    h.fire = 0;
    // Moving the mouse puts the pointer in charge: the D-pad's highlight goes, and comes back
    // with the D-pad.
    if (ui.moved && !h.typing) h.ring = false;
    h.focus_moved = false;
    int previous_tab{}, next_tab{};
    // On the Mod Manager, a mod's page holds the menu's Back (hold_native_menu_back): B and Escape go
    // back to the list there, and keep holding it until they are let go.
    auto &mods_page = mod_page();
    const bool on_mods = h.page.section == 2;
    const bool back_down = keys[7] || (ui.pad.buttons & 0x2000);
    if (ui.input && !typing_before) {
        // Back closes the game's menu, which takes a moment to say so: this goes at once.
        if (key_new(7) || (pressed & 0x2000)) {
            if (on_mods && mods_hold_back(mods_page)) mods_back(h, mods_page), mods_page.hold_back_until = GetTickCount64() + 300, h.sound = sound_select;
            else if (on_mods && guard_leave(mods_page)) offer_leave(mods_page), h.sound = sound_select;
            else if (!on_mods || GetTickCount64() >= mods_page.hold_back_until) h.dismissed = true;
        }
        const std::array<bool, 4> held{keys[0] || (ui.pad.buttons & 1), keys[1] || (ui.pad.buttons & 2), keys[2] || (ui.pad.buttons & 4), keys[3] || (ui.pad.buttons & 8)};
        const std::array<bool, 4> fresh{key_new(0) || (pressed & 1), key_new(1) || (pressed & 2), key_new(2) || (pressed & 4), key_new(3) || (pressed & 8)};
        const auto tick = GetTickCount64();
        int direction = -1;
        for (int i = 0; i < 4; ++i)
            if (fresh[static_cast<std::size_t>(i)]) direction = i, h.repeat_at = tick + 380;
        if (direction < 0 && tick >= h.repeat_at)
            for (int i = 0; i < 4; ++i)
                if (held[static_cast<std::size_t>(i)]) direction = i, h.repeat_at = tick + 70;
        // A plugin's page on the Mod Manager has ImGui's own focus: the D-pad, the arrows and A are
        // its navigation's while it shows (State::hub_nav), and B still the page's way back.
        const bool page_nav = on_mods && mods_imgui_nav(mods_page);
        if (direction >= 0 && !page_nav && (direction == 2 || direction == 3) && h.ring &&
            options_take_step(h.focus, direction == 2 ? -1 : 1)) {
            // (Held, a little slower than the focus moves: each step waits on the game.)
            if (!fresh[static_cast<std::size_t>(direction)]) h.repeat_at = tick + 160;
            direction = -1;
        }
        if (direction >= 0 && !page_nav) {
            // The first press only shows where the focus is.
            if (h.ring || !h.focus) move_focus(h, direction == 2 ? -1 : direction == 3 ? 1 : 0, direction == 0 ? -1 : direction == 1 ? 1 : 0, on_mods);
            h.ring = true;
            h.focus_moved = true;
        }
        if (!page_nav && (key_new(4) || (pressed & 0x1000))) {
            if (h.focus && std::any_of(h.last.begin(), h.last.end(), [&](const Item &entry) { return entry.id == h.focus; })) h.fire = h.focus, h.ring = true;
        }
        // (And LT and RT are its own tabs', as on the Insert menu: B goes back to the list first.)
        previous_tab = !page_nav && (key_new(5) || (pressed & 0x10000));
        next_tab = !page_nav && (key_new(6) || (pressed & 0x20000));
    }
    if (back_down && GetTickCount64() < mods_page.hold_back_until) mods_page.hold_back_until = GetTickCount64() + 300;
    // The answer to leaving with changes not applied: LATER (or an apply that reloads nothing) lets go
    // of Back, and a moment later presses it for the player, on the pad only the game reads.
    if (const int answer = mods_page.leave_answer.exchange(0)) {
        mods_page.leaving = false;
        mods_page.declined = true;
        mods_page.visited = false; // (asked already)
        const bool reloads = answer == 1 && mods_page.mods && mods_page.mods->effect != live_mods::ApplyEffect::maps_only;
        if (!reloads) mods_page.close_at = GetTickCount64() + 250;
    }
    if (mods_page.close_at && GetTickCount64() >= mods_page.close_at) {
        mods_page.close_at = 0;
        if (dingosdk::multiplayer::native_pause_menu_open()) {
            XINPUT_GAMEPAD back{}, neutral{};
            back.wButtons = XINPUT_GAMEPAD_B;
            overlay::play_pad({{0, back}, {6, neutral}, {12, neutral}});
        }
    }
    dingosdk::multiplayer::hold_native_menu_back(on_mods && (mods_hold_back(mods_page) || guard_leave(mods_page) || GetTickCount64() < mods_page.hold_back_until));
    h.keys_before = keys;
    // A key that ended the typing (Enter, Escape) is still down: it is not a fresh press of anything.
    // And so is one held while the game was not the window in front.
    if (typing_before || !ui.input) h.keys_before.fill(true);
    if (!ui.input) h.pad_before = ~0U;
    h.items.clear();

    // ---- the page's tabs
    const bool tools = h.page.section == 1;
    const bool mods = h.page.section == 2;
    {
        // (Each page keeps the tab it was left on.)
        std::vector<std::pair<int, const char *>> tabs;
        int multiplayer_tab = static_cast<int>(h.tab);
        int &open = tools ? h.tools_tab : mods ? mod_page().tab : multiplayer_tab;
        if (mods) {
            tabs = mod_tabs();
        } else if (tools) {
            if (h.tools_tab == custom_tab) h.tools_tab = official_tab;
            tabs = {{official_tab, "MAPS"}, {world_tab, "WORLD"}, {parks_tab, "PARKS"}, {skater_tab, "SKATER"},
                    {camera_tab, "CAMERA"}, {style_tab, "STYLE"}, {online_tab, "ONLINE"}, {settings_tab, "SETTINGS"}};
        } else {
            tabs = {{static_cast<int>(Tab::browser), "SERVER BROWSER"}, {static_cast<int>(Tab::friends), "FRIENDS"}, {static_cast<int>(Tab::host), "HOST LOBBY"}};
            // The session and its voice chat are only there in one.
            if (in_session) tabs.emplace_back(static_cast<int>(Tab::session), "CURRENT SESSION"), tabs.emplace_back(static_cast<int>(Tab::voice), "VOICE CHAT");
            // And its settings only for whoever runs it.
            if (runs_session) tabs.emplace_back(static_cast<int>(Tab::settings), "SESSION SETTINGS");
            tabs.emplace_back(static_cast<int>(Tab::ignored), "IGNORED PLAYERS");
        }
        if (previous_tab || next_tab) {
            std::size_t at = 0;
            for (std::size_t i = 0; i < tabs.size(); ++i)
                if (tabs[i].first == open) at = i;
            at = (at + tabs.size() + (next_tab ? 1 : 0) - (previous_tab ? 1 : 0)) % tabs.size();
            open = tabs[at].first;
            h.focus = 0;
            h.sound = sound_tab;
            set_typing(h, 0);
        }
        // Every page's strip as the Insert menu's segmented tabs: each tab on its own near-black tile,
        // so it reads over a bright part of the world as well as a dark one (bare grey names over the
        // world could not be read on a light background), the open tab white with dark ink, the one
        // under the pointer the kit's lighter grey, and the one under the D-pad or the keys a light
        // tile (the open one, already white, a blue line under it). The strip is as wide as its tabs,
        // with LT and RT (Z and C) at its ends, each on a tile of its own too.
        const float top = tabs_y, bottom = top + tabs_height * u, size = 44 * u, segment_gap = 8 * u;
        float x = a.x;
        const auto cap = [&](std::uint32_t pad_button, const char *key) {
            const auto name = ui.pad.available ? controller_combo_label(pad_button, ui.pad.style) : std::string(key);
            const float scale = u * 2.2f, pad = 18 * u;
            const float w = ui.bold->CalcTextSizeA(12 * scale, FLT_MAX, 0, name.c_str()).x + 12 * scale;
            theme::tile_rect(ui.draw, ImVec2(x, top), ImVec2(x + w + pad * 2, bottom), tile_dark, hash(key) & 0xfff, u * 2);
            theme::keycap(ui.draw, ui.bold, scale, ImVec2(x + pad, top + (bottom - top - 20 * scale) * .5f), name.c_str(), "");
            x += w + pad * 2 + segment_gap;
        };
        cap(0x10000, "Z");
        for (const auto &[tab, name] : tabs) {
            const float w = width_of(ui.bold, size, name) + 56 * u;
            const ImVec2 p(x, top), q(x + w, bottom);
            const Hit hit = item(ui, hash(name, 1), p, q);
            const bool on = open == tab;
            const auto [focused, pointed] = lit_of(hit);
            const ImU32 fill = focused ? theme::focus_fill : on ? primary_rest : pointed ? row_hover : tile_dark;
            const ImU32 ink = on || focused ? focus_ink : pointed ? theme::paper : chip_ink;
            theme::tile_rect(ui.draw, p, q, fill, hash(name) & 0xfff, u * 2);
            ui.draw->AddText(ui.bold, size, ImVec2(p.x + 28 * u, top + (bottom - top - size) * .5f), ink, name);
            // The open tab's brush line, cut a little crooked as skate.'s is.
            if (on) ui.draw->AddQuadFilled(ImVec2(p.x + 16 * u, bottom - 2 * u), ImVec2(q.x - 12 * u, bottom - 6 * u), ImVec2(q.x - 16 * u, bottom + 6 * u),
                                           ImVec2(p.x + 20 * u, bottom + 9 * u), accent);
            if (hit.press && !on) open = tab, h.sound = sound_tab, set_typing(h, 0);
            x = q.x + segment_gap;
        }
        cap(0x20000, "C");
        if (!tools && !mods) h.tab = static_cast<Tab>(multiplayer_tab);
    }

    // Under the connecting card the page is still drawn, to look at and not to use.
    const bool page_input = ui.input, page_clicked = ui.clicked, page_doubled = ui.doubled;
    const bool failed = !connecting && !h.failed.empty() && !in_session;
    if ((connecting || failed) && !tools && !mods) ui.input = ui.clicked = ui.doubled = false;
    if (tools) tools_page(ui, a, z);
    else if (mods) mod_manager_page(ui, a, z);
    else switch (h.tab) {
    case Tab::browser: browser_page(ui, a, z); break;
    case Tab::friends: friends_page(ui, a, z); break;
    case Tab::host: host_page(ui, a, z); break;
    case Tab::session: session_page(ui, a, z); break;
    case Tab::voice: voice_page(ui, a, z); break;
    case Tab::settings: settings_page(ui, a, z); break;
    case Tab::ignored: ignored_page(ui, a, z); break;
    }
    if ((connecting || failed) && !tools && !mods) {
        ui.input = page_input, ui.clicked = page_clicked, ui.doubled = page_doubled;
        h.items.clear();   // nothing of the page takes the focus: only the card's way out
        if (connecting) connecting_page(ui, a, z);
        else failed_page(ui, a, z);
    }
    if (ui.input) h.pad_before = ui.pad.buttons;
    s.hub_nav.store(mods && ui.input && mods_imgui_nav(mod_page()));
    // A press that found nothing to land on is forgotten; the focus stays on a control that is
    // still there, or goes to the first one.
    if (h.focus_next) {
        // Asked for this frame, drawn from the next: not there to find yet.
        h.focus = std::exchange(h.focus_next, 0);
        h.ring = h.focus_moved = true;
        h.last = h.items;
        return;
    }
    if (h.focus && std::none_of(h.items.begin(), h.items.end(), [&](const Item &entry) { return entry.id == h.focus; })) h.focus = 0;
    if (!h.focus && h.ring && !h.items.empty()) {
        // Past the tabs: into the page itself.
        const auto first = std::find_if(h.items.begin(), h.items.end(), [&](const Item &entry) { return entry.a.y >= a.y - 1; });
        h.focus = first != h.items.end() ? first->id : h.items.front().id;
    }
    h.last = h.items;
    // The game's sounds, as its own menus make them: moving the focus, pressing, changing tab.
    if (h.sound == ~0U && h.focus && h.focus != h.focus_heard && ui.input) h.sound = sound_navigate;
    h.focus_heard = h.focus;
    if (const auto wanted = std::exchange(h.sound, ~0U); wanted != ~0U && ui.input) {
        const auto tick = GetTickCount64();
        // (Sweeping the pointer down a list is not a sound for every row.)
        if (const auto play = ui_sound.load(); play && (wanted != sound_navigate || tick - h.sound_at >= 45)) play(wanted), h.sound_at = tick;
    }
}

// The saved choices, read every 2 s (the profile loads after the overlay starts) and passed to the
// pause menu's code; a change from a menu goes at once.
namespace {
bool choice_on(std::string_view key) {
    const auto saved = dingosdk::profile_runtime::local_value(key);
    return !saved || !saved->is_boolean() || saved->get<bool>();
}
void pass_pause_choices() {
    static ULONGLONG next{};
    const auto now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;
    dingosdk::multiplayer::set_hub_front_page(choice_on("HubFrontPage"));
    static int backdrop = -1;
    if (const int on = choice_on("ModManagerBackdrop") ? 1 : 0; on != backdrop)
        backdrop = on, (void)dingosdk::multiplayer::set_mod_backdrop(on ? "store" : "off");
}
}
bool hub_front_page_on() { return choice_on("HubFrontPage"); }
void set_hub_front_page_on(bool on) {
    dingosdk::profile_runtime::set_local_preference("HubFrontPage", on);
    dingosdk::multiplayer::set_hub_front_page(on);
}
bool mod_backdrop_on() { return choice_on("ModManagerBackdrop"); }
void set_mod_backdrop_on(bool on) {
    dingosdk::profile_runtime::set_local_preference("ModManagerBackdrop", on);
    (void)dingosdk::multiplayer::set_mod_backdrop(on ? "store" : "off");
}

void draw_hub_page() {
    pass_pause_choices();
    auto &h = hub();
    if (!h.shown) return;
    auto &s = state();
    const auto display = ImGui::GetIO().DisplaySize;
    // One unit of the game's menu in pixels, and where its page sits in this window.
    const float u = std::min(display.x / layout_width, display.y / layout_height);
    const float across = display.x / u, down = display.y / u, canvas = std::min(across, layout_widest);
    const float page_left = ((across - canvas) * .5f + side_margin) * u, page_right = ((across + canvas) * .5f - side_margin) * u;
    const float lower = (down - layout_height) * .05f; // how far a taller window moves the page down
    const bool others = s.visible.load() || s.console_visible.load() || s.editor_visible.load() || s.chat_visible.load();
    const ImVec2 a(page_left, (body_top + lower) * u);
    const ImVec2 z(std::min(page_right, a.x + body_most * u), (down - (layout_height - body_bottom) - lower) * u);
    // (A card that asks something over the page has the input while it asks.)
    const bool input = !others && !s.prompt_input_active.load(std::memory_order_relaxed) && game_window_foreground(s.window.load());
    // The game's own HUB tab: ReSkate's front page over it (hub_home_page.cpp). It has no tabs, so
    // it starts where they would.
    if (h.page.section == hub_home_section) {
        if (draw_home_page(ImGui::GetBackgroundDrawList(), u, ImVec2(a.x, (tabs_top + lower) * u), z, input, h.full)) h.dismissed = true, h.dismissed_at = Clock::now();
        return;
    }
    hub_frame(h, ImGui::GetBackgroundDrawList(), u, a, z, (tabs_top + lower) * u, input);
}

bool online_embedded_typing() { return hub().typing != 0; }

void show_friends_tab() {
    auto &h = hub();
    h.tab = Tab::friends;
    h.focus = 0;
}
void show_mod_updates() {
    auto &m = mod_page();
    m.tab = updates_tab;
    m.details.clear();
    hub().focus = 0;
}

// Mods being applied with the pause menu closed (most often after the Mod Manager's question): a
// small card at the right edge, out of the way of skating, as the map download's is, then a few
// seconds of how it went.
namespace {
bool apply_card_seen{};
ULONGLONG apply_card_until{};
std::string apply_card_result;
}
bool mod_apply_card_pending() {
    const bool applying = live_mods::busy() && dingosdk::map_download::view().stage == dingosdk::map_download::Stage::idle;
    const bool menu = hub().shown || dingosdk::multiplayer::native_pause_menu_open();
    if (applying && !menu) apply_card_seen = true, apply_card_until = 0;
    else if (apply_card_seen && !applying) {
        apply_card_seen = false;
        apply_card_until = GetTickCount64() + 4000;
        apply_card_result = live_mods::status();
    }
    return (applying && !menu) || GetTickCount64() < apply_card_until;
}
void draw_mod_apply_card() {
    auto &s = state();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto *body = s.menu.body ? s.menu.body : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / layout_width, display.y / layout_height);
    const bool applying = live_mods::busy();
    const auto progress = live_mods::progress();
    const float width = 820 * u, inset = 30 * u, bar = 12 * u;
    const float height = inset + 24 * u + 12 * u + 38 * u + 18 * u + bar + 14 * u + 26 * u + inset;
    const ImVec2 min(std::floor(display.x - 48 * u - width), std::floor(display.y * 0.2f)), max(min.x + width, min.y + height);
    auto *draw = ImGui::GetForegroundDrawList();
    theme::tile_rect(draw, min, max, IM_COL32(26, 26, 26, 235), 0x4d3, u * 2);
    draw->AddRectFilled(min, ImVec2(min.x + 8 * u, max.y), applying ? theme::blue : skate_theme::good);
    const float left = min.x + inset + 8 * u, right = max.x - inset;
    float y = min.y + inset;
    draw->AddText(bold, 24 * u, ImVec2(left, y), theme::blue, "MOD MANAGER");
    y += 36 * u;
    draw->AddText(bold, 38 * u, ImVec2(left, y), theme::paper, applying ? "APPLYING MODS" : "MODS APPLIED");
    y += 56 * u;
    const float fraction = !applying ? 1.f : progress.total ? static_cast<float>(progress.done) / static_cast<float>(progress.total) : -1.f;
    draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + bar), skate_theme::tile_light, 6 * u);
    if (fraction >= 0) draw->AddRectFilled(ImVec2(left, y), ImVec2(left + (right - left) * std::clamp(fraction, 0.f, 1.f), y + bar), applying ? theme::blue : skate_theme::good, 6 * u);
    else {
        const float t = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0)) / 2.f, w = (right - left) * .25f;
        const float x = left + (right - left - w) * (t < .5f ? t * 2 : 2 - t * 2);
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + bar), theme::blue, 6 * u);
    }
    y += bar + 14 * u;
    const std::string line = applying ? (progress.step.empty() ? std::string("Merging the mods into the game...") : progress.step)
                                      : (apply_card_result.empty() ? std::string("Done.") : apply_card_result);
    label(draw, body, 26 * u, ImVec2(left, y), right, theme::muted, line);
}
} // namespace dingosdk::overlay::detail
