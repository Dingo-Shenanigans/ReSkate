#include "mod_catalog.h"
#include "Engine/Core/Platform/path_text.h"
#include "builtin_packs.h"

#include "mod_merge.h"
#include "mod_conflicts.h"
#include "native_db.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Game/Build/supported_build.h"

#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
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

/// Whether a mod's layout (layout.toc and its Win32 superbundle lists) names one of these bundles: it
// brings what a variant displaces (mod_list.h Variants).
bool lists_bundle(const fs::path& mod, const std::vector<std::string>& bundles) {
    if (bundles.empty()) return false;
    std::error_code error;
    std::vector<fs::path> lists{mod / L"layout.toc"};
    for (fs::directory_iterator at(mod / L"Win32", error), end; !error && at != end; at.increment(error))
        if (at->is_regular_file(error) && at->path().extension() == L".toc") lists.push_back(at->path());
    for (const auto& list : lists) {
        std::ifstream file(list, std::ios::binary);
        if (!file) continue;
        const auto bytes = lower(std::string{std::istreambuf_iterator<char>(file), {}});
        for (const auto& bundle : bundles)
            if (bytes.find(bundle) != std::string::npos) return true;
    }
    return false;
}
// The choices of variant are kept beside ReSkate's other files for the user, so that they are there
// before the profile is: the mods are put together as the game starts.
// {"<mod folder, lower-case>": "<variant>"}
fs::path variants_file() {
    std::array<wchar_t, 1024> text{};
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", text.data(), static_cast<DWORD>(text.size()));
    if (!size || size >= text.size()) return {};
    return fs::path(std::wstring(text.data(), size)) / L"ReSkate" / L"variants.json";
}
// A variant's name is its folder's: letters, digits, '_' and '-', in small letters.
bool variant_name(std::string_view name) {
    return !name.empty() && name.size() <= maximum_mod_name &&
           std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}
// The variants a mod holds (mod_list.h), by name, its "order" first and then the others by name.
std::vector<std::pair<std::string, fs::path>> variants_inside(const Mod& mod) {
    std::vector<std::pair<std::string, fs::path>> found;
    if (!mod.variants) return found;
    std::error_code error;
    for (fs::directory_iterator at(mod.directory / mod.variants->folder, error), end; !error && at != end; at.increment(error)) {
        if (!at->is_directory(error) || !fs::is_regular_file(at->path() / L"layout.toc", error)) continue;
        auto name = lower(path_utf8(at->path().filename()));
        if (variant_name(name)) found.emplace_back(std::move(name), at->path());
    }
    const auto& order = mod.variants->order;
    const auto place = [&](const std::string& name) { return static_cast<std::size_t>(std::find(order.begin(), order.end(), name) - order.begin()); };
    std::sort(found.begin(), found.end(), [&](const auto& a, const auto& b) {
        return place(a.first) != place(b.first) ? place(a.first) < place(b.first) : a.first < b.first;
    });
    return found;
}
// The choices made in this run, and what the last live apply gave the game (none since launch: the
// launch's catalogue says).
using VariantMap = std::map<std::string, std::string, std::less<>>;
struct VariantsApplied {
    VariantMap in_play;
    std::vector<std::string> set_aside;
};
std::mutex variants_mutex;
std::optional<VariantMap> chosen_variants;
std::optional<VariantsApplied> applied_variants;
VariantMap read_choices() {
    VariantMap out;
    const auto path = variants_file();
    std::error_code error;
    if (path.empty() || !fs::is_regular_file(path, error)) return out;
    std::ifstream file(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    const auto root = Json::parse(text, {64 * 1024, 4, 1024});
    if (!root.is_object()) return out;
    for (const auto& [mod, variant] : root.items())
        if (variant.is_string() && variant_name(lower(variant.string()))) out[lower(mod)] = lower(variant.string());
    return out;
}

} // namespace

