#include "spotify_rows.h"
#include "spotify_station.h"
#include "music_inject.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/local_music.h"
#include "Engine/Game/Build/addresses.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <random>

namespace dingosdk::spotify_rows {
namespace {
namespace lm = addr::local_music;
// An asset, as much of it as is copied: the 16 bytes it is known by, then the asset.
constexpr std::size_t header = 0x10, copied = 0x200;
constexpr std::size_t asset_flags = 0x14, graph_metadata = 0x20, graph_tags = 0x28, metadata_artist = 0x18, metadata_title = 0x20;
constexpr std::uint16_t has_identity = 0x100, never_freed = 0x2000;

struct Row {
    std::unique_ptr<unsigned char[]> graph, metadata;
    std::string artist, title;
};
std::mutex mutex;
std::map<std::string, Row, std::less<>> rows; // by id; never freed: the game holds them
std::string said_problem;

bool matches(std::uintptr_t base, const game::build::Fingerprint& contract) noexcept {
    std::array<unsigned char, 32> bytes{};
    return memory::peek(base + contract.rva, bytes) && bytes == contract.bytes;
}
void problem(const std::string& text) {
    if (said_problem == text) return;
    said_problem = text;
    try {
        logging::log(logging::Level::warning, logging::Channel::music, "Spotify rows: {}.", text);
    } catch (...) {}
}
bool is_type(std::uintptr_t base, std::uintptr_t asset, std::uintptr_t table) noexcept {
    std::uintptr_t held{};
    return asset && memory::peek(asset, held) && held == base + table;
}
// Any registered song's asset, to copy.
std::uintptr_t a_song(std::uintptr_t base, std::uintptr_t manager) noexcept {
    std::uintptr_t buckets{}, sentinel{};
    std::uint32_t bucket_count{};
    if (!memory::peek(manager + 0x78, buckets) || !buckets || !memory::peek(manager + 0x80, bucket_count) || !bucket_count || bucket_count > 16384 ||
        !memory::peek(buckets + bucket_count * 8ull, sentinel))
        return 0;
    for (std::uint32_t bucket = 0; bucket < bucket_count; ++bucket) {
        std::uintptr_t node{}, graph{}, metadata{};
        std::uint16_t flags{};
        if (!memory::peek(buckets + bucket * 8ull, node) || !node || node == sentinel || !memory::peek(node + 8, graph)) continue;
        graph &= ~std::uintptr_t{4};
        if (!is_type(base, graph, lm::graph_asset_vtable) || !memory::peek(graph + graph_metadata, metadata) || !memory::peek(graph + asset_flags, flags) || !(flags & has_identity)) continue;
        metadata &= ~std::uintptr_t{4};
        if (is_type(base, metadata, lm::metadata_vtable)) return graph;
    }
    return 0;
}
// A copy of an asset with its 16 bytes before it; `at` is where the asset begins in it.
std::unique_ptr<unsigned char[]> copy_of(std::uintptr_t asset, bool with_identity) {
    auto copy = std::make_unique<unsigned char[]>(header + copied);
    std::memset(copy.get(), 0, header + copied);
    // (As much as can be read: an asset near the end of its memory is shorter than this.)
    for (std::size_t size = copied; size >= 0x40; size /= 2)
        if (memory::peek_bytes(asset, copy.get() + header, size)) {
            if (with_identity) memory::peek_bytes(asset - header, copy.get(), header);
            return copy;
        }
    return nullptr;
}
bool registered(std::uintptr_t base, std::uintptr_t manager, const std::string& id) noexcept {
    const char* text = id.c_str();
    __try {
        return reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, const char**)>(base + lm::song_lookup_contract.rva)(manager, &text) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool register_song(std::uintptr_t base, std::uintptr_t manager, std::uintptr_t graph) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(base + lm::song_register_contract.rva)(manager, graph);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The game's routine that begins to play a song's asset.
using PlaySong = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
PlaySong play_song_original{};
std::uintptr_t play_song_hook(std::uintptr_t player, std::uintptr_t asset, std::uint32_t channel, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) {
    try {
        const auto graph = asset & ~std::uintptr_t{4};
        bool ours{};
        {
            std::lock_guard lock(mutex);
            for (const auto& [id, row] : rows)
                if (reinterpret_cast<std::uintptr_t>(row.graph.get() + header) == graph) {
                    spotify_station::row_started(row.graph.get());
                    ours = true;
                    break;
                }
        }
        // (Each song the game begins while a station is on, into the log while this is worked out.)
        if (static std::atomic<int> said{}; logging::enabled(logging::Level::debug) && said.fetch_add(1, std::memory_order_relaxed) < 600)
        {
            // (Which song, and from where in the game it was begun.)
            std::uintptr_t metadata{}, artist{}, title{};
            char artist_text[96]{}, title_text[96]{};
            if (graph && memory::peek(graph + graph_metadata, metadata) && (metadata &= ~std::uintptr_t{4}) != 0 && memory::peek(metadata + metadata_artist, artist) &&
                memory::peek(metadata + metadata_title, title)) {
                memory::peek_cstring(artist, artist_text, sizeof(artist_text));
                memory::peek_cstring(title, title_text, sizeof(title_text));
            }
            void* frames[6]{};
            const auto count = CaptureStackBackTrace(1, 6, frames, nullptr);
            const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::string callers;
            for (unsigned i = 0; i < count; ++i) callers += std::format(" +0x{:x}", reinterpret_cast<std::uintptr_t>(frames[i]) - image);
            logging::log(logging::Level::debug, logging::Channel::music, "Spotify rows: the game began {} (\"{} - {}\") on channel {}, from{}.", ours ? "a row of ours" : "a song of its own",
                artist_text, title_text, channel, callers);
        }
        // (A row of ours keeps its own artist and title: they are what its row says.)
        // The station's own song beginning: its sound file is known for it ("music through 3").
        if (!ours && spotify_station::active() && channel == spotify_station::channel()) music_inject::station_song_begins();
        // Any song of the game's begun while a station is on has Spotify's sound (a speaker's in
        // the world), so it reads as Spotify's. (A row of ours keeps what its row says.)
        if (std::uintptr_t metadata{}; !ours && graph && memory::peek(graph + graph_metadata, metadata) && (metadata &= ~std::uintptr_t{4}) != 0) {
            spotify_station::song_begun(channel, metadata);
            spotify_station::song_started(metadata);
        }
    } catch (...) {}
    return play_song_original(player, asset, channel, a, b, c, d);
}
// (What the music screen does with the game's playlists, into the log while it is being
// worked out how a picked row is told to the game: each song put in or taken out of one, and
// each one emptied or made. A song that is a row of ours is named.)
std::atomic<int> traced{};
std::string song_text(const unsigned char* song) {
    if (!song) return "nothing";
    std::lock_guard lock(mutex);
    for (const auto& [id, row] : rows)
        if (std::memcmp(row.graph.get(), song, header) == 0) return "\"" + id + "\"";
    return "a song of the game's";
}
void trace(const char* what, std::uint32_t playlist, const unsigned char* song) noexcept {
    // (No more than thirty a second, so that the many of the music screen being built do not
    // use up what is kept for a row being picked.)
    static std::atomic<std::uint64_t> second{};
    static std::atomic<int> in_second{};
    if (const auto now = GetTickCount64() / 1000; second.exchange(now, std::memory_order_relaxed) != now) in_second.store(0, std::memory_order_relaxed);
    if (in_second.fetch_add(1, std::memory_order_relaxed) >= 30 || traced.fetch_add(1, std::memory_order_relaxed) >= 1500) return;
    try {
        logging::log(logging::Level::debug, logging::Channel::music, "Playlist {}: {}{}.", playlist, what, song ? " " + song_text(song) : std::string());
    } catch (...) {}
}
using PlaylistSong = void* (*)(std::uintptr_t, std::uint32_t*, const unsigned char*);
using PlaylistOnly = void* (*)(std::uintptr_t, std::uint32_t*);
PlaylistSong add_original{}, remove_original{};
PlaylistOnly clear_original{}, create_original{};
void* add_hook(std::uintptr_t manager, std::uint32_t* playlist, const unsigned char* song) {
    if (playlist) trace("put in", *playlist, song);
    return add_original(manager, playlist, song);
}
void* remove_hook(std::uintptr_t manager, std::uint32_t* playlist, const unsigned char* song) {
    if (playlist) trace("taken out", *playlist, song);
    return remove_original(manager, playlist, song);
}
void* clear_hook(std::uintptr_t manager, std::uint32_t* playlist) {
    if (playlist) trace("emptied", *playlist, nullptr);
    return clear_original(manager, playlist);
}
void* create_hook(std::uintptr_t manager, std::uint32_t* playlist) {
    const auto result = create_original(manager, playlist);
    if (playlist) trace("made", *playlist, nullptr);
    return result;
}
// The game being asked for a song the player picked. A row of ours is no song the queue can
// play, so the game does nothing with it: the station is told, and starts Spotify at it.
using RequestSong = void (*)(std::uintptr_t, std::uintptr_t);
RequestSong request_original{};
void request_hook(std::uintptr_t queue, std::uintptr_t asset) {
    try {
        const auto graph = asset & ~std::uintptr_t{4};
        bool ours{};
        {
            std::lock_guard lock(mutex);
            for (const auto& [id, row] : rows)
                if (reinterpret_cast<std::uintptr_t>(row.graph.get() + header) == graph) {
                    spotify_station::row_started(row.graph.get());
                    ours = true;
                    break;
                }
        }
        // (Each song asked for, into the log while grabbing a speaker's music is worked out.)
        if (static std::atomic<int> said{}; logging::enabled(logging::Level::debug) && spotify_station::recent() && said.fetch_add(1, std::memory_order_relaxed) < 600) {
            std::uintptr_t metadata{}, artist{}, title{};
            char artist_text[96]{}, title_text[96]{};
            if (graph && memory::peek(graph + graph_metadata, metadata) && (metadata &= ~std::uintptr_t{4}) != 0 && memory::peek(metadata + metadata_artist, artist) &&
                memory::peek(metadata + metadata_title, title)) {
                memory::peek_cstring(artist, artist_text, sizeof(artist_text));
                memory::peek_cstring(title, title_text, sizeof(title_text));
            }
            void* frames[6]{};
            const auto count = CaptureStackBackTrace(1, 6, frames, nullptr);
            const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::string callers;
            for (unsigned i = 0; i < count; ++i) callers += std::format(" +0x{:x}", reinterpret_cast<std::uintptr_t>(frames[i]) - image);
            logging::log(logging::Level::debug, logging::Channel::music, "Trace: queue 0x{:x} was asked for {} (\"{} - {}\"), from{}.", queue, ours ? "a row of ours" : "a song of the game's", artist_text, title_text, callers);
        }
    } catch (...) {}
    request_original(queue, asset);
}
template <class T> void watch(std::uintptr_t base, const game::build::Fingerprint& contract, T hook, T& original) noexcept {
    if (!matches(base, contract)) return;
    const auto target = reinterpret_cast<void*>(base + contract.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(&original)) != HookOk || hook_enable(target) != HookOk) original = nullptr;
}
} // namespace

std::string id_of(const unsigned char* row) {
    std::lock_guard lock(mutex);
    for (const auto& [id, made] : rows)
        if (std::memcmp(made.graph.get(), row, header) == 0) return id;
    return {};
}

bool install(std::uintptr_t base) noexcept {
    static bool done{};
    if (done || !base || !matches(base, lm::song_play_contract)) return false;
    done = true;
    // (The game's playlist routines are not watched: with them watched the song added to a
    // station's playlist did not stay in it, 2026-10-11. Only removals are, which change nothing.)
    watch(base, lm::song_request_contract, &request_hook, request_original);
    const auto target = reinterpret_cast<void*>(base + lm::song_play_contract.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&play_song_hook), reinterpret_cast<void**>(&play_song_original)) != HookOk || hook_enable(target) != HookOk) {
        play_song_original = nullptr;
        return false;
    }
    return true;
}

