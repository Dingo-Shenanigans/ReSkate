#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dingosdk::graphics_creator {

struct ImportedImage {
    std::string asset_name;
    std::uint32_t width{};
    std::uint32_t height{};
};

struct ImagePreview {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

ImportedImage import_image(const std::filesystem::path& asset_directory,
                           const std::filesystem::path& source);
ImagePreview load_preview(const std::filesystem::path& asset_directory,
                          const std::string& asset_name,
                          std::uint32_t maximum_side = 32);

} // namespace dingosdk::graphics_creator
