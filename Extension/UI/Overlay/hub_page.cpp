#include "overlay_internal.h"
#include "skate_style.h"
#include "cursor.h"
#include "input_capture.h"
#include "Extension/UI/NativeMenu/native_menu_view.h"
#include "Extension/UI/NativeMenu/native_tools_view.h"
#include "Extension/Boot/discord_presence.h"
#include "Extension/Music/local_music_playback.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/Input/voice_input.h"
#include "Engine/Game/Multiplayer/object_placement.h"
#include "Engine/Game/Multiplayer/session_limits.h"
#include "Engine/Game/Multiplayer/tick_settings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
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
// 16:9, 21:9, 32:9, 11:7 and a window taller than wide.)
constexpr float layout_width = 3840.f, layout_height = 2160.f, layout_widest = 4704.f, side_margin = 192.f, tabs_top = 572.f,
                tabs_height = 84.f, body_top = 770.f, body_bottom = 1905.f, body_most = 4400.f;
constexpr ImU32 sticky = IM_COL32(236, 215, 108, 255), sticky_hover = IM_COL32(246, 228, 132, 255),
                sticky_off = IM_COL32(120, 114, 80, 255), row_hover = IM_COL32(52, 53, 57, 240),
                hint_ink = IM_COL32(110, 110, 110, 255), soft = IM_COL32(210, 210, 214, 255);

enum class Tab { browser, host, session, voice, settings };
// The game's own menu sounds, by their numbers in its list (UI/NativeMenu/ui_sound.h).
constexpr std::uint32_t sound_navigate = 3, sound_select = 7, sound_tab = 56;
enum class Kind { all, servers, lobbies };
enum class Look { tile, sticky, plain, chosen };

constexpr std::uint32_t hash(std::string_view text, std::uint64_t salt = 0) {
    std::uint32_t value = 2166136261U ^ static_cast<std::uint32_t>(salt * 2654435761ULL);
    for (const char c : text) value = (value ^ static_cast<unsigned char>(c)) * 16777619U;
    return value ? value : 1;
}

struct Row {
    const MultiplayerLobby *lobby{};
    std::string map;
    bool joinable{}, here{}, full{}, own{};
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
    view::Sort sort{view::Sort::players};
    std::uint64_t selected{};
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
    hit.hover = ui.input && inside(ui.mouse, a, b) && (!clip || inside(ui.mouse, clip->a, clip->b));
    if (hit.hover && ui.moved && !h.typing) h.focus = id, h.ring = false;
    hit.focus = h.focus == id && (h.ring || hit.hover);
    hit.press = ui.input && ((hit.hover && ui.clicked) || h.fire == id);
    if (hit.press) h.focus = id, h.sound = sound_select;
    return hit;
}

