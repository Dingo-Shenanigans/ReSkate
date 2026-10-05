#include "Launcher/updater.h"

#include <iostream>
#include <stdexcept>

using namespace dingosdk;

int main() {
    static_assert(RESKATE_STEAM_DOWNLOADS_DISABLED == 1);
    int callbacks = 0;
    int rejected = 0;
    launcher_update::RemoteFile tool;
    tool.url = L"https://invalid.example/DepotDownloader.zip";
    try {
        launcher_update::ensure_depot_downloader(tool, [&](auto, auto) { ++callbacks; });
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) == steam_privacy::unsupported_detail) ++rejected;
    }

    launcher_update::GameBuild game;
    launcher_update::SteamLogin login;
    login.username = "must-not-be-used";
    std::atomic<bool> cancel{false};
    try {
        launcher_update::run_depot_downloader(L"must-not-start.exe", game, L"must-not-create", true, login,
            [&](std::string_view) { ++callbacks; },
            [&](const launcher_update::Prompt&) -> std::optional<std::string> {
                ++callbacks;
                return "must-not-be-sent";
            }, cancel);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) == steam_privacy::unsupported_detail) ++rejected;
    }
    if (rejected != 2 || callbacks != 0) {
        std::cerr << "Steam downloader entry points must reject calls without callbacks.\n";
        return 1;
    }
    std::cout << "Steam downloader entry points rejected; no callbacks invoked.\n";
    return 0;
}
