#include "discord_presence.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace dingosdk::discord_presence {
namespace {
// Discord's own limits: a line is 2 to 128 characters.
std::string line(std::string text) {
    if (text.size() > 128) {
        text.resize(125);
        // Not in the middle of a UTF-8 character.
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back();
        if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xc0) text.pop_back();
        text += "...";
    }
    if (text.size() == 1) text += ' ';
    return text;
}

std::mutex mutex;
Presence wanted;            // under mutex
long long wanted_since{};   // Unix seconds: when what the player is doing last changed kind
std::atomic<bool> started{}, on{true};
std::string join_wanted;    // under mutex: take_join

// A join code is "<SteamID64>-<16 hex digits>" (protocol.h: format_invite). Discord hands back
// whatever a status carried, which need not be one of ours.
bool join_code(std::string_view text) {
    const auto dash = text.find('-');
    if (dash == std::string_view::npos || dash < 15 || dash > 20 || text.size() - dash - 1 != 16) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == dash) continue;
        if (!(c >= '0' && c <= '9') && !(i > dash && c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

// Tells Discord how to start ReSkate for someone who presses Join while it is not running:
// the URL scheme Discord opens for an application ("discord-<id>:"), pointing at the launcher
// beside the game. In the player's own part of the registry.
void register_launcher() noexcept {
    try {
        std::wstring game(MAX_PATH, L'\0');
        game.resize(GetModuleFileNameW(nullptr, game.data(), static_cast<DWORD>(game.size())));
        const auto slash = game.find_last_of(L"\\/");
        if (game.empty() || game.size() >= MAX_PATH || slash == std::wstring::npos) return;
        const auto launcher = game.substr(0, slash + 1) + L"ReSkateLauncher.exe";
        if (GetFileAttributesW(launcher.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        const auto scheme = L"discord-" + std::wstring(application_id.begin(), application_id.end());
        const auto put = [](const std::wstring &key, const wchar_t *name, const std::wstring &value) {
            RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        };
        const auto root = L"Software\\Classes\\" + scheme;
        put(root, nullptr, L"URL:Run ReSkate");
        put(root, L"URL Protocol", L"");
        // --no-gui: straight into the game, which is what pressing Join asked for.
        put(root + L"\\shell\\open\\command", nullptr, L"\"" + launcher + L"\" --no-gui");
    } catch (...) {}
}

long long unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The Discord app's pipe, one message at a time. Everything gives up quietly: Discord may
// not be running, may close, or may be a build that answers differently.
class Pipe {
  public:
    ~Pipe() { close(); }
    bool open() {
        close();
        for (int n = 0; n < 10; ++n) {
            const auto name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(n);
            handle_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) return true;
        }
        return false;
    }
    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    bool connected() const { return handle_ != INVALID_HANDLE_VALUE; }
    bool send(unsigned opcode, std::string_view json) {
        const auto bytes = frame(opcode, json);
        DWORD written{};
        if (connected() && WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size())
            return true;
        close();
        return false;
    }
    // The next message if one has arrived (never waits): its opcode, and its JSON in `json`.
    std::optional<unsigned> receive(std::string &json) {
        DWORD waiting{};
        if (!connected()) return std::nullopt;
        if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &waiting, nullptr)) return close(), std::nullopt;
        if (waiting < 8) return std::nullopt;
        std::uint32_t header[2]{};
        DWORD got{};
        if (!ReadFile(handle_, header, sizeof(header), &got, nullptr) || got != sizeof(header) || header[1] > 64 * 1024)
            return close(), std::nullopt;
        json.assign(header[1], '\0');
        for (DWORD at = 0; at < header[1]; at += got)
            if (!ReadFile(handle_, json.data() + at, header[1] - at, &got, nullptr) || !got) return close(), std::nullopt;
        return header[0];
    }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

void run() noexcept {
    try {
        Pipe pipe;
        std::optional<Presence> shown; // what Discord has, while connected
        std::uint64_t next_connect{}, next_send{}, nonce{};
        bool ready{}, said{};
        register_launcher();
        for (;;) {
            Sleep(1000);
            const auto now = GetTickCount64();
            const bool wants = on.load(std::memory_order_relaxed);
            if (!pipe.connected()) {
                ready = false;
                shown.reset();
                if (!wants || now < next_connect) continue;
                next_connect = now + 30000;
                if (!pipe.open() || !pipe.send(0, "{\"v\":1,\"client_id\":\"" + std::string(application_id) + "\"}")) continue;
            }
            // Discord's answers: READY after the handshake, a reply to each command, pings.
            std::string json;
            while (const auto opcode = pipe.receive(json)) {
                if (*opcode == 3) pipe.send(4, json);       // PING -> PONG
                else if (*opcode == 2) pipe.close();        // CLOSE (an application ID it does not know, for one)
                else if (*opcode == 1 && ready) {
                    // Someone pressed Join on a status or an invite: theirs to act on (take_join).
                    if (auto code = join_request(json); !code.empty()) {
                        logging::log(logging::Level::info, logging::Channel::runtime, "Discord: asked to join a session.");
                        std::lock_guard lock(mutex);
                        join_wanted = std::move(code);
                    } else if (const auto asker = join_asker(json); !asker.empty() && shown && !shown->join.empty() && wants) {
                        // "Ask to Join" on a session anyone may walk into: yes, without asking the player.
                        pipe.send(1, "{\"cmd\":\"SEND_ACTIVITY_JOIN_INVITE\",\"args\":{\"user_id\":\"" + asker + "\"},\"nonce\":\"" +
                                         std::to_string(++nonce) + "\"}");
                        logging::log(logging::Level::info, logging::Channel::runtime, "Discord: let someone who asked into this session.");
                    }
                } else if (*opcode == 1 && json.find("\"READY\"") != std::string::npos) {
                    ready = true;
                    // Discord then tells this game when its player asks to join someone.
                    pipe.send(1, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"" + std::to_string(++nonce) + "\"}");
                    // And when someone asks to join this player.
                    pipe.send(1, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN_REQUEST\",\"nonce\":\"" + std::to_string(++nonce) + "\"}");
                    if (!std::exchange(said, true))
                        logging::log(logging::Level::info, logging::Channel::runtime, "Discord: connected; showing what you are doing as your status.");
                }
            }
            if (!pipe.connected() || !ready) continue;
            if (!wants) {
                // Switched off: take the status down, and leave Discord alone.
                if (shown) pipe.send(1, activity_command(nullptr, GetCurrentProcessId(), 0, ++nonce));
                pipe.close();
                continue;
            }
            Presence presence;
            long long since{};
            {
                std::lock_guard lock(mutex);
                presence = wanted;
                since = wanted_since;
            }
            // Discord takes a few updates in twenty seconds and drops the rest.
            if (shown == presence || now < next_send) continue;
            if (pipe.send(1, activity_command(&presence, GetCurrentProcessId(), since, ++nonce))) {
                shown = std::move(presence);
                next_send = now + 5000;
            }
        }
    } catch (...) {}
}
} // namespace

std::string frame(unsigned opcode, std::string_view json) {
    std::string out(8, '\0');
    const std::uint32_t header[2]{opcode, static_cast<std::uint32_t>(json.size())};
    std::memcpy(out.data(), header, sizeof(header));
    out += json;
    return out;
}

std::string activity_command(const Presence *presence, unsigned long process, long long started_at, unsigned long long nonce) {
    auto args = Json::object();
    args["pid"] = static_cast<std::uint64_t>(process);
    if (presence) {
        auto activity = Json::object();
        if (!presence->details.empty()) activity["details"] = line(presence->details);
        if (!presence->state.empty()) activity["state"] = line(presence->state);
        if (started_at > 0) {
            auto timestamps = Json::object();
            timestamps["start"] = static_cast<std::int64_t>(started_at);
            activity["timestamps"] = std::move(timestamps);
        }
        if (presence->party_size > 0 && presence->party_most >= presence->party_size && !presence->party.empty()) {
            auto party = Json::object();
            party["id"] = presence->party;
            party["size"] = Json::array({Json(presence->party_size), Json(presence->party_most)});
            activity["party"] = std::move(party);
            // The Join button: only with room left, and only a code that is one.
            if (presence->party_size < presence->party_most && join_code(presence->join)) {
                auto secrets = Json::object();
                secrets["join"] = presence->join;
                activity["secrets"] = std::move(secrets);
                // A public party: Discord lets people join it outright ("Join") where a
                // private one makes them ask first ("Ask to Join"). The code is only here
                // for a session anyone may walk into.
                activity["party"]["privacy"] = 1;
            }
        }
        auto assets = Json::object();
        assets["large_image"] = "resk8";
        assets["large_text"] = "ReSkate";
        activity["assets"] = std::move(assets);
        args["activity"] = std::move(activity);
    } else {
        args["activity"] = Json();
    }
    auto command = Json::object();
    command["cmd"] = "SET_ACTIVITY";
    command["args"] = std::move(args);
    command["nonce"] = std::to_string(nonce);
    return command.dump();
}

std::string join_request(std::string_view json) {
    try {
        const auto message = Json::parse(json);
        if (!message.is_object() || message.value("cmd", "") != "DISPATCH" || message.value("evt", "") != "ACTIVITY_JOIN" ||
            !message.contains("data") || !message.at("data").is_object())
            return {};
        auto code = message.at("data").value("secret", "");
        return join_code(code) ? code : std::string();
    } catch (...) {
        return {};
    }
}

std::string join_asker(std::string_view json) {
    try {
        const auto message = Json::parse(json);
        if (!message.is_object() || message.value("cmd", "") != "DISPATCH" || message.value("evt", "") != "ACTIVITY_JOIN_REQUEST" ||
            !message.contains("data") || !message.at("data").is_object() || !message.at("data").contains("user") ||
            !message.at("data").at("user").is_object())
            return {};
        // A Discord user ID is a number, sent as text.
        auto id = message.at("data").at("user").value("id", "");
        const bool digits = !id.empty() && id.size() <= 20 && id.find_first_not_of("0123456789") == std::string::npos;
        return digits ? id : std::string();
    } catch (...) {
        return {};
    }
}

std::string take_join() noexcept {
    try {
        std::lock_guard lock(mutex);
        return std::exchange(join_wanted, {});
    } catch (...) {
        return {};
    }
}

bool available() noexcept {
    static const bool allowed = [] {
        // The launcher says (launch.cpp): "0" is its switch turned off.
        wchar_t value[2]{};
        const auto length = GetEnvironmentVariableW(L"RESKATE_DISCORD", value, 2);
        return !(length == 1 && value[0] == L'0');
    }();
    return allowed && !application_id.empty();
}

bool enabled() noexcept { return on.load(std::memory_order_relaxed); }
void set_enabled(bool value) noexcept {
    on.store(value, std::memory_order_relaxed);
    profile_runtime::set_local_preference("DiscordPresence", value);
}

void update(Presence presence) noexcept {
    if (!available()) return;
    try {
        on.store(profile_runtime::local_preference("DiscordPresence").value_or(true), std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex);
            // The timer under the status counts from when the player went into or out of a
            // session, not from every change of map or player count.
            if (!wanted_since || wanted.party != presence.party) wanted_since = unix_now();
            wanted = std::move(presence);
        }
        if (on.load(std::memory_order_relaxed) && !started.exchange(true)) std::thread(run).detach();
    } catch (...) {}
}
} // namespace dingosdk::discord_presence
