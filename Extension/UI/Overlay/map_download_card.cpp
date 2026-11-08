#include "overlay_internal.h"
#include "skate_menu_internal.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/World/world_names.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <mutex>
#include <string_view>
#include <wincodec.h>

// The card for what a multiplayer session needs and this PC does not have: its map, a dedicated
// server's plugins, or both (Extension/Assets/map_download.h). In the look of ReSkate's menus now
// (the Insert menu's kit in skate_menu.cpp and the pause menu's pages in hub_page.cpp): near-black
// tiles, the blue for the main action, white with dark ink for the focus, plates in the kit's tag
// colours and the brushed title face for the heading.
//
// While it asks, a card in the middle of the screen: JOINING and the session's name, with its map
// and that map's picture (the one the server browser shows: an installed map's, or the package's
// Thunderstore icon), then a row for each thing needed (its icon, name, kind, size and state), and
// DOWNLOAD AND JOIN and CANCEL. Once the player has said yes it becomes a smaller card at the right
// edge, so they can skate on while they wait: the bar for all of it with the speed and the time
// left, the step it is on and each item's state. It goes when everything is in the game and the
// map starts loading. When the download fails while the card is up, the card says so in the
// middle again, on the item it failed on, until the player puts it away.
//
// Answered by a controller (D-pad to pick, A to take the pick, B for no), the keyboard (arrow
// keys, Enter, Esc), the vote binds, or a click: the overlay has the pointer while the card asks
// or shows a failure (State::prompt_pointer). The runtime reads those and holds the game's input
// meanwhile.
//
// The first item's icon is the package's own, a PNG the downloader hands over
// (set_map_download_icon). It is decoded here and goes to the GPU from the render thread
// (overlay_render.cpp), into the overlay's second texture slot; the first is ImGui's atlas, which
// is filled once at startup. The other items' icons come by URL, as the Mod Manager's do.

namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
std::atomic<MapDownloadFeed> feed{};
std::atomic<MapDownloadAnswer> answer{};

std::mutex image_mutex;
detail::CardPixels current;   // the icon in use, kept to upload again after a device reset
bool waiting{};               // `current` has not gone to the GPU yet
unsigned image_serial{};
std::atomic<ImTextureID> image{}; // on the GPU and drawable; 0 for none
// The texture is the next-map vote's pictures (set_map_vote_icons), not the card's icon: the
// one slot is whichever was set last. Which square is whose, under image_mutex.
std::atomic<bool> image_is_atlas{};
std::vector<std::string> vote_icon_names;
std::vector<bool> vote_icon_has;
ImTextureID card_icon() { return image_is_atlas.load() ? ImTextureID{} : image.load(); }

// Straight-alpha RGBA, at most 512 pixels a side (an icon is 256). With `side`, scaled to a
// square of that many pixels (the vote's pictures share one texture). With `banner`, a picture up
// to 4096 pixels a side scaled to no wider than that, its shape kept (a news post's picture).
bool decode(const std::string& bytes, detail::CardPixels& out, UINT side = 0, UINT banner = 0) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok{};
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<IWICBitmapScaler> scaler;
        UINT width{}, height{};
        const UINT largest = banner ? 4096U : side ? 2048U : 512U;
        // (Set once the size is known: the square, or the banner's width and the height that keeps its shape.)
        UINT scaled_width = side, scaled_height = side;
        const auto scaled = [&] {
            if (banner && width > banner) scaled_width = banner, scaled_height = std::max(1U, static_cast<UINT>(static_cast<std::uint64_t>(height) * banner / width));
            return scaled_width != 0;
        };
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size()))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width && height &&
            width <= largest && height <= largest && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            (!scaled() || (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
                           SUCCEEDED(scaler->Initialize(frame.Get(), scaled_width, scaled_height, WICBitmapInterpolationModeFant)))) &&
            SUCCEEDED(converter->Initialize(scaled_width ? static_cast<IWICBitmapSource*>(scaler.Get()) : static_cast<IWICBitmapSource*>(frame.Get()),
                                            GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom))) {
            if (scaled_width) width = scaled_width, height = scaled_height;
            out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
            out.width = width;
            out.height = height;
            ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()));
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}
}
void set_map_download_feed(MapDownloadFeed value) noexcept { feed.store(value); }
void set_map_download_answer(MapDownloadAnswer value) noexcept { answer.store(value); }

void set_map_download_icon(const std::string& png) noexcept {
    try {
        detail::CardPixels pixels;
        const bool decoded = !png.empty() && png.size() <= 4 * 1024 * 1024 && decode(png, pixels);
        std::lock_guard lock(image_mutex);
        image.store(0);
        image_is_atlas.store(false);
        pixels.serial = ++image_serial;
        current = decoded ? std::move(pixels) : detail::CardPixels{};
        current.serial = image_serial;
        waiting = decoded;
    } catch (...) {}
}
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
bool decode_picture(const std::string& png, CardPixels& out, UINT side) { return decode(png, out, side); }
bool decode_banner(const std::string& bytes, CardPixels& out, UINT most_width) { return decode(bytes, out, 0, most_width); }
bool take_card_pixels(CardPixels& out) {
    std::lock_guard lock(image_mutex);
    if (!waiting) return false;
    waiting = false;
    out = current; // (kept: a device reset uploads it again)
    return true;
}
void card_image_uploaded(ImTextureID texture, unsigned serial) {
    std::lock_guard lock(image_mutex);
    if (serial == image_serial) image.store(texture);
}
void card_image_lost() {
    std::lock_guard lock(image_mutex);
    image.store(0);
    waiting = !current.rgba.empty();
}

