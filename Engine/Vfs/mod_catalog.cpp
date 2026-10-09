#include "mod_catalog.h"

#include "mod_merge.h"
#include "native_db.h"
#include "Engine/Core/Json/json.h"

#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>

namespace dingosdk::mods {
namespace {
namespace fs = std::filesystem;

constexpr char levels_name[] = "reskate-levels.json";

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}

std::string join(const std::vector<std::string>& parts, const char* separator) {
    std::string text;
    for (const auto& part : parts) { if (!text.empty()) text += separator; text += part; }
    return text;
}

// Identifies this ReSkate.dll build, so a new one retries mods an older one left out.
// The game build a layout.toc belongs to: its pipelineCodeChangelists. A mod's
// layout is a copy of the game's from the build it was made for, so a mod made
// for an earlier build carries that build's number. Empty when unreadable.
std::string layout_changelist(const fs::path& path) noexcept {
    try {
        std::ifstream file(path, std::ios::binary);
        const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
        if (bytes.size() <= native_db::envelope_size) return {};
        const auto root = native_db::read(std::span(bytes).subspan(native_db::envelope_size), "layout.toc",
                                          nullptr, {.unique_fields = false});
        // It sits in the install manifest; search rather than pin the path.
        const auto find = [](const auto& self, const native_db::Node& node) -> const native_db::Node* {
            if (const auto* found = node.field("pipelineCodeChangelists")) return found;
            for (const auto& child : node.children)
                if (const auto* found = self(self, child)) return found;
            return nullptr;
        };
        const auto* field = find(find, root);
        if (!field) return {};
        if (field->children.empty()) return field->text;
        std::string joined;
        for (const auto& child : field->children) joined += (joined.empty() ? "" : ",") + child.text;
        return joined;
    } catch (...) {
        return {};
    }
}

std::string sdk_identity() {
    // ReSkate.dll beside the running executable, not whichever module holds
    // this code: Skate.exe and ReSkateLauncher.exe both sit beside it, and
    // both merge, so both have to arrive at the same answer or each would
    // call the other's exclusions stale and merge everything again. Not the
    // data root, which -dataPath can move away from the dll. An empty answer
    // (no dll) simply retries, as it always did.
    std::array<wchar_t, 32768> host{};
    const auto length = GetModuleFileNameW(nullptr, host.data(), static_cast<DWORD>(host.size()));
    if (!length || length >= host.size()) return {};
    std::error_code error;
    const auto file = fs::path(std::wstring(host.data(), length)).parent_path() / L"ReSkate.dll";
    const auto size = fs::file_size(file, error);
    if (error) return {};
    const auto written = fs::last_write_time(file, error).time_since_epoch().count();
    return error ? std::string{} : std::to_string(size) + "-" + std::to_string(written);
}

} // namespace

std::filesystem::path engine_data_root() noexcept {
    try {
        std::array<wchar_t, 32768> module_path{};
        const auto length = GetModuleFileNameW(nullptr, module_path.data(),
            static_cast<DWORD>(module_path.size()));
        if (!length || length >= module_path.size()) return {};
        const auto game = fs::path(std::wstring(module_path.data(), length)).parent_path();

        int count{};
        const auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments) return game;
        std::wstring value;
        for (int index = 1; index < count; ++index) {
            const std::wstring argument(arguments[index]);
            const auto equals = argument.find(L'=');
            const auto name = argument.substr(0, equals);
            if (_wcsicmp(name.c_str(), L"-dataPath")) continue;
            if (equals != std::wstring::npos) value = argument.substr(equals + 1);
            else if (index + 1 < count) value = arguments[index + 1];
            break;
        }
        LocalFree(arguments);
        if (value.empty()) return game;
        std::error_code error;
        auto root = fs::weakly_canonical(game / value, error);
        return error ? game / value : root;
    } catch (...) {
        return {};
    }
}

