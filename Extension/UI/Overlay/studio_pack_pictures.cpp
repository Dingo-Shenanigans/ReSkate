#include "overlay_internal.h"
#include "Extension/AnimationStudio/anim_studio.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The Animator Studio's pack pictures (PACKS, and the MARKET's covers): each read off the render thread and put in
// one texture, a square each, in the SRV heap's fifth slot (overlay_render.cpp, upload_studio_pack_image). A picture
// keeps its square while it is there, so scrolling and swiping only read and copy in the pictures that are new; a square
// is given to a new picture only when its own is not asked for, and filled two frames after nothing draws it any more.
// And the other way: a picture chosen for a pack is cut to a square from its middle, made anim_studio::thumbnail_side
// pixels a side and saved as a PNG with the pack (set_pack_thumbnail), so a pack never carries a photo's worth of bytes
// around.
namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT cell = detail::studio_pack_cell, columns = detail::studio_pack_columns; // 512: covers stay sharp on a 1440p screen
constexpr std::size_t most = std::size_t{detail::studio_pack_columns} * detail::studio_pack_rows;

struct Pictures {
    std::mutex mutex;
    std::string asked;                    // the pictures last asked for (key and file, a line each)
    std::set<std::string> wanted;         // those pictures, by id (key, tab, file)
    std::set<std::string> reading;        // ids being read now
    std::map<std::string, std::chrono::steady_clock::time_point> unreadable; // ids that could not be read, and when: tried again after a while
    std::array<std::string, most> held{}; // the id each square holds, or is about to
    std::array<std::uint64_t, most> used{}; // when each square's picture was last asked for: the oldest goes first
    std::uint64_t ask_count{};
    std::map<std::string, UINT> ready;       // key -> its square, drawable
    std::map<UINT, detail::PackCell> waiting; // square -> the picture to copy into it
    std::uint64_t frame{};                   // render frames seen (take_studio_pack_cells)
    ImTextureID texture{};
    bool making{}; // a picture is being made for a pack
};
Pictures& pictures() {
    static auto* value = new Pictures; // process lifetime
    return *value;
}
// Nothing draws square `c`'s picture from now on.
void forget_square(Pictures& p, UINT c) {
    for (auto it = p.ready.begin(); it != p.ready.end();)
        if (it->second == c) it = p.ready.erase(it);
        else ++it;
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

// New pictures, read one by one and each given a square: an empty one, else the one whose picture was asked for
// longest ago and is not asked for now. Its old picture stops being drawn at once; the new one is copied in two render
// frames on, when no frame still being drawn can have read the old one (take_studio_pack_cells).
void read_pictures(std::vector<std::pair<std::string, std::string>> fresh) {
    for (const auto& [key, file] : fresh) {
        detail::CardPixels one;
        bool read = false;
        try {
            const auto bytes = read_file(from_utf8(file));
            read = !bytes.empty() && detail::decode_picture(bytes, one, cell) && one.width == cell && one.height == cell &&
                   one.rgba.size() == std::size_t{cell} * cell * 4;
        } catch (...) {}
        const auto id = key + '\t' + file;
        auto& p = pictures();
        std::lock_guard lock(p.mutex);
        p.reading.erase(id);
        if (!read) {
            p.unreadable[id] = std::chrono::steady_clock::now();
            continue;
        }
        if (std::find(p.held.begin(), p.held.end(), id) != p.held.end()) continue; // read twice: it has its square
        int chosen = -1;
        for (std::size_t i = 0; i < most && chosen < 0; ++i)
            if (p.held[i].empty()) chosen = static_cast<int>(i);
        if (chosen < 0 && p.wanted.count(id))
            for (std::size_t i = 0; i < most; ++i)
                if (!p.wanted.count(p.held[i]) && (chosen < 0 || p.used[i] < p.used[static_cast<std::size_t>(chosen)])) chosen = static_cast<int>(i);
        if (chosen < 0) continue; // no room: every square holds a picture asked for (or this one is not asked for any more)
        const auto c = static_cast<UINT>(chosen);
        forget_square(p, c);
        p.held[c] = id;
        p.used[c] = p.ask_count;
        detail::PackCell item;
        item.cell = c;
        item.key = key;
        item.id = id;
        item.rgba = std::move(one.rgba);
        item.after = p.frame + 2;
        p.waiting[c] = std::move(item);
    }
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
    std::string ask;
    for (const auto& [name, file] : wanted) ask += name + '\t' + file + '\n';
    auto& p = pictures();
    std::vector<std::pair<std::string, std::string>> fresh;
    {
        std::lock_guard lock(p.mutex);
        if (ask == p.asked) return;
        p.asked = std::move(ask);
        p.wanted.clear();
        ++p.ask_count;
        for (const auto& [key, file] : wanted) {
            if (p.wanted.size() >= most) break;
            auto id = key + '\t' + file;
            if (!p.wanted.insert(id).second) continue;
            if (const auto at = std::find(p.held.begin(), p.held.end(), id); at != p.held.end()) {
                p.used[static_cast<std::size_t>(at - p.held.begin())] = p.ask_count;
                continue;
            }
            if (p.reading.count(id)) continue;
            if (const auto bad = p.unreadable.find(id); bad != p.unreadable.end()) {
                if (std::chrono::steady_clock::now() - bad->second < std::chrono::seconds(5)) continue; // a half-written file, say: later
                p.unreadable.erase(bad);
            }
            p.reading.insert(std::move(id));
            fresh.emplace_back(key, file);
        }
    }
    if (fresh.empty()) return;
    try {
        std::thread(read_pictures, fresh).detach();
    } catch (...) {
        std::lock_guard lock(p.mutex);
        for (const auto& [key, file] : fresh) p.reading.erase(key + '\t' + file);
    }
}
bool studio_pack_picture(const std::string& key, ImTextureID& texture, ImVec2& uv0, ImVec2& uv1) {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    if (!p.texture) return false;
    const auto found = p.ready.find(key);
    if (found == p.ready.end()) return false;
    constexpr float width = static_cast<float>(cell * columns), height = static_cast<float>(cell * detail::studio_pack_rows);
    const float x = static_cast<float>((found->second % columns) * cell), y = static_cast<float>((found->second / columns) * cell);
    texture = p.texture;
    uv0 = ImVec2(x / width, y / height);
    uv1 = ImVec2((x + cell) / width, (y + cell) / height);
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
bool take_studio_pack_cells(std::vector<PackCell>& out, std::size_t most_now) {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    ++p.frame;
    for (auto it = p.waiting.begin(); it != p.waiting.end() && out.size() < most_now;) {
        if (it->second.after > p.frame) {
            ++it;
            continue;
        }
        out.push_back(std::move(it->second));
        it = p.waiting.erase(it);
    }
    return !out.empty();
}
void studio_pack_cells_uploaded(ImTextureID texture, const std::vector<PackCell>& cells) {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    p.texture = texture;
    for (const auto& c : cells)
        if (c.cell < most && p.held[c.cell] == c.id) p.ready[c.key] = c.cell; // not given to another picture since
}
// The texture is gone (or could not be made): every picture is read again when next asked for.
void studio_pack_pixels_lost() {
    auto& p = pictures();
    std::lock_guard lock(p.mutex);
    p.texture = {};
    p.ready.clear();
    p.waiting.clear();
    p.held.fill({});
    p.used.fill(0);
    p.asked.clear();
}
} // namespace detail
} // namespace dingosdk::overlay
