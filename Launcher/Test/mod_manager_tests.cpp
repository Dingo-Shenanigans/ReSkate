// Launcher mod manager: install from .zip and folders, the Mods list and mods.json.
#include "Launcher/mod_manager.h"

#include "Engine/Vfs/mod_list.h"

#include <Windows.h>
#include <winioctl.h>

#include <miniz.h>

#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace dingosdk;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

// A stored (uncompressed) .zip, enough to exercise the reader.
void make_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, directory;
    const auto u16 = [](std::string& s, unsigned v) { s += char(v & 0xff); s += char(v >> 8 & 0xff); };
    const auto u32 = [](std::string& s, unsigned long v) { for (int i = 0; i < 4; ++i) s += char(v >> (8 * i) & 0xff); };
    for (const auto& [name, data] : files) {
        const auto crc = mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.data()), data.size());
        const auto offset = out.size();
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, crc); u32(out, static_cast<unsigned long>(data.size())); u32(out, static_cast<unsigned long>(data.size()));
        u16(out, static_cast<unsigned>(name.size())); u16(out, 0);
        out += name; out += data;
        u32(directory, 0x02014b50); u16(directory, 20); u16(directory, 20); u16(directory, 0); u16(directory, 0);
        u16(directory, 0); u16(directory, 0); u32(directory, crc);
        u32(directory, static_cast<unsigned long>(data.size())); u32(directory, static_cast<unsigned long>(data.size()));
        u16(directory, static_cast<unsigned>(name.size())); u16(directory, 0); u16(directory, 0); u16(directory, 0);
        u16(directory, 0); u32(directory, name.ends_with('/') ? 0x10 : 0); u32(directory, static_cast<unsigned long>(offset));
        directory += name;
    }
    const auto start = out.size();
    out += directory;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0);
    u16(out, static_cast<unsigned>(files.size())); u16(out, static_cast<unsigned>(files.size()));
    u32(out, static_cast<unsigned long>(directory.size())); u32(out, static_cast<unsigned long>(start)); u16(out, 0);
    write(path, out);
}

std::string install(const fs::path& mods, const fs::path& source, bool replace = false) {
    std::atomic<bool> cancel{};
    return launcher_mods::install(mods, source, replace, {}, cancel);
}

template<class F> std::string error_of(F&& run) {
    try { run(); } catch (const std::exception& failure) { return failure.what(); }
    return {};
}

// Junctions exercise the same no-follow path as symlinks, without requiring
// Developer Mode or SeCreateSymbolicLinkPrivilege. Failure is a test failure,
// never a silently skipped safety check.
bool junction(const fs::path& link, const fs::path& target) {
    fs::create_directories(link);
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const auto substitute = L"\\??\\" + target.wstring();
    const auto print = target.wstring();
    struct Mount {
        DWORD tag{IO_REPARSE_TAG_MOUNT_POINT};
        WORD length{}, reserved{}, substitute_offset{}, substitute_length{}, print_offset{}, print_length{};
        wchar_t buffer[4096]{};
    } data;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.buffer);
    std::copy(print.begin(), print.end(), data.buffer + substitute.size() + 1);
    data.length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
    DWORD returned{};
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.length + 8,
        nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
}

