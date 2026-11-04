#include "chat_emotes.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Text/ascii.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <span>

namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
constexpr int max_emote_side = 256;
constexpr std::size_t max_frames = 4096;

struct Frame {
    int width{}, height{};
    std::uint32_t time{}; // ms this frame shows (animated emotes)
    std::vector<unsigned char> rgba;
};
struct Emote {
    std::vector<Frame> frames;
    std::uint32_t duration{}; // sum of the frame times; 0 = still
};
using Emotes = std::map<std::string, Emote, std::less<>>;
struct Read {
    std::mutex mutex;
    std::future<Emotes> pending;
    // Kept once decoded: the overlay builds a new atlas each time the game recreates its
    // swapchain (resizes, display mode changes), and every one needs the emotes again.
    std::optional<Emotes> decoded;
};
Read &read() {
    static auto *state = new Read;
    return *state;
}
// An emote in the current atlas: the decoded one (kept in Read for the process, so each
// new atlas copies its pixels straight from there) and the atlas rect of each frame.
struct Placed {
    const Emote *emote{};
    std::vector<int> rects;
};
struct Loaded {
    ImFontAtlas *atlas{};
    std::map<std::string, Placed, std::less<>> emotes;
    std::map<std::string, std::string, std::less<>> by_lower; // lowercase name -> name
    bool filled{};
};
Loaded &loaded() {
    static Loaded state;
    return state;
}
bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= 32 &&
           std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}
// A resource built into ReSkate.dll (cmake/Runtime.cmake); it lives as long as the DLL.
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
// The pack's texture as straight-alpha RGBA.
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
        width > 8192 || height > 8192)
        return false;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}
// One frame cut out of the pack texture, or nothing when it lies outside it.
std::optional<Frame> cut(const std::vector<unsigned char> &pixels, UINT width, UINT height, const Json &pos, int w, int h,
                         std::uint32_t time) {
    if (!pos.is_array() || pos.size() != 2 || w <= 0 || h <= 0 || w > max_emote_side || h > max_emote_side) return std::nullopt;
    const auto x = pos.at(0).get<int>(), y = pos.at(1).get<int>();
    if (x < 0 || y < 0 || static_cast<UINT>(x + w) > width || static_cast<UINT>(y + h) > height) return std::nullopt;
    Frame frame{w, h, time};
    frame.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (int row = 0; row < h; ++row)
        std::memcpy(frame.rgba.data() + static_cast<std::size_t>(row) * w * 4,
                    pixels.data() + ((static_cast<std::size_t>(y) + row) * width + x) * 4, static_cast<std::size_t>(w) * 4);
    return frame;
}
// The pack built into ReSkate.dll: assets/emotes/emotes.json and emotes.png in the repository.
Emotes read_embedded() {
    Emotes emotes;
    const auto json_bytes = embedded(L"EMOTES_JSON"), png = embedded(L"EMOTES_PNG");
    if (json_bytes.empty() || png.empty()) return emotes;
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    try {
        const auto json = Json::parse(std::string_view(reinterpret_cast<const char *>(json_bytes.data()), json_bytes.size()));
        if (!json.contains("emotes") || !json.at("emotes").is_object()) throw std::runtime_error("no \"emotes\"");
        std::vector<unsigned char> pixels;
        UINT width{}, height{};
        if (!decode(png, pixels, width, height)) throw std::runtime_error("the texture cannot be decoded");
        std::size_t frames{};
        unsigned skipped{};
        for (const auto &[name, definition] : json.at("emotes").items()) {
            if (!valid_name(name) || emotes.contains(name) || !definition.is_object() || !definition.contains("size")) {
                ++skipped;
                continue;
            }
            const auto &size = definition.at("size");
            if (!size.is_array() || size.size() != 2) { ++skipped; continue; }
            const int w = size.at(0).get<int>(), h = size.at(1).get<int>();
            Emote emote;
            if (definition.contains("frames") && definition.at("frames").is_array()) {
                for (const auto &f : definition.at("frames")) {
                    if (!f.is_object() || !f.contains("pos")) continue;
                    const auto time = std::clamp(f.value("time", 100U), 10U, 10000U);
                    if (auto frame = cut(pixels, width, height, f.at("pos"), w, h, time)) {
                        emote.duration += time;
                        emote.frames.push_back(std::move(*frame));
                    }
                }
            } else if (definition.contains("pos")) {
                if (auto frame = cut(pixels, width, height, definition.at("pos"), w, h, 0)) emote.frames.push_back(std::move(*frame));
            }
            if (emote.frames.empty() || frames + emote.frames.size() > max_frames) { ++skipped; continue; }
            frames += emote.frames.size();
            if (emote.frames.size() == 1) emote.duration = 0;
            emotes.emplace(name, std::move(emote));
        }
        logging::log(logging::Level::info, logging::Channel::ui, "Chat emotes: {} built in{}.", emotes.size(),
                     skipped ? " (" + std::to_string(skipped) + " skipped)" : std::string{});
    } catch (const std::exception &e) {
        emotes.clear();
        logging::log(logging::Level::warning, logging::Channel::ui, "Chat emotes unavailable: {}", e.what());
    }
    if (com) CoUninitialize();
    return emotes;
}
} // namespace

