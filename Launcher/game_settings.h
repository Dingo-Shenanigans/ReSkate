#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// skate.'s own settings, edited from the launcher. The game keeps the ones it saves per PC in
// save containers under its data folder (%LOCALAPPDATA%\ReSkate\Game\Skate\data, where ReSkate
// points it): each container is a DATA file of sections and an INDEX naming them. The "body"
// section is one binary object, a setting a named value: on/off, a whole number, a number or
// text. Its "TierManagerUserSettings" text holds the graphics options as
// "Name;value|Name;value". Only the values asked for are rewritten; every other byte of a
// container is kept.
namespace dingosdk::launcher_game_settings {

using Value = std::variant<bool, std::int32_t, float, std::string>;
using Values = std::map<std::string, Value, std::less<>>;   // by the game's setting key

struct Container {
    std::vector<unsigned char> data, index;
};

// Every setting of those four kinds a container holds. Throws when it is not a container of
// the game's settings, or its format is not the one this was written for.
Values read_values(const Container& container);
// That container with `changes` stored. Each must be a setting the container already holds,
// of the same kind.
Container with_values(const Container& container, const Values& changes);

// ---------------------------------------------------------------- graphics options

inline constexpr char options_key[] = "TierManagerUserSettings";
struct Option {
    std::string name, value;   // "Texture Quality", "texture_quality_ultra"
    bool operator==(const Option&) const = default;
};
using Options = std::vector<Option>;   // in the game's order
// "Name;value|Name;value" and back.
Options parse_options(std::string_view text);
std::string join_options(const Options& options);

// What the launcher offers for one option: the game's own value names, with labels.
struct Choice { const char* value; const char* label; };
struct Setting {
    const char* category;
    const char* name;      // the option's name in the game
    const char* label;
    std::span<const Choice> choices;
    // Only matters while this other option has this value ("" : always).
    const char* needs_name;
    const char* needs_value;
};
std::span<const Setting> graphics_options();
// The value of `level` (0 low .. 3 ultra) for an option that has those four, else null.
const char* level_value(const Setting& setting, int level);
// Any change makes the game's preset "Custom".
inline constexpr char preset_name[] = "Global Graphics Quality";
inline constexpr char preset_custom[] = "Custom";

// ---------------------------------------------------------------- every other setting

// The game's settings menus, as its data defines them (DingoUISettingAsset: key, kind, range
// or choices), for the settings it saves per PC.
enum class Kind { toggle, whole, real, choice, resolution };
struct Named { int value; const char* label; };
struct Item {
    const char* page;      // the launcher's tab
    const char* section;
    const char* key;
    const char* label;
    Kind kind;
    float least, most, step;            // whole, real
    std::span<const Named> choices;     // choice
};
std::span<const Item> items();

// The resolutions the game's Resolution setting counts through: this PC's display modes,
// smallest first.
std::vector<std::pair<int, int>> display_resolutions();

// ---------------------------------------------------------------- on disk

// Where the game keeps its saves.
std::filesystem::path save_root();
// Every container under `root` that holds the settings, and the values of the one written last.
struct Saved {
    std::vector<std::filesystem::path> folders;
    Values values;
};
Saved load(const std::filesystem::path& root);
// Stores `changes` in each of those containers. The first time, the untouched files are copied
// to `backup` (a folder per container). Throws when a file cannot be read or written.
void save(const Saved& saved, const Values& changes, const std::filesystem::path& backup);

} // namespace dingosdk::launcher_game_settings
