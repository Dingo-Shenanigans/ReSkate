#include "Engine/Core/Platform/user_data_paths.h"
#include <cstdio>
#include <fstream>
#include <functional>

namespace {
namespace fs = std::filesystem;
namespace data = dingosdk::user_data;
int failures{};
void check(bool condition, const char* message) {
    if (!condition) { std::printf("FAIL: %s\n", message); ++failures; }
}
bool throws(const std::function<void()>& operation) {
    try { operation(); return false; } catch (const std::exception&) { return true; }
}
struct Environment {
    const wchar_t* name;
    std::optional<std::wstring> saved;
    explicit Environment(const wchar_t* key) : name(key), saved(data::detail::environment(key)) {}
    ~Environment() { SetEnvironmentVariableW(name, saved ? saved->c_str() : nullptr); }
};
void put(const wchar_t* key, const fs::path& path) {
    if (!SetEnvironmentVariableW(key, path.c_str())) throw std::runtime_error("Cannot set test environment");
}
void gate_child(std::string_view mode) {
    const auto selected = fs::path(*data::detail::environment(L"RESKATE_DATA_ROOT"));
    const auto original_local = data::detail::environment(L"LOCALAPPDATA");
    check(!data::runtime_development_selected(), "runtime selection requires completed startup preflight");
    if (mode == "absent") SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", nullptr);
    if (mode == "empty") SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", L"");
    if (mode == "relative") put(L"RESKATE_DATA_ROOT", fs::path(L"relative"));
    if (mode == "oversized") SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", std::wstring(32760, L'x').c_str());
    if (mode == "blocked") { std::ofstream(selected) << "blocker\n"; }
    const bool startup_failed = throws([] { data::initialize_development_root(); });
    if (!mode.starts_with("valid") && mode != "local" && mode != "local-absent" && mode != "local-empty" && mode != "repeat") {
        check(mode == "absent" || startup_failed, "invalid/empty/oversized/blocked startup fails preflight");
        check(!data::runtime_development_selected(), "missing or failed startup cannot select runtime gates");
        if (mode == "blocked") fs::remove(selected);
        put(L"RESKATE_DATA_ROOT", selected);
        data::initialize_development_root();
        check(!data::runtime_development_selected(), "later bootstrap cannot arm a missing/failed first startup");
        return;
    }
    check(!startup_failed && data::runtime_development_selected(), "successful startup selects read-only runtime gate");
    check(data::runtime_development_selected(selected.filename().native(), selected.native()), "basename and validated session root match");
    check(!data::runtime_development_selected(L"wrong-profile") &&
        !data::runtime_development_selected({}, (selected / L"other").native()), "wrong basename/session root cannot authorize a gate");
    SetLastError(0x1234);
    for (unsigned i=0;i<10000;++i) check(data::runtime_development_selected(), "unchanged runtime identity stays selected");
    check(GetLastError()==0x1234, "read-only gate preserves caller LastError");
    if (mode == "repeat") {
        data::initialize_development_root();
        check(data::runtime_development_selected(), "identical repeated preflight does not change valid selection");
    }
    if (mode == "local") put(L"LOCALAPPDATA", selected / L"changed-local");
    else if (mode == "local-absent") SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
    else if (mode == "local-empty") SetEnvironmentVariableW(L"LOCALAPPDATA", L"");
    else if (mode == "valid-empty") SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", L"");
    else if (mode == "valid-oversized") SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", std::wstring(32760,L'x').c_str());
    else if (mode == "valid-relative") put(L"RESKATE_DATA_ROOT", fs::path(L"relative"));
    else if (mode == "valid-other") put(L"RESKATE_DATA_ROOT", selected / L"other");
    else SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", nullptr);
    check(!data::runtime_development_selected(), "observed environment removal/change revokes selection");
    put(L"RESKATE_DATA_ROOT", selected);
    SetEnvironmentVariableW(L"LOCALAPPDATA", original_local ? original_local->c_str() : nullptr);
    check(!data::runtime_development_selected(), "restoring environment cannot revive revoked selection");
    data::initialize_development_root();
    check(!data::runtime_development_selected(), "reinitialization cannot revive revoked selection");
    put(L"RESKATE_DATA_ROOT", fs::path(L"relative"));
    check(throws([] { (void)data::override_root(); }), "actual I/O root still rejects invalid current path after startup");
    put(L"LOCALAPPDATA", selected.parent_path());
    put(L"RESKATE_DATA_ROOT", selected.parent_path() / L"ReSkate" / L"nested");
    check(throws([] { (void)data::override_root(); }), "actual I/O still rechecks current production overlap");
}
bool run_child(const wchar_t* module, const fs::path& scratch, std::wstring arguments) {
    std::wstring command = L"\"" + std::wstring(module) + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(module, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, scratch.c_str(), &startup, &process)) return false;
    const auto wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD exit_code=1; GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exit_code == 0;
}
void child_check() {
    data::initialize_development_root();
    const auto root = data::root();
    check(data::known_folder_root() == root, "child Known Folder and environment consumers share the explicit root");
    check(fs::path(*data::detail::environment(L"TEMP")) == root / L"tmp", "child temporary directory stays isolated");
    std::ofstream(root / L"child-marker.txt") << "isolated\n";
}
}

