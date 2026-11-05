#include "overlay_internal.h"
#include "skate_style.h"
#include "cursor.h"
#include "input_capture.h"
#include "scripted_pad.h"
#include "Extension/UI/NativeMenu/native_menu.h"
#include "Extension/UI/NativeMenu/native_menu_view.h"
#include "Extension/Assets/mod_store.h"
#include "Extension/News/live_news.h"
#include "Extension/News/patch_notes.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Game/Input/controller_bindings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// ReSkate's front page, drawn over the game's own HUB tab in the pause menu: the tab the menu
// opens on. The game keeps the tab, its top bar and its Back prompt; native_hub.cpp empties its
// tiles (one stays, drawn as nothing and doing nothing, so the game has something to focus) and
// says while the HUB is the page on screen. Everything in the page is drawn here:
//
//   left   the latest news post big (its picture, title, description, READ and a pager), and
//          under it three small cards: the latest patch notes and the next posts;
//   right  PATCH NOTES (the latest release's highlights, stretched to end level with the cards on
//          the left), YOUR PROFILE with EDIT PROFILE, and RIGHT NOW (a friend to join, mods with
//          updates), each row opening the page that deals with it.
//
// READ and ALL PATCH NOTES open a reader in the page's place, which B closes; the HUB's Back is held
// for that long (hold_hub_back) so B does not close the pause menu as well.
//
// As on ReSkate's other pages in the menu (hub_page.cpp), the game keeps the controller and this
// only watches it, with a focus of its own moved by the D-pad or the arrow keys and pressed with A
// or Enter. LT and RT (Z and C) step through the news (LB and RB are the game's, for its tabs), and Y
// edits the profile from anywhere on the page.
namespace dingosdk::overlay::detail {
namespace {
namespace ts = dingosdk::thunderstore;
using Kind = ts::ReadmeLine::Kind;

// The Insert menu's colours, as the Multiplayer page has them (hub_page.cpp): its near-black cards,
// blue for the main action, white with dark ink under the D-pad or the keys, a lighter grey under the
// pointer, and the kit's tag colours (news blue, a release green, updates amber).
constexpr ImU32 card = IM_COL32(16, 16, 16, 242), card_pointed = IM_COL32(56, 57, 61, 245), button_dark = IM_COL32(38, 38, 40, 250),
                focus_ink = IM_COL32(11, 11, 11, 255), sub_ink = IM_COL32(138, 140, 146, 255), sub_ink_lit = IM_COL32(70, 70, 74, 255),
                soft = IM_COL32(210, 210, 214, 255), news_light = IM_COL32(124, 196, 255, 255), placeholder = IM_COL32(63, 84, 104, 255),
                placeholder_alt = IM_COL32(74, 97, 119, 255), release_tile = IM_COL32(24, 66, 40, 255);
// The game's own menu sounds, by their numbers in its list (UI/NativeMenu/ui_sound.h).
constexpr std::uint32_t sound_navigate = 3, sound_select = 7, sound_tab = 56;
// Pad buttons (ControllerInput::buttons).
constexpr std::uint32_t pad_up = 1, pad_down = 2, pad_left = 4, pad_right = 8, pad_a = 0x1000, pad_b = 0x2000,
                        pad_y = 0x8000, pad_lt = 0x10000, pad_rt = 0x20000;
// The keys watched: the arrows, Enter, Z and C (LT and RT), Escape.
constexpr std::array<int, 8> key_codes{VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, 'Z', 'C', VK_ESCAPE};
constexpr std::size_t small_cards = 3;

constexpr std::uint32_t hash(std::string_view text, std::uint64_t salt = 0) {
    std::uint32_t value = 2166136261U ^ static_cast<std::uint32_t>(salt * 2654435761ULL);
    for (const char c : text) value = (value ^ static_cast<unsigned char>(c)) * 16777619U;
    return value ? value : 1;
}

enum class View { front, post, notes };

struct Control {
    std::uint32_t id{};
    ImVec2 a, b;
};
// A line of the reader, laid out for its width.
struct Laid {
    enum class Look { picture, title, meta, release, heading, text, bullet, code, rule, gap } look{};
    std::string text, extra; // extra: a release's date
    bool first{};            // the first line of its paragraph or bullet
    float height{};
    std::size_t release{};   // the release it belongs to (the notes)
    float size{};            // its text's size
};

struct Home {
    // The focus, as on ReSkate's other pages: the control it is on, whether the D-pad or the keys
    // put it there (`ring`: shown) or the pointer, and the controls drawn last frame to move it by.
    std::uint32_t focus{}, fire{}, focus_next{}, focus_heard{};
    bool ring{}, focus_moved{};
    std::vector<Control> items, last;
    std::uint32_t pad_before{};
    std::array<bool, key_codes.size()> keys_before{};
    ULONGLONG repeat_at{}, seen{}, sound_at{};
    std::uint32_t sound{~0U};
    // What it shows.
    View view{View::front};
    std::size_t featured{}; // the news post big on the left
    std::size_t reading{};  // the post open in the reader
    float scroll{};         // the reader's, in pixels
    // B (or Escape) that closed the reader holds the HUB's Back until it is let go, and a moment after.
    ULONGLONG hold_until{};
    std::vector<profile::NewsPost> posts;
    ULONGLONG posts_at{};
    std::shared_ptr<const news::PatchNotes> notes;
    // The reader laid out, and for what (the view, the post, the notes' copy, the width).
    std::vector<Laid> laid;
    std::string laid_key;
    // RIGHT NOW: mods with updates, worked out again when the listing or the installed mods change.
    std::shared_ptr<const mod_store::Listing> listing;
    std::shared_ptr<const mod_store::Mods> mods;
    unsigned listing_serial{~0U}, mods_serial{~0U};
    std::size_t updates{};
    ULONGLONG mods_at{};
};
Home &home() {
    static Home value;
    return value;
}

// What one frame draws with.
struct Ui {
    Home &h;
    ImDrawList *draw;
    ImFont *bold, *body, *heading, *title;
    float u;
    ImVec2 mouse;
    bool input, clicked, moved;
    float wheel;
    ControllerInput pad;
};

bool inside(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.x < b.x && p.y >= a.y && p.y < b.y; }
float width_of(ImFont *font, float size, std::string_view text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.data(), text.data() + text.size()).x; }
// One line of text, cut off at `right`.
void label(ImDrawList *draw, ImFont *font, float size, ImVec2 at, float right, ImU32 colour, std::string_view text) {
    if (right <= at.x) return;
    draw->PushClipRect(ImVec2(at.x, at.y - size), ImVec2(right, at.y + size * 2), true);
    draw->AddText(font, size, at, colour, text.data(), text.data() + text.size());
    draw->PopClipRect();
}
// `text` broken into lines no wider than `width`; at most `most` (0: any), the last ending in "..."
// when there was more.
std::vector<std::string> wrap(ImFont *font, float size, std::string_view text, float width, std::size_t most = 0) {
    std::vector<std::string> lines;
    std::string line;
    bool more{};
    for (std::size_t at = 0; at < text.size();) {
        if (most && lines.size() >= most) {
            more = true;
            break;
        }
        if (text[at] == '\n' || text[at] == '\r') {
            if (text[at] == '\n') lines.push_back(std::exchange(line, {}));
            ++at;
            continue;
        }
        auto end = at;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\r') ++end;
        const std::string word(text.substr(at, end - at));
        auto candidate = line.empty() ? word : line + ' ' + word;
        if (!line.empty() && width_of(font, size, candidate) > width) lines.push_back(std::exchange(line, word));
        else line = std::move(candidate);
        while (end < text.size() && text[end] == ' ') ++end;
        at = end;
    }
    if (!line.empty()) {
        if (most && lines.size() >= most) more = true;
        else lines.push_back(std::move(line));
    }
    if (more && !lines.empty()) {
        auto &last = lines.back();
        while (!last.empty() && width_of(font, size, last + "...") > width) {
            last.pop_back();
            while (!last.empty() && (static_cast<unsigned char>(last.back()) & 0xc0) == 0x80) last.pop_back();
            if (!last.empty() && static_cast<unsigned char>(last.back()) >= 0xc0) last.pop_back();
        }
        last += "...";
    }
    return lines;
}
// A small tag in one of the kit's colours; returns its width.
float tag(Ui &f, ImVec2 at, std::string_view text, ImU32 fill, ImU32 ink) {
    const float u = f.u, size = 26 * u, w = width_of(f.bold, size, text) + 24 * u;
    f.draw->AddRectFilled(at, ImVec2(at.x + w, at.y + 40 * u), fill, 3 * u);
    f.draw->AddText(f.bold, size, ImVec2(at.x + 12 * u, at.y + 7 * u), ink, text.data(), text.data() + text.size());
    return w;
}

// ---------------------------------------------------------------- controls and focus

// A control: takes its place in this frame's focus order and says what is happening to it.
struct Hit {
    bool focused{}, pointed{}, press{};
};
Hit item(Ui &f, std::uint32_t id, ImVec2 a, ImVec2 b) {
    auto &h = f.h;
    h.items.push_back({id, a, b});
    const bool over = f.input && inside(f.mouse, a, b);
    if (over && f.moved) h.focus = id, h.ring = false;
    Hit hit;
    // Lit by the pointer only while the pointer is in charge, as on ReSkate's other pages.
    hit.pointed = over && !h.ring;
    hit.focused = h.focus == id && h.ring;
    hit.press = f.input && ((over && f.clicked) || h.fire == id);
    if (hit.press) h.focus = id, h.sound = sound_select;
    return hit;
}
// A button: `primary` on the blue, else a dark one; white with dark ink under the D-pad or the keys.
bool button(Ui &f, std::uint32_t id, ImVec2 a, ImVec2 b, std::string_view text, bool primary, bool enabled = true) {
    const float u = f.u, size = std::min(36 * u, (b.y - a.y) * .44f);
    const Hit hit = enabled ? item(f, id, a, b) : Hit{};
    ImU32 fill = primary ? (hit.pointed ? skate_theme::blue_hover : skate_theme::blue) : hit.pointed ? card_pointed : button_dark;
    ImU32 ink = enabled ? theme::paper : theme::muted;
    if (!enabled) fill = primary ? IM_COL32(1, 131, 255, 90) : button_dark;
    if (hit.focused) fill = theme::paper, ink = focus_ink;
    skate_theme::rough_rect(f.draw, a, b, fill, id & 0xfff, u * 2);
    const float w = width_of(f.bold, size, text);
    label(f.draw, f.bold, size, ImVec2(a.x + std::max(16 * u, (b.x - a.x - w) * .5f), a.y + (b.y - a.y - size) * .5f), b.x - 16 * u, ink, text);
    return hit.press;
}

// Moves the focus one control over, by where the controls were drawn last frame (as hub_page.cpp's
// move_focus does).
void move_focus(Home &h, int dx, int dy) {
    const Control *from{};
    for (const auto &entry : h.last)
        if (entry.id == h.focus) from = &entry;
    if (!from) {
        if (!h.last.empty()) h.focus = h.last.front().id;
        return;
    }
    const ImVec2 centre((from->a.x + from->b.x) * .5f, (from->a.y + from->b.y) * .5f);
    const Control *best{};
    float best_score = FLT_MAX;
    for (const auto &entry : h.last) {
        if (entry.id == from->id) continue;
        // Measured from the nearest point of the other control, so a wide card is as close as it looks.
        const ImVec2 closest(std::clamp(centre.x, entry.a.x, entry.b.x), std::clamp(centre.y, entry.a.y, entry.b.y));
        const float along = dx ? (closest.x - centre.x) * static_cast<float>(dx) : (closest.y - centre.y) * static_cast<float>(dy);
        const float across = dx ? std::abs(closest.y - centre.y) : std::abs(closest.x - centre.x);
        const float edge = dx ? (dx > 0 ? entry.a.x - from->b.x : from->a.x - entry.b.x) : (dy > 0 ? entry.a.y - from->b.y : from->a.y - entry.b.y);
        if (along <= 0 || edge < -2) continue;
        const float score = along + across * 2.5f;
        if (score < best_score) best_score = score, best = &entry;
    }
    if (best) h.focus = best->id;
}

// ---------------------------------------------------------------- what it shows

// The news as the HUB's own news tile had it: the live feed, else the one built into ReSkate.
void read_news(Home &h) {
    const auto now = GetTickCount64();
    if (h.posts_at && now < h.posts_at + 2000) return;
    h.posts_at = now;
    static const profile::Snapshot none;
    try {
        auto feed = news::current_news_feed(none);
        h.posts = feed.enabled ? std::move(feed.posts) : std::vector<profile::NewsPost>{};
        for (auto &post : h.posts) post.large_image = news::news_image_url(post.large_image.empty() ? post.small_image : post.large_image);
    } catch (...) {
        h.posts.clear();
    }
    if (h.featured >= h.posts.size()) h.featured = 0;
}
// How many installed mods Thunderstore has newer, from what the Mod Manager reads (mod_store.h).
void read_updates(Home &h) {
    const auto now = GetTickCount64();
    if (h.mods_at && now < h.mods_at + 2000) return;
    h.mods_at = now;
    h.listing = mod_store::listing();
    h.mods = mod_store::installed();
    if (!h.listing || !h.mods) return void(h.updates = 0);
    if (h.listing->serial == h.listing_serial && h.mods->serial == h.mods_serial) return;
    h.listing_serial = h.listing->serial, h.mods_serial = h.mods->serial;
    h.updates = static_cast<std::size_t>(std::count_if(h.listing->packages.begin(), h.listing->packages.end(),
                                                       [&](const ts::Package &package) { return ts::update_available(package, h.mods->versions); }));
}

// A post's picture, the middle of it cut to the box's shape; with none (or while it is on its way),
// a plain tile with ReSkate's name on it.
void picture(Ui &f, ImVec2 a, ImVec2 b, const std::string &url, unsigned seed) {
    ImVec2 size;
    if (const auto texture = url.empty() ? ImTextureID{} : news_picture(url, size); texture && size.x > 0 && size.y > 0) {
        const float box = (b.x - a.x) / std::max(1.f, b.y - a.y), shape = size.x / size.y;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (shape > box) uv0.x = (1 - box / shape) * .5f, uv1.x = 1 - uv0.x;
        else uv0.y = (1 - shape / box) * .5f, uv1.y = 1 - uv0.y;
        f.draw->AddImage(texture, a, b, uv0, uv1);
        return;
    }
    f.draw->AddRectFilled(a, b, seed & 1 ? placeholder_alt : placeholder);
    const float size_text = std::min(64 * f.u, (b.y - a.y) * .22f);
    constexpr std::string_view name = "RESKATE";
    const float w = width_of(f.heading, size_text, name);
    f.draw->AddText(f.heading, size_text, ImVec2(a.x + (b.x - a.x - w) * .5f, a.y + (b.y - a.y - size_text) * .5f), IM_COL32(245, 245, 245, 90),
                    name.data(), name.data() + name.size());
}

void open_post(Home &h, std::size_t index) {
    h.view = View::post;
    h.reading = index;
    h.scroll = 0;
    h.focus = 0;
    h.sound = sound_select;
}
void open_notes(Home &h) {
    h.view = View::notes;
    h.scroll = 0;
    h.focus = 0;
    h.sound = sound_select;
}
void edit_profile(Home &h) {
    multiplayer::open_native_profile();
    h.sound = sound_select;
}

// ---------------------------------------------------------------- the front page

// The big news card: the post's picture, its tag and date, title, description, READ and the pager.
void featured_card(Ui &f, ImVec2 a, ImVec2 z) {
    auto &h = f.h;
    const float u = f.u, inset = 52 * u;
    auto *draw = f.draw;
    skate_theme::rough_rect(draw, a, z, card, 0x3a1, u * 2);
    if (h.posts.empty()) {
        label(draw, f.heading, 64 * u, ImVec2(a.x + inset, a.y + inset), z.x - inset, theme::paper, "NO NEWS RIGHT NOW");
        label(draw, f.body, 34 * u, ImVec2(a.x + inset, a.y + inset + 100 * u), z.x - inset, sub_ink, "ReSkate's news shows here when there is some.");
        return;
    }
    const auto &post = h.posts[h.featured];
    const float split = a.x + (z.x - a.x) * .58f;
    picture(f, a, ImVec2(split, z.y), post.large_image, static_cast<unsigned>(h.featured));
    const float left = split + inset, right = z.x - inset;
    float y = a.y + inset;
    const float tag_w = tag(f, ImVec2(left, y), "NEWS", skate_theme::blue, theme::paper);
    if (const auto date = news::date_label(post.date); !date.empty())
        label(draw, f.bold, 28 * u, ImVec2(left + tag_w + 20 * u, y + 6 * u), right, sub_ink, date);
    y += 70 * u;
    // The pager and READ at the foot, and the title and description in what is left over them.
    const float foot = z.y - inset - 96 * u;
    const float title_size = 84 * u;
    for (const auto &line : wrap(f.title, title_size, post.title, right - left, 3)) {
        label(draw, f.title, title_size, ImVec2(left, y), right + 8 * u, theme::paper, line);
        y += title_size * .98f;
    }
    y += 24 * u;
    const float text_size = 34 * u, line_height = text_size * 1.42f;
    const auto most = static_cast<std::size_t>(std::max(0.f, (foot - 30 * u - y) / line_height));
    if (most)
        for (const auto &line : wrap(f.body, text_size, post.description, right - left, most)) {
            label(draw, f.body, text_size, ImVec2(left, y), right, soft, line);
            y += line_height;
        }
    if (button(f, hash("home-read"), ImVec2(left, foot), ImVec2(left + 260 * u, foot + 96 * u), "READ", true)) open_post(h, h.featured);
    if (h.posts.size() > 1) {
        const auto pages = "<  " + std::to_string(h.featured + 1) + " / " + std::to_string(h.posts.size()) + "  >";
        const float w = width_of(f.bold, 36 * u, pages) + 70 * u;
        if (button(f, hash("home-pager"), ImVec2(left + 280 * u, foot), ImVec2(left + 280 * u + w, foot + 96 * u), pages, false))
            h.featured = (h.featured + 1) % h.posts.size();
    }
}

// One of the small cards under it: a picture, a tag and a title. Returns whether it was pressed.
bool small_card(Ui &f, std::uint32_t id, ImVec2 a, ImVec2 z, std::string_view tag_text, ImU32 tag_ink, std::string_view title,
                const std::string &url, bool release, std::string_view version) {
    const float u = f.u, inset = 30 * u;
    const Hit hit = item(f, id, a, z);
    auto *draw = f.draw;
    skate_theme::rough_rect(draw, a, z, hit.focused ? theme::paper : hit.pointed ? card_pointed : card, id & 0xfff, u * 2);
    const float text_part = std::min(150 * u, (z.y - a.y) * .45f);
    const ImVec2 pa(a.x, a.y), pz(z.x, z.y - text_part);
    if (release) {
        // A release has no picture: its version, big, on a green tile.
        draw->AddRectFilled(pa, pz, release_tile);
        const float size = std::min(96 * u, (pz.y - pa.y) * .4f), w = width_of(f.heading, size, version);
        label(draw, f.heading, size, ImVec2(pa.x + std::max(inset, (pz.x - pa.x - w) * .5f), pa.y + (pz.y - pa.y - size) * .5f), pz.x - inset,
              theme::paper, version);
    } else {
        picture(f, pa, pz, url, id);
    }
    const ImU32 ink = hit.focused ? focus_ink : theme::paper;
    label(draw, f.bold, 26 * u, ImVec2(a.x + inset, pz.y + 22 * u), z.x - inset, hit.focused ? sub_ink_lit : tag_ink, tag_text);
    const auto lines = wrap(f.bold, 34 * u, title, z.x - a.x - inset * 2, 2);
    for (std::size_t i = 0; i < lines.size(); ++i)
        label(draw, f.bold, 34 * u, ImVec2(a.x + inset, pz.y + 60 * u + static_cast<float>(i) * 42 * u), z.x - inset, ink, lines[i]);
    return hit.press;
}

// PATCH NOTES: the latest release's version and highlights, as many as fit, and ALL PATCH NOTES at
// its foot. It takes whatever height the right column has left, so it ends level with the left.
void notes_card(Ui &f, ImVec2 a, ImVec2 z) {
    auto &h = f.h;
    const float u = f.u, inset = 40 * u;
    auto *draw = f.draw;
    skate_theme::rough_rect(draw, a, z, card, 0x3b2, u * 2);
    const float left = a.x + inset, right = z.x - inset;
    float y = a.y + inset;
    label(draw, f.heading, 48 * u, ImVec2(left, y), right, theme::paper, "PATCH NOTES");
    const news::Release *latest = h.notes && !h.notes->releases.empty() ? &h.notes->releases.front() : nullptr;
    if (latest) {
        const auto version = news::release_version(*latest);
        const float w = width_of(f.bold, 26 * u, version) + 24 * u;
        tag(f, ImVec2(right - w, y + 4 * u), version, skate_theme::good, skate_theme::black);
    }
    y += 76 * u;
    const float button_top = z.y - inset - 90 * u;
    if (button(f, hash("home-all-notes"), ImVec2(left, button_top), ImVec2(right, z.y - inset), "ALL PATCH NOTES", false, latest != nullptr)) open_notes(h);
    if (!latest) {
        const char *note = h.notes && h.notes->fetching ? "Reading ReSkate's releases..."
                           : launcher::offline_mode()  ? "Patch notes need a connection. They come from ReSkate's GitHub page."
                                                       : "Patch notes could not be read just now. They come from ReSkate's GitHub page.";
        for (const auto &line : wrap(f.body, 32 * u, note, right - left, 4)) label(draw, f.body, 32 * u, ImVec2(left, y), right, sub_ink, line), y += 46 * u;
        return;
    }
    {
        auto title = latest->name;
        if (const auto date = news::date_label(latest->date); !date.empty()) title += "  -  " + date;
        label(draw, f.bold, 32 * u, ImVec2(left, y), right, sub_ink, title);
        y += 62 * u;
    }
    // Each highlight whole, while it fits over the button.
    const float size = 32 * u, line_height = size * 1.4f, bottom = button_top - 24 * u, bullet_x = left + 34 * u;
    const auto highlights = news::release_highlights(*latest, 40);
    if (highlights.empty()) label(draw, f.body, size, ImVec2(left, y), right, sub_ink, "Read what changed in ALL PATCH NOTES.");
    for (const auto &highlight : highlights) {
        const auto lines = wrap(f.body, size, highlight, right - bullet_x);
        if (lines.empty() || y + line_height * static_cast<float>(lines.size()) > bottom) break;
        draw->AddCircleFilled(ImVec2(left + 10 * u, y + size * .55f), 6 * u, skate_theme::blue, 12);
        for (const auto &line : lines) label(draw, f.body, size, ImVec2(bullet_x, y), right, soft, line), y += line_height;
        y += 8 * u;
    }
}

// YOUR PROFILE: the player's name and what their card says, and EDIT PROFILE (Y from anywhere).
void profile_card(Ui &f, ImVec2 a, ImVec2 z, const Model &model) {
    auto &h = f.h;
    const float u = f.u, inset = 40 * u;
    auto *draw = f.draw;
    skate_theme::rough_rect(draw, a, z, card, 0x3c3, u * 2);
    const float side = z.y - a.y - inset * 2;
    const ImVec2 sa(a.x + inset, a.y + inset), sz(sa.x + side, sa.y + side);
    draw->AddRectFilled(sa, sz, skate_theme::avatar);
    auto name = !model.player_card.custom_name.empty() ? model.player_card.custom_name : model.multiplayer.local_name;
    if (name.empty()) name = "Skater";
    {
        const std::string letter(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name.front()))));
        const float size = side * .55f, w = width_of(f.heading, size, letter);
        draw->AddText(f.heading, size, ImVec2(sa.x + (side - w) * .5f, sa.y + (side - size) * .5f), skate_theme::black, letter.c_str());
    }
    const float button_w = std::min(340 * u, (z.x - a.x) * .4f);
    const ImVec2 ba(z.x - inset - button_w, a.y + (z.y - a.y - 96 * u) * .5f), bz(z.x - inset, ba.y + 96 * u);
    const float left = sz.x + 30 * u, right = ba.x - 24 * u;
    label(draw, f.bold, 40 * u, ImVec2(left, a.y + (z.y - a.y) * .5f - 50 * u), right, theme::paper, name);
    label(draw, f.body, 28 * u, ImVec2(left, a.y + (z.y - a.y) * .5f + 4 * u), right, sub_ink,
          model.player_card.custom_name.empty() ? "Your player card" : "Your player card, renamed");
    if (button(f, hash("home-edit-profile"), ba, bz, "EDIT PROFILE", true)) edit_profile(h);
}