namespace {
MapDownloadCard card; // this frame's, read by map_download_pending

// The kit's colours (skate_menu.cpp, and the pause menu's pages in hub_page.cpp): the card and its
// rows near black, the buttons a dark grey that lightens under the pointer, the one the D-pad or
// the keys are on white with dark ink, and the plates in the kit's tag colours.
constexpr ImU32 card_fill = IM_COL32(26, 26, 26, 250), row_fill = IM_COL32(16, 16, 16, 242), button_fill = IM_COL32(40, 40, 43, 255),
                button_hover = IM_COL32(56, 57, 61, 255), focus_ink = IM_COL32(11, 11, 11, 255), plate_grey = IM_COL32(61, 62, 66, 255),
                quiet = IM_COL32(138, 140, 146, 255), later = IM_COL32(90, 91, 96, 255), no_picture = IM_COL32(52, 56, 62, 255),
                map_tile = IM_COL32(63, 84, 104, 255);
constexpr char dot[] = "  \xc2\xb7  ";

std::string megabytes(std::uint64_t bytes) {
    const auto mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb >= 1024.0 ? std::format("{:.2f} GB", mb / 1024.0) : bytes && mb < 1.0 ? std::string("1 MB") : std::format("{:.0f} MB", mb);
}
std::string per_second(double bytes) {
    const double mb = bytes / (1024.0 * 1024.0);
    return mb >= 1.0 ? std::format("{:.1f} MB/s", mb) : std::format("{:.0f} KB/s", std::max(1.0, bytes / 1024.0));
}
std::string time_left(double seconds) {
    if (seconds < 60.0) return std::format("{} s left", std::max(1, static_cast<int>(std::ceil(seconds))));
    if (seconds < 3600.0) return std::format("{} min left", static_cast<int>(std::ceil(seconds / 60.0)));
    return std::format("{:.1f} h left", seconds / 3600.0);
}
// The brushed title face has capitals only, as the Insert menu's page titles use it.
std::string upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
float width_of(ImFont* font, float size, const std::string& text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }
// One line of text, cut off at `right`.
void line(ImDrawList* draw, ImFont* font, float size, ImVec2 at, float right, ImU32 colour, const std::string& text) {
    if (right <= at.x || text.empty()) return;
    draw->PushClipRect(ImVec2(at.x, at.y - size), ImVec2(right, at.y + size * 2), true);
    draw->AddText(font, size, at, colour, text.c_str());
    draw->PopClipRect();
}

struct Fonts {
    ImFont *bold, *body, *heading, *title;
};
Fonts fonts(const State& s) {
    Fonts f{s.menu.bold ? s.menu.bold : ImGui::GetFont(), s.menu.body ? s.menu.body : ImGui::GetFont(), nullptr, nullptr};
    f.heading = s.menu.heading ? s.menu.heading : f.bold;
    // The big brushed titles draw from the larger face, as the Insert menu's do (skate_menu.cpp: title_font).
    f.title = s.menu.title_large ? s.menu.title_large : s.menu.title ? s.menu.title : f.heading;
    return f;
}

// A plate in one of the kit's tag colours (MAP, DOWNLOADING, FAILED), `scale` times the size of
// the card's. Returns its width; with no draw list it only measures.
float plate(ImDrawList* draw, ImFont* bold, float u, ImVec2 at, const std::string& text, ImU32 fill, ImU32 ink, float scale = 1) {
    const float size = 24 * u * scale, pad = 12 * u * scale, tall = 38 * u * scale, w = width_of(bold, size, text) + pad * 2;
    if (draw) {
        draw->AddRectFilled(at, ImVec2(at.x + w, at.y + tall), fill, 3 * u * scale);
        draw->AddText(bold, size, ImVec2(at.x + pad, at.y + (tall - size) * .5f), ink, text.c_str());
    }
    return w;
}

// A bar filled to `share` (0 to 1); a negative share is under way by an amount not known, drawn as
// a block going back and forth.
void bar(ImDrawList* draw, ImVec2 a, ImVec2 b, float share, ImU32 colour, float round) {
    draw->AddRectFilled(a, b, plate_grey, round);
    const float span = b.x - a.x;
    if (share >= 0.0f) {
        if (share > 0.0f) draw->AddRectFilled(a, ImVec2(a.x + span * std::clamp(share, 0.0f, 1.0f), b.y), colour, round);
        return;
    }
    const float at = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0));
    const float where = (at < 1.0f ? at : 2.0f - at) * span * .75f;
    draw->AddRectFilled(ImVec2(a.x + where, a.y), ImVec2(a.x + where + span * .25f, b.y), colour, round);
}

// The heading in the brushed title face, tilted as the Insert menu's page titles are, made smaller
// (down to two thirds) to fit before `right` and cut off past it. Returns the size it was drawn at.
float brushed(ImDrawList* draw, ImFont* font, float size, ImVec2 at, float right, float u, const std::string& text) {
    const float most = right - at.x, wide = width_of(font, size, text);
    if (wide > most && wide > 0) size = std::max(size * .66f, size * most / wide);
    const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    draw->PushClipRect(ImVec2(at.x - 8 * u, at.y - size * .5f), ImVec2(right, at.y + size * 1.5f), true);
    const int start = draw->VtxBuffer.Size;
    draw->AddText(font, size, ImVec2(at.x + 4 * u, at.y + 6 * u), IM_COL32(0, 0, 0, 160), text.c_str());
    draw->AddText(font, size, at, theme::paper, text.c_str());
    skate_theme::rotate_since(draw, start, -2.0f, ImVec2(at.x + std::min(extent.x, most) * .5f, at.y + extent.y * .5f));
    draw->PopClipRect();
    return size;
}

// A picture to draw in a square: its texture and the part of it that is the picture.
struct Picture {
    ImTextureID texture{};
    ImVec2 uv0{0, 0}, uv1{1, 1};
};
// An item's icon. The first item's is the card's own picture (set_map_download_icon), which the
// downloader hands over decoded; the others come from Thunderstore by their URL, as the Mod
// Manager's do (store_icons.cpp). One that is here already has none of its own beyond the first.
Picture item_picture(std::size_t at) {
    if (at == 0)
        if (const auto texture = card_icon()) return {texture};
    if (at < card.items.size() && !card.items[at].icon.empty())
        if (const auto texture = store_icon(card.items[at].icon)) return {texture};
    return {};
}
// The session's map, its picture as the server browser shows it: the map's own icon when it is the
// map that is fetched, else (only plugins are needed) the installed map's picture from the menu's
// map pictures (menu_map_pictures.cpp), once those have been found.
Picture map_picture() {
    if (!card.items.empty() && !card.items.front().plugin) return item_picture(0);
    Picture picture;
    if (!card.session_map.empty() &&
        menu_map_picture(std::string(world_destination_asset(card.session_map)), picture.texture, picture.uv0, picture.uv1))
        return picture;
    return {};
}
std::string map_name() {
    if (!card.map.empty()) return card.map;
    return card.session_map.empty() ? std::string() : world_level_name(world_destination_asset(card.session_map));
}
// A picture in a square, or a plain tile with `mark` on it while there is none (or it is still on
// its way: nothing here waits for one). `wrap`: the mark is a map's name, wrapped to the tile.
void square_picture(ImDrawList* draw, ImFont* bold, float u, ImVec2 a, ImVec2 b, const Picture& picture, const std::string& mark, bool wrap) {
    if (picture.texture) {
        draw->AddImage(picture.texture, a, b, picture.uv0, picture.uv1);
        return;
    }
    draw->AddRectFilled(a, b, wrap ? map_tile : no_picture);
    const float side = b.x - a.x, size = wrap ? std::min(34 * u, side * .14f) : std::min(30 * u, side * .3f), inset = side * .1f;
    const auto extent = bold->CalcTextSizeA(size, FLT_MAX, wrap ? side - inset * 2 : 0.0f, mark.c_str());
    draw->PushClipRect(a, b, true);
    draw->AddText(bold, size, ImVec2(wrap ? a.x + inset : a.x + (side - extent.x) * .5f, a.y + std::max(inset, (side - extent.y) * .5f)),
                  wrap ? theme::paper : quiet, mark.c_str(), nullptr, wrap ? side - inset * 2 : 0.0f);
    draw->PopClipRect();
}

