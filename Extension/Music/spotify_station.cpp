#include "spotify_station.h"
#include "external_player.h"
#include "spotify_cover.h"
#include "spotify_web.h"
#include "spotify_device.h"
#include "external_audio.h"
#include "youtube_music.h"
#include "spotify_rows.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include <set>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>

namespace dingosdk::spotify_station {
namespace {
constexpr std::uint32_t no_channel = ~0u;
std::atomic<std::uint32_t> station_channel{no_channel}; // the music channel bound to one of them
std::atomic<int> station_index{-1};                     // and which
std::atomic<std::uint64_t> binds_seen{};
std::atomic<std::size_t> station_count{};
std::atomic<std::uint64_t> last_said{~0ull};
std::atomic<bool> skip{};
// The channel last bound to a playlist that is no station, and when.
std::atomic<std::uint32_t> plain_channel{no_channel};
std::atomic<std::uint64_t> plain_at{};
void left_at_store() noexcept;
// The music player's queue, as last seen choosing a song while a station is on, how it was
// flagged when the player asked for the next song, and when this last asked it itself.
std::atomic<std::uintptr_t> player_queue{}, player_queue_table{}; // (the table a queue begins with: by it the queue is known to be there still)
std::atomic<std::uint8_t> asked_flag{1};
std::atomic<std::uint64_t> self_asked{};
constexpr std::size_t queue_asked = 0xf1, queue_start = 0xf2;

// The stations, and the artist and title of the one that is on (see the header). The game may
// still be reading a text it was given earlier, so none of them is ever freed: a song's worth
// of bytes for each song heard.
constexpr std::size_t artist_at = 0x18, title_at = 0x20;
std::mutex mutex;
std::vector<Station> stations;
std::array<unsigned char, 16> picked_row{};
std::atomic<bool> row_picked{};
std::uintptr_t metadata_table{};
// The songs whose artist and title read as Spotify's now, with what they read before: the
// station's own, and any other song of the game's begun while it is on (a speaker's in the
// world, one grabbed from it), whose sound is Spotify's as well. `current_*`: what they read.
struct Shown {
    std::uintptr_t metadata{}, artist{}, title{};
};
std::vector<Shown> shown;
// Those that are to read so again after they were put back for the catalog to be read (the
// station's own is not among them: it is shown again as a matter of course).
std::vector<std::uintptr_t> again;
// What each of the game's music channels last began (its metadata asset).
std::array<std::uintptr_t, 16> channel_song{};
std::uintptr_t current_artist{}, current_title{};
// The texts the game's songs are made to read: each kept once, for as long as the game runs
// (the game holds their addresses). A set's strings stay where they are as it grows.
//
// They were copied anew on every call of show(), which is every frame while the station's own
// song cannot be made to read (its asset gone after a change of map, or 64 songs shown
// already): memory that only ever grew, by two copies a frame (reported 2026-10-13).
std::set<std::string, std::less<>> kept;

bool poke(std::uintptr_t address, std::uintptr_t value) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool there(std::uintptr_t metadata) noexcept {
    std::uintptr_t table{};
    return metadata && memory::peek(metadata, table) && table == metadata_table;
}
std::uintptr_t keep(const std::string& text) {
    return reinterpret_cast<std::uintptr_t>(kept.insert(text).first->c_str());
}
// (With the mutex held.)
void restore_locked() noexcept {
    for (const auto& entry : shown)
        if (there(entry.metadata)) {
            poke(entry.metadata + artist_at, entry.artist);
            poke(entry.metadata + title_at, entry.title);
        }
    shown.clear();
}
// (With the mutex held.) A song reads as Spotify's from now on.
void show_locked(std::uintptr_t metadata) noexcept {
    if (!current_artist || !current_title || !there(metadata)) return;
    if (std::none_of(shown.begin(), shown.end(), [&](const Shown& entry) { return entry.metadata == metadata; })) {
        Shown entry{metadata};
        if (shown.size() >= 64 || !memory::peek(metadata + artist_at, entry.artist) || !memory::peek(metadata + title_at, entry.title)) return;
        shown.push_back(entry);
    }
    const bool done = poke(metadata + artist_at, current_artist) && poke(metadata + title_at, current_title);
    // (Each song made to read as Spotify's, into the log while the speakers are worked out.)
    if (static int said{}; said++ < 60) {
        try {
            logging::log(logging::Level::debug, logging::Channel::music, "Spotify station: a song of the game's {} as Spotify's ({} in all).", done ? "now reads" : "could not be made to read", shown.size());
        } catch (...) {}
    }
}
bool is_shown_locked(std::uintptr_t metadata) noexcept {
    return std::any_of(shown.begin(), shown.end(), [&](const Shown& entry) { return entry.metadata == metadata; });
}
void show(int index, const std::string& artist, const std::string& title) {
    std::lock_guard lock(mutex);
    if (index < 0 || static_cast<std::size_t>(index) >= stations.size() || artist.empty() || title.empty()) return;
    current_artist = keep(artist);
    current_title = keep(title);
    show_locked(stations[static_cast<std::size_t>(index)].metadata);
    for (const auto metadata : again) show_locked(metadata);
    again.clear();
    for (const auto metadata : channel_song)
        if (metadata) show_locked(metadata);
    for (const auto& entry : std::vector<Shown>(shown)) show_locked(entry.metadata);
}
// Which of YouTube Music's playlists a station's is ("~yt:<number>").
std::size_t youtube_list(const std::string& playlist) {
    try {
        return std::stoul(playlist.substr(std::strlen(youtube_music::station_prefix)));
    } catch (...) {
        return ~std::size_t{};
    }
}
} // namespace

bool spotify_used() noexcept {
    try {
        return spotify_device::in_use() || spotify_web::state().signed_in;
    } catch (...) {
        return false;
    }
}
bool is_station(std::string_view id) noexcept { return id == playlist_id || id.starts_with(playlist_prefix); }

void set_stations(std::vector<Station> next, std::uintptr_t table) {
    std::lock_guard lock(mutex);
    restore_locked();
    stations = std::move(next);
    metadata_table = table;
    station_count.store(stations.size(), std::memory_order_release);
}
std::vector<std::string> playlist_ids() {
    std::lock_guard lock(mutex);
    std::vector<std::string> ids;
    for (const auto& station : stations) ids.push_back(station.id);
    return ids;
}
bool has_stations() noexcept { return station_count.load(std::memory_order_acquire) != 0; }
int station_of(const unsigned char* members, std::uint32_t count, unsigned char* carrier, bool& add) noexcept {
    std::lock_guard lock(mutex);
    add = false;
    int best{-1};
    std::uint32_t best_rows{};
    for (std::size_t i = 0; i < stations.size(); ++i) {
        const auto& station = stations[i];
        // Every member is one of its rows or its carrier, and the more of its rows the better.
        std::uint32_t rows{};
        bool mine{true}, carried{};
        for (std::uint32_t member = 0; member < count && mine; ++member) {
            const auto* bytes = members + std::size_t{member} * 16;
            const bool row = std::any_of(station.rows.begin(), station.rows.end(), [&](const auto& candidate) { return std::memcmp(candidate.data(), bytes, 16) == 0; });
            const bool is_carrier = std::memcmp(station.carrier.data(), bytes, 16) == 0;
            rows += row;
            carried = carried || is_carrier;
            mine = row || is_carrier;
        }
        if (!mine || !rows || rows <= best_rows) continue;
        best = static_cast<int>(i);
        best_rows = rows;
        add = !carried;
        std::memcpy(carrier, station.carrier.data(), 16);
    }
    return best;
}
void song_begun(std::uint32_t channel, std::uintptr_t metadata) noexcept {
    std::lock_guard lock(mutex);
    if (channel < channel_song.size()) channel_song[channel] = metadata;
}
void song_started(std::uintptr_t metadata) noexcept {
    if (!active()) return;
    std::lock_guard lock(mutex);
    show_locked(metadata);
}
int row_of(int station, const unsigned char* song) noexcept {
    std::lock_guard lock(mutex);
    if (station < 0 || static_cast<std::size_t>(station) >= stations.size()) return -1;
    const auto& rows = stations[static_cast<std::size_t>(station)].rows;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (std::memcmp(rows[i].data(), song, 16) == 0) return static_cast<int>(i);
    return -1;
}
void row_started(const unsigned char* row) noexcept {
    std::lock_guard lock(mutex);
    std::memcpy(picked_row.data(), row, 16);
    row_picked.store(true, std::memory_order_release);
}

void bound(std::uint32_t channel, bool music_channel, std::uint32_t playlist, std::uint32_t members, int station) noexcept {
    binds_seen.fetch_add(1, std::memory_order_relaxed);
    // (Each change of a channel's playlist to a station, into the log.)
    if (const auto signature = (std::uint64_t{channel} << 40) ^ (std::uint64_t{playlist} << 8) ^ members ^ (static_cast<std::uint64_t>(station + 1) << 52);
        last_said.exchange(signature, std::memory_order_relaxed) != signature && station >= 0) {
        try {
            logging::log(logging::Level::debug, logging::Channel::music, "Spotify station: the game bound channel {} to playlist {}, station {}{}.",
                channel, playlist, station, music_channel ? "" : " (not yet a music channel)");
        } catch (...) {}
    }
    const auto before = station_channel.load(std::memory_order_relaxed);
    if (station < 0) {
        plain_channel.store(channel, std::memory_order_relaxed);
        plain_at.store(GetTickCount64(), std::memory_order_relaxed);
    }
    if (station >= 0 || before == channel) player_queue.store(0, std::memory_order_relaxed);
    if (station >= 0) {
        station_index.store(station, std::memory_order_relaxed);
        station_channel.store(channel, std::memory_order_relaxed);
    } else if (before == channel) {
        station_channel.store(no_channel, std::memory_order_relaxed);
        station_index.store(-1, std::memory_order_relaxed);
        left_at_store();
    }
}

// The map is about to change, and the game's songs with it.
//
// A song is in the game's registry under its artist and title, and is taken out of it by them
// when its bundle is unloaded. One that reads as Spotify's is looked for under Spotify's
// words and not found, so its own entry stays, pointing at an asset that is freed; the game
// then reads that asset as it registers the song again for the new map (Skate.exe+0x3dd1db0
// from 0x43e260, on joining a session with a station on, 2026-10-13). So every song is given
// its own words back first, and none is remembered: their assets are not the next map's.
void before_level_transition() noexcept {
    try {
        std::lock_guard lock(mutex);
        restore_locked();
        again.clear();
        channel_song.fill(0);
        for (auto& station : stations) station.metadata = 0;
    } catch (...) {}
}
void restore_presentation() noexcept {
    std::lock_guard lock(mutex);
    // (While a station is on they are only put back for a moment, for the catalog to be read.)
    if (active())
        for (const auto& entry : shown)
            if (again.size() < 64 && std::find(again.begin(), again.end(), entry.metadata) == again.end()) again.push_back(entry.metadata);
    restore_locked();
}
void song_chosen(std::uintptr_t queue, bool asked) noexcept {
    // (A queue is the game's for as long as its channel stays on a playlist: a new one is made
    // when the channel is bound again, and the old one's memory goes to something else. Written
    // into after that, it took the game down, 2026-10-12.)
    if (std::uintptr_t table{}; memory::peek(queue, table)) {
        player_queue_table.store(table, std::memory_order_relaxed);
        player_queue.store(queue, std::memory_order_relaxed);
    }
    if (!asked) return;
    // (Its own asking is answered within a frame or two; a press of the player's is not it.)
    if (const auto mine = self_asked.exchange(0, std::memory_order_relaxed); mine && GetTickCount64() - mine < 3000) return;
    if (std::uint8_t start{}; memory::peek(queue + queue_start, start)) asked_flag.store(start, std::memory_order_relaxed);
    skip.store(true, std::memory_order_relaxed);
}
void refresh_now_playing() noexcept {
    const auto queue = player_queue.load(std::memory_order_relaxed);
    if (!queue || !active()) return;
    if (std::uintptr_t table{}; !memory::peek(queue, table) || table != player_queue_table.load(std::memory_order_relaxed)) {
        player_queue.store(0, std::memory_order_relaxed);
        return;
    }
    std::uint8_t asked{};
    if (!memory::peek(queue + queue_asked, asked) || asked) return; // (it is choosing one as it is)
    self_asked.store(GetTickCount64(), std::memory_order_relaxed);
    __try {
        *reinterpret_cast<std::uint8_t*>(queue + queue_start) = asked_flag.load(std::memory_order_relaxed);
        *reinterpret_cast<std::uint8_t*>(queue + queue_asked) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        self_asked.store(0, std::memory_order_relaxed);
    }
}
bool active() noexcept { return station_channel.load(std::memory_order_relaxed) != no_channel; }
std::atomic<bool> screen_open{};
void set_music_screen(bool open) noexcept { screen_open.store(open, std::memory_order_relaxed); }
bool music_screen() noexcept { return screen_open.load(std::memory_order_relaxed); }
std::atomic<std::uint64_t> left_at{}, grabbed_at{};
void next_pressed() noexcept {
    if (active()) skip.store(true, std::memory_order_relaxed);
}
void grabbed() noexcept { grabbed_at.store(GetTickCount64(), std::memory_order_relaxed); }
namespace {
void left_at_store() noexcept { left_at.store(GetTickCount64(), std::memory_order_relaxed); }
}
bool recent() noexcept { return active() || GetTickCount64() - left_at.load(std::memory_order_relaxed) < 15000; }
std::uint32_t channel() noexcept { return station_channel.load(std::memory_order_relaxed); }
int current() noexcept { return active() ? station_index.load(std::memory_order_relaxed) : -1; }

void tick() noexcept {
    static int was{-1};
    static std::uint64_t said{}, entered{}, picked_at{}, tube_start_at{};
    static std::size_t tube_start{};
    const int now = active() ? station_index.load(std::memory_order_relaxed) : -1;
    if (now != was) {
        const bool first = was < 0;
        was = now;
        // Entered, or gone from one to another: the outside player is watched and started, on
        // the playlist when the station has one. Left: it is paused, and the watcher stays (it
        // costs a look a second) so the next entry is at once.
        if (now >= 0) {
            external_player::start();
            std::string playlist;
            {
                std::lock_guard lock(mutex);
                if (static_cast<std::size_t>(now) < stations.size()) playlist = stations[static_cast<std::size_t>(now)].playlist;
            }
            // The External Audio station (external_audio.h) is another application's sound and
            // session; the others are Spotify's. What was playing is paused as one gives way to
            // the other.
            // (And a YouTube Music station, youtube_music.h, is a playlist of the player's own.)
            const bool others = playlist == external_audio::station, tube = playlist.starts_with(youtube_music::station_prefix);
            if (others != external_audio::on() || tube != youtube_music::on()) {
                external_player::send(external_player::Command::pause);
                external_audio::set_on(others);
                youtube_music::set_on(tube);
            }
            external_player::use_app(others ? external_audio::app() : std::string());
            if (tube) {
                tube_start = youtube_list(playlist);
                tube_start_at = GetTickCount64();
            } else if (others) external_player::send(external_player::Command::play);
            // (On the game's own device, Spotify is had to play there, wherever it was playing.)
            else if (!playlist.empty() && GetTickCount64() - picked_at > 1500) {
                if (spotify_device::connected()) spotify_device::activate();
                spotify_web::play(playlist);
            } else if (spotify_device::connected()) spotify_device::start_radio();
            else if (first) external_player::send(external_player::Command::play);
            entered = GetTickCount64();
            try {
                logging::log(logging::Level::debug, logging::Channel::music, "Spotify station: station {} is on{}.", now,
                    playlist.empty() ? std::string() : " (Spotify playlist " + playlist + ")");
            } catch (...) {}
        } else {
            external_player::send(external_player::Command::pause);
        }
    }
    // A row the player picked: Spotify is started at its track, and the game is had to go on
    // to the station's own song (the row's is a copy of some song of the game's, which would
    // otherwise play out under its own name).
    if (row_picked.exchange(false, std::memory_order_acquire)) {
        // (The row is looked for in the station that is on first, then in the others: the game
        // may ask for it a moment before it binds the playlist.)
        std::string playlist, track;
        int found{-1};
        {
            std::lock_guard lock(mutex);
            const auto look = [&](const Station& station) {
                for (std::size_t i = 0; i < station.rows.size() && i < station.tracks.size(); ++i)
                    if (station.rows[i] == picked_row && !station.tracks[i].empty()) {
                        playlist = station.playlist;
                        track = station.tracks[i];
                        return true;
                    }
                return false;
            };
            if (now >= 0 && static_cast<std::size_t>(now) < stations.size() && look(stations[static_cast<std::size_t>(now)])) found = now;
            else
                for (std::size_t i = 0; i < stations.size() && found < 0; ++i)
                    if (look(stations[i])) found = static_cast<int>(i);
        }
        // A playlist added since the game began has no playlist of the game's behind its tile
        // (the game makes those as it starts), so the game binds the channel to none: the row
        // picked says which station is meant, and it is taken to be on, on the channel the game
        // has just bound. Whatever song of the game's plays there is the one whose sound is
        // replaced. (Seen 2026-10-12: a playlist added from the setup page played the game's
        // own songs.)
        if (found >= 0 && found != now && music_screen() && GetTickCount64() - plain_at.load(std::memory_order_relaxed) < 4000) {
            if (const auto channel = plain_channel.load(std::memory_order_relaxed); channel != no_channel) {
                station_index.store(found, std::memory_order_relaxed);
                station_channel.store(channel, std::memory_order_relaxed);
                try {
                    logging::log(logging::Level::debug, logging::Channel::music, "Spotify station: station {} is taken to be on, on channel {} (its tile has no playlist of the game's behind it yet).", found, channel);
                } catch (...) {}
            }
        }
        // The song Spotify is playing now is asked for when the player grabs the music of a
        // speaker in the world: its song reads as Spotify's, and the game finds the row of that
        // name. Spotify is playing it already, and is left to.
        bool current{};
        try {
            std::array<unsigned char, 16> row{};
            {
                std::lock_guard lock(mutex);
                row = picked_row;
            }
            if (const auto playing = external_player::now(); playing.available && now >= 0) current = spotify_rows::id_of(row.data()) == playing.artist + " - " + playing.title;
        } catch (...) {}
        // A row is picked in the music screen, which is a menu of the game's. A song asked for
        // with no menu up is the world's doing (the player grabbed a speaker's music, whose song
        // reads as one Spotify played): Spotify is left where it is.
        if (!music_screen()) track = "asked for with no menu up: Spotify is left as it is";
        else if (current) track = "the song Spotify is playing: left to play";
        else if (playlist.starts_with(youtube_music::station_prefix) && track.starts_with(youtube_music::track_prefix))
            youtube_music::play(youtube_list(playlist), track.substr(std::strlen(youtube_music::track_prefix)));
        else if (!playlist.empty() && !track.empty()) spotify_web::play(playlist, track);
        picked_at = GetTickCount64(); // (with a track or not: the game's next that follows is not Spotify's)
        try {
            logging::log(logging::Level::debug, logging::Channel::music, "Spotify station: a row was picked{}.", track.empty() ? std::string(" (not one with a track)") : " (" + track + ")");
        } catch (...) {}
    }
    // A YouTube Music station that has come on starts its playlist a moment after, unless a row
    // of it was picked meanwhile (which is how it usually comes on, and is what is played).
    if (tube_start_at && GetTickCount64() - tube_start_at > 400) {
        if (now >= 0 && youtube_music::on() && GetTickCount64() - picked_at > 1500) youtube_music::play(tube_start);
        tube_start_at = 0;
    }
    if (now < 0) {
        skip.store(false, std::memory_order_relaxed);
        player_queue.store(0, std::memory_order_relaxed);
        restore_presentation();
        {
            std::lock_guard lock(mutex);
            again.clear();
        }
        return;
    }
    // The game's "next" is Spotify's (once for a press: the game may ask more than once).
    static std::uint64_t skipped{};
    if (const auto tick_now = GetTickCount64(); skip.exchange(false, std::memory_order_relaxed) && tick_now - skipped > 1200 &&
        tick_now - entered > 2000 && tick_now - picked_at > 2500 && tick_now - grabbed_at.load(std::memory_order_relaxed) > 2500) { // (the game asks the same way for a station's first song)
        skipped = tick_now;
        // (The game asks the same way when its music is taken off pause, which is not a next:
        // Spotify is paused then, by music_inject, and is only played again.)
        bool playing{true};
        try {
            if (const auto track = external_player::now(); track.available) playing = track.playing;
        } catch (...) {}
        if (playing) external_player::send(external_player::Command::next);
        try {
            logging::log(logging::Level::debug, logging::Channel::music, "Spotify station: the game asked for its next song; {}.", playing ? "Spotify's next" : "Spotify is paused, so it is left where it is");
        } catch (...) {}
    }
    try {
        const auto track = external_player::now();
        if (!track.available) return;
        // Its cover is made ready for the music screen as Windows comes to have it.
        static std::uint64_t cover{~0ull};
        if (!track.artwork.empty() && track.artwork_revision != cover) {
            cover = track.artwork_revision;
            spotify_cover::want(track.artwork, track.artwork_revision);
        }
        bool unshown{};
        {
            std::lock_guard lock(mutex);
            unshown = static_cast<std::size_t>(now) >= stations.size() || !is_shown_locked(stations[static_cast<std::size_t>(now)].metadata);
        }
        const auto revision = external_player::revision();
        if (unshown || revision != said) show(now, track.artist, track.title);
        if (revision != said) {
            said = revision;
            logging::log(logging::Level::debug, logging::Channel::music, "Spotify station: {} {}: {} - {}{}.", track.spotify ? "Spotify" : track.app,
                track.playing ? "is playing" : "is paused on", track.artist, track.title, track.artwork.empty() ? "" : " (with a cover)");
        }
    } catch (...) {}
}

std::string status() {
    const auto channel = station_channel.load(std::memory_order_relaxed);
    const auto track = external_player::now();
    return std::format("Spotify station: {} station(s); {}; {} playlist binding(s) seen. Outside player: {}.", station_count.load(std::memory_order_relaxed),
        channel == no_channel ? std::string("none selected") : std::format("station {} selected on channel {}", station_index.load(std::memory_order_relaxed), channel),
        binds_seen.load(std::memory_order_relaxed),
        !track.available ? std::string("none seen (it is watched once a station has been selected)")
                         : (track.spotify ? std::string("Spotify") : track.app) + (track.playing ? " playing " : " paused on ") + track.artist + " - " + track.title);
}
} // namespace dingosdk::spotify_station