int main(int argc, char** argv) {
    try {
        if (argc > 2 && std::string_view(argv[1]) == "--gate-child") {
            gate_child(argv[2]);
            return failures ? 1 : 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--child-check") {
            child_check();
            return failures ? 1 : 0;
        }
        Environment keep_root(L"RESKATE_DATA_ROOT"), keep_local(L"LOCALAPPDATA"), keep_temp(L"TEMP"),
            keep_tmp(L"TMP"), keep_logs(L"RESKATE_LOG_DIRECTORY"), keep_favorites(L"RESKATE_FAVORITES_FILE");
        const auto scratch = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("build/bmx/path-fixtures")) /
            (L"run-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(scratch);
        const auto fake_local = scratch / L"local";
        put(L"LOCALAPPDATA", fake_local);
        SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", nullptr);
        check(!data::development_requested() && !data::override_root(), "no override retains normal operation");
        check(data::root() == fake_local / L"ReSkate", "ordinary profile path preserves LOCALAPPDATA default");
        check(data::launcher_settings(scratch / L"install") == scratch / L"install" / L"ReSkateLauncher.settings.json",
            "ordinary launcher preferences remain beside the executable");
        check(throws([] { data::validate_override(L"relative\\folder"); }), "relative override is rejected");
        check(throws([] { data::validate_override(L""); }), "empty override is rejected");
        check(throws([&] { data::validate_override(scratch.root_path().wstring()); }), "drive-root override is rejected");
        check(throws([&] { data::validate_override((fake_local / L"ReSkate").wstring()); }), "normal ReSkate root cannot be selected as development data");
        check(throws([&] { data::validate_override((fake_local / L"ReSkate" / L"nested").wstring()); }), "nested production profile is not isolation");
        check(throws([&] { data::validate_override(fake_local.wstring()); }), "ancestor of production profile is rejected");
        const auto isolated = scratch / L"isolated-\u03b2";
        std::vector<std::wstring> flags{L"--offline", L"--dev-data-root=" + isolated.wstring(), L"--no-update"};
        data::consume_development_root_option(flags);
        check(flags == std::vector<std::wstring>{L"--offline", L"--no-update"}, "development flag is removed instead of forwarded to the game");
        data::initialize_development_root();
        check(data::root() == isolated && data::known_folder_root() == isolated, "both root conventions obey a Unicode override");
        check(fs::is_directory(isolated / L"logs") && fs::is_directory(isolated / L"tmp"), "preflight creates only isolated working folders");
        check(fs::path(*data::detail::environment(L"TEMP")) == isolated / L"tmp" &&
            fs::path(*data::detail::environment(L"TMP")) == isolated / L"tmp", "temporary APIs receive an isolated process root");
        check(fs::path(*data::detail::environment(L"RESKATE_LOG_DIRECTORY")) == isolated / L"logs", "log environment is isolated");
        check(data::launcher_settings(scratch / L"install").parent_path() == isolated, "development launcher settings are isolated");
        for (const auto& relative : {fs::path(L"profiles/offline/reskate.sqlite3"), fs::path(L"profiles/offline/objects.sqlite3"),
            fs::path(L"Game/Skate/data/settings"), fs::path(L"trainer/trainer.json"), fs::path(L"grindexit/control.txt"),
            fs::path(L"BoardBreak.settings"), fs::path(L"cache/build/engine-settings.txt")}) {
            const auto file = data::root() / relative;
            fs::create_directories(file.parent_path());
            std::ofstream(file) << "synthetic fixture\n";
            check(fs::is_regular_file(file), "representative profile/game/feature writes stay below the isolated root");
        }
        check(!fs::exists(fake_local), "normal profile tree was never created or written");
        std::array<wchar_t, 32768> module{};
        const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (!length || length >= module.size()) throw std::runtime_error("Cannot resolve path-test executable");
        std::wstring command = L"\"" + std::wstring(module.data(), length) + L"\" --child-check";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        check(CreateProcessW(module.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, scratch.c_str(), &startup, &process) != FALSE, "synthetic child starts with inherited development root");
        if (process.hProcess) {
            const auto wait = WaitForSingleObject(process.hProcess, 10000);
            DWORD exit_code = 1;
            GetExitCodeProcess(process.hProcess, &exit_code);
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
            check(wait == WAIT_OBJECT_0 && exit_code == 0 && fs::exists(isolated / L"child-marker.txt"),
                "launcher-style child uses the same isolated root without touching the game");
        }
        for (const auto* mode : {L"valid", L"valid-empty", L"valid-oversized", L"valid-relative", L"valid-other",
            L"local", L"local-absent", L"local-empty", L"repeat", L"absent", L"empty", L"relative", L"oversized", L"blocked"}) {
            put(L"RESKATE_DATA_ROOT", scratch / (L"gate-" + std::wstring(mode)));
            check(run_child(module.data(), scratch, L"--gate-child " + std::wstring(mode)), "runtime selection child regression passes");
        }
        put(L"RESKATE_DATA_ROOT", isolated);
        std::vector<std::wstring> missing{L"--dev-data-root"};
        check(throws([&] { data::consume_development_root_option(missing); }), "missing flag value is rejected");
        std::vector<std::wstring> duplicate{L"--dev-data-root=" + isolated.wstring(), L"--dev-data-root=" + isolated.wstring()};
        check(throws([&] { data::consume_development_root_option(duplicate); }), "duplicate development flags are rejected");
        put(L"RESKATE_DATA_ROOT", fs::path("relative"));
        check(throws([] { (void)data::root(); }) && throws([] { (void)data::known_folder_root(); }) &&
            throws([] { data::initialize_development_root(); }), "invalid requested override never falls back to production data");
        const auto blocker = scratch / L"file-not-directory";
        std::ofstream(blocker) << "blocker\n";
        put(L"RESKATE_DATA_ROOT", blocker);
        check(throws([] { data::initialize_development_root(); }), "unwritable/not-directory override fails preflight");
        check(!fs::exists(fake_local), "failure cases still never create the normal profile");
    } catch (const std::exception& error) {
        std::printf("FAIL: unexpected exception: %s\n", error.what());
        return 1;
    }
    if (failures) { std::printf("User-data isolation: %d failures\n", failures); return 1; }
    std::puts("User-data isolation: defaults, overrides, synthetic writers, failure boundaries and child inheritance passed.");
    return 0;
}