// RIGHT NOW: what is going on that the player can act on, a row each; with nothing, no card. A
// friend's row opens the Multiplayer page's FRIENDS tab (JOIN is there), the mods' the Mod Manager's
// UPDATES.
struct NowRow {
    std::string text, does;
    ImU32 colour{};
    int page{};       // the pause menu page it opens (native_menu_view.h)
};
std::vector<NowRow> now_rows(const Home &h, const Model &model) {
    std::vector<NowRow> rows;
    const auto &mp = model.multiplayer;
    if (!model.steam_offline && !mp.friends_skating.empty()) {
        // A friend in a session the player can join first; else whoever is skating.
        const MultiplayerFriend *who = &mp.friends_skating.front();
        const MultiplayerLobby *where{};
        for (const auto &mate : mp.friends_skating)
            for (const auto &lobby : mp.lobbies)
                if (mate.session && lobby.id == mate.session && !where && multiplayer::menu_view::can_join(mp, lobby)) who = &mate, where = &lobby;
        const auto others = mp.friends_skating.size() - 1;
        auto text = multiplayer::menu_view::caption(who->name, 24) +
                    (where ? " is on " + multiplayer::menu_view::caption(where->name, 32) : std::string(" is skating"));
        if (!where && others) text += " with " + std::to_string(others) + (others == 1 ? " other friend" : " other friends");
        rows.push_back({std::move(text), where ? "JOIN" : "FRIENDS", skate_theme::blue, 0});
    }
    if (h.updates)
        rows.push_back({std::to_string(h.updates) + (h.updates == 1 ? " mod has an update" : " mods have updates"), "UPDATE", skate_theme::warning,
                        static_cast<int>(multiplayer::menu_view::mods_page)});
    return rows;
}
float now_height(std::size_t rows, float u) { return rows ? 40 * u + 64 * u + static_cast<float>(rows) * 110 * u + 30 * u : 0; }
void now_card(Ui &f, ImVec2 a, ImVec2 z, const std::vector<NowRow> &rows) {
    const float u = f.u, inset = 40 * u;
    auto *draw = f.draw;
    skate_theme::rough_rect(draw, a, z, card, 0x3d4, u * 2);
    label(draw, f.heading, 44 * u, ImVec2(a.x + inset, a.y + inset), z.x - inset, theme::paper, "RIGHT NOW");
    float y = a.y + inset + 64 * u;
    for (std::size_t i = 0; i < rows.size(); ++i, y += 110 * u) {
        const auto &row = rows[i];
        const ImVec2 ra(a.x + inset, y), rz(z.x - inset, y + 96 * u);
        const Hit hit = item(f, hash("home-now", i), ra, rz);
        skate_theme::rough_rect(draw, ra, rz, hit.focused ? theme::paper : hit.pointed ? card_pointed : button_dark, 0x500 + static_cast<unsigned>(i), u * 2);
        const float size = 32 * u, does_w = width_of(f.bold, size, row.does + "  >");
        label(draw, f.bold, size, ImVec2(ra.x + 28 * u, ra.y + (96 * u - size) * .5f), rz.x - does_w - 50 * u, hit.focused ? focus_ink : theme::paper, row.text);
        label(draw, f.bold, size, ImVec2(rz.x - 28 * u - does_w, ra.y + (96 * u - size) * .5f), rz.x, hit.focused ? focus_ink : row.colour, row.does + "  >");
        if (hit.press) {
            // The page that deals with it, on the tab that shows it.
            if (row.page == static_cast<int>(multiplayer::menu_view::mods_page)) show_mod_updates();
            else show_friends_tab();
            multiplayer::show_native_menu_page(row.page);
        }
    }
}

