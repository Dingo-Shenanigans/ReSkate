// Actual Boot selection/startup wiring with real process environment and root
// validation. Native image qualification intentionally refuses; no hooks, game
// memory, settings I/O, original query or native storage entry is exercised.
#include "local_settings_selection_fixture.h"
#include "Extension/Boot/offline_local_settings.cpp"
#include <cstdio>
#include <iostream>

namespace dingosdk::local_settings_selection_fixture {
void unexpected_hook_teardown() noexcept { fail("unexpected hook transaction teardown"); }
void environment(const wchar_t* name, const wchar_t* value) {
    require(SetEnvironmentVariableW(name, value) != 0, "could not set isolated test-process environment");
}
void baseline(const std::filesystem::path& root, bool steam_offline) {
    auto& f = leaves();
    f = {};
    f.redirect_root = root;
    environment(L"RESKATE_DATA_ROOT", root.c_str());
    environment(L"RESKATE_OFFLINE_LOCAL_SETTINGS", L"1");
    environment(L"RESKATE_OFFLINE", steam_offline ? L"1" : L"0");
    environment(L"RESKATE_GLOBAL_OFFLINE", L"1");
    environment(L"RESKATE_DEBUG_TEST_GLOBAL_OFFLINE", nullptr);
    const auto selected = user_data::override_root();
    require(selected && same_path(*selected, root), "real root validation changed the selected isolated root");
}
void selected_state(State& s, const std::filesystem::path& root) {
    s.base.store(image_base, std::memory_order_release);
    s.root = root;
    // This constructed authority is only for the private current-selection
    // boundary. It makes no claim that the native hooks or image qualified.
    require(s.authority.begin({L"1", true, true, true, true}) && s.authority.publish(true),
        "could not construct current-selection policy authority");
}
void startup(bool opted_out, unsigned expected_image_probes) {
    std::string error = "not cleared";
    SetLastError(902);
    const bool result = start_offline_local_settings(image_base, error);
    require(GetLastError() == 902, "actual startup changed its caller's LastError");
    require(leaves().image_probes == expected_image_probes, "actual startup crossed the wrong selection boundary");
    if (opted_out) require(result && error.empty(), "absent opt-in installed or refused instead of remaining unselected");
    else require(!result && !error.empty(), "startup did not stop at its intended deliberate refusal");
}

void run(std::wstring_view mode, const std::filesystem::path& root) {
    const bool steam_offline = mode.find(L"steam_offline") != std::wstring_view::npos;
    baseline(root, steam_offline);
    State s;
    selected_state(s, root);
    // Separate entry cases let the old helper's negative control fail at each
    // actual boundary, rather than letting a current-selection failure hide
    // the startup wiring regression. Disconnected controls remain functional.
    if (mode == L"connected_startup" || mode == L"steam_offline_startup") {
        startup(false, 1);
        return;
    }
    require(selection_matches(s), "positive current selection depends on Steam-offline mode");
    require(selection_current(s), "positive current authority depends on Steam-offline mode");
    if (mode == L"connected_current" || mode == L"steam_offline_current") {
        startup(false, 1);
        return;
    }
    if (mode == L"global_missing_connected" || mode == L"global_missing_steam_offline") {
        leaves().global_requested = false;
        environment(L"RESKATE_GLOBAL_OFFLINE", nullptr);
    } else if (mode == L"global_inactive_connected" || mode == L"global_inactive_steam_offline") {
        leaves().global_active = false;
    } else if (mode == L"invalid_optin") {
        for (const auto* flag : {L"", L"0", L"true", L"01", L"1 ", L" 1"}) {
            environment(L"RESKATE_OFFLINE_LOCAL_SETTINGS", flag);
            require(requested() == Request::invalid && !selection_matches(s), "actual environment parser accepted an inexact opt-in");
        }
    } else if (mode == L"absent_optin") {
        environment(L"RESKATE_OFFLINE_LOCAL_SETTINGS", nullptr);
        require(requested() == Request::absent && !selection_matches(s), "absent selection became authority");
        startup(true, 0);
        return;
    } else if (mode == L"invalid_root") {
        environment(L"RESKATE_DATA_ROOT", L"relative-test-profile");
    } else if (mode == L"missing_root") {
        environment(L"RESKATE_DATA_ROOT", nullptr);
    } else if (mode == L"redirect_unready") {
        leaves().redirect_ready = false;
    } else if (mode == L"redirect_mismatch") {
        leaves().redirect_root = root / L"different-redirect";
    } else if (mode == L"root_drift" || mode == L"global_drift" || mode == L"optin_drift" || mode == L"active_drift") {
        if (mode == L"root_drift") {
            const auto other = root / L"different-selection";
            environment(L"RESKATE_DATA_ROOT", other.c_str());
        }
        if (mode == L"global_drift") {
            leaves().global_requested = false;
            environment(L"RESKATE_GLOBAL_OFFLINE", nullptr);
        }
        if (mode == L"optin_drift") environment(L"RESKATE_OFFLINE_LOCAL_SETTINGS", L"0");
        if (mode == L"active_drift") leaves().global_active = false;
        leaves().expect_revocation_log = true;
        require(!selection_current(s) && s.authority.phase() == policy::Phase::revoked &&
            leaves().revocation_logs == 1, "actual selection drift did not revoke authority");
        // Callback error preservation belongs to gate/configuration composition
        // tests; selection_matches itself reads real environment APIs here.
        const auto logs = leaves().revocation_logs;
        environment(L"RESKATE_DATA_ROOT", root.c_str());
        environment(L"RESKATE_OFFLINE_LOCAL_SETTINGS", L"1");
        environment(L"RESKATE_GLOBAL_OFFLINE", L"1");
        leaves().global_requested = leaves().global_active = true;
        require(selection_matches(s) && !selection_current(s) && leaves().revocation_logs == logs &&
            !s.authority.begin({L"1", true, true, true, true}) && !s.authority.publish(true),
            "restoring actual selection facts rearmed revoked authority");
        startup(false, 1);
        return;
    } else {
        fail("unknown scenario");
    }
    require(!selection_matches(s), "missing required authority still matched actual selection");
    startup(false, 0);
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace dingosdk::local_settings_selection_fixture;
    if (argc != 3) fail("expected scenario and absolute isolated build-root arguments");
    try {
        // The fixture never calls initialize_development_root or creates files.
        // Actual validation still rejects overlap with both normal profiles.
        const auto root = dingosdk::user_data::validate_override(argv[2]);
        run(argv[1], root);
        std::cout << "Actual Boot selection boundary passed; no native I/O or hooks executed\n";
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL actual local-settings selection: %s\n", error.what());
        return 1;
    }
    return 0;
}