// What each item is up to, as its tag says it: NEEDED, DOWNLOADING (with how far: `share`, -1 when
// that is not known), INSTALLED, FAILED. `share` is -2 for a row with no bar.
struct Status {
    const char* tag{};
    ImU32 fill{}, ink{};
    float share = -2.0f;
};
Status status_of(std::size_t at) {
    const auto& item = card.items[at];
    constexpr ImU32 good = skate_theme::good, black = skate_theme::black;
    if (card.stage == 6) {
        // Those before the one it failed on are in; a failure while the mods were applied came
        // after every one of them was. A failure while it asked fetched nothing.
        if (at == card.failed_item) return {"FAILED", skate_theme::danger, black};
        if (card.failed_stage >= 4 || (card.failed_item < card.items.size() && at < card.failed_item)) return {"INSTALLED", good, black};
        return {item.installed ? "SWITCHED OFF" : "NEEDED", plate_grey, theme::paper};
    }
    if (card.stage == 1) {
        // Already here, switched off: nothing is downloaded, it is switched on.
        if (item.installed) return {"INSTALLED", good, black};
        return {item.have.empty() ? "NEEDED" : "UPDATE", skate_theme::warning, black};
    }
    // Under way: those before the one in hand are in, those after it wait their turn.
    if (card.stage >= 4 || at < card.current) return {"INSTALLED", good, black};
    if (at > card.current) return {"NEEDED", plate_grey, theme::paper};
    if (card.stage == 2 && !item.installed)
        return {"DOWNLOADING", theme::blue, black,
                card.total ? std::clamp(static_cast<float>(card.received) / static_cast<float>(card.total), 0.0f, 1.0f) : -1.0f};
    return {item.installed ? "SWITCHING ON" : "INSTALLING", theme::blue, black};
}
// The line under an item's name: who made it, its version and its size, or how far its download
// is, or why it failed.
std::string detail_of(std::size_t at, const Status& status) {
    const auto& item = card.items[at];
    if (card.stage == 6 && at == card.failed_item) return card.failure;
    if (status.share > -2.0f)
        return card.total ? megabytes(card.received) + " of " + megabytes(card.total) : megabytes(card.received) + " so far";
    std::string text = item.author.empty() ? std::string() : "by " + item.author;
    const auto add = [&](const std::string& part) {
        if (!part.empty()) text += (text.empty() ? "" : dot) + part;
    };
    if (item.installed) add("In your Mods folder, switched off");
    else if (!item.have.empty()) add(item.have + " \xe2\x86\x92 " + item.version);
    else if (!item.version.empty()) add("v" + item.version);
    if (!item.installed && item.bytes) add(megabytes(item.bytes));
    return text;
}
// One item as a row: its picture, its name, its kind and what getting it takes, and its state as a
// tag at the right end; the one downloading has its bar and how far along the bottom. The small
// card's rows (`compact`) are a little smaller, but no text in them under the kit's tag size.
void item_row(ImDrawList* draw, const Fonts& f, float u, ImVec2 a, ImVec2 b, std::size_t at, bool compact) {
    const auto& item = card.items[at];
    const auto status = status_of(at);
    const bool failed = card.stage == 6 && at == card.failed_item, moving = status.share > -2.0f;
    const float h = b.y - a.y, pad = (compact ? 12 : 14) * u, icon = h - pad * 2, strip = 6 * u, gap = 20 * u;
    const float tag_scale = compact ? .9f : 1.0f, kind_scale = compact ? .8f : .85f;
    const float name_size = (compact ? 30 : 34) * u, text_size = (compact ? 24 : 26) * u, percent_size = (compact ? 26 : 28) * u;
    draw->AddRectFilled(a, b, row_fill);
    // The one in hand, and the one that failed, are marked at their left edge.
    if (moving || failed || (card.stage >= 2 && card.stage <= 3 && at == card.current)) draw->AddRectFilled(a, ImVec2(a.x + strip, b.y), status.fill);
    const float icon_x = a.x + strip + pad;
    square_picture(draw, f.bold, u, ImVec2(icon_x, a.y + pad), ImVec2(icon_x + icon, a.y + pad + icon), item_picture(at), item.plugin ? "MOD" : "MAP", false);
    const float tag_w = plate(nullptr, f.bold, u, {}, status.tag, 0, 0, tag_scale), tag_h = 38 * u * tag_scale;
    const ImVec2 tag_at(b.x - pad - tag_w, moving ? a.y + pad : a.y + (h - tag_h) * .5f);
    plate(draw, f.bold, u, tag_at, status.tag, status.fill, status.ink, tag_scale);
    float right = tag_at.x - gap;
    if (status.share >= 0.0f) {
        const auto percent = std::format("{}%", static_cast<int>(status.share * 100.0f));
        const float w = width_of(f.bold, percent_size, percent);
        draw->AddText(f.bold, percent_size, ImVec2(right - w, tag_at.y + (tag_h - percent_size) * .5f), theme::paper, percent.c_str());
        right -= w + gap;
    }
    const float x = icon_x + icon + 22 * u;
    line(draw, f.bold, name_size, ImVec2(x, a.y + pad), right, theme::paper, item.name);
    const float y = a.y + pad + name_size + (compact ? 6 : 8) * u, kind_h = 38 * u * kind_scale;
    const float kind_w = plate(draw, f.bold, u, ImVec2(x, y), item.plugin ? "PLUGIN" : "MAP", item.plugin ? plate_grey : theme::blue,
                               item.plugin ? theme::paper : skate_theme::black, kind_scale);
    line(draw, f.body, text_size, ImVec2(x + kind_w + 12 * u, y + (kind_h - text_size) * .5f), moving ? b.x - pad : right,
         failed ? skate_theme::danger : quiet, detail_of(at, status));
    const float bar_h = (compact ? 7 : 8) * u;
    if (moving) bar(draw, ImVec2(x, b.y - pad - bar_h), ImVec2(b.x - pad, b.y - pad), status.share, theme::blue, 3 * u);
}

// The controls a card names under it: a pad's face button as the pad in use shows it (Xbox's
// coloured letters, PlayStation's shapes, as the Insert menu's prompts are), or a key on a keycap.
struct Hint {
    int face{-1}; // 0 A (cross), 1 B (circle); -1: `key` on a keycap
    std::string key, what;
};
float hint(ImDrawList* draw, ImFont* bold, float u, ImVec2 at, const Hint& shown, ControllerStyle style) {
    const float side = 44 * u, text = 28 * u, gap = 12 * u;
    float w{};
    if (shown.face >= 0) {
        if (draw) {
            const ImVec2 centre(at.x + side * .5f, at.y + side * .5f);
            const bool a = shown.face == 0;
            if (style == ControllerStyle::xbox) {
                draw->AddCircleFilled(centre, side * .5f, a ? IM_COL32(95, 211, 106, 255) : IM_COL32(255, 107, 107, 255));
                const char* letter = a ? "A" : "B";
                const float size = side * .58f;
                const auto extent = bold->CalcTextSizeA(size, FLT_MAX, 0, letter);
                draw->AddText(bold, size, ImVec2(centre.x - extent.x * .5f, centre.y - extent.y * .5f), skate_theme::black, letter);
            } else {
                draw->AddCircleFilled(centre, side * .5f, IM_COL32(32, 32, 34, 255));
                const float r = side * .24f, stroke = side * .09f;
                if (a) {
                    constexpr ImU32 cross = IM_COL32(159, 185, 255, 255);
                    draw->AddLine(ImVec2(centre.x - r, centre.y - r), ImVec2(centre.x + r, centre.y + r), cross, stroke);
                    draw->AddLine(ImVec2(centre.x - r, centre.y + r), ImVec2(centre.x + r, centre.y - r), cross, stroke);
                } else {
                    draw->AddCircle(centre, r, IM_COL32(255, 154, 168, 255), 0, stroke);
                }
            }
        }
        w = side;
    } else {
        const float size = 24 * u, inset = 12 * u, cap = width_of(bold, size, shown.key) + inset * 2;
        if (draw) {
            draw->AddRectFilled(ImVec2(at.x, at.y + 2 * u), ImVec2(at.x + cap, at.y + side - 2 * u), theme::paper, 6 * u);
            draw->AddText(bold, size, ImVec2(at.x + inset, at.y + (side - size) * .5f), skate_theme::black, shown.key.c_str());
        }
        w = cap;
    }
    if (draw) draw->AddText(bold, text, ImVec2(at.x + w + gap, at.y + (side - text) * .5f), theme::paper, shown.what.c_str());
    return w + gap + width_of(bold, text, shown.what);
}
// A row of them, centred on `middle`.
void hints_row(ImDrawList* draw, ImFont* bold, float u, float middle, float y, const std::vector<Hint>& hints, ControllerStyle style) {
    const float gap = 44 * u;
    float total{};
    for (const auto& shown : hints) total += hint(nullptr, bold, u, {}, shown, style) + gap;
    float x = middle - (total - gap) * .5f;
    for (const auto& shown : hints) x += hint(draw, bold, u, ImVec2(x, y), shown, style) + gap;
}

