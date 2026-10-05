#include "Engine/Core/Log/logging.h"
#include "Extension/Customization/local_cosmetic_catalog.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Customization/local_player_card_runtime.h"
#include "local_music_assets.h"
#include "local_music_safety.h"
#include "local_music_ui.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_music.h"
#include "Engine/Game/World/location_travel.h"
#include "Extension/Profile/runtime_internal.h"

namespace dingosdk::profile_runtime {
namespace {
std::string music_artwork_url(std::string_view value) {
    // Only URLs produced by the local artwork server bypass CDN resolution.
    if (value.starts_with("http://127.0.0.1:")) return std::string(value);
    return travel_artwork_url(value);
}
}
// Runtime-only music UI hydration: read_music_catalog copies actual registered

// MusicGraphAsset metadata/TagRefs. No generated catalog, guessed memberships,

// artwork, audio-residency gate, or preference persistence. Bounds: 1024 songs,

// 256 authored groups, 8192 membership edges. Include after assets/news/model helpers.

void music_release_weak(std::uintptr_t owner) noexcept {
    if (owner && InterlockedDecrement(reinterpret_cast<volatile LONG*>(owner + 12)) == 0) {
        const auto table = *reinterpret_cast<const std::uintptr_t**>(owner);
        reinterpret_cast<void (*)(void*)>(table[2])(reinterpret_cast<void*>(owner));
    }
}

MusicUiRuntime& music_ui_runtime() { static auto* r = new MusicUiRuntime; return *r; }

bool initialize_music_functions(std::uintptr_t base) {
    namespace music = addr::local_music;
    for (const auto& fp : music::music_ui_contracts) {
        std::array<unsigned char, 32> actual{};
        if (!read(base + fp.rva, actual) || actual != fp.bytes) return false;
    }
    std::array<std::uintptr_t, 4> playlist{}, song{};
    const auto rebased = [base](const std::array<std::uintptr_t, 4>& slots) {
        return std::array<std::uintptr_t, 4>{base + slots[0], base + slots[1], base + slots[2], base + slots[3]};
    };
    if (!read(base + music::playlist_control_vtable, playlist) || !read(base + music::song_control_vtable, song) ||
        playlist != rebased(music::playlist_control_slots) ||
        song != rebased(music::song_control_slots)) return false;
    // Guard the native playlist walk before any mod song can reach it. A failed
    // install leaves music usable; the crash guard is a safety net, not a gate.
    if (!install_playlist_lookup_safety(base))
        logging::event(logging::Channel::music, "{\"event\":\"music_playlist_lookup_safety_failed\"}");
    auto& f = music_ui_runtime().functions;
    f.construct_playlist = reinterpret_cast<decltype(f.construct_playlist)>(base + music::construct_playlist);
    f.construct_song = reinterpret_cast<decltype(f.construct_song)>(base + music::construct_song);
    f.playlists = reinterpret_cast<decltype(f.playlists)>(base + music::publish_playlists);
    f.songs = reinterpret_cast<decltype(f.songs)>(base + music::publish_songs);
    f.insert = reinterpret_cast<decltype(f.insert)>(base + music::context_insert);
    f.complete = reinterpret_cast<decltype(f.complete)>(base + music::complete_delegate);
    f.allocator = {base + addr::engine::allocator_adapter_vtable, 0, 8};
    return true; // Root installs only the initialize hook after these contracts pass.
}

bool music_ui_identity(const MusicUiPending& pending) {
    std::uintptr_t manager{}, model{}, owner{}, vtable{}; std::int32_t strong{};
    const auto base = local_runtime().base;
    return pending.manager && pending.model && pending.owner &&
        read(base + addr::local_music::ui_manager, manager) && manager == pending.manager &&
        read(manager, vtable) && vtable == base + addr::local_music::ui_manager_vtable &&
        read(manager + 0x80, model) && model == pending.model &&
        read(manager + 0x58, owner) && owner == pending.owner &&
        read(owner + 8, strong) && strong > 0 && strong < 0x1000000;
}

bool music_ui_current(const MusicUiPending& pending) {
    return pending.generation == music_ui_runtime().generation && music_ui_identity(pending);
}

CosmeticShared music_ui_lease(const MusicUiPending& pending) {
    if (!music_ui_identity(pending)) return {};
    auto* counter = reinterpret_cast<volatile LONG*>(pending.owner + 8);
    LONG observed = InterlockedCompareExchange(counter, 0, 0);
    for (unsigned i = 0; i < 32 && observed > 0 && observed < 0x1000000; ++i) {
        const auto previous = InterlockedCompareExchange(counter, observed + 1, observed);
        if (previous == observed) {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(pending.owner + 12));
            return {reinterpret_cast<void*>(pending.manager), reinterpret_cast<void*>(pending.owner)};
        }
        observed = previous;
    }
    return {};
}

