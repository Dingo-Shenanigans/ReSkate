// The user-data root: the normal folders without RESKATE_DATA_ROOT, everything under the requested
// folder with it (also in a child process), and a refusal, never the normal profile, for a folder
// that cannot be used. LOCALAPPDATA points at a scratch folder throughout; later fake cache
// canaries there must stay unchanged when the actual workers' path selectors use private data.
#include "Engine/Core/Platform/user_data_paths.h"
#include "Extension/News/patch_notes_cache.h"
#include "Extension/UI/Overlay/store_icon_paths.h"
#include <winioctl.h>
#include <algorithm>
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
// Why an operation was refused, or empty when it was not: for a case another rule could also refuse.
std::string refusal(const std::function<void()>& operation) {
    try { operation(); return {}; } catch (const std::exception& error) { return error.what(); }
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
// A directory junction needs no symbolic-link privilege. Its destructor removes only the link,
// so the executable folder and the synthetic normal profile remain intact after a failed check.
struct Junction {
    fs::path link;
    Junction(const fs::path& path, const fs::path& target) : link(path) {
        const auto destination = fs::canonical(target).wstring();
        const auto substitute = destination.starts_with(L"\\\\?\\") ? L"\\??\\" + destination.substr(4)
            : destination.starts_with(L"\\\\") ? L"\\??\\UNC\\" + destination.substr(2) : L"\\??\\" + destination;
        struct Buffer {
            DWORD tag{IO_REPARSE_TAG_MOUNT_POINT};
            USHORT length{}, reserved{}, substitute_offset{}, substitute_length{}, print_offset{}, print_length{};
            wchar_t paths[4096]{};
        } buffer;
        static_assert(offsetof(Buffer, paths) == 16);
        const auto characters = substitute.size() + 1 + destination.size() + 1;
        if (characters > sizeof(buffer.paths) / sizeof(wchar_t))
            throw std::runtime_error("Junction fixture target is too long");
        buffer.substitute_length = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
        buffer.print_offset = static_cast<USHORT>((substitute.size() + 1) * sizeof(wchar_t));
        buffer.print_length = static_cast<USHORT>(destination.size() * sizeof(wchar_t));
        const auto path_bytes = characters * sizeof(wchar_t);
        buffer.length = static_cast<USHORT>(4 * sizeof(USHORT) + path_bytes);
        std::copy(substitute.begin(), substitute.end(), buffer.paths);
        std::copy(destination.begin(), destination.end(), buffer.paths + substitute.size() + 1);
        if (!fs::create_directory(link)) throw std::runtime_error("Cannot create junction fixture directory");
        const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            RemoveDirectoryW(link.c_str());
            throw std::runtime_error("Cannot open junction fixture directory");
        }
        DWORD returned{};
        const auto linked = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer,
            static_cast<DWORD>(offsetof(Buffer, paths) + path_bytes), nullptr, 0, &returned, nullptr);
        const auto error = GetLastError();
        CloseHandle(handle);
        if (!linked) {
            RemoveDirectoryW(link.c_str());
            throw std::runtime_error("Cannot create junction fixture: " + std::to_string(error));
        }
    }
    Junction(const Junction&) = delete;
    Junction& operator=(const Junction&) = delete;
    ~Junction() {
        check(RemoveDirectoryW(link.c_str()) != FALSE, "junction fixture is removed without touching its target");
    }
};
void alias_boundaries(const fs::path& scratch, const fs::path& fake_local, const fs::path& program) {
    const auto aliases = scratch / L"aliases";
    fs::create_directory(aliases);
    const Junction normal(aliases / L"normal profile", fake_local / L"ReSkate");
    check(refusal([&] { data::validate_override(normal.link.wstring()); }).find("normal ReSkate user data") != std::string::npos,
        "a junction to the normal profile is refused for its resolved target");
    check(refusal([&] { data::validate_override((normal.link / L"new-child").wstring()); }).find("normal ReSkate user data") != std::string::npos,
        "a new directory beneath a normal-profile junction is refused");
    const Junction game(aliases / L"game folder", program);
    check(refusal([&] { data::validate_override(game.link.wstring()); }).find("game's folder") != std::string::npos,
        "a junction to the game folder is refused for its resolved target");
    check(data::validate_override((game.link / L"test-profile").wstring()) == data::detail::canonical(program / L"test-profile"),
        "a separate profile inside the game folder is accepted through its junction");
    const auto private_root = scratch / L"private-profile";
    fs::create_directory(private_root);
    const Junction separate(aliases / L"separate profile", private_root);
    check(data::validate_override(separate.link.wstring()) == fs::canonical(private_root),
        "a junction to a separate private root is accepted and resolved");
}
// These are the selectors the actual workers call, without starting a fetch or a render worker.
std::array<fs::path, 3> cache_files() {
    const auto news = dingosdk::overlay::detail::store_icon_paths::folder(true);
    const auto icons = dingosdk::overlay::detail::store_icon_paths::folder(false);
    return {dingosdk::news::patch_notes_cache::file(),
        news.empty() ? fs::path{} : news / L"fixture-news.png",
        icons.empty() ? fs::path{} : icons / L"fixture-icon.png"};
}
std::array<fs::path, 3> expected_cache_files(const fs::path& root) {
    return {root / L"news" / L"releases.json", root / L"news" / L"pictures" / L"fixture-news.png",
        root / L"thunderstore" / L"icons" / L"fixture-icon.png"};
}
void write_fixture(const fs::path& file, const char* text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    check(out.good(), "synthetic cache fixture was written");
}
std::string read_fixture(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void cache_isolation(const fs::path& scratch, const fs::path& fake_local) {
    const auto isolated = scratch / L"cache-\u03b2";
    put(L"RESKATE_DATA_ROOT", isolated);
    data::initialize_development_root();
    const auto ordinary = expected_cache_files(fake_local / L"ReSkate");
    const auto selected = cache_files();
    const auto expected = expected_cache_files(isolated);
    check(selected == expected, "actual patch-note/news-picture/icon selectors use the Unicode private root");
    // A broken selector must fail the test before the fixture follows it outside its private tree.
    if (selected != expected) return;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        write_fixture(ordinary[i], "normal fake cache\n");
        write_fixture(expected[i], "private cache\n");
        check(read_fixture(selected[i]) == "private cache\n", "actual cache paths read private fixtures rather than normal canaries");
        write_fixture(selected[i], "private cache changed\n");
        check(read_fixture(ordinary[i]) == "normal fake cache\n", "actual cache writes leave normal fake canaries unchanged");
    }
    put(L"RESKATE_DATA_ROOT", fs::path(L"relative"));
    check(cache_files() == std::array<fs::path, 3>{}, "invalid root disables all three worker caches without throwing");
    SetEnvironmentVariableW(L"RESKATE_DATA_ROOT", L"");
    check(cache_files() == std::array<fs::path, 3>{}, "empty requested root never selects the normal worker caches");
    const auto blocker = scratch / L"cache-blocker";
    std::ofstream(blocker) << "not a directory\n";
    put(L"RESKATE_DATA_ROOT", blocker);
    check(throws([] { data::initialize_development_root(); }), "a file root refuses startup before any cache worker");
    for (const auto& file : cache_files()) {
        check(file.empty() || data::detail::within(file, blocker), "a refused startup cannot redirect a cache to normal data");
        if (!file.empty() && data::detail::within(file, blocker)) {
            std::error_code error;
            fs::create_directories(file.parent_path(), error);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            check(!out, "a cache below the file root cannot be written");
        }
    }
    for (const auto& file : ordinary)
        check(read_fixture(file) == "normal fake cache\n", "refused roots leave every normal fake cache canary unchanged");
}
// Run as a child of the test: it inherits the root the parent set up, as the game does from the launcher.
void child_check() {
    data::initialize_development_root();
    const auto root = data::root();
    check(data::known_folder_root() == root, "child Known Folder and environment consumers share the explicit root");
    check(fs::path(*data::detail::environment(L"TEMP")) == root / L"tmp", "child temporary directory stays isolated");
    const auto selected = cache_files();
    check(selected == expected_cache_files(root), "child worker cache selectors inherit the same private root");
    if (selected != expected_cache_files(root)) return;
    for (const auto& file : selected) write_fixture(file, "child private cache\n");
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
        check(cache_files() == expected_cache_files(fake_local / L"ReSkate"), "without an override the worker cache paths keep their normal locations");
        check(throws([] { data::validate_override(L"relative\\folder"); }), "relative override is rejected");
        check(throws([] { data::validate_override(L""); }), "empty override is rejected");
        check(throws([&] { data::validate_override(scratch.root_path().wstring()); }), "drive-root override is rejected");
        check(throws([&] { data::validate_override((fake_local / L"ReSkate").wstring()); }), "normal ReSkate root cannot be selected as development data");
        check(throws([&] { data::validate_override((fake_local / L"ReSkate" / L"nested").wstring()); }), "nested production profile is not isolation");
        check(throws([&] { data::validate_override(fake_local.wstring()); }), "ancestor of production profile is rejected");
        check(throws([&] { data::validate_override((scratch / std::wstring(200, L'x')).wstring()); }), "a root too long for the game's folder API is rejected");
        // The test's own folder stands in for the game's, where the launcher keeps its settings and logs.
        std::array<wchar_t, 32768> module{};
        const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (!length || length >= module.size()) throw std::runtime_error("Cannot resolve path-test executable");
        const auto program = fs::path(std::wstring(module.data(), length)).parent_path();
        check(refusal([&] { data::validate_override(program.wstring()); }).find("game's folder") != std::string::npos,
            "the game's folder, which holds the launcher's settings and logs, is rejected");
        check(!throws([&] { data::validate_override((program / L"test-profile").wstring()); }),
            "a folder inside the game's folder is still accepted");
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
            const auto child_files = cache_files();
            check(child_files == expected_cache_files(isolated), "parent still selects the private child cache paths");
            if (child_files == expected_cache_files(isolated))
                for (const auto& file : child_files)
                    check(read_fixture(file) == "child private cache\n", "child writes through actual worker selectors stay private");
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
        cache_isolation(scratch, fake_local);
        alias_boundaries(scratch, fake_local, program);
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