// A button in the kit's look: the main one on the blue, the others a dark grey; under the D-pad or
// the keys (`focused`) white with dark ink, under the pointer a lighter blue or grey. True when it
// is clicked.
bool card_button(ImDrawList* draw, ImFont* bold, float u, ImVec2 a, ImVec2 b, const std::string& text, bool primary, bool focused, unsigned seed) {
    const bool hovered = ImGui::IsMouseHoveringRect(a, b, false);
    const bool lit = focused && !hovered;
    const ImU32 fill = lit ? theme::paper : primary ? (hovered ? skate_theme::blue_hover : theme::blue) : hovered ? button_hover : button_fill;
    skate_theme::rough_rect(draw, a, b, fill, seed, u * 2);
    const float size = std::min(38 * u, (b.y - a.y) * .4f), w = width_of(bold, size, text);
    line(draw, bold, size, ImVec2(a.x + std::max(16 * u, (b.x - a.x - w) * .5f), a.y + (b.y - a.y - size) * .5f), b.x - 16 * u,
         lit ? focus_ink : theme::paper, text);
    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// The heading's words: what is happening and where to.
std::string heading_text() {
    const auto where = !card.session.empty() ? upper(card.session) : !map_name().empty() ? upper(map_name()) : std::string("THE SESSION");
    return (card.moved ? "STAYING IN " : "JOINING ") + where;
}

// How much of everything has come, in bytes, and how much there is: those before the one in hand
// whole, and the one in hand as far as its download is. The one in hand counts at the size its
// download says, once it says one (a listing's size is the same download's, asked for beforehand).
struct Bytes {
    std::uint64_t done{}, total{};
};
Bytes overall() {
    Bytes out;
    for (std::size_t at = 0; at < card.items.size(); ++at) {
        const auto& item = card.items[at];
        const bool here = at == card.current;
        const std::uint64_t size = here && card.stage == 2 && card.total ? card.total : item.bytes;
        out.total += size;
        if (card.stage >= 4 || at < card.current || (here && card.stage == 3)) out.done += size;
        else if (here && card.stage == 2) out.done += size ? std::min(card.received, size) : card.received;
    }
    return out;
}
// How fast it comes, for the speed and the time left: bytes a second, smoothed over the last few
// seconds so a burst or a stall does not throw the estimate about. Sampled while it downloads; a
// pause (installing one before the next) is not counted as a slow stretch.
struct Rate {
    double sampled{};
    std::uint64_t done{};
    double speed{};
    bool running{};
};
Rate rate;
void update_rate(std::uint64_t done, bool downloading) {
    const double now = ImGui::GetTime();
    if (!downloading) {
        rate.running = false;
        return;
    }
    if (!rate.running || done < rate.done) {
        if (done < rate.done) rate.speed = 0;
        rate.sampled = now, rate.done = done, rate.running = true;
        return;
    }
    const double gap = now - rate.sampled;
    if (gap < .5) return;
    const double fresh = static_cast<double>(done - rate.done) / gap;
    rate.speed = rate.speed > 0 ? rate.speed * .7 + fresh * .3 : fresh;
    rate.sampled = now, rate.done = done;
}
}

bool map_download_pending() {
    const auto read = feed.load();
    card = read ? read() : MapDownloadCard{};
    // (Nothing is shown once the map is in the game: its loading has the game's own screen.)
    if (card.stage == 5) card.stage = 0;
    if (card.stage == 0) rate = {};
    // (A message to say OK to, message_card.cpp, has the pointer as these have, and is drawn
    // with them: it counts as a card to draw.)
    state().prompt_pointer.store(card.stage == 1 || card.stage == 6 || map_vote_asking() || message_pending());
    return card.stage != 0 || message_pending();
}

namespace {
// While it asks, and when it failed: a card in the middle of the screen, over everything (dimmed
// behind it: the game and the pause menu take no input meanwhile), in the look of the pause menu's
// cards. Its heading says where the player is going, with that map's picture; under it everything
// the session needs, a row each, with what it is and what getting it takes; then the buttons, and
// the controls under the card. Answered by a controller (D-pad to pick, A to take the pick, B for
// no), the keyboard (arrow keys, Enter, Esc), the vote binds, or a click: the overlay has the
// pointer while it is up (State::prompt_pointer). A failure is put away by any of them.
void draw_card(State& s) {
    const Fonts f = fonts(s);
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
    const bool failed = card.stage == 6;
    const auto& items = card.items;
    const char* host = card.server ? "server" : "host";
    const auto count = [&](auto test) { return static_cast<std::size_t>(std::count_if(items.begin(), items.end(), test)); };
    const std::size_t here = count([](const MapDownloadCard::Item& item) { return item.installed; });
    const std::size_t plugins = count([](const MapDownloadCard::Item& item) { return item.plugin; });
    const bool all_here = here == items.size();

    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);
    std::vector<Hint> hints;
    if (failed) {
        hints.push_back(pad.available ? Hint{0, {}, "OK"} : Hint{-1, "Enter", "OK"});
    } else {
        hints.push_back({-1, pad.available ? std::string("D-pad") : std::string("< >"), "Pick"});
        hints.push_back(pad.available ? Hint{0, {}, "Select"} : Hint{-1, "Enter", "Select"});
        hints.push_back(pad.available ? Hint{1, {}, "Cancel"} : Hint{-1, "Esc", "Cancel"});
    }

    // What the card says.
    const char* kicker = failed ? "DOWNLOAD FAILED" : card.moved ? "THE MAP CHANGED" : "DOWNLOAD TO JOIN";
    const char* kind = card.server ? "DEDICATED SERVER" : "PLAYER LOBBY";
    const auto heading = heading_text();
    const auto map = map_name();
    std::string summary;
    if (failed) {
        std::size_t installed{};
        for (std::size_t at = 0; at < items.size(); ++at) installed += std::string_view(status_of(at).tag) == "INSTALLED";
        summary = std::format("{} of {} installed. Nothing more is downloaded.", installed, items.size());
    } else {
        summary = card.moved ? std::format("The {} changed to this map, and you need {} to stay in the session.", host, items.size() == 1 ? "this" : "these")
                             : std::format("The {} needs {} to let you in.", host, items.size() == 1 ? "this" : "these");
        if (all_here) summary += items.size() == 1 ? " You have it, switched off: it is switched on." : " You have them, switched off: they are switched on.";
        else if (!here) summary += card.bytes ? " " + megabytes(card.bytes) + " from Thunderstore." : " It comes from Thunderstore.";
        else summary += std::format(" {} here already; the rest{} from Thunderstore.", here, card.bytes ? " (" + megabytes(card.bytes) + ")" : std::string());
    }
    // Under the list: why it failed, or what a plugin is (it is the one kind that runs code).
    const std::string note = failed ? card.failure
                           : plugins ? "A plugin is a script that runs inside your game. It is not turned on by the download: once it is in, it asks you, "
                                       "with the list of what it can do, whether to turn it on."
                                     : std::string();
    const std::string primary = failed ? "OK"
                              : std::string(all_here ? "SWITCH ON" : items.size() == 1 && !items.front().have.empty() ? "UPDATE" : "DOWNLOAD") +
                                    (card.moved ? " AND STAY" : " AND JOIN");

