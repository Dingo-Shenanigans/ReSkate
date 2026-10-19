// Opens a COPY of a real save and reports what loading it changes. Only runs
// when given a scratch folder holding reskate.sqlite3 (never point it at the
// live profile folder: loading commits the migrated document).
//   dingosdk_profile_migration_tests <scratch folder>
#include "Extension/Profile/local_profile.h"
#include <iostream>

namespace {
std::size_t saved_items(const dingosdk::profile::Snapshot& snapshot, bool objects) {
    const auto& section = objects ? snapshot.extensions.value("object_dropper", dingosdk::Json::object()) : snapshot.customization;
    return section.value("inventory", dingosdk::Json::object()).size();
}
}

int main(int argc, char** argv) {
    using namespace dingosdk;
    if (argc < 2) {
        std::cout << "No scratch save given; skipped.\n";
        return 0;
    }
    const auto save = std::filesystem::absolute(argv[1]) / "reskate.sqlite3";
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    {
        profile::Store store(save);
        const auto snapshot = store.snapshot();
        const auto policy = profile::challenge_policy(snapshot);
        const auto& section = snapshot.extensions.at("challenges");
        std::size_t available{};
        for (const auto& [id, definition] : policy.catalog) available += definition.available;
        std::cout << "challenges: " << policy.catalog.size() << " in catalogue (" << available << " available), "
                  << section.at("catalog").size() << " stored changes, " << section.at("progress").size()
                  << " with progress\n"
                  << "entitlements: " << snapshot.entitlements.size() << "\n";
        check(!policy.catalog.empty(), "the challenge catalogue loads");

        // Ownership is a rule while a kind's unlock option is on: the rows earlier versions saved
        // for every item they saw go, and nothing else in the profile changes. Off, they stay.
        const auto cosmetics = store.bool_option(profile::unlock_cosmetics_option);
        const auto objects = store.bool_option(profile::unlock_objects_option);
        std::cout << "saved ownership: " << saved_items(snapshot, false) << " cosmetic(s), " << saved_items(snapshot, true)
                  << " object(s)\n";
        store.set_bool_option(profile::unlock_cosmetics_option, false);
        store.set_bool_option(profile::unlock_objects_option, false);
        store.forget_unlocked_inventory();
        check(saved_items(store.snapshot(), false) == saved_items(snapshot, false) &&
                  saved_items(store.snapshot(), true) == saved_items(snapshot, true),
              "with the unlock options off, saved ownership stays");
        store.set_bool_option(profile::unlock_cosmetics_option, true);
        store.set_bool_option(profile::unlock_objects_option, true);
        store.forget_unlocked_inventory();
        // (The options as they were, so only the inventory differs from the save that was copied.)
        if (cosmetics) store.set_bool_option(profile::unlock_cosmetics_option, *cosmetics);
        if (objects) store.set_bool_option(profile::unlock_objects_option, *objects);
    }
    {
        profile::Store reopened(save);
        const auto after = reopened.snapshot();
        check(saved_items(after, false) == 0 && saved_items(after, true) == 0, "with the unlock options on, saved ownership is gone after a restart");
        profile::Store original(std::filesystem::absolute(argv[1]) / "original" / "reskate.sqlite3");
        const auto before = original.snapshot();
        auto expected = before, actual = after;
        expected.customization.erase("inventory");
        actual.customization.erase("inventory");
        if (expected.extensions.contains("object_dropper")) expected.extensions.at("object_dropper").erase("inventory");
        if (actual.extensions.contains("object_dropper")) actual.extensions.at("object_dropper").erase("inventory");
        expected.revision = actual.revision = 0;
        check(expected.customization == actual.customization, "outfits, the player card and the rest of customization are unchanged");
        check(expected.extensions == actual.extensions, "every other saved section is unchanged");
        check(expected.bool_options == actual.bool_options && expected.entitlements == actual.entitlements &&
                  expected.quests == actual.quests && expected.settings == actual.settings,
              "options, entitlements, quests and settings are unchanged");
    }
    if (failures) return 1;
    std::cout << "profile migration: ok\n";
    return 0;
}
