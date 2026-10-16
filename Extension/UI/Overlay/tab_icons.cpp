#include "tab_icons.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/item_thumbnails.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <span>

namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
constexpr int side = 96;
// The game's category icons and the bundle that holds them (with the rest of the core game assets).
constexpr std::string_view game_bundle = "win32/levels/game/bam_levelroot/bam_coregameassets";
constexpr std::array<std::string_view, 17> game_icons{
    "tops", "bottoms", "shoes", "outfit", "fullbody", "headwear", "headaccessories", "eyewear", "handwear", "socks",
    "deck", "trucks", "wheels", "gripcolor", "grippattern", "sticker", "decorator_vfxcosmetic"};

struct Icon {
    std::string name;
    std::vector<unsigned char> rgba; // side x side
};
struct Read {
    std::mutex mutex;
    std::future<std::vector<Icon>> pending;
    std::optional<std::vector<Icon>> decoded; // kept for every atlas the overlay builds
};
Read &read() {
    static auto *state = new Read;
    return *state;
}
struct Loaded {
    ImFontAtlas *atlas{};
    std::map<std::string, int, std::less<>> rects; // name -> atlas rect
    std::vector<std::string> order;
    bool filled{};
};
Loaded &loaded() {
    static Loaded state;
    return state;
}

std::span<const unsigned char> embedded(const wchar_t *name) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&embedded), &module))
        return {};
    const auto resource = FindResourceW(module, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto handle = resource ? LoadResource(module, resource) : nullptr;
    const auto *data = handle ? static_cast<const unsigned char *>(LockResource(handle)) : nullptr;
    return data ? std::span<const unsigned char>(data, SizeofResource(module, resource)) : std::span<const unsigned char>{};
}
bool decode(std::span<const unsigned char> bytes, std::vector<unsigned char> &pixels, UINT &width, UINT &height) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height ||
        width > 4096 || height > 4096)
        return false;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}

// ReSkate's own icons, built into ReSkate.dll: assets/tab_icons/tab_icons.json and tab_icons.png.
void read_own(std::vector<Icon> &icons) {
    const auto json_bytes = embedded(L"TABICONS_JSON"), png = embedded(L"TABICONS_PNG");
    if (json_bytes.empty() || png.empty()) return;
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    try {
        const auto json = Json::parse(std::string_view(reinterpret_cast<const char *>(json_bytes.data()), json_bytes.size()));
        std::vector<unsigned char> pixels;
        UINT width{}, height{};
        if (!json.contains("icons") || !decode(png, pixels, width, height)) throw std::runtime_error("the pack cannot be read");
        for (const auto &[name, entry] : json.at("icons").items()) {
            if (!entry.is_object() || !entry.contains("pos")) continue;
            const auto x = entry.at("pos").at(0).get<int>(), y = entry.at("pos").at(1).get<int>();
            if (x < 0 || y < 0 || static_cast<UINT>(x + side) > width || static_cast<UINT>(y + side) > height) continue;
            Icon icon{name, std::vector<unsigned char>(static_cast<std::size_t>(side) * side * 4)};
            for (int row = 0; row < side; ++row)
                std::memcpy(icon.rgba.data() + static_cast<std::size_t>(row) * side * 4,
                            pixels.data() + ((static_cast<std::size_t>(y) + row) * width + x) * 4, static_cast<std::size_t>(side) * 4);
            icons.push_back(std::move(icon));
        }
    } catch (const std::exception &e) {
        logging::log(logging::Level::warning, logging::Channel::ui, "Tab icons: ReSkate's own are unavailable: {}", e.what());
    }
    if (com) CoUninitialize();
}
// The game's category icons, by the names its UI gives them (the 128-pixel textures; the names without
// a size are not textures).
void read_game(const std::filesystem::path &root, std::vector<Icon> &icons) {
    std::vector<std::string> names;
    for (const auto name : game_icons) names.push_back("ui/textures/icons/cas/img_cas_" + std::string(name) + "_128_128_128");
    try {
        const auto read = vfs::read_ui_textures(root, game_bundle, names, side);
        // In the order of the list above, whatever order the bundle has them in.
        for (const auto name : game_icons)
            for (const auto &texture : read.thumbnails)
                if (texture.item == "ui/textures/icons/cas/img_cas_" + std::string(name) + "_128_128_128" &&
                    texture.image.rgba.size() == static_cast<std::size_t>(side) * side * 4)
                    icons.push_back({"game_" + std::string(name), texture.image.rgba});
    } catch (const std::exception &e) {
        logging::log(logging::Level::warning, logging::Channel::ui, "Tab icons: the game's are unavailable: {}", e.what());
    }
}
} // namespace

