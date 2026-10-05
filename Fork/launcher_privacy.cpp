#include "Launcher/gui_internal.h"

#include <stdexcept>

namespace dingosdk::launcher_gui::detail {

void Launcher::show_installed_game_problem() {
    std::error_code error;
    if (fs::is_regular_file(session_.paths.game, error))
        set(Phase::game_outdated, "Skate files are not the supported build", steam_privacy::unsupported_detail);
    else
        set(Phase::game_missing, "Skate is not installed here", steam_privacy::missing_detail);
}

// Upstream buttons can retain this entry point; it rechecks local files and
// never opens a password, Steam Guard or QR-code panel.
void open_sign_in(Launcher& launcher, Ui&, bool) {
    launcher.check();
}

void Launcher::download(bool, bool, std::string password) {
    SecureZeroMemory(password.data(), password.size());
    throw std::runtime_error(steam_privacy::unsupported_detail);
}

void Launcher::run_download(bool) {
    throw std::runtime_error(steam_privacy::unsupported_detail);
}

void Launcher::answer(std::optional<std::string> value) {
    if (value) SecureZeroMemory(value->data(), value->size());
    throw std::runtime_error(steam_privacy::unsupported_detail);
}

// No Steam downloader or sign-in prompt exists to cancel in this build.
void Launcher::cancel() {}

} // namespace dingosdk::launcher_gui::detail
