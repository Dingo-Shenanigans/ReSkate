#include "game_images.h"
#include "Extension/Skater/xray_skeleton.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/ui_textures.h"
#include "Engine/Vfs/game_textures.h"
#include "Engine/Vfs/item_thumbnails.h"
#include "Extension/UI/Overlay/overlay_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <optional>

namespace dingosdk::overlay {
namespace {
namespace ui = addr::ui_textures;
namespace theme = dingosdk::skate_theme;

// The pictures, by the names plugins give them.
enum Picture : std::size_t { loading_board = 13, first_icon = 14, picture_count = 31 };
constexpr std::array<std::string_view, picture_count> names{
    "stopwatch", "wipeout", "wipeout_broken", "spread_eagle", "airtime", "gap_height", "flaming_wheel", "roll",
    "thrasher", "brush_bar", "rough_tile", "scratches", "streak", "loading_board",
    "cas_tops", "cas_bottoms", "cas_shoes", "cas_outfit", "cas_fullbody", "cas_headwear", "cas_headaccessories", "cas_eyewear", "cas_handwear", "cas_socks", "cas_deck", "cas_trucks", "cas_wheels", "cas_gripcolor", "cas_grippattern", "cas_sticker", "cas_decorator_vfxcosmetic"};
// The customization menu's category icons (the pictures from first_icon on): its tabs', white on clear,
// in the bundle with the rest of the core game assets, which is looked for among the game's TOCs.
// (Pull request 269, the player's own item tabs.)
constexpr std::string_view icon_bundle = "win32/levels/game/bam_levelroot/bam_coregameassets";
constexpr std::uint32_t icon_side = 96;
struct Source {
    const ui::Texture* texture;
    std::uint32_t side;      // fitted into a side x side square, its aspect kept
    bool brightness{};       // white with the texture's brightness as alpha (light marks on black), else its own alpha
    bool colours{};          // its own colours, as it is (the loading board)
};
// The loading board's first place stands for it; decode tries all of ui::loading_board's.
constexpr std::array<Source, first_icon> sources{{
    {&ui::stopwatch, 64}, {&ui::wipeout, 64}, {&ui::wipeout_broken, 64}, {&ui::spread_eagle, 64},
    {&ui::airtime, 64}, {&ui::gap_height, 64}, {&ui::flaming_wheel, 64}, {&ui::roll, 64},
    {&ui::thrasher_wordmark, 512}, {&ui::brush_bar, 512}, {&ui::rough_tile, 256}, {&ui::scratches, 512, true},
    {&ui::streak, 512}, {&ui::loading_board[0], 1152, false, true},
}};
using Images = std::array<std::optional<frostbite::Image>, picture_count>;

struct Reads {
    std::mutex mutex;
    bool started{};
    std::future<Images> pending;
    Images decoded; // kept: every atlas the overlay builds (one per swapchain) needs them
};
Reads& reads() {
    static auto* state = new Reads;
    return *state;
}

// The picture's part of the texture, white: with its own alpha, or its brightness as alpha.
frostbite::Image cut(const frostbite::Image& image, const ui::Region& region, bool brightness) {
    const auto at = [](float fraction, std::uint32_t size) { return static_cast<std::uint32_t>(fraction * static_cast<float>(size) + 0.5f); };
    if (!image.width || !image.height || image.rgba.size() != std::size_t{image.width} * image.height * 4)
        throw std::runtime_error("the texture is empty");
    const auto x0 = std::min(at(region.left, image.width), image.width - 1), y0 = std::min(at(region.top, image.height), image.height - 1);
    const auto x1 = std::clamp(at(region.right, image.width), x0 + 1, image.width);
    const auto y1 = std::clamp(at(region.bottom, image.height), y0 + 1, image.height);
    frostbite::Image result{x1 - x0, y1 - y0, std::vector<std::uint8_t>(std::size_t{x1 - x0} * (y1 - y0) * 4)};
    for (std::uint32_t y = y0; y < y1; ++y)
        std::memcpy(result.rgba.data() + std::size_t{y - y0} * result.width * 4,
                    image.rgba.data() + (std::size_t{y} * image.width + x0) * 4, std::size_t{result.width} * 4);
    for (std::size_t i = 0; i < result.rgba.size(); i += 4) {
        auto* p = result.rgba.data() + i;
        if (brightness) p[3] = std::max({p[0], p[1], p[2]});
        p[0] = p[1] = p[2] = 255;
    }
    return result;
}

Images decode() {
    Images result;
    try {
        vfs::GameTextures textures(xray::game_folder());
        for (std::size_t index = 0; index < first_icon; ++index) {
            const auto& source = sources[index];
            if (source.colours) {
                for (const auto& place : ui::loading_board) {
                    try {
                        result[index] = textures.read(place.toc, place.bundle, place.name, source.side);
                        break;
                    } catch (const std::exception& error) {
                        logging::log(logging::Level::debug, logging::Channel::graphics, "The loading board is not in {}: {}.", place.bundle, error.what());
                    }
                }
                if (!result[index]) logging::write(logging::Level::warning, logging::Channel::graphics, "The game's loading board picture is unavailable.");
                continue;
            }
            try {
                result[index] = cut(textures.read(source.texture->toc, source.texture->bundle, source.texture->name, source.side),
                                    source.texture->region, source.brightness);
            } catch (const std::exception& error) {
                logging::log(logging::Level::warning, logging::Channel::graphics, "The game's picture {} is unavailable: {}.",
                    source.texture->name, error.what());
            }
        }
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "The game's pictures are unavailable: {}.", error.what());
    }
    try {
        std::vector<std::string> wanted;
        for (std::size_t index = first_icon; index < picture_count; ++index)
            wanted.push_back("ui/textures/icons/cas/img_cas_" + std::string(names[index].substr(4)) + "_128_128_128");
        const auto read = vfs::read_ui_textures(xray::game_folder(), icon_bundle, wanted, icon_side);
        for (std::size_t i = 0; i < wanted.size(); ++i)
            for (const auto& texture : read.thumbnails)
                if (texture.item == wanted[i] && texture.image.rgba.size() == std::size_t{icon_side} * icon_side * 4) result[first_icon + i] = texture.image;
        std::size_t found{};
        for (std::size_t index = first_icon; index < picture_count; ++index) found += result[index].has_value();
        if (found < wanted.size())
            logging::log(logging::Level::warning, logging::Channel::graphics, "The menu's category icons: {} of {} found.", found, wanted.size());
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "The menu's category icons are unavailable: {}.", error.what());
    }
    return result;
}

