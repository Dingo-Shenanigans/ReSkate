#include "overlay_internal.h"
#include "Engine/Game/Input/controller_bindings.h"
#include <format>
#include <mutex>
#include <wincodec.h>

// The card for a map a multiplayer session needs and this PC does not have
// (Extension/Assets/map_download.h), laid out as the mod's page is: its icon, name, author,
// version, size and description, then what is being asked or done. When a dedicated server
// needs plugins too, or plugins alone, one card lists everything instead (draw_asked_list), so
// the player is asked once. It asks whether to download it,
// in the middle of the screen with the controls under it as the game's own menus have them.
// Once the player has said yes it becomes a small card at the right edge, with the download,
// the install and the apply, so they can skate on while they wait; it goes when the map is in
// the game and starts loading. Both in the look of ReSkate's pages in the pause menu (hub_page.cpp).
//
// Answered by a controller (D-pad to pick, A to take the pick, B for no), the keyboard (arrow
// keys, Enter, Esc), the vote binds, or a click: the overlay has the pointer while
// the card asks (State::prompt_pointer). The runtime reads those and holds the game's input meanwhile.
//
// The icon is the package's own, a PNG the downloader hands over (set_map_download_icon). It
// is decoded here and goes to the GPU from the render thread (overlay_render.cpp), into the
// overlay's second texture slot; the first is ImGui's atlas, which is filled once at startup.

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
// square of that many pixels (the vote's pictures share one texture).
bool decode(const std::string& bytes, detail::CardPixels& out, UINT side = 0) {
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
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size()))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width && height &&
            width <= (side ? 2048U : 512U) && height <= (side ? 2048U : 512U) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            (!side || (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
                       SUCCEEDED(scaler->Initialize(frame.Get(), side, side, WICBitmapInterpolationModeFant)))) &&
            SUCCEEDED(converter->Initialize(side ? static_cast<IWICBitmapSource*>(scaler.Get()) : static_cast<IWICBitmapSource*>(frame.Get()),
                                            GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom))) {
            if (side) width = height = side;
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

ImU32 faded(ImU32 colour, float alpha) {
    const auto base = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(base * std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
}
std::string megabytes(std::uint64_t bytes) {
    const auto mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb >= 1024.0 ? std::format("{:.2f} GB", mb / 1024.0) : bytes && mb < 1.0 ? std::string("1 MB") : std::format("{:.0f} MB", mb);
}
// Whether the card lists what is asked for: a map with plugins, or plugins alone. A map alone
// has its own card, as it always had.
bool listed(const MapDownloadCard& of) {
    return of.items.size() > 1 || (of.items.size() == 1 && of.items.front().plugin);
}
}

bool map_download_pending() {
    const auto read = feed.load();
    card = read ? read() : MapDownloadCard{};
    // (Nothing is shown once the map is in the game: its loading has the game's own screen.)
    if (card.stage == 5) card.stage = 0;
    // (A message to say OK to, message_card.cpp, has the pointer as these have, and is drawn
    // with them: it counts as a card to draw.)
    state().prompt_pointer.store(card.stage == 1 || map_vote_asking() || message_pending());
    return card.stage != 0 || message_pending();
}

namespace {
// After the player said yes: a small card at the right edge, out of the way of skating, with
// what is being fetched and how far it is.
void draw_progress(State& s) {
    auto* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
    const bool downloading = card.stage == 2, applying = card.stage == 4;
    // Several packages: which of them is in hand, and its name.
    const bool several = listed(card);
    const auto count = several && !applying ? std::format(" {} OF {}", card.current + 1, card.items.size()) : std::string();
    const std::string title = downloading ? (several ? "DOWNLOADING" + count : "DOWNLOADING MAP")
                            : card.stage == 3 ? (several ? "INSTALLING" + count : "INSTALLING MAP")
                            : several ? "APPLYING MODS" : "APPLYING MAP";
    const std::string name = several && !applying && card.current < card.items.size() ? card.items[card.current].name
                           : card.map.empty() && !card.items.empty() ? card.items.front().name : card.map;
    const std::string status = downloading ? (card.total ? megabytes(card.received) + " of " + megabytes(card.total) : megabytes(card.received))
                             : card.stage == 3 ? std::string("Putting it in your Mods folder...")
                             : card.step_name.empty() ? std::string("Adding it to the game...") : card.step_name + "...";
    const float width = 820 * u, inset = 30 * u, icon = 132 * u, between = 26 * u, bar = 12 * u;
    const float label_size = 24 * u, name_size = 38 * u, text_size = 26 * u;
    const float column = width - inset * 2 - icon - between;
    const float height = inset + std::max(icon, label_size + 12 * u + name_size + 18 * u + bar + 14 * u + text_size) + inset;
    // Beside where a vote's card goes (the middle of the right edge): above it.
    const ImVec2 min(std::floor(display.x - 48 * u - width), std::floor(display.y * 0.2f)), max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    skate_theme::rough_rect(draw, min, max, IM_COL32(26, 26, 26, 235), 0x4d2, u * 2);
    const float left = min.x + inset, top = min.y + inset;
    const ImVec2 icon_min(left, top), icon_max(left + icon, top + icon);
    if (const auto texture = card_icon()) draw->AddImage(texture, icon_min, icon_max);
    else skate_theme::rough_rect(draw, icon_min, icon_max, skate_theme::tile_light, 0x1c1, u * 2);
    const float x = left + icon + between, right = max.x - inset;
    float y = top;
    draw->AddText(bold, label_size, ImVec2(x, y), theme::blue, title.c_str());
    y += label_size + 12 * u;
    draw->PushClipRect(ImVec2(x, y - name_size), ImVec2(right, y + name_size * 2), true);
    draw->AddText(bold, name_size, ImVec2(x, y), theme::paper, name.c_str());
    draw->PopClipRect();
    y += name_size + 18 * u;
    const float share = downloading ? (card.total ? static_cast<float>(card.received) / static_cast<float>(card.total) : -1.0f)
                                    : (applying && card.steps ? static_cast<float>(card.step) / static_cast<float>(card.steps) : -1.0f);
    draw->AddRectFilled(ImVec2(x, y), ImVec2(right, y + bar), skate_theme::tile_light, 4 * u);
    if (share >= 0.0f) draw->AddRectFilled(ImVec2(x, y), ImVec2(x + column * std::clamp(share, 0.0f, 1.0f), y + bar), theme::blue, 4 * u);
    else {
        // How far is not known: a block going back and forth.
        const float at = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0));
        const float where = (at < 1.0f ? at : 2.0f - at) * (column * 0.75f);
        draw->AddRectFilled(ImVec2(x + where, y), ImVec2(x + where + column * 0.25f, y + bar), theme::blue, 4 * u);
    }
    y += bar + 14 * u;
    draw->PushClipRect(ImVec2(x, y - text_size), ImVec2(right, y + text_size * 2), true);
    draw->AddText(body, text_size, ImVec2(x, y), theme::muted, status.c_str());
    draw->PopClipRect();
}
}

