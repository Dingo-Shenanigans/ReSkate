#pragma once
// Gameplay/SkatePhysicsTuning as the game's data defines it, and the differences a host
// sends so its guests skate with its tuning (Extension/Skater/physics_tuning.h). No game
// process is needed here: the tests and the dedicated server use it as is.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dingosdk::physics_tuning {
// One plain value of the asset: the only bytes differences may write (never pointers,
// strings, the asset's name or its object header).
struct Field {
    std::uint16_t offset{}, size{};
    bool real{}; // float32: must stay finite
    bool flag{}; // boolean: 0 or 1
    std::string name; // the game's name for it, dotted through its groups: "PhysicsPush.MaxPushableSpeed"
};
// A FloatCurve's points, laid out as the game keeps them (curve_point_size bytes each).
struct Curve {
    float min{}, max{};
    std::vector<std::uint8_t> points;
    bool operator==(const Curve &) const = default;
};
struct Model {
    std::vector<std::uint8_t> image;          // the whole asset (asset_size bytes)
    std::vector<Field> fields;                // by offset, not overlapping
    std::vector<std::uint16_t> curve_slots;   // offsets of the curve pointers, ascending
    std::vector<std::optional<Curve>> curves; // per slot: the game's own curve
};
// One copy of the asset's values: the game's, a player's, or what a guest should have.
struct Values {
    std::vector<std::uint8_t> image;
    std::vector<std::optional<Curve>> curves; // per model slot; nothing = unknown (left as it is)
};
// The game's own tuning, read from the installed game's Data folder (never the mods).
Model read_game_tuning(const std::filesystem::path &game_root);

// Differences, little endian: u8 'T', u8 version 1; u16 runs {u16 offset, u16 size, bytes};
// u16 curves {u16 slot offset, u16 points, f32 min, f32 max, points}. Empty = none.
struct Encoded {
    std::vector<std::uint8_t> bytes;
    std::size_t runs{}, curves{}, left_out{}; // left_out: curves that did not fit in `limit`
};
Encoded encode_differences(const Model &, const Values &live, std::size_t limit);
// The game's values (curves included) with `differences` applied, or nothing when they are malformed or
// write outside the model's fields and curves. Reals that are not finite and flags other
// than 0 and 1 keep the game's value.
std::optional<Values> apply_differences(const Model &, std::span<const std::uint8_t> differences);
} // namespace dingosdk::physics_tuning