void start_chat_emotes() {
    auto &state = read();
    std::lock_guard lock(state.mutex);
    if (state.pending.valid()) return;
    state.pending = std::async(std::launch::async, [] {
        try {
            return read_embedded();
        } catch (...) {
            return Emotes{};
        }
    });
}

std::size_t reserve_chat_emotes(ImFontAtlas &atlas, std::chrono::milliseconds wait) noexcept {
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
        for (const auto &[name, emote] : *state.decoded) {
            auto &placed = l.emotes[name];
            placed.emote = &emote;
            for (const auto &frame : emote.frames) placed.rects.push_back(atlas.AddCustomRectRegular(frame.width, frame.height));
            l.by_lower.emplace(ascii_lower(name), name);
        }
        l.atlas = &atlas;
        return l.emotes.size();
    } catch (...) {
        loaded() = {};
        return 0;
    }
}

void fill_chat_emotes(ImFontAtlas &atlas) noexcept {
    auto &l = loaded();
    if (l.atlas != &atlas || l.emotes.empty()) return;
    try {
        unsigned char *pixels{};
        int width{}, height{};
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0) { l = {}; return; }
        for (const auto &[name, placed] : l.emotes)
            for (std::size_t i = 0; i < placed.emote->frames.size(); ++i) {
                const auto &frame = placed.emote->frames[i];
                const auto *rect = atlas.GetCustomRectByIndex(placed.rects[i]);
                if (!rect || !rect->IsPacked() || rect->X + frame.width > width || rect->Y + frame.height > height) {
                    l = {};
                    return;
                }
                for (int y = 0; y < frame.height; ++y)
                    std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4,
                                frame.rgba.data() + static_cast<std::size_t>(y) * frame.width * 4,
                                static_cast<std::size_t>(frame.width) * 4);
            }
        atlas.TexPixelsUseColors = true;
        l.filled = true;
    } catch (...) {
        l = {};
    }
}

bool chat_emotes_loaded() noexcept { return loaded().filled; }

bool chat_emote(std::string_view name, ChatEmote &image) {
    const auto &l = loaded();
    if (!l.filled || !l.atlas || !ImGui::GetCurrentContext() || ImGui::GetIO().Fonts != l.atlas || !l.atlas->TexID) return false;
    auto it = l.emotes.find(name);
    if (it == l.emotes.end()) {
        const auto alias = l.by_lower.find(ascii_lower(name));
        if (alias == l.by_lower.end()) return false;
        it = l.emotes.find(alias->second);
        if (it == l.emotes.end()) return false;
    }
    const auto &placed = it->second;
    const auto &emote = *placed.emote;
    std::size_t index = 0;
    if (emote.duration) {
        const auto now = static_cast<std::uint64_t>(ImGui::GetTime() * 1000.0);
        auto at = static_cast<std::uint32_t>(now % emote.duration);
        for (std::size_t i = 0; i < emote.frames.size(); ++i) {
            if (at < emote.frames[i].time) { index = i; break; }
            at -= emote.frames[i].time;
        }
    }
    const Frame *frame = &emote.frames[index];
    const auto *rect = l.atlas->GetCustomRectByIndex(placed.rects[index]);
    if (!rect || !rect->IsPacked()) return false;
    image.texture = l.atlas->TexID;
    l.atlas->CalcCustomRectUV(rect, &image.uv0, &image.uv1);
    image.aspect = static_cast<float>(frame->width) / static_cast<float>(frame->height);
    return true;
}

std::vector<std::string> chat_emote_names(std::string_view prefix, std::size_t limit) {
    std::vector<std::string> names;
    const auto &l = loaded();
    if (!l.filled) return names;
    const auto wanted = ascii_lower(prefix);
    for (auto it = l.by_lower.lower_bound(wanted); it != l.by_lower.end() && names.size() < limit; ++it) {
        if (!it->first.starts_with(wanted)) break;
        names.push_back(it->second);
    }
    return names;
}
} // namespace dingosdk::overlay