void draw_asked_map(State& s);
void draw_asked_list(State& s);
void draw_map_download() {
    if (card.stage) {
        auto& s = state();
        if (card.stage != 1) draw_progress(s);
        else if (listed(card)) draw_asked_list(s);
        else draw_asked_map(s);
    }
    draw_message(); // (over whatever else is up)
}

// A dedicated server's plugins, with its map when that is missing too: one card that lists all
// of it, asked once. Laid out as the map's card is (the pause menu's look), a row for each
// package: what it is, its name and author, and what getting it takes. Answered the same ways.
void draw_asked_list(State& s) {
    auto* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    auto* heading = s.menu.heading ? s.menu.heading : bold;
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
    constexpr ImU32 sticky = IM_COL32(236, 215, 108, 255), sticky_dim = IM_COL32(150, 140, 86, 255);
    const char* host = card.server ? "server" : "host";
    const auto width_of = [](ImFont* font, float size, const std::string& text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; };
    const bool with_map = !card.items.front().plugin;
    const bool all_here = std::all_of(card.items.begin(), card.items.end(), [](const MapDownloadCard::Item& item) { return item.installed; });

    const char* title = with_map ? (card.moved ? "MAP CHANGED" : "MAP AND PLUGINS NEEDED") : "PLUGINS NEEDED";
    const auto plugins = static_cast<std::size_t>(std::count_if(card.items.begin(), card.items.end(),
        [](const MapDownloadCard::Item& item) { return item.plugin; }));
    const std::string headline = with_map ? std::format("{} and {} {}", card.items.front().name, plugins, plugins == 1 ? "plugin" : "plugins")
                                          : std::format("{} {}", plugins, plugins == 1 ? "plugin" : "plugins");
    const std::string why = std::format("The {} needs {} to join. Plugins and maps are the only mods a {} can ask for, and they come from Thunderstore.",
                                        host, card.items.size() == 1 ? "this" : "these", host);
    const std::string total = card.bytes ? "Downloads " + megabytes(card.bytes) + " in all" : std::string();

    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);
    struct Hint { std::string key; const char* what; };
    const std::vector<Hint> hints{
        {pad.available ? std::string("D-pad") : std::string("< >"), "Pick"},
        {pad.available ? dingosdk::controller_combo_label(0x1000, pad.style) : std::string("Enter"), "Select"},
        {pad.available ? dingosdk::controller_combo_label(0x2000, pad.style) : std::string("Esc"), "Cancel"}};
    const float key_scale = u * 2, hint_gap = 36 * u;
    float hints_width{};
    for (const auto& hint : hints)
        hints_width += width_of(bold, 12 * key_scale, hint.key) + 20 * key_scale + width_of(bold, 14 * key_scale, hint.what);
    hints_width += hint_gap * static_cast<float>(hints.size() - 1);

    const float width = 1500 * u, inset = 44 * u, tall = 110 * u, row = 104 * u, row_gap = 12 * u;
    const float inner = width - inset * 2;
    const float label_size = 28 * u, name_size = 64 * u, row_name = 36 * u, row_text = 28 * u, text_size = 32 * u;
    const auto why_extent = body->CalcTextSizeA(text_size, FLT_MAX, inner, why.c_str());
    // (A long list is cut short to what fits the screen: the rest are said in a line.)
    const std::size_t most = std::max<std::size_t>(3, static_cast<std::size_t>((display.y * 0.45f) / (row + row_gap)));
    const std::size_t shown = std::min(card.items.size(), most);
    float height = inset + label_size + 20 * u + name_size + 24 * u + why_extent.y + 30 * u +
                   static_cast<float>(shown) * (row + row_gap) + inset + tall + inset;
    if (shown < card.items.size()) height += row_text + 12 * u;
    if (!total.empty()) height += row_text + 18 * u;
    const float under = 34 * u + 20 * key_scale;
    const ImVec2 min(std::floor((display.x - width) * 0.5f), std::floor((display.y - height - under) * 0.5f)), max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, s.hub_pointer ? 170 : 110));
    skate_theme::rough_rect(draw, min, max, IM_COL32(26, 26, 26, 250), 0x4d4, u * 2);
    const float left = min.x + inset, right = max.x - inset;
    float y = min.y + inset;
    draw->AddText(bold, label_size, ImVec2(left, y), theme::blue, title);
    y += label_size + 20 * u;
    draw->PushClipRect(ImVec2(left, y - name_size), ImVec2(right, y + name_size * 2), true);
    draw->AddText(heading, name_size, ImVec2(left, y), theme::paper, headline.c_str());
    draw->PopClipRect();
    y += name_size + 24 * u;
    draw->AddText(body, text_size, ImVec2(left, y), theme::muted, why.c_str(), nullptr, inner);
    y += why_extent.y + 30 * u;

    // A row for each: MAP or PLUGIN, its name and who made it, and on the right what it takes.
    for (std::size_t at = 0; at < shown; ++at) {
        const auto& item = card.items[at];
        const ImVec2 a(left, y), b(right, y + row);
        skate_theme::rough_rect(draw, a, b, skate_theme::tile, 0x700 + static_cast<unsigned>(at), u * 2);
        const std::string kind = item.plugin ? "PLUGIN" : "MAP";
        const float tag_size = 24 * u, tag_pad = 14 * u, tag_width = 150 * u;
        const ImVec2 tag_min(a.x + 24 * u, a.y + (row - tag_size - tag_pad * 2) * 0.5f),
            tag_max(tag_min.x + tag_width, tag_min.y + tag_size + tag_pad * 2);
        skate_theme::rough_rect(draw, tag_min, tag_max, item.plugin ? theme::blue : sticky, 0x720 + static_cast<unsigned>(at), u * 2);
        draw->AddText(bold, tag_size, ImVec2(tag_min.x + (tag_width - width_of(bold, tag_size, kind)) * 0.5f, tag_min.y + tag_pad),
                      skate_theme::black, kind.c_str());
        const std::string what = item.installed ? "Here, switched off"
                               : !item.have.empty() ? "Update " + item.have + " \xe2\x86\x92 " + item.version
                               : item.bytes ? megabytes(item.bytes) : std::string("Download");
        const float what_width = width_of(bold, row_text, what);
        const float text_x = tag_max.x + 28 * u, text_right = b.x - 24 * u - what_width - 28 * u;
        draw->PushClipRect(ImVec2(text_x, a.y), ImVec2(text_right, b.y), true);
        draw->AddText(bold, row_name, ImVec2(text_x, a.y + 14 * u), theme::paper, item.name.c_str());
        const auto by = "by " + item.author + (item.version.empty() || !item.have.empty() ? std::string() : "  v" + item.version);
        draw->AddText(body, row_text, ImVec2(text_x, a.y + 14 * u + row_name + 8 * u), theme::muted, by.c_str());
        draw->PopClipRect();
        draw->AddText(bold, row_text, ImVec2(b.x - 24 * u - what_width, a.y + (row - row_text) * 0.5f),
                      item.installed || item.have.empty() ? theme::paper : theme::blue, what.c_str());
        y += row + row_gap;
    }
    if (shown < card.items.size()) {
        const auto more = std::format("and {} more", card.items.size() - shown);
        draw->AddText(body, row_text, ImVec2(left, y), theme::muted, more.c_str());
        y += row_text + 12 * u;
    }
    if (!total.empty()) {
        draw->AddText(body, row_text, ImVec2(left, y + 6 * u), theme::muted, total.c_str());
        y += row_text + 18 * u;
    }
    y += inset - row_gap;
    const float button_width = (inner - 24 * u) * 0.5f;
    const auto button = [&](float at, bool yes, unsigned seed) {
        const ImVec2 a(at, y), b(at + button_width, y + tall);
        const bool picked = card.choice == yes;
        const bool hovered = ImGui::IsMouseHoveringRect(a, b, false);
        const bool lit = picked || hovered;
        const ImU32 fill = yes ? (lit ? sticky : sticky_dim) : (lit ? theme::blue : skate_theme::tile_light);
        skate_theme::rough_rect(draw, a, b, fill, seed, u * 2);
        if (yes && picked) draw->AddRect(a, b, theme::blue, 0, 0, 6 * u);
        const std::string label = yes ? (all_here ? "SWITCH ON" : card.items.size() == 1 ? "DOWNLOAD" : "GET ALL") : "CANCEL";
        const float size = 40 * u;
        draw->AddText(bold, size, ImVec2(a.x + (button_width - width_of(bold, size, label)) * 0.5f, a.y + (tall - size) * 0.5f),
                      yes || lit ? skate_theme::black : theme::paper, label.c_str());
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            if (const auto reply = answer.load()) reply(yes);
    };
    button(left, true, 0x53);
    button(left + button_width + 24 * u, false, 0x54);
    float x = min.x + (width - hints_width) * 0.5f;
    const float hint_y = max.y + 34 * u;
    for (const auto& hint : hints) x += theme::keycap(draw, bold, key_scale, ImVec2(x, hint_y), hint.key.c_str(), hint.what) + hint_gap;
}
void draw_asked_map(State& s) {
    // Drawn as the pages of the game's pause menu are (hub_page.cpp): the same unit (one of
    // the game's 3840 by 2160), brushed heading, hand-cut tiles, the yellow note for what to
    // press and the keycaps under it.
    auto* bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* body = s.menu.body ? s.menu.body : ImGui::GetFont();
    auto* heading = s.menu.heading ? s.menu.heading : bold;
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
    constexpr ImU32 sticky = IM_COL32(236, 215, 108, 255), sticky_dim = IM_COL32(150, 140, 86, 255);
    const bool asking = card.stage == 1, downloading = card.stage == 2, applying = card.stage == 4;
    const bool clickable = asking || s.visible.load() || s.console_visible.load();
    const char* host = card.server ? "server" : "host";
    const auto width_of = [](ImFont* font, float size, const std::string& text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; };

    const char* title = asking ? (!card.have.empty() ? "MAP VERSION" : card.moved ? "MAP CHANGED" : "MAP NEEDED") : downloading ? "DOWNLOADING MAP" : card.stage == 3 ? "INSTALLING MAP"
                      : applying ? "APPLYING MAP" : "JOINING";
    // Why it is asked: the session moved here with the player in it, or the player is joining.
    const auto why = card.moved ? std::format("The {} changed to this map, and you need it to stay in the session.", host)
                                : std::format("The {} is on this map, and you need it to join.", host);
    const std::string status = asking ? (!card.have.empty() ? std::format("The {} runs version {} of this map and you have {}. Update to its version?", host, card.version, card.have)
                                                            : why + (card.installed ? " You have it, switched off. Switch it on?" : " Download it?"))
                             : downloading ? (card.total ? megabytes(card.received) + " of " + megabytes(card.total) : megabytes(card.received))
                             : card.stage == 3 ? std::string("Putting it in your Mods folder...")
                             : applying ? (card.step_name.empty() ? std::string("Adding it to the game...") : card.step_name + "...")
                             : std::string("Loading the map...");
    struct Fact { const char* name; std::string value; };
    std::vector<Fact> facts{{"Author", card.author}};
    if (!card.version.empty()) facts.push_back({"Version", card.version});
    facts.push_back({"From", card.installed ? std::string("Your Mods folder, switched off") : std::string("Thunderstore")});
    // A download or an update. A mod that is here and only switched off has nothing to fetch.
    if (asking && !card.installed && card.bytes) facts.push_back({"Size", megabytes(card.bytes)});

    // The controls under the card: the pad's own names for its buttons when one is connected.
    dingosdk::ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);
    struct Hint { std::string key; const char* what; };
    std::vector<Hint> hints;
    if (asking) {
        hints.push_back({pad.available ? std::string("D-pad") : std::string("< >"), "Pick"});
        hints.push_back({pad.available ? dingosdk::controller_combo_label(0x1000, pad.style) : std::string("Enter"), "Select"});
        hints.push_back({pad.available ? dingosdk::controller_combo_label(0x2000, pad.style) : std::string("Esc"), "Cancel"});
    }
    const float key_scale = u * 2, hint_gap = 36 * u;
    float hints_width{};
    for (const auto& hint : hints)
        hints_width += width_of(bold, 12 * key_scale, hint.key) + 20 * key_scale + width_of(bold, 14 * key_scale, hint.what);
    if (!hints.empty()) hints_width += hint_gap * static_cast<float>(hints.size() - 1);

    const float width = 1500 * u, inset = 44 * u, icon = 300 * u, between = 44 * u, tall = 110 * u, bar = 14 * u;
    const float inner = width - inset * 2, column = inner - icon - between;
    const float label_size = 28 * u, name_size = 64 * u, fact_size = 34 * u, text_size = 32 * u;
    const auto name_extent = heading->CalcTextSizeA(name_size, FLT_MAX, column, card.map.c_str());
    const auto described = card.description.empty() ? ImVec2{} : body->CalcTextSizeA(text_size, FLT_MAX, inner, card.description.c_str());
    const float beside = name_extent.y + 22 * u + static_cast<float>(facts.size()) * 54 * u;
    const float top_block = std::max(icon, beside);
    const auto status_extent = body->CalcTextSizeA(text_size, FLT_MAX, inner, status.c_str());
    const bool progress = downloading || applying;
    float height = inset + label_size + 26 * u + top_block + inset + status_extent.y + inset;
    if (!card.description.empty()) height += described.y + 36 * u;
    if (progress) height += bar + 30 * u;
    if (asking) height += 30 * u + tall;
    if (downloading) height += 16 * u + label_size;
    const float under = hints.empty() ? 0.0f : 34 * u + 20 * key_scale;
    // In the middle while it asks; near the top, out of the way, while the player skates on.
    const float top = asking ? (display.y - height - under) * 0.5f : 150 * u;
    const ImVec2 min(std::floor((display.x - width) * 0.5f), std::floor(top)), max(min.x + width, min.y + height);
    auto* draw = ImGui::GetForegroundDrawList();
    // While it asks, what is behind is dimmed: the pause menu takes no input then, nor the game.
    if (asking) draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, s.hub_pointer ? 170 : 110));
    skate_theme::rough_rect(draw, min, max, IM_COL32(26, 26, 26, 250), 0x4d1, u * 2);
    const float left = min.x + inset, right = max.x - inset;
    float y = min.y + inset;
    draw->AddText(bold, label_size, ImVec2(left, y), theme::blue, title);
    y += label_size + 26 * u;

    // The icon, or a plain tile until it is here (and for a mod that has none).
    const ImVec2 icon_min(left, y), icon_max(left + icon, y + icon);
    if (const auto texture = card_icon()) draw->AddImage(texture, icon_min, icon_max);
    else {
        skate_theme::rough_rect(draw, icon_min, icon_max, skate_theme::tile_light, 0x1c0, u * 2);
        const std::string mark = "MAP";
        draw->AddText(heading, 56 * u, ImVec2(left + (icon - width_of(heading, 56 * u, mark)) * 0.5f, y + (icon - 56 * u) * 0.5f), theme::muted, mark.c_str());
    }
    // Beside it, the map's name and what is known of the mod, as a session's facts are listed.
    const float text_left = left + icon + between;
    float text_y = y;
    draw->AddText(heading, name_size, ImVec2(text_left, text_y), theme::paper, card.map.c_str(), nullptr, column);
    text_y += name_extent.y + 22 * u;
    for (const auto& fact : facts) {
        draw->AddText(body, fact_size, ImVec2(text_left, text_y), theme::muted, fact.name);
        draw->PushClipRect(ImVec2(text_left + 220 * u, text_y - fact_size), ImVec2(right, text_y + fact_size * 2), true);
        draw->AddText(bold, fact_size, ImVec2(std::max(text_left + 220 * u, right - width_of(bold, fact_size, fact.value)), text_y), theme::paper, fact.value.c_str());
        draw->PopClipRect();
        text_y += 54 * u;
    }
    y += top_block + inset;
    if (!card.description.empty()) {
        draw->AddText(body, text_size, ImVec2(left, y), theme::muted, card.description.c_str(), nullptr, inner);
        y += described.y + 36 * u;
    }
    if (progress) {
        const float share = downloading ? (card.total ? static_cast<float>(card.received) / static_cast<float>(card.total) : -1.0f)
                                        : (card.steps ? static_cast<float>(card.step) / static_cast<float>(card.steps) : -1.0f);
        draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + bar), skate_theme::tile_light, 4 * u);
        if (share >= 0.0f) draw->AddRectFilled(ImVec2(left, y), ImVec2(left + inner * std::clamp(share, 0.0f, 1.0f), y + bar), theme::blue, 4 * u);
        else {
            // How far is not known: a block going back and forth.
            const float at = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0));
            const float where = (at < 1.0f ? at : 2.0f - at) * (inner * 0.75f);
            draw->AddRectFilled(ImVec2(left + where, y), ImVec2(left + where + inner * 0.25f, y + bar), theme::blue, 4 * u);
        }
        y += bar + 30 * u;
    }
    draw->AddText(body, text_size, ImVec2(left, y), asking ? theme::paper : theme::muted, status.c_str(), nullptr, inner);
    y += status_extent.y;
    if (downloading) {
        y += 16 * u;
        draw->AddText(body, label_size, ImVec2(left, y), theme::muted, "You can keep skating while it downloads.");
        return;
    }
    if (!asking) return;
    y += 30 * u;
    // The game's two looks for a button: the yellow note for the thing to do, a tile for the
    // way out. The picked one is the lit one: the note full yellow in a blue edge, the tile blue.
    const float button_width = (inner - 24 * u) * 0.5f;
    const auto button = [&](float at, bool yes, unsigned seed) {
        const ImVec2 a(at, y), b(at + button_width, y + tall);
        const bool picked = card.choice == yes;
        const bool hovered = clickable && ImGui::IsMouseHoveringRect(a, b, false);
        const bool lit = picked || hovered;
        const ImU32 fill = yes ? (lit ? sticky : sticky_dim) : (lit ? theme::blue : skate_theme::tile_light);
        skate_theme::rough_rect(draw, a, b, fill, seed, u * 2);
        if (yes && picked) draw->AddRect(a, b, theme::blue, 0, 0, 6 * u);
        const std::string label = yes ? (!card.have.empty() ? "UPDATE" : card.installed ? "SWITCH ON" : "DOWNLOAD") : "CANCEL";
        const float size = 40 * u;
        draw->AddText(bold, size, ImVec2(a.x + (button_width - width_of(bold, size, label)) * 0.5f, a.y + (tall - size) * 0.5f),
                      yes || lit ? skate_theme::black : theme::paper, label.c_str());
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            if (const auto reply = answer.load()) reply(yes);
    };
    button(left, true, 0x51);
    button(left + button_width + 24 * u, false, 0x52);
    // The controls, under the card and in its middle.
    float x = min.x + (width - hints_width) * 0.5f;
    const float hint_y = max.y + 34 * u;
    for (const auto& hint : hints) x += theme::keycap(draw, bold, key_scale, ImVec2(x, hint_y), hint.key.c_str(), hint.what) + hint_gap;
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
