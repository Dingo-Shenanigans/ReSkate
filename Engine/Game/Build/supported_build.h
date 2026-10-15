#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// This is the accepted contract identity, not a list of hashes to bypass.
// Advance it only together with the reviewed native/asset contracts in
// Engine/Game/Build/<build>/. Diagnostic tools with historical addresses
// intentionally retain their own historical build pins.
namespace dingosdk::supported_build {

inline constexpr std::uint64_t game_file_size = 143196032;
inline constexpr std::uint32_t game_image_size = 0x08FF5000;
inline constexpr std::string_view game_sha256 =
    "915522645e3b151a641743f52b6c7437e75b6d1381d8420edaa83493b025c4b2";
inline constexpr std::uint64_t steam_api_file_size = 300928;
inline constexpr std::string_view steam_api_sha256 =
    "25d4c6a6e2e18cb1baada01e8946b03c08e41d7fc16337ecc1dc37bfe55f1e54";
// The Steam content that ships the files above; the launcher config repeats these
// so the launcher can fetch this build with DepotDownloader.
inline constexpr std::uint32_t steam_app_id = 3354750;
inline constexpr std::uint32_t steam_depot_id = 3354751;
inline constexpr std::string_view steam_manifest_id = "4187479023593343011";
inline constexpr std::string_view steam_build_id = "25617022";
// The build number this build's data files carry (Data\layout.toc, "pipelineCodeChangelists"):
// what mods made for it carry too. Data of another number beside a supported Skate.exe is a
// game that was updated or copied over in part, and has to be verified.
inline constexpr std::string_view game_data_changelist = "TODO"; // TODO(25617022): read from Data\layout.toc
// How many world layers this build's levels have: the catalog players scan from their game and
// a dedicated server reads from world-layers.json. A session's layers are sent as one choice for
// each, in the catalog's order, so both ends must have the same catalog. (2.0.8's server was
// released with the catalog of a game Steam had updated, 263 layers, and no player could join a
// server that shared its layers: 2026-10-13.)
inline constexpr std::size_t world_layer_count = 263; // TODO(25617022): confirm by scanning this build
// The live game's HTTP content cache for this build (catalogue chunks and CDN
// images), published as a ReSkateCache release and installed by the launcher.
// TODO(25617022): needs a ReSkateCache release from a maintainer.
inline constexpr std::wstring_view content_cache_url =
    L"https://github.com/Dingo-Shenanigans/ReSkateCache/releases/download/25617022/25617022.zip";
inline constexpr std::string_view content_cache_sha256 =
    "TODO-content-cache-sha256";
inline constexpr std::uint64_t content_cache_bytes = 0; // TODO

consteval std::array<unsigned char, 32> sha256_bytes(std::string_view text) {
    if (text.size() != 64) throw "SHA-256 must have exactly 64 hexadecimal digits";
    const auto nibble = [](char value) -> unsigned char {
        if (value >= '0' && value <= '9') return static_cast<unsigned char>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<unsigned char>(value - 'a' + 10);
        throw "SHA-256 must use lowercase hexadecimal digits";
    };
    std::array<unsigned char, 32> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
    return result;
}

inline constexpr auto game_sha256_bytes = sha256_bytes(game_sha256);
inline constexpr auto steam_api_sha256_bytes = sha256_bytes(steam_api_sha256);

} // namespace dingosdk::supported_build