bool music_ui_text(std::string_view value, std::size_t limit ) {
    return !value.empty() && value.size() <= limit &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

bool music_ui_catalog_valid(const MusicCatalog& catalog, const std::string& favorites) {
    if (catalog.songs.empty() || catalog.songs.size() > 1024 || catalog.playlists.empty() || catalog.playlists.size() > 256) return false;
    std::set<std::string> songs, playlists;
    std::set<std::pair<std::string, std::string>> declared, grouped;
    for (const auto& song : catalog.songs) {
        if (!music_ui_text(song.id) || !music_ui_text(song.artist, 512) || !music_ui_text(song.title, 512) ||
            song.id != song.artist + " - " + song.title || !songs.insert(song.id).second) return false;
        for (const auto& playlist : song.playlists)
            if (!music_ui_text(playlist) || !declared.emplace(playlist, song.id).second || declared.size() > 8192) return false;
    }
    for (const auto& playlist : catalog.playlists) {
        if (!music_ui_text(playlist.id) || playlist.id == favorites || !playlists.insert(playlist.id).second) return false;
        for (const auto& id : playlist.songs)
            if (!songs.contains(id) || !grouped.emplace(playlist.id, id).second || grouped.size() > 8192) return false;
    }
    return declared == grouped;
}

std::string music_ui_wire(std::string_view id, std::string_view artist, std::string_view title,
    const std::vector<std::string>* members, std::string_view name, std::string_view artwork) {
    std::string body, presentation, framed;
    cosmetic_wire_string(body, 1, id);
    if (members) {
        for (const auto& song : *members) cosmetic_wire_string(body, 2, song);
        // A local shelf choice for real authored groups, NOT recovered AMP classification.
        cosmetic_wire_number(body, 3, 1);
        // The catalogue's display name when known; the raw id otherwise.
        cosmetic_wire_string(presentation, 10, name.empty() ? id : name);
        // Artwork: the catalogue's cdn:/ id, resolved to the CDN rendition.
        if (const auto url = music_artwork_url(artwork); !url.empty())
            cosmetic_wire_string(presentation, 11, url);
        cosmetic_wire_number(presentation, 12, 0);
    } else {
        cosmetic_wire_string(presentation, 10, artist);
        cosmetic_wire_string(presentation, 11, title);
        // Cover art: the content cache song record's cdn:/ id (its field 10.12), resolved like a playlist's.
        if (const auto url = music_artwork_url(artwork); !url.empty())
            cosmetic_wire_string(presentation, 12, url);
    }
    cosmetic_wire_string(body, 10, presentation);
    cosmetic_varint(framed, body.size()); framed += body;
    return framed;
}

CosmeticShared music_ui_message(const std::string& wire, bool playlist) {
    auto& f = music_ui_runtime().functions; auto& native = cosmetic_catalog_functions();
    const auto base = local_runtime().base;
    const std::size_t size = playlist ? 0xf0 : 0xc0;
    const std::uintptr_t control = base +
        (playlist ? addr::local_music::playlist_control_vtable : addr::local_music::song_control_vtable);
    std::uintptr_t allocator{};
    auto* block = static_cast<std::byte*>(native.allocate(&allocator, size, 0));
    if (!block) throw std::bad_alloc();
    std::memset(block, 0, size); std::memcpy(block, &control, 8);
    const std::uint32_t one = 1;
    std::memcpy(block + 8, &one, 4); std::memcpy(block + 12, &one, 4);
    std::memcpy(block + size - 8, &allocator, 8);
    const auto adapter = reinterpret_cast<std::uintptr_t>(f.allocator.data());
    const std::array<std::uintptr_t, 3> context{adapter, adapter, 0};
    (playlist ? f.construct_playlist : f.construct_song)(block + 16, context.data());
    CosmeticSharedGuard result{{block + 16, block}};
    std::array<std::uintptr_t, 4> reader{reinterpret_cast<std::uintptr_t>(wire.data()), wire.size(), 0, 0};
    if (!native.decode(reader.data(), result.value.body) || reader[2] != wire.size())
        throw std::runtime_error("Native music DTO decode failed");
    const auto value = result.value; result.value = {}; return value;
}

bool music_ui_has_context(std::uintptr_t map, const std::string& id) {
    std::uintptr_t buckets{}, node{}, sentinel{}; std::uint32_t count{}, capacity{};
    if (!read(map, buckets) || !buckets || !read(map + 8, capacity) || !capacity || capacity > 4096 ||
        !read(map + 12, count) || count > 2048 || !read(buckets + capacity * 8ULL, sentinel) ||
        !read(buckets + (cosmetic_hash(id) % capacity) * 8ULL, node)) return false;
    std::set<std::uintptr_t> seen;
    while (node && node != sentinel) {
        std::array<std::uintptr_t, 3> row{}; std::string name;
        if (seen.size() >= count || !seen.insert(node).second || !read(node, row) || !cosmetic_text(row[0], name)) return false;
        if (name == id) return row[1] != 0;
        node = row[2];
    }
    return false;
}

void music_ui_initialize_hook(std::uint64_t all, std::uint64_t hidden, std::uint64_t featured,
    std::uint64_t liked, std::uint64_t discovered, std::uint64_t favorites, std::uint64_t songs,
    const std::uint32_t* handle, const void* callback) {
    auto& s = local_runtime(); auto& ui = music_ui_runtime();
    {
        PreserveError preserve; std::lock_guard lock(s.native_mutex);
        // Even a forwarded initialization supersedes old local contexts.
        const auto generation = ++ui.generation;
        ui.pending.reset();
        if (s.active.load(std::memory_order_acquire))
        try {
            const auto thread = GetCurrentThreadId();
            auto pending = std::make_unique<MusicUiPending>();
            pending->contexts = {all, hidden, featured, liked, discovered, favorites, songs};
            std::uintptr_t owner{}, vtable{}; std::int32_t strong{};
            if ((!cosmetic_runtime().update_thread || cosmetic_runtime().update_thread == thread) && callback &&
                std::all_of(pending->contexts.begin(), pending->contexts.end(), [](auto c) { return c != 0; }) &&
                read(reinterpret_cast<std::uintptr_t>(handle), pending->favorites) && pending->favorites &&
                read(s.base + addr::local_music::ui_manager, pending->manager) && pending->manager &&
                read(pending->manager, vtable) && vtable == s.base + addr::local_music::ui_manager_vtable &&
                read(pending->manager + 0x80, pending->model) && pending->model &&
                read(pending->manager + 0x58, owner) && owner && read(owner + 8, strong) && strong > 0 && strong < 0x1000000) {
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(owner + 12)); pending->owner = owner;
                if (music_ui_identity(*pending)) {
                    game::native_data().values.copy_delegate(&pending->delegate, callback);
                    if (pending->delegate) {
                        if (generation != ui.generation) return;
                        pending->thread = thread; pending->generation = generation;
                        ui.pending = std::move(pending);
                        dingosdk::logging::event(dingosdk::logging::Channel::music, "{\"event\":\"native_music_ui_queued\"}"); return;
                    }
                }
            }
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::music, "{\"event\":\"native_music_ui_queue_failed\"}"); }
    }
    ui.functions.initialize(all, hidden, featured, liked, discovered, favorites, songs, handle, callback);
}

