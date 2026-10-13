#include "spotify_device.h"
#include "app_audio_capture.h"
#include "local_music_ui.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "Extension/Boot/user_data_redirect.h"
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if DINGOSDK_SPOTIFY_DEVICE
// SpotifyHelper/src/lib.rs, linked into the DLL (cmake/Runtime.cmake).
extern "C" {
int reskate_spotify_start(const char* name, const char* cache, void (*sound)(const float* samples, std::uint32_t frames), void (*told)(const char* line));
void reskate_spotify_ask(const char* line);
}
#endif

namespace dingosdk::spotify_device {
namespace {
constexpr unsigned device_rate = 44100;
std::atomic<bool> started{}, running{}, ready{}, account{};
std::atomic<std::uint64_t> changes{1};
std::mutex mutex; // the track
external_player::Track track;
std::string user_name, gave_up_why;
std::atomic<bool> not_premium{};

void say(logging::Level level, const std::string& text) noexcept {
    try {
        logging::log(level, logging::Channel::music, "Spotify device: {}", text);
    } catch (...) {}
}
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    std::wstring out(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
    return out;
}
std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    std::string out(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    return out;
}
void ask(const std::string& line) noexcept {
#if DINGOSDK_SPOTIFY_DEVICE
    if (running.load(std::memory_order_relaxed)) reskate_spotify_ask(line.c_str());
#else
    (void)line;
#endif
}

// A song's cover, fetched from where Spotify keeps it, for the music screen.
void fetch_cover(std::string address, std::uint64_t of) {
    try {
        if (!address.starts_with("https://")) return;
        const auto image = https::get_text(wide(address), 4 * 1024 * 1024, 10, L"ReSkate");
        if (!image) return;
        std::lock_guard lock(mutex);
        if (track.revision != of) return; // (the song has changed since)
        track.artwork.assign(image->begin(), image->end());
        ++track.artwork_revision;
        changes.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {}
}

// A line the device told the game (SpotifyHelper/src/lib.rs), on a thread of the device's.
void told(const char* text) {
    try {
        std::vector<std::string> parts;
        std::stringstream stream{std::string(text)};
        for (std::string part; std::getline(stream, part, '\t');) parts.push_back(part);
        if (parts.empty()) return;
        if (parts[0] == "LOG") {
            static std::atomic<int> logged{};
            if (parts.size() > 1 && logged.fetch_add(1, std::memory_order_relaxed) < 300) say(logging::Level::debug, "(librespot) " + parts[1]);
            // (What it says just before it gives up on an account that is not Premium.)
            if (parts.size() > 1 && parts[1].find("does not support") != std::string::npos && parts[1].find("accounts") != std::string::npos) not_premium.store(true);
        } else if (parts[0] == "READY") {
            ready.store(true);
            say(logging::Level::info, "\"ReSkate\" is among Spotify's devices on this network.");
        } else if (parts[0] == "CONNECTED") {
            {
                std::lock_guard lock(mutex);
                user_name = parts.size() > 1 ? parts[1] : std::string();
            }
            if (!account.exchange(true)) {
                say(logging::Level::info, "a Spotify account is on it.");
                profile_runtime::request_music_catalog_refresh(); // (its tiles are in the music screen from now)
            }
            changes.fetch_add(1, std::memory_order_relaxed);
        } else if (parts[0] == "DISCONNECTED") {
            if (account.exchange(false)) say(logging::Level::info, "no Spotify account is on it.");
            changes.fetch_add(1, std::memory_order_relaxed);
        } else if (parts[0] == "ENDED") {
            running.store(false);
            account.store(false);
            say(logging::Level::warning, "it has stopped.");
        } else if (parts[0] == "TRACK" && parts.size() >= 4) {
            std::uint64_t of{};
            {
                std::lock_guard lock(mutex);
                track.available = true;
                track.spotify = true;
                track.app = "ReSkate device";
                track.can_next = track.can_previous = track.can_pause = track.can_play = true;
                track.artist = parts[2];
                track.title = parts[3];
                track.album.clear();
                track.artwork.clear();
                of = ++track.revision;
            }
            changes.fetch_add(1, std::memory_order_relaxed);
            if (parts.size() > 4 && !parts[4].empty()) std::thread(fetch_cover, parts[4], of).detach();
        } else if (parts[0] == "STATE" && parts.size() >= 2) {
            std::lock_guard lock(mutex);
            track.playing = parts[1] == "playing";
            changes.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {}
}

// The sound, on the device's own thread. It is taken as fast as the game hears it and no faster
// (a fraction of a second is kept ahead): this returning when there is room for more is what
// keeps Spotify at the speed of the game.
void sound(const float* samples, std::uint32_t frames) {
    // (For half a second at the most: when the game is not listening at all, the sound is let go
    // by, so the device can still be told to pause.)
    for (int waited = 0; app_audio::ahead_frames() > device_rate * 15 / 100; ++waited) {
        if (waited >= 125) return;
        Sleep(4);
    }
    app_audio::push(samples, frames);
}
} // namespace

void start() noexcept {
    if (started.exchange(true)) return;
#if DINGOSDK_SPOTIFY_DEVICE
    try {
        const auto cache = redirected_local_app_data().parent_path() / L"spotify-device";
        std::error_code ignored;
        std::filesystem::create_directories(cache, ignored);
        running.store(true);
        if (!reskate_spotify_start("ReSkate", narrow(cache.wstring()).c_str(), &sound, &told)) {
            running.store(false);
            say(logging::Level::warning, "it could not be started.");
        }
    } catch (...) {
        running.store(false);
    }
#else
    say(logging::Level::info, "not built into this ReSkate.dll (it needs Rust's cargo when ReSkate is built): Spotify is played through its own app.");
#endif
}
void library_gave_up() noexcept {
    running.store(false);
    account.store(false);
    try {
        const bool premium = !not_premium.load();
        {
            std::lock_guard lock(mutex);
            gave_up_why = premium ? "it stopped on a fault of its own; it is back the next time the game is started"
                                  : "that Spotify account is not Premium, which Spotify asks for to play to a device; the account was forgotten";
        }
        if (!premium) {
            // (Kept, it would be tried again, and refused again, at every start.)
            std::error_code ignored;
            std::filesystem::remove(redirected_local_app_data().parent_path() / L"spotify-device" / L"credentials.json", ignored);
        }
        say(logging::Level::warning, premium ? "the library asked for the game to be ended; it was stopped instead, and the device is off for this run."
                                             : "that Spotify account is not Premium: the device is off, and the account was forgotten. The game goes on.");
    } catch (...) {}
}
bool in_use() noexcept {
    if (!running.load(std::memory_order_relaxed) || not_premium.load(std::memory_order_relaxed)) return false;
    if (account.load(std::memory_order_relaxed)) return true;
    try {
        std::error_code ignored;
        return std::filesystem::exists(redirected_local_app_data().parent_path() / L"spotify-device" / L"credentials.json", ignored);
    } catch (...) {
        return false;
    }
}
bool connected() noexcept { return running.load(std::memory_order_relaxed) && account.load(std::memory_order_relaxed); }
external_player::Track now() {
    std::lock_guard lock(mutex);
    return track;
}
std::uint64_t revision() noexcept { return changes.load(std::memory_order_relaxed); }
void send(external_player::Command command) noexcept {
    switch (command) {
    case external_player::Command::play: ask("play"); break;
    case external_player::Command::pause: ask("pause"); break;
    case external_player::Command::next: ask("next"); break;
    case external_player::Command::previous: ask("prev"); break;
    case external_player::Command::toggle: {
        bool playing{};
        {
            std::lock_guard lock(mutex);
            playing = track.playing;
        }
        ask(playing ? "pause" : "play");
        break;
    }
    }
}
void activate() noexcept { ask("activate"); }
void start_radio() noexcept {
    try {
        std::thread([] {
            ask("activate");
            ask("play");
            // (A moment for what Spotify was playing to come over; with nothing to come over,
            // there is nothing playing after it.)
            Sleep(2000);
            std::string user;
            bool playing{};
            {
                std::lock_guard lock(mutex);
                user = user_name;
                playing = track.playing;
            }
            if (!playing && !user.empty()) {
                say(logging::Level::info, "nothing was playing to go on with: Liked Songs, in no order.");
                ask("shuffle spotify:user:" + user + ":collection");
            }
        }).detach();
    } catch (...) {}
}
void load(const std::string& context, const std::string& track_uri) noexcept { ask("load " + context + (track_uri.empty() ? std::string() : " " + track_uri)); }
void load_tracks(const std::string& tracks_spaced, unsigned index) noexcept { ask("tracks " + std::to_string(index) + " " + tracks_spaced); }
std::string status() {
    std::lock_guard lock(mutex);
    if (!gave_up_why.empty()) return "Spotify device: off (" + gave_up_why + ").";
    return std::format("Spotify device: {}{}.", !running.load() ? "not running (Spotify is played through its own app)" : !ready.load() ? "starting" : account.load() ? "a Spotify account is on it" :
        "in Spotify's list of devices; pick \"ReSkate\" there once to use it", account.load() && !user_name.empty() ? " (" + user_name + ")" : std::string());
}
} // namespace dingosdk::spotify_device
