#pragma once
#include "Engine/Game/World/park_editor.h"
#include "Extension/Objects/object_placements.h"

namespace dingosdk {
ParkEditorModel local_park_editor();
// Whether a menu shows the park list (the park editor, or World > Build). Listing the Mods folder
// walks the disk, so local_park_editor rescans it only while one does.
void set_park_mod_list_visible(bool visible) noexcept;
// Whether the local player may place objects under the lobby host's policy.
// Gates the park editor and the game's own native create/copy requests. Reset
// on disconnect; existing placed objects remain in the world.
void set_lobby_object_placement_allowed(bool enabled);
bool lobby_object_placement_allowed() noexcept;
// The session's limit on this player's objects (object_placement.h), 0 for none: the host's
// own game and a server admin's are given none. Adding an object is refused at the limit.
void set_lobby_object_limit(unsigned limit) noexcept;
unsigned lobby_object_limit() noexcept;
// Objects this player has on the current map, and whether one more would pass the limit.
std::size_t lobby_object_count();
bool lobby_object_limit_reached();
inline constexpr char object_limit_notice[] = "You have placed as many objects as this session allows each player.";
bool queue_local_park_surface(const EditorSurfaceRequest &);
bool queue_local_park_preview(const EditorPreviewRequest &);
bool queue_local_park_selection(const EditorSelectionRequest &);
bool queue_local_park_paste(const EditorPasteRequest &);
void tick_local_park_editor() noexcept;
// Invoked through the console's verified game-thread dispatcher. `texts`
// carries a park mod's name, author, version and description (mod-create:
// all four; mod-details: the same after the folder in `argument`).
std::string edit_local_park(std::string_view operation, std::string_view map, std::uint64_t generation,
                            std::uint64_t revision, std::string_view argument,
                            const profile::PlacedObject &object = {},
                            const std::vector<std::string> &texts = {});
} // namespace dingosdk
namespace dingosdk::profile_runtime {
void initialize_park_editor(const std::filesystem::path &directory);
// The engine data root whose Mods folder holds park mods.
void set_park_mods_root(const std::filesystem::path &data_root);
void update_park_editor();
void update_park_editor_moves() noexcept;
void reset_park_editor();
bool park_editor_owns_placements();
bool park_editor_observe(std::uint64_t entity, const profile::PlacedObject &object);
void park_editor_external_change();
} // namespace dingosdk::profile_runtime
