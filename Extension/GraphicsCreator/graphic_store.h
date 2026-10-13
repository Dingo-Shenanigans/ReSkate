#pragma once

#include "graphic_document.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::graphics_creator {

struct LoadedGraphic {
    GraphicDocument document;
    bool recovered_from_backup{};
};

bool valid_file_name(std::string_view name);
void save(const std::filesystem::path& directory, std::string_view name, const GraphicDocument& document);
LoadedGraphic load(const std::filesystem::path& directory, std::string_view name);
std::vector<std::string> list(const std::filesystem::path& directory);

} // namespace dingosdk::graphics_creator