void front_page(Ui &f, ImVec2 a, ImVec2 z, const Model &model) {
    auto &h = f.h;
    const float u = f.u, gap = 30 * u;
    const float right_w = std::clamp((z.x - a.x) * .29f, 980 * u, 1200 * u);
    const float left_z = z.x - right_w - gap;
    // ---- left: the featured post, and the small cards under it
    const float small_h = std::clamp((z.y - a.y) * .3f, 320 * u, 440 * u);
    const float featured_bottom = z.y - small_h - gap;
    featured_card(f, a, ImVec2(left_z, featured_bottom));
    {
        struct Small {
            bool release{};
            std::size_t post{};
        };
        std::vector<Small> cards;
        if (h.notes && !h.notes->releases.empty()) cards.push_back({true, 0});
        for (std::size_t i = 1; i < h.posts.size() && cards.size() < small_cards; ++i) cards.push_back({false, (h.featured + i) % h.posts.size()});
        const float w = (left_z - a.x - gap * static_cast<float>(small_cards - 1)) / static_cast<float>(small_cards);
        for (std::size_t i = 0; i < cards.size(); ++i) {
            const float x = a.x + (w + gap) * static_cast<float>(i);
            const ImVec2 ca(x, featured_bottom + gap), cz(x + w, z.y);
            if (cards[i].release) {
                const auto &release = h.notes->releases.front();
                const auto version = news::release_version(release);
                if (small_card(f, hash("home-small-notes"), ca, cz, "PATCH " + version, skate_theme::good, release.name, {}, true, version)) open_notes(h);
            } else {
                const auto &post = h.posts[cards[i].post];
                const auto date = news::date_label(post.date);
                if (small_card(f, hash("home-small-post", cards[i].post), ca, cz, date.empty() ? "NEWS" : "NEWS  -  " + date, news_light, post.title,
                               post.large_image, false, {}))
                    open_post(h, cards[i].post);
            }
        }
    }
    // ---- right: patch notes (stretched), the profile, right now
    const float rx = left_z + gap, profile_h = 230 * u;
    const auto rows = now_rows(h, model);
    const float now_h = now_height(rows.size(), u);
    float y = z.y;
    if (now_h > 0) {
        now_card(f, ImVec2(rx, y - now_h), ImVec2(z.x, y), rows);
        y -= now_h + gap;
    }
    profile_card(f, ImVec2(rx, y - profile_h), ImVec2(z.x, y), model);
    y -= profile_h + gap;
    notes_card(f, ImVec2(rx, a.y), ImVec2(z.x, std::max(a.y + 300 * u, y)));
}

