#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace dingosdk::graphics_creator {

struct ProjectSlotResult {
    bool attempted{};
    bool authored{};
    std::filesystem::path project;
    std::filesystem::path encoded_dds;
    std::string message;
};

// Builds a new Studio project from a separately-authored, hash-pinned board
// slot. The source template is never modified.
ProjectSlotResult build_board_project_slot(const std::filesystem::path& source_png,
                                           const std::filesystem::path& output_project);

// Kept public for focused format tests. Production exports call this through
// build_board_project_slot.
std::string sha256_file_hex(const std::filesystem::path& path);
void patch_board_project_slot(const std::filesystem::path& descriptor,
                              const std::filesystem::path& encoded_dds,
                              const std::filesystem::path& output_project);

} // namespace dingosdk::graphics_creator
