#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
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
// A text's font (Style::font) from this number on is one of the plugin's own, a file of its package
// that its manifest's "fonts" names: number own_fonts + i is the layer's fonts[i].
inline constexpr int own_fonts = 5;

struct Style {
    std::uint32_t colour = 0xffffffff; // ImGui's IM_COL32 order: 0xAABBGGRR
    float size = 18;                   // text height, in pixels at 1080 lines
    float thickness = 2;
    bool fill{};
    float rounding{};
    int align{};                       // text: 0 left, 1 centre, 2 right of its point
    bool shadow = true;
    std::array<float, 4> uv{0, 0, 1, 1}; // pictures: the part of the picture drawn (u0, v0, u1, v1)
    int font{};                          // text: 0 bold, 1 body, 2 heading, 3 title, 4 title_large (the menus' fonts);
                                         // own_fonts on, a font of the plugin's own
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
    // poly: `fit` 1 leaves an outline open (a line through the points, not back to the first).
    // mark: a place in the drawing, `seed` its number. adjust: everything drawn since mark `seed`, turned
    // `turn` radians about points[0], then moved by points[1], its opacity times `alpha`; with `fit` (1 the
    // top edge, 2 the bottom, 3 both) faded out over `edge` pixels towards points[2].x (the top) and
    // points[2].y (the bottom), triangles taller than points[2].z left as they are (0: none are).
    enum class Kind { text, rect, line, circle, poly, world_text, world_line, world_circle, image, world_image, game_image, gradient, skeleton,
                      rough_rect, halftone, scribble, clip, unclip, game_map, mark, adjust, mesh } kind{Kind::text};
    std::vector<Vec3> points;
    float radius{};
    std::string text;
    Style style;
    std::vector<std::uint32_t> colours;
    int fit{};
    float edge{};
    int frame{}; // game_image: a flipbook's frame (the loading board's, 0 to 26)
    // game_map: the game's own map of the level in the box points[0] to [1], about the world place
    // points[2]; `radius` the metres from the box's middle to its side, `turn` the heading that is
    // upward (radians).
    float turn{};
    std::uint32_t seed{};
    int columns{}, rows{};
    float alpha{1}; // adjust: the opacity of what was drawn since the mark, times this
    // mesh: triangles. `points` the corners, `colours` one a corner, `indices` three a triangle (into
    // points), and with a picture (`text` its file) `uvs` one a corner, the picture's place there.
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::uint16_t> indices;
};
struct Layer {
    std::string plugin;
    std::vector<Shape> shapes;
    // The plugin's own fonts (its manifest's "fonts"), in the manifest's order: each one's file, its
    // full path in UTF-8. None for a plugin without any.
    std::shared_ptr<const std::vector<std::string>> fonts;
};

