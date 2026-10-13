#include "ui_backgrounds.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <vector>

namespace dingosdk::overlay {
namespace {

using Microsoft::WRL::ComPtr;

struct Background {
    int rect = -1;
    unsigned width{}, height{};
    std::vector<unsigned char> pixels;
};

struct Backgrounds {
    ImFontAtlas* atlas{};
    std::array<Background, 4> images;
};

Backgrounds& state() {
    static Backgrounds value;
    return value;
}

HMODULE module() noexcept {
    HMODULE value{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&reserve_ui_backgrounds), &value);
    return value;
}

bool decode_resource(const wchar_t* name, Background& out) {
    const auto instance = module();
    const auto resource = instance ? FindResourceW(instance, name, RT_RCDATA) : nullptr;
    const auto loaded = resource ? LoadResource(instance, resource) : nullptr;
    const auto bytes = loaded ? static_cast<unsigned char*>(LockResource(loaded)) : nullptr;
    const DWORD byte_count = resource ? SizeofResource(instance, resource) : 0;
    if (!bytes || !byte_count) return false;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width{}, height{};
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(bytes, byte_count)) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(frame->GetSize(&width, &height)) || !width || !height ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
            nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;

    out.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.pixels.size()), out.pixels.data()))) {
        out.pixels.clear();
        return false;
    }
    out.width = width;
    out.height = height;
    return true;
}

} // namespace

void clear_ui_backgrounds() noexcept { state() = {}; }

std::size_t reserve_ui_backgrounds(ImFontAtlas& atlas) noexcept {
    clear_ui_backgrounds();
    try {
        auto& backgrounds = state();
        static constexpr std::array<const wchar_t*, 4> names{L"HUB_BG_1", L"HUB_BG_2", L"HUB_BG_3", L"HUB_BG_4"};
        std::size_t loaded{};
        for (std::size_t i = 0; i < names.size(); ++i) {
            auto& image = backgrounds.images[i];
            if (!decode_resource(names[i], image)) continue;
            image.rect = atlas.AddCustomRectRegular(static_cast<int>(image.width), static_cast<int>(image.height));
            ++loaded;
        }
        backgrounds.atlas = &atlas;
        return loaded;
    } catch (...) {
        clear_ui_backgrounds();
        return 0;
    }
}

bool fill_ui_backgrounds(ImFontAtlas& atlas) noexcept {
    auto& backgrounds = state();
    if (backgrounds.atlas != &atlas) return false;
    unsigned char* pixels{};
    int atlas_width{}, atlas_height{};
    atlas.GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
    if (!pixels || atlas_width <= 0 || atlas_height <= 0) return false;

    bool any{};
    for (const auto& image : backgrounds.images) {
        if (image.rect < 0 || image.pixels.empty()) continue;
        const auto* rect = atlas.GetCustomRectByIndex(image.rect);
        if (!rect || !rect->IsPacked() || rect->X + image.width > static_cast<unsigned>(atlas_width) ||
            rect->Y + image.height > static_cast<unsigned>(atlas_height))
            continue;
        for (unsigned y = 0; y < image.height; ++y)
            std::memcpy(pixels + ((rect->Y + y) * static_cast<std::size_t>(atlas_width) + rect->X) * 4,
                image.pixels.data() + static_cast<std::size_t>(y) * image.width * 4,
                static_cast<std::size_t>(image.width) * 4);
        any = true;
    }
    atlas.TexPixelsUseColors = true;
    return any;
}

bool ui_background_image(unsigned index, UiBackgroundImage& image) noexcept {
    auto& backgrounds = state();
    if (!index || index > backgrounds.images.size() || !backgrounds.atlas || !ImGui::GetCurrentContext() ||
        ImGui::GetIO().Fonts != backgrounds.atlas || !backgrounds.atlas->TexID)
        return false;
    const auto& source = backgrounds.images[index - 1];
    if (source.rect < 0) return false;
    const auto* rect = backgrounds.atlas->GetCustomRectByIndex(source.rect);
    if (!rect || !rect->IsPacked()) return false;
    image.texture = backgrounds.atlas->TexID;
    backgrounds.atlas->CalcCustomRectUV(rect, &image.uv0, &image.uv1);
    return true;
}

} // namespace dingosdk::overlay
