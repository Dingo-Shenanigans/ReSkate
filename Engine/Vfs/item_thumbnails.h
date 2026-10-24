#pragma once
#include "Engine/Resource/texture.h"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::vfs {
struct ItemThumbnail {
    std::string item;       // owned asset id, e.g. own_bkbanks_generic_twinkiehalf_00001
    frostbite::Image image; // side x side RGBA
};
struct ThumbnailRead {
    std::vector<ItemThumbnail> thumbnails;
    std::size_t failed{};   // textures that were found but could not be decoded
};
// The build-kit object thumbnails the game ships ("thumbnail/tool/own_bk*"),
// read from the installed game's data and scaled to `side` x `side`.
ThumbnailRead read_build_kit_thumbnails(const std::filesystem::path& gameRoot, std::uint32_t side);
// Textures of the game's own UI by their resource names (e.g. "ui/textures/icons/cas/img_cas_tops_128"),
// from the bundle that holds them, scaled to `side` x `side`; `item` is the name. Names that are not
// in the bundle are left out.
ThumbnailRead read_ui_textures(const std::filesystem::path& gameRoot, std::string_view bundle,
                               std::span<const std::string> names, std::uint32_t side);
}
