#include "local_music_playback.h"

#include "local_music_playback_policy.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/local_music.h"
#include "Engine/Game/Build/supported_build.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

namespace dingosdk::profile_runtime {
namespace {
using namespace dingosdk::game::build::v20260929;
using local_music::playback_mode_setter_contract;
using local_music::playback_select_next_contract;
using music_playback::CursorPlan;

std::atomic<std::uintptr_t> image_base{};
std::atomic<bool> shuffle_enabled{};
std::atomic<bool> playback_available{};
MusicSelectNext select_next{};
using SetPlaybackMode = void (__fastcall *)(std::uintptr_t manager, std::uint32_t playlist_id,
                                           std::uint32_t mode);
SetPlaybackMode set_playback_mode{};
struct LastBinding {
    std::uintptr_t manager{};
    std::uintptr_t queue{};
    std::uintptr_t node{};
    std::uint32_t playlist_id{std::numeric_limits<std::uint32_t>::max()};
};
std::array<LastBinding, 256> last_binding_by_channel{}; // Protected by the binding-cache lock.
SRWLOCK binding_cache_lock = SRWLOCK_INIT;

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
    if (!base || !queue || !playlist_asset) return;
    std::uint32_t queue_type{}, channel{}, channel_playlist{};
    if (!read_at(playlist_asset + 0x70, queue_type) || queue_type != 4 ||
        !read_at(queue + 0x60, channel) || channel >= last_binding_by_channel.size()) return;

    // Snapshot the queue-local state under its native lock, then release it before
    // taking the music manager lock. The selector can re-enter this queue lock.
    bool pending{}, has_current_guid{};
    std::array<unsigned char, 16> playing_guid{};
    std::uint32_t asset_songs{};
    {
        CriticalSectionGuard queue_lock(queue + 0x150);
        if (!pending_song_request(queue, pending) || !song_count(playlist_asset, asset_songs) ||
            !current_guid(queue, playlist_asset, has_current_guid, playing_guid)) return;
    }

    std::uintptr_t manager{};
    if (!read_at(base + local_music::asset_manager, manager) || !manager ||
        !valid_range(manager, 0xd8)) return;
    CriticalSectionGuard manager_lock(manager + 0x20);
    std::uintptr_t current_manager{};
    if (!read_at(base + local_music::asset_manager, current_manager) || current_manager != manager ||
        !find_channel_node(manager, channel, channel_playlist)) return;
    const auto node = find_playlist_node(manager, channel_playlist);
    if (!node) return;
    BindingCacheGuard cache_lock;

    std::uint32_t current_mode{}, current_cursor{}, member_total{};
    std::uintptr_t members{};
    if (!read_at(node + 0x0c, current_mode) || !read_at(node + 0x30, current_cursor) ||
        !member_count(node, members, member_total)) return;

    const auto& previous = last_binding_by_channel[channel];
    const bool new_group = previous.manager != manager || previous.queue != queue ||
        previous.node != node || previous.playlist_id != channel_playlist;
    bool current_found{};
    std::uint32_t current_position{};
    if (!member_index(members, member_total, playing_guid, has_current_guid, current_found, current_position)) return;

    const auto plan = music_playback::plan_cursor(shuffle_enabled.load(std::memory_order_acquire),
        current_mode, current_cursor, new_group, pending, current_found, current_position, member_total);
    if (plan.set_mode) {
        // Clear the rebuild latch only as an input. The native rebuild sets it on
        // success; preserving that output is required for later rebuild semantics.
        *reinterpret_cast<volatile std::uint8_t*>(node + 8) = 0;
        set_playback_mode(manager, channel_playlist, plan.mode);
    }
    if (plan.set_cursor) *reinterpret_cast<volatile std::uint32_t*>(node + 0x30) = plan.cursor;
    last_binding_by_channel[channel] = {manager, queue, node, channel_playlist};
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

bool music_shuffle_enabled() noexcept { return shuffle_enabled.load(std::memory_order_acquire); }

void set_music_shuffle_enabled(bool enabled) noexcept {
    shuffle_enabled.store(enabled, std::memory_order_release);
}
} // namespace dingosdk::profile_runtime
