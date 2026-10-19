#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Debug/trace.h"
#include "crash_reporter.h"
#include "gui.h"
#include "launch.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

namespace app = dingosdk::launcher_app;
namespace update = dingosdk::launcher_update;
using dingosdk::logging::Channel;
using dingosdk::logging::Level;

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    auto length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return L"Unknown error";
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length);
    return output;
}

// Removes a launcher-only flag so it never reaches Skate.exe.
bool take_flag(std::vector<std::wstring>& arguments, std::wstring_view flag) {
    const auto found = std::find(arguments.begin(), arguments.end(), flag);
    if (found == arguments.end()) return false;
    arguments.erase(found);
    return true;
}

// Scripted launches (`--no-gui`): update, validate and start Skate without a window.
// Returns true when the launcher replaced itself and restarted.
bool apply_headless_updates(const app::Session& session, const update::Config& config,
                            const std::vector<std::wstring>& original, bool relaunched) {
    if (!update::binary_updates_enabled()) return false;
    if (!config.runtime.url.empty() && !update::file_matches(session.paths.dll, config.runtime)) {
        try {
            update::replace_file(session.paths.dll, config.runtime);
            dingosdk::logging::log(Level::info, Channel::launcher, "ReSkate.dll updated to {}.", config.runtime.version);
        } catch (const std::exception& exception) {
            dingosdk::logging::log(Level::warning, Channel::launcher, "ReSkate.dll update failed: {}", exception.what());
        }
    }
    if (relaunched || config.launcher.url.empty() || update::file_matches(session.self, config.launcher)) return false;
    try {
        update::replace_running_launcher(session.self, config.launcher);
        dingosdk::logging::log(Level::info, Channel::launcher, "Launcher updated to {}; restarting.", config.launcher.version);
        auto arguments = original;
        arguments.emplace_back(L"--reskate-updated");
        app::relaunch(session.self, arguments);
        return true;
    } catch (const std::exception& exception) {
        dingosdk::logging::log(Level::warning, Channel::launcher, "Launcher update failed: {}", exception.what());
        return false;
    }
}

void run_headless(const app::Session& session, std::vector<std::wstring> arguments,
                  const std::vector<std::wstring>& original) {
    const bool relaunched = take_flag(arguments, L"--reskate-updated");
    const bool updates = !take_flag(arguments, L"--no-update") && dingosdk::launcher_gui::updates_enabled(session);
    // With no options of its own, the launch is the one the launcher window's Play would make.
    // A map to start on is not one of those: "--no-gui -map <name>" is that launch, on that map.
    auto map = dingosdk::launcher::take_map_option(arguments);
    auto options = arguments.empty() ? dingosdk::launcher_gui::saved_launch_options(session)
                                     : dingosdk::launcher::parse_launch_options(arguments);
    if (!map.empty()) options.map = std::move(map);
    update::remove_previous_launcher(session.self);
    if (updates) {
        const auto config = update::fetch_config(dingosdk::launcher_gui::prereleases_enabled(session));
        if (config && apply_headless_updates(session, *config, original, relaunched)) return;
    }
    if (!app::game_files_supported(session.paths))
        throw std::runtime_error("Skate.exe is missing or is not the supported build. "
            "Open ReSkateLauncher.exe without --no-gui to download it from Steam.");
    app::start_game(session, options);
}

// Whether the launcher was put in a game's folder of a Steam library (...\steamapps\common\<game>),
// which is where Steam keeps skate. and updates it: ReSkate has a folder of its own, with its
// own copy of the game. The same under Wine: on Proton, Steam is given the launcher's place in
// that folder of its own (README, "Linux (Proton)").
bool in_steam_library() {
    std::wstring self(32768, L' ');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    if (self.empty()) return false;
    const auto same = [](const fs::path& part, const wchar_t* name) { return _wcsicmp(part.c_str(), name) == 0; };
    for (auto folder = fs::path(self).parent_path(); folder.has_parent_path() && folder != folder.parent_path(); folder = folder.parent_path())
        if (same(folder.filename(), L"common") && same(folder.parent_path().filename(), L"steamapps")) return true;
    return false;
}