Catalog load_catalog(const std::filesystem::path& data_root, const MergeObserver& observe, bool run_merge) noexcept {
    Catalog result;
    result.data_root = data_root;
    if (data_root.empty()) {
        result.issue = "The engine data root could not be resolved";
        return result;
    }
    auto list = scan_mods(data_root);
    result.root = std::move(list.root);
    result.present = list.present;
    if (!result.present) return result;
    for (auto& note : list.notes) result.notes.push_back(std::move(note));
    if (!list.issue.empty()) {
        // A typo in mods.json must not silently change which mods load.
        result.issue = std::move(list.issue);
        return result;
    }

    try {
        for (const auto& name : list.missing)
            result.notes.push_back("mods.json names a folder that is not present: " + name);
        for (auto& entry : list.entries) {
            if (!entry.enabled) {
                result.notes.push_back("Mod disabled in mods.json: " + entry.mod.name);
                result.disabled.push_back(entry.mod.name);
                if (entry.mod.provides_layout) result.inactive.push_back(std::move(entry.mod));
                continue;
            }
            if (!entry.mod.provides_layout && !entry.mod.provides_levels && entry.mod.park_maps.empty())
                result.notes.push_back("Mod " + entry.mod.name + " has no layout.toc or reskate-levels.json");
            result.mods.push_back(std::move(entry.mod));
        }
        std::sort(result.disabled.begin(), result.disabled.end(),
            [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
        // Mods built for another Skate.exe (or not stamped with one) never load until they are
        // updated, enabled or not (a disabled mod's layout is merged too). They are checked on every
        // launch, so they never enter the exclusions file.
        for (std::size_t i = result.mods.size(); i-- > 0;) {
            if (result.mods[i].outdated.empty()) continue;
            auto mod = std::move(result.mods[i]);
            result.mods.erase(result.mods.begin() + static_cast<std::ptrdiff_t>(i));
            result.notes.push_back("Leaving out " + mod.name + ": " + mod.outdated);
            mod.problems = {"outdated: " + mod.outdated + "; update it for this game version"};
            result.outdated.push_back(mod);
            result.excluded.push_back(std::move(mod));
        }
        for (std::size_t i = result.inactive.size(); i-- > 0;) {
            if (result.inactive[i].outdated.empty()) continue;
            result.notes.push_back("Disabled mod " + result.inactive[i].name + " is left out of the patch too: " + result.inactive[i].outdated);
            result.outdated.push_back(std::move(result.inactive[i]));
            result.inactive.erase(result.inactive.begin() + static_cast<std::ptrdiff_t>(i));
        }
        // Mods that failed before and are unchanged stay out without a retry.
        auto exclusions = read_exclusions(result.root);
        const auto sdk = sdk_identity();
        std::map<std::string, Exclusion, std::less<>> still_excluded;
        const auto leave_out = [&](std::size_t index, std::vector<std::string> problems, const std::string& fingerprint) {
            auto mod = std::move(result.mods[index]);
            result.mods.erase(result.mods.begin() + static_cast<std::ptrdiff_t>(index));
            // Level bundles decide whether a map loads at all, so their problems lead.
            std::stable_partition(problems.begin(), problems.end(), [](const std::string& problem) {
                return problem.find("levels/game/") != std::string::npos;
            });
            mod.problems = std::move(problems);
            still_excluded[mod.name] = {fingerprint, sdk, mod.problems};
            result.excluded.push_back(std::move(mod));
        };
        for (std::size_t i = result.mods.size(); i-- > 0;) {
            const auto found = list.excluded.find(result.mods[i].name);
            if (found == list.excluded.end()) continue;
            const auto& previous = exclusions.at(result.mods[i].name);
            if (sdk.empty() || previous.sdk != sdk) continue; // a new ReSkate may merge it now
            result.notes.push_back("Leaving out " + result.mods[i].name +
                ": it could not be merged cleanly before and its files have not changed since");
            leave_out(i, found->second, previous.fingerprint);
        }
        // A mod made for another game build ships that build's layout and full
        // copies of shared game bundles (globals, the level root) that point
        // into the old build's archives: merged in, the game waits on the
        // splash screen forever. Leave such mods out, enabled or not (a
        // disabled map's globals are merged too), until they are rebuilt.
        const auto game_build = layout_changelist(data_root / L"Data" / L"layout.toc");
        const auto made_for = [&](const Mod& mod) -> std::string {
            if (game_build.empty() || !mod.provides_layout) return {};
            const auto build = layout_changelist(mod.directory / L"layout.toc");
            return !build.empty() && build != game_build ? build : std::string{};
        };
        for (std::size_t i = result.mods.size(); i-- > 0;) {
            const auto build = made_for(result.mods[i]);
            if (build.empty()) continue;
            result.notes.push_back("Leaving out " + result.mods[i].name + ": it was made for game build " + build +
                                   ", the installed game is " + game_build);
            leave_out(i, {"made for game build " + build + ", not the installed " + game_build +
                          "; rebuild it with ReSkate Studio for this game build"},
                      mod_fingerprint(result.mods[i].directory));
        }
        for (std::size_t i = result.inactive.size(); i-- > 0;) {
            const auto build = made_for(result.inactive[i]);
            if (build.empty()) continue;
            result.notes.push_back("Disabled mod " + result.inactive[i].name + " is left out of the patch too: it was made for game build " +
                                   build + ", the installed game is " + game_build);
            result.inactive.erase(result.inactive.begin() + static_cast<std::ptrdiff_t>(i));
        }
        // Any Mods folder gets a patch, even an empty one or one of disabled
        // mods only: a mod enabled or dropped in while the game runs needs it
        // mounted from launch.
        if (run_merge) {
            MergeReport merge;
            // Merge, and whenever a mod could not be merged cleanly, merge again
            // without it. Removing one mod cannot break another, but a few rounds
            // are allowed in case two damaged mods hid each other's problems.
            for (int round = 0;; ++round) {
                merge = merge_mods(result, observe);
                if (!merge.issue.empty() || round == 4) break; // a fifth merge only rebuilds without round four's removals
                bool removed = false;
                for (std::size_t i = result.mods.size(); i-- > 0;) {
                    const auto found = merge.problems.find(result.mods[i].name);
                    if (found == merge.problems.end() || found->second.empty()) continue;
                    result.notes.push_back("Leaving out " + result.mods[i].name +
                        ": it could not be merged cleanly; merging the other mods again without it");
                    leave_out(i, found->second, mod_fingerprint(result.mods[i].directory));
                    removed = true;
                }
                if (!removed) break;
            }
            for (auto& note : merge.notes) result.notes.push_back(std::move(note));
            result.merged = merge.built;
            if (!merge.issue.empty())
                result.issue = "the mods could not be merged into one patch: " + merge.issue;
            else if (merge.reused)
                result.notes.push_back("Kept the merged patch of " + std::to_string(result.mods.size()) +
                    " mod(s) (" + std::to_string(merge.superbundles) + " superbundle(s), " +
                    std::to_string(merge.archives) + " archive(s)); nothing changed since it was built");
            else if (merge.built)
                result.notes.push_back("Merged " + std::to_string(result.mods.size()) +
                    " mod(s) into " + std::to_string(merge.superbundles) + " superbundle(s) and " +
                    std::to_string(merge.archives) + " archive(s)");
        }
        if (run_merge) write_exclusions(result.root, still_excluded);
        // One line per mod, so a log shows at a glance what was installed and
        // what built it.
        for (const auto& mod : result.mods) result.notes.push_back(describe(mod));
        for (const auto& mod : result.excluded) {
            if (!mod.outdated.empty()) {
                result.warnings.push_back("Mod " + mod.name + " was not loaded: it is outdated (" + mod.outdated +
                    "). Update it for this game version.");
                continue;
            }
            if (!mod.problems.empty() && mod.problems.front().starts_with("made for game build ")) {
                result.warnings.push_back("Mod " + mod.name + " was not loaded: it was " + mod.problems.front() +
                    (mod.levels.empty() ? std::string{} : " (its maps are not listed: " + join(mod.levels, ", ") + ")") + ".");
                continue;
            }
            constexpr std::size_t shown = 3;
            const std::vector<std::string> first(mod.problems.begin(),
                mod.problems.begin() + static_cast<std::ptrdiff_t>(std::min(shown, mod.problems.size())));
            result.warnings.push_back("Mod " + mod.name + " was not loaded because it could not be merged cleanly" +
                (mod.levels.empty() ? std::string{} : " (its maps are not listed: " + join(mod.levels, ", ") + ")") +
                ". " + join(first, " | ") +
                (mod.problems.size() > shown
                     ? " | and " + std::to_string(mod.problems.size() - shown) + " more"
                     : std::string{}) +
                ". Reinstall the whole mod folder, or rebuild it with a current ReSkate Studio.");
        }
    } catch (const std::exception& failure) {
        result.mods.clear();
        result.merged = false;
        result.issue = failure.what();
    } catch (...) {
        result.mods.clear();
        result.merged = false;
        result.issue = "The mod catalogue could not be read";
    }
    return result;
}

const Catalog& catalog(const MergeObserver& observe) noexcept {
    static const Catalog value = load_catalog(engine_data_root(), observe);
    return value;
}

std::vector<std::filesystem::path> level_manifest_paths(const Catalog& catalog) {
    std::vector<std::filesystem::path> paths;
    for (const auto& mod : catalog.mods)
        if (mod.provides_levels) paths.push_back(mod.directory / levels_name);
    return paths;
}

const Mod* mod_registering(const Catalog& catalog, std::string_view destination) noexcept {
    try {
        const auto haystack = lower(destination);
        for (const auto& mod : catalog.mods)
            for (const auto& level : mod.levels)
                if (!level.empty() && haystack.find(lower(level)) != std::string::npos) return &mod;
    } catch (...) {}
    return nullptr;
}

std::string describe(const Mod& mod) {
    std::string text = "Mod " + mod.name + ": ";
    if (mod.tool.empty() && mod.version.empty() && mod.built.empty())
        text += "no build info (an older Studio or another tool built it)";
    else
        text += "built by " + (mod.tool.empty() ? std::string("an unnamed tool") : mod.tool) +
            (mod.version.empty() ? "" : " " + mod.version) + (mod.built.empty() ? "" : " on " + mod.built);
    text += mod.provides_layout ? "; ships a layout" : "; ships no layout";
    if (!mod.levels.empty())
        text += "; registers " + std::to_string(mod.levels.size()) + " level(s): " + join(mod.levels, ", ");
    else if (mod.provides_levels)
        text += "; registers no level";
    if (!mod.provides_layout) return text;
    text += mod.problems.empty() ? "; no merge problem recorded"
                                 : "; " + std::to_string(mod.problems.size()) + " merge problem(s), see the warning";
    return text;
}

} // namespace dingosdk::mods