bool known(const std::string& artist, const std::string& title) {
    std::lock_guard lock(mutex);
    return rows.contains(artist + " - " + title);
}

std::string ensure(std::uintptr_t base, std::uintptr_t manager, const std::string& artist, const std::string& title) {
    if (!base || !manager || artist.empty() || title.empty()) return {};
    if (!matches(base, lm::song_register_contract) || !matches(base, lm::song_lookup_contract)) {
        problem("the game's routines for registering a song are not the ones this was written for");
        return {};
    }
    const auto id = artist + " - " + title;
    std::lock_guard lock(mutex);
    auto found = rows.find(id);
    if (found == rows.end()) {
        const auto original = a_song(base, manager);
        std::uintptr_t original_metadata{};
        if (!original || !memory::peek(original + graph_metadata, original_metadata)) {
            problem("no registered song was found to copy");
            return {};
        }
        Row row{copy_of(original, true), copy_of(original_metadata & ~std::uintptr_t{4}, true), artist, title};
        if (!row.graph || !row.metadata) {
            problem("a registered song could not be copied");
            return {};
        }
        // The copy: known by 16 bytes of its own, never freed by the game, in no playlist, and
        // with an artist and title of its own.
        std::random_device random;
        for (std::size_t i = 0; i < header; ++i) row.graph[i] = static_cast<unsigned char>(random());
        for (std::size_t i = 0; i < header; ++i) row.metadata[i] = static_cast<unsigned char>(random());
        const auto set = [](unsigned char* asset, std::size_t at, std::uintptr_t value) { std::memcpy(asset + at, &value, sizeof(value)); };
        for (auto* asset : {row.graph.get() + header, row.metadata.get() + header}) {
            std::uint16_t flags{};
            std::memcpy(&flags, asset + asset_flags, sizeof(flags));
            flags |= never_freed;
            std::memcpy(asset + asset_flags, &flags, sizeof(flags));
        }
        found = rows.emplace(id, std::move(row)).first;
        auto& made = found->second;
        set(made.metadata.get() + header, metadata_artist, reinterpret_cast<std::uintptr_t>(made.artist.c_str()));
        set(made.metadata.get() + header, metadata_title, reinterpret_cast<std::uintptr_t>(made.title.c_str()));
        set(made.graph.get() + header, graph_metadata, reinterpret_cast<std::uintptr_t>(made.metadata.get() + header));
        set(made.graph.get() + header, graph_tags, 0);
    }
    if (registered(base, manager, id)) return id;
    if (!register_song(base, manager, reinterpret_cast<std::uintptr_t>(found->second.graph.get() + header)) || !registered(base, manager, id)) {
        problem("the game did not take the song \"" + id + "\"");
        return {};
    }
    try {
        logging::log(logging::Level::debug, logging::Channel::music, "Spotify rows: registered \"{}\".", id);
    } catch (...) {}
    return id;
}

bool add_member(std::uintptr_t base, std::uintptr_t manager, std::uint32_t playlist, const unsigned char* song) noexcept {
    if (!base || !manager || !matches(base, lm::playlist_add_contract)) return false;
    __try {
        (add_original ? add_original : reinterpret_cast<PlaylistSong>(base + lm::playlist_add_contract.rva))(manager, &playlist, song);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace dingosdk::spotify_rows