void update_music_catalog() {
    auto& s = local_runtime(); auto& ui = music_ui_runtime();
    if (!s.active.load(std::memory_order_acquire) || ui.updating || cosmetic_runtime().update_thread != GetCurrentThreadId()) return;
    PreserveError preserve; std::lock_guard lock(s.native_mutex);
    if (!ui.pending) return;
    if (ui.pending->thread != GetCurrentThreadId() || !music_ui_current(*ui.pending)) { ui.pending.reset(); return; }
    const auto now = GetTickCount64(); if (now < ui.pending->next_poll) return;
    ui.pending->next_poll = now + 250;
    auto pending = std::move(ui.pending);
    ui.updating = true;
    bool retry = true;
    struct PendingScope {
        MusicUiRuntime& ui; std::unique_ptr<MusicUiPending>& pending; bool& retry;
        ~PendingScope() {
            if (retry && !ui.pending && pending && pending->generation == ui.generation)
                ui.pending = std::move(pending);
            ui.updating = false;
        }
    } pending_scope{ui, pending, retry};
    try {
        CosmeticSharedGuard lease{music_ui_lease(*pending)};
        if (!lease.value.control) { retry = false; return; }
        MusicCatalog catalog;
        if (!read_music_catalog(catalog) || catalog.playlists.empty()) {
            if (!pending->waiting_logged) { dingosdk::logging::event(dingosdk::logging::Channel::music, "{\"event\":\"native_music_ui_waiting_assets\"}"); pending->waiting_logged = true; }
            return;
        }
        if (!music_ui_current(*pending)) { retry = false; return; }
        std::string favorite_id;
        if (!identifier(reinterpret_cast<const void*>(pending->manager + 8), favorite_id) ||
            !music_ui_catalog_valid(catalog, favorite_id)) throw std::runtime_error("Invalid runtime music catalog");
        std::uintptr_t arena{}, vtable{};
        if (!read(s.base + addr::engine::default_arena, arena) || !arena || !read(arena, vtable) || vtable < s.base || vtable >= s.base + supported_build::game_image_size) return;
        ui.functions.allocator[1] = arena;
        MusicUiMessages playlists, songs;
        playlists.items.reserve(catalog.playlists.size()); songs.items.reserve(catalog.songs.size());
        for (const auto& playlist : catalog.playlists) playlists.items.push_back(music_ui_message(music_ui_wire(playlist.id, {}, {}, &playlist.songs, playlist.name, playlist.artwork), true));
        for (const auto& song : catalog.songs) songs.items.push_back(music_ui_message(music_ui_wire(song.id, song.artist, song.title, nullptr, {}, song.artwork), false));
        auto& model = game::native_data().models; auto& field = game::native_data().models.field;
        if (!music_ui_current(*pending)) { retry = false; return; }
        {
            game::ModelWriteLock model_lock(pending->model);
            for (auto context : pending->contexts)
                if (!model.value(pending->model, context, 0, 0)) throw std::runtime_error("Invalid music model context");
        }
        if (!music_ui_current(*pending)) { retry = false; return; }
        const auto favorite = pending->contexts[5], manager = pending->manager, native_model = pending->model;
        const auto song_field = field(native_model, favorite, 2, 0xffffffffU, false);
        const auto id_field = field(native_model, favorite, 0, 0xffffffffU, false);
        const auto title_field = field(native_model, favorite, 3, 0xffffffffU, false);
        if (!song_field || !id_field || !title_field) throw std::runtime_error("Invalid native Favorites fields");
        if (!music_ui_current(*pending)) { retry = false; return; }
        retry = false; // Once native publication starts, never replay a partial generation.
        std::memcpy(reinterpret_cast<void*>(manager + 0xf8), pending->contexts.data(), sizeof(pending->contexts));
        std::memcpy(reinterpret_cast<void*>(manager + 0x1f8), &pending->favorites, 4);
        std::memcpy(reinterpret_cast<void*>(manager + 0x130), &song_field, 8);
        std::array<std::uintptr_t, 3> inserted{};
        ui.functions.insert(manager + 0xa8, inserted.data(), reinterpret_cast<const void*>(manager + 8), &favorite);
        model.publish(native_model, id_field, s.base + addr::engine::string_type, reinterpret_cast<const void*>(manager + 8));
        if (!music_ui_current(*pending)) return;
        model.publish(native_model, title_field, s.base + addr::engine::string_type, reinterpret_cast<const void*>(manager + 8));
        if (!music_ui_current(*pending)) return;
        playlists.publish(manager, ui.functions.playlists);
        if (!music_ui_current(*pending)) return;
        songs.publish(manager, ui.functions.songs);
        if (!music_ui_current(*pending)) return;
        std::uint32_t playlist_count{}, song_count{};
        if (!read(manager + 0x1b8, playlist_count) || playlist_count != catalog.playlists.size() ||
            !read(manager + 0x1e8, song_count) || song_count != catalog.songs.size() ||
            !music_ui_has_context(manager + 0xa8, favorite_id)) throw std::runtime_error("Native music publication count mismatch");
        for (const auto& playlist : catalog.playlists)
            if (!music_ui_has_context(manager + 0xa8, playlist.id)) throw std::runtime_error("Native playlist identity missing");
        for (const auto& song : catalog.songs)
            if (!music_ui_has_context(manager + 0xd0, song.id)) throw std::runtime_error("Native song identity missing");
        if (!music_ui_current(*pending)) return;
        ui.functions.complete(&pending->delegate);
        dingosdk::logging::event(dingosdk::logging::Channel::music, dingosdk::Json{{"event", "native_music_ui_complete"}, {"playlists", playlist_count}, {"songs", song_count}, {"source", "runtime_music_assets"}}.dump().c_str());
    } catch (...) { retry = false; dingosdk::logging::event(dingosdk::logging::Channel::music, "{\"event\":\"native_music_ui_publish_failed\"}"); }
}
}
