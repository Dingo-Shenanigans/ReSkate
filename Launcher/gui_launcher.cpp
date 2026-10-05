#include "gui_internal.h"

#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/mod_catalog.h"

#include <shellapi.h>

#include <format>
#include <fstream>
#include <stdexcept>

namespace dingosdk::launcher_gui::detail {
namespace {

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length);
    return output;
}

std::string format_bytes(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024) return std::format("{:.1f} GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024));
}

fs::path settings_path(const launcher_app::Session& session) {
    return session.self.parent_path() / L"ReSkateLauncher.settings.json";
}

Settings load_settings(const fs::path& path) {
    Settings settings;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return settings;
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto json = Json::parse(text);
        settings.windowed = json.value("windowed", settings.windowed);
        settings.width = std::clamp(json.value("width", settings.width), 320, 16384);
        settings.height = std::clamp(json.value("height", settings.height), 200, 16384);
        settings.loose_files = json.value("loose_files", settings.loose_files);
        settings.gpu_diagnostics = json.value("gpu_diagnostics", settings.gpu_diagnostics);
        settings.offline = json.value("offline", settings.offline);
        settings.menu_key = json.value("menu_key", settings.menu_key);
        settings.console_key = json.value("console_key", settings.console_key);
        if (!launcher::bindable_key(static_cast<unsigned>(settings.menu_key)) ||
            !launcher::bindable_key(static_cast<unsigned>(settings.console_key)) ||
            settings.menu_key == settings.console_key) {
            settings.menu_key = static_cast<int>(launcher::default_menu_key);
            settings.console_key = static_cast<int>(launcher::default_console_key);
        }
        const auto level = json.value("log_level", std::string("info"));
        for (std::size_t index = 0; index < log_levels.size(); ++index)
            if (level == log_levels[index]) settings.log_level = static_cast<int>(index);
        settings.arguments = json.value("arguments", std::string());
        // Keep the original settings key so existing launcher preferences survive.
        settings.keep_open_after_launch = !json.value("close_on_launch", !settings.keep_open_after_launch);
        settings.updates = json.value("updates", settings.updates);
        settings.crash_reports = json.value("crash_reports", settings.crash_reports);
    } catch (const std::exception& exception) {
        logging::log(logging::Level::warning, logging::Channel::launcher, "Ignoring launcher settings: {}", exception.what());
    }
    return settings;
}

void save_settings(const fs::path& path, const Settings& settings) {
    auto json = Json::object();
    json["windowed"] = settings.windowed;
    json["width"] = settings.width;
    json["height"] = settings.height;
    json["loose_files"] = settings.loose_files;
    json["gpu_diagnostics"] = settings.gpu_diagnostics;
    json["offline"] = settings.offline;
    json["menu_key"] = settings.menu_key;
    json["console_key"] = settings.console_key;
    json["log_level"] = log_levels[static_cast<std::size_t>(settings.log_level)];
    json["arguments"] = settings.arguments;
    json["close_on_launch"] = !settings.keep_open_after_launch;
    json["updates"] = settings.updates;
    json["crash_reports"] = settings.crash_reports;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json.dump(2) << '\n';
}

// Turns saved settings into the same flags the command line accepts, so both
// paths share parse_launch_options' validation.
launcher::LaunchOptions launch_options(const Settings& settings) {
    std::vector<std::wstring> arguments;
    if (settings.windowed) {
        arguments.push_back(std::format(L"--width={}", settings.width));
        arguments.push_back(std::format(L"--height={}", settings.height));
    }
    if (!settings.loose_files) arguments.emplace_back(L"--no-loose-files");
    if (settings.gpu_diagnostics) arguments.emplace_back(L"--gpu-diagnostics");
    if (settings.offline) arguments.emplace_back(L"-offline");
    arguments.push_back(std::format(L"--menu-key={}", settings.menu_key));
    arguments.push_back(std::format(L"--console-key={}", settings.console_key));
    arguments.push_back(L"--log-level=" + widen(log_levels[static_cast<std::size_t>(settings.log_level)]));
    if (settings.arguments.find_first_not_of(" \t") != std::string::npos) {
        int count{};
        const auto line = L"skate " + widen(settings.arguments);
        if (auto** split = CommandLineToArgvW(line.c_str(), &count)) {
            for (int index = 1; index < count; ++index) arguments.emplace_back(split[index]);
            LocalFree(split);
        }
    }
    return launcher::parse_launch_options(arguments);
}

} // namespace

Launcher::Launcher(const launcher_app::Session& session, std::vector<std::wstring> arguments)
    : session_(session), arguments_(std::move(arguments)), settings_(load_settings(settings_path(session))) {
    relaunched_ = std::find(arguments_.begin(), arguments_.end(), L"--reskate-updated") != arguments_.end();
    binaries_ = std::find(arguments_.begin(), arguments_.end(), L"--no-update") == arguments_.end();
}

