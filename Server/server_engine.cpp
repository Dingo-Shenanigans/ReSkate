#include "server_engine.h"
#include "global_bans.h"
#include "server_update.h"
#include "Extension/Multiplayer/Session/monotonic_clock.h"
#include "Engine/Core/Text/word_filter.h"
#include "Engine/Core/Platform/path_text.h"
#include "Extension/Multiplayer/developer_identity.h"
#include "Engine/Game/World/world_layer_catalog.h"
#include "Engine/Game/World/world_names.h"
#include "Engine/Vfs/world_layer_scan.h"

#ifdef _WIN32
#include <Windows.h>
#include <timeapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iostream>

namespace dingosdk::server {

ServerEngine& ServerEngine::instance() {
    static ServerEngine engine;
    return engine;
}

ServerEngine::ServerEngine() = default;

ServerEngine::~ServerEngine() {
    stop();
}

void ServerEngine::log(std::string text, LogLevel level) {
    char stamp[64]{};
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (GetTimeFormatA(LOCALE_USER_DEFAULT, 0, &st, nullptr, stamp, sizeof(stamp)) == 0) {
        const bool pm = st.wHour >= 12;
        const int hour12 = (st.wHour % 12 == 0) ? 12 : (st.wHour % 12);
        std::snprintf(stamp, sizeof(stamp), "%d:%02d:%02d %s", hour12, st.wMinute, st.wSecond, pm ? "PM" : "AM");
    }
#else
    const auto now_tp = std::chrono::system_clock::now();
    const auto now_t = std::chrono::system_clock::to_time_t(now_tp);
    std::tm local{};
    localtime_r(&now_t, &local);
    std::strftime(stamp, sizeof(stamp), "%X", &local);
#endif

    // Auto-detect log level if default info was provided
    if (level == LogLevel::info) {
        if (text.find("[chat]") != std::string::npos) {
            level = LogLevel::chat;
        } else if (text.find("[cmd]") != std::string::npos || text.find("[command]") != std::string::npos ||
                   text.find("[admin]") != std::string::npos) {
            level = LogLevel::command;
        } else if (text.find("[+]") != std::string::npos || text.find(" joined (") != std::string::npos ||
                   text.find(" is up on ") != std::string::npos || text.find("Join code:") != std::string::npos ||
                   text.find("Update applied successfully") != std::string::npos) {
            level = LogLevel::success;
        } else if (text.find("[-]") != std::string::npos || text.find(" left (") != std::string::npos ||
                   text.find(" was kicked") != std::string::npos || text.find(" was removed") != std::string::npos) {
            level = LogLevel::warning;
        } else if (text.starts_with("[error]") || text.starts_with("Error:") || text.starts_with("FATAL:") ||
                   text.starts_with("Server error:") || text.starts_with("Command error:") ||
                   text.find("Cannot read") != std::string::npos || text.find("error:") != std::string::npos) {
            level = LogLevel::error;
        } else if (text.starts_with("[warning]") || text.starts_with("Warning:") ||
                   text.find("Mods: skipped") != std::string::npos || text.find("[network] The server is behind") != std::string::npos) {
            level = LogLevel::warning;
        }
    }

    ServerLogEntry entry;
    entry.timestamp = stamp;
    entry.text = text;
    entry.level = level;

    std::vector<LogCallback> callbacks;
    {
        std::lock_guard lock(log_mutex_);
        entry.id = next_log_id_++;
        if (log_history_.size() >= 2048) {
            log_history_.pop_front();
        }
        log_history_.push_back(entry);
        if (log_file_.is_open()) {
            log_file_ << "[" << entry.timestamp << "] " << text << std::endl;
        }
        callbacks = log_callbacks_;
    }

    std::printf("[%s] %s\n", stamp, text.c_str());

    for (const auto& cb : callbacks) {
        if (cb) cb(entry);
    }
}

void ServerEngine::add_log_callback(LogCallback cb) {
    std::lock_guard lock(log_mutex_);
    log_callbacks_.push_back(std::move(cb));
}

std::vector<ServerLogEntry> ServerEngine::log_entries(std::size_t from_id) const {
    std::lock_guard lock(log_mutex_);
    std::vector<ServerLogEntry> result;
    result.reserve(log_history_.size());
    for (const auto& item : log_history_) {
        if (item.id >= from_id) {
            result.push_back(item);
        }
    }
    return result;
}

std::string ServerEngine::last_error() const {
    std::lock_guard lock(error_mutex_);
    return last_error_;
}

void ServerEngine::clear_error() {
    std::lock_guard lock(error_mutex_);
    last_error_.clear();
    if (state_.load() == ServerState::Error) {
        state_.store(ServerState::Stopped);
    }
}

bool ServerEngine::load_environment(const std::filesystem::path& folder, const std::filesystem::path& config_file, bool skip_update) {
    folder_ = folder;
    config_file_ = config_file;
    skip_update_ = skip_update;

    if (!log_file_.is_open()) {
        log_file_.open(folder_ / "ReSkateServer.log", std::ios::app);
    }

    try {
        const bool fresh = !std::filesystem::exists(config_file_);
        std::vector<std::string> added;
        config_ = load_config(config_file_, &added);
        if (fresh) log("Wrote default " + path_utf8(config_file_.filename()) + ". Edit it to name the server and add admins.");
        if (!added.empty()) {
            std::string names;
            for (const auto &name : added) names += (names.empty() ? "" : ", ") + name;
            log("Added new settings to " + path_utf8(config_file_.filename()) + ": " + names + ".");
        }
    } catch (const std::exception &e) {
        std::string err = "Cannot read " + path_utf8(config_file_) + ": " + e.what();
        log(err, LogLevel::error);
        std::lock_guard lock(error_mutex_);
        last_error_ = err;
        state_.store(ServerState::Error);
        return false;
    }

    for (const auto &problem : load_levels(folder_ / "Mods")) log("Mods: skipped " + problem, LogLevel::warning);
    if (levels().size() > 6) log("Mods: " + std::to_string(levels().size() - 6) + " custom map(s).", LogLevel::info);

    bool renamed{};
    if (const auto setting = map_setting(config_.map); setting != config_.map && !setting.empty()) {
        config_.map = setting;
        renamed = true;
    }
    for (auto &map : config_.map_pool) {
        if (const auto *level = find_level(map); level && level->name != map) {
            map = level->name;
            renamed = true;
        }
    }
    if (renamed) try { save_config(config_); } catch (...) {}
    if (const auto error = config_error(config_); !error.empty()) {
        std::string err = "Config problem: " + error;
        log(err, LogLevel::error);
        std::lock_guard lock(error_mutex_);
        last_error_ = err;
        state_.store(ServerState::Error);
        return false;
    }

    remove_previous_update(folder_);

    if (const auto catalog = folder_ / "world-layers.json"; std::filesystem::exists(catalog)) {
        try {
            install_world_layer_catalog(world_layer_scan::read(catalog));
            log("World layers: " + std::to_string(world_layers().size()) + " from world-layers.json.");
        } catch (const std::exception &e) {
            log(std::string("world-layers.json error: ") + e.what(), LogLevel::warning);
        }
    }

    // Fetch ReSkate identity lists, categories (Centrix, Dev, Staff, Homie, Creator) and global bans in background
    std::thread([]() {
        try {
            read_global_bans();
        } catch (...) {}
    }).detach();

    initialized_.store(true);
    state_.store(ServerState::Stopped);
    return true;
}

void ServerEngine::start_server_async() {
    auto current = state_.load();
    if (current == ServerState::Starting || current == ServerState::Running) return;

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    if (start_thread_.joinable()) {
        start_thread_.join();
    }

    clear_error();
    state_.store(ServerState::Starting);
    stopping_.store(false);

    start_thread_ = std::thread([this] {
        run_startup();
    });
}

void ServerEngine::run_startup() {
    try {
        log("Booting ReSkate dedicated server session...", LogLevel::info);

        // Reload latest config before starting
        try {
            config_ = load_config(config_file_);
        } catch (...) {}

        // If auto-update is configured and updates are enabled, check and apply update before opening ports
        if (config_.auto_update && updates_enabled()) {
            log("Checking for server updates...", LogLevel::info);
            const auto check = check_for_update();
            if (check.available) {
                log("New server release v" + check.version + " available. Installing update before startup...", LogLevel::info);
                try {
                    install_update(folder_);
                    log("Update applied successfully. Relaunching server...", LogLevel::success);
                    if (relaunch()) {
                        std::exit(0);
                    }
                } catch (const std::exception& e) {
                    log(std::string("Auto-update failed: ") + e.what() + ". Starting server with current version.", LogLevel::warning);
                }
            } else if (!check.problem.empty()) {
                log("Startup update check: " + check.problem, LogLevel::warning);
            } else {
                log("Server is up to date (v" + (check.version.empty() ? "2.0.3" : check.version) + ").", LogLevel::info);
            }
        }

        steam_ = std::make_unique<SteamServer>();
        std::string error;
        if (!steam_->start(folder_, config_.port, config_.query_port, config_.steam_token, error)) {
            std::string err_text = "Steam server failed to start: " + error;
            log(err_text, LogLevel::error);
            std::lock_guard lock(error_mutex_);
            last_error_ = err_text;
            state_.store(ServerState::Error);
            cleanup_server_instances();
            return;
        }

        log(config_.steam_token.empty() ? "Signing in to Steam..." : "Signing in to Steam with steam_token...");
        const auto login_started = std::chrono::steady_clock::now();
        while (!steam_->logged_on() && !stopping_) {
            steam_->run_callbacks();
            if (std::chrono::steady_clock::now() - login_started > std::chrono::seconds(45)) {
                std::string timeout_msg = config_.steam_token.empty()
                    ? "Steam sign-in timed out after 45 s. Check your internet connection."
                    : "Steam sign-in timed out. Check steam_token validity or internet connection.";
                log(timeout_msg, LogLevel::error);
                std::lock_guard lock(error_mutex_);
                last_error_ = timeout_msg;
                state_.store(ServerState::Error);
                cleanup_server_instances();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (stopping_) {
            cleanup_server_instances();
            state_.store(ServerState::Stopped);
            return;
        }

        transport_ = std::make_unique<multiplayer::SteamTransport>();
        if (!transport_->open_game_server(steam_->module())) {
            std::string trans_err = "Steam networking failed: " + transport_->status().detail;
            log(trans_err, LogLevel::error);
            std::lock_guard lock(error_mutex_);
            last_error_ = trans_err;
            state_.store(ServerState::Error);
            cleanup_server_instances();
            return;
        }

        host_ = std::make_unique<Host>(config_, *transport_, [this](const std::string& msg) {
            log(msg);
        });

        if (!host_->start(error)) {
            std::string host_err = "Could not open server listener: " + error;
            log(host_err, LogLevel::error);
            std::lock_guard lock(error_mutex_);
            last_error_ = host_err;
            state_.store(ServerState::Error);
            cleanup_server_instances();
            return;
        }

        log(config_.name + " is up on " + host_->map_name() + " for " + std::to_string(config_.max_players) + " players.", LogLevel::success);
        log("Steam ID " + std::to_string(steam_->steam_id()) +
            (config_.steam_token.empty() ? " (anonymous: new every start; set steam_token to keep one)" : " (from steam_token)") +
            ", public IP " + steam_->public_ip() + ".", LogLevel::info);
        if (config_.steam_token.empty() && config_.listed) {
            log("No steam_token: the server browser can be set to show only servers that have one, and then "
                "this server is not in it (players can still join with the code). It takes a minute to make "
                "one: see steam_token in README.", LogLevel::warning);
        }
        log("Join code: " + host_->invite() + (config_.password.empty() ? "" : " (password required)"), LogLevel::success);
        log(config_.admins.empty() ? "No admins yet. Type 'admin add <SteamID64>' to add one."
                                  : std::to_string(config_.admins.size()) + " admin(s) configured.", LogLevel::info);

        start_time_ = std::chrono::steady_clock::now();
        running_.store(true);
        state_.store(ServerState::Running);

        worker_loop();

    } catch (const std::exception& e) {
        std::string ex_msg = "Fatal Server Exception: " + std::string(e.what());
        log(ex_msg, LogLevel::error);
        std::lock_guard lock(error_mutex_);
        last_error_ = ex_msg;
        cleanup_server_instances();
        state_.store(ServerState::Error);
    } catch (...) {
        log("Unknown fatal server error during startup.", LogLevel::error);
        std::lock_guard lock(error_mutex_);
        last_error_ = "Unknown fatal exception in server startup.";
        cleanup_server_instances();
        state_.store(ServerState::Error);
    }
}

void ServerEngine::cleanup_server_instances() {
    if (host_) {
        try { host_->stop("Server stopped."); } catch (...) {}
        host_.reset();
    }
    if (transport_) {
        try { transport_->stop(); } catch (...) {}
        transport_.reset();
    }
    if (steam_) {
        try { steam_->stop(); } catch (...) {}
        steam_.reset();
    }
    running_.store(false);
}

void ServerEngine::worker_loop() {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    auto next_advertise = std::chrono::steady_clock::now();
    auto next_update_check = std::chrono::steady_clock::now() + std::chrono::minutes(15);
    auto next_ban_check = std::chrono::steady_clock::now();
    std::future<BanListCheck> ban_check;
    bool bans_unread = true;
    std::optional<bool> name_allowed;
    const auto target_tick_interval = std::chrono::microseconds(1000000 / std::max(10U, config_.tps));

    auto last_tick = std::chrono::steady_clock::now();
    auto fps_start = last_tick;
    size_t tick_counter = 0;
    bool tokens_required{};

    while (!stopping_.load()) {
        const auto tick_start = std::chrono::steady_clock::now();
        steam_->run_callbacks();

        try {
            host_->tick(multiplayer::now_us());
        } catch (const std::exception &e) {
            log(std::string("Server tick error: ") + e.what(), LogLevel::error);
        }

        // Live ReSkate Identity Sync (Centrix, Dev, Staff, Homie, Content Creator, Global Bans)
        const auto now_time = std::chrono::steady_clock::now();
        if (!ban_check.valid() && now_time >= next_ban_check) {
            ban_check = std::async(std::launch::async, read_global_bans);
        }
        if (ban_check.valid() && ban_check.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const auto check = ban_check.get();
            next_ban_check = now_time + (check.ok ? std::chrono::minutes(10) : std::chrono::minutes(1));
            if (config_.global_bans && check.ok && (check.changed || bans_unread)) {
                log("Global bans: " + std::to_string(check.banned) + " player(s) banned from ReSkate multiplayer cannot join.");
            }
            if (check.ok && check.words_changed) {
                log("Word lists: " + std::to_string(check.filtered_words) + " filtered and " + std::to_string(check.forbidden_words) +
                    " not allowed at all, from the ReSkate team's lists.");
            } else if (!check.ok && !bans_unread) {
                log("The ReSkate team's lists could not be read (" + check.problem + "). Trying again every minute.", LogLevel::warning);
            }
            bans_unread = false;
        }

        // The ReSkate team's rule, read with its ban list: say when it starts or stops hiding this server.
        if (const bool required = multiplayer::server_tokens_required(); required != tokens_required) {
            tokens_required = required;
            if (config_.steam_token.empty() && config_.listed) {
                log(required ? "The server browser now shows only servers with a steam_token, so this server is "
                               "hidden from it. Add a steam_token (see README) to be listed again."
                             : "The server browser shows servers without a steam_token again.", LogLevel::warning);
            }
        }

        // Non-disruptive periodic update check: notify only, never stop the server mid-session
        const auto now_check = std::chrono::steady_clock::now();
        if (config_.auto_update && updates_enabled() && now_check >= next_update_check) {
            next_update_check = now_check + std::chrono::minutes(15);
            std::thread([this]() {
                try {
                    const auto check = check_for_update();
                    std::lock_guard lock(update_mutex_);
                    if (check.available) {
                        update_info_.status = UpdateInfo::Status::Available;
                        update_info_.version = check.version;
                        update_info_.message = "New release available: v" + check.version;
                        update_info_.available = true;
                        log("[Updates] A new dedicated server release (v" + check.version + ") is available. It will be installed on next server boot.", LogLevel::info);
                    } else if (check.problem.empty()) {
                        update_info_.status = UpdateInfo::Status::UpToDate;
                        update_info_.version = check.version;
                        update_info_.message = "Server is up to date (v" + (check.version.empty() ? "2.0.3" : check.version) + ")";
                        update_info_.available = false;
                    }
                } catch (...) {}
            }).detach();
        }

        // Process pending commands
        std::deque<PendingCommand> commands;
        {
            std::lock_guard lock(cmd_mutex_);
            commands = std::move(pending_commands_);
            pending_commands_.clear();
        }

        for (auto& cmd : commands) {
            if (cmd.command == "quit" || cmd.command == "exit" || cmd.command == "stop") {
                stopping_.store(true);
                if (cmd.promise) cmd.promise->set_value("Server stopping.");
                break;
            }
            try {
                const auto result = host_->command(cmd.command);
                if (cmd.promise) {
                    cmd.promise->set_value(result);
                } else {
                    log(result, LogLevel::command);
                }
            } catch (const std::exception &e) {
                const std::string err = std::string("Command error: ") + e.what();
                if (cmd.promise) {
                    cmd.promise->set_value(err);
                } else {
                    log(err, LogLevel::error);
                }
            }
        }

        // Update Steam Advertise every 2 seconds
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_advertise) {
            next_advertise = now + std::chrono::seconds(2);
            const bool allowed = !text::contains_bad_words(config_.name);
            steam_->advertise({config_.name, host_->map_name(), host_->players(), config_.max_players, !config_.password.empty(),
                             config_.listed && allowed, host_->secret(), host_->direct_port()});
        }

        // Snapshot stats
        const auto tick_end = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(tick_end - tick_start);
        frame_time_ms_ = elapsed.count() / 1000.0;

        ++tick_counter;
        if (now - fps_start >= std::chrono::seconds(1)) {
            const auto sec = std::chrono::duration_cast<std::chrono::duration<double>>(now - fps_start).count();
            measured_tps_ = static_cast<double>(tick_counter) / sec;
            tick_counter = 0;
            fps_start = now;

            const double mem_mb = process_memory_mb();
            const auto uptime_s = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
            const unsigned p_count = host_->players();

            TelemetrySample sample;
            sample.uptime_seconds = uptime_s;
            sample.tps = static_cast<float>(measured_tps_);
            sample.frame_time_ms = static_cast<float>(frame_time_ms_);
            sample.memory_mb = static_cast<float>(mem_mb);
            sample.player_count = p_count;

            {
                std::lock_guard lock(telemetry_mutex_);
                telemetry_history_.push_back(sample);
                if (telemetry_history_.size() > 3600) {
                    telemetry_history_.pop_front();
                }
            }
        }

        {
            std::lock_guard lock(stats_mutex_);
            cached_players_ = host_->player_list();

            ServerStatusSnapshot s;
            s.server_name = config_.name;
            s.map_name = host_->map_name();
            s.player_count = host_->players();
            s.max_players = config_.max_players;
            s.target_tps = config_.tps;
            s.actual_tps = measured_tps_;
            s.frame_time_ms = frame_time_ms_;
            s.uptime_seconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
            s.invite_code = host_->invite();
            s.public_ip = steam_->public_ip();
            s.steam_id = steam_->steam_id();
            s.port = config_.port;
            s.query_port = config_.query_port;
            s.voice_allowed = host_->voice_allowed();
            s.voice_range = config_.voice_range;
            s.has_password = host_->has_password();
            s.is_listed = config_.listed;
            s.steam_token = config_.steam_token;
            s.has_steam_token = !config_.steam_token.empty();
            s.reserved_slots = static_cast<unsigned>(config_.reserved.size());
            s.crowd_budget = config_.crowd_budget;
            s.send_rate = config_.send_rate;
            s.object_limit = config_.object_limit;
            s.bone_scale_limit = config_.bone_scale_limit;
            s.running = true;
            cached_status_ = std::move(s);
        }

        // Precise sleep to hit target TPS
        const auto target_end = tick_start + target_tick_interval;
        const auto current = std::chrono::steady_clock::now();
        if (current < target_end) {
            const auto sleep_dur = std::chrono::duration_cast<std::chrono::milliseconds>(target_end - current);
            if (sleep_dur.count() > 1) {
                std::this_thread::sleep_for(sleep_dur - std::chrono::milliseconds(1));
            }
            while (std::chrono::steady_clock::now() < target_end) {
                // busy spin micro-wait
            }
        }
    }

#ifdef _WIN32
    timeEndPeriod(1);
#endif
    log("Server engine stopping...", LogLevel::info);
    cleanup_server_instances();
    state_.store(ServerState::Stopped);
    log("Server stopped successfully.", LogLevel::info);
}

void ServerEngine::tick_once() {
    if (!host_ || !steam_) return;
    steam_->run_callbacks();
    host_->tick(multiplayer::now_us());
}

void ServerEngine::stop() {
    auto current = state_.load();
    if (current == ServerState::Stopped) return;
    stopping_.store(true);
    state_.store(ServerState::Stopping);
    if (start_thread_.joinable()) {
        start_thread_.join();
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    cleanup_server_instances();
    state_.store(ServerState::Stopped);
}

void ServerEngine::stop_server_async() {
    std::thread([this] {
        stop();
    }).detach();
}

void ServerEngine::restart_server_async() {
    std::thread([this] {
        log("Server restart requested...", LogLevel::info);
        stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        start_server_async();
    }).detach();
}

std::string ServerEngine::execute_command(const std::string& line, std::chrono::milliseconds timeout) {
    if (state_.load() != ServerState::Running || !host_) {
        return "Server is offline. Start the server to execute game console commands.";
    }

    // If caller is already the worker thread
    if (std::this_thread::get_id() == worker_thread_.get_id()) {
        return host_->command(line);
    }

    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();

    {
        std::lock_guard lock(cmd_mutex_);
        pending_commands_.push_back({line, promise});
    }

    if (future.wait_for(timeout) == std::future_status::ready) {
        return future.get();
    }
    return "Command timed out waiting for server tick.";
}

void ServerEngine::queue_command(std::string line) {
    std::lock_guard lock(cmd_mutex_);
    pending_commands_.push_back({std::move(line), nullptr});
}

ServerStatusSnapshot ServerEngine::status_snapshot() const {
    std::lock_guard lock(stats_mutex_);
    ServerStatusSnapshot s = cached_status_;
    s.state = state_.load();
    s.autostart = true;
    s.last_error = last_error();

    switch (s.state) {
        case ServerState::Stopped:
            s.state_text = "OFFLINE";
            s.running = false;
            break;
        case ServerState::Starting:
            s.state_text = "STARTING";
            s.running = false;
            break;
        case ServerState::Running:
            s.state_text = "ONLINE";
            s.running = true;
            break;
        case ServerState::Stopping:
            s.state_text = "STOPPING";
            s.running = false;
            break;
        case ServerState::Error:
            s.state_text = "ERROR";
            s.running = false;
            break;
    }

    if (s.state != ServerState::Running) {
        s.server_name = config_.name;
        s.map_name = config_.map;
        s.player_count = 0;
        s.max_players = config_.max_players;
        s.target_tps = config_.tps;
        s.actual_tps = 0.0;
        s.frame_time_ms = 0.0;
        s.uptime_seconds = 0;
        s.port = config_.port;
        s.query_port = config_.query_port;
        s.voice_allowed = config_.voice_chat;
        s.voice_range = config_.voice_range;
        s.has_password = !config_.password.empty();
        s.is_listed = config_.listed;
        s.steam_token = config_.steam_token;
        s.has_steam_token = !config_.steam_token.empty();
        s.reserved_slots = static_cast<unsigned>(config_.reserved.size());
        s.crowd_budget = config_.crowd_budget;
        s.send_rate = config_.send_rate;
        s.object_limit = config_.object_limit;
        s.bone_scale_limit = config_.bone_scale_limit;
    }
    return s;
}

std::vector<Host::PlayerInfo> ServerEngine::player_list() const {
    if (state_.load() != ServerState::Running) return {};
    std::lock_guard lock(stats_mutex_);
    return cached_players_;
}

ServerEngine::UpdateInfo ServerEngine::update_info() const {
    std::lock_guard lock(update_mutex_);
    return update_info_;
}

void ServerEngine::check_for_updates_async() {
    {
        std::lock_guard lock(update_mutex_);
        if (update_info_.status == UpdateInfo::Status::Checking || update_info_.status == UpdateInfo::Status::Updating) return;
        update_info_.status = UpdateInfo::Status::Checking;
        update_info_.message = "Checking GitHub for new releases...";
    }
    log("Checking GitHub for new dedicated server releases...", LogLevel::info);

    std::thread([this]() {
        try {
            const auto check = check_for_update();
            std::lock_guard lock(update_mutex_);
            if (!check.problem.empty()) {
                update_info_.status = UpdateInfo::Status::Error;
                update_info_.message = "Update check failed: " + check.problem;
                update_info_.available = false;
                log(update_info_.message, LogLevel::warning);
            } else if (check.available) {
                update_info_.status = UpdateInfo::Status::Available;
                update_info_.version = check.version;
                update_info_.message = "New release available: v" + check.version;
                update_info_.available = true;
                log(update_info_.message, LogLevel::info);
            } else {
                update_info_.status = UpdateInfo::Status::UpToDate;
                update_info_.version = check.version;
                update_info_.message = "Server is up to date (v" + (check.version.empty() ? "2.0.3" : check.version) + ")";
                update_info_.available = false;
                log(update_info_.message, LogLevel::info);
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(update_mutex_);
            update_info_.status = UpdateInfo::Status::Error;
            update_info_.message = std::string("Update check exception: ") + e.what();
            update_info_.available = false;
            log(update_info_.message, LogLevel::warning);
        }
    }).detach();
}

void ServerEngine::install_update_async() {
    {
        std::lock_guard lock(update_mutex_);
        if (update_info_.status == UpdateInfo::Status::Updating) return;
        update_info_.status = UpdateInfo::Status::Updating;
        update_info_.message = "Downloading and applying update files...";
    }
    log("Downloading and installing latest update files...", LogLevel::info);

    std::thread([this]() {
        try {
            install_update(folder_);
            std::lock_guard lock(update_mutex_);
            update_info_.status = UpdateInfo::Status::Success;
            update_info_.message = "Update applied successfully! Restart to complete.";
            update_info_.available = false;
            log("Update applied successfully! Restart the server to finish updating.", LogLevel::success);
        } catch (const std::exception& e) {
            std::lock_guard lock(update_mutex_);
            update_info_.status = UpdateInfo::Status::Error;
            update_info_.message = std::string("Update installation failed: ") + e.what();
            log(update_info_.message, LogLevel::error);
        }
    }).detach();
}

double ServerEngine::process_memory_mb() const {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return 0.0;
}

std::vector<ServerEngine::TelemetrySample> ServerEngine::telemetry_history() const {
    std::lock_guard lock(telemetry_mutex_);
    return {telemetry_history_.begin(), telemetry_history_.end()};
}

} // namespace dingosdk::server