// ---------------------------------------------------------------- the reader

// The open post or the patch notes, laid out for `width` (again only when either changes).
void lay_out(Ui &f, float width) {
    auto &h = f.h;
    const float u = f.u;
    const auto key = std::to_string(static_cast<int>(h.view)) + "/" + std::to_string(h.reading) + "/" + std::to_string(h.notes ? h.notes->serial : 0) +
                     "/" + std::to_string(h.posts.size()) + "/" + std::to_string(static_cast<int>(width));
    if (key == h.laid_key) return;
    h.laid_key = key;
    h.laid.clear();
    using Look = Laid::Look;
    const auto add = [&](Look look, ImFont *font, float size, std::string_view text, float indent, float spacing, std::size_t release = 0) {
        bool first = true;
        for (auto &line : wrap(font, size, text, width - indent))
            h.laid.push_back({look, std::move(line), {}, std::exchange(first, false), size * spacing, release, size});
    };
    if (h.view == View::post && h.reading < h.posts.size()) {
        const auto &post = h.posts[h.reading];
        h.laid.push_back({Look::picture, post.large_image, {}, true, std::min(width * 9.f / 16.f, 760 * u)});
        h.laid.push_back({Look::gap, {}, {}, true, 40 * u});
        add(Look::title, f.title, 96 * u, post.title, 0, .98f);
        h.laid.push_back({Look::meta, news::date_label(post.date), {}, true, 70 * u});
        h.laid.push_back({Look::gap, {}, {}, true, 20 * u});
        const std::string_view body = post.body.empty() ? std::string_view(post.description) : std::string_view(post.body);
        for (std::size_t at = 0; at <= body.size();) {
            const auto end = std::min(body.find('\n', at), body.size());
            const auto paragraph = body.substr(at, end - at);
            if (paragraph.find_first_not_of(" \t\r") == std::string_view::npos) h.laid.push_back({Look::gap, {}, {}, true, 26 * u});
            else add(Look::text, f.body, 36 * u, paragraph, 0, 1.45f);
            at = end + 1;
        }
        return;
    }
    if (!h.notes) return;
    for (std::size_t r = 0; r < h.notes->releases.size(); ++r) {
        const auto &release = h.notes->releases[r];
        if (r) h.laid.push_back({Look::gap, {}, {}, true, 60 * u, r});
        h.laid.push_back({Look::release, release.name, news::date_label(release.date), true, 110 * u, r});
        for (const auto &line : release.lines) {
            switch (line.kind) {
            case Kind::heading: h.laid.push_back({Look::gap, {}, {}, true, 14 * u, r}), add(Look::heading, f.bold, 42 * u, line.text, 0, 1.4f, r); break;
            case Kind::bullet: add(Look::bullet, f.body, 34 * u, line.text, 52 * u, 1.42f, r); break;
            case Kind::code: add(Look::code, f.body, 32 * u, line.text, 40 * u, 1.4f, r); break;
            case Kind::rule: h.laid.push_back({Look::rule, {}, {}, true, 34 * u, r}); break;
            case Kind::gap: h.laid.push_back({Look::gap, {}, {}, true, 22 * u, r}); break;
            default: add(Look::text, f.body, 34 * u, line.text, 0, 1.42f, r); break;
            }
        }
    }
}

