#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

// What plugins show, handed from the client thread (where plugins run) to the render thread (the
// overlay draws it): each plugin's shapes for this frame, and its own page in the menu's PLUGINS
// section (reskate.menu.page, and its panels as cards on it) with what the player did there on the
// way back. Header only, so the overlay and
// the plugin host share it without depending on each other.
namespace dingosdk::plugin_ui {
using Vec3 = std::array<float, 3>;

struct Style {
    std::uint32_t colour = 0xffffffff; // ImGui's IM_COL32 order: 0xAABBGGRR
    float size = 18;                   // text height, in pixels at 1080 lines
    float thickness = 2;
    bool fill{};
    float rounding{};
    int align{};                       // text: 0 left, 1 centre, 2 right of its point
    bool shadow = true;
    std::array<float, 4> uv{0, 0, 1, 1}; // pictures: the part of the picture drawn (u0, v0, u1, v1)
    int font{};                          // text: 0 bold, 1 body, 2 heading, 3 title, 4 title_large (the menus' fonts)
    float wrap{};                        // text: wrapped to this width (pixels at 1080 lines); 0 one line
};
// The skater's bodies by the ragdoll's order (Engine/Game/Skater/skater_body.h's names): d:skeleton's paint.
inline constexpr std::array<std::string_view, 24> body_names{
    "Board", "Head", "Neck", "Left hand", "Left forearm", "Left upper arm", "Left collarbone",
    "Right hand", "Right forearm", "Right upper arm", "Right collarbone", "Upper chest", "Chest",
    "Lower back", "Spine", "Left toes", "Left foot", "Left shin", "Left thigh",
    "Right toes", "Right foot", "Right shin", "Right thigh", "Pelvis"};
// The game's own pictures a plugin draws by name (game_image; Extension/UI/Overlay/game_images.h).
inline constexpr std::array<std::string_view, 31> game_images{
    "stopwatch", "wipeout", "wipeout_broken", "spread_eagle", "airtime", "gap_height", "flaming_wheel", "roll",
    "thrasher", "brush_bar", "rough_tile", "scratches", "streak", "loading_board",
    // The customization menu's category icons (its tabs'), white on clear.
    "cas_tops", "cas_bottoms", "cas_shoes", "cas_outfit", "cas_fullbody", "cas_headwear", "cas_headaccessories", "cas_eyewear", "cas_handwear", "cas_socks", "cas_deck", "cas_trucks", "cas_wheels", "cas_gripcolor", "cas_grippattern", "cas_sticker", "cas_decorator_vfxcosmetic"};
// The loading board is a flipbook: this many frames, at 30 a second (style.frame).
inline constexpr int loading_board_frames = 27;
// Screen shapes are in pixels from the top left; world shapes in world metres, projected through
// the camera as the frame is drawn, and left out behind it.
struct Shape {
    // image: `text` is the picture's file, points[0] and [1] its corners; world_image: centred on
    // points[0], `radius` metres tall. game_image: `text` names one of game_images, `fit` 0 contain,
    // 1 shape, 2 panel (its corners `edge` pixels). gradient: points[0] and [1] the corners, `colours`
    // the top left, top right, bottom right and bottom left. skeleton: the local skater's x-ray, a
    // colour for each body in `colours` (body_names' order; 0 leaves it out), the style's alpha
    // fading the whole.
    // rough_rect: skate.'s hand-cut tile from points[0] to [1], `seed` its edge, `radius` its scale.
    // halftone: a screen-print of `columns` x `rows` dots from points[0]. scribble: the dry-brush
    // scribble over points[0] to [1]. clip: what follows is clipped to points[0] to [1], until unclip.
    enum class Kind { text, rect, line, circle, poly, world_text, world_line, world_circle, image, world_image, game_image, gradient, skeleton,
                      rough_rect, halftone, scribble, clip, unclip } kind{Kind::text};
    std::vector<Vec3> points;
    float radius{};
    std::string text;
    Style style;
    std::vector<std::uint32_t> colours;
    int fit{};
    float edge{};
    int frame{}; // game_image: a flipbook's frame (the loading board's, 0 to 26)
    std::uint32_t seed{};
    int columns{}, rows{};
};
struct Layer {
    std::string plugin;
    std::vector<Shape> shapes;
};

struct Widget {
    // bar: a read-only gauge, `value` between `minimum` and `maximum` (an axis, a level). card: a
    // titled tile the widgets after it go in (`help` its subtitle). note / warn: muted help text, a
    // warning. info: `text` and its value in `help`. tabs: the page's tabs, `choices` and `chosen`.
    // toggle and dropdown: `help` says what it does. slider: `format` shows its value ("%.0f %%").
    // button: `on` makes it the card's main action.
    enum class Kind { heading, label, separator, button, toggle, slider, dropdown, bar, card, note, warn, info, tabs } kind{Kind::label};
    std::string id, text;
    bool on{};
    float value{}, minimum{}, maximum{1};
    std::vector<std::string> choices;
    int chosen{};
    std::string help, format;
};
struct Panel {
    std::string plugin, title;
    std::vector<Widget> widgets;
    bool screen{}; // a full-screen screen (reskate.ui.screen)
    int dock{};    // a screen docked to a side, the game showing beside it: 1 left, 2 right; 0 full screen;
                   // 3 no panel at all (the plugin draws its own with the draw handler)
    float width{}; // a docked screen's share of the window's width
    bool page{};   // the plugin's page (reskate.menu.page); else a panel, a card on that page
};
// A plugin with a page in the menu's PLUGINS section: its id, and the page's name there (its page's
// title, else the plugin's id when it only has panels).
struct PageEntry {
    std::string plugin, title;
    bool operator==(const PageEntry&) const = default;
};
// The widget a full-screen screen's Esc sends.
inline constexpr char close_widget[] = "__close";
// A plugin's bind (reskate.input.bind) as the Binds page lists it: the plugin's own combo, and the
// one in use (the player's, when they recorded or cleared one; 0 is none).
struct Bind {
    std::string plugin, action;
    std::uint32_t own{}, combo{};
};
// The Binds page's change to one: a combo (0 clears it), or nullopt for the plugin's own again.
struct Rebind {
    std::string plugin, action;
    std::optional<std::uint32_t> combo;
};
// What the player did to a widget: pressed a button, or set a toggle, slider or dropdown.
struct Action {
    std::string plugin, panel, widget;
    bool on{};
    float value{};
    int chosen{};
};

namespace detail {
inline std::mutex mutex;
inline std::shared_ptr<const std::vector<Layer>> layers = std::make_shared<std::vector<Layer>>();
inline std::shared_ptr<const std::vector<Panel>> panels = std::make_shared<std::vector<Panel>>();
inline std::shared_ptr<const std::vector<PageEntry>> pages = std::make_shared<std::vector<PageEntry>>();
inline std::string shown_page; // under mutex: the plugin whose page the menu shows
inline std::vector<Action> actions;
inline std::shared_ptr<const std::vector<Bind>> binds = std::make_shared<std::vector<Bind>>();
inline std::vector<Rebind> rebinds;
inline std::atomic<std::int64_t> panels_seen_ms{};
inline std::atomic<float> display_width{1920}, display_height{1080};
// The menus' four fonts as the overlay has them (set_font): each printable character's advance at the
// font's own size, for d:measure.
struct FontMetrics {
    float size{};
    std::array<float, 96> advance{}; // characters 32 to 127
};
inline std::array<FontMetrics, 5> fonts{};
inline std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

// Client thread: this tick's shapes and panels. Render thread: the latest of each.
inline void publish_layers(std::vector<Layer> value) {
    auto shared = std::make_shared<const std::vector<Layer>>(std::move(value));
    std::lock_guard lock(detail::mutex);
    detail::layers = std::move(shared);
}
inline std::shared_ptr<const std::vector<Layer>> latest_layers() {
    std::lock_guard lock(detail::mutex);
    return detail::layers;
}
inline void publish_panels(std::vector<Panel> value) {
    auto shared = std::make_shared<const std::vector<Panel>>(std::move(value));
    std::lock_guard lock(detail::mutex);
    detail::panels = std::move(shared);
}
inline std::shared_ptr<const std::vector<Panel>> latest_panels() {
    std::lock_guard lock(detail::mutex);
    return detail::panels;
}
// Client thread, whenever they change: the plugins with a page. Render thread: the latest.
inline void publish_pages(std::vector<PageEntry> value) {
    auto shared = std::make_shared<const std::vector<PageEntry>>(std::move(value));
    std::lock_guard lock(detail::mutex);
    detail::pages = std::move(shared);
}
inline std::shared_ptr<const std::vector<PageEntry>> latest_pages() {
    std::lock_guard lock(detail::mutex);
    return detail::pages;
}
// Render thread: a widget changed. At most 64 wait, so a stuck client thread holds no more.
inline void act(Action action) {
    std::lock_guard lock(detail::mutex);
    if (detail::actions.size() < 64) detail::actions.push_back(std::move(action));
}
inline std::vector<Action> take_actions() {
    std::lock_guard lock(detail::mutex);
    return std::exchange(detail::actions, {});
}
// Client thread: every plugin's binds, whenever they change.
inline void publish_binds(std::vector<Bind> value) {
    auto shared = std::make_shared<const std::vector<Bind>>(std::move(value));
    std::lock_guard lock(detail::mutex);
    detail::binds = std::move(shared);
}
inline std::shared_ptr<const std::vector<Bind>> latest_binds() {
    std::lock_guard lock(detail::mutex);
    return detail::binds;
}
// Render thread: the player recorded, cleared or reset a plugin's bind.
inline void rebind(Rebind change) {
    std::lock_guard lock(detail::mutex);
    if (detail::rebinds.size() < 64) detail::rebinds.push_back(std::move(change));
}
inline std::vector<Rebind> take_rebinds() {
    std::lock_guard lock(detail::mutex);
    return std::exchange(detail::rebinds, {});
}
// Render thread, each frame a plugin's page shows: pages are built only while one is open, and then
// only that plugin's.
inline void note_page_shown(std::string plugin) {
    detail::panels_seen_ms.store(detail::now_ms(), std::memory_order_relaxed);
    std::lock_guard lock(detail::mutex);
    if (detail::shown_page != plugin) detail::shown_page = std::move(plugin);
}
inline bool panels_shown() { return detail::now_ms() - detail::panels_seen_ms.load(std::memory_order_relaxed) < 500; }
inline std::string shown_page() {
    std::lock_guard lock(detail::mutex);
    return detail::shown_page;
}
// Render thread, each frame: the game window's size, which plugins draw in.
inline void set_display(float width, float height) {
    detail::display_width.store(width, std::memory_order_relaxed);
    detail::display_height.store(height, std::memory_order_relaxed);
}
inline std::array<float, 2> display() {
    return {detail::display_width.load(std::memory_order_relaxed), detail::display_height.load(std::memory_order_relaxed)};
}
// Render thread, when the overlay's fonts are (re)built.
inline void set_font(int font, const detail::FontMetrics& metrics) {
    if (font < 0 || font > 4) return;
    std::lock_guard lock(detail::mutex);
    detail::fonts[static_cast<std::size_t>(font)] = metrics;
}
// How big `text` is drawn in `font` at `size` pixels (the window's, not 1080's), wrapped to `wrap`
// pixels when that is more than 0, as the overlay draws it. Before the fonts are known, a guess.
inline std::array<float, 2> measure(int font, float size, std::string_view text, float wrap) {
    detail::FontMetrics m;
    {
        std::lock_guard lock(detail::mutex);
        m = detail::fonts[static_cast<std::size_t>(std::clamp(font, 0, 4))];
    }
    const float scale = m.size > 0 ? size / m.size : 1.0f;
    const auto advance = [&](char c) {
        const auto u = static_cast<unsigned char>(c);
        if (m.size <= 0) return size * 0.55f;
        return (u >= 32 && u < 128 ? m.advance[u - 32] : m.advance['?' - 32]) * scale;
    };
    float widest = 0, line = 0;
    int lines = 1;
    std::size_t at = 0;
    while (at < text.size()) {
        if (text[at] == '\n') {
            widest = std::max(widest, line);
            line = 0;
            ++lines;
            ++at;
            continue;
        }
        // A word and the spaces before it: it goes on a new line when it would pass the wrap.
        std::size_t end = at;
        float word = 0;
        while (end < text.size() && text[end] == ' ') word += advance(text[end++]);
        while (end < text.size() && text[end] != ' ' && text[end] != '\n') word += advance(text[end++]);
        if (wrap > 0 && line > 0 && line + word > wrap) {
            widest = std::max(widest, line);
            ++lines;
            float trimmed = word;
            for (std::size_t i = at; i < end && text[i] == ' '; ++i) trimmed -= advance(' ');
            line = trimmed;
        } else line += word;
        at = end;
    }
    widest = std::max(widest, line);
    return {widest, static_cast<float>(lines) * size};
}
}
