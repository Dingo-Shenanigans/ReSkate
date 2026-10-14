#include "graphic_assets.h"
#include "graphic_document.h"
#include "graphic_raster.h"
#include <Windows.h>
#include <wincodec.h>
#include <objbase.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <format>
#include <stdexcept>

namespace dingosdk::graphics_creator {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::uintmax_t maximum_image_bytes = 16 * 1024 * 1024;

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (result == S_OK || result == S_FALSE) CoUninitialize(); }
};

struct Decoded {
    std::uint32_t width{}, height{};
    GUID container{};
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICImagingFactory> factory;
};

Decoded inspect(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        throw std::runtime_error("Choose a valid PNG or JPEG image.");
    UINT width{}, height{};
    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) || FAILED(frame->GetSize(&width, &height)) ||
        !width || !height || width > 4096 || height > 4096)
        throw std::runtime_error("Imported images must be between 1 and 4096 pixels on each side.");
    return {width, height, container, std::move(frame), std::move(factory)};
}

std::string extension(const std::filesystem::path& path) {
    auto result = path.extension().string();
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (result != ".png" && result != ".jpg" && result != ".jpeg")
        throw std::runtime_error("Only PNG and JPEG images can be imported.");
    return result == ".jpeg" ? ".jpg" : result;
}

std::uint64_t hash_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("The selected image could not be opened.");
    std::uint64_t hash = 1469598103934665603ULL;
    std::array<char, 16384> bytes{};
    while (stream) {
        stream.read(bytes.data(), bytes.size());
        for (std::streamsize i = 0; i < stream.gcount(); ++i)
            hash = (hash ^ static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)])) * 1099511628211ULL;
    }
    return hash;
}
} // namespace

ImportedImage import_image(const std::filesystem::path& asset_directory,
                           const std::filesystem::path& source) {
    ComApartment apartment;
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error))
        throw std::runtime_error("The selected image is not a readable file.");
    const auto bytes = std::filesystem::file_size(source, error);
    if (error || !bytes || bytes > maximum_image_bytes)
        throw std::runtime_error("Imported images must be smaller than 16 MB.");
    const auto suffix = extension(source);
    const auto decoded = inspect(source);
    if ((suffix == ".png" && decoded.container != GUID_ContainerFormatPng) ||
        (suffix == ".jpg" && decoded.container != GUID_ContainerFormatJpeg))
        throw std::runtime_error("The image contents do not match its PNG or JPEG file extension.");
    const auto name = std::format("asset-{:016x}{}", hash_file(source), suffix);
    std::filesystem::create_directories(asset_directory);
    const auto destination = asset_directory / name;
    if (!std::filesystem::exists(destination, error)) {
        auto temporary = destination; temporary += L".tmp";
        std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing, error);
        if (error || !MoveFileExW(temporary.c_str(), destination.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("The image could not be copied into ReSkate.Create.");
        }
    }
    return {name, decoded.width, decoded.height};
}

