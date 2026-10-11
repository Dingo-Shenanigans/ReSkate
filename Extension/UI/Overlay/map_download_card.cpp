#include "overlay_internal.h"
#include "Engine/Game/Input/controller_bindings.h"
#include <format>
#include <mutex>
#include <wincodec.h>

// The card for a map a multiplayer session needs and this PC does not have
// (Extension/Assets/map_download.h), laid out as the mod's page is: its icon, name, author,
// version and description, then what is being asked or done. It asks whether to download it,
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

// Straight-alpha RGBA, at most 512 pixels a side (an icon is 256).
bool decode(const std::string& bytes, detail::CardPixels& out) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok{};
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        UINT width{}, height{};
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size()))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width && height &&
            width <= 512 && height <= 512 && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom))) {
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
        pixels.serial = ++image_serial;
        current = decoded ? std::move(pixels) : detail::CardPixels{};
        current.serial = image_serial;
        waiting = decoded;
    } catch (...) {}
}
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
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
    return mb >= 1024.0 ? std::format("{:.2f} GB", mb / 1024.0) : std::format("{:.0f} MB", mb);
}
}

bool map_download_pending() {
    const auto read = feed.load();
    card = read ? read() : MapDownloadCard{};
    // (Nothing is shown once the map is in the game: its loading has the game's own screen.)
    if (card.stage == 5) card.stage = 0;
    state().prompt_pointer.store(card.stage == 1);
    return card.stage != 0;
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
    const char* title = downloading ? "DOWNLOADING MAP" : card.stage == 3 ? "INSTALLING MAP" : "APPLYING MAP";
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
    if (const auto texture = image.load()) draw->AddImage(texture, icon_min, icon_max);
    else skate_theme::rough_rect(draw, icon_min, icon_max, skate_theme::tile_light, 0x1c1, u * 2);
    const float x = left + icon + between, right = max.x - inset;
    float y = top;
    draw->AddText(bold, label_size, ImVec2(x, y), theme::blue, title);
    y += label_size + 12 * u;
    draw->PushClipRect(ImVec2(x, y - name_size), ImVec2(right, y + name_size * 2), true);
    draw->AddText(bold, name_size, ImVec2(x, y), theme::paper, card.map.c_str());
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

void draw_map_download() {
    if (!card.stage) return;
    auto& s = state();
    if (card.stage != 1) return draw_progress(s);
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

    const char* title = asking ? (card.moved ? "MAP CHANGED" : "MAP NEEDED") : downloading ? "DOWNLOADING MAP" : card.stage == 3 ? "INSTALLING MAP"
                      : applying ? "APPLYING MAP" : "JOINING";
    // Why it is asked: the session moved here with the player in it, or the player is joining.
    const auto why = card.moved ? std::format("The {} changed to this map, and you need it to stay in the session.", host)
                                : std::format("The {} is on this map, and you need it to join.", host);
    const std::string status = asking ? why + (card.installed ? " You have it, switched off. Switch it on?" : " Download it?")
                             : downloading ? (card.total ? megabytes(card.received) + " of " + megabytes(card.total) : megabytes(card.received))
                             : card.stage == 3 ? std::string("Putting it in your Mods folder...")
                             : applying ? (card.step_name.empty() ? std::string("Adding it to the game...") : card.step_name + "...")
                             : std::string("Loading the map...");
    struct Fact { const char* name; std::string value; };
    std::vector<Fact> facts{{"Author", card.author}};
    if (!card.version.empty()) facts.push_back({"Version", card.version});
    facts.push_back({"From", card.installed ? std::string("Your Mods folder, switched off") : std::string("Thunderstore")});

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
    if (const auto texture = image.load()) draw->AddImage(texture, icon_min, icon_max);
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
        const std::string label = yes ? (card.installed ? "SWITCH ON" : "DOWNLOAD") : "CANCEL";
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
} // namespace dingosdk::overlay::detail
