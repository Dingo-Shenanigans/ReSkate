#include "overlay_internal.h"
#include "Extension/AnimationStudio/anim_studio.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The Animator Studio's pack pictures (PACKS): each pack's presets/<pack>.png, read off the render thread and put
// in one texture, a square each, in the SRV heap's fifth slot (overlay_render.cpp, upload_studio_pack_image), as the
// menu's map pictures are. And the other way: a picture chosen for a pack is cut to a square from its middle, made
// anim_studio::thumbnail_side pixels a side and saved as a PNG with the pack (set_pack_thumbnail), so a pack never
// carries a photo's worth of bytes around.
namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT cell = 512, columns = 8; // 512: the MARKET shows covers big, and they stay sharp on a 1440p screen
constexpr std::size_t most = 24;

struct Pictures {
    std::mutex mutex;
    std::string asked;   // the pictures last asked for (key and file, a line each)
    unsigned asking{};   // goes up with every new ask: a read that finishes after a newer ask is dropped
    detail::CardPixels current, waiting_pixels;
    std::map<std::string, UINT> waiting_cells, ready_cells;
    UINT width{}, height{}, ready_width{}, ready_height{};
    bool waiting{};
    unsigned serial{};
    ImTextureID texture{};
    bool making{}; // a picture is being made for a pack
};
Pictures& pictures() {
    static auto* value = new Pictures; // process lifetime
    return *value;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(in.tellg());
    if (size <= 0 || size > 4 * 1024 * 1024) return {};
    in.seekg(0);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), size);
    return in ? bytes : std::string{};
}
std::filesystem::path from_utf8(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

// The pictures, read and put together into one texture.
void read_pictures(std::vector<std::pair<std::string, std::string>> wanted, unsigned asking) {
    try {
        std::vector<std::pair<std::string, detail::CardPixels>> squares;
        for (const auto& [key, file] : wanted) {
            if (squares.size() >= most) break;
            detail::CardPixels one;
            const auto bytes = read_file(from_utf8(file));
            if (!bytes.empty() && detail::decode_picture(bytes, one, cell) && one.width == cell && one.height == cell)
                squares.emplace_back(key, std::move(one));
        }
        auto& p = pictures();
        std::lock_guard lock(p.mutex);
        if (asking != p.asking) return; // asked again since: that read has it
        if (squares.empty()) {
            p.texture = {};
            p.ready_cells.clear();
            p.current = {};
            return;
        }
        const auto count = static_cast<UINT>(squares.size());
        detail::CardPixels atlas;
        atlas.width = cell * std::min(count, columns);
        atlas.height = cell * ((count + columns - 1) / columns);
        atlas.rgba.assign(std::size_t{atlas.width} * atlas.height * 4, 0);
        std::map<std::string, UINT> cells;
        for (UINT i = 0; i < count; ++i) {
            const UINT x = (i % columns) * cell, y = (i / columns) * cell;
            for (UINT row = 0; row < cell; ++row)
                std::memcpy(atlas.rgba.data() + (std::size_t{y + row} * atlas.width + x) * 4,
                            squares[i].second.rgba.data() + std::size_t{row} * cell * 4, std::size_t{cell} * 4);
            cells[squares[i].first] = i;
        }
        atlas.serial = ++p.serial;
        p.width = atlas.width;
        p.height = atlas.height;
        p.current = atlas;
        p.waiting_pixels = std::move(atlas);
        p.waiting_cells = std::move(cells);
        p.waiting = true;
    } catch (...) { /* No pictures: the packs show their drawn covers. */ }
}

// Any picture Windows can read, cut to a square from its middle, scaled to `side` and written as a PNG.
std::string square_png(const std::filesystem::path& file, UINT side, std::string& why) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string out;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapClipper> clipper;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame_out;
        ComPtr<IPropertyBag2> options;
        UINT width{}, height{};
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height) {
            why = "That file is not a picture the studio can read (PNG, JPG, BMP, GIF or TIFF work).";
        } else {
            const UINT square = std::min(width, height);
            const WICRect middle{static_cast<INT>((width - square) / 2), static_cast<INT>((height - square) / 2), static_cast<INT>(square),
                                 static_cast<INT>(square)};
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            HGLOBAL memory{};
            if (SUCCEEDED(factory->CreateBitmapClipper(&clipper)) && SUCCEEDED(clipper->Initialize(frame.Get(), &middle)) &&
                SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(clipper.Get(), side, side, WICBitmapInterpolationModeHighQualityCubic)) &&
                SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                                WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
                SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                SUCCEEDED(encoder->CreateNewFrame(&frame_out, &options)) && SUCCEEDED(frame_out->Initialize(options.Get())) &&
                SUCCEEDED(frame_out->SetSize(side, side)) && SUCCEEDED(frame_out->SetPixelFormat(&format)) &&
                format == GUID_WICPixelFormat32bppBGRA && SUCCEEDED(frame_out->WriteSource(converter.Get(), nullptr)) &&
                SUCCEEDED(frame_out->Commit()) && SUCCEEDED(encoder->Commit()) && SUCCEEDED(GetHGlobalFromStream(stream.Get(), &memory))) {
                STATSTG stat{};
                if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0 && stat.cbSize.QuadPart < 8ull * 1024 * 1024)
                    if (const void* bytes = GlobalLock(memory)) {
                        out.assign(static_cast<const char*>(bytes), static_cast<std::size_t>(stat.cbSize.QuadPart));
                        GlobalUnlock(memory);
                    }
            }
            if (out.empty()) why = "That picture could not be made into a pack picture.";
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return out;
}
}