struct Widget {
    // bar: a read-only gauge, `value` between `minimum` and `maximum` (an axis, a level). card: a
    // titled tile the widgets after it go in (`help` its subtitle). note / warn: muted help text, a
    // warning. info: `text` and its value in `help`. tabs: the page's tabs, `choices` and `chosen`.
    // toggle and dropdown: `help` says what it does. slider: `format` shows its value ("%.0f %%").
    // button: `on` makes it the card's main action. buttons: small buttons side by side, wrapping as the
    // card's width runs out, `choices` their names and `chosen` the one lit (0: none); pressing one sends
    // its number. text: a line to type in, `input` what it holds and `help` what it shows when empty.
    // colour: `colour` (red, green, blue, 0 to 1). stepper: `value` from `minimum` to `maximum` in
    // `step`s. section: a card that folds, `on` while open; the widgets after it go in it, until the
    // next card or section. list: one of `choices` (`chosen`, 0 for none), `rows` of them in view.
    enum class Kind { heading, label, separator, button, toggle, slider, dropdown, bar, card, note, warn, info, tabs, buttons, text, colour,
                      stepper, section, list } kind{Kind::label};
    std::string id, text;
    bool on{};
    float value{}, minimum{}, maximum{1};
    std::vector<std::string> choices;
    int chosen{};
    std::string help, format;
    std::string input;
    std::array<float, 3> colour{};
    float step{};
    int rows{};
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
// What the player did to a widget: pressed a button, or set a toggle, slider, dropdown, text,
// colour, stepper, section (`on`: open) or list.
struct Action {
    std::string plugin, panel, widget;
    bool on{};
    float value{};
    int chosen{};
    std::string text;
    std::array<float, 3> colour{};
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
    // Each of those characters' drawn box at the font's size, from the text's top left: left, top, right,
    // bottom (d:glyph; all 0 for one with nothing to draw, a space).
    std::array<std::array<float, 4>, 96> box{};
    // The characters past 127 the font has (accents, Cyrillic), by code point, with their advances.
    std::vector<std::pair<std::uint32_t, float>> wide;
    // A plugin's own font, read from its file by Windows (Extension/Plugins/plugin_font.h): the
    // characters 32 to 127 it has no glyph for are in `lacks`, and those and any past 127 not in
    // `wide` are drawn, and so measured, in "body". `ascent` is how far its baseline is below a
    // text's top, at the font's size.
    bool own{};
    std::array<bool, 96> lacks{};
    float ascent{};
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
// The code point at `at` of UTF-8 `text`, and `at` moved past it; a byte that starts none is itself.
inline std::uint32_t next_code_point(std::string_view text, std::size_t& at) {
    const auto byte = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[i])); };
    const auto first = byte(at);
    const std::size_t more = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : first >= 0xc0 ? 1 : 0;
    if (first < 0x80 || first >= 0xf8 || at + more >= text.size() + (more ? 0 : 1)) return ++at, first;
    std::uint32_t code = first & (0x3fu >> more);
    for (std::size_t i = 1; i <= more; ++i) {
        if ((byte(at + i) & 0xc0) != 0x80) return ++at, first;
        code = code << 6 | (byte(at + i) & 0x3f);
    }
    at += more + 1;
    return code;
}
// One character's advance in a font's metrics, at the font's own size; one the font lacks takes '?'s.
inline float advance_of(const detail::FontMetrics& m, std::uint32_t code) {
    if (code >= 32 && code < 128) return m.advance[code - 32];
    const auto found = std::lower_bound(m.wide.begin(), m.wide.end(), code, [](const auto& entry, std::uint32_t value) { return entry.first < value; });
    return found != m.wide.end() && found->first == code ? found->second : m.advance['?' - 32];
}
// Whether a plugin's own font has a glyph for the character.
inline bool has_glyph(const detail::FontMetrics& m, std::uint32_t code) {
    if (code < 32) return false;
    if (code < 128) return !m.lacks[code - 32];
    const auto found = std::lower_bound(m.wide.begin(), m.wide.end(), code, [](const auto& entry, std::uint32_t value) { return entry.first < value; });
    return found != m.wide.end() && found->first == code;
}
// One of the menus' fonts as the overlay last gave it (set_font).
inline detail::FontMetrics menu_font(int font) {
    std::lock_guard lock(detail::mutex);
    return detail::fonts[static_cast<std::size_t>(std::clamp(font, 0, 4))];
}
// How wide one character is set in a font at `size` pixels, and whether the font draws it itself: a
// plugin's own font leaves the characters it lacks to `other` ("body"), so a name with an accent or
// in another script still shows. Before a font is known, a guess.
struct Advance {
    float width{};
    bool own{true};
};
inline Advance advance_at(const detail::FontMetrics& m, const detail::FontMetrics* other, float size, std::uint32_t code) {
    if (m.own && !has_glyph(m, code)) {
        if (!other || other->size <= 0) return {size * 0.55f, false};
        return {advance_of(*other, code) * (size / other->size), false};
    }
    if (m.size <= 0) return {size * 0.55f, true};
    return {advance_of(m, code) * (size / m.size), true};
}
// A text (UTF-8) set in a font at `size` pixels, wrapped to `wrap` pixels when that is more than 0:
// how wide and tall it is. With a `place` that is not nullptr, each character's place in it too,
// place(code, x, line, own): x from the text's left, its line from 0, and whether the font draws it
// (advance_at). What measures a text and what draws one in a plugin's own font both set it with
// this, so a drawn text is as wide as it measured.
template <class Place>
std::array<float, 2> set_text(const detail::FontMetrics& m, const detail::FontMetrics* other, float size, std::string_view text, float wrap, Place&& place) {
    // The advance of the character at `at`, which moves past it.
    const auto advance = [&](std::size_t& at) { return advance_at(m, other, size, next_code_point(text, at)).width; };
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
        float word = 0, spaces = 0;
        while (end < text.size() && text[end] == ' ') spaces += advance(end);
        const std::size_t letters = end;
        word = spaces;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n') word += advance(end);
        float x = line;
        if (wrap > 0 && line > 0 && line + word > wrap) {
            widest = std::max(widest, line);
            ++lines;
            line = word - spaces;
            // (On its new line the word starts at the left: its spaces are not set.)
            x = 0;
            at = letters;
        } else line += word;
        if constexpr (!std::is_same_v<std::remove_cvref_t<Place>, std::nullptr_t>) {
            while (at < end) {
                const auto code = next_code_point(text, at);
                const auto step = advance_at(m, other, size, code);
                place(code, x, lines - 1, step.own);
                x += step.width;
            }
        }
        at = end;
    }
    widest = std::max(widest, line);
    return {widest, static_cast<float>(lines) * size};
}
// How big `text` (UTF-8) is drawn in `font` at `size` pixels (the window's, not 1080's), wrapped to `wrap`
// pixels when that is more than 0, as the overlay draws it. Before the fonts are known, a guess.
inline std::array<float, 2> measure(int font, float size, std::string_view text, float wrap) {
    return set_text(menu_font(font), nullptr, size, text, wrap, nullptr);
}
// The same in a plugin's own font, the characters it lacks in "body".
inline std::array<float, 2> measure(const detail::FontMetrics& own, float size, std::string_view text, float wrap) {
    const auto body = menu_font(1);
    return set_text(own, &body, size, text, wrap, nullptr);
}
// One character as `font` draws it at `size` pixels: its advance, and its drawn box from the text's top
// left (left, top, right, bottom). Nothing before the fonts are known. A character past 127 has an
// advance and no box.
struct Glyph {
    float advance{};
    std::array<float, 4> box{};
};
inline std::optional<Glyph> glyph(int font, float size, std::uint32_t code) {
    detail::FontMetrics m;
    {
        std::lock_guard lock(detail::mutex);
        m = detail::fonts[static_cast<std::size_t>(std::clamp(font, 0, 4))];
    }
    if (m.size <= 0) return std::nullopt;
    const float scale = size / m.size;
    Glyph out{advance_of(m, code) * scale};
    if (code >= 32 && code < 128)
        for (std::size_t i = 0; i < 4; ++i) out.box[i] = m.box[code - 32][i] * scale;
    return out;
}
// The same in a plugin's own font; a character it lacks is "body"'s.
inline std::optional<Glyph> glyph(const detail::FontMetrics& own, float size, std::uint32_t code) {
    if (own.size <= 0 || !has_glyph(own, code)) return glyph(1, size, code);
    const float scale = size / own.size;
    Glyph out{advance_of(own, code) * scale};
    if (code < 128)
        for (std::size_t i = 0; i < 4; ++i) out.box[i] = own.box[code - 32][i] * scale;
    return out;
}
}
