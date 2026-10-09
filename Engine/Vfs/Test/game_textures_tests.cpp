// Game textures read from an installed game: dingosdk_game_textures_tests <Skate folder>.
// Without a folder there is nothing to read, and it passes.
#include "Engine/Vfs/game_textures.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/ui_textures.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace dingosdk;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// How much of a region of the image is solid, 0 to 1.
float solid(const frostbite::Image& image, const addr::ui_textures::Region& region) {
    const auto left = static_cast<std::uint32_t>(region.left * image.width), right = static_cast<std::uint32_t>(region.right * image.width);
    const auto top = static_cast<std::uint32_t>(region.top * image.height), bottom = static_cast<std::uint32_t>(region.bottom * image.height);
    std::size_t opaque{}, all{};
    for (auto y = top; y < bottom; ++y)
        for (auto x = left; x < right; ++x, ++all) opaque += image.rgba[(std::size_t{y} * image.width + x) * 4 + 3] >= 128;
    return all ? static_cast<float>(opaque) / static_cast<float>(all) : 0.0f;
}

void the_overlays_textures_read(const char* game_root) {
    namespace ui = addr::ui_textures;
    vfs::GameTextures textures(game_root);
    for (const auto& texture : {ui::airtime, ui::wipeout, ui::spread_eagle, ui::gap_height, ui::stopwatch}) {
        const auto image = textures.read(texture.toc, texture.bundle, texture.name, 64);
        check(image.width == 64 && image.height == 64 && image.rgba.size() == 64 * 64 * 4, "a 64 pixel icon");
    }
    const auto broken = textures.read(ui::wipeout_broken.toc, ui::wipeout_broken.bundle, ui::wipeout_broken.name, 64);
    check(broken.width == 64 && broken.height == 60, "a wider icon fits the square, its aspect kept");
    const auto wheel = textures.read(ui::flaming_wheel.toc, ui::flaming_wheel.bundle, ui::flaming_wheel.name, 64);
    check(wheel.width == 64 && wheel.height == 64, "a larger icon is taken from a smaller mip");
    // Streamed: the full size comes from the TOC's own chunk, not the bundle's small mips.
    const auto logo = textures.read(ui::thrasher_wordmark.toc, ui::thrasher_wordmark.bundle, ui::thrasher_wordmark.name, 512);
    check(logo.width == 512 && logo.height == 256, "the streamed logo at full size");
    bool opaque{}, clear{};
    for (std::size_t i = 3; i < logo.rgba.size(); i += 4) (logo.rgba[i] ? opaque : clear) = true;
    check(opaque && clear, "the logo on a clear background");
    // Beneath the wordmark's arch, between a fifth in from either side, as fractions of the whole logo.
    const auto& mark = ui::thrasher_wordmark.region;
    const float fifth = (mark.right - mark.left) / 5;
    check(solid(logo, {mark.left + fifth, mark.top + ui::thrasher_wordmark_arch * (mark.bottom - mark.top), mark.right - fifth, mark.bottom}) == 0,
        "the wordmark's arch is clear beneath");
    bool refused{};
    try { (void)textures.read(ui::airtime.toc, ui::airtime.bundle, "ui/textures/icons/no_such_icon", 64); }
    catch (const std::runtime_error&) { refused = true; }
    check(refused, "a missing texture is refused");
}

void the_ui_shapes_read(const char* game_root) {
    namespace ui = addr::ui_textures;
    vfs::GameTextures textures(game_root);
    const auto read = [&](const ui::Texture& texture, std::uint32_t side) {
        return textures.read(texture.toc, texture.bundle, texture.name, side);
    };
    const auto bar = read(ui::brush_bar, 512), streak = read(ui::streak, 512);
    check(bar.width == 512 && bar.height == 192 && streak.width == 512 && streak.height == 76, "the strokes, their aspect kept");
    check(solid(bar, ui::brush_bar.body) > 0.95f && solid(bar, {}) < 0.6f, "a stroke's body is its solid bar, not its splatter");
    check(solid(streak, ui::streak.body) > 0.15f && solid(streak, {}) < solid(streak, ui::streak.body),
        "the streak tapers within its body");
    const auto tile = read(ui::rough_tile, 256);
    check(tile.width == 256 && solid(tile, {ui::rough_tile.slice, ui::rough_tile.slice, 1 - ui::rough_tile.slice,
                                            1 - ui::rough_tile.slice}) > 0.99f,
        "the tile is solid inside its rough edges");
    const auto scratches = read(ui::scratches, 512);
    std::uint8_t brightest{};
    for (std::size_t i = 0; i < scratches.rgba.size(); i += 4) brightest = std::max(brightest, scratches.rgba[i]);
    check(scratches.width == 512 && solid(scratches, {}) > 0.99f && brightest > 64, "light scratches on opaque black");
}
}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && *argv[1]) {
            the_overlays_textures_read(argv[1]);
            the_ui_shapes_read(argv[1]);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Game texture tests passed.\n";
    return 0;
}