    // How it is laid out: the measures of the pause menu's cards (hub_page.cpp: connecting_page), in
    // the game's units of a 3840 by 2160 screen, so it reads the same at 1080p and 4K.
    const float width = std::min(1680 * u, display.x - 96 * u), inset = 48 * u, inner = width - inset * 2;
    const float picture = 300 * u, between = 44 * u, column = inner - picture - between;
    const float label_size = 28 * u, title_size = 92 * u, map_size = 34 * u, text_size = 30 * u, section_size = 26 * u;
    const float row = 116 * u, row_gap = 10 * u, tall = 104 * u, note_size = 30 * u;
    const float summary_h = f.body->CalcTextSizeA(text_size, FLT_MAX, column, summary.c_str()).y;
    const float head_h = std::max(picture, label_size + 22 * u + title_size + 22 * u + (map.empty() ? 0.0f : map_size + 18 * u) + summary_h);
    const float note_pad = failed ? 24 * u : 0.0f;
    const float note_h = note.empty() ? 0.0f : f.body->CalcTextSizeA(note_size, FLT_MAX, inner - note_pad * 2 - (failed ? 6 * u : 0.0f), note.c_str()).y +
                                                 (failed ? note_pad * 2 + 24 * u + 12 * u : 0.0f);
    const float under = 40 * u + 44 * u; // the controls under the card
    // (A long list is cut short to what fits the screen: the rest are said in a line.)
    const float fixed = inset + head_h + 40 * u + section_size + 18 * u + (note.empty() ? 0.0f : 28 * u + note_h) + 36 * u + tall + inset;
    const auto most = static_cast<std::size_t>(std::max(2.0f, std::floor((display.y * .92f - under - fixed - 40 * u) / (row + row_gap))));
    const std::size_t shown = std::min(items.size(), most);
    const float list_h = static_cast<float>(shown) * (row + row_gap) - row_gap + (shown < items.size() ? 16 * u + 28 * u : 0.0f);
    const float height = fixed + list_h;
    const ImVec2 min(std::floor((display.x - width) * .5f), std::floor(std::max(16 * u, (display.y - height - under) * .5f))), max(min.x + width, min.y + height);

    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, s.hub_pointer ? 170 : 130));
    skate_theme::rough_rect(draw, min, max, card_fill, failed ? 0x4d5 : 0x4d4, u * 2);
    const float left = min.x + inset, right = max.x - inset;
    float y = min.y + inset;

    // The heading: the map's picture, then where the player is going.
    const std::string picture_mark = map.empty() ? std::string("MAP") : map;
    square_picture(draw, f.bold, u, ImVec2(left, y), ImVec2(left + picture, y + picture), map_picture(), picture_mark, true);
    const float text_left = left + picture + between;
    float ty = y;
    draw->AddText(f.bold, label_size, ImVec2(text_left, ty), failed ? skate_theme::danger : theme::blue, kicker);
    plate(draw, f.bold, u, ImVec2(text_left + width_of(f.bold, label_size, kicker) + 20 * u, ty + (label_size - 38 * u * .8f) * .5f), kind, plate_grey,
          theme::paper, .8f);
    ty += label_size + 22 * u;
    brushed(draw, f.title, title_size, ImVec2(text_left, ty), right, u, heading);
    ty += title_size + 22 * u;
    if (!map.empty()) {
        draw->AddText(f.bold, 24 * u, ImVec2(text_left, ty + (map_size - 24 * u) * .5f), quiet, "MAP");
        line(draw, f.bold, map_size, ImVec2(text_left + width_of(f.bold, 24 * u, "MAP") + 16 * u, ty), right, theme::paper, map);
        ty += map_size + 18 * u;
    }
    draw->AddText(f.body, text_size, ImVec2(text_left, ty), quiet, summary.c_str(), nullptr, column);
    y += head_h + 40 * u;

    // What is needed, a row each.
    draw->AddText(f.bold, section_size, ImVec2(left, y), quiet, failed ? "WHAT WAS NEEDED" : "WHAT YOU NEED");
    if (!failed && card.bytes) {
        const auto total = megabytes(card.bytes) + " IN ALL";
        draw->AddText(f.bold, section_size, ImVec2(right - width_of(f.bold, section_size, total), y), theme::paper, total.c_str());
    }
    y += section_size + 18 * u;
    for (std::size_t at = 0; at < shown; ++at) {
        item_row(draw, f, u, ImVec2(left, y), ImVec2(right, y + row), at, false);
        y += row + row_gap;
    }
    y -= row_gap;
    if (shown < items.size()) {
        y += 16 * u;
        const auto more = std::format("and {} more", items.size() - shown);
        draw->AddText(f.body, 28 * u, ImVec2(left, y), quiet, more.c_str());
        y += 28 * u;
    }

    // Why it failed, in a box marked red; or what a plugin is.
    if (!note.empty()) {
        y += 28 * u;
        if (failed) {
            draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + note_h), row_fill);
            draw->AddRectFilled(ImVec2(left, y), ImVec2(left + 6 * u, y + note_h), skate_theme::danger);
            draw->AddText(f.bold, 24 * u, ImVec2(left + 6 * u + note_pad, y + note_pad), skate_theme::danger, "WHAT WENT WRONG");
            draw->AddText(f.body, note_size, ImVec2(left + 6 * u + note_pad, y + note_pad + 24 * u + 12 * u), theme::paper, note.c_str(), nullptr,
                          inner - note_pad * 2 - 6 * u);
        } else {
            draw->AddText(f.body, note_size, ImVec2(left, y), quiet, note.c_str(), nullptr, inner);
        }
        y += note_h;
    }
    y += 36 * u;

    // The buttons: the main one blue, the way out beside it. The picked one is white.
    const auto reply = answer.load();
    if (failed) {
        if (card_button(draw, f.bold, u, ImVec2(left, y), ImVec2(right, y + tall), primary, true, true, 0x55) && reply) reply(false);
    } else {
        const float main_w = std::floor((inner - 20 * u) * .64f);
        if (card_button(draw, f.bold, u, ImVec2(left, y), ImVec2(left + main_w, y + tall), primary, true, card.choice, 0x51) && reply) reply(true);
        if (card_button(draw, f.bold, u, ImVec2(left + main_w + 20 * u, y), ImVec2(right, y + tall), "CANCEL", false, !card.choice, 0x52) && reply)
            reply(false);
    }
    hints_row(draw, f.bold, u, min.x + width * .5f, max.y + 40 * u, hints, pad.style);
}

