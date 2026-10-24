#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Plugins' server copies on a dedicated server (Extension/Plugins): the session's authority, where
// a game mode's rules run. The operator installs them in Plugins beside the server, a folder a
// plugin (with its reskate-plugin.json, "side" "server" or "both"); they load, reload and unload as
// the folder changes, checked every 2 s. The server has no game: what a plugin may do here is the
// session's messages, its players and their chat, the server API (kick, say, tell, move), the map
// (session.world, travel.levels and travel.load) and the time of day for everyone (world.set_time),
// its storage and reading the content folders of the mods in Mods (mods.content).
// Everything here runs on the server's tick thread.
namespace dingosdk::server {
class Host;

class ServerPlugins {
  public:
    using Log = std::function<void(const std::string &)>;
    ServerPlugins(Host &host, std::filesystem::path folder, std::filesystem::path storage, Log log);
    ~ServerPlugins();
    ServerPlugins(const ServerPlugins &) = delete;
    ServerPlugins &operator=(const ServerPlugins &) = delete;

    void tick(std::uint64_t now_us);
    // A player's plugin message (plugin_wire.h): for the authority, or for everyone.
    void receive(std::uint64_t from, const std::vector<std::uint8_t> &bytes);
    // A console line for the plugins: "plugin list|reload|off|on" or a plugin's own command.
    // Empty when it is neither.
    std::optional<std::string> command(std::string_view line);
    // Whether a running plugin hears chat (chat.read).
    bool hears_chat() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace dingosdk::server
