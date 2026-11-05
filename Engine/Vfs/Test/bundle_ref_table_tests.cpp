// Merges the game's character bundle-reference table with a cosmetic mod's
// copy of it. Arguments: <Skate folder> <cosmetic mod folder> [<another
// cosmetic mod folder>]; skipped when either of the first two is missing.
#include "Engine/Vfs/mod_merge_internal.h"
#include "Engine/Resource/bundle_ref_table.h"
#include "Engine/Resource/cas_codec.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
using namespace dingosdk::mods::detail;
using dingosdk::ascii_lower;

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::cerr << "FAILED: " << what << "\n"; ++failures; }
}
struct Copy { std::vector<std::byte> bytes, meta; };
Copy read_table(const CasStore& store, const fs::path& root, const fs::path& base_root, const fs::path& game_root) {
    const auto toc = fb::read_toc(read_file(root / L"Win32" / L"items.toc"));
    for (const auto& bundle : toc.bundles) {
        if (ascii_lower(bundle.name) != "win32/characters/customization/configs/cas_main_sharedbundle") continue;
        const auto listing = list_bundle(store, root, base_root, bundle, game_root);
        if (!listing) break;
        for (std::size_t i = 0; i < listing->manifest.resources.size(); ++i) {
            const auto& resource = listing->manifest.resources[i];
            if (ascii_lower(resource.name) != "characters/customization/configs/cas_main_bundlereftable") continue;
            const auto& file = listing->files.at(listing->first + listing->manifest.ebx.size() + i);
            return {fb::decode_cas(store.read(file.location.patch ? root : base_root, file.location, file.offset, file.size),
                                   {game_root}),
                    resource.resourceMeta};
        }
    }
    throw std::runtime_error("no cas_main_bundlereftable in " + root.string());
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || !fs::exists(argv[1]) || !fs::exists(fs::path(argv[2]) / L"Win32" / L"items.toc")) {
        std::cout << "No game or cosmetic mod given; skipped.\n";
        return 0;
    }
    try {
        const fs::path game_root = argv[1], mod = argv[2], base_root = game_root / L"Data";
        const auto layout = dingosdk::vfs::read_layout(mod / L"layout.toc");
        const CasStore store(base_root, fs::temp_directory_path() / L"reskate-bundle-ref-test", layout.root);
        const auto base = read_table(store, base_root, base_root, game_root);
        const auto copy = read_table(store, mod, base_root, game_root);
        const fb::bundle_ref::Table base_table{base.bytes, base.meta}, mod_table{copy.bytes, copy.meta};
        const auto once = fb::bundle_ref::merge(base_table, std::span(&mod_table, 1));
        check(once.added > 0, "the mod's presets are added");
        const std::array twice_tables{mod_table, mod_table};
        const auto twice = fb::bundle_ref::merge(base_table, twice_tables);
        check(twice.added == once.added && !twice.conflicts, "the same preset from two mods is added once");
        const fb::bundle_ref::Table merged{once.resource, once.resourceMeta};
        const auto again = fb::bundle_ref::merge(merged, std::span(&mod_table, 1));
        check(again.added == 0, "the merged table already holds every preset");
        std::cout << "base " << base.bytes.size() << " bytes, mod " << copy.bytes.size() << " bytes, merged "
                  << once.resource.size() << " bytes; " << once.added << " preset(s) added.\n";
        if (argc > 3 && fs::exists(fs::path(argv[3]) / L"Win32" / L"items.toc")) {
            // Mods number their presets alike (cust_tops/1_ap, cust_bottoms/1_ap), so two
            // mods can share leaf names. Every preset of both still has to go in.
            const auto other = read_table(store, argv[3], base_root, game_root);
            const fb::bundle_ref::Table other_table{other.bytes, other.meta};
            const auto alone = fb::bundle_ref::merge(base_table, std::span(&other_table, 1));
            const std::array both_tables{mod_table, other_table}, reversed_tables{other_table, mod_table};
            const auto both = fb::bundle_ref::merge(base_table, both_tables);
            const auto reversed = fb::bundle_ref::merge(base_table, reversed_tables);
            check(both.added >= std::max(once.added, alone.added) && both.added <= once.added + alone.added,
                  "two mods' presets are all added");
            check(reversed.added == both.added && reversed.shadowed.size() == both.shadowed.size(),
                  "the order of the mods changes which preset keeps a shared leaf name, not what is added");
            for (const auto& clash : both.shadowed)
                check(clash.edit == 1 && !clash.path.empty() && !clash.holder.empty() && clash.path != clash.holder,
                      "a shared leaf name is reported with the preset that holds it");
            std::cout << "two mods: " << both.added << " preset(s) added, " << both.shadowed.size()
                      << " under their full path only.\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
    return failures ? 1 : 0;
}