// A button in one of the game's looks. `enabled` off: drawn dim, and not a control at all.
bool button(Ui &ui, std::uint32_t id, ImVec2 a, ImVec2 b, const std::string &text, Look look = Look::tile, bool enabled = true,
            const std::string &value = {}, const Item *clip = nullptr) {
    const float u = ui.u, height = b.y - a.y, size = std::min(40 * u, height * .42f);
    const Hit hit = enabled ? item(ui, id, a, b, clip) : Hit{};
    const bool lit = hit.focus || hit.hover;
    ImU32 fill = skate_theme::tile, ink = enabled ? theme::paper : theme::muted;
    if (look == Look::sticky) fill = !enabled ? sticky_off : lit ? sticky_hover : sticky, ink = skate_theme::black;
    else if (lit) fill = theme::blue;
    else if (look == Look::plain) fill = skate_theme::tile_light;
    else if (look == Look::chosen) fill = theme::paper, ink = skate_theme::black;
    skate_theme::rough_rect(ui.draw, a, b, fill, id & 0xfff, u * 2);
    if (look == Look::sticky && hit.focus) ui.draw->AddRect(a, b, theme::blue, 0, 0, 6 * u);
    if (value.empty()) {
        ui.draw->AddText(ui.bold, size, ImVec2(a.x + (b.x - a.x - width_of(ui.bold, size, text)) * .5f, a.y + (height - size) * .5f), ink, text.c_str());
    } else {
        // A setting: its name on the left, what it is now on the right.
        const float w = width_of(ui.bold, size, value);
        label(ui.draw, ui.body, size, ImVec2(a.x + 30 * u, a.y + (height - size) * .5f), b.x - w - 50 * u, ink, text);
        ui.draw->AddText(ui.bold, size, ImVec2(b.x - w - 30 * u, a.y + (height - size) * .5f), enabled && !lit && look != Look::chosen ? theme::blue : ink, value.c_str());
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
bool field(Ui &ui, std::uint32_t id, ImVec2 a, ImVec2 b, std::string &text, const char *hint, bool secret = false, std::size_t limit = 64) {
    auto &h = ui.h;
    const float u = ui.u, size = 40 * u;
    const Hit hit = item(ui, id, a, b);
    bool typing = h.typing == id, entered{};
    if (hit.press && !typing) set_typing(h, id), typing = true;
    else if (typing && ui.clicked && !hit.hover) set_typing(h, 0), typing = false;
    if (typing) {
        take_typing(text, limit);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) set_typing(h, 0), typing = false;
        else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) set_typing(h, 0), typing = false, entered = true;
    }
    skate_theme::rough_rect(ui.draw, a, b, theme::paper, id & 0xfff, u * 2);
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

void rebuild(Hub &h) {
    h.rows_stale = false;
    h.rows.clear();
    const auto &mp = h.model;
    const std::span<const Level> levels = h.full.levels;
    const auto found = view::browse(mp, {h.search, h.same_map, h.sort}, levels);
    h.listed = mp.lobbies.size();
    h.skaters = 0;
    for (const auto &lobby : mp.lobbies) h.skaters += std::max(0, lobby.players);
    for (const auto *lobby : found.lobbies) {
        const bool full = lobby->players >= lobby->capacity;
        if ((h.kind == Kind::servers && !lobby->dedicated) || (h.kind == Kind::lobbies && lobby->dedicated) ||
            (h.friends_only && lobby->friends.empty()) || (h.hide_full && full))
            continue;
        h.rows.push_back({lobby, view::caption(view::map_name(lobby->map, levels), 40), view::can_join(mp, *lobby),
                          mp.active && lobby->id == mp.public_lobby, full, lobby->owner == mp.local_id});
    }
    if (std::none_of(h.rows.begin(), h.rows.end(), [&](const Row &row) { return row.lobby->id == h.selected; }))
        h.selected = h.rows.empty() ? 0 : h.rows.front().lobby->id;
}
const Row *selected_row(const Hub &h) {
    for (const auto &row : h.rows)
        if (row.lobby->id == h.selected) return &row;
    return nullptr;
}
void join(Hub &h, const Row &row) {
    if (!row.joinable) return;
    if (row.lobby->password_required && h.password.empty()) {
        h.focus_next = hash("password");
        return say(h, "Enter the password, then join.");
    }
    if (send(h, "join-lobby", std::to_string(row.lobby->id), row.lobby->password_required ? h.password : std::string())) {
        say(h, "Joining " + view::caption(row.lobby->name, 40) + "...");
        wipe(h.password);
    }
}

void browser_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    const float u = ui.u, gap = 28 * u, bar = 96 * u, list_width = (z.x - a.x) * .62f;
    auto *draw = ui.draw;
    if (!h.searched && !mp.lobby_searching) h.searched = true, queue_multiplayer_action("browse", "");
    bool changed{};
    // ---- the bar: search, filters, refresh
    {
        const float search_width = std::min(1000 * u, list_width * .4f);
        const auto before = h.search;
        field(ui, hash("search"), a, ImVec2(a.x + search_width, a.y + bar), h.search, "Search name or map");
        changed |= before != h.search;
        float x = a.x + search_width + gap;
        const auto chip = [&](const char *text, bool on) {
            const float size = 34 * u, w = width_of(ui.bold, size, text) + 44 * u;
            const ImVec2 c(x, a.y + 10 * u), d(x + w, a.y + bar - 10 * u);
            const Hit hit = item(ui, hash(text, 7), c, d);
            const bool lit = hit.focus || hit.hover;
            skate_theme::rough_rect(draw, c, d, lit ? theme::blue : on ? theme::paper : skate_theme::tile, hash(text) & 0xfff, u * 2);
            draw->AddText(ui.bold, size, ImVec2(c.x + 22 * u, c.y + (d.y - c.y - size) * .5f), on && !lit ? skate_theme::black : theme::paper, text);
            x = d.x + 12 * u;
            return hit.press;
        };
        if (chip("ALL", h.kind == Kind::all)) h.kind = Kind::all, changed = true;
        if (chip("SERVERS", h.kind == Kind::servers)) h.kind = Kind::servers, changed = true;
        if (chip("LOBBIES", h.kind == Kind::lobbies)) h.kind = Kind::lobbies, changed = true;
        x += 16 * u;
        if (chip("FRIENDS", h.friends_only)) h.friends_only = !h.friends_only, changed = true;
        if (chip("HIDE FULL", h.hide_full)) h.hide_full = !h.hide_full, changed = true;
        if (chip("MY MAP", h.same_map)) h.same_map = !h.same_map, changed = true;
        if (button(ui, hash("refresh"), ImVec2(z.x - 420 * u, a.y), ImVec2(z.x, a.y + bar), mp.lobby_searching ? "SEARCHING..." : "REFRESH",
                   Look::sticky, !mp.lobby_searching))
            queue_multiplayer_action("browse", "");
    }
    if (ui.input && !h.typing && (ui.pad.buttons & ~h.pad_before & 0x4000) && !mp.lobby_searching) queue_multiplayer_action("browse", ""); // X / Square
    if (ui.input && !h.typing && (ui.pad.buttons & ~h.pad_before & 0x8000)) h.sort = static_cast<view::Sort>((static_cast<int>(h.sort) + 1) % 3), changed = true; // Y / Triangle

    // ---- the list
    const ImVec2 la(a.x, a.y + bar + gap), lz(a.x + list_width, z.y - 60 * u);
    const float header = 52 * u, row_height = 104 * u, row_gap = 8 * u, pad_x = 28 * u;
    const float map_x = la.x + (lz.x - la.x) * .56f, count_right = lz.x - pad_x - 16 * u;
    {
        const float size = 28 * u;
        const auto column = [&](const char *text, float x, view::Sort sort, bool right_aligned) {
            const float w = width_of(ui.bold, size, text);
            const ImVec2 c(right_aligned ? x - w : x, la.y), d(c.x + w + 30 * u, la.y + header);
            const bool on = h.sort == sort;
            const Hit hit = item(ui, hash(text, 9), c, d);
            draw->AddText(ui.bold, size, ImVec2(c.x, c.y + 8 * u), on || hit.focus || hit.hover ? theme::blue : theme::muted, text);
            if (on) draw->AddTriangleFilled(ImVec2(c.x + w + 10 * u, c.y + 16 * u), ImVec2(c.x + w + 26 * u, c.y + 16 * u), ImVec2(c.x + w + 18 * u, c.y + 28 * u), theme::blue);
            if (hit.press && !on) h.sort = sort, changed = true;
        };
        column("NAME", la.x + pad_x, view::Sort::name, false);
        column("MAP", map_x, view::Sort::map, false);
        column("SKATERS", count_right - 26 * u, view::Sort::players, true);
    }
    if (changed) rebuild(h), h.scrolls[hash("servers")] = 0;
    const Row *chosen{};
    {
        Scroller list(ui, hash("servers"), ImVec2(la.x, la.y + header), lz);
        for (const auto &row : h.rows) {
            const auto &lobby = *row.lobby;
            const auto id = hash("server", lobby.id);
            bool drawn{};
            const float top = list.next(row_height, row_gap, drawn);
            const ImVec2 p(la.x, top), q(lz.x - 20 * u, top + row_height);
            const Hit hit = item(ui, id, p, q, &list.box);
            list.reveal(id, top, row_height);
            // Pointing at a row (or moving the focus onto it) is choosing it: the panel beside the
            // list shows it. Pressing it joins.
            if (hit.focus && (h.ring || ui.moved || ui.clicked)) {
                if (h.selected != lobby.id) wipe(h.password), h.code_mode = false;
                h.selected = lobby.id;
            }
            if (hit.press) chosen = &row;
            if (!drawn) continue;
            // Blue is where the pointer or the focus is; the row the panel shows keeps a mark.
            const bool lit = hit.focus || hit.hover, shown = h.selected == lobby.id;
            const ImU32 tile = lit ? theme::blue : hit.hover ? row_hover : lobby.official ? skate_theme::official_tile
                               : !lobby.friends.empty() ? skate_theme::friends_tile : skate_theme::tile;
            skate_theme::rough_rect(draw, p, q, tile, 0x900 + static_cast<unsigned>(lobby.id & 0xff), u * 2);
            if (shown && !lit) draw->AddRectFilled(ImVec2(p.x, p.y + 6 * u), ImVec2(p.x + 10 * u, q.y - 6 * u), theme::blue);
            const ImU32 ink = row.full && !lit ? theme::muted : theme::paper;
            const float name_size = 42 * u;
            float x = p.x + pad_x;
            const char *plate = lobby.official ? "OFFICIAL" : lobby.dedicated ? "SERVER" : "LOBBY";
            const float plate_width = width_of(ui.bold, 24 * u, plate) + 20 * u, plate_top = top + (row_height - 38 * u) * .5f;
            draw->AddRectFilled(ImVec2(x, plate_top), ImVec2(x + plate_width, plate_top + 38 * u),
                                lit ? IM_COL32(0, 0, 0, 90) : lobby.official ? skate_theme::official : skate_theme::tile_light, 4 * u);
            draw->AddText(ui.bold, 24 * u, ImVec2(x + 10 * u, plate_top + 7 * u), lit || lobby.official ? theme::paper : theme::muted, plate);
            x += plate_width + 18 * u;
            const bool friends = !lobby.friends.empty();
            label(draw, ui.bold, name_size, ImVec2(x, top + (friends ? 14 * u : (row_height - name_size) * .5f)), map_x - 24 * u, ink, view::caption(lobby.name, 64));
            if (friends) label(draw, ui.body, 28 * u, ImVec2(x, top + 62 * u), map_x - 24 * u, lit ? theme::paper : skate_theme::friends_text, "with " + lobby_friends_text(lobby));
            const auto count = row.full ? std::string("FULL") : std::to_string(lobby.players) + " / " + std::to_string(lobby.capacity);
            const float count_x = count_right - width_of(ui.bold, name_size, count);
            float map_right = count_x - 24 * u;
            if (lit) {
                // What pressing it does, on the game's yellow: there is no other join button.
                const char *does = mp.lobby_joining ? "JOINING" : row.here ? "YOU ARE HERE" : row.own ? "YOURS" : row.full ? nullptr
                                   : mp.active && mp.hosting ? "END YOUR SESSION FIRST" : "JOIN";
                if (does) {
                    const float w = width_of(ui.bold, 30 * u, does) + 40 * u;
                    map_right -= w + 20 * u;
                    const ImVec2 c(map_right + 8 * u, top + 22 * u), d(c.x + w, top + row_height - 22 * u);
                    skate_theme::rough_rect(draw, c, d, row.joinable ? sticky : sticky_off, 0x51, u * 2);
                    draw->AddText(ui.bold, 30 * u, ImVec2(c.x + 20 * u, c.y + (d.y - c.y - 30 * u) * .5f), skate_theme::black, does);
                }
            }
            if (lobby.password_required) {
                map_right -= width_of(ui.bold, 24 * u, "PASSWORD") + 24 * u;
                draw->AddText(ui.bold, 24 * u, ImVec2(map_right + 8 * u, top + (row_height - 24 * u) * .5f), lit ? theme::paper : skate_theme::warning, "PASSWORD");
            }
            label(draw, ui.body, 36 * u, ImVec2(map_x, top + (row_height - 36 * u) * .5f), map_right, lit ? theme::paper : row.full ? theme::muted : soft, row.map);
            draw->AddText(ui.bold, name_size, ImVec2(count_x, top + (row_height - name_size) * .5f), row.full && !lit ? skate_theme::danger : ink, count.c_str());
        }
        list.end();
    }
    if (h.rows.empty()) {
        const char *line = mp.lobby_searching ? "Finding skaters..." : !mp.lobby_searched ? "Looking for sessions..."
                           : h.listed ? "Nothing matches these filters." : "Nobody is hosting right now. Host a lobby and invite some skaters.";
        draw->AddText(ui.body, 40 * u, ImVec2(la.x + pad_x, la.y + header + 40 * u), theme::muted, line);
    }
    {
        const auto total = std::to_string(h.rows.size()) + (h.rows.size() == h.listed ? "" : " of " + std::to_string(h.listed)) +
                           (h.listed == 1 ? " session, " : " sessions, ") + std::to_string(h.skaters) + " skating";
        draw->AddText(ui.body, 30 * u, ImVec2(la.x + pad_x, z.y - 44 * u), theme::muted, total.c_str());
        float x = la.x + pad_x + width_of(ui.body, 30 * u, total) + 60 * u;
        const auto hint = [&](std::uint32_t pad_button, const char *key, const char *what) {
            const auto name = ui.pad.available ? controller_combo_label(pad_button, ui.pad.style) : std::string(key);
            if (!name.empty()) x += theme::keycap(draw, ui.bold, u * 2, ImVec2(x, z.y - 46 * u), name.c_str(), what) + 36 * u;
        };
        hint(0x1000, "Enter", "Join");
        if (ui.pad.available) hint(0x4000, "", "Refresh"), hint(0x8000, "", "Sort");
    }

    // ---- beside the list: the chosen session, or joining with a code
    const ImVec2 da(lz.x + gap, la.y), dz(z.x, z.y);
    skate_theme::rough_rect(draw, da, dz, skate_theme::tile, 0x77, u * 2);
    const float inset = 40 * u, left = da.x + inset, edge = dz.x - inset, tall = 110 * u;
    const ImVec2 foot(left, dz.y - inset - 84 * u);
    if (h.code_mode) {
        float y = da.y + inset;
        draw->AddText(ui.heading, 56 * u, ImVec2(left, y), theme::paper, "JOIN BY CODE");
        y += 84 * u;
        label(draw, ui.body, 32 * u, ImVec2(left, y), edge, theme::muted, "Ask the host for their session code. You will travel to their map.");
        y += 76 * u;
        draw->AddText(ui.bold, 28 * u, ImVec2(left, y), theme::muted, "JOIN CODE");
        y += 44 * u;
        bool go = field(ui, hash("code"), ImVec2(left, y), ImVec2(edge, y + bar), h.code, "Paste a ReSkate join code", false, 120);
        y += bar + 36 * u;
        draw->AddText(ui.bold, 28 * u, ImVec2(left, y), theme::muted, "PASSWORD");
        y += 44 * u;
        go |= field(ui, hash("code-password"), ImVec2(left, y), ImVec2(edge, y + bar), h.code_password, "Only if it has one", true);
        y += bar + 40 * u;
        const bool idle = !mp.active && !mp.lobby_joining;
        go |= button(ui, hash("join-code"), ImVec2(left, y), ImVec2(edge, y + tall), mp.lobby_joining ? "JOINING..." : idle ? "JOIN" : "LEAVE YOUR SESSION FIRST",
                     Look::sticky, idle && !h.code.empty());
        if (go && idle && !h.code.empty() && send(h, "join", h.code, h.code_password)) say(h, "Joining..."), wipe(h.code_password);
        y += tall + 20 * u;
        label(draw, ui.body, 30 * u, ImVec2(left, y), edge, theme::muted, view::caption(!h.notice.empty() ? h.notice : mp.status, 96));
        if (button(ui, hash("code-back"), foot, ImVec2(edge, foot.y + 84 * u), "BACK TO THE LIST", Look::plain)) h.code_mode = false, h.focus_next = hash("code-open");
        return;
    }
    if (const auto *row = selected_row(h)) {
        const auto &lobby = *row->lobby;
        float y = da.y + inset;
        label(draw, ui.heading, 56 * u, ImVec2(left, y), edge, theme::paper, view::caption(lobby.name, 64));
        y += 72 * u;
        label(draw, ui.body, 32 * u, ImVec2(left, y), edge, lobby.official ? skate_theme::official_text : theme::muted,
              std::string(lobby.official ? "Official server" : lobby.dedicated ? "Dedicated server" : "Player lobby") + (lobby.password_required ? ", password needed" : ""));
        y += 66 * u;
        const auto fact = [&](const char *name, const std::string &value, ImU32 colour = theme::paper) {
            draw->AddText(ui.body, 34 * u, ImVec2(left, y), theme::muted, name);
            const float w = std::min(width_of(ui.bold, 34 * u, value), edge - left - 260 * u);
            label(draw, ui.bold, 34 * u, ImVec2(edge - w, y), edge, colour, value);
            y += 54 * u;
        };
        fact("Map", row->map);
        fact("Skaters", std::to_string(lobby.players) + " / " + std::to_string(lobby.capacity), row->full ? skate_theme::danger : theme::paper);
        if (lobby.ping >= 0) fact("Ping", std::to_string(lobby.ping) + " ms");
        if (!lobby.friends.empty()) fact("Friends here", lobby_friends_text(lobby), skate_theme::friends_text);
        const float fill = lobby.capacity > 0 ? std::clamp(static_cast<float>(lobby.players) / static_cast<float>(lobby.capacity), 0.f, 1.f) : 0.f;
        draw->AddRectFilled(ImVec2(left, y + 4 * u), ImVec2(edge, y + 18 * u), skate_theme::tile_light, 4 * u);
        draw->AddRectFilled(ImVec2(left, y + 4 * u), ImVec2(left + (edge - left) * fill, y + 18 * u), row->full ? skate_theme::danger : theme::blue, 4 * u);
        y += 56 * u;
        bool go = chosen == row;
        if (lobby.password_required && row->joinable) {
            draw->AddText(ui.bold, 28 * u, ImVec2(left, y), theme::muted, "PASSWORD");
            y += 42 * u;
            go |= field(ui, hash("password"), ImVec2(left, y), ImVec2(edge, y + bar), h.password, "Type it here", true);
            y += bar + 24 * u;
            label(draw, ui.body, 28 * u, ImVec2(left, y), edge, theme::muted, "Type it, then press Enter or the session to join.");
            y += 52 * u;
        }
        if (go) join(h, *row);
        const auto &status = !h.notice.empty() ? h.notice : mp.active && !mp.hosting && !mp.echo && row->joinable
                                 ? std::string("Joining leaves the session you are in.") : mp.browser_status;
        label(draw, ui.body, 30 * u, ImVec2(left, y), edge, theme::muted, view::caption(status, 96));
    } else {
        draw->AddText(ui.body, 36 * u, ImVec2(left, da.y + inset), theme::muted, "Pick a session to see it here.");
        label(draw, ui.body, 30 * u, ImVec2(left, da.y + inset + 60 * u), edge, theme::muted, view::caption(!h.notice.empty() ? h.notice : mp.browser_status, 96));
    }
    if (button(ui, hash("code-open"), foot, ImVec2(edge, foot.y + 84 * u), "JOIN BY CODE", Look::plain)) h.code_mode = true, h.focus_next = hash("code");
}

