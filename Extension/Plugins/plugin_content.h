#pragma once
#include "plugin_game.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// mods.content and read_content, from the mods' folders on disk: the game reads its enabled mods
// (plugin_game_runtime.cpp), a dedicated server every mod folder in its Mods (server_plugins.cpp).
// Both keep the same limits, so a plugin reads the same files on either.
namespace dingosdk::plugins::content {
struct Mod {
    std::string name, title; // the folder's name and the mod's own name
    std::filesystem::path directory;
};
inline constexpr std::size_t most_files = 1000, most_bytes = 1u << 20;

// Every folder in `mods_root` a mod can be, by name: not hidden (".reskate_plugin_data"), and a
// name a plugin can ask for again. Titled by its manifest.json's "name", else its folder's name.
std::vector<Mod> mod_folders(const std::filesystem::path& mods_root);
// The files under <mod>/<folder>/ of each of `mods` in turn, sorted within a mod, four folders deep
// at most and most_files in all; `folder` is checked against the manifest already.
void list(const std::vector<Mod>& mods, std::string_view folder, std::vector<Game::ContentFile>& files);
// One of them, at most most_bytes; `path` is checked already (safe_relative_path). Empty when read,
// else why not.
std::string read(const std::vector<Mod>& mods, std::string_view mod, std::string_view folder, std::string_view path, std::string& text);
} // namespace dingosdk::plugins::content
