#pragma once

#include "graphic_document.h"
#include <filesystem>
#include <string>

namespace dingosdk::graphics_creator {

struct NativeExportResult {
    bool attempted{};
    bool validated{};
    std::filesystem::path manifest;
    std::filesystem::path encoded_preview;
    std::string message;
};

// Validates the flattened artwork through ReSkate Studio's native Frostbite
// texture encoder. Project authoring, FBMOD packaging and live application are
// deliberately separate steps until Studio exposes that authoring operation.
NativeExportResult stage_native_export(const GraphicDocument& document,
                                       const std::filesystem::path& exported_png);

} // namespace dingosdk::graphics_creator
