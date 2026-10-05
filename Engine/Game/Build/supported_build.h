#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// This is the accepted contract identity, not a list of hashes to bypass.
// Advance it only together with the reviewed native/asset contracts in
// Engine/Game/Build/<build>/. Diagnostic tools with historical addresses
// intentionally retain their own historical build pins.
namespace dingosdk::supported_build {

inline constexpr std::uint64_t game_file_size = 144567168;
inline constexpr std::uint32_t game_image_size = 0x09144000;
inline constexpr std::string_view game_sha256 =
    "fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9";
inline constexpr std::uint64_t steam_api_file_size = 300928;
inline constexpr std::string_view steam_api_sha256 =
    "88b55dc33bfe9d8f998b72bf7074ebe0d21318a0b6dfdcca3530cb1e0c102a39";
// The Steam content that ships the files above; the launcher config repeats these
// identifiers to describe the supported build.
inline constexpr std::uint32_t steam_app_id = 3354750;
inline constexpr std::uint32_t steam_depot_id = 3354751;
inline constexpr std::string_view steam_manifest_id = "4621099302092747785";
inline constexpr std::string_view steam_build_id = "25414733";
// The live game's HTTP content cache for this build (catalogue chunks and CDN
// images), published as a ReSkateCache release and installed by the launcher.
inline constexpr std::wstring_view content_cache_url =
    L"https://github.com/Dingo-Shenanigans/ReSkateCache/releases/download/25414733/25414733.zip";
inline constexpr std::string_view content_cache_sha256 =
    "01867746b29301eead35a473ae523f8c84aa43784c244fcfafe0057a113045f4";
inline constexpr std::uint64_t content_cache_bytes = 9296380;

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