// After the player said yes: a smaller card at the right edge, out of the way of skating, as the
// Mod Manager's apply card is (hub_page.cpp: draw_mod_apply_card). Where the player is going, how
// far everything is (what has come of all of it, the speed and the time left while it downloads,
// and which step: download, install, apply the mods, then load the map), and the items with their
// state. While it downloads the No bind stops it (Runtime/client_tick.cpp), which the card names.
// It takes no clicks: it sits over the Insert menu's side sheet and the pause menu, which would
// take the same click.
void draw_progress(State& s) {
    const Fonts f = fonts(s);
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
    const bool downloading = card.stage == 2, applying = card.stage == 4;
    const auto& items = card.items;
    const auto [done, total] = overall();
    update_rate(done, downloading);
    const bool several = items.size() > 1;
    const auto of = several ? std::format(" {} OF {}", std::min(card.current, items.size() - 1) + 1, items.size()) : std::string();
    const std::string kicker = downloading ? "DOWNLOADING" + of : card.stage == 3 ? "INSTALLING" + of : "APPLYING MODS";
    const auto heading = heading_text();
    const auto map = map_name();
    // The bar is the bytes while they come, and the merge's steps while the mods are applied.
    const float share = applying ? (card.steps ? static_cast<float>(card.step) / static_cast<float>(card.steps) : -1.0f)
                                 : (total ? static_cast<float>(done) / static_cast<float>(total) : -1.0f);
    const std::string amount = applying ? (card.step_name.empty() ? std::string("Adding it to the game") : card.step_name)
                             : total ? std::format("{} of {}{}{}%", megabytes(done), megabytes(total), dot, static_cast<int>(std::clamp(share, 0.0f, 1.0f) * 100.0f))
                                     : megabytes(done);
    std::string pace;
    if (downloading && rate.speed > 1.0) pace = per_second(rate.speed) + (total > done ? dot + time_left(static_cast<double>(total - done) / rate.speed) : std::string());
    else if (applying && card.steps) pace = std::format("Step {} of {}", std::min(card.step + 1, card.steps), card.steps);
    const bool has_map = !items.empty() && !items.front().plugin;
    const std::array<const char*, 4> steps{"DOWNLOAD", "INSTALL", "APPLY MODS", has_map ? "LOAD MAP" : "JOIN"};
    const int step = downloading ? 0 : card.stage == 3 ? 1 : 2;

    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);

    const float width = std::min(1120 * u, display.x * .5f), inset = 32 * u, strip = 8 * u;
    const float picture = 128 * u, between = 28 * u, label_size = 24 * u, title_size = 56 * u, text_size = 26 * u, bar_h = 16 * u;
    const float row = 100 * u, row_gap = 8 * u, step_size = 22 * u, footer = 44 * u;
    const std::size_t shown = std::min<std::size_t>(items.size(), 3);
    const float head_h = std::max(picture, label_size + 14 * u + title_size + 12 * u + (map.empty() ? 0.0f : text_size));
    float height = inset + head_h + 28 * u + bar_h + 14 * u + text_size + 22 * u + step_size + 24 * u;
    height += static_cast<float>(shown) * (row + row_gap) - row_gap;
    if (shown < items.size()) height += 12 * u + text_size;
    if (downloading) height += 24 * u + footer;
    height += inset;
    // Beside where a vote's card goes (the middle of the right edge): above it.
    const ImVec2 min(std::floor(display.x - 48 * u - width), std::floor(display.y * .16f)), max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    skate_theme::rough_rect(draw, min, max, IM_COL32(26, 26, 26, 238), 0x4d2, u * 2);
    draw->AddRectFilled(min, ImVec2(min.x + strip, max.y), theme::blue);
    const float left = min.x + strip + inset, right = max.x - inset;
    float y = min.y + inset;

    square_picture(draw, f.bold, u, ImVec2(left, y), ImVec2(left + picture, y + picture), map_picture(), map.empty() ? std::string("MAP") : map, true);
    const float text_left = left + picture + between;
    float ty = y;
    draw->AddText(f.bold, label_size, ImVec2(text_left, ty), theme::blue, kicker.c_str());
    ty += label_size + 14 * u;
    brushed(draw, f.title, title_size, ImVec2(text_left, ty), right, u, heading);
    ty += title_size + 12 * u;
    if (!map.empty()) line(draw, f.body, text_size, ImVec2(text_left, ty), right, quiet, map);
    y += head_h + 28 * u;

    // Everything at once: the bar, what has come of all of it, and the speed and the time left.
    bar(draw, ImVec2(left, y), ImVec2(right, y + bar_h), share, theme::blue, 4 * u);
    y += bar_h + 14 * u;
    const float pace_w = pace.empty() ? 0.0f : width_of(f.body, text_size, pace);
    line(draw, f.bold, text_size, ImVec2(left, y), right - pace_w - 24 * u, theme::paper, amount);
    if (!pace.empty()) draw->AddText(f.body, text_size, ImVec2(right - pace_w, y), quiet, pace.c_str());
    y += text_size + 22 * u;

    // The steps, the one under way lit: those done white, those to come dim.
    float sx = left;
    for (int at = 0; at < static_cast<int>(steps.size()); ++at) {
        const ImU32 colour = at < step ? theme::paper : at == step ? theme::blue : later;
        draw->AddText(f.bold, step_size, ImVec2(sx, y), colour, steps[static_cast<std::size_t>(at)]);
        sx += width_of(f.bold, step_size, steps[static_cast<std::size_t>(at)]);
        if (at + 1 < static_cast<int>(steps.size())) {
            // (A small arrow between them, drawn: it reads at any size.)
            const float mid = y + step_size * .5f, gap = 18 * u, arm = 6 * u;
            draw->AddTriangleFilled(ImVec2(sx + gap, mid - arm), ImVec2(sx + gap + arm * 1.2f, mid), ImVec2(sx + gap, mid + arm), at < step ? theme::paper : later);
            sx += gap * 2 + arm * 1.2f;
        }
    }
    y += step_size + 24 * u;

    for (std::size_t at = 0; at < shown; ++at) {
        // (The one in hand is always among those shown.)
        const std::size_t index = items.size() > shown && card.current >= shown ? card.current - (shown - 1) + at : at;
        item_row(draw, f, u, ImVec2(left, y), ImVec2(right, y + row), std::min(index, items.size() - 1), true);
        y += row + row_gap;
    }
    y -= row_gap;
    if (shown < items.size()) {
        y += 12 * u;
        const auto more = std::format("{} items in all", items.size());
        draw->AddText(f.body, text_size, ImVec2(left, y), quiet, more.c_str());
        y += text_size;
    }
    if (!downloading) return;
    y += 24 * u;
    float taken{};
    if (card.no_bind) {
        // The No bind, as the player set it: a key, or a pad's buttons by their names.
        const Hint stop{-1, dingosdk::controller_combo_label(card.no_bind, pad.style), "Cancel"};
        taken = hint(nullptr, f.bold, u, {}, stop, pad.style);
        hint(draw, f.bold, u, ImVec2(right - taken, y), stop, pad.style);
    }
    line(draw, f.body, text_size, ImVec2(left, y + (footer - text_size) * .5f), right - taken - 24 * u, quiet, "You can keep skating while it downloads.");
}
}

