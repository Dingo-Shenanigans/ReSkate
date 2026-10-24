// Checks a data root's enabled mods for conflicts the way the launcher's mod list does
// (mods::ConflictChecker): once, again with nothing changed, and again with the load order
// reversed, saying how long each took and which mods each would leave out. The second check
// has to find what the first did, and both later ones have to be quick: nothing is read again.
// First argument: a folder holding the game's Data/ and a Mods/ folder. Skips when none is given.
#include "Engine/Vfs/mod_conflicts.h"
#include "Engine/Vfs/mod_list.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace mods = dingosdk::mods;

namespace {
void say(const char* what, const std::vector<std::string>& order, const std::vector<mods::MergeReport::Conflict>& conflicts,
         std::chrono::steady_clock::duration took) {
    std::cout << what << ": " << conflicts.size() << " conflicting pair(s) in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms\n";
    for (const auto& [lost, loss] : mods::conflict_losses(order, conflicts)) {
        std::string areas;
        for (const auto& area : loss.rows.front()->areas) areas += (areas.empty() ? "" : ", ") + area;
        std::cout << "  left out: " << lost << " (loses to " << loss.winner << ": " << areas << ")\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::filesystem::exists(std::filesystem::path(argv[1]) / "Data")) {
        std::cout << "No data root given; skipped.\n";
        return 0;
    }
    const std::filesystem::path root(argv[1]);
    const auto list = mods::scan_mods(root);
    std::vector<const mods::Mod*> enabled;
    for (const auto& entry : list.entries)
        if (entry.enabled && entry.mod.provides_layout && entry.mod.outdated.empty()) enabled.push_back(&entry.mod);
    const auto threads = std::max(1U, std::thread::hardware_concurrency()) - 1;
    mods::ConflictChecker checker(root);
    const auto timed = [&](const std::vector<const mods::Mod*>& order, const char* what) {
        std::vector<std::string> names;
        for (const auto* mod : order) names.push_back(mod->name);
        const auto start = std::chrono::steady_clock::now();
        auto conflicts = checker.check(order, threads);
        say(what, names, conflicts, std::chrono::steady_clock::now() - start);
        return conflicts;
    };
    std::cout << enabled.size() << " enabled mod(s)\n";
    const auto first = timed(enabled, "first check");
    const auto again = timed(enabled, "again, nothing changed");
    auto reversed = enabled;
    std::reverse(reversed.begin(), reversed.end());
    (void)timed(reversed, "load order reversed");
    // Nothing is held open between checks: every mod's archives can be taken alone, as an update
    // replacing them needs.
    std::size_t held{}, tried{};
    for (const auto* mod : enabled) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator at(mod->directory, error), end; !error && at != end; at.increment(error)) {
            if (at->path().extension() != L".cas") continue;
            ++tried;
            const auto file = CreateFileW(at->path().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (file == INVALID_HANDLE_VALUE) ++held;
            else CloseHandle(file);
        }
    }
    std::cout << tried << " mod archive(s) tried after the checks, " << held << " still held\n";
    const bool same = held == 0 && first.size() == again.size() &&
        std::equal(first.begin(), first.end(), again.begin(), [](const auto& a, const auto& b) {
            return a.kept == b.kept && a.lost == b.lost && a.assets == b.assets; });
    std::cout << (same ? "mod conflicts: ok\n" : "mod conflicts: the second check differs from the first, or a file is held\n");
    return same ? 0 : 1;
}