// The images in the atlas being built or drawn with: render thread only.
struct Loaded {
    ImFontAtlas* atlas{};
    bool filled{};
    std::array<int, picture_count> rects{}; // each one's custom rect in the atlas; -1 none
};
Loaded& loaded() {
    static Loaded state;
    return state;
}
// A picture in the atlas the overlay draws with: where, and its texture coordinates.
struct Found {
    const ImFontAtlasCustomRect* rect{};
    ImVec2 uv0, uv1;
};
bool find(Picture picture, Found& found) {
    const auto& l = loaded();
    if (!l.filled || !l.atlas || ImGui::GetIO().Fonts != l.atlas || !l.atlas->TexID || l.rects[picture] < 0) return false;
    found.rect = l.atlas->GetCustomRectByIndex(l.rects[picture]);
    if (!found.rect || !found.rect->IsPacked() || !found.rect->Width || !found.rect->Height) return false;
    l.atlas->CalcCustomRectUV(found.rect, &found.uv0, &found.uv1);
    return true;
}

// Fitted into the box with its aspect kept, centred: an icon, a logo; of a flipbook (the loading
// board), its frame `frame`.
bool draw_image(ImDrawList* draw, Picture picture, ImVec2 min, ImVec2 max, ImU32 tint, int frame) {
    Found found;
    if (!find(picture, found)) return false;
    const auto* rect = found.rect;
    float width = rect->Width, height = rect->Height;
    if (picture == loading_board) {
        const int f = std::clamp(frame, 0, ui::loading_board_frames - 1);
        const float du = (found.uv1.x - found.uv0.x) / ui::loading_board_columns, dv = (found.uv1.y - found.uv0.y) / ui::loading_board_rows;
        found.uv0 = ImVec2(found.uv0.x + du * static_cast<float>(f % ui::loading_board_columns), found.uv0.y + dv * static_cast<float>(f / ui::loading_board_columns));
        found.uv1 = ImVec2(found.uv0.x + du, found.uv0.y + dv);
        width /= ui::loading_board_columns;
        height /= ui::loading_board_rows;
    }
    const float scale = std::min((max.x - min.x) / width, (max.y - min.y) / height);
    const ImVec2 size(width * scale, height * scale);
    const ImVec2 at(min.x + (max.x - min.x - size.x) * 0.5f, min.y + (max.y - min.y - size.y) * 0.5f);
    draw->AddImage(loaded().atlas->TexID, at, ImVec2(at.x + size.x, at.y + size.y), found.uv0, found.uv1, tint);
    return true;
}
// Its body (ui_textures.h Texture::body) stretched onto the box, the rest of it around: a brush
// stroke's bar on the box, its splatter beyond.
bool draw_shape(ImDrawList* draw, Picture picture, ImVec2 min, ImVec2 max, ImU32 tint) {
    Found found;
    if (!find(picture, found)) return false;
    const auto& body = sources[picture].texture->body;
    const float width = (max.x - min.x) / (body.right - body.left), height = (max.y - min.y) / (body.bottom - body.top);
    const ImVec2 at(min.x - body.left * width, min.y - body.top * height);
    draw->AddImage(loaded().atlas->TexID, at, ImVec2(at.x + width, at.y + height), found.uv0, found.uv1, tint);
    return true;
}
// Nine-sliced onto the box: its corners (Texture::slice of the picture) kept `edge` pixels square,
// its edges stretched along them and its middle across the rest. A box smaller than two edges
// shrinks them to half of it.
bool draw_panel(ImDrawList* draw, Picture picture, ImVec2 min, ImVec2 max, float edge, ImU32 tint) {
    Found found;
    if (!find(picture, found)) return false;
    const float slice = sources[picture].texture->slice;
    const float edge_x = std::clamp(edge, 0.0f, (max.x - min.x) * 0.5f), edge_y = std::clamp(edge, 0.0f, (max.y - min.y) * 0.5f);
    const float uv_x = slice * (found.uv1.x - found.uv0.x), uv_y = slice * (found.uv1.y - found.uv0.y);
    const std::array<float, 4> xs{min.x, min.x + edge_x, max.x - edge_x, max.x}, ys{min.y, min.y + edge_y, max.y - edge_y, max.y};
    const std::array<float, 4> us{found.uv0.x, found.uv0.x + uv_x, found.uv1.x - uv_x, found.uv1.x};
    const std::array<float, 4> vs{found.uv0.y, found.uv0.y + uv_y, found.uv1.y - uv_y, found.uv1.y};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            draw->AddImage(loaded().atlas->TexID, ImVec2(xs[column], ys[row]), ImVec2(xs[column + 1], ys[row + 1]),
                           ImVec2(us[column], vs[row]), ImVec2(us[column + 1], vs[row + 1]), tint);
    return true;
}
} // namespace

