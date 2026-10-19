#pragma once
#include "park_editor_runtime.h"
#include "park_editor_highlight.h"
#include "park_editor_picking.h"
#include "park_editor_surface.h"
#include "park_mods.h"
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Editor state and helpers shared by the park_editor_*.cpp files.
namespace dingosdk::profile_runtime::park_editor_detail {
// Native dropper movement updates the existing physics body and transform
// (addr::park_editor::movement_contracts). Message 2f51fbd9 is COPY: never use
// it to move an existing entity.
enum class Kind { create, move, erase };
struct Step {
    Kind kind{};
    profile::PlacedObject object;
    std::uint64_t entity{};
    bool acknowledged{};
    std::set<std::uint64_t> before_entities;
    bool identities_captured{};
    bool live_dirty{true};
};
struct Change {
    profile::ObjectLayout before, after;
};
struct Preview {
    std::uint64_t token{}, revision{}, sequence{}, last_sample{};
    profile::PlacedObject target;
    bool placing{}, ending{}, cancelled{};
    std::vector<std::uint64_t> selected;
    std::set<std::uint64_t> identities;
    bool identities_captured{};
};
struct Transaction {
    Change change;
    std::deque<Step> steps;
    int history{}; // -1 undo, 0 normal, 1 redo
    bool sent{};
    std::uint64_t sent_at{}, waiting_since{};
    std::optional<Preview> preview;
};
struct EditorRuntime {
    std::filesystem::path directory;
    std::uint64_t generation{1}, revision{1}, catalog_revision{UINT64_MAX}, next_catalog{};
    std::uintptr_t catalog_manager{};
    std::shared_ptr<const std::vector<EditorAsset>> assets;
    // Park mods under <data_root>/Mods, rescanned every few seconds and after
    // every mod operation; parks saved before mods existed stay convertible.
    std::filesystem::path data_root;
    std::vector<editor::ParkMod> park_mods;
    std::vector<std::string> legacy_parks;
    std::string project;
    std::uint64_t next_park_scan{};
    // Saved object sets (park_object_sets.h): read at startup and again after every change.
    std::filesystem::path sets_directory;
    std::shared_ptr<const std::vector<EditorObjectSet>> object_sets;
    std::deque<Change> undo, redo;
    std::optional<Transaction> transaction;
    bool failed{}, native_ready{};
    std::string status;
    void *(*physics_query)(const void *, void *){};
    void (*physics_move)(const void *, const float *){};
    void (*transform_move)(const void *, const float *, std::uintptr_t){};
    editor::NativeSurfaceApi surface_api;
    editor::NativePickingApi picking_api;
    std::optional<EditorSurfaceRequest> probe;
    EditorSurfaceHit surface;
    std::uint64_t last_preview_token{};
    editor::NativeHighlightApi highlight_api;
    struct HighlightLease {
        editor::HighlightValue original, applied;
    };
    std::map<std::uint64_t, HighlightLease> highlights;
    std::vector<std::uint64_t> selection;
    std::uint64_t selection_time{};
    std::map<std::uint32_t, std::uint64_t> native_drops;
    std::map<std::uint64_t, std::uint64_t> client_roots;
    std::optional<Step> late_create;
};
EditorRuntime &editor_state();
bool same_layout(const profile::ObjectLayout &a, const profile::ObjectLayout &b);
const profile::PlacedObject *find_object(const profile::ObjectLayout &objects, std::uint64_t id);
void refresh_park_mods();
void refresh_object_sets();
bool is_lobby_guest();
bool idle();
void fail(std::string reason);
bool begin(profile::ObjectLayout target, int history);

// park_editor_preview.cpp
void update_preview_target(Transaction &tx);
void cancel_preview(Transaction &tx);
void expire_preview();

// park_editor_selection.cpp
bool surface_body(std::uintptr_t world, std::uint32_t index, std::uintptr_t data, editor::SurfaceBody &body);
std::uint64_t surface_owner(std::uintptr_t world, std::uint32_t index, std::uintptr_t data);
std::uint64_t client_root(std::uint64_t server);
void update_highlights();
} // namespace dingosdk::profile_runtime::park_editor_detail
