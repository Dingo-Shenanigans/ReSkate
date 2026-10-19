#pragma once
#include "Engine/Game/World/park_editor.h"
#include "Extension/Objects/object_placements.h"
#include <filesystem>
#include <string_view>
#include <vector>

// Saved object sets: a selection kept under a name and placed again later, on any map.
// Each set is a park file (park_document.h) in its own folder, so the format, the size limit
// and the name rules are the park's. Positions are relative to the set's anchor.
namespace dingosdk::editor {
inline constexpr std::size_t max_object_sets = 256;
// Moves the objects so the anchor (the middle of their footprint, at their lowest origin) is at 0.
profile::ObjectLayout anchored_set(profile::ObjectLayout objects);
void save_object_set(const std::filesystem::path &directory, std::string_view name, std::string_view map,
                     const profile::ObjectLayout &objects);
void remove_object_set(const std::filesystem::path &directory, std::string_view name);
// Every readable set, by name. A file that does not decode is left out.
std::vector<EditorObjectSet> load_object_sets(const std::filesystem::path &directory);
} // namespace dingosdk::editor