void start_tab_icons(std::filesystem::path game_root) {
    auto &state = read();
    std::lock_guard lock(state.mutex);
    if (state.pending.valid() || state.decoded) return;
    state.pending = std::async(std::launch::async, [root = std::move(game_root)] {
        std::vector<Icon> icons;
        try {
            read_game(root, icons);
            const auto game = icons.size();
            read_own(icons);
            logging::log(logging::Level::info, logging::Channel::ui, "Tab icons: {} of the game's, {} of ReSkate's.", game,
                         icons.size() - game);
        } catch (...) {}
        return icons;
    });
}

std::size_t reserve_tab_icons(ImFontAtlas &atlas, std::chrono::milliseconds wait) noexcept {
    try {
        auto &state = read();
        std::lock_guard lock(state.mutex);
        auto &l = loaded();
        l = {};
        if (!state.decoded) {
            if (!state.pending.valid() || state.pending.wait_for(wait) != std::future_status::ready) return 0;
            state.decoded = state.pending.get();
        }
        if (state.decoded->empty()) return 0;
        atlas.TexDesiredWidth = std::max(atlas.TexDesiredWidth, 2048);
        atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
        for (const auto &icon : *state.decoded) {
            if (l.rects.contains(icon.name)) continue;
            l.rects.emplace(icon.name, atlas.AddCustomRectRegular(side, side));
            l.order.push_back(icon.name);
        }
        l.atlas = &atlas;
        return l.rects.size();
    } catch (...) {
        loaded() = {};
        return 0;
    }
}

void fill_tab_icons(ImFontAtlas &atlas) noexcept {
    auto &l = loaded();
    auto &state = read();
    if (l.atlas != &atlas || l.rects.empty() || !state.decoded) return;
    try {
        unsigned char *pixels{};
        int width{}, height{};
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0) { l = {}; return; }
        for (const auto &icon : *state.decoded) {
            const auto *rect = atlas.GetCustomRectByIndex(l.rects.at(icon.name));
            if (!rect || !rect->IsPacked() || rect->X + side > width || rect->Y + side > height) { l = {}; return; }
            for (int y = 0; y < side; ++y)
                std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4,
                            icon.rgba.data() + static_cast<std::size_t>(y) * side * 4, static_cast<std::size_t>(side) * 4);
        }
        atlas.TexPixelsUseColors = true;
        l.filled = true;
    } catch (...) {
        l = {};
    }
}

bool tab_icon(std::string_view name, TabIcon &icon) {
    const auto &l = loaded();
    if (!l.filled || !l.atlas || !ImGui::GetCurrentContext() || ImGui::GetIO().Fonts != l.atlas || !l.atlas->TexID) return false;
    const auto found = l.rects.find(name);
    if (found == l.rects.end()) return false;
    const auto *rect = l.atlas->GetCustomRectByIndex(found->second);
    if (!rect || !rect->IsPacked()) return false;
    icon.texture = l.atlas->TexID;
    l.atlas->CalcCustomRectUV(rect, &icon.uv0, &icon.uv1);
    return true;
}

std::vector<std::string> tab_icon_names() {
    std::vector<std::string> out;
    if (!loaded().filled) return out;
    for (const auto& name : loaded().order)
        if (!name.starts_with("_")) out.push_back(name); // "_" ones are ReSkate's own marks, not tab icons
    return out;
}
} // namespace dingosdk::overlay
