#pragma once

#include "Engine/Resource/texture.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// How much video memory a mod's own textures and meshes take once the game has them loaded, read
// from the mod's files and the installed game: what the launcher's mod manager says of each mod on
// its row. An estimate: it counts what the mod ships (a texture's
// mips as its header gives them, a mesh's vertex and index buffers), not what the game makes of
// them at run time (render targets, shadows, what the streamer leaves out when memory is short).
namespace dingosdk::mods {

struct ModVram {
    // A map: what the heaviest of its levels holds, every mip of every texture loaded, and of that
    // what stays loaded wherever the skater is (the mips its textures keep with their bundles).
    // Only one level is loaded at a time, so a mod with several maps costs its heaviest.
    std::uint64_t level{}, level_resident{};
    // Game data, in bundles the game keeps loaded (a pack's thumbnails, a changed world texture).
    // A texture of the game's own that the mod replaces counts for what it adds over the game's.
    std::uint64_t always{};
    // Game data in bundles the game loads when what is in them is used (an item's look, loaded
    // while a skater wears it: the bundles named "..._bundlereftable"): all of them together, the
    // heaviest one, and how many there are.
    std::uint64_t on_demand{}, on_demand_largest{};
    std::uint32_t on_demand_bundles{};
    std::uint32_t textures{}, meshes{};   // counted, each once
    std::uint32_t unread{};               // that could not be read, and count for nothing
    std::string problem;                  // why the mod could not be read, when it could not

    // What it can take at most while one of its maps is loaded and all it adds is in use.
    [[nodiscard]] std::uint64_t most() const noexcept { return level + always + on_demand; }
    bool operator==(const ModVram&) const = default;
};

// A texture's bytes on the graphics card: with every mip loaded, and with only the mips its bundle
// keeps (from `first_mip`; the sharper ones stream in as they are needed).
struct TextureBytes {
    std::uint64_t whole{}, resident{};
};
[[nodiscard]] TextureBytes texture_bytes(const frostbite::TextureHeader& header) noexcept;

// Whether a bundle of the game's (its name, in lower case) is one loaded only while what it holds is
// in use.
[[nodiscard]] bool on_demand_bundle(std::string_view name) noexcept;

// What `directory` (a mod's folder, or one of its variants') costs. `stop`, when given and set,
// ends the read early with a problem. Never throws. Reads every texture header and mesh of the
// mod: seconds for a big map, so for a worker thread.
[[nodiscard]] ModVram read_mod_vram(const std::filesystem::path& data_root, const std::filesystem::path& directory,
                                    const std::atomic<bool>* stop = nullptr) noexcept;

// One line of text holding `vram`, and back: how the launcher keeps a mod's between runs. Nothing
// comes back for a line this does not read.
[[nodiscard]] std::string vram_line(const ModVram& vram);
[[nodiscard]] bool parse_vram_line(std::string_view line, ModVram& vram) noexcept;

} // namespace dingosdk::mods