void reset_tests(const fs::path& fixtures, const fs::path& sources) {
    using namespace launcher_mods;
    const auto fixture = [&](const wchar_t* name) {
        const auto game = fixtures / name;
        write(game / L"Skate.exe", "game");
        return game;
    };
    const auto reset = [](const fs::path& game) { return execute_factory_reset(prepare_factory_reset(game)); };

    // A/B/C/K/L: both layouts individually, full coverage, preservation,
    // idempotency and reinstall. Enabled state never narrows the reset scope.
    for (const bool mod_data : {false, true}) {
        const auto game = fixture(mod_data ? L"moddata" : L"legacy");
        if (mod_data) fs::create_directories(game / L"ModData" / L"Default");
        const auto root = mods_root(game);
        check(root == (mod_data ? game / L"ModData" / L"Default" / L"Mods" : game / L"Mods"),
            "Reset discovery uses the game's default data path");
        for (const auto* name : {L"One", L"Two", L"Disabled"}) {
            write(root / name / L"layout.toc", "toc");
            write(root / name / L"nested" / L"assets.bin", "assets");
        }
        auto list = mods::scan_mods(root.parent_path());
        list.entries.back().enabled = false;
        mods::save_mod_order(root, list.entries);
        write(root / mods::exclusions_file, "{}");
        write(root / L".reskate" / L"merged" / L"asset.cas", "merged");
        write(root / L".reskate-install" / L"pending" / L"file", "temporary");
        write(root / L".reskate-download" / L"package.zip", "zip");
        write(root / L"mods.json.tmp", "temporary metadata");
        write(root / L".reskate-excluded.json.tmp", "temporary exclusions");
        for (const auto& file : {game / L"ReSkateLauncher.exe", game / L"ReSkate.dll", game / L"Data" / L"layout.toc",
                game / L"ReSkate.settings.json", game / L"logs" / L"launcher.log", game / L"personal.txt",
                game / L"savegames" / L"save", game / L"steam_api64.dll"}) write(file, "preserve");
        const auto plan = prepare_factory_reset(game);
        check(plan.errors.empty() && plan.targets.size() == 1 && plan.mod_count == 3 && plan.estimated_bytes > 0,
            "Preflight includes enabled, disabled, nested and generated data");
        check(fs::exists(root / L"Disabled" / L"layout.toc"), "Preflight does not modify mod files");
        const auto result = execute_factory_reset(plan);
        if (!result.success) for (const auto& error : result.errors) std::cerr << error << '\n';
        check(result.success && result.errors.empty() && result.remaining.empty() && result.removed_items > 3 &&
                result.removed_bytes == plan.estimated_bytes && !fs::exists(root), "Full reset permanently removes the Mods tree");
        check(read(game / L"Skate.exe") == "game" && read(game / L"Data" / L"layout.toc") == "preserve" &&
                read(game / L"ReSkate.dll") == "preserve" && read(game / L"ReSkateLauncher.exe") == "preserve" &&
                read(game / L"ReSkate.settings.json") == "preserve" && read(game / L"personal.txt") == "preserve" &&
                read(game / L"logs" / L"launcher.log") == "preserve" && read(game / L"savegames" / L"save") == "preserve" &&
                read(game / L"steam_api64.dll") == "preserve", "Game, settings, logs, saves and personal files are preserved");
        if (mod_data) check(fs::is_directory(root.parent_path()), "ModData/Default itself is preserved");
        const auto empty = mods::scan_mods(root.parent_path());
        check(!empty.present && empty.entries.empty() && empty.missing.empty() && empty.excluded.empty(),
            "Rescanning a reset installation has no stale order, exclusions or enabled mods");
        check(reset(game).success && !fs::exists(root), "A second reset succeeds without recreating Mods");
        check(install(root, sources / L"v2.zip") == "Cool Park" &&
                mods::scan_mods(root.parent_path()).entries.size() == 1, "A new mod installs and is discovered after reset");
        check(reset(game).success, "Reinstalled mods can be reset again");
    }

    // C/J/I: independent roots; one locked file must not prevent cleanup of the
    // other root or siblings, and must never be reported as success.
    {
        const auto game = fixture(L"both");
        const auto old = game / L"Mods";
        const auto current = game / L"ModData" / L"Default" / L"Mods";
        write(old / L"Old" / L"layout.toc", "old");
        write(old / L"Independent" / L"layout.toc", "independent");
        write(current / L"New" / L"layout.toc", "new");
        const auto plan = prepare_factory_reset(game);
        check(plan.errors.empty() && plan.targets.size() == 2 && plan.mod_count == 3,
            "Both known managed layouts are included");
        const auto file = old / L"Old" / L"layout.toc";
        const auto lock = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "Locked-file fixture opens");
        const auto result = execute_factory_reset(plan);
        check(!result.success && !result.errors.empty() && !result.remaining.empty() && result.removed_items > 0 &&
                fs::exists(file) && !fs::exists(old / L"Independent") && !fs::exists(current),
            "Partial reset reports locked paths and continues independent safe targets");
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        check(reset(game).success && !fs::exists(old), "Partial cleanup can be retried after a lock is released");
        write(old / L"Orphan" / L"layout.toc", "readonly");
        check(SetFileAttributesW((old / L"Orphan" / L"layout.toc").c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
            "Read-only fixture is set");
        const auto readonly = reset(game);
        check(!readonly.success && !readonly.errors.empty() && !readonly.remaining.empty(), "Read-only failure never claims success");
        SetFileAttributesW((old / L"Orphan" / L"layout.toc").c_str(), FILE_ATTRIBUTE_NORMAL);
        check(reset(game).success, "Read-only failure is retryable");
        write(old / L"mods.json", "{}");
        write(current / L".reskate" / L"generated", "generated");
        check(reset(game).success && !fs::exists(old) && !fs::exists(current), "Both layouts are fully removed on success");
    }

    // D/E/F: absent/empty directories, metadata without mods and corrupt JSON.
    {
        const auto game = fixture(L"empty");
        check(reset(game).success && !fs::exists(game / L"Mods") && !fs::exists(game / L"ModData"),
            "An empty installation succeeds without creating unrelated directories");
        const auto root = game / L"Mods";
        fs::create_directories(root);
        check(reset(game).success && !fs::exists(root), "An empty active Mods root is removed");
        write(root / L"mods.json", "invalid JSON [");
        write(root / mods::exclusions_file, "also invalid");
        write(root / L".reskate" / L"merged", "orphan");
        const auto plan = prepare_factory_reset(game);
        check(plan.errors.empty() && plan.mod_count == 0 && plan.targets.size() == 1,
            "Orphaned managed state is discovered without parsing JSON");
        check(execute_factory_reset(plan).success && !fs::exists(root), "Malformed metadata and orphaned merge data are removed");
        write(game / L"ModData" / L"Default" / L"Mods" / L"Only" / L"layout.toc", "mod");
        const auto vanished = prepare_factory_reset(game);
        fs::remove_all(game / L"ModData");
        check(execute_factory_reset(vanished).success, "Already missing targets and layout parents are harmless");
    }

    // G: even a modified plan cannot authorise arbitrary folders or traversal.
    {
        const auto game = fixture(L"unsafe");
        write(game / L"Mods" / L"One" / L"layout.toc", "safe");
        write(game / L"personal" / L"keep", "preserve");
        const auto plan = prepare_factory_reset(game);
        if (plan.targets.empty()) { check(false, "Unsafe-target fixture requires a validated reset plan"); return; }
        for (const auto& path : {fs::path(), game, game.root_path(), game / L"personal",
                game / L"ModData" / L"Default", game / L"Mods" / L".." / L"personal"}) {
            auto forged = plan;
            forged.targets[0].path = path;
            check(!execute_factory_reset(forged).success && read(game / L"personal" / L"keep") == "preserve" &&
                    fs::exists(game / L"Mods" / L"One" / L"layout.toc"), "Unsafe reset targets are rejected without deletion");
        }
        for (const auto& path : {fs::path(), game.root_path(), game / L"Mods" / L".."})
            check(!prepare_factory_reset(path).errors.empty(), "Unsafe game paths fail preflight");
        check(!execute_factory_reset(ResetPlan{}).success, "An empty unresolved plan is rejected");
        // A confirmation plan that is simply discarded has no side effects.
        { const auto cancelled = prepare_factory_reset(game); check(cancelled.errors.empty(), "Cancel fixture preflights"); }
        check(read(game / L"Mods" / L"One" / L"layout.toc") == "safe", "Discarding preflight leaves every file intact");
        check(reset(game).success, "Safe cleanup still works after invalid requests");
    }

    // Inactive folders need ownership evidence; a folder name alone is insufficient.
    {
        const auto game = fixture(L"unowned");
        write(game / L"Mods" / L"personal.txt", "preserve");
        write(game / L"ModData" / L"Default" / L"Mods" / L"New" / L"layout.toc", "mod");
        const auto plan = prepare_factory_reset(game);
        check(plan.targets.size() == 1 && !plan.excluded.empty(), "Inactive unowned Mods roots are excluded and explained");
        check(execute_factory_reset(plan).success && read(game / L"Mods" / L"personal.txt") == "preserve",
            "Unowned inactive roots are preserved");
        write(game / L"Mods" / L"Old" / L"LAYOUT.TOC", "mod");
        const auto case_plan = prepare_factory_reset(game);
        check(case_plan.targets.size() == 1 && case_plan.excluded.empty() && execute_factory_reset(case_plan).success,
            "Inactive ownership markers follow Windows case-insensitive filename rules");
    }

    // H plus changed/new reparse points after confirmation (TOCTOU).
    {
        const auto external = fixtures / L"external";
        write(external / L"keep", "outside");
        const auto game = fixture(L"links");
        write(game / L"Mods" / L"One" / L"layout.toc", "mod");
        const auto link = game / L"Mods" / L"escape";
        const bool linked = junction(link, external);
        check(linked, "Junction escape fixture must execute (no silent skip)");
        if (linked) {
            const auto plan = prepare_factory_reset(game);
            check(plan.errors.empty() && !plan.excluded.empty() && plan.estimated_bytes == 3,
                "Preflight never counts or traverses junction targets");
            check(execute_factory_reset(plan).success && read(external / L"keep") == "outside",
                "Reset deletes the junction itself and preserves external content");
        }
        write(game / L"Mods" / L"Two" / L"layout.toc", "mod");
        const auto before_link = prepare_factory_reset(game);
        const bool introduced = junction(game / L"Mods" / L"late-link", external);
        check(introduced, "New junction after preflight fixture must execute");
        if (introduced) check(execute_factory_reset(before_link).success && read(external / L"keep") == "outside",
            "Newly introduced junctions cannot redirect deletion");
        write(game / L"Mods" / L"Three" / L"layout.toc", "mod");
        const auto replaced = prepare_factory_reset(game);
        fs::rename(game / L"Mods", game / L"original");
        const bool root_link = junction(game / L"Mods", external);
        check(root_link, "Changed root junction fixture must execute");
        if (root_link) {
            check(!execute_factory_reset(replaced).success && read(external / L"keep") == "outside" &&
                    fs::exists(game / L"original" / L"Three" / L"layout.toc"), "A Mods root replaced by a junction is refused");
            check(!prepare_factory_reset(game).errors.empty(), "A linked Mods root is rejected during preflight");
            fs::remove(game / L"Mods");
        }
        fs::rename(game / L"original", game / L"Mods");
        const auto changed = prepare_factory_reset(game);
        fs::rename(game / L"Mods", game / L"original");
        write(game / L"Mods" / L"Replacement" / L"layout.toc", "new");
        check(!execute_factory_reset(changed).success && fs::exists(game / L"Mods" / L"Replacement" / L"layout.toc"),
            "A different ordinary Mods root invalidates the confirmed identity");
        check(reset(game).success, "Changed roots require a fresh plan");
        write(game / L"Mods" / L"Pinned" / L"layout.toc", "mod");
        const auto pinned = prepare_factory_reset(game);
        bool rename_checked = false;
        const auto pinned_result = execute_factory_reset(pinned, [&](float progress) {
            if (progress >= 0 || rename_checked) return;
            rename_checked = true;
            check(MoveFileExW((game / L"Mods").c_str(), (game / L"moved").c_str(), 0) == FALSE,
                "A pinned root cannot be renamed during deletion");
            const auto writer = CreateFileW((game / L"Mods").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
                FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            check(writer == INVALID_HANDLE_VALUE, "A pinned root denies reparse mutation through a write handle");
            if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
            const auto image = CreateFileW((game / L"Skate.exe").c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            check(image == INVALID_HANDLE_VALUE, "Skate executable is held exclusively while deleting mods");
            if (image != INVALID_HANDLE_VALUE) CloseHandle(image);
        });
        check(pinned_result.success && rename_checked, "Deletion exercises handle pinning against concurrent replacement");
        const auto ancestor_game = fixture(L"linked-parent");
        const bool ancestor = junction(ancestor_game / L"ModData", external);
        check(ancestor, "Linked ancestor fixture must execute");
        if (ancestor) {
            check(!prepare_factory_reset(ancestor_game).errors.empty() && read(external / L"keep") == "outside",
                "Reparse ancestors are refused without traversal");
            fs::remove(ancestor_game / L"ModData");
        }
    }

    // A long path and a hard link: no MAX_PATH failure or modification of the
    // shared external file's attributes/content while unlinking the Mods copy.
    {
        const auto game = fixture(L"long");
        const auto asset = game / L"Mods" / L"One" / std::wstring(150, L'a') / std::wstring(150, L'b') / L"asset";
        write(fs::path(L"\\\\?\\" + asset.wstring()), "long");
        write(game / L"Mods" / L"One" / L"layout.toc", "mod");
        const auto external = fixtures / L"hardlink-original";
        write(external, "preserve");
        check(CreateHardLinkW((game / L"Mods" / L"One" / L"linked.cas").c_str(), external.c_str(), nullptr) != FALSE,
            "Hard-link fixture opens");
        check(reset(game).success && read(external) == "preserve", "Long paths reset and external hard-link content remains intact");
    }

    // N: real installation worker paused inside progress holds the coordinator.
    {
        const auto game = fixture(L"concurrency");
        std::mutex mutex;
        std::condition_variable wake;
        bool entered = false, release = false;
        std::string failure;
        std::thread worker([&] {
            std::atomic<bool> cancel{};
            try {
                launcher_mods::install(game / L"Mods", sources / L"v2.zip", false, [&](float) {
                    std::unique_lock lock(mutex);
                    entered = true;
                    wake.notify_all();
                    wake.wait(lock, [&] { return release; });
                }, cancel);
            } catch (const std::exception& error) { failure = error.what(); }
        });
        {
            std::unique_lock lock(mutex);
            check(wake.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }), "Installation worker reaches progress");
        }
        check(!prepare_factory_reset(game).errors.empty(), "Factory Reset is refused while a real installation runs");
        { std::lock_guard lock(mutex); release = true; }
        wake.notify_all();
        worker.join();
        check(failure.empty(), "Concurrent installation completes normally");
        const auto plan = prepare_factory_reset(game);
        {
            OperationLease resetting(Operation::reset);
            check(static_cast<bool>(resetting), "Reset obtains the operation lease");
            OperationLease second(Operation::reset);
            check(!second && !execute_factory_reset(plan).success, "Two simultaneous Factory Resets are refused");
            check(!error_of([&] { install(game / L"Mods", sources / L"v2.zip", true); }).empty(),
                "Installation is refused during Factory Reset");
            OperationLease launch(Operation::launcher);
            check(!launch, "Game launch/merge is refused during reset");
            check(execute_factory_reset(plan, {}, &resetting).success, "The lease owner can execute reset");
        }
        check(!OperationLease::busy(), "Operation lease is released after completion");
    }

    // A real process with the game's executable name, launched independently
    // of the UI. It is this test binary's wait-only fixture mode, never Skate.
    {
        const auto game = fixture(L"running");
        write(game / L"Mods" / L"One" / L"layout.toc", "mod");
        const auto plan = prepare_factory_reset(game);
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        fs::copy_file(executable, game / L"Skate.exe", fs::copy_options::overwrite_existing);
        auto command = L"\"" + (game / L"Skate.exe").wstring() + L"\" --reset-game-fixture";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const bool started = CreateProcessW((game / L"Skate.exe").c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, game.c_str(), &startup, &process) != FALSE;
        check(started, "Externally launched Skate-name process fixture starts");
        if (started) {
            check(!factory_reset_blocker().empty() && !prepare_factory_reset(game).errors.empty() &&
                    !execute_factory_reset(plan).success && fs::exists(game / L"Mods" / L"One" / L"layout.toc"),
                "A running game outside the launcher blocks preflight and execution");
            TerminateProcess(process.hProcess, 0);
            WaitForSingleObject(process.hProcess, 10000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        check(reset(game).success, "Reset can proceed after the external game process closes");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--reset-game-fixture") { Sleep(30000); return 0; }
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root = fs::path(temp) / (L"reskate-mod-manager-tests-" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    const auto game = root / L"game";
    const auto sources = root / L"sources";
    fs::create_directories(game);
    const auto mods = launcher_mods::mods_root(game);
    check(mods == game / L"Mods", "Without ModData the Mods folder sits beside the game");

    // A mod inside a top-level folder, with its own description.
    make_zip(sources / L"download (1).zip", {
        {"Cool Park/", ""},
        {"Cool Park/layout.toc", "toc"},
        {"Cool Park/Win32/levels/cool.sb", "bundle"},
        {"Cool Park/reskate-mod.json", R"({"schema":1,"name":"Cool Park Deluxe","author":"Zee","version":"1.2","description":"A park."})"},
        {"Cool Park/reskate-levels.json", R"({"schema":1,"levels":[{"asset":"levels/game/cool/cool"}]})"}});
    check(install(mods, sources / L"download (1).zip") == "Cool Park", "A zip's top folder names the mod");
    check(read(mods / L"Cool Park" / L"Win32" / L"levels" / L"cool.sb") == "bundle", "Nested files are extracted");
    check(!fs::exists(mods / L".reskate-install"), "Staging is cleaned up");

    // Files at the top of the zip: the archive's name becomes the folder name.
    make_zip(sources / L"Flat Mod!.zip", {{"reskate-levels.json", R"({"schema":1,"levels":[]})"}});
    check(install(mods, sources / L"Flat Mod!.zip") == "Flat Mod_", "Unsafe characters in names are replaced");

    // Mods packed deeper, as release zips often are.
    make_zip(sources / L"pack.zip", {{"readme.txt", "hi"}, {"pack/Mods/Deep_Mod/layout.toc", "toc"}});
    check(install(mods, sources / L"pack.zip") == "Deep_Mod", "The folder holding the marker is the mod");
    check(!fs::exists(mods / L"Deep_Mod" / L"readme.txt"), "Files outside the mod are left behind");

    // Same name again: refused, then replaced on request.
    bool conflict = false;
    try { install(mods, sources / L"download (1).zip"); } catch (const launcher_mods::AlreadyInstalled& existing) {
        conflict = existing.name == "Cool Park";
    }
    check(conflict, "Installing over an installed mod asks first");
    make_zip(sources / L"v2.zip", {{"Cool Park/layout.toc", "toc v2"}});
    check(install(mods, sources / L"v2.zip", true) == "Cool Park" && read(mods / L"Cool Park" / L"layout.toc") == "toc v2",
          "Replacing installs the new copy");

    // Hostile or unrelated archives.
    make_zip(sources / L"evil.zip", {{"../evil.txt", "x"}, {"layout.toc", "toc"}});
    check(error_of([&] { install(mods, sources / L"evil.zip"); }).find("unsafe path") != std::string::npos &&
              !fs::exists(game / L"evil.txt") && !fs::exists(mods / L"evil"),
          "Paths leaving the mod folder are refused");
    make_zip(sources / L"photos.zip", {{"a.jpg", "x"}});
    check(error_of([&] { install(mods, sources / L"photos.zip"); }).find("does not contain a ReSkate mod") != std::string::npos,
          "Archives without a mod are refused");
    write(sources / L"notes.txt", "x");
    check(!error_of([&] { install(mods, sources / L"notes.txt"); }).empty(), "Only .zip files and folders install");

    // A folder.
    write(sources / L"Loose" / L"My Folder Mod" / L"layout.toc", "toc");
    check(install(mods, sources / L"Loose") == "My Folder Mod", "Folders install like zips");
    check(fs::exists(sources / L"Loose" / L"My Folder Mod" / L"layout.toc"), "Installing a folder copies it");

    // The list: unlisted folders load after listed ones, by name.
    auto list = mods::scan_mods(game);
    check(list.present && list.issue.empty() && list.entries.size() == 4, "Every installed mod is listed");
    check(list.entries[0].mod.name == "Cool Park" && list.entries[0].enabled, "Unlisted mods are enabled");
    check(list.entries[0].mod.title == "Cool Park" && list.entries[0].mod.provides_layout,
          "The replaced mod shows its new contents");
    const auto deep = std::find_if(list.entries.begin(), list.entries.end(), [](const auto& e) { return e.mod.name == "Deep_Mod"; });
    check(deep != list.entries.end() && deep->mod.title == "Deep_Mod", "Titles fall back to the folder name");

    // Save an order with one mod disabled, then read it back.
    std::reverse(list.entries.begin(), list.entries.end());
    list.entries[1].enabled = false;
    mods::save_mod_order(mods, list.entries);
    const auto saved = mods::scan_mods(game);
    check(saved.issue.empty() && saved.entries.size() == 4, "The saved mods.json reads back");
    for (std::size_t i = 0; i < 4; ++i)
        check(saved.entries[i].mod.name == list.entries[i].mod.name && saved.entries[i].enabled == list.entries[i].enabled,
              "Order and enabled state round-trip");
    check(read(mods / L"mods.json").find(R"("enabled": false)") != std::string::npos, "mods.json is plain readable JSON");

    // Metadata from reskate-mod.json.
    write(mods / L"Deep_Mod" / L"reskate-mod.json",
          "\xef\xbb\xbf{\"name\":\"Deep\",\"author\":\"Someone\",\"version\":\"3\",\"description\":\"Line\\u0001\"}");
    for (const auto& entry : mods::scan_mods(game).entries)
        if (entry.mod.name == "Deep_Mod")
            check(entry.mod.title == "Deep" && entry.mod.author == "Someone" && entry.mod.version == "3" &&
                  entry.mod.description == "Line?", "reskate-mod.json fills the details, control characters removed");

    // manifest.json is preferred, with version_number.
    write(mods / L"Deep_Mod" / L"manifest.json",
          R"({"name":"Deep Manifest","author":"Park Maker","version_number":"2.1.0","description":"From the manifest"})");
    for (const auto& entry : mods::scan_mods(game).entries)
        if (entry.mod.name == "Deep_Mod")
            check(entry.mod.title == "Deep Manifest" && entry.mod.author == "Park Maker" && entry.mod.version == "2.1.0" &&
                  entry.mod.description == "From the manifest", "manifest.json fills the details ahead of reskate-mod.json");

    // Park mods: parks/<map>.park.json is listed per map.
    write(mods / L"Deep_Mod" / L"parks" / L"bam.park.json", "{}");
    write(mods / L"Deep_Mod" / L"parks" / L"grom.park.json", "{}");
    for (const auto& entry : mods::scan_mods(game).entries)
        if (entry.mod.name == "Deep_Mod")
            check(entry.mod.park_maps == std::vector<std::string>{"bam", "grom"}, "Park maps are listed");
    make_zip(sources / L"parkmod.zip", {{"My Park/manifest.json", R"({"name":"My Park"})"},
                                        {"My Park/parks/bam.park.json", "{}"}});
    check(install(mods, sources / L"parkmod.zip") == "My Park", "A park mod (manifest.json only) installs");

    // Left-out mods: reported while their files are unchanged, forgotten once they change.
    {
        std::map<std::string, mods::Exclusion, std::less<>> exclusions;
        exclusions["Deep_Mod"] = {mods::mod_fingerprint(mods / L"Deep_Mod"), "sdk", {"levels/game/x: damaged"}};
        mods::write_exclusions(mods, exclusions);
        const auto read_back = mods::read_exclusions(mods);
        check(read_back.size() == 1 && read_back.at("Deep_Mod").sdk == "sdk" &&
                  read_back.at("Deep_Mod").problems == std::vector<std::string>{"levels/game/x: damaged"},
              "Exclusions round-trip");
        const auto listed = mods::scan_mods(game);
        check(listed.excluded.contains("Deep_Mod") && listed.excluded.size() == 1, "An unchanged left-out mod is reported");
        write(mods / L"Deep_Mod" / L"new file.txt", "reinstalled");
        check(mods::scan_mods(game).excluded.empty(), "A changed mod is no longer reported as left out");
        mods::write_exclusions(mods, {});
        check(!fs::exists(mods / mods::exclusions_file), "An empty exclusion list removes the file");
    }

    // A broken mods.json still lists the folders, so the manager can repair it.
    write(mods / L"mods.json", R"({"schema":1,"mods":[{"name":"Cool Park"}]})");
    const auto broken = mods::scan_mods(game);
    check(!broken.issue.empty() && broken.entries.size() == 5, "A malformed mods.json is reported, folders still listed");
    mods::save_mod_order(mods, broken.entries);
    check(mods::scan_mods(game).issue.empty(), "Saving repairs a malformed mods.json");

    // A big collection, with the longest names, can be switched off and reordered, and the
    // list reads back. (There was a limit of 64 mods once: past it nothing could be saved, so
    // no mod could be disabled.)
    {
        constexpr std::size_t collection = 300;
        const auto big_game = root / L"big";
        const auto big = launcher_mods::mods_root(big_game);
        for (std::size_t i = 0; i < collection; ++i) {
            auto name = std::to_string(i);
            name = std::string(4 - name.size(), '0') + name + "-";
            name.resize(mods::maximum_mod_name, 'x');
            write(big / name / L"layout.toc", "toc");
        }
        auto many = mods::scan_mods(big_game);
        check(many.issue.empty() && many.entries.size() == collection, "A big Mods folder is listed");
        for (std::size_t i = 0; i < many.entries.size(); i += 3) many.entries[i].enabled = false;
        std::swap(many.entries.front(), many.entries.back());
        std::string refusal;
        try { mods::save_mod_order(big, many.entries); } catch (const std::exception& failure) { refusal = failure.what(); }
        if (!refusal.empty()) std::cerr << "save refused: " << refusal << '\n';
        check(refusal.empty(), "A big mod list can be saved");
        const auto again = mods::scan_mods(big_game);
        check(again.issue.empty() && again.entries.size() == many.entries.size(), "A big mods.json reads back");
        bool same = again.entries.size() == many.entries.size();
        for (std::size_t i = 0; same && i < again.entries.size(); ++i)
            same = again.entries[i].mod.name == many.entries[i].mod.name && again.entries[i].enabled == many.entries[i].enabled;
        check(same, "Every mod keeps its place and whether it loads");
    }

    // Thunderstore packages: manifest.json, icon.png and README.md at the top of the zip.
    {
        const auto store_game = root / L"store";
        fs::create_directories(store_game);
        const auto store = launcher_mods::mods_root(store_game);
        const std::string manifest =
            R"({"name":"Desert_Springs","version_number":"1.2.3","website_url":"","description":"Sand.","dependencies":[]})";
        const auto installed = [&](std::string_view name) -> std::optional<mods::Mod> {
            for (const auto& entry : mods::scan_mods(store_game).entries)
                if (entry.mod.name == name) return entry.mod;
            return std::nullopt;
        };

        // Downloaded from the site: files at the top, the archive named Namespace-Name-Version.
        make_zip(sources / L"Zee-Desert_Springs-1.2.3.zip", {{"manifest.json", manifest}, {"icon.png", "png"},
            {"README.md", "# Desert"}, {"layout.toc", "toc"}, {"Win32/levels/desert.sb", "bundle"}});
        check(install(store, sources / L"Zee-Desert_Springs-1.2.3.zip") == "Zee-Desert_Springs",
              "A downloaded package installs without the version in its folder name");
        check(read(store / L"Zee-Desert_Springs" / L"icon.png") == "png" &&
                  read(store / L"Zee-Desert_Springs" / L"Win32" / L"levels" / L"desert.sb") == "bundle",
              "Package files install with the mod");
        make_zip(sources / L"Map-2.zip", {{"layout.toc", "toc"}});
        check(install(store, sources / L"Map-2.zip") == "Map-2", "Names that merely end in a number keep it");

        // The usual packaging mistake: the mod folder zipped inside the package.
        make_zip(sources / L"nested.zip", {{"manifest.json", manifest}, {"icon.png", "png"}, {"README.md", "readme"},
            {"CHANGELOG.md", "log"}, {"DesertSprings/layout.toc", "toc"},
            {"DesertSprings/manifest.json", R"({"name":"Old"})"}, {"DesertSprings/Win32/levels/desert.sb", "bundle"}});
        check(install(store, sources / L"nested.zip") == "DesertSprings",
              "Content in a folder is found past the package's own manifest.json");
        check(read(store / L"DesertSprings" / L"manifest.json") == manifest &&
                  read(store / L"DesertSprings" / L"icon.png") == "png" &&
                  read(store / L"DesertSprings" / L"CHANGELOG.md") == "log" &&
                  read(store / L"DesertSprings" / L"Win32" / L"levels" / L"desert.sb") == "bundle",
              "Package files at the top follow the mod in and replace its own copies");
        check(!fs::exists(store / L"DesertSprings" / L"DesertSprings"), "The wrapper folder is not kept");

        // A package only replaces the nested metadata files it actually supplies.
        const std::vector<std::pair<std::string, std::string>> partial_package{
            {"manifest.json", manifest}, {"rEaDmE.md", "package readme"},
            {"Partial/layout.toc", "toc"}, {"Partial/manifest.json", R"({"name":"Old"})"},
            {"Partial/README.md", "old readme"}, {"Partial/icon.png", "nested icon"},
            {"Partial/CHANGELOG.md", "nested changelog"}};
        make_zip(sources / L"partial.zip", partial_package);
        const auto folder_package = sources / L"partial-folder";
        for (const auto& [name, data] : partial_package) write(folder_package / name, data);
        for (const auto& source : {sources / L"partial.zip", folder_package}) {
            check(install(store, source, true) == "Partial", "Partial packages install from zips and folders");
            check(read(store / L"Partial" / L"manifest.json") == manifest &&
                      read(store / L"Partial" / L"README.md") == "package readme",
                  "Present package files replace nested copies regardless of filename case");
            check(read(store / L"Partial" / L"icon.png") == "nested icon" &&
                      read(store / L"Partial" / L"CHANGELOG.md") == "nested changelog",
                  "Nested metadata survives when the package has no replacement");
        }

        // What the launcher's Thunderstore installs ask for.
        launcher_mods::InstallOptions options;
        options.folder = "Zee-Desert_Springs";
        options.author = "Zee";
        options.require_content = true;
        std::atomic<bool> cancel{};
        check(launcher_mods::install(store, sources / L"nested.zip", true, {}, cancel, options) == "Zee-Desert_Springs",
              "The package's own folder name wins");
        const auto desert = installed("Zee-Desert_Springs");
        check(desert && desert->author == "Zee" && desert->version == "1.2.3" && desert->title == "Desert_Springs" &&
                  desert->provides_layout,
              "The team becomes the author and the manifest gives the version");

        make_zip(sources / L"authored.zip", {{"manifest.json", R"({"name":"Park","author":"Someone","version_number":"1.0.0"})"},
            {"icon.png", "png"}, {"README.md", "readme"}, {"parks/bam.park.json", "{}"}});
        options.folder = "Team-Park";
        check(launcher_mods::install(store, sources / L"authored.zip", true, {}, cancel, options) == "Team-Park",
              "A park package installs");
        const auto park = installed("Team-Park");
        check(park && park->author == "Someone" && park->park_maps == std::vector<std::string>{"bam"},
              "An author the manifest already names is kept");

        make_zip(sources / L"empty.zip", {{"manifest.json", manifest}, {"icon.png", "png"}, {"README.md", "readme"}});
        options.folder = "Team-Empty";
        check(error_of([&] { launcher_mods::install(store, sources / L"empty.zip", true, {}, cancel, options); })
                      .find("nothing for ReSkate to load") != std::string::npos &&
                  !fs::exists(store / L"Team-Empty"),
              "Thunderstore installs refuse a package with nothing to load");
        check(install(store, sources / L"empty.zip") == "empty", "A plain install still takes a description-only mod");
    }

    reset_tests(root / L"reset", sources);

    std::error_code ignored;
    fs::remove_all(root, ignored);
    if (failures) {
        std::cerr << failures << " mod manager check(s) failed\n";
        return 1;
    }
    std::cout << "Mod manager checks passed.\n";
    return 0;
}
