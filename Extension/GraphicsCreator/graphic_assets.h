#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
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
ImagePreview load_image(const std::filesystem::path& asset_directory,
                        const std::string& asset_name);
ImportedImage create_text_image(const std::filesystem::path& asset_directory,
                                std::string_view text, std::wstring_view font_face);
ImportedImage import_embedded_image(const std::filesystem::path& asset_directory,
                                    const wchar_t* resource_name);

} // namespace dingosdk::graphics_creator