Launcher::~Launcher() {
    if (worker_.joinable()) worker_.join();
}

State Launcher::snapshot() {
    std::lock_guard lock(mutex_);
    return state_;
}

void Launcher::game_exited(bool seen) {
    game_ = 0;
    launched_ = false;
    if (busy_ || snapshot().phase == Phase::failed) return;
    if (seen) check();
    else fail("Skate closed while it was starting.");
}

void Launcher::save() {
    save_settings(settings_path(session_), settings_);
    // The game inherits the launcher's environment, so this reaches the next launch; the
    // launcher's own crash reporting follows the setting from its next start.
    SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", settings_.crash_reports ? nullptr : L"0");
}

template<class Task> void Launcher::start(Task task) {
    if (busy_) return;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    worker_ = std::thread([this, task] {
        try { task(); }
        catch (const std::exception& exception) { fail(exception.what()); }
        busy_ = false;
    });
}

void Launcher::check() { start([this] { run_check(); }); }
void Launcher::apply_updates() { start([this] { run_updates(); }); }

void Launcher::play() { start([this] { run_play(); }); }

void Launcher::restart() {
    auto arguments = arguments_;
    if (std::find(arguments.begin(), arguments.end(), L"--reskate-updated") == arguments.end())
        arguments.emplace_back(L"--reskate-updated");
    launcher_app::relaunch(session_.self, arguments);
}

fs::path Launcher::mods_data_root() const {
    // The game resolves -dataPath against its own folder (engine_data_root);
    // the same argument can be typed into Settings, so follow it here too.
    const auto& extra = settings_.arguments;
    const auto found = extra.find("-dataPath");
    if (found == std::string::npos) return session_.paths.directory;
    auto rest = std::string_view(extra).substr(found + 9);
    if (!rest.empty() && (rest.front() == '=' || rest.front() == ' ')) rest.remove_prefix(1);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    const auto end = rest.find_first_of(" 	");
    const auto value = rest.substr(0, end);
    if (value.empty()) return session_.paths.directory;
    std::error_code error;
    const auto root = fs::weakly_canonical(session_.paths.directory / wide(value), error);
    return error ? session_.paths.directory / wide(value) : root;
}

void Launcher::play_anyway() {
    ignore_mod_problems_ = true;
    play();
}

void Launcher::dismiss_mod_problems() {
    std::lock_guard lock(mutex_);
    if (state_.phase != Phase::mods_broken) return;
    state_.phase = Phase::ready;
    state_.status = "Ready to skate";
    state_.detail = "Some mods are switched off or will not load.";
    state_.mod_problems.clear();
}

void Launcher::set(Phase phase, std::string status, std::string detail, float progress) {
    std::lock_guard lock(mutex_);
    state_.phase = phase;
    state_.status = std::move(status);
    state_.detail = std::move(detail);
    state_.progress = progress;
}

void Launcher::set_progress(std::string detail, float progress) {
    std::lock_guard lock(mutex_);
    state_.detail = std::move(detail);
    state_.progress = progress;
}

void Launcher::fail(const std::string& message) {
    logging::write(logging::Level::error, logging::Channel::launcher, message);
    std::lock_guard lock(mutex_);
    state_.phase = Phase::failed;
    state_.status = message;   // raw: the STATUS tile explains it and can copy it
    state_.detail.clear();
    state_.progress = -1;
}

std::optional<update::Config> Launcher::config() {
    std::lock_guard lock(mutex_);
    return state_.config;
}

update::Progress Launcher::progress_for(std::string label) {
    return [this, label](std::uint64_t received, std::uint64_t total) {
        set_progress(std::format("{}  {} of {}", label, format_bytes(received), format_bytes(total)),
            total ? static_cast<float>(static_cast<double>(received) / static_cast<double>(total)) : -1.0f);
    };
}

bool Launcher::launcher_outdated(const update::Config& config) const {
    return !relaunched_ && !config.launcher.url.empty() && !update::file_matches(session_.self, config.launcher);
}

bool Launcher::runtime_outdated(const update::Config& config) const {
    return !config.runtime.url.empty() && !update::file_matches(session_.paths.dll, config.runtime);
}