void draw_map_download() {
    if (card.stage) {
        auto& s = state();
        if (card.stage == 1 || card.stage == 6) draw_card(s);
        else draw_progress(s);
    }
    draw_message(); // (over whatever else is up)
}

// ---------------------------------------------------------------- the vote for the next map
// A dedicated server's vote for the next map (Server/server_votes.cpp: the rotation's vote) is
// a poll whose answers are maps. It comes up as the maps to pick from: each with its picture,
// its name and its votes, in the middle of the screen as the map download's question is, and
// answered the same ways (D-pad or arrow keys to pick, A or Enter to vote, B or Esc to close
// it, or a click). Voting closes it; the vote then shows as the poll's small card, where the
// answer can still be changed.
namespace {
std::atomic<std::uint32_t> vote_shown{};      // the vote the popup is up for (its id)
std::atomic<std::uint32_t> vote_closed{};     // the vote it was answered or closed for
std::atomic<ULONGLONG> vote_drawn_at{};       // when it was last drawn
std::atomic<int> vote_pick{}, vote_options{}; // the picked answer, of how many
std::atomic<bool> vote_mine_seen{};
}
} // namespace dingosdk::overlay::detail

namespace dingosdk::overlay {
bool map_vote_asking() noexcept {
    const auto shown = detail::vote_shown.load();
    return shown && shown != detail::vote_closed.load() && GetTickCount64() - detail::vote_drawn_at.load() < 400;
}
void map_vote_move(int by) noexcept {
    const int options = detail::vote_options.load();
    if (options > 0) detail::vote_pick.store(((detail::vote_pick.load() + by) % options + options) % options);
}
void map_vote_dismiss() noexcept { detail::vote_closed.store(detail::vote_shown.load()); }
void map_vote_take() noexcept {
    if (!map_vote_asking()) return;
    try {
        detail::queue_multiplayer_action("vote", std::to_string(detail::vote_pick.load() + 1));
    } catch (...) {}
    map_vote_dismiss();
}

void set_map_vote_icons(const std::vector<MapVoteIcon>& icons) noexcept {
    try {
        // One texture for them all: a row of squares, a map to each.
        constexpr UINT side = 256;
        detail::CardPixels atlas;
        std::vector<std::string> names;
        std::vector<bool> has;
        if (!icons.empty()) {
            atlas.width = side * static_cast<UINT>(icons.size());
            atlas.height = side;
            atlas.rgba.assign(static_cast<std::size_t>(atlas.width) * side * 4, 0);
        }
        bool any{};
        for (std::size_t i = 0; i < icons.size(); ++i) {
            names.push_back(icons[i].name);
            detail::CardPixels one;
            bool ok{};
            if (icons[i].side == side && icons[i].rgba.size() == static_cast<std::size_t>(side) * side * 4) {
                one.rgba = icons[i].rgba; // pixels already, at the atlas' size
                one.width = one.height = side;
                ok = true;
            } else {
                ok = !icons[i].png.empty() && icons[i].png.size() <= 4 * 1024 * 1024 && decode(icons[i].png, one, side) &&
                     one.width == side && one.height == side;
            }
            has.push_back(ok);
            any |= ok;
            if (!ok) continue;
            for (UINT row = 0; row < side; ++row)
                std::memcpy(atlas.rgba.data() + (static_cast<std::size_t>(row) * atlas.width + i * side) * 4,
                            one.rgba.data() + static_cast<std::size_t>(row) * side * 4, side * 4);
        }
        std::lock_guard lock(image_mutex);
        vote_icon_names = std::move(names);
        vote_icon_has = std::move(has);
        // (Nothing to show: the card's icon, if one is up, is left alone.)
        if (!any) {
            if (image_is_atlas.load()) {
                image.store(0);
                current = detail::CardPixels{};
                current.serial = ++image_serial;
                waiting = false;
                image_is_atlas.store(false);
            }
            return;
        }
        image.store(0);
        atlas.serial = ++image_serial;
        current = std::move(atlas);
        waiting = true;
        image_is_atlas.store(true);
    } catch (...) {}
}
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
bool draw_map_vote(const MultiplayerVote& vote) {
    const bool open = vote.id && vote.next_map && vote.outcome == 0 && vote.may_vote && vote.answers.size() >= 2;
    if (!open) return false;
    // A new vote comes up with nothing picked over from the last; one already answered (from
    // the keys or chat) is not put in the way again.
    if (vote_shown.exchange(vote.id) != vote.id) {
        vote_pick.store(0);
        vote_mine_seen.store(vote.mine != 0);
        if (vote.mine) vote_closed.store(vote.id);
    } else if (vote.mine && !vote_mine_seen.exchange(true)) {
        vote_closed.store(vote.id);
    }
    if (vote_closed.load() == vote.id) return false;
    vote_drawn_at.store(GetTickCount64());
    const int options = static_cast<int>(vote.answers.size());
    vote_options.store(options);
    const int pick = std::clamp(vote_pick.load(), 0, options - 1);

    auto& s = state();
    auto* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    auto* heading = s.menu.heading ? s.menu.heading : bold;
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
    constexpr ImU32 sticky = IM_COL32(236, 215, 108, 255);
    const auto width_of = [](ImFont* font, float size, const std::string& text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; };

    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);
    struct Hint { std::string key; const char* what; };
    const std::vector<Hint> hints{
        {pad.available ? std::string("D-pad") : std::string("< >"), "Pick"},
        {pad.available ? dingosdk::controller_combo_label(0x1000, pad.style) : std::string("Enter"), "Vote"},
        {pad.available ? dingosdk::controller_combo_label(0x2000, pad.style) : std::string("Esc"), "Close"}};
    const float key_scale = u * 2, hint_gap = 36 * u;
    float hints_width{};
    for (const auto& hint : hints)
        hints_width += width_of(bold, 12 * key_scale, hint.key) + 20 * key_scale + width_of(bold, 14 * key_scale, hint.what);
    hints_width += hint_gap * static_cast<float>(hints.size() - 1);

