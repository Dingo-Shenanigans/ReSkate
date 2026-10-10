#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// The launcher's side of the Mods folder: installing mods from a .zip or a
// folder, and removing them. Order and enabled state live in mods.json
// (Engine/Vfs/mod_list.h), which the runtime reads when the game starts.
namespace dingosdk::launcher_mods {

// The Mods folder the game will read: <game>/ModData/Default/Mods when the
// game is started with that data path (a ModData folder exists), else <game>/Mods.
std::filesystem::path mods_root(const std::filesystem::path& game_directory);

// Thrown when a mod with the same folder name is already installed and the
// caller did not ask to replace it.
struct AlreadyInstalled : std::runtime_error {
    explicit AlreadyInstalled(std::string folder)
        : std::runtime_error("A mod named \"" + folder + "\" is already installed."), name(std::move(folder)) {}
    std::string name;
};

using Progress = std::function<void(float fraction)>;

// One launcher filesystem operation at a time, including the entire download,
// merge or launch worker. The mod panel retains its lease until worker join.
enum class Operation { install, reset, launcher };
class OperationLease {
public:
    explicit OperationLease(Operation operation);
    ~OperationLease();
    OperationLease(OperationLease&& other) noexcept;
    OperationLease(const OperationLease&) = delete;
    OperationLease& operator=(const OperationLease&) = delete;
    explicit operator bool() const { return held_; }
    bool permits(Operation operation) const { return held_ && operation_ == operation; }
    static bool busy();
private:
    Operation operation_;
    bool held_{};
};

struct ResetIdentity {
    std::uint64_t volume{}, file{};
    bool operator==(const ResetIdentity&) const = default;
};
struct ResetTarget {
    std::filesystem::path path;
    std::uintmax_t estimated_bytes{};
    ResetIdentity identity;
};
struct ResetPlan {
    std::filesystem::path game_directory;
    ResetIdentity game_identity;
    std::vector<ResetTarget> targets;
    std::vector<std::string> excluded, errors;
    std::uintmax_t estimated_bytes{}; // logical sizes; hard links can overestimate freed space
    std::size_t mod_count{};
};
struct ResetResult {
    bool success{};
    std::size_t removed_items{};
    std::uintmax_t removed_bytes{};
    std::vector<std::string> errors;
    std::vector<std::filesystem::path> remaining;
};

// Read-only inspection of the two known layouts. Never follows reparse points.
ResetPlan prepare_factory_reset(const std::filesystem::path& game_directory,
                                const OperationLease* lease = nullptr);
// Permanent, handle-based deletion; revalidates the plan and checks Skate
// processes again. Progress is indeterminate (-1) while the tree can change.
ResetResult execute_factory_reset(const ResetPlan& plan, const Progress& progress = {},
                                  const OperationLease* lease = nullptr,
                                  const std::function<void(const std::string&)>& activity = {});
// Conservatively blocks reset if any Skate.exe is running, including games
// started outside this launcher. Returns a user-facing reason, or empty.
std::string factory_reset_blocker();

struct InstallOptions {
    // Install under this folder name instead of one taken from the source
    // (Thunderstore packages: Namespace-Name).
    std::string folder;
    // Written into manifest.json as "author" when it names none (Thunderstore
    // manifests have no author; the team is the author).
    std::string author;
    // Refuse a source with nothing the game loads, unless its manifest.json
    // lists dependencies (a mod pack).
    bool require_content{};
};

// Installs `source` (a .zip, or a folder) as Mods/<name> and returns <name>.
// The mod is the shallowest folder holding layout.toc, reskate-levels.json or
// parks/<map>.park.json, else the shallowest holding manifest.json or
// reskate-mod.json. Thunderstore's manifest.json, icon.png, README.md and
// CHANGELOG.md at the top of a package are copied in when the mod sits one
// folder deeper. A top-level mod takes the archive's name without a trailing
// version ("Team-Map-1.0.0.zip" installs as Team-Map). With `replace`, an
// installed mod of the same name goes to the Recycle Bin first. Throws a
// user-facing message; a failed or cancelled install leaves the Mods folder as it was.
std::string install(const std::filesystem::path& mods_root, const std::filesystem::path& source, bool replace,
                    const Progress& progress, const std::atomic<bool>& cancel, const InstallOptions& options = {},
                    const OperationLease* lease = nullptr);

// Moves Mods/<name> to the Recycle Bin. Throws a user-facing message.
void remove(const std::filesystem::path& mods_root, const std::string& name);

} // namespace dingosdk::launcher_mods