void Launcher::run_check() {
    set(Phase::checking, "Checking for updates");
    update::remove_previous_launcher(session_.self);
    auto config = update::fetch_config();
    {
        std::lock_guard lock(mutex_);
        state_.config = config;
    }
    const bool newer = config && (launcher_outdated(*config) || runtime_outdated(*config));
    if (newer && update::binary_updates_enabled() && binaries_ && settings_.updates) {
        set(Phase::update_available, "A ReSkate update is ready",
            std::format("Version {} will be downloaded and installed.",
                config->runtime.version.empty() ? config->launcher.version : config->runtime.version));
        return;
    }
    set(Phase::checking, "Checking game files");
    if (!launcher_app::game_files_supported(session_.paths)) {
        std::error_code error;
        const bool installed = fs::is_regular_file(session_.paths.game, error);
        // Installation and build checks stay local; this launcher does not download Steam depots.
        if (installed) set(Phase::game_outdated, "Skate files are not the supported build",
            "Use an installed copy of the supported Skate build. Steam downloads are disabled in this launcher.");
        else set(Phase::game_missing, "Skate is not installed here",
            "Place ReSkateLauncher.exe and ReSkate.dll beside an existing Skate.exe, then check again.");
        return;
    }
    std::error_code error;
    if (!fs::is_regular_file(session_.paths.dll, error)) {
        fail("ReSkate.dll is missing from the game folder.");
        return;
    }
    set(Phase::ready, "Ready to skate", !config ? "Offline: update check skipped."
        : !settings_.updates ? (newer ? "ReSkate updates are off; keeping your ReSkate files." : "ReSkate updates are off.")
        : "Up to date.");
}

void Launcher::run_updates() {
    const auto config = this->config();
    if (!config) return run_check();
    set(Phase::updating, "Updating ReSkate");
    if (runtime_outdated(*config))
        update::replace_file(session_.paths.dll, config->runtime, progress_for("ReSkate.dll"));
    if (launcher_outdated(*config)) {
        update::replace_running_launcher(session_.self, config->launcher, progress_for("Launcher"));
        logging::flush();
        restart_ = true;
        return;
    }
    run_check();
}

// The merge the game does behind its splash, done here instead, so a mod that
// cannot be merged is a question the launcher can ask rather than a surprise
// three minutes into a loading screen. The game reuses what this builds.
bool Launcher::run_mod_merge() {
    const auto root = mods_data_root();
    std::error_code error;
    if (!fs::is_directory(root / mods::mods_folder, error)) return true;
    set(Phase::merging, "Preparing mods", "Merging what is installed", 0);
    auto catalog = mods::load_catalog(root, [this](const mods::MergeProgress& progress) {
        set_progress(progress.step, progress.total
            ? static_cast<float>(progress.done) / static_cast<float>(progress.total) : -1.0f);
    });
    for (const auto& note : catalog.notes)
        logging::write(logging::Level::info, logging::Channel::launcher, note);
    for (const auto& warning : catalog.warnings)
        logging::write(logging::Level::warning, logging::Channel::launcher, warning);

    std::vector<ModProblem> problems;
    if (!catalog.issue.empty())
        problems.push_back({{}, "The Mods folder", catalog.issue});
    for (const auto& mod : catalog.excluded) {
        // Mods built for another game version say so for themselves, in game
        // and in the mod manager; they do not hold up a launch.
        if (!mod.outdated.empty()) continue;
        problems.push_back({mod.name, mod.title.empty() ? mod.name : mod.title,
            mod.problems.empty() ? std::string("it could not be merged cleanly") : mod.problems.front()});
    }
    if (problems.empty()) return true;
    std::lock_guard lock(mutex_);
    state_.phase = Phase::mods_broken;
    state_.status = problems.size() == 1 ? "A mod could not be merged"
                                         : std::format("{} mods could not be merged", problems.size());
    state_.detail.clear();
    state_.progress = -1;
    state_.mod_problems = std::move(problems);
    return false;
}

void Launcher::run_play() {
    if (!ignore_mod_problems_ && !run_mod_merge()) return;
    ignore_mod_problems_ = false;
    set(Phase::launching, "Starting Skate");
    save();
    auto options = launch_options(settings_);
    // The launcher's own command line can request offline mode too.
    if (std::find(arguments_.begin(), arguments_.end(), L"-offline") != arguments_.end() ||
        std::find(arguments_.begin(), arguments_.end(), L"--offline") != arguments_.end())
        options.offline = true;
    launched_ = false;
    launcher_app::start_game(session_, options, [this](DWORD id) { game_ = id; });
    launched_ = true;
    set(Phase::launching, "Skate is starting", options.offline || !launcher_app::steam_signed_in() ?
        "Offline mode: playing without Steam as Unknown Player; multiplayer is hidden." :
        settings_.keep_open_after_launch ? "The launcher hides while you play and returns when Skate closes." :
        "The launcher closes now; Skate keeps starting on its own.");
}

} // namespace dingosdk::launcher_gui::detail

namespace dingosdk::launcher_gui {
bool updates_enabled(const launcher_app::Session& session) {
    return detail::load_settings(detail::settings_path(session)).updates;
}
void apply_crash_report_setting() noexcept {
    try {
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return;
        path.resize(length);
        if (!detail::load_settings(std::filesystem::path(path).parent_path() / L"ReSkateLauncher.settings.json").crash_reports)
            SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", L"0");
    } catch (...) {}
}
} // namespace dingosdk::launcher_gui
