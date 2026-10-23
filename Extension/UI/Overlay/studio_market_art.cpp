#include "studio_market_art.h"
#include "overlay_internal.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/AnimationStudio/marketplace.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

// The MARKET's art on its way to the GPU (studio_market_art.h): read from the DLL off the render thread, the wall in a
// texture of its own (the SRV heap's seventh slot) and the rest packed into one sheet (the eighth), each tiling texture
// with a border of itself wrapped round it, so a quad that runs a little past a tile's edge still finds the tile there.
// The cover designer's preview has the ninth (overlay_render.cpp, upload_market_art).
namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
constexpr int tile_side = 1024, tile_border = 32;

struct Art {
    std::mutex mutex;
    bool started{};
    detail::CardPixels wall, sheet, preview;
    bool wall_waiting{}, sheet_waiting{}, preview_waiting{};
    unsigned serial{}, preview_serial{};
    std::array<ArtPlace, static_cast<std::size_t>(MarketArt::count)> places{};
    std::map<std::string, ArtPlace> crew; // "<team, lower case>|profile" or "|thumbnail"
    ImTextureID wall_texture{}, sheet_texture{}, preview_texture{};
    std::vector<std::uint8_t> preview_kept; // the preview again after the device is lost
    int preview_side{};
};
Art& art() {
    static auto* value = new Art; // process lifetime
    return *value;
}

std::string crew_key(std::string_view team, CrewArt which) {
    std::string key;
    for (const char c : team) key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key + (which == CrewArt::profile ? "|profile" : "|thumbnail");
}
std::optional<ArtPixels> decode_with(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, int most_wide) {
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width{}, height{};
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 8192 || height > 8192)
        return std::nullopt;
    ComPtr<IWICBitmapSource> source = frame;
    if (most_wide > 0 && static_cast<int>(width) > most_wide) {
        ComPtr<IWICBitmapScaler> scaler;
        const UINT w = static_cast<UINT>(most_wide), h = std::max(1u, static_cast<UINT>(static_cast<double>(height) * most_wide / width));
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeHighQualityCubic)))
            return std::nullopt;
        source = scaler;
        width = w;
        height = h;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return std::nullopt;
    ArtPixels out;
    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()))) return std::nullopt;
    return out;
}
struct Com {
    bool ok;
    Com() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~Com() {
        if (ok) CoUninitialize();
    }
};

struct Canvas {
    std::vector<std::uint8_t> rgba;
    int width{}, height{};
};
void blit(Canvas& c, const ArtPixels& p, int x, int y) {
    for (int row = 0; row < p.height && y + row < c.height; ++row)
        std::memcpy(c.rgba.data() + (static_cast<std::size_t>(y + row) * c.width + x) * 4, p.rgba.data() + static_cast<std::size_t>(row) * p.width * 4,
                    static_cast<std::size_t>(std::min(p.width, c.width - x)) * 4);
}
// A tiling texture with its border: the texels round it are its own, from the opposite edge.
void blit_tile(Canvas& c, const ArtPixels& p, int x, int y) {
    const int side = tile_side + tile_border * 2;
    for (int row = 0; row < side; ++row) {
        const int sy = ((row - tile_border) % p.height + p.height) % p.height;
        for (int col = 0; col < side; ++col) {
            const int sx = ((col - tile_border) % p.width + p.width) % p.width;
            std::memcpy(c.rgba.data() + (static_cast<std::size_t>(y + row) * c.width + x + col) * 4,
                        p.rgba.data() + (static_cast<std::size_t>(sy) * p.width + sx) * 4, 4);
        }
    }
}

