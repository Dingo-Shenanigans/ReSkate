#include "external_player.h"
#include "plex_web.h"
#include "spotify_device.h"
#include "youtube_music.h"
#include <Windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#pragma comment(lib, "windowsapp")

namespace dingosdk::external_player {
namespace {
namespace control = winrt::Windows::Media::Control;
namespace streams = winrt::Windows::Storage::Streams;
using Status = control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

constexpr std::uint32_t largest_artwork = 4 * 1024 * 1024;

std::mutex mutex;
std::condition_variable wake;
Track current;
// (Each with whose session it was for when it was sent: the station may have gone on to
// another's by the time it is done, and a pause for the one left is still for it.)
std::deque<std::pair<Command, std::string>> commands;
std::atomic<std::uint64_t> revision_value{};
std::atomic<bool> running{}, stopping{};
std::thread worker;

bool spotify_app(const std::string& id) {
    std::string lower(id);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("spotify") != std::string::npos;
}
// Spotify's session, or none: another player's (a video in a browser) is not what the game's
// Spotify station shows or drives.
std::mutex app_mutex;
std::string wanted_app; // (lower case, without ".exe"; empty: Spotify)
control::GlobalSystemMediaTransportControlsSession pick(const control::GlobalSystemMediaTransportControlsSessionManager& manager, const std::string* of = nullptr) {
    std::string wanted;
    if (of) wanted = *of;
    else {
        std::lock_guard lock(app_mutex);
        wanted = wanted_app;
    }
    for (const auto& session : manager.GetSessions()) {
        auto id = winrt::to_string(session.SourceAppUserModelId());
        if (wanted.empty() ? spotify_app(id) : (std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }), id.find(wanted) != std::string::npos))
            return session;
    }
    // (A browser's session does not always go by the browser's name.)
    return wanted.empty() ? nullptr : manager.GetCurrentSession();
}
std::vector<std::uint8_t> read_artwork(const streams::IRandomAccessStreamReference& reference) {
    if (!reference) return {};
    const auto stream = reference.OpenReadAsync().get();
    const auto size = stream.Size();
    if (!size || size > largest_artwork) return {};
    streams::DataReader reader(stream);
    reader.LoadAsync(static_cast<std::uint32_t>(size)).get();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    reader.ReadBytes(bytes);
    return bytes;
}
// What Plex's own server says, laid over what Windows gives: the server's title, artist, album
// and cover are the ones the player's library has, which a player's media session often shortens
// or leaves without a cover. Used for Plex's own apps, and for any other that is playing the song
// the server says an account of the player is.
bool plex_is(const std::string& app) {
    std::string lower(app);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("plex") != std::string::npos;
}
bool same_text(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}
void lay_plex(Track& track, const Track& before, bool force) {
    const auto plex = plex_web::now_playing();
    if (!plex.valid || plex.title.empty()) return;
    if (!force && !same_text(plex.title, track.title)) return;
    track.title = plex.title;
    track.artist = plex.artist;
    track.album = plex.album;
    track.playing = plex.playing;
    if (!plex.art.empty()) {
        track.artwork = plex.art;
        track.artwork_revision = plex.art == before.artwork ? before.artwork_revision : before.artwork_revision + 1;
    }
}
// One look at the player: the track, with its cover read again only when the song changed.
Track look(const control::GlobalSystemMediaTransportControlsSessionManager& manager, const Track& before) {
    Track track;
    const auto session = pick(manager);
    if (!session) {
        // Plex's player has no media session of its own to read (a Plex app on another device):
        // the server's word is all there is, and its transport is the server's.
        bool plex_wanted{};
        {
            std::lock_guard lock(app_mutex);
            plex_wanted = wanted_app.find("plex") != std::string::npos;
        }
        if (plex_wanted) {
            track.app = "plex";
            lay_plex(track, before, true);
            if (!track.title.empty()) {
                track.available = true;
                track.can_next = track.can_previous = track.can_pause = track.can_play = true;
            }
        }
        return track;
    }
    track.available = true;
    track.app = winrt::to_string(session.SourceAppUserModelId());
    track.spotify = spotify_app(track.app);
    if (const auto info = session.GetPlaybackInfo()) {
        track.playing = info.PlaybackStatus() == Status::Playing;
        const auto controls = info.Controls();
        track.can_next = controls.IsNextEnabled();
        track.can_previous = controls.IsPreviousEnabled();
        track.can_pause = controls.IsPauseEnabled();
        track.can_play = controls.IsPlayEnabled();
    }
    const auto properties = session.TryGetMediaPropertiesAsync().get();
    if (!properties) return track;
    track.title = winrt::to_string(properties.Title());
    track.artist = winrt::to_string(properties.Artist());
    track.album = winrt::to_string(properties.AlbumTitle());
    // (A player often publishes the new title a moment before the new cover: the cover is
    // read again for a few looks after a change, which `artwork_pending` counts down.)
    const bool same_song = before.available && before.app == track.app && before.title == track.title && before.artist == track.artist;
    if (same_song && !before.artwork.empty()) {
        track.artwork = before.artwork;
        track.artwork_revision = before.artwork_revision;
    } else {
        try {
            track.artwork = read_artwork(properties.Thumbnail());
        } catch (const winrt::hresult_error&) {}
        track.artwork_revision = track.artwork == before.artwork ? before.artwork_revision : before.artwork_revision + 1;
    }
    if (!track.spotify) lay_plex(track, before, plex_is(track.app));
    return track;
}
bool same(const Track& a, const Track& b) {
    return a.available == b.available && a.spotify == b.spotify && a.playing == b.playing && a.can_next == b.can_next &&
           a.can_previous == b.can_previous && a.can_pause == b.can_pause && a.can_play == b.can_play && a.app == b.app &&
           a.title == b.title && a.artist == b.artist && a.album == b.album && a.artwork_revision == b.artwork_revision;
}
void run_command(const control::GlobalSystemMediaTransportControlsSessionManager& manager, Command command, const std::string& app) {
    const auto session = pick(manager, &app);
    if (!session) {
        // No media session for a Plex player: the server is asked to drive it.
        if (app.find("plex") != std::string::npos) {
            using plex_web::Action;
            switch (command) {
            case Command::play: plex_web::control(Action::play); break;
            case Command::pause: plex_web::control(Action::pause); break;
            case Command::toggle: plex_web::control(plex_web::now_playing().playing ? Action::pause : Action::play); break;
            case Command::next: plex_web::control(Action::next); break;
            case Command::previous: plex_web::control(Action::previous); break;
            }
        }
        return;
    }
    switch (command) {
    case Command::play: session.TryPlayAsync().get(); break;
    case Command::pause: session.TryPauseAsync().get(); break;
    case Command::toggle: session.TryTogglePlayPauseAsync().get(); break;
    case Command::next: session.TrySkipNextAsync().get(); break;
    case Command::previous: session.TrySkipPreviousAsync().get(); break;
    }
}
void watch() noexcept {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        running.store(false);
        return;
    }
    try {
        control::GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
        while (!stopping.load()) {
            try {
                if (!manager) manager = control::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                std::deque<std::pair<Command, std::string>> todo;
                Track before;
                {
                    std::lock_guard lock(mutex);
                    todo.swap(commands);
                    before = current;
                }
                for (const auto& [command, app] : todo) run_command(manager, command, app);
                auto track = look(manager, before);
                if (!same(track, before)) {
                    track.revision = before.revision + 1;
                    std::lock_guard lock(mutex);
                    current = std::move(track);
                    revision_value.store(current.revision);
                }
            } catch (const winrt::hresult_error&) {
                manager = nullptr; // asked for again on the next look
            }
            // A look a second, and at once after a command (so its effect shows) or a stop.
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(commands.empty() ? 1000 : 150), [] { return stopping.load() || !commands.empty(); });
        }
    } catch (...) {}
    winrt::uninit_apartment();
    running.store(false);
}
} // namespace