namespace detail {
void want_studio_pack_pictures(const std::vector<std::pair<std::string, std::string>>& wanted) {
    std::string key;
    for (const auto& [name, file] : wanted) key += name + '\t' + file + '\n';
    auto& p = pictures();
    unsigned asking{};
    {
        std::lock_guard lock(p.mutex);
        if (key == p.asked) return;
        p.asked = std::move(key);
        asking = ++p.asking;
    }
    try {
        std::thread(read_pictures, wanted, asking).detach();
    } catch (...) {}
}
bool studio_pack_picture(const std::string& key, ImTextureID& texture, ImVec2& uv0, ImVec2& uv1) {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    if (!p.texture || !p.ready_width || !p.ready_height) return false;
    const auto found = p.ready_cells.find(key);
    if (found == p.ready_cells.end()) return false;
    const float x = static_cast<float>((found->second % columns) * cell), y = static_cast<float>((found->second / columns) * cell);
    texture = p.texture;
    uv0 = ImVec2(x / static_cast<float>(p.ready_width), y / static_cast<float>(p.ready_height));
    uv1 = ImVec2((x + cell) / static_cast<float>(p.ready_width), (y + cell) / static_cast<float>(p.ready_height));
    return true;
}
void make_studio_pack_picture(const std::string& pack, const std::string& file) {
    {
        auto& p = pictures();
        std::lock_guard lock(p.mutex);
        if (p.making) return;
        p.making = true;
    }
    try {
        std::thread([pack, file] {
            std::string why, message;
            const auto png = square_png(from_utf8(file), anim_studio::thumbnail_side, why);
            if (png.empty()) anim_studio::report(why);
            else anim_studio::set_pack_thumbnail(pack, png, message);
            auto& p = pictures();
            std::lock_guard lock(p.mutex);
            p.making = false;
        }).detach();
    } catch (...) {
        auto& p = pictures();
        std::lock_guard lock(p.mutex);
        p.making = false;
    }
}
bool studio_pack_picture_making() {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    return p.making;
}
bool take_studio_pack_pixels(CardPixels& out) {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    if (!p.waiting) return false;
    p.waiting = false;
    out = std::exchange(p.waiting_pixels, {});
    return true;
}
void studio_pack_pixels_uploaded(ImTextureID texture, unsigned serial) {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    if (serial != p.serial) return;
    p.texture = texture;
    p.ready_cells = p.waiting_cells;
    p.ready_width = p.width;
    p.ready_height = p.height;
}
void studio_pack_pixels_lost() {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    p.texture = {};
    if (!p.current.rgba.empty()) {
        p.waiting_pixels = p.current;
        p.waiting = true;
    }
}
} // namespace detail
} // namespace dingosdk::overlay