void read_art() {
    Com com;
    auto& a = art();
    const auto log_missing = [](const char* what) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Animator Studio market: {} could not be read.", what);
    };
    // The wall, whole.
    detail::CardPixels wall;
    if (auto w = decode_studio_resource(L"STUDIO_ART_WALLPAPER", 3840)) {
        wall.rgba = std::move(w->rgba);
        wall.width = static_cast<UINT>(w->width);
        wall.height = static_cast<UINT>(w->height);
    } else log_missing("the wall");
    // The sheet: the three tiles along the top, the pictures under them.
    struct Item {
        MarketArt art;
        const wchar_t* name;
        int most_wide;
        bool tile;
    };
    const std::array<Item, 6> items{{{MarketArt::grip, L"STUDIO_ART_GRIP", 0, true},
                                     {MarketArt::paper_white, L"STUDIO_ART_PAPER_WHITE", 0, true},
                                     {MarketArt::paper_blue, L"STUDIO_ART_PAPER_BLUE", 0, true},
                                     {MarketArt::skater, L"STUDIO_ART_SKATER", 1000, false},
                                     {MarketArt::splatter_a, L"STUDIO_ART_SPLATTER_A", 0, false},
                                     {MarketArt::splatter_b, L"STUDIO_ART_SPLATTER_B", 0, false}}};
    std::array<std::optional<ArtPixels>, 6> pictures;
    for (std::size_t i = 0; i < items.size(); ++i) {
        pictures[i] = decode_studio_resource(items[i].name, items[i].most_wide);
        if (!pictures[i] || (items[i].tile && (pictures[i]->width != tile_side || pictures[i]->height != tile_side))) {
            pictures[i].reset();
            log_missing("a picture of the shop");
        }
    }
    const int cell = tile_side + tile_border * 2;
    int row_width = 0, row_height = 0;
    for (std::size_t i = 3; i < items.size(); ++i)
        if (pictures[i]) row_width += pictures[i]->width + 8, row_height = std::max(row_height, pictures[i]->height);
    // THE CREW's pictures, those who sent theirs: in rows under the rest, as many to a row as fit.
    struct CrewPicture {
        std::string key;
        ArtPixels pixels;
        int x{}, y{};
    };
    std::vector<CrewPicture> crew;
    std::vector<std::string_view> owners; // THE CREW by team, BUILT BY by their pictures' name
    for (const auto& member : anim_studio::market::crew_members) owners.push_back(member.team);
    for (const auto& builder : anim_studio::market::builders) owners.push_back(builder.pictures);
    for (const auto owner : owners)
        for (const auto which : {CrewArt::profile, CrewArt::thumbnail}) {
            std::wstring name = L"STUDIO_CREW_";
            for (const char c : owner) name += static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(c)));
            name += which == CrewArt::profile ? L"_PROFILE" : L"_THUMBNAIL";
            if (!studio_resource_bytes(name.c_str()).first) continue; // not sent yet
            if (auto pixels = decode_studio_resource(name.c_str(), which == CrewArt::profile ? 384 : 960))
                crew.push_back({crew_key(owner, which), std::move(*pixels)});
            else log_missing("a picture of THE CREW");
        }
    const int sheet_width = std::max({cell * 3, row_width, crew.empty() ? 0 : 4096});
    int crew_height = 0;
    {
        int cx = 0, cy = 0, line = 0;
        for (auto& c : crew) {
            if (cx > 0 && cx + c.pixels.width > sheet_width) cx = 0, cy += line + 8, line = 0;
            c.x = cx;
            c.y = cy;
            cx += c.pixels.width + 8;
            line = std::max(line, c.pixels.height);
        }
        if (!crew.empty()) crew_height = cy + line + 8;
    }
    Canvas sheet;
    sheet.width = sheet_width;
    sheet.height = cell + row_height + 8 + crew_height;
    sheet.rgba.assign(static_cast<std::size_t>(sheet.width) * sheet.height * 4, 0);
    std::array<ArtPlace, static_cast<std::size_t>(MarketArt::count)> places{};
    const float sw = static_cast<float>(sheet.width), sh = static_cast<float>(sheet.height);
    int x = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!pictures[i]) continue;
        auto& place = places[static_cast<std::size_t>(items[i].art)];
        if (items[i].tile) {
            const int tx = static_cast<int>(i) * cell;
            blit_tile(sheet, *pictures[i], tx, 0);
            place.uv0 = ImVec2((tx + tile_border) / sw, tile_border / sh);
            place.uv1 = ImVec2((tx + tile_border + tile_side) / sw, (tile_border + tile_side) / sh);
            place.width = place.height = static_cast<float>(tile_side);
            place.border = static_cast<float>(tile_border);
        } else {
            blit(sheet, *pictures[i], x, cell);
            place.uv0 = ImVec2(x / sw, cell / sh);
            place.uv1 = ImVec2((x + pictures[i]->width) / sw, (cell + pictures[i]->height) / sh);
            place.width = static_cast<float>(pictures[i]->width);
            place.height = static_cast<float>(pictures[i]->height);
            x += pictures[i]->width + 8;
        }
    }
    std::map<std::string, ArtPlace> crew_places;
    for (const auto& c : crew) {
        const int y = cell + row_height + 8 + c.y;
        blit(sheet, c.pixels, c.x, y);
        ArtPlace place;
        place.uv0 = ImVec2(c.x / sw, y / sh);
        place.uv1 = ImVec2((c.x + c.pixels.width) / sw, (y + c.pixels.height) / sh);
        place.width = static_cast<float>(c.pixels.width);
        place.height = static_cast<float>(c.pixels.height);
        crew_places[c.key] = place;
    }
    if (!wall.rgba.empty()) {
        auto& place = places[static_cast<std::size_t>(MarketArt::wallpaper)];
        place.uv0 = ImVec2(0, 0);
        place.uv1 = ImVec2(1, 1);
        place.width = static_cast<float>(wall.width);
        place.height = static_cast<float>(wall.height);
    }
    std::lock_guard lock(a.mutex);
    a.places = places;
    a.crew = std::move(crew_places);
    a.wall = std::move(wall);
    a.wall.serial = ++a.serial;
    a.wall_waiting = !a.wall.rgba.empty();
    a.sheet.rgba = std::move(sheet.rgba);
    a.sheet.width = static_cast<UINT>(sheet.width);
    a.sheet.height = static_cast<UINT>(sheet.height);
    a.sheet.serial = ++a.serial;
    a.sheet_waiting = true;
}
} // namespace

