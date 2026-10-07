// The game's settings hooks cache saved values per thread and look them up again only when
// profile::Store::changes() advances, so every save that changes the profile must advance it.
// Runs on a scratch save it makes in the temporary folder.
#include "Extension/Profile/local_profile.h"
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}

int main() {
    using namespace dingosdk;
    const auto folder = std::filesystem::temp_directory_path() / "reskate-profile-change-tests";
    const auto save = folder / "reskate.sqlite3";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    {
        profile::Store store(save);
        auto seen = profile::Store::changes();
        const auto announced = [&] {
            const auto now = profile::Store::changes();
            const bool changed = now != seen;
            seen = now;
            return changed;
        };
        store.set_native_profile_option(2, "UseHighCam", true);
        check(announced(), "a first native option save is announced");
        store.set_native_profile_option(2, "UseHighCam", false);
        check(announced() && store.native_profile_option(2, "UseHighCam") == Json(false),
            "a changed native option is announced");
        store.set_native_profile_option(2, "UseHighCam", false);
        check(!announced(), "saving the same native option again announces nothing");
        store.set_user_value("enableCompass", false);
        check(announced() && store.user_value("enableCompass") == Json(false), "a Boolean setting save is announced");
        store.set_user_value("grindAssist", 20.0);
        check(announced() && store.user_value("grindAssist") == Json(20.0), "a number setting save is announced");
        store.set_user_values({{"cameraMode", 2}, {"enableScoringTicker", false}});
        check(announced(), "several settings saved together are announced");
        store.set_bool_option("gp_use_high_cam", false);
        check(announced(), "a profile option save is announced");
        store.set_user_value("ReSkate.MusicShuffle", true);
        check(announced() && store.user_value("ReSkate.MusicShuffle") == Json(true),
            "the music shuffle preference is saved as a Boolean profile option");
        store.set_selected_cosmetic_preset(3);
        check(announced(), "a save outside the settings is announced");
    }
    {
        // A restart: the saved values and their types (a whole number saved as a float stays one).
        profile::Store store(save);
        const auto number = store.user_value("grindAssist");
        check(store.native_profile_option(2, "UseHighCam") == Json(false) &&
            store.user_value("enableCompass") == Json(false) && store.user_value("cameraMode") == Json(2) &&
            store.user_value("ReSkate.MusicShuffle") == Json(true) &&
            number && number->is_number_float() && number->get<double>() == 20.0 &&
            store.selected_cosmetic_preset() == 3, "saved settings come back after a restart");
    }
    std::filesystem::remove_all(folder, error);
    if (failures) return 1;
    std::cout << "profile changes: ok\n";
    return 0;
}