std::string variant_of(std::string_view mod) {
    const auto mark = mod.rfind('#');
    if (mark == std::string_view::npos) return {};
    auto name = lower(mod.substr(mark + 1));
    return variant_name(name) ? name : std::string{};
}
std::string variant_owner(std::string_view mod) {
    const auto mark = mod.rfind('#');
    return mark == std::string_view::npos ? std::string{} : std::string(mod.substr(0, mark));
}
std::vector<std::string> installed_variants(const Mod& mod) {
    std::vector<std::string> found;
    try {
        for (auto& variant : variants_inside(mod)) found.push_back(std::move(variant.first));
    } catch (...) {}
    return found;
}
std::filesystem::path variant_folder(const Mod& mod, std::string_view variant) {
    try {
        const auto wanted = lower(variant);
        for (auto& one : variants_inside(mod))
            if (one.first == wanted) return std::move(one.second);
    } catch (...) {}
    return {};
}
std::string chosen_variant(std::string_view mod) noexcept {
    try {
        std::lock_guard lock(variants_mutex);
        if (!chosen_variants) {
            try {
                chosen_variants = read_choices();
            } catch (...) {
                chosen_variants.emplace(); // an unreadable file chooses nothing
            }
        }
        const auto found = chosen_variants->find(lower(mod));
        return found == chosen_variants->end() ? std::string{} : found->second;
    } catch (...) {
        return {};
    }
}
bool save_variant(std::string_view mod, std::string_view variant) noexcept {
    try {
        const auto name = lower(variant);
        if (mod.empty() || !variant_name(name)) return false;
        const auto path = variants_file();
        if (path.empty()) return false;
        (void)chosen_variant(mod); // read the file once, so its other choices are kept
        std::lock_guard lock(variants_mutex);
        auto choices = *chosen_variants;
        choices[lower(mod)] = name;
        auto root = Json::object();
        for (const auto& [owner, chosen] : choices) root[owner] = chosen;
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << root.dump(2) << '\n';
        if (!file) return false;
        chosen_variants = std::move(choices);
        return true;
    } catch (...) {
        return false;
    }
}
std::string variant_in_play(std::string_view mod) noexcept {
    try {
        const auto look = [&](const VariantMap& in_play) {
            for (const auto& [owner, variant] : in_play)
                if (lower(owner) == lower(mod)) return variant;
            return std::string{};
        };
        {
            std::lock_guard lock(variants_mutex);
            if (applied_variants) return look(applied_variants->in_play);
        }
        return look(catalog().variants_in_play);
    } catch (...) {
        return {};
    }
}
std::vector<std::string> set_aside_for_variants() {
    {
        std::lock_guard lock(variants_mutex);
        if (applied_variants) return applied_variants->set_aside;
    }
    return catalog().set_aside;
}
void variants_applied(const Catalog& catalog) noexcept {
    try {
        std::lock_guard lock(variants_mutex);
        applied_variants = VariantsApplied{catalog.variants_in_play, catalog.set_aside};
    } catch (...) {}
}

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
    // Content ReSkate carries itself (builtin_packs.h), unpacked first: it makes the Mods folder
    // when there is none, so the content loads without any mod installed.
    std::vector<std::string> builtin_notes;
    const auto builtin = unpack_builtin_packs(data_root / mods_folder, builtin_notes);
    auto list = scan_mods(data_root);
    for (auto& note : builtin_notes) list.notes.push_back(std::move(note));
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
        // A mod ReSkate retired (builtin_packs.h) is left out.
        std::erase_if(list.entries, [&](const ModEntry& entry) {
            if (!kept_by_reskate(data_root / mods_folder, entry.mod.name)) return false;
            result.notes.push_back(entry.mod.name + " is retired; it is left out");
            return true;
        });
        // After every installed mod, so none of theirs is ever shadowed by it; always on.
        for (const auto& pack : builtin)
            list.entries.push_back({describe_mod(pack.directory, pack.name, result.notes), true});
        // Variants (mod_catalog.h): each variant a mod holds becomes a mod of its own for the run,
        // listed after it as <folder>#<variant>, so that variants are chosen, set aside and switched
        // like mods in folders of their own. It carries its mod's build stamp.
        std::vector<std::string> owners;
        for (std::size_t i = 0; i < list.entries.size(); ++i) {
            auto inside = variants_inside(list.entries[i].mod);
            if (inside.empty()) continue;
            const auto owner = list.entries[i].mod;
            owners.push_back(owner.name);
            const bool enabled = list.entries[i].enabled;
            for (auto& [variant, directory] : inside) {
                ModEntry part;
                part.enabled = enabled;
                part.mod.name = owner.name + "#" + variant;
                part.mod.title = owner.title + " (" + variant + ")";
                part.mod.author = owner.author;
                part.mod.version = owner.version;
                part.mod.directory = std::move(directory);
                part.mod.provides_layout = true;
                part.mod.studio_marker = owner.studio_marker;
                part.mod.skate_sha256 = owner.skate_sha256;
                part.mod.outdated = owner.outdated;
                list.entries.insert(list.entries.begin() + static_cast<std::ptrdiff_t>(++i), std::move(part));
            }
        }
        // One variant of each loads, and what would fight it is set aside: the mod's other variants,
        // and every other mod whose layout lists a bundle its variants displace (the mod's own layout
        // too). Those are left out whole: with a variant in place of their own, such a mod would have
        // the rest of its changes on game data they were not made for. mods.json is not changed, and
        // their archives are placed like a disabled mod's, so another variant can be chosen while the
        // game runs. A choice that is not installed falls back to the first of the "order", then to
        // any variant there is.
        for (const auto& owner : owners) {
            const auto held = std::find_if(list.entries.begin(), list.entries.end(), [&](const auto& entry) { return entry.mod.name == owner; });
            if (held == list.entries.end() || !held->enabled) continue;
            const auto declared = *held->mod.variants;
            const auto of_owner = [&](const ModEntry& entry) { return variant_owner(entry.mod.name) == owner; };
            const auto enabled_variant = [&](const std::string& name) {
                return std::find_if(list.entries.begin(), list.entries.end(), [&](const auto& entry) {
                    return entry.enabled && of_owner(entry) && (name.empty() || variant_of(entry.mod.name) == name);
                });
            };
            auto first = list.entries.end();
            for (const auto& name : {chosen_variant(owner), declared.order.empty() ? std::string() : declared.order.front(), std::string()})
                if (first == list.entries.end()) first = enabled_variant(name);
            if (first == list.entries.end()) continue;
            std::rotate(list.entries.begin(), first, first + 1);
            result.notes.push_back("Variants: " + list.entries.front().mod.name + " loads first, so it is the one of " + owner + "'s in the game");
            for (auto entry = list.entries.begin() + 1; entry != list.entries.end(); ++entry) {
                if (!entry->enabled || (!of_owner(*entry) && !lists_bundle(entry->mod.directory, declared.displaces))) continue;
                entry->enabled = false;
                result.set_aside.push_back(entry->mod.name);
                result.notes.push_back("Variants: " + entry->mod.name + (of_owner(*entry) ? " is another of " + owner + "'s" : " brings what they replace") +
                                       " and is set aside for this run");
            }
        }
        for (auto& entry : list.entries) {
            if (!entry.enabled) {
                if (std::find(result.set_aside.begin(), result.set_aside.end(), entry.mod.name) == result.set_aside.end()) {
                    result.notes.push_back("Mod disabled in mods.json: " + entry.mod.name);
                    result.disabled.push_back(entry.mod.name);
                }
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
        // `remembered`: it stays out on later launches without a retry, until its files or ReSkate change
        // (or, left out for a conflict, the mod it conflicts with: `conflict`, that mod's name and files).
        const auto leave_out = [&](std::size_t index, std::vector<std::string> problems, const std::string& fingerprint, bool remembered = true,
                                   std::pair<std::string, std::string> conflict = {}) {
            auto mod = std::move(result.mods[index]);
            result.mods.erase(result.mods.begin() + static_cast<std::ptrdiff_t>(index));
            // Level bundles decide whether a map loads at all, so their problems lead.
            std::stable_partition(problems.begin(), problems.end(), [](const std::string& problem) {
                return problem.find("levels/game/") != std::string::npos;
            });
            mod.problems = std::move(problems);
            if (remembered) still_excluded[mod.name] = {fingerprint, sdk, mod.problems, conflict.first, conflict.second};
            result.excluded.push_back(std::move(mod));
        };
        for (std::size_t i = result.mods.size(); i-- > 0;) {
            const auto found = list.excluded.find(result.mods[i].name);
            if (found == list.excluded.end()) continue;
            const auto& previous = exclusions.at(result.mods[i].name);
            if (sdk.empty() || previous.sdk != sdk) continue; // a new ReSkate may merge it now
            // One left out because the game was of another build is looked at again below: the
            // game may have been put right since (its files verified), with the mod untouched.
            // Remembered, it stayed out for good with the game fixed (seen 2026-10-13).
            if (!found->second.empty() && found->second.front().starts_with("made for game build ")) continue;
            // Left out for a conflict: only while the mod it lost to still loads above it, as it was.
            if (!previous.conflictWith.empty()) {
                const auto winner = std::find_if(result.mods.begin(), result.mods.begin() + static_cast<std::ptrdiff_t>(i),
                                                 [&](const Mod& mod) { return mod.name == previous.conflictWith; });
                if (winner == result.mods.begin() + static_cast<std::ptrdiff_t>(i) ||
                    mod_fingerprint(winner->directory) != previous.conflictFingerprint)
                    continue;
                result.notes.push_back("Leaving out " + result.mods[i].name + ": it conflicts with " + previous.conflictWith +
                    ", which loads above it, and neither has changed since");
                leave_out(i, found->second, previous.fingerprint, true, {previous.conflictWith, previous.conflictFingerprint});
                continue;
            }
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
            // (When it is the game that is of another build than ReSkate's, the mod is not what
            // needs seeing to.)
            leave_out(i, {"made for game build " + build + ", not the installed " + game_build +
                          (game_build != supported_build::game_data_changelist
                               ? "; your game files are not the version ReSkate supports: in the launcher, open Settings and press Verify game files"
                               : "; rebuild it with ReSkate Studio for this game build")},
                      mod_fingerprint(result.mods[i].directory), false); // (checked on every launch: the game's build can change under it)
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
                // The first looks for conflicts before building anything (ConflictChecker).
                merge = merge_mods(result, observe, {.conflicts_first = round == 0});
                if (!merge.issue.empty() || round == 4) break; // a fifth merge only rebuilds without round four's removals
                // (What the check before the merge noted, which the merge's own notes below would not carry.)
                if (merge.stopped_for_conflicts)
                    for (auto& note : merge.notes) result.notes.push_back(std::move(note));
                // Everything this merge found is settled at once, so one more merge is enough: the
                // mods that could not be merged cleanly, and, of two mods that change one asset in
                // ways that cannot both be in the game, the one lower in the load order, whole (so
                // neither loads half its changes). Mods are looked at from the top of the load
                // order down, so one that lost only to a mod that is itself left out stays in.
                // ReSkate's own packs, which load below every mod, are never left out for this.
                const auto ours = [](const std::string& name) {
                    return name.starts_with("reskate-builtin-") || name.find('#') != std::string::npos;
                };
                struct Out {
                    std::vector<std::string> problems;
                    std::string winner;   // left out for a conflict with this mod
                };
                std::map<std::size_t, Out> out;
                std::set<std::string, std::less<>> broken;
                std::vector<std::string> order;
                for (std::size_t i = 0; i < result.mods.size(); ++i) {
                    const auto& name = result.mods[i].name;
                    order.push_back(name);
                    if (const auto found = merge.problems.find(name); found != merge.problems.end() && !found->second.empty()) {
                        out[i].problems = found->second;
                        broken.insert(name);
                    }
                }
                const auto losses = conflict_losses(order, merge.conflicts, broken, ours);
                for (std::size_t i = 0; i < result.mods.size(); ++i) {
                    const auto loss = losses.find(result.mods[i].name);
                    if (loss == losses.end()) continue;
                    auto& entry = out[i];
                    entry.winner = loss->second.winner;
                    for (const auto* row : loss->second.rows) {
                        std::string areas;
                        for (const auto& area : row->areas) areas += (areas.empty() ? "" : ", ") + area;
                        entry.problems.push_back("conflicts with " + row->kept + ": both change " +
                            (areas.empty() ? std::string("the same game files") : areas) + " (" + row->asset +
                            (row->assets > 1 ? " and " + std::to_string(row->assets - 1) + " more" : std::string{}) +
                            "), and " + row->kept + " is higher in the load order");
                    }
                }
                const bool removed = !out.empty();
                if (removed)
                    result.notes.push_back((merge.stopped_for_conflicts ? std::string("The check before merging")
                                                                        : "Merge " + std::to_string(round + 1)) +
                        " left " + std::to_string(out.size()) + " mod(s) out; merging once more without them");
                for (auto at = out.rbegin(); at != out.rend(); ++at) {
                    const auto i = at->first;
                    const auto& [problems, winner] = at->second;
                    if (winner.empty()) {
                        result.notes.push_back("Leaving out " + result.mods[i].name +
                            ": it could not be merged cleanly; merging the other mods again without it");
                        leave_out(i, problems, mod_fingerprint(result.mods[i].directory));
                        continue;
                    }
                    const auto kept = std::ranges::find_if(result.mods, [&](const Mod& mod) { return mod.name == winner; });
                    result.notes.push_back("Leaving out " + result.mods[i].name + ": it conflicts with " + winner +
                        ", which is higher in the load order; merging the other mods again without it");
                    leave_out(i, problems, mod_fingerprint(result.mods[i].directory), true,
                              {winner, kept != result.mods.end() ? mod_fingerprint(kept->directory) : std::string{}});
                }
                // (Conflicts only of ReSkate's own packs leave nothing out: then the merge itself runs.)
                if (!removed && !merge.stopped_for_conflicts) break;
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
        // A mod's variant is the one in the game when it is among the mods that load (what would fight
        // it was set aside above).
        for (const auto& mod : result.mods)
            if (const auto owner = variant_owner(mod.name); !owner.empty() && !result.variants_in_play.contains(owner))
                result.variants_in_play.emplace(owner, variant_of(mod.name));
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
            if (!mod.problems.empty() && mod.problems.front().starts_with("conflicts with ")) {
                const auto& problem = mod.problems.front();
                const auto other = problem.substr(15, problem.find(':') - 15);
                result.warnings.push_back("Mod " + mod.name + " was not loaded: it " + problem + ". Only " + other +
                    " is loaded. To use " + mod.name + " instead, move it above " + other + " in the mod list, or turn one of them off.");
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