// ---------------------------------------------------------------- hosting

void host_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &mp = ui.mp;
    const float u = ui.u, bar = 96 * u, row = 104 * u, gap = 12 * u;
    auto *draw = ui.draw;
    if (!h.host_seeded && mp.saved_host.loaded) {
        // Start from the host settings used last time.
        h.host_seeded = true;
        h.public_lobby = mp.saved_host.public_lobby;
        h.capacity = static_cast<unsigned>(mp.saved_host.capacity);
        h.tps = mp.saved_host.tps;
        if (!mp.saved_host.lobby_name.empty()) h.host_name = mp.saved_host.lobby_name, h.host_named = true;
    }
    if (!h.host_named && !mp.local_name.empty()) h.host_name = mp.local_name, h.host_named = true;
    const bool idle = !mp.active && !mp.lobby_joining;
    const float left_width = std::min((z.x - a.x) * .5f, 1800 * u);
    float y = a.y;
    draw->AddText(ui.heading, 52 * u, ImVec2(a.x, y), theme::paper, "MAKE IT YOUR SESSION");
    y += 84 * u;
    draw->AddText(ui.bold, 28 * u, ImVec2(a.x, y), theme::muted, "LOBBY NAME");
    y += 44 * u;
    field(ui, hash("host-name"), ImVec2(a.x, y), ImVec2(a.x + left_width, y + bar), h.host_name, "Your Steam name", false, 120);
    y += bar + 40 * u;
    draw->AddText(ui.bold, 28 * u, ImVec2(a.x, y), theme::muted, "PASSWORD (OPTIONAL)");
    y += 44 * u;
    field(ui, hash("host-password"), ImVec2(a.x, y), ImVec2(a.x + left_width, y + bar), h.host_password, "Leave blank for an open lobby", true);
    y += bar + 48 * u;
    draw->AddText(ui.bold, 28 * u, ImVec2(a.x, y), theme::muted, "MAP");
    label(draw, ui.bold, 40 * u, ImVec2(a.x, y + 44 * u), a.x + left_width, theme::paper, view::map_name(mp.map, ui.h.full.levels));

    const ImVec2 sa(a.x + left_width + 60 * u, a.y);
    const float right = std::min(z.x, sa.x + 1500 * u);
    y = sa.y;
    draw->AddText(ui.heading, 52 * u, ImVec2(sa.x, y), theme::paper, "LOBBY SETTINGS");
    y += 84 * u;
    const auto setting = [&](const char *name, const std::string &value) {
        const bool pressed = button(ui, hash(name, 3), ImVec2(sa.x, y), ImVec2(right, y + row), name, Look::tile, idle, value);
        y += row + gap;
        return pressed;
    };
    if (setting("Visibility", (mp.hosting ? mp.public_host : h.public_lobby) ? "Public" : "Join code")) h.public_lobby = !h.public_lobby;
    if (setting("Skaters", std::to_string(mp.hosting ? static_cast<unsigned>(mp.capacity) : h.capacity))) {
        constexpr std::array<unsigned, 5> limits{2, 4, 8, 16, static_cast<unsigned>(multiplayer_lobby_player_limit)};
        const auto next = std::upper_bound(limits.begin(), limits.end(), h.capacity);
        h.capacity = next == limits.end() ? limits.front() : *next;
    }
    if (setting("Tick rate", std::to_string(mp.hosting ? mp.tps : h.tps))) {
        const auto next = std::upper_bound(multiplayer_tick_rates.begin(), multiplayer_tick_rates.end(), h.tps);
        h.tps = next == multiplayer_tick_rates.end() ? multiplayer_tick_rates.front() : *next;
    }
    label(draw, ui.body, 30 * u, ImVec2(sa.x, y + 4 * u), right, theme::muted,
          h.public_lobby ? "Listed in the server browser for anyone to join." : "Only skaters you give your code to can join.");
    y += 64 * u;
    const bool ready = idle && mp.local_ready;
    if (button(ui, hash("host"), ImVec2(sa.x, y), ImVec2(right, y + 120 * u), !idle ? "SESSION ALREADY ACTIVE" : mp.local_ready ? "HOST LOBBY" : "LOAD A MAP TO HOST",
               Look::sticky, ready)) {
        if (h.host_name.size() > 128) say(h, "Lobby name is too long. Use a shorter name.");
        else if (send(h, "host-config", std::string(h.public_lobby ? "public " : "code ") + std::to_string(h.capacity) + " " + std::to_string(h.tps) + " " + h.host_name,
                      h.host_password))
            wipe(h.host_password);
    }
    y += 140 * u;
    label(draw, ui.body, 30 * u, ImVec2(sa.x, y), right, theme::muted, view::caption(!h.notice.empty() ? h.notice : mp.status, 110));
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
            skate_theme::rough_rect(draw, p, q, hit.focus && h.ring ? row_hover : skate_theme::tile, key & 0xfff, u * 2);
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
    skate_theme::rough_rect(draw, da, dz, skate_theme::tile, 0x7a, u * 2);
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
        if (option(mp.hosting ? "End session" : "Leave server", {}, true, Look::sticky)) send(h, "stop");
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
            const float right = a.x + list_width - 20 * u, volume_x = right - 330 * u, mute_x = volume_x - 12 * u - 260 * u;
            const auto mute = hash("mute", player.id), volume = hash("volume", player.id);
            list.reveal(mute, top, row), list.reveal(volume, top, row);
            if (drawn) {
                skate_theme::rough_rect(draw, ImVec2(a.x, top), ImVec2(mute_x - 12 * u, top + row), skate_theme::tile, mute & 0xfff, u * 2);
                label(draw, ui.bold, 40 * u, ImVec2(a.x + 30 * u, top + (row - 40 * u) * .5f), mute_x - 40 * u,
                      mp.voice.allowed && voice.speaking && !voice.muted ? skate_theme::good : theme::paper, view::caption(player.name, 40));
            }
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

