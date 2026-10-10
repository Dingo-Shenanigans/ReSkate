#pragma once

#include "server_config.h"
#include "server_host.h"
#include "steam_server.h"
#include "Extension/Multiplayer/Steam/steam_transport.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>

namespace dingosdk::server {

enum class LogLevel {
    info,
    success,
    warning,
    error,
    command,
    chat
};

enum class ServerState {
    Stopped,
    Starting,
    Running,
    Stopping,
    Error
};

struct ServerLogEntry {
    std::size_t id{};
    std::string timestamp; // "HH:MM:SS"
    std::string text;
    LogLevel level{LogLevel::info};
};

struct ServerStatusSnapshot {
    ServerState state{ServerState::Stopped};
    std::string state_text{"OFFLINE"};
    std::string last_error;
    std::string server_name;
    std::string map_name;
    unsigned player_count{};
    unsigned max_players{};
    unsigned target_tps{dedicated_tps};
    double actual_tps{0.0};
    double frame_time_ms{0.0};
    std::uint64_t uptime_seconds{};
    unsigned reserved_slots{0};
    unsigned crowd_budget{0};
    unsigned send_rate{900};
    unsigned object_limit{0};
    float bone_scale_limit{1.0f};
    std::string invite_code;
    std::string public_ip;
    std::uint64_t steam_id{};
    std::uint16_t port{27015};
    std::uint16_t query_port{27016};
    bool voice_allowed{true};
    float voice_range{60.0f};
    bool has_password{false};
    bool is_listed{true};
    bool autostart{false};
    std::string steam_token;
    bool has_steam_token{false};
    bool running{false};
};

class ServerEngine {
public:
    static ServerEngine& instance();

    // Background host lifecycle and session management.
    bool load_environment(const std::filesystem::path& folder, const std::filesystem::path& config_file, bool skip_update = true);
    void start_server_async();
    void stop_server_async();
    void restart_server_async();
    void clear_error();

    ServerState state() const noexcept { return state_.load(std::memory_order_relaxed); }
    std::string last_error() const;

    // Backward-compatible wrappers
    bool init(const std::filesystem::path& folder, const std::filesystem::path& config_file, bool skip_update = true) {
        return load_environment(folder, config_file, skip_update);
    }
    void start_async() { start_server_async(); }
    void stop();
    void tick_once();
    bool is_running() const noexcept { return state_.load(std::memory_order_relaxed) == ServerState::Running; }

    // Thread-safe command execution
    std::string execute_command(const std::string& line, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
    void queue_command(std::string line);

    // Snapshots for GUI
    ServerStatusSnapshot status_snapshot() const;
    std::vector<Host::PlayerInfo> player_list() const;
    std::vector<ServerLogEntry> log_entries(std::size_t from_id = 0) const;

    // Updates & Releases API
    struct UpdateInfo {
        enum class Status { Idle, Checking, UpToDate, Available, Updating, Success, Error };
        Status status{Status::Idle};
        std::string version;
        std::string message{"No update check run yet."};
        bool available{false};
    };

    UpdateInfo update_info() const;
    void check_for_updates_async();
    void install_update_async();

    // Logging
    void log(std::string text, LogLevel level = LogLevel::info);
    using LogCallback = std::function<void(const ServerLogEntry&)>;
    void add_log_callback(LogCallback cb);

    // Process memory usage (MB)
    double process_memory_mb() const;

    // Rolling Telemetry History
    struct TelemetrySample {
        std::uint64_t uptime_seconds{0};
        float tps{0.0f};
        float frame_time_ms{0.0f};
        float memory_mb{0.0f};
        unsigned player_count{0};
    };

    std::vector<TelemetrySample> telemetry_history() const;

    ServerConfig& config() { return config_; }
    const std::filesystem::path& folder() const noexcept { return folder_; }

private:
    ServerEngine();
    ~ServerEngine();

    void worker_loop();

    std::filesystem::path folder_;
    std::filesystem::path config_file_;
    ServerConfig config_;
    bool skip_update_{true};

    std::unique_ptr<SteamServer> steam_;
    std::unique_ptr<multiplayer::SteamTransport> transport_;
    std::unique_ptr<Host> host_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<ServerState> state_{ServerState::Stopped};
    mutable std::mutex error_mutex_;
    std::string last_error_;

    std::thread worker_thread_;
    std::thread start_thread_;

    void run_startup();
    void cleanup_server_instances();

    mutable std::mutex log_mutex_;
    std::deque<ServerLogEntry> log_history_;
    std::size_t next_log_id_{1};
    std::vector<LogCallback> log_callbacks_;
    std::ofstream log_file_;

    struct PendingCommand {
        std::string command;
        std::shared_ptr<std::promise<std::string>> promise;
    };
    mutable std::mutex cmd_mutex_;
    std::deque<PendingCommand> pending_commands_;

    std::chrono::steady_clock::time_point start_time_;
    mutable std::mutex stats_mutex_;
    double measured_tps_{static_cast<double>(dedicated_tps)};
    double frame_time_ms_{0.0};
    mutable std::vector<Host::PlayerInfo> cached_players_;
    mutable ServerStatusSnapshot cached_status_;

    mutable std::mutex update_mutex_;
    UpdateInfo update_info_;

    mutable std::mutex telemetry_mutex_;
    std::deque<TelemetrySample> telemetry_history_;
};

} // namespace dingosdk::server