// The launcher's folder (where the game is installed and run from), when its path has a
// character outside plain ASCII; empty otherwise. skate. reads its own folder in the Windows
// code page and then decodes it as UTF-8, so a folder with a Cyrillic user name, or a Portuguese
// or Japanese OneDrive Desktop, comes out wrong: it cannot read its data and crashes in its first
// second (Skate.exe+0x18df4cf, four players on 2026-10-17, every one in such a folder).
std::wstring non_ascii_folder() {
    std::wstring self(32768, L' ');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    const auto folder = fs::path(self).parent_path().wstring();
    return std::any_of(folder.begin(), folder.end(), [](wchar_t c) { return c > 0x7e || c < 0x20; }) ? folder : std::wstring{};
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        MessageBoxW(nullptr, L"Could not read the launcher command line.",
            L"ReSkate Launcher", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--reskate-crash-helper") {
        const auto result = dingosdk::crash_report::run_reporter(argc, argv);
        LocalFree(argv);
        return result;
    }
    const std::vector<std::wstring> original(argv + 1, argv + argc);
    LocalFree(argv);
    // (Before anything is read or written there: the only thing to do is to close.)
    if (in_steam_library()) {
        MessageBoxW(nullptr,
            L"ReSkate cannot run from Steam's skate. folder.\n\n"
            L"Move ReSkateLauncher.exe and ReSkate.dll to a new, empty folder of their own (for example C:\\Games\\ReSkate) "
            L"and run the launcher from there. It installs its own copy of the game into that folder, so a Steam update "
            L"cannot break it.\n\nThe launcher will now close.",
            L"ReSkate Launcher", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }
    if (const auto folder = non_ascii_folder(); !folder.empty() &&
        MessageBoxW(nullptr,
            (L"ReSkate is in a folder whose path has letters skate. cannot read:\n\n" + folder +
             L"\n\nThe game cannot find its own files there (accented, Cyrillic, Japanese and other letters outside "
             L"A to Z; OneDrive and Desktop folders often have them) and crashes as it starts.\n\n"
             L"Move ReSkateLauncher.exe and ReSkate.dll, with the game the launcher installed beside them, to a folder "
             L"with only plain letters, for example C:\\Games\\ReSkate, and run the launcher from there.\n\n"
             L"Start anyway?").c_str(),
            L"ReSkate Launcher", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES)
        return 1;
    auto arguments = original;
    const bool headless = take_flag(arguments, L"--no-gui");

    int result = 0;
    fs::path logs;
    try {
        std::string log_level = "info";
        if (headless) log_level = dingosdk::launcher::parse_launch_options(
            [&] { auto copy = arguments; take_flag(copy, L"--no-update"); take_flag(copy, L"--reskate-updated"); return copy; }()).log_level;
        dingosdk::launcher_gui::apply_crash_report_setting();
        const auto session = app::open_session(log_level);
        logs = session.paths.logs;
        if (headless) run_headless(session, arguments, original);
        else if (app::relaunch_without_mangohud(session.self, original)) dingosdk::logging::flush();
        else result = dingosdk::launcher_gui::run(session, arguments);
    } catch (const std::exception& exception) {
        const auto message = widen(exception.what());
        dingosdk::logging::write(Level::critical, Channel::launcher, message);
        dingosdk::logging::flush();
        std::wstring dialog = L"Could not start Skate.\n\n" + message;
        if (!logs.empty()) dialog += L"\n\nDetails: " + (logs / L"ReSkate.log").wstring();
        MessageBoxW(nullptr, dialog.c_str(), L"ReSkate Launcher", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        result = 1;
    }
    dingosdk::trace::flush(std::chrono::seconds(3));
    dingosdk::logging::shutdown();
    return result;
}