void start() noexcept {
    if (running.exchange(true)) return;
    stopping.store(false);
    try {
        worker = std::thread(watch);
    } catch (...) {
        running.store(false);
    }
}
void stop() noexcept {
    stopping.store(true);
    wake.notify_all();
    if (worker.joinable()) worker.join();
}
// (When a Spotify account is on the game's own device, spotify_device.h, that is the player:
// what it plays is what there is, and the game's play, pause and next go to it.)
bool others() {
    std::lock_guard lock(app_mutex);
    return !wanted_app.empty();
}
void use_app(const std::string& executable) {
    auto name = executable;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.ends_with(".exe")) name.resize(name.size() - 4);
    std::lock_guard lock(app_mutex);
    wanted_app = name;
}
Track now() {
    if (youtube_music::on()) return youtube_music::now();
    if (!others() && spotify_device::connected()) return spotify_device::now();
    std::lock_guard lock(mutex);
    return current;
}
std::uint64_t revision() noexcept { return revision_value.load() + (spotify_device::revision() << 24) + (youtube_music::revision() << 40) + (youtube_music::on() ? 1ull << 62 : 0); }
void send(Command command) noexcept {
    if (youtube_music::on()) return youtube_music::send(command);
    if (!others() && spotify_device::connected()) return spotify_device::send(command);
    try {
        {
            std::lock_guard lock(mutex);
            std::string app;
            {
                std::lock_guard app_lock(app_mutex);
                app = wanted_app;
            }
            if (commands.size() < 16) commands.emplace_back(command, std::move(app));
        }
        wake.notify_all();
    } catch (...) {}
}
} // namespace dingosdk::external_player