    // The tiles in a row, as many as fit the screen's width at their size, smaller when there are more.
    const float inset = 44 * u, gap = 24 * u, label_size = 28 * u, title_size = 64 * u;
    const float most = display.x - 200 * u - inset * 2;
    const float tile = std::min(520 * u, (most - gap * static_cast<float>(options - 1)) / static_cast<float>(options));
    const float pad_in = 20 * u, icon = tile - pad_in * 2, name_size = std::min(36 * u, tile * 0.075f), count_size = 30 * u, bar = 12 * u;
    const float tile_height = pad_in + icon + 18 * u + name_size + 14 * u + bar + 12 * u + count_size + pad_in;
    const float width = inset * 2 + tile * static_cast<float>(options) + gap * static_cast<float>(options - 1);
    const float height = inset + label_size + 20 * u + title_size + 30 * u + tile_height + inset;
    const float under = 34 * u + 20 * key_scale;
    const ImVec2 min(std::floor((display.x - width) * 0.5f), std::floor((display.y - height - under) * 0.5f)), max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, s.hub_pointer ? 170 : 110));
    skate_theme::rough_rect(draw, min, max, IM_COL32(26, 26, 26, 250), 0x4d3, u * 2);
    const float left = min.x + inset, right = max.x - inset;
    float y = min.y + inset;
    draw->AddText(bold, label_size, ImVec2(left, y), theme::blue, "TIME IS UP ON THIS MAP");
    {
        const auto time = std::to_string(vote.seconds) + " s";
        draw->AddText(bold, label_size, ImVec2(right - width_of(bold, label_size, time), y), theme::paper, time.c_str());
    }
    y += label_size + 20 * u;
    draw->AddText(heading, title_size, ImVec2(left, y), theme::paper, "VOTE FOR THE NEXT MAP");
    y += title_size + 30 * u;

    // Which square of the atlas is each answer's, if it has one.
    std::vector<int> cell(static_cast<std::size_t>(options), -1);
    int cells{};
    ImTextureID texture{};
    {
        std::lock_guard lock(image_mutex);
        cells = static_cast<int>(vote_icon_names.size());
        if (image_is_atlas.load()) texture = image.load();
        for (int i = 0; i < options; ++i)
            for (int k = 0; k < cells; ++k)
                if (vote_icon_has[static_cast<std::size_t>(k)] && vote_icon_names[static_cast<std::size_t>(k)] == vote.answers[static_cast<std::size_t>(i)]) cell[static_cast<std::size_t>(i)] = k;
    }
    unsigned total{};
    for (const auto count : vote.counts) total += count;
    for (int i = 0; i < options; ++i) {
        const auto& name = vote.answers[static_cast<std::size_t>(i)];
        const unsigned count = static_cast<std::size_t>(i) < vote.counts.size() ? vote.counts[static_cast<std::size_t>(i)] : 0;
        const ImVec2 a(left + (tile + gap) * static_cast<float>(i), y), b(a.x + tile, a.y + tile_height);
        const bool hovered = ImGui::IsMouseHoveringRect(a, b, false);
        if (hovered && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) vote_pick.store(i);
        const bool picked = pick == i, mine = vote.mine == static_cast<std::uint8_t>(i + 1);
        skate_theme::rough_rect(draw, a, b, picked ? theme::blue : skate_theme::tile, 0x600 + static_cast<unsigned>(i), u * 2);
        const ImVec2 icon_min(a.x + pad_in, a.y + pad_in), icon_max(icon_min.x + icon, icon_min.y + icon);
        if (texture && cell[static_cast<std::size_t>(i)] >= 0 && cells > 0) {
            const float from = static_cast<float>(cell[static_cast<std::size_t>(i)]) / static_cast<float>(cells);
            draw->AddImage(texture, icon_min, icon_max, ImVec2(from, 0), ImVec2(from + 1.0f / static_cast<float>(cells), 1));
        } else {
            // No picture: the game's own maps, "Random map" and "Stay here", and one still on its way.
            // Each is drawn as shapes on its own colour, sharp at any size (text this large is not).
            const bool random = name == "Random map", stay = name == "Stay here";
            constexpr ImU32 ink = IM_COL32(26, 26, 26, 255), green = IM_COL32(96, 190, 128, 255);
            skate_theme::rough_rect(draw, icon_min, icon_max, random ? sticky : stay ? green : IM_COL32(52, 56, 62, 255), 0x620 + static_cast<unsigned>(i), u * 2);
            const ImVec2 mid((icon_min.x + icon_max.x) * 0.5f, (icon_min.y + icon_max.y) * 0.5f);
            const float r = icon * 0.26f, line = std::max(2.0f, icon * 0.035f);
            if (random) {
                // A die showing five.
                draw->AddRectFilled(ImVec2(mid.x - r, mid.y - r), ImVec2(mid.x + r, mid.y + r), ink, r * 0.22f);
                const float pip = r * 0.15f, out = r * 0.5f;
                for (const auto& at : {ImVec2(0, 0), ImVec2(-1, -1), ImVec2(1, -1), ImVec2(-1, 1), ImVec2(1, 1)})
                    draw->AddCircleFilled(ImVec2(mid.x + at.x * out, mid.y + at.y * out), pip, sticky, 24);
            } else if (stay) {
                // A clock, and a plus for the time it adds.
                draw->AddCircle(mid, r, ink, 64, line);
                draw->AddLine(mid, ImVec2(mid.x, mid.y - r * 0.62f), ink, line);
                draw->AddLine(mid, ImVec2(mid.x + r * 0.45f, mid.y + r * 0.2f), ink, line);
                draw->AddCircleFilled(mid, line * 0.8f, ink, 16);
                const ImVec2 plus(mid.x + r * 0.95f, mid.y - r * 0.95f);
                const float arm = r * 0.26f;
                draw->AddCircleFilled(plus, arm * 1.7f, ink, 32);
                draw->AddLine(ImVec2(plus.x - arm, plus.y), ImVec2(plus.x + arm, plus.y), green, line);
                draw->AddLine(ImVec2(plus.x, plus.y - arm), ImVec2(plus.x, plus.y + arm), green, line);
            } else {
                // A map without a picture: hills under a sun.
                constexpr ImU32 far_hill = IM_COL32(104, 112, 122, 255), near_hill = IM_COL32(138, 146, 156, 255);
                const float base = mid.y + r * 0.75f;
                draw->AddCircleFilled(ImVec2(mid.x + r * 0.7f, mid.y - r * 0.7f), r * 0.24f, sticky, 32);
                draw->AddTriangleFilled(ImVec2(mid.x - r * 0.2f, base), ImVec2(mid.x + r * 0.55f, mid.y - r * 0.2f), ImVec2(mid.x + r * 1.3f, base), far_hill);
                draw->AddTriangleFilled(ImVec2(mid.x - r * 1.3f, base), ImVec2(mid.x - r * 0.35f, mid.y - r * 0.65f), ImVec2(mid.x + r * 0.6f, base), near_hill);
            }
        }
        float ty = icon_max.y + 18 * u;
        draw->PushClipRect(ImVec2(a.x + pad_in, ty - name_size), ImVec2(b.x - pad_in, ty + name_size * 2), true);
        draw->AddText(bold, name_size, ImVec2(a.x + pad_in, ty), theme::paper, name.c_str());
        draw->PopClipRect();
        ty += name_size + 14 * u;
        draw->AddRectFilled(ImVec2(a.x + pad_in, ty), ImVec2(b.x - pad_in, ty + bar), picked ? IM_COL32(0, 0, 0, 90) : skate_theme::tile_light, 4 * u);
        if (total && count)
            draw->AddRectFilled(ImVec2(a.x + pad_in, ty), ImVec2(a.x + pad_in + icon * static_cast<float>(count) / static_cast<float>(total), ty + bar), sticky, 4 * u);
        ty += bar + 12 * u;
        const auto votes = std::to_string(count) + (count == 1 ? " vote" : " votes") + (mine ? "  -  yours" : "");
        draw->AddText(body, count_size, ImVec2(a.x + pad_in, ty), mine ? sticky : picked ? theme::paper : theme::muted, votes.c_str());
        if (picked) draw->AddRect(a, b, sticky, 0, 0, 6 * u);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            vote_pick.store(i);
            map_vote_take();
        }
    }
    float x = min.x + (width - hints_width) * 0.5f;
    const float hint_y = max.y + 34 * u;
    for (const auto& hint : hints) x += theme::keycap(draw, bold, key_scale, ImVec2(x, hint_y), hint.key.c_str(), hint.what) + hint_gap;
    return true;
}
} // namespace dingosdk::overlay::detail