// ---------------------------------------------------------------- Mod Options

enum ToolsTab : int { official_tab, custom_tab, world_tab, parks_tab, player_tab, online_tab, visuals_tab, binds_tab };

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

// A column of settings that scrolls: each a row with its name and what it is now.
struct Column {
    Ui &ui;
    Scroller list;
    float left, right, tall, between;
    Column(Ui &owner, std::uint32_t id, ImVec2 a, ImVec2 b)
        : ui(owner), list(owner, id, a, b), left(a.x), right(b.x - 26 * owner.u), tall(96 * owner.u), between(10 * owner.u) {}
    // A row that is pressed: a switch, a choice that goes round, or an action (no value).
    bool setting(const std::string &name, const std::string &value, bool enabled, Look look = Look::tile) {
        const auto id = hash(name, 21);
        bool drawn{};
        const float top = list.next(tall, between, drawn);
        list.reveal(id, top, tall);
        return button(ui, id, ImVec2(left, top), ImVec2(right, top + tall), name, look, enabled, value, &list.box);
    }
    // A row with a number: less and more at its right end. Returns -1, 0 or 1.
    int adjust(const std::string &name, const std::string &value, bool enabled) {
        const float u = ui.u, side = 96 * u, readout = 300 * u;
        const auto less = hash(name, 22), more = hash(name, 23);
        bool drawn{};
        const float top = list.next(tall, between, drawn);
        list.reveal(less, top, tall), list.reveal(more, top, tall);
        const float more_x = right - side, less_x = more_x - readout - side;
        if (drawn) {
            skate_theme::rough_rect(ui.draw, ImVec2(left, top), ImVec2(less_x - 10 * u, top + tall), skate_theme::tile, less & 0xfff, u * 2);
            label(ui.draw, ui.body, 40 * u, ImVec2(left + 30 * u, top + (tall - 40 * u) * .5f), less_x - 30 * u, enabled ? theme::paper : theme::muted, name);
            skate_theme::rough_rect(ui.draw, ImVec2(less_x + side + 6 * u, top), ImVec2(more_x - 6 * u, top + tall), skate_theme::tile, more & 0xfff, u * 2);
            const float w = width_of(ui.bold, 40 * u, value);
            ui.draw->AddText(ui.bold, 40 * u, ImVec2(less_x + side + (readout - w) * .5f, top + (tall - 40 * u) * .5f), enabled ? theme::blue : theme::muted, value.c_str());
        }
        int result{};
        if (button(ui, less, ImVec2(less_x, top), ImVec2(less_x + side, top + tall), "-", Look::plain, enabled, {}, &list.box)) result = -1;
        if (button(ui, more, ImVec2(more_x, top), ImVec2(right, top + tall), "+", Look::plain, enabled, {}, &list.box)) result = 1;
        return result;
    }
    void heading(const char *text) {
        bool drawn{};
        const float top = list.next(52 * ui.u, 30 * ui.u, drawn);
        if (drawn) ui.draw->AddText(ui.heading, 52 * ui.u, ImVec2(left, top), theme::paper, text);
    }
    void title(const char *text) {
        bool drawn{};
        const float top = list.next(54 * ui.u, 12 * ui.u, drawn);
        if (drawn) ui.draw->AddText(ui.bold, 28 * ui.u, ImVec2(left, top + 26 * ui.u), theme::muted, text);
    }
    void note(const std::string &text, ImU32 colour = theme::muted) {
        if (text.empty()) return;
        const float size = 30 * ui.u, wrap = right - left;
        bool drawn{};
        const float top = list.next(ui.body->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str()).y + 6 * ui.u, 18 * ui.u, drawn);
        if (drawn) ui.draw->AddText(ui.body, size, ImVec2(left, top + 6 * ui.u), colour, text.c_str(), nullptr, wrap);
    }
    void end() { list.end(); }
};

