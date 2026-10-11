#include "local_music_playback.h"

#include "local_music_playback_policy.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/local_music.h"
#include "Engine/Game/Build/supported_build.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

namespace dingosdk::profile_runtime {
namespace {
using namespace dingosdk::game::build::v20260929;
using local_music::playback_mode_setter_contract;
using local_music::playback_select_next_contract;
using local_music::playback_channel_playlist_bind_contract;
using music_playback::CursorPlan;

std::atomic<std::uintptr_t> image_base{};
std::atomic<bool> shuffle_enabled{};
std::atomic<bool> playback_available{};
MusicSelectNext select_next{};
MusicChannelPlaylistBind bind_channel_playlist{};
std::array<std::atomic<std::uint64_t>, 256> last_logged_channel_bindings{};
// The channels the music player's own queues select for (apply_policy learns them: the game
// supplies the numbers, they are not written down here). Only those are ever given Favorites
// for a playlist ID of zero: on any other channel zero is left to mean what the game means.
std::array<std::atomic<bool>, 256> music_channels{};
using SetPlaybackMode = void (__fastcall *)(std::uintptr_t manager, std::uint32_t playlist_id,
                                           std::uint32_t mode);
SetPlaybackMode set_playback_mode{};
struct LastBinding {
    std::uintptr_t manager{};
    std::uintptr_t queue{};
    std::uintptr_t node{};
    std::uint32_t playlist_id{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t member_count{};
};
std::array<LastBinding, 256> last_binding_by_channel{}; // Protected by the binding-cache lock.
SRWLOCK binding_cache_lock = SRWLOCK_INIT;
std::atomic<std::uint32_t> logged_policy_events{};

void log_policy_event_once(std::uint32_t bit, const char* json) noexcept {
    if (!(logged_policy_events.fetch_or(bit, std::memory_order_relaxed) & bit))
        dingosdk::logging::event(dingosdk::logging::Channel::music, json);
}

bool read_at(std::uintptr_t address, auto& value) noexcept {
    return dingosdk::memory::peek(address, value);
}

struct CriticalSectionGuard {
    explicit CriticalSectionGuard(std::uintptr_t address) noexcept
        : section(reinterpret_cast<LPCRITICAL_SECTION>(address)) { EnterCriticalSection(section); }
    ~CriticalSectionGuard() { LeaveCriticalSection(section); }
    CriticalSectionGuard(const CriticalSectionGuard&) = delete;
    CriticalSectionGuard& operator=(const CriticalSectionGuard&) = delete;
    LPCRITICAL_SECTION section;
};

struct BindingCacheGuard {
    BindingCacheGuard() noexcept { AcquireSRWLockExclusive(&binding_cache_lock); }
    ~BindingCacheGuard() { ReleaseSRWLockExclusive(&binding_cache_lock); }
    BindingCacheGuard(const BindingCacheGuard&) = delete;
    BindingCacheGuard& operator=(const BindingCacheGuard&) = delete;
};

bool valid_range(std::uintptr_t value, std::size_t size) noexcept {
    return value >= 0x10000 && value <= dingosdk::memory::highest_user_address - size;
}

bool native_queue_channel(std::uintptr_t base, std::uintptr_t queue,
                          std::uint32_t& channel) noexcept {
    std::uintptr_t vtable{}, getter{};
    if (!read_at(queue, vtable) || !valid_range(vtable, 0x20) ||
        !read_at(vtable + 0x18, getter) || getter < base ||
        getter - base >= supported_build::game_image_size) return false;

    // The engine's candidate-admission callback uses the return from this same
    // virtual slot as its channel-map key (sub_1414F8ED0, call through vtable+0x18).
    using GetChannel = std::uint32_t (__fastcall *)(std::uintptr_t queue);
    channel = reinterpret_cast<GetChannel>(getter)(queue);
    return true;
}

bool find_channel_node(std::uintptr_t manager, std::uint32_t channel,
                       std::uint32_t& playlist_id) noexcept {
    std::uintptr_t buckets{};
    std::uint32_t capacity{};
    std::uintptr_t sentinel{}, node{};
    if (!read_at(manager + 0xc8, buckets) || !read_at(manager + 0xd0, capacity) ||
        !buckets || !capacity || capacity > 4096 || !valid_range(buckets, (capacity + 1ULL) * 8) ||
        !read_at(buckets + capacity * 8ULL, sentinel) ||
        !read_at(buckets + (channel % capacity) * 8ULL, node)) return false;
    for (std::uint32_t seen = 0; node && node != sentinel && seen < 4096; ++seen) {
        std::uint32_t key{}, id{};
        std::uintptr_t next{};
        if (!valid_range(node, 0x10) || !read_at(node, key) || !read_at(node + 4, id) ||
            !read_at(node + 8, next)) return false;
        if (key == channel) { playlist_id = id; return true; }
        node = next;
    }
    return false;
}

std::uintptr_t find_playlist_node(std::uintptr_t manager, std::uint32_t playlist_id) noexcept {
    std::uintptr_t buckets{};
    std::uint32_t capacity{};
    std::uintptr_t sentinel{}, node{};
    if (!read_at(manager + 0xa0, buckets) || !read_at(manager + 0xa8, capacity) ||
        !buckets || !capacity || capacity > 4096 || !valid_range(buckets, (capacity + 1ULL) * 8) ||
        !read_at(buckets + capacity * 8ULL, sentinel) ||
        !read_at(buckets + (playlist_id % capacity) * 8ULL, node)) return 0;
    for (std::uint32_t seen = 0; node && node != sentinel && seen < 8192; ++seen) {
        std::uint32_t id{};
        std::uintptr_t next{};
        if (!valid_range(node, 0x60) || !read_at(node, id) || !read_at(node + 0x58, next)) return 0;
        if (id == playlist_id) return node;
        node = next;
    }
    return 0;
}

bool song_count(std::uintptr_t playlist_asset, std::uint32_t& count) noexcept {
    std::uintptr_t entries{};
    std::uint32_t encoded_count{};
    if (!read_at(playlist_asset + 0x60, entries) || !entries ||
        !read_at(entries - sizeof(encoded_count), encoded_count)) return false;
    count = encoded_count & 0x7fffffffu;
    return count > 0 && count <= 8192 && valid_range(entries, static_cast<std::size_t>(count) * 8);
}

bool member_count(std::uintptr_t node, std::uintptr_t& begin, std::uint32_t& count) noexcept {
    std::uintptr_t end{};
    if (!read_at(node + 0x10, begin) || !read_at(node + 0x18, end) || !begin || end < begin ||
        (end - begin) % 16 || (end - begin) / 16 > 8192) return false;
    count = static_cast<std::uint32_t>((end - begin) / 16);
    return count > 0 && valid_range(begin, static_cast<std::size_t>(count) * 16);
}

bool set_favorites_order(std::uintptr_t node, std::uint32_t member_total) noexcept {
    std::uintptr_t order_begin{}, order_end{}, order_capacity{};
    if (!member_total || member_total > 8192 ||
        !read_at(node + 0x38, order_begin) || !read_at(node + 0x40, order_end) ||
        !read_at(node + 0x48, order_capacity) || !order_begin || order_end < order_begin ||
        order_capacity < order_end || (order_end - order_begin) % sizeof(std::uint32_t) ||
        (order_end - order_begin) / sizeof(std::uint32_t) != member_total ||
        !valid_range(order_begin, static_cast<std::size_t>(member_total) * sizeof(std::uint32_t)))
        return false;

    auto* order = reinterpret_cast<volatile std::uint32_t*>(order_begin);
    for (std::uint32_t position = 0; position < member_total; ++position)
        order[position] = music_playback::ordered_member_index(position, member_total, true);
    return true;
}

bool favorites_order_shape_matches(std::uintptr_t node, std::uint32_t member_total) noexcept {
    std::uintptr_t order_begin{}, order_end{}, order_capacity{};
    return member_total && member_total <= 8192 &&
        read_at(node + 0x38, order_begin) && read_at(node + 0x40, order_end) &&
        read_at(node + 0x48, order_capacity) && order_begin && order_end >= order_begin &&
        order_capacity >= order_end && (order_end - order_begin) % sizeof(std::uint32_t) == 0 &&
        (order_end - order_begin) / sizeof(std::uint32_t) == member_total &&
        valid_range(order_begin, static_cast<std::size_t>(member_total) * sizeof(std::uint32_t));
}

bool favorites_node_ready(std::uintptr_t manager, std::uint32_t favorites_playlist) noexcept {
    if (!favorites_playlist) return false;
    const auto node = find_playlist_node(manager, favorites_playlist);
    std::uintptr_t members{};
    std::uint32_t count{};
    return node && member_count(node, members, count);
}

bool current_guid(std::uintptr_t queue, std::uintptr_t playlist_asset,
                  bool& found, std::array<unsigned char, 16>& guid) noexcept {
    found = false;
    std::int32_t current_index{};
    std::uintptr_t candidates_begin{}, candidates_end{}, entries{};
    std::uint32_t count{};
    if (!read_at(queue + 0x70, current_index)) return false;
    if (current_index < 0) return true;
    if (!read_at(queue + 0x78, candidates_begin) || !read_at(queue + 0x80, candidates_end) ||
        !read_at(playlist_asset + 0x60, entries) || !song_count(playlist_asset, count) ||
        candidates_end < candidates_begin || (candidates_end - candidates_begin) % 4) return false;
    if (static_cast<std::uint64_t>(current_index) >= (candidates_end - candidates_begin) / 4) return false;

    std::uint32_t asset_index{};
    if (!read_at(candidates_begin + static_cast<std::uintptr_t>(current_index) * 4, asset_index) || asset_index >= count)
        return false;
    std::uintptr_t graph{};
    if (!read_at(entries + static_cast<std::uintptr_t>(asset_index) * 8, graph)) return false;
    graph &= ~std::uintptr_t{4};
    if (!valid_range(graph, 0x20) || graph < 16) return false;
    std::uint32_t flags{};
    if (!read_at(graph + 0x14, flags)) return false;
    if (!(flags & 0x100)) return true;
    if (!dingosdk::memory::peek_bytes(graph - 16, guid.data(), guid.size())) return false;
    found = true;
    return true;
}

bool member_index(std::uintptr_t members, std::uint32_t member_count_value,
                  const std::array<unsigned char, 16>& guid, bool has_guid,
                  bool& found, std::uint32_t& position) noexcept {
    found = false;
    if (!has_guid) return true;
    for (std::uint32_t i = 0; i < member_count_value; ++i) {
        std::array<unsigned char, 16> candidate{};
        if (!dingosdk::memory::peek_bytes(members + static_cast<std::uintptr_t>(i) * 16,
                                          candidate.data(), candidate.size())) return false;
        std::uint32_t ignored{};
        if (music_playback::find_member_position(candidate.data(), 1, guid, ignored)) {
            found = true;
            position = i;
            break;
        }
    }
    return true;
}

bool pending_song_request(std::uintptr_t queue, bool& pending) noexcept {
    std::uintptr_t begin{}, end{};
    pending = false;
    if (!read_at(queue + 0x98, begin) || !read_at(queue + 0xa0, end) || end < begin ||
        (end - begin) % 4 || (end - begin) / 4 > 4096) return false;
    pending = begin != end;
    return true;
}

void apply_policy(std::uintptr_t queue, std::uintptr_t playlist_asset) noexcept {
    const auto base = image_base.load(std::memory_order_acquire);
    if (!base || !queue || !playlist_asset) {
        log_policy_event_once(1u << 0, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"queue_context_unavailable\"}");
        return;
    }
    std::uint32_t queue_type{}, channel{}, channel_playlist{};
    if (!read_at(playlist_asset + 0x70, queue_type) || queue_type != 4) {
        log_policy_event_once(1u << 1, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"unsupported_playlist_type\"}");
        return;
    }
    if (!native_queue_channel(base, queue, channel) || channel >= last_binding_by_channel.size()) {
        log_policy_event_once(1u << 2, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"queue_channel_unavailable\"}");
        return;
    }
    music_channels[channel].store(true, std::memory_order_relaxed);

    std::uintptr_t manager{};
    if (!read_at(base + local_music::asset_manager, manager) || !manager ||
        !valid_range(manager, 0xd8)) {
        log_policy_event_once(1u << 3, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"asset_manager_unavailable\"}");
        return;
    }
    {
        CriticalSectionGuard manager_lock(manager + 0x20);
        std::uintptr_t current_manager{};
        if (!read_at(base + local_music::asset_manager, current_manager) || current_manager != manager ||
            !find_channel_node(manager, channel, channel_playlist)) {
            log_policy_event_once(1u << 4, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"channel_playlist_unavailable\"}");
            return;
        }
    }

    // Favorites is a dynamic playlist. Its native node owns the member GUIDs,
    // while its playlist asset has no compiled entries graph at +0x60.
    std::uintptr_t ui_manager{};
    std::uint32_t favorites_playlist{};
    const bool has_favorites_playlist =
        read_at(base + local_music::ui_manager, ui_manager) && valid_range(ui_manager, 0x1fc) &&
        read_at(ui_manager + 0x1f8, favorites_playlist) && favorites_playlist != 0;
    const bool dynamic_favorites = has_favorites_playlist && channel_playlist == favorites_playlist;
    if (dynamic_favorites) {
        char message[192]{};
        std::snprintf(message, sizeof(message),
            "{\"event\":\"music_playback_dynamic_favorites\",\"source\":\"native_playlist_members\",\"channel\":%u,\"playlist\":%u}",
            channel, channel_playlist);
        log_policy_event_once(1u << 5, message);
    }

    // Snapshot the queue-local state under its native lock, then release it before
    // taking the music manager lock. The selector can re-enter this queue lock.
    bool pending{}, has_current_guid{};
    std::array<unsigned char, 16> playing_guid{};
    {
        CriticalSectionGuard queue_lock(queue + 0x150);
        std::uint32_t current_channel{};
        if (!native_queue_channel(base, queue, current_channel) || current_channel != channel ||
            !pending_song_request(queue, pending)) {
            log_policy_event_once(1u << 6, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"queue_snapshot_unavailable\"}");
            return;
        }
        if (!dynamic_favorites) {
            std::uint32_t asset_songs{};
            if (!song_count(playlist_asset, asset_songs)) {
                log_policy_event_once(1u << 7, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"playlist_asset_entries_unavailable\"}");
                return;
            }
            if (!current_guid(queue, playlist_asset, has_current_guid, playing_guid)) {
                log_policy_event_once(1u << 8, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"current_track_unavailable\"}");
                return;
            }
        }
    }

    CriticalSectionGuard manager_lock(manager + 0x20);
    std::uintptr_t current_manager{};
    std::uint32_t current_channel_playlist{};
    if (!read_at(base + local_music::asset_manager, current_manager) || current_manager != manager ||
        !find_channel_node(manager, channel, current_channel_playlist) ||
        current_channel_playlist != channel_playlist) {
        log_policy_event_once(1u << 9, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"channel_playlist_changed\"}");
        return;
    }
    const auto node = find_playlist_node(manager, channel_playlist);
    if (!node) {
        char message[256]{};
        std::snprintf(message, sizeof(message),
            "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"playlist_node_unavailable\",\"channel\":%u,\"playlist\":%u,\"favorites\":%u}",
            channel, channel_playlist, favorites_playlist);
        log_policy_event_once(1u << 10, message);
        return;
    }
    BindingCacheGuard cache_lock;

    std::uint32_t current_mode{}, current_cursor{}, member_total{};
    std::uintptr_t members{};
    if (!read_at(node + 0x0c, current_mode) || !read_at(node + 0x30, current_cursor) ||
        !member_count(node, members, member_total)) {
        log_policy_event_once(1u << 11, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"playlist_members_unavailable\"}");
        return;
    }

    const auto& previous = last_binding_by_channel[channel];
    const bool new_group = previous.manager != manager || previous.queue != queue ||
        previous.node != node || previous.playlist_id != channel_playlist;
    const auto added_favorites = dynamic_favorites && !new_group &&
        member_total > previous.member_count ? member_total - previous.member_count : 0;
    const bool favorites_members_changed = dynamic_favorites && !new_group &&
        member_total != previous.member_count;
    bool current_found{};
    std::uint32_t current_position{};
    if (!member_index(members, member_total, playing_guid, has_current_guid, current_found, current_position)) {
        log_policy_event_once(1u << 12, "{\"event\":\"music_playback_policy_skipped\",\"reason\":\"member_identity_unavailable\"}");
        return;
    }

    const auto shuffle = shuffle_enabled.load(std::memory_order_acquire);
    auto plan = music_playback::plan_cursor(shuffle,
        current_mode, current_cursor, new_group, pending, current_found, current_position, member_total);
    if (dynamic_favorites && !shuffle && !plan.set_cursor && current_mode == music_playback::native_sequential_mode &&
        added_favorites && current_cursor != music_playback::cursor_sync_sentinel && current_cursor > 0 &&
        current_cursor < member_total) {
        // New favorites appear at the top of the visible list. The reversed index
        // vector therefore grows at its front; advance the cursor by the insertion
        // count to keep the next selection after the currently playing member.
        plan.set_cursor = true;
        plan.cursor = current_cursor + added_favorites;
        if (plan.cursor >= member_total) plan.cursor = 0;
    }
    const bool rebuild_favorites_order = dynamic_favorites && !shuffle && !new_group &&
        current_mode == music_playback::native_sequential_mode &&
        (favorites_members_changed || !favorites_order_shape_matches(node, member_total));
    if (plan.set_mode || rebuild_favorites_order) {
        // Clear the rebuild latch only as an input. The native rebuild sets it on
        // success; preserving that output is required for later rebuild semantics.
        *reinterpret_cast<volatile std::uint8_t*>(node + 8) = 0;
        set_playback_mode(manager, channel_playlist,
            plan.set_mode ? plan.mode : music_playback::native_sequential_mode);
    }
    if (dynamic_favorites && !shuffle) {
        if (!set_favorites_order(node, member_total)) {
            log_policy_event_once(1u << 13,
                "{\"event\":\"music_playback_favorites_order_unavailable\"}");
        } else if (plan.set_mode || rebuild_favorites_order) {
            char message[144]{};
            std::snprintf(message, sizeof(message),
                "{\"event\":\"music_playback_favorites_order\",\"direction\":\"top_to_bottom\",\"members\":%u}",
                member_total);
            dingosdk::logging::event(dingosdk::logging::Channel::music, message);
        }
    }
    if (plan.set_cursor) *reinterpret_cast<volatile std::uint32_t*>(node + 0x30) = plan.cursor;
    last_binding_by_channel[channel] = {manager, queue, node, channel_playlist, member_total};
}
}

bool initialize_music_playback(std::uintptr_t base, bool shuffle) noexcept {
    if (!base || local_music::playback_select_next_contract.rva >= supported_build::game_image_size ||
        local_music::playback_mode_setter_contract.rva >= supported_build::game_image_size) return false;
    std::array<unsigned char, 32> selector{}, setter{};
    if (!dingosdk::memory::read_bytes(base + local_music::playback_select_next_contract.rva,
            selector.data(), selector.size()) || selector != local_music::playback_select_next_contract.bytes ||
        !dingosdk::memory::read_bytes(base + local_music::playback_mode_setter_contract.rva,
            setter.data(), setter.size()) || setter != local_music::playback_mode_setter_contract.bytes) return false;
    image_base.store(base, std::memory_order_release);
    set_playback_mode = reinterpret_cast<SetPlaybackMode>(base + local_music::playback_mode_setter_contract.rva);
    shuffle_enabled.store(shuffle, std::memory_order_release);
    last_binding_by_channel = {};
    for (auto& binding : last_logged_channel_bindings)
        binding.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
    for (auto& known : music_channels) known.store(false, std::memory_order_relaxed);
    logged_policy_events.store(0, std::memory_order_relaxed);
    playback_available.store(false, std::memory_order_release);
    return true;
}

void activate_music_playback() noexcept { playback_available.store(true, std::memory_order_release); }
void deactivate_music_playback() noexcept { playback_available.store(false, std::memory_order_release); }
bool music_playback_available() noexcept { return playback_available.load(std::memory_order_acquire); }

MusicSelectNext& music_select_next_original() noexcept { return select_next; }

std::int32_t __fastcall music_select_next_hook(std::uintptr_t queue, std::uintptr_t playlist_asset) {
    if (select_next) {
        apply_policy(queue, playlist_asset);
        return select_next(queue, playlist_asset);
    }
    return -1;
}

MusicChannelPlaylistBind& music_channel_playlist_bind_original() noexcept { return bind_channel_playlist; }

std::uintptr_t __fastcall music_channel_playlist_bind_hook(std::uintptr_t channel_pointer,
                                                           std::uintptr_t playlist_id_pointer) {
    if (!bind_channel_playlist) return 0;
    std::uint32_t channel{}, requested_playlist_id{}, favorites_playlist{};
    const bool have_binding = read_at(channel_pointer, channel) &&
        read_at(playlist_id_pointer, requested_playlist_id);
    auto playlist_id = requested_playlist_id;
    bool favorites_remapped{};
    // Zero is Favorites only where the music player asks: a channel it has selected songs for.
    const bool music_channel = have_binding && channel < music_channels.size() &&
        music_channels[channel].load(std::memory_order_relaxed);
    if (music_channel && requested_playlist_id == 0) {
        const auto base = image_base.load(std::memory_order_acquire);
        std::uintptr_t manager{}, ui_manager{};
        const bool have_favorites = base &&
            read_at(base + local_music::asset_manager, manager) && manager && valid_range(manager, 0xd8) &&
            read_at(base + local_music::ui_manager, ui_manager) && valid_range(ui_manager, 0x1fc) &&
            read_at(ui_manager + 0x1f8, favorites_playlist);
        bool node_ready{};
        if (have_favorites && favorites_playlist) {
            CriticalSectionGuard manager_lock(manager + 0x20);
            std::uintptr_t current_manager{};
            node_ready = read_at(base + local_music::asset_manager, current_manager) &&
                current_manager == manager && favorites_node_ready(manager, favorites_playlist);
        }
        playlist_id = music_playback::resolve_playlist_binding_id(
            requested_playlist_id, favorites_playlist, node_ready);
        favorites_remapped = playlist_id != requested_playlist_id;
    }

    const auto result = favorites_remapped
        ? bind_channel_playlist(channel_pointer, reinterpret_cast<std::uintptr_t>(&playlist_id))
        : bind_channel_playlist(channel_pointer, playlist_id_pointer);

    // Said once for each change of a channel's binding. A channel outside the table cannot be
    // told from its last binding, so it is not said at all rather than every time.
    if (have_binding && channel < last_logged_channel_bindings.size()) {
        const auto signature = (static_cast<std::uint64_t>(channel) << 32) | playlist_id;
        if (last_logged_channel_bindings[channel].exchange(signature, std::memory_order_relaxed) != signature) {
            char message[256]{};
            std::snprintf(message, sizeof(message),
                "{\"event\":\"music_channel_playlist_binding\",\"channel\":%u,\"music_channel\":%s,\"requested_playlist_id\":%u,\"playlist_id\":%u,\"favorites\":%u,\"favorites_remapped\":%s,\"applied\":%s}",
                channel, music_channel ? "true" : "false", requested_playlist_id, playlist_id, favorites_playlist,
                favorites_remapped ? "true" : "false", result ? "true" : "false");
            dingosdk::logging::event(dingosdk::logging::Channel::music, message);
        }
    }
    return result;
}

bool music_shuffle_enabled() noexcept { return shuffle_enabled.load(std::memory_order_acquire); }

void set_music_shuffle_enabled(bool enabled) noexcept {
    shuffle_enabled.store(enabled, std::memory_order_release);
}
} // namespace dingosdk::profile_runtime
