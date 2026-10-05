#include "Launcher/updater.h"

#include <stdexcept>

// Keep the upstream entry points as rejecting replacements. Even a new caller
// cannot install or start DepotDownloader through these functions.
namespace dingosdk::launcher_update {

std::filesystem::path ensure_depot_downloader(const RemoteFile&, const Progress&) {
    throw std::runtime_error(steam_privacy::unsupported_detail);
}

DWORD run_depot_downloader(const std::filesystem::path&, const GameBuild&,
                          const std::filesystem::path&, bool, const SteamLogin&,
                          const std::function<void(std::string_view)>&,
                          const PromptHandler&, const std::atomic<bool>&) {
    throw std::runtime_error(steam_privacy::unsupported_detail);
}

} // namespace dingosdk::launcher_update