void reader(Ui &f, ImVec2 a, ImVec2 z, int step, int page_step) {
    auto &h = f.h;
    const float u = f.u, inset = 60 * u;
    auto *draw = f.draw;
    skate_theme::rough_rect(draw, a, z, card, 0x3e5, u * 2);
    const bool post = h.view == View::post;
    // ---- its bar: what it is, the pager (a post) and BACK
    const float bar = 96 * u, top = a.y + inset;
    {
        float x = a.x + inset;
        x += tag(f, ImVec2(x, top + 28 * u), post ? "NEWS" : "PATCH NOTES", post ? skate_theme::blue : skate_theme::good,
                 post ? theme::paper : skate_theme::black) + 24 * u;
        if (post && h.posts.size() > 1) {
            const auto pages = "<  " + std::to_string(h.reading + 1) + " / " + std::to_string(h.posts.size()) + "  >";
            const float w = width_of(f.bold, 36 * u, pages) + 70 * u;
            if (button(f, hash("home-reader-pager"), ImVec2(x, top), ImVec2(x + w, top + bar), pages, false)) page_step = 1;
        }
        if (button(f, hash("home-reader-back"), ImVec2(z.x - inset - 300 * u, top), ImVec2(z.x - inset, top + bar), "BACK", false)) {
            h.view = View::front;
            h.focus_next = post ? hash("home-read") : hash("home-all-notes");
            return;
        }
    }
    if (post && page_step && h.posts.size() > 1) {
        h.reading = (h.reading + h.posts.size() + (page_step > 0 ? 1 : h.posts.size() - 1)) % h.posts.size();
        h.scroll = 0;
        h.sound = sound_tab;
    }
    // ---- the text, in a column that scrolls
    const ImVec2 ba(a.x + inset, top + bar + 40 * u), bz(z.x - inset, z.y - inset);
    const float column = std::min(bz.x - ba.x - 40 * u, 2400 * u);
    lay_out(f, column);
    float content{};
    for (const auto &line : h.laid) content += line.height;
    // LT / RT in the notes: the release before or after the one at the top.
    if (!post && page_step) {
        float at{};
        std::vector<float> tops;
        for (const auto &line : h.laid) {
            if (line.look == Laid::Look::release) tops.push_back(at);
            at += line.height;
        }
        if (page_step > 0) {
            for (const float t : tops)
                if (t > h.scroll + 2) { h.scroll = t; break; }
        } else {
            float before{};
            for (const float t : tops)
                if (t < h.scroll - 2) before = t;
            h.scroll = before;
        }
        h.sound = sound_tab;
    }
    const float view_height = bz.y - ba.y, most = std::max(0.f, content - view_height);
    h.scroll += static_cast<float>(step) * 150 * u;
    if (f.input && inside(f.mouse, ba, bz) && f.wheel != 0) h.scroll -= f.wheel * 170 * u;
    h.scroll = std::clamp(h.scroll, 0.f, most);
    draw->PushClipRect(ba, bz, true);
    float y = ba.y - h.scroll;
    const float x = ba.x;
    using Look = Laid::Look;
    for (const auto &line : h.laid) {
        const float height = line.height;
        if (y + height < ba.y || y > bz.y) {
            y += height;
            continue;
        }
        switch (line.look) {
        case Look::picture: picture(f, ImVec2(x, y), ImVec2(x + column, y + height), line.text, static_cast<unsigned>(h.reading)); break;
        case Look::title: draw->AddText(f.title, 96 * u, ImVec2(x, y), theme::paper, line.text.c_str()); break;
        case Look::meta: draw->AddText(f.bold, 30 * u, ImVec2(x, y + 16 * u), sub_ink, line.text.c_str()); break;
        case Look::release: {
            const auto &release = h.notes->releases[line.release];
            const auto version = news::release_version(release);
            const float w = tag(f, ImVec2(x, y + 14 * u), version, skate_theme::good, skate_theme::black);
            label(draw, f.heading, 52 * u, ImVec2(x + w + 24 * u, y + 6 * u), x + column, theme::paper, line.text);
            if (!line.extra.empty()) draw->AddText(f.bold, 28 * u, ImVec2(x, y + 68 * u), sub_ink, line.extra.c_str());
            break;
        }
        case Look::heading: draw->AddText(f.bold, 42 * u, ImVec2(x, y), theme::paper, line.text.c_str()); break;
        case Look::bullet:
            if (line.first) draw->AddCircleFilled(ImVec2(x + 18 * u, y + 34 * u * .55f), 6 * u, skate_theme::blue, 12);
            draw->AddText(f.body, 34 * u, ImVec2(x + 52 * u, y), soft, line.text.c_str());
            break;
        case Look::code:
            draw->AddRectFilled(ImVec2(x + 12 * u, y), ImVec2(x + column, y + height), IM_COL32(10, 10, 10, 200));
            draw->AddText(f.body, 32 * u, ImVec2(x + 40 * u, y), IM_COL32(190, 220, 255, 255), line.text.c_str());
            break;
        case Look::rule: draw->AddRectFilled(ImVec2(x, y + 15 * u), ImVec2(x + column, y + 18 * u), skate_theme::tile_light); break;
        case Look::text: draw->AddText(f.body, line.size, ImVec2(x, y), soft, line.text.c_str()); break;
        case Look::gap: break;
        }
        y += height;
    }
    draw->PopClipRect();
    if (most > 0) {
        const float thumb = std::max(60 * u, view_height * view_height / content), at = ba.y + (view_height - thumb) * (h.scroll / most);
        draw->AddRectFilled(ImVec2(bz.x - 10 * u, at), ImVec2(bz.x - 2 * u, at + thumb), theme::muted, 4 * u);
    }
}

