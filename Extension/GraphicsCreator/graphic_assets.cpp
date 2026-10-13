#include "graphic_assets.h"
#include "graphic_document.h"
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

} // namespace dingosdk::graphics_creator
