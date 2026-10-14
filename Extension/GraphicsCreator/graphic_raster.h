#pragma once

#include "graphic_assets.h"
#include "graphic_document.h"
#include <cstdint>
#include <filesystem>

namespace dingosdk::graphics_creator {

// Flattens an editable document without mutating it. Large canvases are fitted to maximum_side.
ImagePreview rasterize(const GraphicDocument& document,
                       const std::filesystem::path& asset_directory,
                       std::uint32_t maximum_side = 2048);

// Writes a complete PNG through WIC, replacing an older export atomically.
void export_png(const std::filesystem::path& destination, const ImagePreview& image);

} // namespace dingosdk::graphics_creator
