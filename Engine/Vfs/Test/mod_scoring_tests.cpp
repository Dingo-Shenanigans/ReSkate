// The scoring check. Argument: <Skate folder> (optional): its enabled mods are
// checked against the game's own scoring assets, without merging anything.
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_merge_internal.h"
#include "Engine/Vfs/mod_scoring.h"

#include <chrono>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
using namespace dingosdk::mods;
using namespace dingosdk::mods::detail;
using dingosdk::ascii_lower;

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::cerr << "FAILED: " << what << "\n"; ++failures; }
}

constexpr char core_toc[] = "Win32/levels/game/bam_levelroot/bam_levelroot.toc";
constexpr char core_bundle[] = "win32/levels/game/bam_levelroot/bam_coregameassets";
constexpr char flip[] = "gameplay/scorables/fliptricks/scoreable_360flip";

// The game's own copy of the 360 flip's points.
fb::Sha1 game_flip(const fs::path& game) {
    const auto base = game / L"Data";
    const auto layout = dingosdk::vfs::read_layout(base / L"layout.toc");
    const CasStore store(base, fs::temp_directory_path() / L"reskate-scoring-test-out", layout.root);
    for (const auto& bundle : fb::read_toc(read_file(base / core_toc)).bundles) {
        if (ascii_lower(bundle.name) != core_bundle) continue;
        if (const auto listing = list_bundle(store, base, base, bundle, game))
            for (const auto& asset : listing->manifest.ebx)
                if (ascii_lower(asset.name) == flip) return asset.sha1;
    }
    throw std::runtime_error("the game has no 360 flip");
}

// A mod shipping the core bundle with these assets (only the manifest is read).
Mod fake_mod(const fs::path& root, const std::string& name, const std::vector<fb::BundleAsset>& ebx) {
    fb::BinaryBundle manifest;
    manifest.ebx = ebx;
    std::vector<fb::BundleFileInfo> files(ebx.size(), fb::BundleFileInfo{{true, 0, 1}, 0, 16});
    fb::TocBundle bundle;
    bundle.name = core_bundle;
    bundle.region = fb::write_bundle_region(files, fb::write_binary_bundle(manifest));
    const auto directory = root / name;
    write_file(directory / core_toc, fb::write_patch_toc(std::span(&bundle, 1)));
    Mod mod;
    mod.name = name;
    mod.directory = directory;
    mod.provides_layout = true;
    return mod;
}

fb::BundleAsset ebx_asset(std::string name, fb::Sha1 sha1) {
    fb::BundleAsset asset;
    asset.kind = fb::AssetKind::ebx;
    asset.name = std::move(name);
    asset.sha1 = sha1;
    asset.originalSize = 3712;
    return asset;
}
} // namespace

int main(int argc, char** argv) {
    check(scoring_asset("gameplay/scorables/fliptricks/scoreable_360flip"), "a trick's points are scoring");
    check(scoring_asset("ecs/scoringmanager/scoringdataresource"), "the scoring manager's data is scoring");
    check(scoring_asset("animation/dingo/skatercorephysicsscoringconfig"), "the physics scoring config is scoring");
    check(scoring_asset("gameplay/activity/components/throwdowns/serverjamsessionthrowdown"), "throwdown logic is scoring");
    check(!scoring_asset("gameplay/scorablesx/whatever"), "a similar prefix is not scoring");
    check(!scoring_asset("levels/game/dingolevel_mpr/materialgrid_win32"), "a map's surfaces are not scoring");
    check(!scoring_asset("camera/skatecamera"), "a camera is not scoring");
    check(scoring_asset("animation/dingo/skatercorephysicswipeoutconfig"), "wipeout physics are checked");
    check(scoring_asset("gameplay/input/gestures/fliptricks/gesture_fliptrick_360flip"), "trick gestures are checked");
    check(!scoring_asset("animation/dingo/skatercorephysicsikconfig"), "the skater's IK (looks only) is not checked");
    check(!scoring_asset("gameplay/skatephysicstuning"), "physics tuning is left to multiplayer's own enforcement");

    Catalog empty;
    check(check_scoring(empty).fingerprint == 0, "no mods, the game's own scoring");

    if (argc >= 2 && fs::exists(fs::path(argv[1]) / L"Data" / L"layout.toc")) {
        const auto catalog = load_catalog(argv[1], {}, false);
        std::vector<std::string> notes;
        const auto start = std::chrono::steady_clock::now();
        const auto result = check_scoring(catalog, &notes);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::cout << catalog.mods.size() << " enabled mod(s) checked in " << ms << " ms; fingerprint " << std::hex
                  << result.fingerprint << std::dec << "\n";
        for (const auto& mod : catalog.mods) std::cout << "  mod " << mod.name << "\n";
        for (const auto& mod : result.mods) std::cout << "  changes scoring: " << mod << "\n";
        for (const auto& asset : result.assets) std::cout << "  changed: " << asset << "\n";
        for (const auto& note : notes) std::cout << "  note: " << note << "\n";
        check((result.fingerprint == 0) == result.mods.empty(), "a fingerprint exactly when a mod changes scoring");

        // Synthetic mods next to the real game: one carries the game's own copy of the 360 flip
        // and adds a scorable the game does not have, one changes the 360 flip's points.
        const auto root = fs::temp_directory_path() / L"reskate-scoring-test";
        std::error_code error;
        fs::remove_all(root, error);
        const auto original = game_flip(argv[1]);
        auto changed = original;
        changed.bytes[0] = static_cast<std::byte>(static_cast<unsigned>(changed.bytes[0]) ^ 0xff);
        Catalog fake;
        fake.data_root = argv[1];
        fake.root = root;
        fake.mods.push_back(fake_mod(root, "same_points", {ebx_asset(flip, original),
                                                           ebx_asset("gameplay/scorables/fliptricks/scoreable_madeup", changed)}));
        check(check_scoring(fake).fingerprint == 0, "the game's own copy and a new scorable change nothing");
        fake.mods.push_back(fake_mod(root, "big_points", {ebx_asset(flip, changed), ebx_asset("camera/whatever", changed)}));
        const auto flagged = check_scoring(fake);
        check(flagged.fingerprint != 0, "a changed trick is flagged");
        check(flagged.mods == std::vector<std::string>{"big_points"}, "only the mod that changes it is named");
        check(flagged.assets == std::vector<std::string>{flip}, "only the trick is listed");
        check(check_scoring(fake).fingerprint == flagged.fingerprint, "the same mods give the same fingerprint");
        fs::remove_all(root, error);
    } else {
        std::cout << "No game given; the installed mods were not checked.\n";
    }
    return failures ? 1 : 0;
}
