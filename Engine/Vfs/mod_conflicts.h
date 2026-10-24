#pragma once

#include "mod_catalog.h"
#include "mod_merge.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::mods {

// Which enabled mods conflict (MergeReport::conflicts): two mods that change one of the game's EBX
// in ways that cannot be combined, or one of its Lua scripts differently. Worked out from the
// mods' asset lists without merging, by the rule the merge itself applies. What it reads of each
// mod and of the game is kept, so a check after a mod was moved, turned on or turned off reads
// only what is new: the launcher's mod list checks after every change. A mod whose files changed
// is read again. No file is held open between checks. One check at a time.
class ConflictChecker {
public:
    // `data_root`: the game folder, holding Data and Mods.
    explicit ConflictChecker(std::filesystem::path data_root);
    ~ConflictChecker();
    ConflictChecker(const ConflictChecker&) = delete;
    ConflictChecker& operator=(const ConflictChecker&) = delete;

    // `mods`: the enabled ones, highest priority first. On up to `threads` threads beside the
    // calling one (`background`: under normal priority). Never throws: what cannot be read is
    // left out, as the merge's own check catches it.
    [[nodiscard]] std::vector<MergeReport::Conflict> check(const std::vector<const Mod*>& mods, std::size_t threads = 0,
                                                           bool background = false) noexcept;

    struct State;   // what has been read (mod_merge_conflicts.cpp)

private:
    std::unique_ptr<State> state_;
};

// A mod the launch leaves out for conflicts: the first mod it lost to, and every row it lost.
struct ConflictLoss {
    std::string winner;
    std::vector<const MergeReport::Conflict*> rows;   // pointing into the conflicts given
};
// Which of the mods in `order` (folder names, highest priority first) the launch leaves out for
// `conflicts`: from the top down, a mod that lost to one still loading. One that lost only to a
// mod itself left out stays in. `leaving`: mods left out already, for other reasons; `exempt`:
// mods never left out for this (ReSkate's own packs). By folder name.
std::map<std::string, ConflictLoss, std::less<>> conflict_losses(const std::vector<std::string>& order,
    const std::vector<MergeReport::Conflict>& conflicts, const std::set<std::string, std::less<>>& leaving = {},
    const std::function<bool(const std::string&)>& exempt = {});

// An EBX type that is only how something looks: an appearance preset (a colourway), a texture, a
// mesh. Two mods' versions of one are no conflict: the higher mod's is kept, and both mods still
// work, the lower one with the other's look on that one item.
bool content_type(std::string_view root_type);

// The mod a mod left out of the launch lost a conflict to (its first problem names it, as the
// catalog writes it), or empty when it was left out for something else.
std::string conflict_winner(const Mod& mod);

// What part of the game an asset is, in a player's words, by where it is: "menus", "skater
// setup", "scripts", "items"...
std::string asset_area(std::string_view asset);

} // namespace dingosdk::mods