ImagePreview load_preview(const std::filesystem::path& asset_directory,
                          const std::string& asset_name, std::uint32_t maximum_side) {
    ComApartment apartment;
    if (!valid_asset_name(asset_name) || maximum_side < 4 || maximum_side > 128)
        throw std::runtime_error("The graphic references an invalid local image.");
    const auto decoded = inspect(asset_directory / asset_name);
    const double ratio = std::min(1.0, static_cast<double>(maximum_side) /
        static_cast<double>(std::max(decoded.width, decoded.height)));
    const UINT width = std::max(1U, static_cast<UINT>(decoded.width * ratio));
    const UINT height = std::max(1U, static_cast<UINT>(decoded.height * ratio));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(decoded.factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(decoded.frame.Get(), width, height, WICBitmapInterpolationModeFant)) ||
        FAILED(decoded.factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        throw std::runtime_error("The imported image preview could not be decoded.");
    ImagePreview preview{width, height, std::vector<std::uint8_t>(width * height * 4)};
    if (FAILED(converter->CopyPixels(nullptr, width * 4,
        static_cast<UINT>(preview.rgba.size()), preview.rgba.data())))
        throw std::runtime_error("The imported image preview could not be read.");
    return preview;
}

ImagePreview load_image(const std::filesystem::path& asset_directory,
                        const std::string& asset_name) {
    ComApartment apartment;
    if (!valid_asset_name(asset_name))
        throw std::runtime_error("The graphic references an invalid local image.");
    const auto decoded = inspect(asset_directory / asset_name);
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(decoded.factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(decoded.frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        throw std::runtime_error("The imported image could not be decoded.");
    ImagePreview image{decoded.width, decoded.height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(decoded.width) * decoded.height * 4)};
    if (FAILED(converter->CopyPixels(nullptr, decoded.width * 4,
        static_cast<UINT>(image.rgba.size()), image.rgba.data())))
        throw std::runtime_error("The imported image could not be read.");
    return image;
}

ImportedImage create_text_image(const std::filesystem::path& asset_directory,
                                std::string_view text, std::wstring_view font_face) {
    if (text.empty() || text.size() > 96) throw std::runtime_error("Text must be between 1 and 96 characters.");
    const int wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                               static_cast<int>(text.size()), nullptr, 0);
    if (wide_size <= 0) throw std::runtime_error("The text contains unsupported characters.");
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        wide.data(), wide_size);

    const auto private_font = asset_directory.parent_path() / L"Fonts" / L"MTV2C___.TTF";
    const bool private_font_loaded = font_face == L"MTV2C" && std::filesystem::exists(private_font) &&
        AddFontResourceExW(private_font.c_str(), FR_PRIVATE, nullptr) > 0;
    if (font_face == L"MTV2C" && !private_font_loaded) font_face = L"Impact";

    constexpr int width = 1536, height = 384;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits{};
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HFONT font = CreateFontW(270, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            VARIABLE_PITCH, std::wstring(font_face).c_str());
    if (!dc || !bitmap || !font || !bits) {
        if (font) DeleteObject(font);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        throw std::runtime_error("The text layer could not be rendered.");
    }
    const auto old_bitmap = SelectObject(dc, bitmap);
    const auto old_font = SelectObject(dc, font);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT bounds{20, 20, width - 20, height - 20};
    DrawTextW(dc, wide.c_str(), static_cast<int>(wide.size()), &bounds,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    ImagePreview image{width, height, std::vector<std::uint8_t>(width * height * 4)};
    const auto *source = static_cast<const std::uint8_t *>(bits);
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel) {
        const auto alpha = std::max({source[pixel * 4], source[pixel * 4 + 1], source[pixel * 4 + 2]});
        image.rgba[pixel * 4] = image.rgba[pixel * 4 + 1] = image.rgba[pixel * 4 + 2] = 255;
        image.rgba[pixel * 4 + 3] = alpha;
    }
    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    if (private_font_loaded) RemoveFontResourceExW(private_font.c_str(), FR_PRIVATE, nullptr);

    std::uint64_t value = 1469598103934665603ULL;
    for (const unsigned char c : text) value = (value ^ c) * 1099511628211ULL;
    for (const wchar_t c : font_face) value = (value ^ static_cast<std::uint16_t>(c)) * 1099511628211ULL;
    const auto name = std::format("text-{:016x}.png", value);
    export_png(asset_directory / name, image);
    return {name, width, height};
}

ImportedImage import_embedded_image(const std::filesystem::path& asset_directory,
                                    const wchar_t* resource_name) {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&import_embedded_image), &module);
    const auto resource = module ? FindResourceW(module, resource_name, RT_RCDATA) : nullptr;
    const auto loaded = resource ? LoadResource(module, resource) : nullptr;
    const auto *bytes = loaded ? static_cast<const char *>(LockResource(loaded)) : nullptr;
    const auto size = resource ? SizeofResource(module, resource) : 0;
    if (!bytes || !size) throw std::runtime_error("The built-in logo is unavailable.");
    std::filesystem::create_directories(asset_directory);
    const auto temporary = asset_directory / L"embedded-logo.tmp.png";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(bytes, size);
        if (!output) throw std::runtime_error("The built-in logo could not be prepared.");
    }
    try {
        auto imported = import_image(asset_directory, temporary);
        std::filesystem::remove(temporary);
        auto image = load_image(asset_directory, imported.asset_name);
        for (std::size_t pixel = 0; pixel < image.rgba.size(); pixel += 4)
            if (image.rgba[pixel] < 12 && image.rgba[pixel + 1] < 12 && image.rgba[pixel + 2] < 12)
                image.rgba[pixel + 3] = 0;
        export_png(asset_directory / imported.asset_name, image);
        return imported;
    } catch (...) {
        std::filesystem::remove(temporary);
        throw;
    }
}

} // namespace dingosdk::graphics_creator