std::pair<const unsigned char*, std::size_t> studio_resource_bytes(const wchar_t* name) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&studio_resource_bytes), &module))
        return {};
    const auto found = FindResourceW(module, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto handle = found ? LoadResource(module, found) : nullptr;
    const auto* bytes = handle ? static_cast<const unsigned char*>(LockResource(handle)) : nullptr;
    const auto size = found ? SizeofResource(module, found) : 0;
    if (!bytes || !size) return {};
    return {bytes, static_cast<std::size_t>(size)};
}
std::optional<ArtPixels> decode_studio_resource(const wchar_t* resource, int most_wide) {
    const auto [bytes, size] = studio_resource_bytes(resource);
    if (!bytes) return std::nullopt;
    Com com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(size))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
        return std::nullopt;
    return decode_with(factory.Get(), decoder.Get(), most_wide);
}
std::optional<ArtPixels> decode_picture_file(const wchar_t* path, int most_wide) {
    Com com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return std::nullopt;
    return decode_with(factory.Get(), decoder.Get(), most_wide);
}

void want_market_art() noexcept {
    auto& a = art();
    {
        std::lock_guard lock(a.mutex);
        if (a.started) return;
        a.started = true;
    }
    try {
        std::thread(read_art).detach();
    } catch (...) {
        std::lock_guard lock(a.mutex);
        a.started = false;
    }
}
bool market_art(MarketArt which, ArtPlace& out) noexcept {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    out = a.places[static_cast<std::size_t>(which)];
    out.texture = which == MarketArt::wallpaper ? a.wall_texture : a.sheet_texture;
    return out.texture != ImTextureID{} && out.width > 0;
}
bool crew_art(std::string_view team, CrewArt which, ArtPlace& out) noexcept {
    try {
        auto& a = art();
        std::lock_guard lock(a.mutex);
        const auto found = a.crew.find(crew_key(team, which));
        if (found == a.crew.end() || !a.sheet_texture) return false;
        out = found->second;
        out.texture = a.sheet_texture;
        return out.width > 0;
    } catch (...) { return false; }
}
void show_cover_preview(std::vector<std::uint8_t> rgba, int side) {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    a.preview_kept = rgba;
    a.preview_side = side;
    a.preview.rgba = std::move(rgba);
    a.preview.width = a.preview.height = static_cast<UINT>(side);
    a.preview.serial = ++a.preview_serial;
    a.preview_waiting = true;
}
bool cover_preview(ImTextureID& texture) noexcept {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    texture = a.preview_texture;
    return texture != ImTextureID{};
}

namespace detail {
bool take_market_wall_pixels(CardPixels& out) {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    if (!a.wall_waiting) return false;
    a.wall_waiting = false;
    out = std::exchange(a.wall, {});
    return true;
}
void market_wall_uploaded(ImTextureID texture, unsigned) {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    a.wall_texture = texture;
}
bool take_market_sheet_pixels(CardPixels& out) {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    if (!a.sheet_waiting) return false;
    a.sheet_waiting = false;
    out = std::exchange(a.sheet, {});
    return true;
}
void market_sheet_uploaded(ImTextureID texture, unsigned) {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    a.sheet_texture = texture;
}
bool take_cover_preview_pixels(CardPixels& out) {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    if (!a.preview_waiting) return false;
    a.preview_waiting = false;
    out = std::exchange(a.preview, {});
    return true;
}
void cover_preview_uploaded(ImTextureID texture, unsigned serial) {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    if (serial == a.preview_serial) a.preview_texture = texture;
}
// The device went: the art is read again when it is next wanted, the preview handed out again.
void market_art_lost() {
    auto& a = art();
    std::lock_guard lock(a.mutex);
    a.wall_texture = a.sheet_texture = a.preview_texture = {};
    a.started = false;
    a.wall_waiting = a.sheet_waiting = false;
    if (!a.preview_kept.empty()) {
        a.preview.rgba = a.preview_kept;
        a.preview.width = a.preview.height = static_cast<UINT>(a.preview_side);
        a.preview.serial = ++a.preview_serial;
        a.preview_waiting = true;
    }
}
} // namespace detail
} // namespace dingosdk::overlay