void prepare_game_images() noexcept {
    try {
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        if (std::exchange(state.started, true)) return;
        state.pending = std::async(std::launch::async, &decode);
    } catch (...) {
        logging::write(logging::Level::warning, logging::Channel::graphics, "The game's pictures are unavailable: their reading could not start.");
    }
}

void reserve_game_images(ImFontAtlas& atlas, std::chrono::milliseconds wait) noexcept {
    auto& l = loaded();
    l = {};
    l.rects.fill(-1);
    try {
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        // A read still running stays pending for the next atlas.
        if (state.pending.valid() && state.pending.wait_for(wait) == std::future_status::ready) state.decoded = state.pending.get();
        for (std::size_t index = 0; index < picture_count; ++index) {
            const auto& image = state.decoded[index];
            if (!image) continue;
            if (!l.atlas) {
                atlas.TexDesiredWidth = std::max(atlas.TexDesiredWidth, 2048);
                atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
                l.atlas = &atlas;
            }
            l.rects[index] = atlas.AddCustomRectRegular(static_cast<int>(image->width), static_cast<int>(image->height));
        }
    } catch (...) {
        l = {};
    }
}

void fill_game_images(ImFontAtlas& atlas) noexcept {
    auto& l = loaded();
    if (l.atlas != &atlas) return;
    try {
        unsigned char* pixels{};
        int width{}, height{};
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0) {
            l = {};
            return;
        }
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        for (std::size_t index = 0; index < picture_count; ++index) {
            if (l.rects[index] < 0) continue;
            const auto& image = *state.decoded[index];
            const auto* rect = atlas.GetCustomRectByIndex(l.rects[index]);
            if (!rect || !rect->IsPacked() || rect->X + image.width > static_cast<unsigned>(width) ||
                rect->Y + image.height > static_cast<unsigned>(height)) {
                l = {};
                return;
            }
            for (std::uint32_t y = 0; y < image.height; ++y)
                std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4,
                            image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4, static_cast<std::size_t>(image.width) * 4);
        }
        atlas.TexPixelsUseColors = true;
        l.filled = true;
    } catch (...) {
        l = {};
    }
}

bool game_image_known(std::string_view name) noexcept { return std::find(names.begin(), names.end(), name) != names.end(); }
bool draw_game_image(ImDrawList* draw, std::string_view name, ImVec2 min, ImVec2 max, ImU32 tint, GameImageFit fit, float edge, int frame) {
    const auto at = std::find(names.begin(), names.end(), name);
    if (at == names.end()) return false;
    const auto picture = static_cast<Picture>(at - names.begin());
    if (picture == loading_board) return draw_image(draw, picture, min, max, tint, frame); // a flipbook's frame, fitted
    if (picture >= first_icon) return draw_image(draw, picture, min, max, tint, 0);        // an icon: always fitted
    switch (fit) {
    case GameImageFit::contain: return draw_image(draw, picture, min, max, tint, 0);
    case GameImageFit::shape: return draw_shape(draw, picture, min, max, tint);
    case GameImageFit::panel: return draw_panel(draw, picture, min, max, edge, tint);
    }
    return false;
}
float thrasher_arch() noexcept { return ui::thrasher_wordmark_arch; }
}