// The page's tabs. What a control does, and when it can, is the same as on ReSkate's own menu
// (native_tools_view.h for travel, parks and the world; the same requests and commands that
// menu sends for the rest); how it is laid out is here.
void tools_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const auto &m = h.full;
    const auto &mp = m.multiplayer;
    const auto &cb = h.callbacks;
    const float u = ui.u, gap = 28 * u, tall = 96 * u, between = 10 * u;
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
    const float list_width = std::min((z.x - a.x) * .56f, 2300 * u), column_width = std::min(z.x - a.x, 2100 * u);
    // A list of levels: the one pointed at says LOAD, and pressing it travels there.
    const auto levels = [&](bool custom, const char *title, const char *empty, const char *empty_help) {
        draw->AddText(ui.heading, 52 * u, a, theme::paper, title);
        std::vector<const Level *> listed;
        for (const auto &entry : m.levels)
            if (native_tools::listed(entry) && entry.custom == custom) listed.push_back(&entry);
        std::sort(listed.begin(), listed.end(), [](const auto *x, const auto *y) { return native_tools::level_name(*x) < native_tools::level_name(*y); });
        const auto *root = native_tools::level(m, native_tools::root_asset);
        const bool travel = root && root->native_registered && root->can_load && m.can_queue_load && !multiplayer_controls_level(mp) && cb.queue_load;
        Scroller list(ui, hash(title, 30), ImVec2(a.x, a.y + 96 * u), ImVec2(a.x + list_width, z.y));
        for (const auto *entry : listed) {
            const auto id = hash("level", hash(entry->asset));
            const bool can = travel && entry->can_load && entry != root;
            bool drawn{};
            const float top = list.next(tall, between, drawn);
            const ImVec2 p(a.x, top), q(a.x + list_width - 20 * u, top + tall);
            const Hit hit = item(ui, id, p, q, &list.box);
            list.reveal(id, top, tall);
            if (hit.press && can) h.tools.destination = entry->asset, act("load-level");
            if (!drawn) continue;
            const bool lit = hit.focus || hit.hover;
            skate_theme::rough_rect(draw, p, q, lit ? theme::blue : skate_theme::tile, id & 0xfff, u * 2);
            float name_right = q.x - 30 * u;
            if (lit) {
                const char *does = can ? "LOAD" : multiplayer_controls_level(mp) ? "HOST'S CHOICE" : "NOT NOW";
                const float w = width_of(ui.bold, 30 * u, does) + 40 * u;
                const ImVec2 c(q.x - w - 24 * u, top + 20 * u), d(q.x - 24 * u, top + tall - 20 * u);
                skate_theme::rough_rect(draw, c, d, can ? sticky : sticky_off, 0x52, u * 2);
                draw->AddText(ui.bold, 30 * u, ImVec2(c.x + 20 * u, c.y + (d.y - c.y - 30 * u) * .5f), skate_theme::black, does);
                name_right = c.x - 20 * u;
            }
            label(draw, ui.bold, 42 * u, ImVec2(p.x + 30 * u, top + (tall - 42 * u) * .5f), name_right, can || lit ? theme::paper : theme::muted,
                  native_tools::level_name(*entry));
        }
        list.end();
        if (listed.empty()) {
            draw->AddText(ui.body, 38 * u, ImVec2(a.x, a.y + 110 * u), theme::muted, empty);
            if (*empty_help) draw->AddText(ui.body, 32 * u, ImVec2(a.x, a.y + 164 * u), theme::muted, empty_help);
        }
        const std::string note = multiplayer_controls_level(mp) ? "Only the lobby host can change levels."
                                 : !m.can_queue_load ? "Travel is unavailable right now. Wait for the game to finish loading." : h.notice;
        if (!note.empty())
            draw->AddText(ui.body, 30 * u, ImVec2(a.x + list_width + gap, a.y + 110 * u), theme::muted, note.c_str(), nullptr, std::min(z.x - a.x - list_width - gap, 1500 * u));
    };

    switch (h.tools_tab) {
    case official_tab: levels(false, "FIND YOUR SPOT", "Waiting for available levels...", ""); break;
    case custom_tab: levels(true, "CUSTOM MAPS", "No custom maps installed.", "Custom maps from the Mods folder appear here."); break;
    case world_tab: {
        Column list(ui, hash("world"), a, ImVec2(a.x + column_width, z.y));
        list.heading("THIS WORLD");
        const bool population = m.world.ready && m.world_controls.population_available && console;
        if (list.setting("Traffic", population_labels[std::clamp(m.world_controls.choices.traffic, -1, 3) + 1], population)) act("traffic");
        if (list.setting("Pedestrians", population_labels[std::clamp(m.world_controls.choices.pedestrians, -1, 3) + 1], population)) act("pedestrians");
        if (list.setting("Time of day", native_tools::time_of_day_labels[native_tools::time_of_day(m)], native_tools::time_of_day_available(m, cb))) act("time-of-day");
        if (m.world.controlled_by_host)
            list.note(mp.server_admin ? "You are an admin: time of day changes for the whole server." : "The lobby host controls world layers, including time of day.");
        list.note(h.notice);
        list.end();
        break;
    }
    case parks_tab: {
        // One card for each of the three lots: its style, its layout, and loading it.
        draw->AddText(ui.heading, 52 * u, a, theme::paper, "PARK LAYOUTS");
        const bool available = native_tools::can_park(m, cb);
        const float cards = static_cast<float>(park_lots.size());
        const float width = std::min(((z.x - a.x) - gap * (cards - 1)) / cards, 1300 * u), top = a.y + 96 * u;
        const float card_height = 36 * u + 60 * u + (tall + between) * 2 + 16 * u + 110 * u + 36 * u;
        for (unsigned lot = 0; lot < park_lots.size(); ++lot) {
            const ImVec2 p(a.x + static_cast<float>(lot) * (width + gap), top), q(p.x + width, top + card_height);
            skate_theme::rough_rect(draw, p, q, skate_theme::tile, 0x60 + lot, u * 2);
            const float left = p.x + 36 * u, right = q.x - 36 * u;
            float y = p.y + 36 * u;
            label(draw, ui.bold, 40 * u, ImVec2(left, y), right, theme::paper, std::string(park_lots[lot].label));
            y += 60 * u;
            std::string family = "Empty lot";
            for (unsigned i = 0; i < park_families.size(); ++i)
                if (h.tools.parks[lot].starts_with(park_families[i])) family = park_family_labels[i];
            const auto row = [&](const char *name, const std::string &value, std::string_view command) {
                if (button(ui, hash(name, 40 + lot), ImVec2(left, y), ImVec2(right, y + tall), name, Look::plain, available, value)) h.tools.lot = lot, act(command);
                y += tall + between;
            };
            row("Style", family, "park-family");
            row("Layout", park_label(h.tools.parks[lot]), "park-layout");
            y += 16 * u;
            if (button(ui, hash("load-park", lot), ImVec2(left, y), ImVec2(right, y + 110 * u), "LOAD THIS LAYOUT", Look::sticky,
                       available && valid_park(lot, h.tools.parks[lot])))
                h.tools.lot = lot, act("load-park");
        }
        Column all(ui, hash("parks"), ImVec2(a.x, top + card_height + 30 * u), ImVec2(a.x + std::min(z.x - a.x, 1500 * u), z.y));
        all.title("ALL THREE LOTS");
        if (all.setting("Load random parks", {}, available)) act("load-random-parks");
        if (all.setting("Randomize on launch", on_off(m.parks.randomize_on_launch), m.parks.available && console)) act("park-random-on-launch");
        all.note(m.world.map != WorldMap::bam ? std::string("Load San Vansterdam to change parks.")
                 : native_tools::parks_locked(m) ? std::string("The lobby host controls park layouts.")
                 : m.parks.controlled_by_host ? std::string("You are an admin: layouts you load change for the whole server.") : h.notice);
        all.end();
        break;
    }
    case player_tab: {
        Column list(ui, hash("player"), a, ImVec2(a.x + column_width, z.y));
        const auto &d = m.debug;
        list.heading("SKATE YOUR WAY");
        if (list.setting("Noclip", on_off(d.noclip), (d.noclip_available || d.noclip) && debugging)) debug({DebugAction::set_noclip, !d.noclip});
        if (list.setting("No bail", on_off(d.no_bail), (d.no_bail_available || d.no_bail) && debugging)) debug({DebugAction::set_no_bail, !d.no_bail});
        if (list.setting("Hall of Meat", on_off(m.hall_of_meat.enabled), m.hall_of_meat.available && console)) act("hall-of-meat");
        const auto &wear = m.offline.board_wear;
        if (list.setting("Board wear", on_off(wear.effective), wear.available && cb.queue_offline_feature)) act("board-wear");
        if (list.setting("Reset board wear", {}, wear.available && wear.effective && console)) act("board-wear-reset");
        list.title("CAMERA");
        const bool camera = d.available && d.camera_available && debugging, edit = d.available && debugging;
        if (list.setting("First person", on_off(d.first_person), camera)) debug({DebugAction::set_first_person, !d.first_person});
        if (list.setting("True first person", on_off(d.first_person_arm.stabilize), edit)) debug({DebugAction::set_first_person_stabilize, !d.first_person_arm.stabilize});
        if (list.setting("Third person on foot", on_off(d.first_person_arm.board_only), edit)) debug({DebugAction::set_first_person_board_only, !d.first_person_arm.board_only});
        if (list.setting("Freecam", on_off(d.free_camera), camera)) debug({DebugAction::set_free_camera, !d.free_camera});
        {
            constexpr std::array<float, 7> speeds{.6f, 3.f, 5.f, 15.f, 60.f, 300.f, 1500.f};
            const auto text = std::to_string(d.camera_speed);
            if (const int way = list.adjust("Flight speed", text.substr(0, text.find('.') + 2), (d.free_camera || d.noclip) && d.camera_available && debugging))
                debug({DebugAction::set_camera_speed, false, stepped(d.camera_speed, speeds, way)});
        }
        list.title("BOOSTS");
        {
            constexpr std::array<float, 9> forward{1.f, 5.f, 10.f, 25.f, 50.f, 100.f, 150.f, 200.f, 300.f};
            constexpr std::array<float, 8> up{1.f, 2.f, 3.f, 5.f, 8.f, 12.f, 18.f, 25.f};
            if (const int way = list.adjust("Forward boost", "+" + whole(d.forward_velocity_speed), debugging))
                debug({DebugAction::set_forward_velocity_speed, false, stepped(d.forward_velocity_speed, forward, way)});
            if (const int way = list.adjust("Up boost", "+" + whole(d.up_velocity_speed), debugging))
                debug({DebugAction::set_up_velocity_speed, false, stepped(d.up_velocity_speed, up, way)});
            if (const int way = list.adjust("Off-board up boost", "+" + whole(d.offboard_up_velocity_speed), debugging))
                debug({DebugAction::set_offboard_up_velocity_speed, false, stepped(d.offboard_up_velocity_speed, up, way)});
        }
        list.note(!h.notice.empty() ? h.notice : std::string("The buttons for the boosts are on the BINDS tab. Flight speed is for noclip and freecam; close the menu to fly."));
        list.end();
        break;
    }
    case online_tab: {
        Column list(ui, hash("online"), a, ImVec2(a.x + column_width, z.y));
        list.heading("OTHER SKATERS");
        if (m.steam_offline) {
            list.note("Multiplayer is off in offline mode.");
        } else {
            constexpr std::array<float, 8> shown{50.f, 75.f, 100.f, 150.f, 200.f, 300.f, 500.f, 1000.f};
            constexpr std::array<float, 8> tags{10.f, 25.f, 50.f, 75.f, 120.f, 200.f, 300.f, 500.f};
            constexpr std::array<float, 7> reach{5.f, 10.f, 20.f, 40.f, 75.f, 120.f, 200.f};
            constexpr std::array<float, 6> seconds{1.f, 3.f, 5.f, 8.f, 15.f, 30.f};
            constexpr std::array<float, 8> lines{1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
            if (const int way = list.adjust("Player distance", mp.player_distance >= player_distance_unlimited ? std::string("Everyone") : whole(mp.player_distance, " m"), true))
                online("player-distance", whole(stepped(mp.player_distance, shown, way)));
            if (list.setting("Direct connections", on_off(mp.prefer_direct), true)) online("direct-connections", mp.prefer_direct ? "off" : "on");
            list.title("NAMETAGS");
            if (list.setting("Nametags", on_off(mp.nametags), true)) online("nametags", mp.nametags ? "off" : "on");
            if (const int way = list.adjust("Nametag distance", whole(mp.nametag_distance, " m"), true)) online("nametag-distance", whole(stepped(mp.nametag_distance, tags, way)));
            if (list.setting("Nametag dots", on_off(mp.nametag_dots), true)) online("nametag-dots", mp.nametag_dots ? "off" : "on");
            if (list.setting("Friends' nametags only", on_off(mp.nametags_friends), true)) online("nametags-friends", mp.nametags_friends ? "off" : "on");
            list.title("CHAT");
            if (list.setting("Text chat", on_off(mp.chat_visible), true)) online("chat-visible", mp.chat_visible ? "off" : "on");
            if (list.setting("Chat bubbles", on_off(mp.chat_bubbles), true)) online("chat-bubbles", mp.chat_bubbles ? "off" : "on");
            if (list.setting("Own chat bubbles", on_off(mp.chat_bubbles_own), mp.chat_bubbles)) online("chat-bubbles-own", mp.chat_bubbles_own ? "off" : "on");
            if (const int way = list.adjust("Bubble distance", whole(mp.chat_bubbles_distance, " m"), mp.chat_bubbles))
                online("chat-bubbles-distance", whole(stepped(mp.chat_bubbles_distance, reach, way)));
            if (const int way = list.adjust("Bubble time", whole(mp.chat_bubbles_duration, " s"), mp.chat_bubbles))
                online("chat-bubbles-duration", whole(stepped(mp.chat_bubbles_duration, seconds, way)));
            if (const int way = list.adjust("Bubble lines", whole(static_cast<float>(mp.chat_bubbles_history)), mp.chat_bubbles))
                online("chat-bubbles-history", whole(stepped(static_cast<float>(mp.chat_bubbles_history), lines, way)));
            if (dingosdk::discord_presence::available()) {
                list.title("DISCORD");
                if (list.setting("Discord status", on_off(dingosdk::discord_presence::enabled()), true)) dingosdk::discord_presence::set_enabled(!dingosdk::discord_presence::enabled());
            }
            if (!mp.identity_tag.empty()) {
                // Only for players the ReSkate team has given a tag.
                list.title("YOUR TAG");
                if (list.setting("Show my tag", on_off(mp.identity_tag_shown), true)) online("mark-tag", mp.identity_tag_shown ? "off" : "on");
                if (list.setting("Show my items", on_off(mp.identity_items_shown), true)) online("mark-items", mp.identity_items_shown ? "off" : "on");
            }
            list.note(h.notice);
        }
        list.end();
        break;
    }
    case visuals_tab: {
        Column list(ui, hash("visuals"), a, ImVec2(a.x + column_width, z.y));
        list.heading("WHAT YOU SEE AND HEAR");
        constexpr std::array<const char *, 3> names{"Film grain", "Vignette", "Chromatic aberration"};
        for (unsigned i = 0; i < names.size(); ++i) {
            const auto choice = m.graphics.choices.effects[i];
            const bool on = choice < 0 ? !m.graphics.ready[i] || m.graphics.enabled[i] : choice != 0;
            if (list.setting(names[i], on_off(on) + (choice < 0 ? " (default)" : ""), m.graphics.available && console)) act("graphics", std::string(graphics_keys[i]));
        }
        if (list.setting("Restore default effects", {}, m.graphics.available && console)) act("graphics-reset");
        if (list.setting("Challenges", m.progression.challenges_hidden ? "Hidden" : "Shown", m.progression.challenges_enabled && console)) act("challenges");
        {
            const bool shuffle = dingosdk::profile_runtime::music_shuffle_enabled();
            if (list.setting("Shuffle playlists", on_off(shuffle), true)) {
                dingosdk::profile_runtime::set_music_shuffle_enabled(!shuffle);
                dingosdk::profile_runtime::set_local_preference("MusicShuffle", !shuffle);
            }
        }
        list.note(!h.notice.empty() ? h.notice : std::string("Hiding challenges keeps your progress."));
        list.end();
        break;
    }
    default: {
        // Buttons for ReSkate's own actions. Pressing a row records the next button, combination
        // of buttons or key; for that long the game does not see them.
        const auto &binds = m.bindings;
        const bool available = binds.available && console;
        const auto recording = hash("bind-recording");
        const auto save = [&](int action, std::uint32_t combo) {
            run("bind " + (action >= 10 ? std::string(action_binds[static_cast<std::size_t>(action - 10)].name) : std::string(action == 1 ? "freecamcontroller" : action == 2 ? "freecam"
                           : action == 3 ? "noclip" : action == 4 ? "forwardvelocity" : action == 5 ? "upvelocity" : action == 6 ? "tptofreecam" : action == 8 ? "voteyes"
                           : action == 9 ? "voteno" : "offboardupvelocity")) + " " + std::to_string(combo));
        };
        if (h.recording_bind) {
            ControllerInput controller;
            DingoSDKOverlayReadControllerInput(&controller, true);
            if (!available || GetTickCount64() >= h.recording_until || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                h.recording_bind = 0, set_typing(h, 0);
                say(h, "Recording cancelled. Your binding is unchanged.");
            } else if (const auto combo = h.bind_capture.update(controller, true)) {
                const int action = std::exchange(h.recording_bind, 0);
                set_typing(h, 0);
                save(action, *combo);
            }
        }
        Column list(ui, hash("binds"), a, ImVec2(a.x + column_width, z.y));
        list.heading("ACTION BINDS");
        list.note(h.recording_bind ? std::string("Press the button, combination or key to use. Escape cancels.")
                  : !h.notice.empty() ? h.notice : std::string("Pick an action, then press what should do it. CLEAR takes its button away."));
        const auto row = [&](int action, const std::string &name, std::uint32_t combo) {
            const float u2 = ui.u, clear_width = 260 * u2;
            const auto id = hash("bind", static_cast<std::uint64_t>(action)), clear = hash("bind-clear", static_cast<std::uint64_t>(action));
            bool drawn{};
            const float top = list.list.next(list.tall, list.between, drawn);
            list.list.reveal(id, top, list.tall), list.list.reveal(clear, top, list.tall);
            const bool mine = h.recording_bind == action, idle = available && !h.recording_bind;
            if (button(ui, id, ImVec2(list.left, top), ImVec2(list.right - clear_width - 12 * u2, top + list.tall), name, mine ? Look::chosen : Look::tile, idle,
                       mine ? std::string("Recording...") : controller_combo_label(combo, ui.pad.style), &list.list.box)) {
                h.recording_bind = action;
                h.bind_capture = {};
                h.recording_until = GetTickCount64() + 30000;
                h.notice.clear();
                set_typing(h, recording);
            }
            if (button(ui, clear, ImVec2(list.right - clear_width, top), ImVec2(list.right, top + list.tall), "CLEAR", Look::plain, idle && combo != 0, {}, &list.list.box))
                save(action, 0);
        };
        row(1, "Freecam controller", binds.freecam_controller_combo);
        row(2, "Freecam", binds.freecam_combo);
        row(3, "Noclip", binds.noclip_combo);
        row(4, "Forward boost", binds.forward_velocity_combo);
        row(5, "Up boost", binds.up_velocity_combo);
        row(7, "Off-board up boost", binds.offboard_up_velocity_combo);
        row(6, "Teleport to freecam", binds.tp_to_freecam_combo);
        row(8, "Vote yes", binds.vote_yes_combo);
        row(9, "Vote no", binds.vote_no_combo);
        for (std::size_t i = 0; i < action_binds.size(); ++i) row(10 + static_cast<int>(i), std::string(action_binds[i].label), binds.action_combos[i]);
        list.end();
        break;
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

// ---------------------------------------------------------------- focus

// Moves the focus one control over, by where the controls were drawn last frame.
void move_focus(Hub &h, int dx, int dy) {
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
    for (const auto &entry : h.last) {
        if (entry.id == from->id) continue;
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
    const bool shown = h.page.visible && !h.dismissed;
    if (shown != h.shown) {
        h.shown = shown;
        h.next_model = {};
        h.searched = false;
        h.last.clear();
        if (!shown) {
            h.recording_bind = 0;
            set_typing(h, 0);
            wipe(h.password), wipe(h.code_password), wipe(h.host_password);
            h.notice.clear();
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

void draw_hub_page() {
    auto &h = hub();
    if (!h.shown) return;
    auto &s = state();
    auto &io = ImGui::GetIO();
    const auto display = io.DisplaySize;
    // One unit of the game's menu in pixels, and where its page sits in this window.
    const float u = std::min(display.x / layout_width, display.y / layout_height);
    const float across = display.x / u, down = display.y / u, canvas = std::min(across, layout_widest);
    const float page_left = ((across - canvas) * .5f + side_margin) * u, page_right = ((across + canvas) * .5f - side_margin) * u;
    const float lower = (down - layout_height) * .05f; // how far a taller window moves the page down
    const bool others = s.visible.load() || s.console_visible.load() || s.editor_visible.load() || s.chat_visible.load();
    Ui ui{h, ImGui::GetBackgroundDrawList(), s.menu.bold ? s.menu.bold : ImGui::GetFont(), s.menu.body ? s.menu.body : ImGui::GetFont(), nullptr, u, io.MousePos,
          !others && game_window_foreground(s.window.load()), false, false, false, 0, h.model, {}};
    ui.heading = s.menu.heading ? s.menu.heading : ui.bold;
    ui.clicked = ui.input && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    ui.doubled = ui.input && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ui.moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    ui.wheel = ui.input ? io.MouseWheel : 0;
    const auto &mp = h.model;
    if (h.rows_stale) rebuild(h);
    if (h.notice_until && GetTickCount64() >= h.notice_until) h.notice.clear(), h.notice_until = 0;
    // Going into a session shows it; leaving one goes back to the browser.
    if (mp.active != h.was_active) {
        h.was_active = mp.active;
        h.tab = mp.active ? Tab::session : Tab::browser;
        h.selected_player.clear();
    }
    if ((h.tab == Tab::session || h.tab == Tab::voice) && !mp.active) h.tab = Tab::browser;
    const bool runs_session = mp.active && (mp.hosting || mp.server_admin);
    if (h.tab == Tab::settings && !runs_session) h.tab = mp.active ? Tab::session : Tab::browser;

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
    if (ui.input && !typing_before) {
        // Back closes the game's menu, which takes a moment to say so: this goes at once.
        if (key_new(7) || (pressed & 0x2000)) h.dismissed = true;
        const std::array<bool, 4> held{keys[0] || (ui.pad.buttons & 1), keys[1] || (ui.pad.buttons & 2), keys[2] || (ui.pad.buttons & 4), keys[3] || (ui.pad.buttons & 8)};
        const std::array<bool, 4> fresh{key_new(0) || (pressed & 1), key_new(1) || (pressed & 2), key_new(2) || (pressed & 4), key_new(3) || (pressed & 8)};
        const auto tick = GetTickCount64();
        int direction = -1;
        for (int i = 0; i < 4; ++i)
            if (fresh[static_cast<std::size_t>(i)]) direction = i, h.repeat_at = tick + 380;
        if (direction < 0 && tick >= h.repeat_at)
            for (int i = 0; i < 4; ++i)
                if (held[static_cast<std::size_t>(i)]) direction = i, h.repeat_at = tick + 70;
        if (direction >= 0) {
            // The first press only shows where the focus is.
            if (h.ring || !h.focus) move_focus(h, direction == 2 ? -1 : direction == 3 ? 1 : 0, direction == 0 ? -1 : direction == 1 ? 1 : 0);
            h.ring = true;
            h.focus_moved = true;
        }
        if (key_new(4) || (pressed & 0x1000)) {
            if (h.focus && std::any_of(h.last.begin(), h.last.end(), [&](const Item &entry) { return entry.id == h.focus; })) h.fire = h.focus, h.ring = true;
        }
        previous_tab = key_new(5) || (pressed & 0x10000);
        next_tab = key_new(6) || (pressed & 0x20000);
    }
    h.keys_before = keys;
    // A key that ended the typing (Enter, Escape) is still down: it is not a fresh press of anything.
    // And so is one held while the game was not the window in front.
    if (typing_before || !ui.input) h.keys_before.fill(true);
    if (!ui.input) h.pad_before = ~0U;
    h.items.clear();

    // ---- the page's tabs
    const ImVec2 a(page_left, (body_top + lower) * u);
    const ImVec2 z(std::min(page_right, a.x + body_most * u), (down - (layout_height - body_bottom) - lower) * u);
    const bool tools = h.page.section == 1;
    {
        // (Each page keeps the tab it was left on.)
        std::vector<std::pair<int, const char *>> tabs;
        int multiplayer_tab = static_cast<int>(h.tab);
        int &open = tools ? h.tools_tab : multiplayer_tab;
        if (tools) {
            tabs = {{official_tab, "OFFICIAL MAPS"}, {custom_tab, "CUSTOM MAPS"}, {world_tab, "WORLD"}, {parks_tab, "PARKS"},
                    {player_tab, "PLAYER"}, {online_tab, "ONLINE"}, {visuals_tab, "VISUALS"}, {binds_tab, "BINDS"}};
        } else {
            tabs = {{static_cast<int>(Tab::browser), "SERVER BROWSER"}, {static_cast<int>(Tab::host), "HOST LOBBY"}};
            // The session and its voice chat are only there in one.
            if (mp.active) tabs.emplace_back(static_cast<int>(Tab::session), "CURRENT SESSION"), tabs.emplace_back(static_cast<int>(Tab::voice), "VOICE CHAT");
            // And its settings only for whoever runs it.
            if (runs_session) tabs.emplace_back(static_cast<int>(Tab::settings), "SESSION SETTINGS");
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
        const float top = (tabs_top + lower) * u, bottom = top + tabs_height * u, size = 44 * u;
        float x = a.x;
        const auto cap = [&](std::uint32_t pad_button, const char *key) {
            const auto name = ui.pad.available ? controller_combo_label(pad_button, ui.pad.style) : std::string(key);
            x += theme::keycap(ui.draw, ui.bold, u * 2.2f, ImVec2(x, top + 20 * u), name.c_str(), "") + 4 * u;
        };
        cap(0x10000, "Z");
        for (const auto &[tab, name] : tabs) {
            const float w = width_of(ui.bold, size, name) + 56 * u;
            const ImVec2 p(x, top), q(x + w, bottom);
            const Hit hit = item(ui, hash(name, 1), p, q);
            const bool on = open == tab, lit = hit.focus || hit.hover;
            skate_theme::rough_rect(ui.draw, p, q, lit && !on ? theme::blue : on ? theme::paper : IM_COL32(26, 26, 26, 200), hash(name) & 0xfff, u * 2);
            ui.draw->AddText(ui.bold, size, ImVec2(p.x + 28 * u, top + (bottom - top - size) * .5f), on ? skate_theme::black : theme::paper, name);
            // The game's brushed blue line under the tab that is open.
            if (on) ui.draw->AddRectFilled(ImVec2(p.x + 6 * u, bottom + 4 * u), ImVec2(q.x - 6 * u, bottom + 14 * u), theme::blue, 5 * u);
            if (hit.press && !on) open = tab, h.sound = sound_tab, set_typing(h, 0);
            x = q.x + 20 * u;
        }
        cap(0x20000, "C");
        if (!tools) h.tab = static_cast<Tab>(multiplayer_tab);
    }

    if (tools) tools_page(ui, a, z);
    else switch (h.tab) {
    case Tab::browser: browser_page(ui, a, z); break;
    case Tab::host: host_page(ui, a, z); break;
    case Tab::session: session_page(ui, a, z); break;
    case Tab::voice: voice_page(ui, a, z); break;
    case Tab::settings: settings_page(ui, a, z); break;
    }
    if (ui.input) h.pad_before = ui.pad.buttons;
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
} // namespace dingosdk::overlay::detail
