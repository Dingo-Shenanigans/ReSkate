#include "park_previews.h"
#include "Engine/Core/Log/logging.h"
#include <algorithm>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace dingosdk::overlay {
namespace {
constexpr std::uint32_t side = 128;
struct Previews {
    ImFontAtlas *atlas{};
    std::map<std::string, int, std::less<>> rectangles;
};
Previews &previews() {
    static Previews state;
    return state;
}
struct Read {
    std::mutex mutex;
    std::future<vfs::ThumbnailRead> pending;
    // Kept once read: each atlas the overlay builds (one per swapchain) needs them again.
    std::optional<vfs::ThumbnailRead> result;
};
Read &read() {
    static auto *state = new Read;
    return *state;
}
bool valid_key(std::string_view key) {
    return key.starts_with("own_bk") && key.size() <= 256 &&
           std::all_of(key.begin(), key.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}
} // namespace

void clear_park_previews() noexcept {
    previews().rectangles.clear();
    previews().atlas = nullptr;
}

void start_park_previews(std::filesystem::path game_root) {
    auto &state = read();
    std::lock_guard lock(state.mutex);
    if (state.pending.valid())
        return;
    state.pending = std::async(std::launch::async, [root = std::move(game_root)] {
        try {
            return vfs::read_build_kit_thumbnails(root, side);
        } catch (const std::exception &error) {
            logging::log(logging::Level::warning, logging::Channel::graphics,
                         "Park library thumbnails unavailable: {}", error.what());
            return vfs::ThumbnailRead{};
        }
    });
}

std::size_t load_park_previews(ImFontAtlas &atlas, std::chrono::milliseconds wait) noexcept {
    try {
        auto &state = read();
        std::lock_guard lock(state.mutex);
        if (!state.result) {
            if (!state.pending.valid() || state.pending.wait_for(wait) != std::future_status::ready)
                return 0;
            state.result = state.pending.get();
            if (state.result->failed)
                logging::log(logging::Level::warning, logging::Channel::graphics,
                             "Park library: {} object thumbnails could not be decoded.", state.result->failed);
        }
        return load_park_previews(atlas, state.result->thumbnails);
    } catch (...) {
        return 0;
    }
}

std::size_t load_park_previews(ImFontAtlas &atlas, std::span<const vfs::ItemThumbnail> thumbnails) noexcept {
    clear_park_previews();
    try {
        std::set<std::string_view> keys;
        for (const auto &thumbnail : thumbnails)
            if (!valid_key(thumbnail.item) || thumbnail.image.width != side || thumbnail.image.height != side ||
                thumbnail.image.rgba.size() != std::size_t{side} * side * 4 || !keys.insert(thumbnail.item).second)
                return 0;
        if (thumbnails.empty() || thumbnails.size() > 1024)
            return 0;
        // Use the existing, fenced ImGui upload/lifetime. No native game GPU
        // descriptors, resource states or streamed texture pointers are borrowed.
        auto &state = previews();
        atlas.TexDesiredWidth = std::max(atlas.TexDesiredWidth, 2048); // wider when the fonts asked for it (load_skate_fonts)
        atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
        for (const auto &thumbnail : thumbnails)
            state.rectangles.emplace(thumbnail.item, atlas.AddCustomRectRegular(side, side));
        unsigned char *pixels{};
        int width{}, height{};
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0 || width > 16384 || height > 16384) {
            clear_park_previews();
            return 0;
        }
        for (const auto &thumbnail : thumbnails) {
            const auto *rect = atlas.GetCustomRectByIndex(state.rectangles.at(thumbnail.item));
            if (!rect->IsPacked() || rect->X + side > static_cast<unsigned>(width) ||
                rect->Y + side > static_cast<unsigned>(height)) {
                clear_park_previews();
                return 0;
            }
            for (std::size_t y = 0; y < side; ++y)
                std::memcpy(pixels + ((rect->Y + y) * static_cast<std::size_t>(width) + rect->X) * 4,
                            thumbnail.image.rgba.data() + y * side * 4, side * 4);
        }
        atlas.TexPixelsUseColors = true;
        state.atlas = &atlas;
        return thumbnails.size();
    } catch (...) {
        clear_park_previews();
        return 0;
    }
}

bool park_preview_image(std::string_view key, ParkPreviewImage &image) {
    const auto &state = previews();
    if (!state.atlas || !ImGui::GetCurrentContext() || ImGui::GetIO().Fonts != state.atlas)
        return false;
    const auto it = state.rectangles.find(key);
    if (it == state.rectangles.end() || !state.atlas->TexID)
        return false;
    const auto *rect = state.atlas->GetCustomRectByIndex(it->second);
    if (!rect->IsPacked())
        return false;
    image.texture = state.atlas->TexID;
    state.atlas->CalcCustomRectUV(rect, &image.uv0, &image.uv1);
    return true;
}
} // namespace dingosdk::overlay
