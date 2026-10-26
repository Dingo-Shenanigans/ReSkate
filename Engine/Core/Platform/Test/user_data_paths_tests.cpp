// The user-data root: the normal folders without RESKATE_DATA_ROOT, everything under the requested
// folder with it (also in a child process), and a refusal, never the normal profile, for a folder
// that cannot be used. LOCALAPPDATA points at a scratch folder throughout, which must stay empty.
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
// Puts a variable back as it was when the test started.
struct Environment {
    const wchar_t* name;
    std::optional<std::wstring> saved;
    explicit Environment(const wchar_t* key) : name(key), saved(data::detail::environment(key)) {}
    ~Environment() { SetEnvironmentVariableW(name, saved ? saved->c_str() : nullptr); }
};
void put(const wchar_t* key, const fs::path& path) {
    if (!SetEnvironmentVariableW(key, path.c_str())) throw std::runtime_error("Cannot set test environment");
}
// Run as a child of the test: it inherits the root the parent set up, as the game does from the launcher.
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
        if (argc > 1 && std::string_view(argv[1]) == "--child-check") {
            child_check();
            return failures ? 1 : 0;
        }
        Environment keep_root(L"RESKATE_DATA_ROOT"), keep_local(L"LOCALAPPDATA"), keep_temp(L"TEMP"),
            keep_tmp(L"TMP"), keep_logs(L"RESKATE_LOG_DIRECTORY"), keep_favorites(L"RESKATE_FAVORITES_FILE");
        // Canonical, as the root is: a short name or a junction in the fixture folder still compares equal.
        const auto base = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "reskate-user-data-tests");
        fs::create_directories(base);
        const auto scratch = fs::canonical(base) / (L"run-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(scratch);
        const auto fake_local = scratch / L"local";
        put(L"LOCALAPPDATA", fake_local);
        SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", nullptr);
        check(!data::development_requested() && !data::override_root(), "no override retains normal operation");
        data::initialize_development_root();
        check(!data::development_requested() && !fs::exists(fake_local), "without an override, startup changes nothing");
        check(data::root() == fake_local / L"ReSkate", "ordinary profile path preserves LOCALAPPDATA default");
        check(data::launcher_settings(scratch / L"install") == scratch / L"install" / L"ReSkateLauncher.settings.json",
            "ordinary launcher preferences remain beside the executable");
        check(throws([] { data::validate_override(L"relative\\folder"); }), "relative override is rejected");
        check(throws([] { data::validate_override(L""); }), "empty override is rejected");
        check(throws([&] { data::validate_override(scratch.root_path().wstring()); }), "drive-root override is rejected");
        check(throws([&] { data::validate_override((fake_local / L"ReSkate").wstring()); }), "normal ReSkate root cannot be selected as development data");
        check(throws([&] { data::validate_override((fake_local / L"ReSkate" / L"nested").wstring()); }), "nested production profile is not isolation");
        check(throws([&] { data::validate_override(fake_local.wstring()); }), "ancestor of production profile is rejected");
        check(throws([&] { data::validate_override((scratch / std::wstring(200, L'x')).wstring()); }), "a root too long for the game's folder API is rejected");
        const auto isolated = scratch / L"isolated-\u03b2";
        std::vector<std::wstring> flags{L"--offline", L"--dev-data-root=" + isolated.wstring(), L"--no-update"};
        data::consume_development_root_option(flags);
        check(flags == std::vector<std::wstring>{L"--offline", L"--no-update"}, "development flag is removed instead of forwarded to the game");
        SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", nullptr);
        std::vector<std::wstring> separate{L"--dev-data-root", isolated.wstring(), L"--offline"};
        data::consume_development_root_option(separate);
        check(separate == std::vector<std::wstring>{L"--offline"} && data::development_requested(),
            "a separate folder argument is taken with the flag");
        data::initialize_development_root();
        check(data::root() == isolated && data::known_folder_root() == isolated, "both root conventions obey a Unicode override");
        check(fs::is_directory(isolated / L"logs") && fs::is_directory(isolated / L"tmp"), "preflight creates only isolated working folders");
        check(fs::path(*data::detail::environment(L"TEMP")) == isolated / L"tmp" &&
            fs::path(*data::detail::environment(L"TMP")) == isolated / L"tmp", "temporary APIs receive an isolated process root");
        check(fs::path(*data::detail::environment(L"RESKATE_LOG_DIRECTORY")) == isolated / L"logs", "log environment is isolated");
        check(fs::path(*data::detail::environment(L"RESKATE_FAVORITES_FILE")) == isolated / L"ReSkate.favorites.json",
            "favourites environment is isolated");
        check(data::launcher_settings(scratch / L"install").parent_path() == isolated, "development launcher settings are isolated");
        for (const auto& relative : {fs::path(L"profiles/offline/reskate.sqlite3"), fs::path(L"profiles/offline/objects.sqlite3"),
            fs::path(L"Game/Skate/data/settings"), fs::path(L"trainer/trainer.json"), fs::path(L"thunderstore/icons/icon.png"),
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
        std::vector<std::wstring> missing{L"--dev-data-root"};
        check(throws([&] { data::consume_development_root_option(missing); }), "missing flag value is rejected");
        std::vector<std::wstring> duplicate{L"--dev-data-root=" + isolated.wstring(), L"--dev-data-root=" + isolated.wstring()};
        check(throws([&] { data::consume_development_root_option(duplicate); }), "duplicate development flags are rejected");
        std::vector<std::wstring> normal{L"--dev-data-root=" + (fake_local / L"ReSkate").wstring()};
        check(throws([&] { data::consume_development_root_option(normal); }), "the flag cannot name the normal profile");
        put(L"RESKATE_DATA_ROOT", fs::path("relative"));
        check(data::development_requested(), "an unusable override is still a request");
        check(throws([] { (void)data::root(); }) && throws([] { (void)data::known_folder_root(); }) &&
            throws([] { (void)data::launcher_settings(L"C:\\install"); }) &&
            throws([] { data::initialize_development_root(); }), "invalid requested override never falls back to production data");
        SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", L"");
        check(data::development_requested() && throws([] { (void)data::root(); }), "an empty override is refused, not ignored");
        const auto blocker = scratch / L"file-not-directory";
        std::ofstream(blocker) << "blocker\n";
        put(L"RESKATE_DATA_ROOT", blocker);
        check(throws([] { data::initialize_development_root(); }), "unwritable/not-directory override fails preflight");
        check(!fs::exists(fake_local), "failure cases still never create the normal profile");
        if (!failures) {
            std::error_code ignored;
            fs::remove_all(scratch, ignored);
        }
    } catch (const std::exception& error) {
        std::printf("FAIL: unexpected exception: %s\n", error.what());
        return 1;
    }
    if (failures) { std::printf("User-data isolation: %d failures\n", failures); return 1; }
    std::puts("User-data isolation: defaults, overrides, synthetic writers, failure boundaries and child inheritance passed.");
    return 0;
}
