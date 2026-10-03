#pragma once
// Gameplay/SkatePhysicsTuning is one asset that every skater's physics in the process reads,
// so a player who edits theirs (truck positions, jump heights, ...) skates differently, and
// everyone sees it. In a session a host's differences from the game's own tuning go to its
// guests, who skate with the game's tuning plus those; a dedicated server's guests skate
// with the game's. Client thread only.
#include "physics_tuning_model.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::physics_tuning {
// Reads the game's own tuning from the game data, once, in the background.
void prepare() noexcept;
// Host: how the running game's tuning differs from the game's own, in at most `limit` bytes;
// nothing while either is unknown (still reading, or this build's tuning does not match).
std::optional<Encoded> local_differences(std::uintptr_t base, std::size_t limit);
// Guest, each client tick in a world: keeps the game's tuning plus `differences` in the
// running game (checked about once a second, at once when they change), and refreshes the
// local skater's cached copy of the values after a change. Masses and collision sizes a
// skater's rig was built with change on its next respawn.
void enforce(std::uintptr_t base, std::uintptr_t entity, std::span<const std::uint8_t> differences) noexcept;
// Puts the player's own values back (the ones from before enforce). Cheap when nothing is enforced.
void release(std::uintptr_t base) noexcept;
// What enforcement is doing, for the session UI and the log; empty when off.
std::string status();

// The player's own edits from the Skater > Physics menu. The running game's values are changed
// in place (the same way a host's are enforced on a guest) and the local skater's cached copy is
// refreshed, so a slider is felt at once. Edits are kept and put back after a level load or a
// respawn, and stand aside while a host's tuning is enforced. Only plain numbers and flags are
// offered; the tuning curves are not.
namespace live {
struct Param {
    std::string path;  // the game's name: "PhysicsPush.MaxPushableSpeed"
    std::string group; // "PhysicsPush"
    std::string name;  // "MaxPushableSpeed"
    std::uint16_t offset{};
    bool flag{};  // a checkbox, not a number
    float game{}; // the game's own value (not any mod's)
};
struct Snapshot {
    bool ready{};  // the game's tuning and the running copy are both known
    bool locked{}; // a host's tuning is in use; edits wait for the session to end
    std::string status; // why not ready; empty otherwise
    std::shared_ptr<const std::vector<Param>> params; // by offset
    std::vector<float> values;                        // per param: the running game's value
    std::vector<std::string> presets;
    std::size_t changed{}; // params whose running value differs from the game's
};
// Any thread, cheap: the menu calls it every frame it is drawing (that is also what makes the
// client thread keep the values fresh).
Snapshot snapshot();
// Any thread: ask for a value. Applied on the next client tick; flags take 0 or 1.
void set(std::uint16_t offset, float value);
// Any thread: every value this menu has changed goes back to the game's own.
void reset_all();
// Any thread. Presets hold the values that differ from the game's own. Loading one first puts
// every other value back to the game's. False when it could not be done.
bool save_preset(std::string_view name);
bool load_preset(std::string_view name);
bool delete_preset(std::string_view name);
// Client thread, each tick, with the local skater's entity (0 when there is none).
void tick(std::uintptr_t base, std::uintptr_t entity) noexcept;
} // namespace live
} // namespace dingosdk::physics_tuning