// The keycaps at the foot: what the controller's (or the keyboard's) buttons do here.
void hints(Ui &f, float x, float y, bool reading) {
    struct Hint {
        std::uint32_t pad;
        const char *key, *what;
    };
    const std::vector<Hint> list = reading ? std::vector<Hint>{{pad_b, "Esc", "Back"}, {pad_up | pad_down, "", "Scroll"}, {pad_lt | pad_rt, "Z C", "Next"}}
                                           : std::vector<Hint>{{pad_a, "Enter", "Open"}, {pad_y, "", "Edit profile"}, {pad_lt | pad_rt, "Z C", "Next news"}};
    for (const auto &hint : list) {
        std::string name;
        if (f.pad.available) {
            // Two buttons at once read as a combination; these are either one.
            for (const std::uint32_t bit : {pad_up, pad_down, pad_lt, pad_rt, pad_a, pad_b, pad_y})
                if (hint.pad & bit) name += (name.empty() ? "" : " ") + controller_combo_label(bit, f.pad.style);
        } else {
            name = hint.key;
        }
        if (!name.empty()) x += theme::keycap(f.draw, f.bold, f.u * 2, ImVec2(x, y), name.c_str(), hint.what) + 36 * f.u;
    }
}
} // namespace

bool draw_home_page(ImDrawList *draw, float u, ImVec2 a, ImVec2 z, bool input, const Model &model) {
    auto &h = home();
    auto &s = state();
    auto &io = ImGui::GetIO();
    const auto tick = GetTickCount64();
    // Shown again after a while away: what is held as it comes up is not a press on it (the Back
    // that closed the game's side menu over it, the A that picked the HUB tab), and it opens on the
    // front page.
    if (!h.seen || tick - h.seen > 250) {
        h.keys_before.fill(true);
        h.pad_before = ~0U;
        h.view = View::front;
        h.focus = h.focus_next = 0;
        h.ring = false;
        h.last.clear();
        h.hold_until = 0;
    }
    h.seen = tick;
    Ui f{h, draw, s.menu.bold ? s.menu.bold : ImGui::GetFont(), s.menu.body ? s.menu.body : ImGui::GetFont(), nullptr, nullptr, u, io.MousePos,
            input, false, false, 0, {}};
    f.heading = s.menu.heading ? s.menu.heading : f.bold;
    f.title = s.menu.title_large ? s.menu.title_large : s.menu.title ? s.menu.title : f.heading;
    f.clicked = input && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    f.moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    f.wheel = input ? io.MouseWheel : 0;
    read_news(h);
    h.notes = news::patch_notes();
    read_updates(h);
    if (h.view == View::post && h.reading >= h.posts.size()) h.view = View::front;
    if (h.view == View::notes && (!h.notes || h.notes->releases.empty())) h.view = View::front;

    // ---- the controller and the keys, watched while the game has them
    if (input) DingoSDKOverlayReadControllerInput(&f.pad, true);
    if (!f.pad.available) f.pad.buttons = 0;
    const std::uint32_t pressed = f.pad.buttons & ~h.pad_before;
    std::array<bool, key_codes.size()> keys{};
    if (input) {
        OverlayInputAccess access;
        for (std::size_t i = 0; i < key_codes.size(); ++i) keys[i] = (GetAsyncKeyState(key_codes[i]) & 0x8000) != 0;
    }
    const auto key_new = [&](std::size_t i) { return keys[i] && !h.keys_before[i]; };
    h.fire = 0;
    if (f.moved) h.ring = false;
    h.focus_moved = false;
    const bool reading = h.view != View::front;
    bool dismiss{};
    int scroll_step{}, page_step{};
    const bool back_down = keys[7] || (f.pad.buttons & pad_b);
    if (input) {
        if (key_new(7) || (pressed & pad_b)) {
            // Back closes the reader (the HUB's Back is held meanwhile), else the pause menu: the
            // game does that, and this goes at once rather than when the game has said so.
            if (reading) {
                h.focus_next = h.view == View::post ? hash("home-read") : hash("home-all-notes");
                h.view = View::front;
                h.hold_until = tick + 300;
                h.sound = sound_select;
            } else if (tick >= h.hold_until) {
                dismiss = true;
            }
        }
        const std::array<bool, 4> held{keys[0] || (f.pad.buttons & pad_up), keys[1] || (f.pad.buttons & pad_down),
                                       keys[2] || (f.pad.buttons & pad_left), keys[3] || (f.pad.buttons & pad_right)};
        const std::array<bool, 4> fresh{key_new(0) || (pressed & pad_up), key_new(1) || (pressed & pad_down), key_new(2) || (pressed & pad_left),
                                        key_new(3) || (pressed & pad_right)};
        int direction = -1;
        for (int i = 0; i < 4; ++i)
            if (fresh[static_cast<std::size_t>(i)]) direction = i, h.repeat_at = tick + 380;
        if (direction < 0 && tick >= h.repeat_at)
            for (int i = 0; i < 4; ++i)
                if (held[static_cast<std::size_t>(i)]) direction = i, h.repeat_at = tick + (reading ? 60 : 70);
        const bool on_pager = h.focus == hash("home-pager") || h.focus == hash("home-reader-pager");
        if (direction >= 0 && reading && direction < 2) {
            // Up and down read on down the page.
            scroll_step = direction == 0 ? -1 : 1;
        } else if (direction >= 0 && on_pager && h.ring && direction >= 2) {
            // Left and right on the pager turn it.
            page_step = direction == 3 ? 1 : -1;
            h.repeat_at = std::max(h.repeat_at, tick + 260);
        } else if (direction >= 0) {
            // The first press only shows where the focus is.
            if (h.ring || !h.focus) move_focus(h, direction == 2 ? -1 : direction == 3 ? 1 : 0, direction == 0 ? -1 : direction == 1 ? 1 : 0);
            h.ring = true;
            h.focus_moved = true;
        }
        if (key_new(4) || (pressed & pad_a))
            if (h.focus && std::any_of(h.last.begin(), h.last.end(), [&](const Control &entry) { return entry.id == h.focus; })) h.fire = h.focus, h.ring = true;
        if (key_new(5) || (pressed & pad_lt)) page_step = -1;
        if (key_new(6) || (pressed & pad_rt)) page_step = 1;
        if (pressed & pad_y) {
            if (reading) h.view = View::front;
            edit_profile(h);
        }
    }
    // EDIT PROFILE that nothing the game was asked could do (native_hub.cpp): View, as the player
    // would press it, opens their card from any page of the pause menu.
    if (multiplayer::native_profile_failed() && multiplayer::native_pause_menu_open()) {
        logging::log(logging::Level::info, logging::Channel::ui, "Front page: EDIT PROFILE pressed View for the player.");
        XINPUT_GAMEPAD view{}, neutral{};
        view.wButtons = XINPUT_GAMEPAD_BACK;
        play_pad({{0, view}, {6, neutral}, {12, neutral}});
    }
    if (back_down && tick < h.hold_until) h.hold_until = tick + 300;
    multiplayer::hold_hub_back(h.view != View::front || tick < h.hold_until);
    h.keys_before = keys;
    if (!input) h.keys_before.fill(true), h.pad_before = ~0U;
    else h.pad_before = f.pad.buttons;
    h.items.clear();

    // ---- the page
    const float footer = 70 * u;
    const ImVec2 body_z(z.x, z.y - footer);
    if (h.view == View::front) {
        if (page_step && h.posts.size() > 1) {
            h.featured = (h.featured + h.posts.size() + (page_step > 0 ? 1 : h.posts.size() - 1)) % h.posts.size();
            h.sound = sound_tab;
        }
        front_page(f, a, body_z, model);
    } else {
        reader(f, a, body_z, scroll_step, page_step);
    }
    hints(f, a.x, z.y - 52 * u, h.view != View::front);

    // A press that found nothing to land on is forgotten; the focus stays on a control that is still
    // there, or goes to the first one once the D-pad or the keys are in charge.
    if (h.focus_next) {
        h.focus = std::exchange(h.focus_next, 0);
        h.ring = h.focus_moved = true;
    } else {
        if (h.focus && std::none_of(h.items.begin(), h.items.end(), [&](const Control &entry) { return entry.id == h.focus; })) h.focus = 0;
        if (!h.focus && h.ring && !h.items.empty()) h.focus = h.items.front().id;
    }
    h.last = h.items;
    if (h.sound == ~0U && h.focus && h.focus != h.focus_heard && input && h.ring) h.sound = sound_navigate;
    h.focus_heard = h.focus;
    if (const auto wanted = std::exchange(h.sound, ~0U); wanted != ~0U && input) {
        if (wanted != sound_navigate || tick - h.sound_at >= 45) play_ui_sound(wanted), h.sound_at = tick;
    }
    return dismiss;
}
} // namespace dingosdk::overlay::detail
