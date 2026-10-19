#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dingosdk {
enum class EditorPreviewAction { begin_move, begin_place, update, commit, cancel };
struct EditorTransform {
    std::uint64_t id{};
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    float scale{1};
};
struct EditorPreviewRequest {
    EditorPreviewAction action{};
    std::uint64_t generation{}, revision{}, token{}, sequence{}, object{};
    std::string item;
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    float scale{1};
    std::vector<EditorTransform> transforms;
};
struct EditorSurfaceRequest {
    std::uint64_t generation{}, id{};
    std::array<float, 3> origin{}, direction{};
    float grid{};
    bool picking{};
};
struct EditorSelectionRequest {
    std::uint64_t generation{};
    std::vector<std::uint64_t> objects;
};
struct EditorSurfaceHit {
    std::uint64_t generation{}, id{};
    bool available{}, hit{};
    std::array<float, 3> position{};
    std::uint64_t entity{}, object{};
};
struct EditorAsset {
    std::string key, title, category;
};
struct EditorObject {
    std::uint64_t id{};
    std::string item;
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    bool spawned{};
    float scale{1};
};
struct EditorPasteRequest {
    std::uint64_t generation{}, revision{};
    std::vector<EditorObject> objects;
};
// Removes several objects in one edit (one undo step), as a paste adds several.
struct EditorDeleteRequest {
    std::uint64_t generation{}, revision{};
    std::vector<std::uint64_t> objects;
};
// A saved set of objects (<profile>/object_sets/<name>.park.json), placed again as one paste on any
// map. Positions are relative to the set's anchor: the middle of its footprint, at its lowest origin.
struct EditorObjectSet {
    std::string name, map; // map: where the set was saved, for display only
    std::vector<EditorObject> objects;
};
enum class EditorObjectSetAction { save, remove };
struct EditorObjectSetRequest {
    EditorObjectSetAction action{};
    std::uint64_t generation{};
    std::string name;
    std::vector<EditorObject> objects; // save: the objects in world space
};
// A mod with a parks/ folder (Mods/<folder>/parks/<map>.park.json).
struct EditorParkMod {
    std::string folder, title, author, version, description;
    bool enabled{}, has_map{}; // has_map: it has a park for the current base map
    std::vector<std::string> maps;
};
struct ParkEditorModel {
    bool available{}, busy{}, failed{}, can_undo{}, can_redo{};
    std::uint64_t generation{}, revision{};
    std::string map, status;
    std::vector<EditorObject> objects;
    std::shared_ptr<const std::vector<EditorAsset>> assets;
    std::vector<EditorParkMod> park_mods;
    std::vector<std::string> legacy_parks; // parks saved before park mods; convertible
    std::string project;                   // mod folder the editor saves to; empty = unsaved
    bool can_load_parks{};                 // false for multiplayer guests: only the host loads parks
    EditorSurfaceHit surface;
    std::uint64_t preview_token{};
    std::shared_ptr<const std::vector<EditorObjectSet>> object_sets;
};
} // namespace dingosdk
