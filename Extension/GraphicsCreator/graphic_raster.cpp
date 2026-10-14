#include "graphic_raster.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace dingosdk::graphics_creator {
namespace {
using Microsoft::WRL::ComPtr;

bool polygon(float x, float y, const std::vector<std::array<float, 2>>& points) {
    bool inside{};
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const auto& a = points[i]; const auto& b = points[j];
        if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
    }
    return inside;
}

bool contains(ShapeKind kind, float x, float y, float half_width, float half_height) {
    if (std::abs(x) > half_width || std::abs(y) > half_height) return false;
    if (kind == ShapeKind::rectangle || kind == ShapeKind::image) return true;
    if (kind == ShapeKind::ellipse) {
        const auto nx = x / half_width, ny = y / half_height;
        return nx * nx + ny * ny <= 1;
    }
    if (kind == ShapeKind::triangle)
        return polygon(x, y, {{0, -half_height}, {half_width, half_height}, {-half_width, half_height}});
    if (kind == ShapeKind::diamond)
        return std::abs(x) / half_width + std::abs(y) / half_height <= 1;
    std::vector<std::array<float, 2>> star;
    for (int i = 0; i < 10; ++i) {
        const float angle = -1.570796327f + static_cast<float>(i) * .628318531f;
        const float radius = i & 1 ? .42f : 1.f;
        star.push_back({std::cos(angle) * half_width * radius, std::sin(angle) * half_height * radius});
    }
    return polygon(x, y, star);
}

void blend(std::uint8_t* out, const std::array<std::uint8_t, 4>& source) {
    const unsigned sa = source[3], da = out[3], inverse = 255 - sa;
    const unsigned alpha = sa + (da * inverse + 127) / 255;
    if (!alpha) { std::fill_n(out, 4, std::uint8_t{}); return; }
    for (int channel = 0; channel < 3; ++channel) {
        const unsigned premultiplied = source[channel] * sa + (out[channel] * da * inverse + 127) / 255;
        out[channel] = static_cast<std::uint8_t>((premultiplied + alpha / 2) / alpha);
    }
    out[3] = static_cast<std::uint8_t>(alpha);
}
} // namespace

ImagePreview rasterize(const GraphicDocument& document, const std::filesystem::path& asset_directory,
                       std::uint32_t maximum_side) {
    validate(document);
    if (maximum_side < 64 || maximum_side > 4096) throw std::runtime_error("Graphic export size is invalid.");
    const double fitted = std::min(1.0, static_cast<double>(maximum_side) /
        static_cast<double>(std::max(document.canvas.width, document.canvas.height)));
    ImagePreview output{
        std::max(1U, static_cast<std::uint32_t>(document.canvas.width * fitted)),
        std::max(1U, static_cast<std::uint32_t>(document.canvas.height * fitted))};
    output.rgba.resize(static_cast<std::size_t>(output.width) * output.height * 4);

    for (const auto& layer : document.layers) {
        if (!layer.visible || layer.opacity <= 0) continue;
        ImagePreview image;
        if (layer.shape.kind == ShapeKind::image) image = load_image(asset_directory, layer.shape.source);
        const float scale = static_cast<float>(fitted);
        const float centre_x = layer.transform.x * scale, centre_y = layer.transform.y * scale;
        const float width = layer.shape.width * layer.transform.scale_x * scale;
        const float height = layer.shape.height * layer.transform.scale_y * scale;
        const float half_width = width * .5f, half_height = height * .5f;
        const float angle = layer.transform.rotation_degrees * .01745329252f;
        const float cosine = std::cos(angle), sine = std::sin(angle);
        const float extent_x = std::abs(cosine) * half_width + std::abs(sine) * half_height;
        const float extent_y = std::abs(sine) * half_width + std::abs(cosine) * half_height;
        const int x0 = std::max(0, static_cast<int>(std::floor(centre_x - extent_x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(centre_y - extent_y)));
        const int x1 = std::min(static_cast<int>(output.width), static_cast<int>(std::ceil(centre_x + extent_x)));
        const int y1 = std::min(static_cast<int>(output.height), static_cast<int>(std::ceil(centre_y + extent_y)));
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
            const float dx = x + .5f - centre_x, dy = y + .5f - centre_y;
            const float local_x = dx * cosine + dy * sine, local_y = -dx * sine + dy * cosine;
            if (!contains(layer.shape.kind, local_x, local_y, half_width, half_height)) continue;
            std::array<std::uint8_t, 4> color{layer.shape.fill.red, layer.shape.fill.green,
                                              layer.shape.fill.blue, layer.shape.fill.alpha};
            if (layer.shape.kind == ShapeKind::image) {
                const auto u = std::clamp(local_x / width + .5f, 0.f, .999999f);
                const auto v = std::clamp(local_y / height + .5f, 0.f, .999999f);
                const auto sx = static_cast<std::uint32_t>(u * image.width);
                const auto sy = static_cast<std::uint32_t>(v * image.height);
                const auto at = (static_cast<std::size_t>(sy) * image.width + sx) * 4;
                for (int channel = 0; channel < 3; ++channel)
                    color[channel] = static_cast<std::uint8_t>(image.rgba[at + channel] * color[channel] / 255);
                color[3] = static_cast<std::uint8_t>(image.rgba[at + 3] * color[3] / 255);
            }
            color[3] = static_cast<std::uint8_t>(color[3] * layer.opacity);
            blend(output.rgba.data() + (static_cast<std::size_t>(y) * output.width + x) * 4, color);
        }
    }
    return output;
}

void export_png(const std::filesystem::path& destination, const ImagePreview& image) {
    if (!image.width || !image.height || image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4)
        throw std::runtime_error("The rendered graphic is invalid.");
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    std::filesystem::create_directories(destination.parent_path());
    auto temporary = destination; temporary += L".tmp";
    std::filesystem::remove(temporary);
    auto bgra = image.rgba;
    for (std::size_t pixel = 0; pixel < bgra.size(); pixel += 4)
        std::swap(bgra[pixel], bgra[pixel + 2]);
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
    if (SUCCEEDED(result)) result = frame->SetSize(image.width, image.height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
    if (SUCCEEDED(result) && format != GUID_WICPixelFormat32bppBGRA) result = E_FAIL;
    if (SUCCEEDED(result)) result = frame->WritePixels(image.height, image.width * 4,
        static_cast<UINT>(bgra.size()), bgra.data());
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    stream.Reset(); encoder.Reset(); frame.Reset(); factory.Reset();
    if (com == S_OK || com == S_FALSE) CoUninitialize();
    if (FAILED(result) || !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("The target PNG could not be exported.");
    }
}

} // namespace dingosdk::graphics_creator
