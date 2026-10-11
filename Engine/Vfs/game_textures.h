#pragma once
// Textures out of the installed game's data, decoded for display: icons, logos, anything the
// game ships as a 2D texture. Each superbundle TOC and bundle is read once per reader.
#include "game_bundles.h"
#include "Engine/Resource/texture.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace dingosdk::vfs {
class GameTextures {
public:
    // `gameRoot` is the folder with Skate.exe.
    explicit GameTextures(std::filesystem::path gameRoot);

    // The texture `name` in `bundle` of the superbundle `toc`, fitted into a `side` x `side`
    // square with its aspect kept: decoded from its smallest mip that still covers it, then
    // scaled down. Throws when it is missing or cannot be decoded.
    [[nodiscard]] frostbite::Image read(std::string_view toc, std::string_view bundle, std::string_view name,
                                        std::uint32_t side);

private:
    const frostbite::TocDocument& toc(std::string_view name);
    const GameBundle& bundle(std::string_view toc, std::string_view name);

    GameData data_;
    std::map<std::string, frostbite::TocDocument, std::less<>> tocs_;
    std::map<std::pair<std::string, std::string>, GameBundle> bundles_;
};
}
