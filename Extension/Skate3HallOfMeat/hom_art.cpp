#include "hom_art.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/mod_list.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <Windows.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

struct Picture {
    int width{}, height{};
    std::vector<unsigned char> rgba;
    int rect{-1};
};
struct Loaded {
    std::map<std::string, Picture, std::less<>> pictures;
    ImFontAtlas *atlas{};
    bool filled{};
    std::map<int, ImFont *> fonts;
};
Loaded &loaded() {
    static Loaded value;
    return value;
}

bool decode(const fs::path &path, Picture &picture) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width{}, height{};
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 1024 ||
        height > 1024)
        return false;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    picture.width = static_cast<int>(width);
    picture.height = static_cast<int>(height);
    picture.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(picture.rgba.size()), picture.rgba.data()));
}

fs::path game_directory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return fs::path(path).parent_path();
}
// The Mods folder's Hall of Meat assets, read now (slow: every mod's description).
fs::path scan_hom_directory() noexcept;
} // namespace

fs::path hom_directory() noexcept {
    // Asked every game tick and every frame: answered from the last scan, and the Mods folder is
    // scanned again every 2 s on a thread of its own, never on the caller's.
    static std::mutex mutex;
    static fs::path found;
    static std::chrono::steady_clock::time_point next{};
    static std::atomic<bool> scanning{};
    {
        std::lock_guard lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now < next || scanning.exchange(true)) return found;
        next = now + std::chrono::seconds(2);
    }
    try {
        std::thread([] {
            const auto result = scan_hom_directory();
            {
                std::lock_guard lock(mutex);
                if (result != found)
                    logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: {}.",
                                 result.empty() ? std::string("assets not installed, off") : "assets in " + mods::ascii_path(result));
                found = result;
            }
            scanning.store(false);
        }).detach();
    } catch (...) {
        scanning.store(false);
    }
    std::lock_guard lock(mutex);
    return found;
}

namespace {
fs::path scan_hom_directory() noexcept {
    fs::path result;
    try {
        const auto game = game_directory();
        std::error_code error;
        const auto list = mods::scan_mods(game);
        if (list.issue.empty())
            for (const auto &entry : list.entries) {
                if (!entry.enabled || !entry.mod.outdated.empty()) continue;
                const auto candidate = entry.mod.directory / L"HallOfMeat";
                if (fs::is_regular_file(candidate / L"bones.bin", error)) {
                    result = candidate;
                    break;
                }
            }
        // A mod installed by hand into a disabled slot does not count; the game folder copy is for development.
        if (result.empty() && !game.empty() && fs::is_regular_file(game / L"HallOfMeat" / L"bones.bin", error)) result = game / L"HallOfMeat";
    } catch (...) {
        result.clear();
    }
    return result;
}
} // namespace

std::size_t reserve_hom_art(ImFontAtlas &atlas) noexcept {
    try {
        auto &l = loaded();
        l = {};
        const auto root = hom_directory();
        if (root.empty()) return 0;
        const auto font_path = root / L"fonts" / L"FuturaStd-Heavy.ttf";
        std::error_code font_error;
        if (fs::is_regular_file(font_path, font_error)) {
            static const ImWchar ranges[]{0x0020, 0x00FF, 0};
            for (const int size : {22, 28, 60}) {
                ImFontConfig config;
                config.OversampleH = 2;
                config.PixelSnapH = false;
                if (auto *font = atlas.AddFontFromFileTTF(font_path.string().c_str(), static_cast<float>(size), &config, ranges))
                    l.fonts[size] = font;
            }
        }
        const auto directory = root / L"art";
        std::error_code error;
        if (directory.empty() || !fs::is_directory(directory, error)) return 0;
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (const auto &entry : fs::directory_iterator(directory, error)) {
            if (!entry.is_regular_file() || entry.path().extension() != L".png") continue;
            Picture picture;
            if (!decode(entry.path(), picture)) continue;
            const auto name = entry.path().stem().string();
            picture.rect = atlas.AddCustomRectRegular(picture.width, picture.height);
            l.pictures.emplace(name, std::move(picture));
        }
        if (SUCCEEDED(hr)) CoUninitialize();
        {
            // Film grain for the X-ray (Skate 3 scrolls a noise texture over the picture). Each texel is
            // a dark or a light speck whose strength is in its alpha, so the grain roughly averages out
            // instead of lifting the whole picture toward grey (light specks are rarer and fainter:
            // over a dark picture they show more).
            Picture grain;
            grain.width = grain.height = 128;
            grain.rgba.resize(128 * 128 * 4);
            std::uint32_t seed = 0x9E3779B9u;
            for (std::size_t i = 0; i < 128 * 128; ++i) {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                const unsigned noise = seed & 0xff;
                const bool light = ((seed >> 8) & 0xff) < 90;
                const unsigned strength = light ? noise * 2 / 5 : noise;
                grain.rgba[i * 4] = grain.rgba[i * 4 + 1] = grain.rgba[i * 4 + 2] = light ? 230 : 0;
                grain.rgba[i * 4 + 3] = static_cast<unsigned char>(strength * strength / 255);
            }
            grain.rect = atlas.AddCustomRectRegular(grain.width, grain.height);
            l.pictures.emplace("grain", std::move(grain));
        }
        l.atlas = &atlas;
        atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
        logging::log(logging::Level::info, logging::Channel::graphics, "Hall Of Meat: {} pictures reserved.", l.pictures.size());
        return l.pictures.size();
    } catch (...) {
        loaded() = {};
        return 0;
    }
}

void fill_hom_art(ImFontAtlas &atlas) noexcept {
    auto &l = loaded();
    if (l.atlas != &atlas || l.pictures.empty()) return;
    try {
        unsigned char *pixels{};
        int width{}, height{};
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0) { l = {}; return; }
        for (auto &[name, picture] : l.pictures) {
            const auto *rect = atlas.GetCustomRectByIndex(picture.rect);
            if (!rect || !rect->IsPacked() || rect->X + picture.width > width || rect->Y + picture.height > height) {
                picture.rect = -1;
                continue;
            }
            for (int y = 0; y < picture.height; ++y)
                std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4,
                            picture.rgba.data() + static_cast<std::size_t>(y) * picture.width * 4,
                            static_cast<std::size_t>(picture.width) * 4);
        }
        atlas.TexPixelsUseColors = true;
        l.filled = true;
    } catch (...) {
        l = {};
    }
}

ImFont *hom_font(int size) noexcept {
    const auto &l = loaded();
    const auto it = l.fonts.find(size);
    return it == l.fonts.end() ? nullptr : it->second;
}

bool hom_art(std::string_view name, HomArt &art) noexcept {
    const auto &l = loaded();
    if (!l.filled || !l.atlas || !ImGui::GetCurrentContext() || ImGui::GetIO().Fonts != l.atlas || !l.atlas->TexID) return false;
    const auto it = l.pictures.find(name);
    if (it == l.pictures.end() || it->second.rect < 0) return false;
    const auto *rect = l.atlas->GetCustomRectByIndex(it->second.rect);
    if (!rect || !rect->IsPacked()) return false;
    art.texture = l.atlas->TexID;
    l.atlas->CalcCustomRectUV(rect, &art.uv0, &art.uv1);
    art.width = static_cast<float>(it->second.width);
    art.height = static_cast<float>(it->second.height);
    return true;
}
} // namespace dingosdk::overlay
