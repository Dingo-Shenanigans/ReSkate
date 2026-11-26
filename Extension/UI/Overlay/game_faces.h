#pragma once
#include <cstddef>
#include <filesystem>

// The faces skate.'s own UI writes in, for plugins' texts (style.font "game_label", "game_value" and "game_title":
// plugin_ui.h's game_faces): read from the installed game's data (Engine/Game/Build/20260929/ui_textures.h) into
// ReSkate's cache once, and drawn from there as a plugin's own font files are (plugin_fonts.h: Windows reads them).
// Nothing of the game's is shipped.
namespace dingosdk::overlay::game_faces {
// The file the face is kept in, whether it is there yet or not; empty when there is no cache folder.
std::filesystem::path file(std::size_t face);
// Their reading started on a thread of its own, once: each face not in its file yet is read from the game into it.
void prepare() noexcept;
}
