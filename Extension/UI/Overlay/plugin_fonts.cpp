#include "plugin_fonts.h"
#include "glyph_pages.h"
#include "overlay_internal.h"
#include "Engine/Core/Platform/clock.h"
#include "Extension/Plugins/plugin_font.h"

#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

// Texts in plugins' own fonts. A font has pages of glyphs, made at a few sizes (glyph_pages::size_for):
// grey coverage DirectWrite made (plugin_font.h), a byte a pixel here and white with that alpha on the
// GPU, where a page is one of the plugins' pictures (plugin_images.cpp) and each glyph a quad of it
// on the draw list, scaled to the text's size. A font's sizes share its pages, so a font is a
// picture or two on the GPU however many sizes it is drawn at: the places plugins' pictures have
// there are few.
//
// No frame waits for DirectWrite. A glyph a text needs and no page has is asked of a worker thread,
// which is the only one to call DirectWrite here (2000 glyphs take it some 50 ms); the frame after it
// is made it is put on a page, a copy, and the page is sent to the GPU again, whole, under a new
// name, the page drawn so far staying up until the new one is there (about a millisecond for a full
// page, in the frames that bring new glyphs). Until then the character is drawn in "body", in the
// place and the width the font's metrics give it, so a text never moves when its glyphs arrive: its
// width is the metrics', which is also what d:measure answered. In practice that is the first frame
// or two of a font at a size (a new size brings its digits with it, and up to 64 pixels all of
// ASCII, so a counter does not flicker as new digits come up).
//
// Pages are let go when they are not drawn from for a few seconds (a plugin unloaded, a screen
// closed), when a font has its eight and needs another, and when all of them together pass 64 MB; a
// device lost takes the pictures with it, and the pages are sent again from the coverage kept here.
namespace dingosdk::overlay::detail {
namespace own_text {
using Metrics = plugin_ui::detail::FontMetrics;
using namespace glyph_pages;

constexpr double idle_seconds = 5;       // a page, a size or a font not drawn from for this long is let go
constexpr std::size_t most_fonts = 64;   // font files known at once; a text in one more is drawn in "body"
constexpr std::size_t most_asked = 8192; // glyphs waiting for the worker; more wait for a later frame
constexpr int ascii_up_to = 64;          // a new size up to this many pixels brings all of ASCII, a larger one its digits
constexpr std::size_t pixels_a_frame = 1u << 20; // of glyphs put on pages in one frame; the rest wait for the next

struct Glyph {
    // asked: the worker has it, or will. drawn: it is on a page. blank: there is nothing to draw (a
    // space). body: it could not be made, or there was no room for it, and "body" draws it.
    enum class State : std::uint8_t { asked, drawn, blank, body } state{State::asked};
    std::uint32_t page{};    // its page's number
    std::uint32_t version{}; // the page's version it is on from
    std::uint16_t x{}, y{}, wide{}, tall{};
    std::int16_t left{}, top{}; // its box's top left from the pen on the baseline, at the size it was made
    double again{};             // body: when it is asked for again (0: never)
};
struct Page {
    std::uint32_t number{};
    Shelves shelves;
    std::vector<std::uint8_t> grey;                  // page_width x shelves.height
    std::uint32_t version{};                         // of what `grey` holds
    bool touched{};                                  // glyphs were put on it this frame
    std::uint32_t shown_version{}, coming_version{}; // of the picture on the GPU, and of the one on its way
    int shown_height{}, coming_height{};
    std::string shown, coming; // their names among the plugins' pictures
    ImTextureID texture{};     // the shown one's, this frame
    double used{}, retry{};
};
// The font at one of the sizes its glyphs are made at.
struct Face {
    std::unordered_map<std::uint32_t, Glyph> glyphs;
    std::vector<std::uint32_t> asking; // asked for this frame, not yet with the worker
    double used{};
};
}
struct OwnFont {
    std::filesystem::path file;
    std::shared_ptr<const own_text::Metrics> metrics;
    std::map<int, own_text::Face> faces;                // by the size its glyphs are made at
    std::vector<std::unique_ptr<own_text::Page>> pages; // of all its sizes; the last is the one new glyphs go on
    double used{}, looked{-1}, again{};
};

namespace own_text {
// The worker: DirectWrite makes the glyphs here, off the frame.
struct Request {
    std::string name;
    std::filesystem::path file;
    std::shared_ptr<const Metrics> metrics; // (which font the file was when they were asked for)
    int size{};
    std::vector<std::uint32_t> codes;
};
struct Result {
    Request request;
    bool read{};
    std::vector<plugins::font::Coverage> glyphs;
};
struct Work {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> requests;
    std::vector<Result> results;
    std::size_t waiting{}; // glyphs asked for and not yet handed back
    bool worker{};
};
Work& work() {
    static auto* value = new Work;
    return *value;
}
void work_loop() {
    auto& w = work();
    for (;;) {
        Result result;
        {
            std::unique_lock lock(w.mutex);
            w.wake.wait(lock, [&] { return !w.requests.empty(); });
            result.request = std::move(w.requests.front());
            w.requests.pop_front();
        }
        try {
            result.read = plugins::font::rasterise(result.request.file, static_cast<float>(result.request.size), result.request.codes,
                                                   std::min(page_width, most_page_height) - 2, result.glyphs);
        } catch (...) {
            result.read = false;
        }
        std::lock_guard lock(w.mutex);
        w.waiting -= std::min(w.waiting, result.request.codes.size());
        w.results.push_back(std::move(result));
    }
}

// What the worker handed back, and how far it has been put on pages.
struct Made {
    Result result;
    std::size_t next{};
};
// Render thread only.
struct Own {
    std::map<std::string, OwnFont, std::less<>> fonts; // by the file's path as a layer gives it
    std::deque<Made> made;                             // handed back, not yet all on pages
    std::uint32_t pages{};                             // page numbers given so far
    double now{}, last{};                              // this frame's time, and the one before's
    Metrics body;                                      // "body" as plugins measure with it, read once a frame
    bool body_read{};
};
Own& own() {
    static auto* value = new Own;
    return *value;
}

// A page goes, and its glyphs with it: one that is drawn again is made again.
void let_go(OwnFont& font, std::size_t index) {
    const auto& page = *font.pages[index];
    if (!page.shown.empty()) plugin_pixels_drop(page.shown);
    if (!page.coming.empty()) plugin_pixels_drop(page.coming);
    for (auto& [size, face] : font.faces)
        std::erase_if(face.glyphs, [&](const auto& item) { return item.second.state == Glyph::State::drawn && item.second.page == page.number; });
    font.pages.erase(font.pages.begin() + static_cast<std::ptrdiff_t>(index));
}
void let_go(OwnFont& font) {
    while (!font.pages.empty()) let_go(font, font.pages.size() - 1);
    font.faces.clear();
}

// Room for `more` bytes of pages under the cap: the pages drawn from longest ago go, never one drawn
// from in the last frame (make_room). False when there is none to be had.
bool room(std::size_t more) {
    auto& s = own();
    struct Where {
        OwnFont* font{};
        std::uint32_t number{};
    };
    std::vector<Held> held;
    std::vector<Where> where;
    for (auto& [name, font] : s.fonts)
        for (const auto& page : font.pages) {
            held.push_back({bytes_of(page->shelves.height), page->used});
            where.push_back({&font, page->number});
        }
    const auto go = make_room(held, more, most_bytes, s.last);
    if (!go) return false;
    for (const auto index : *go) {
        auto& pages = where[index].font->pages;
        const auto found = std::find_if(pages.begin(), pages.end(), [&](const auto& page) { return page->number == where[index].number; });
        if (found != pages.end()) let_go(*where[index].font, static_cast<std::size_t>(found - pages.begin()));
    }
    return true;
}

// A glyph the worker made, onto the font's open page, or a new one. False when there is no room.
bool put(OwnFont& font, Glyph& glyph, const plugins::font::Coverage& made) {
    auto& s = own();
    Page* page = font.pages.empty() ? nullptr : font.pages.back().get();
    auto place = page ? page->shelves.find(made.width, made.height) : std::nullopt;
    if (!place) {
        if (font.pages.size() >= most_pages_a_font) {
            std::size_t oldest = 0;
            for (std::size_t i = 1; i < font.pages.size(); ++i)
                if (font.pages[i]->used < font.pages[oldest]->used) oldest = i;
            if (font.pages[oldest]->used >= s.last) return false; // (every one of them was drawn from in the last frame)
            let_go(font, oldest);
        }
        if (!room(bytes_of(least_page_height))) return false;
        font.pages.push_back(std::make_unique<Page>());
        page = font.pages.back().get();
        page->number = ++s.pages;
        page->grey.assign(static_cast<std::size_t>(page_width) * static_cast<std::size_t>(page->shelves.height), 0);
        place = page->shelves.find(made.width, made.height);
        if (!place) return false;
    }
    page->used = s.now; // (it is not the one to go while it is being filled)
    if (place->height > page->shelves.height) {
        if (!room(bytes_of(place->height) - bytes_of(page->shelves.height))) return false;
        page->grey.resize(static_cast<std::size_t>(page_width) * static_cast<std::size_t>(place->height), 0);
    }
    page->shelves.take(*place, made.width, made.height);
    for (int row = 0; row < made.height; ++row)
        std::copy_n(made.grey.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(made.width), made.width,
                    page->grey.data() + static_cast<std::size_t>(place->y + row) * page_width + static_cast<std::size_t>(place->x));
    glyph.state = Glyph::State::drawn;
    glyph.page = page->number;
    glyph.version = page->version + 1;
    glyph.x = static_cast<std::uint16_t>(place->x);
    glyph.y = static_cast<std::uint16_t>(place->y);
    glyph.wide = static_cast<std::uint16_t>(made.width);
    glyph.tall = static_cast<std::uint16_t>(made.height);
    glyph.left = static_cast<std::int16_t>(std::clamp(made.left, -32768, 32767));
    glyph.top = static_cast<std::int16_t>(std::clamp(made.top, -32768, 32767));
    page->touched = true;
    return true;
}

// What the worker handed back, onto the pages of the font it was asked for, for the size it was
// asked at (when they are still there, and the file is still the font it was). At most `budget` pixels of glyphs are copied
// in a frame: false when there are more, which the next frame goes on with.
bool place(Made& made, std::size_t& budget) {
    auto& s = own();
    const auto& result = made.result;
    const auto found = s.fonts.find(result.request.name);
    if (found == s.fonts.end() || found->second.metrics != result.request.metrics) return true;
    const auto sized = found->second.faces.find(result.request.size);
    if (sized == found->second.faces.end()) return true;
    auto& font = found->second;
    auto& face = sized->second;
    bool done = true;
    for (; made.next < result.request.codes.size(); ++made.next) {
        const auto entry = face.glyphs.find(result.request.codes[made.next]);
        if (entry == face.glyphs.end() || entry->second.state != Glyph::State::asked) continue;
        auto& glyph = entry->second;
        if (!result.read || made.next >= result.glyphs.size()) {
            glyph.state = Glyph::State::body;
            glyph.again = s.now + idle_seconds; // (the file could not be read just now)
            continue;
        }
        const auto& one = result.glyphs[made.next];
        if (one.kind == plugins::font::Coverage::Kind::drawn && !budget) {
            done = false;
            break;
        }
        glyph.state = Glyph::State::body;
        if (one.kind == plugins::font::Coverage::Kind::blank) glyph.state = Glyph::State::blank;
        else if (one.kind == plugins::font::Coverage::Kind::drawn) {
            budget -= std::min(budget, one.grey.size());
            if (!put(font, glyph, one)) glyph.again = s.now + 1; // (no room: asked for again)
        }
    }
    for (auto& page : font.pages)
        if (std::exchange(page->touched, false)) ++page->version;
    return done;
}

// A page's pictures: the one on its way takes over once it is on the GPU, and one that was lost on
// the way, or after (the device was lost, or its place was given to another picture), is sent again.
void settle(Page& page) {
    auto& s = own();
    if (!page.coming.empty()) {
        if (plugin_pixels_id(page.coming)) {
            // (The one shown was drawn from in the last frame at the latest: whatever takes its place
            // on the GPU waits for that frame, as for any plugin's picture.)
            if (!page.shown.empty()) plugin_pixels_drop(page.shown);
            page.shown = std::exchange(page.coming, {});
            page.shown_version = page.coming_version;
            page.shown_height = page.coming_height;
        } else if (!plugin_pixels_waiting(page.coming)) {
            plugin_pixels_drop(page.coming);
            page.coming.clear();
            page.retry = s.now + 1;
        }
    }
    page.texture = page.shown.empty() ? ImTextureID{} : plugin_pixels_id(page.shown);
    if (!page.shown.empty() && !page.texture) {
        page.shown.clear();
        page.shown_version = 0;
    }
}
// The page as it is now, to the GPU under a new name. White, the coverage its alpha.
bool send(Page& page) {
    auto& s = own();
    CardPixels pixels;
    pixels.width = static_cast<UINT>(page_width);
    pixels.height = static_cast<UINT>(page.shelves.height);
    pixels.rgba.assign(page.grey.size() * 4, 255);
    for (std::size_t i = 0; i < page.grey.size(); ++i) pixels.rgba[i * 4 + 3] = page.grey[i];
    auto name = "glyphs:" + std::to_string(page.number) + ":" + std::to_string(page.version);
    if (!plugin_pixels(name, std::move(pixels))) {
        page.retry = s.now + 0.5;
        return false;
    }
    page.coming = std::move(name);
    page.coming_version = page.version;
    page.coming_height = page.shelves.height;
    return true;
}

Face& face_of(OwnFont& font, int size) {
    const auto found = font.faces.find(size);
    if (found != font.faces.end()) return found->second;
    auto& face = font.faces[size];
    // A new size starts with the characters most likely to come up one at a time.
    for (std::uint32_t code = 33; code < 127; ++code) {
        if (font.metrics->lacks[code - 32] || (size > ascii_up_to && (code < '0' || code > '9'))) continue;
        face.glyphs.try_emplace(code);
        face.asking.push_back(code);
    }
    return face;
}
Glyph& glyph_of(Face& face, std::uint32_t code) {
    const auto [entry, added] = face.glyphs.try_emplace(code);
    auto& glyph = entry->second;
    if (added) face.asking.push_back(code);
    else if (glyph.state == Glyph::State::body && glyph.again > 0 && own().now >= glyph.again) {
        glyph.state = Glyph::State::asked;
        glyph.again = 0;
        face.asking.push_back(code);
    }
    return glyph;
}
ImU32 shadow_of(ImU32 colour) {
    const auto alpha = static_cast<ImU32>(((colour >> IM_COL32_A_SHIFT) & 0xff) * 0.75f);
    return IM_COL32(0, 0, 0, alpha);
}
// One character of a text as it is set: which, where from the text's top left, and whether the font draws it.
struct Set {
    std::uint32_t code{};
    float x{};
    int line{};
    bool own{};
};
}

void own_fonts_begin() {
    using namespace own_text;
    auto& s = own();
    s.last = s.now;
    s.now = steady_seconds();
    s.body_read = false;
    {
        auto& w = work();
        std::lock_guard lock(w.mutex);
        for (auto& result : w.results) s.made.push_back({std::move(result)});
        w.results.clear();
    }
    if (s.fonts.empty()) {
        s.made.clear();
        return;
    }
    for (auto& [name, font] : s.fonts)
        for (auto& page : font.pages) settle(*page);
    // The glyphs made since the last frame go onto their pages, so many pixels of them a frame: the
    // copy is then well under a millisecond however many came back at once.
    std::size_t budget = pixels_a_frame;
    while (!s.made.empty() && place(s.made.front(), budget)) s.made.pop_front();
    // What is no longer drawn goes: a page, then a size with no glyph left on a page, then a font
    // with neither.
    const double idle = s.now - idle_seconds;
    for (auto entry = s.fonts.begin(); entry != s.fonts.end();) {
        auto& font = entry->second;
        for (std::size_t i = font.pages.size(); i-- > 0;)
            if (font.pages[i]->used < idle) let_go(font, i);
        std::erase_if(font.faces, [&](const auto& item) {
            const auto& face = item.second;
            return face.used < idle &&
                   std::none_of(face.glyphs.begin(), face.glyphs.end(), [](const auto& glyph) { return glyph.second.state == Glyph::State::drawn; });
        });
        if (font.faces.empty() && font.pages.empty() && font.used < idle) entry = s.fonts.erase(entry);
        else ++entry;
    }
    // One page a frame to the GPU (a picture a frame is what goes up): the first with glyphs the
    // picture of it lacks, and none on its way.
    for (auto& [name, font] : s.fonts)
        for (auto& page : font.pages)
            if (page->coming.empty() && page->version > page->shown_version && s.now >= page->retry && send(*page)) return;
}

OwnFont* own_font(const std::string& file) {
    using namespace own_text;
    try {
        auto& s = own();
        auto found = s.fonts.find(file);
        if (found == s.fonts.end()) {
            if (s.fonts.size() >= most_fonts) return nullptr;
            found = s.fonts.emplace(file, OwnFont{}).first;
            found->second.file = std::filesystem::path(std::u8string(file.begin(), file.end()));
        }
        auto& font = found->second;
        if (font.looked != s.now) { // once a frame
            font.looked = s.now;
            // (A plugin's API asks for a font's metrics before it publishes a text in it, so they are
            // known here; a layer from anywhere else has them read now.)
            auto known = plugins::font::known_metrics(font.file);
            if (!known && s.now >= font.again) {
                known = plugins::font::metrics(font.file);
                if (!known) font.again = s.now + 2;
            }
            if (known != font.metrics) {
                // The file is another font than it was (written again): its glyphs are made again.
                let_go(font);
                font.metrics = std::move(known);
            }
        }
        font.used = s.now;
        return font.metrics && font.metrics->size > 0 ? &font : nullptr;
    } catch (const std::exception&) {
        return nullptr;
    }
}

void draw_own_text(ImDrawList* draw, OwnFont& font, ImFont* body, float k, ImVec2 where, const plugin_ui::Shape& shape) {
    using namespace own_text;
    auto& s = own();
    const auto& m = *font.metrics;
    const float size = shape.style.size * k, wrap = shape.style.wrap * k;
    if (!(size > 0) || shape.text.empty()) return;
    if (!std::exchange(s.body_read, true)) s.body = plugin_ui::menu_font(1);
    const int made_at = size_for(size);
    auto& face = face_of(font, made_at);
    face.used = s.now;
    // Each character's place, exactly as d:measure set the text.
    static std::vector<Set> set; // render thread only
    set.clear();
    const auto extent = plugin_ui::set_text(m, &s.body, size, shape.text, wrap,
                                            [&](std::uint32_t code, float x, int line, bool own_glyph) { set.push_back({code, x, line, own_glyph}); });
    // One line is placed by its alignment; a wrapped text is from its top left, as the menus' fonts are.
    if (wrap <= 0) {
        if (shape.style.align == 1) where.x -= extent[0] * 0.5f;
        else if (shape.style.align == 2) where.x -= extent[0];
    }
    const float left = static_cast<float>(static_cast<int>(where.x)), top = static_cast<float>(static_cast<int>(where.y));
    const float scale = size / static_cast<float>(made_at), rise = m.ascent * (size / m.size);
    const bool exact = scale == 1.0f; // drawn at the size it was made: kept on whole pixels, so it is as sharp as it was made
    const auto pass = [&](float offset, ImU32 colour) {
        ImTextureID bound{};
        Page* page{};
        for (const auto& one : set) {
            const float x = left + offset + one.x, y = top + offset + static_cast<float>(one.line) * size;
            if (one.own) {
                auto& glyph = glyph_of(face, one.code);
                if (glyph.state == Glyph::State::blank) continue;
                if (glyph.state == Glyph::State::drawn) {
                    if (!page || page->number != glyph.page) {
                        page = nullptr;
                        for (auto& candidate : font.pages)
                            if (candidate->number == glyph.page) page = candidate.get();
                    }
                    if (page) page->used = s.now;
                    if (page && page->texture && glyph.version <= page->shown_version) {
                        if (bound != page->texture) {
                            if (bound) draw->PopTextureID();
                            draw->PushTextureID(page->texture);
                            bound = page->texture;
                        }
                        float x0 = x + static_cast<float>(glyph.left) * scale;
                        if (exact) x0 = std::round(x0);
                        const float y0 = std::round(y + rise) + static_cast<float>(glyph.top) * scale;
                        const float u = 1.0f / static_cast<float>(page_width), v = 1.0f / static_cast<float>(page->shown_height);
                        draw->PrimReserve(6, 4);
                        draw->PrimRectUV(ImVec2(x0, y0), ImVec2(x0 + static_cast<float>(glyph.wide) * scale, y0 + static_cast<float>(glyph.tall) * scale),
                                         ImVec2(static_cast<float>(glyph.x) * u, static_cast<float>(glyph.y) * v),
                                         ImVec2(static_cast<float>(glyph.x + glyph.wide) * u, static_cast<float>(glyph.y + glyph.tall) * v), colour);
                        continue;
                    }
                }
            }
            // "body" draws it: a character the font lacks, or one whose glyph is not on the GPU yet.
            if (bound) {
                draw->PopTextureID();
                bound = {};
            }
            body->RenderChar(draw, size, ImVec2(x, y), colour, static_cast<ImWchar>(one.code <= 0xffff ? one.code : 0xfffd));
        }
        if (bound) draw->PopTextureID();
    };
    if (shape.style.shadow) pass(std::max(1.0f, size * 0.06f), shadow_of(shape.style.colour));
    pass(0, shape.style.colour);
}

void own_fonts_end() {
    using namespace own_text;
    auto& s = own();
    auto& w = work();
    std::unique_lock lock(w.mutex, std::defer_lock);
    bool asked = false;
    for (auto& [name, font] : s.fonts)
        for (auto& [size, face] : font.faces) {
            if (face.asking.empty()) continue;
            if (!lock.owns_lock()) lock.lock();
            if (w.waiting + face.asking.size() > most_asked) continue; // (they stay asked for: a later frame sends them)
            w.waiting += face.asking.size();
            w.requests.push_back({name, font.file, font.metrics, size, std::exchange(face.asking, {})});
            asked = true;
        }
    if (!asked) return;
    if (!std::exchange(w.worker, true)) std::thread(work_loop).detach();
    w.wake.notify_one();
}
}
