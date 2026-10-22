#include "bmx_player_startup.h"
#include "bmx_player_startup_guard.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/image_identity.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_party.h"
#include "Engine/Game/Build/20260929/world_model.h"
#include "Engine/Game/World/world_model.h"
#include "Extension/Multiplayer/Session/session.h"
#include <Windows.h>
#include <Psapi.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>

namespace dingosdk::bmx {
namespace {
namespace party = addr::native_party;
constexpr std::uintptr_t publish_rva = 0x4691310;
struct State {
    std::atomic<bool> requested{}, scanning{}, scan_done{};
    std::atomic<std::uintptr_t> found_type{};
    std::jthread scanner;
    std::mutex mutex;
    std::string message{"idle; no initial-player event requested"};
    startup_detail::AttemptGate attempts;
    startup_detail::WorldKey requested_world{};
    std::string requested_level;
    std::uint64_t generation{}, deadline{}, observation_until{};
    bool ready_before{}, pending{}, failed{};
};
State& state() { static State value; return value; }
bool opt_in() noexcept {
    try {
        std::array<wchar_t, 2> value{};
        return GetEnvironmentVariableW(L"RESKATE_BMX_PLAYER_STARTUP_REQUEST", value.data(), static_cast<DWORD>(value.size())) == 1 &&
            value[0] == L'1' && user_data::override_root().has_value();
    } catch (...) { return false; }
}
void message(std::string value) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.message == value) return;
    s.message = std::move(value);
    logging::log(logging::Level::info, logging::Channel::runtime, "BMX player recovery: {}", s.message);
}
template<class T> bool get(std::uintptr_t address, T& value) noexcept { return memory::peek(address, value); }
bool readable_span(std::uintptr_t address, std::size_t bytes) noexcept {
    if (!bytes || address < 0x10000 || address % 8 || bytes > memory::highest_user_address ||
        address > memory::highest_user_address - bytes) return false;
    const auto limit = address + bytes;
    for (auto cursor = address; cursor < limit;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        const auto region_begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize > memory::highest_user_address - region_begin) return false;
        const auto next = region_begin + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}
bool event_type(std::uintptr_t base, std::uintptr_t object) noexcept {
    return startup_detail::event_type_contract(object, base + addr::engine::bool_type,
        [](std::uintptr_t address, void* out, std::size_t size) noexcept { return memory::peek_bytes(address, out, size); });
}
template<std::size_t Size> bool matches(std::uintptr_t address, const std::array<unsigned char, Size>& expected) noexcept {
    std::array<unsigned char, Size> actual{};
    return memory::peek_bytes(address, actual.data(), actual.size()) && actual == expected;
}
bool supported(std::uintptr_t base) noexcept {
    constexpr std::array<unsigned char, 32> publish_prefix{
        0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xf9,0x48,0xc7,0x44,
        0x24,0x34,0x01,0x00,0x00,0x00,0xc5,0xf8,0x57,0xc0,0x48,0x8d,0x4c,0x24,0x50,0x48};
    if (!supported_build::running_image_matches(base) || !matches(base + publish_rva, publish_prefix)) return false;
    for (const auto rva : {party::current_context, party::event_dispatcher, party::event_post}) {
        bool verified{};
        for (const auto& contract : party::party_request_contracts)
            if (contract.rva == rva) verified = matches(base + rva, contract.bytes);
        if (!verified) return false;
    }
    return true;
}
// The scanner reads only resident private writable pages, never calls native code,
// and checks cancellation between every page. Its result is revalidated on the
// game thread; no type or context pointer from a previous world is used there.
void scan(std::stop_token stop, std::uintptr_t base) noexcept {
    const bool lowered = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
    std::uintptr_t cursor = 0x10000, found{};
    MEMORY_BASIC_INFORMATION region{};
    while (!stop.stop_requested() && !found && cursor < 0x00007fffffff0000ULL &&
        VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region))) {
        const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto end = begin + region.RegionSize;
        if (end <= cursor) break;
        if (region.State == MEM_COMMIT && region.Type == MEM_PRIVATE && (region.Protect & PAGE_READWRITE) &&
            !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && region.RegionSize <= (1ULL << 30)) {
            for (auto page = begin; page + 4096 <= end && !stop.stop_requested() && !found; page += 4096) {
                PSAPI_WORKING_SET_EX_INFORMATION resident{};
                resident.VirtualAddress = reinterpret_cast<void*>(page);
                if (!QueryWorkingSetEx(GetCurrentProcess(), &resident, sizeof(resident)) || !resident.VirtualAttributes.Valid) continue;
                std::array<std::uint32_t, 1024> words{};
                if (!memory::peek_bytes(page, words.data(), sizeof(words))) continue;
                for (std::size_t i = 0; i < words.size(); i += 2) {
                    if (words[i] != startup_detail::event_hash) continue;
                    const auto record = page + i * 4;
                    std::uintptr_t array{}, array_record{}, object{};
                    if (get(record + 0x20, array) && get(array, array_record) && get(array_record + 0x30, object) && event_type(base, object)) {
                        found = object; break;
                    }
                }
            }
        }
        cursor = end;
        SwitchToThread();
    }
    if (lowered) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    auto& s = state();
    if (!stop.stop_requested()) s.found_type.store(found, std::memory_order_release);
    s.scan_done.store(true, std::memory_order_release);
    s.scanning.store(false, std::memory_order_release);
}
void cancel_scan() noexcept {
    auto& s = state();
    if (s.scanner.joinable()) { s.scanner.request_stop(); s.scanner.join(); }
    s.found_type.store(0, std::memory_order_release);
    s.scanning.store(false, std::memory_order_release);
    s.scan_done.store(false, std::memory_order_release);
}
struct World {
    startup_detail::WorldKey key;
    std::string level;
    std::uintptr_t entity{};
    const char* issue{"world unavailable"};
};
bool world(std::uintptr_t base, std::uintptr_t client, World& result, bool require_missing) {
    std::uintptr_t table{}, context{}, manager{}, begin{}, end{}, player{}, server{}, root_manager{}, root{};
    std::uint32_t phase{}, kind{}, manager_offset{}, realm_offset{}, realm{}, tls_index{}, dispatcher_offset{};
    std::uint8_t human{}, bot{}, root_active{}, tls_active{}, dispatcher_active{};
    const auto description = read_world_description(GetCurrentProcess(), client + 0x198);
    result.issue = "WorldDescription unavailable";
    if (!description.available) return false;
    auto level = description.level;
    std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    result.issue = "WorldDescription must select DingoLevel_Root plus authored BAM/MPR detached world";
    if (!startup_detail::recovery_world(description.level, description.lm_level)) return false;
    level += "|" + description.lm_level;
    const auto sublevels = read_server_sublevels(GetCurrentProcess(), base);
    result.issue = "native server root or requested detached world is not active";
    if (!sublevels.available || !sublevels.active || !std::ranges::any_of(sublevels.levels, [&](const auto& entry) {
        return entry.state == 5 && startup_detail::asset_equal(entry.asset, description.lm_level);
    })) return false;
    result.issue = "client phase, kind or canonical client realm rejected";
    if (!get(client, table) || table != base + addr::engine::client_vtable || !get(client + 8, context) || !context ||
        !get(client + 0xc4, phase) || (phase != 13 && phase != 21) || !get(client + 0xc0, kind) || kind > 1 ||
        !get(base + addr::engine::context_type_offset, realm_offset) || realm_offset > 0x1000000 ||
        !get(context + realm_offset, realm) || realm != 0xbf0f9789) return false;
    result.issue = "native server root identity unavailable";
    if (
        !get(base + addr::engine::game_server, server) || !get(server, table) || table != base + addr::engine::server_vtable ||
        !get(base + addr::world_model::sublevel_manager, root_manager) || !get(root_manager + 0x40, root) || !root ||
        !get(root_manager + 0xb0, root_active) || root_active != 1) return false;
    result.issue = "local human player manager or singleton identity rejected";
    if (
        !get(base + addr::engine::context_player_manager_offset, manager_offset) || manager_offset > 0x1000000 ||
        !get(context + manager_offset, manager) || !get(manager, table) || table != base + addr::engine::local_player_manager_vtable ||
        !get(manager + 0x4c8, begin) || !get(manager + 0x4d0, end) || begin < 0x10000 || begin % 8 || end != begin + 8 ||
        !get(begin, player) || !get(player, table) || table != base + addr::engine::local_player_vtable ||
        !get(player + 0x45, human) || human != 1 || !get(player + 0x44, bot) || bot) return false;
    result.issue = "local actor is already present or entity field unavailable";
    if (!get(player + 0xb8, result.entity) || (require_missing && result.entity)) return false;
    std::uintptr_t controller{}, owned_server{};
    result.issue = "local connection controller does not own the native server";
    if (!get(client + 0xd0, controller) || !get(controller, table) || table != base + addr::engine::controller_vtable ||
        !get(controller + 0x4c0, owned_server) || owned_server != server) return false;
    if (result.entity) {
        std::uintptr_t actor_player{}, actor_context{};
        result.issue = "observed local actor type or ownership rejected";
        if (!get(result.entity, table) || table != base + addr::engine::skater_entity_vtable ||
            !get(result.entity + 0xf8, actor_player) || actor_player != player ||
            !get(result.entity + 0x20, actor_context) || actor_context != context) return false;
    }
    const auto tls_array = static_cast<std::uintptr_t>(__readgsqword(0x58));
    std::uintptr_t tls{}, current{}, dispatch_queue{}, buckets{}, selected_bucket{}, sentinel_bucket{};
    std::uint32_t bucket_count{};
    result.issue = "native TLS job context is not the canonical client context";
    if (!get(base + addr::engine::tls_index, tls_index) || tls_index > 4095 ||
        !get(tls_array + std::uintptr_t{tls_index} * 8, tls) || !get(tls + 0xb19, tls_active) || !tls_active ||
        !get(tls + 0x550, current) || current != context) return false;
    result.issue = "native ECS dispatcher is unavailable or inactive";
    if (
        !get(base + 0x77dd848, dispatcher_offset) || dispatcher_offset > 0x1000000 ||
        !get(context + dispatcher_offset + 0x28, dispatcher_active) || dispatcher_active != 1 ||
        !get(context + dispatcher_offset + 0x20, dispatch_queue) || dispatch_queue < 0x10000) return false;
    // Native 4b2e2b0 divides the event hash by count, reads buckets[hash %
    // count], and may read its end sentinel at buckets[count]. Validate those
    // exact initial reads and the complete bounded table before invoking it.
    result.issue = "native ECS dispatch bucket table is unavailable or out of bounds";
    if (!get(dispatch_queue + 0x18c0, bucket_count) || !bucket_count || bucket_count > 65536 ||
        !get(dispatch_queue + 0x18b8, buckets) ||
        !readable_span(buckets, (std::size_t{bucket_count} + 1) * sizeof(std::uintptr_t)) ||
        !get(buckets + (startup_detail::event_hash % bucket_count) * sizeof(std::uintptr_t), selected_bucket) ||
        !get(buckets + std::uintptr_t{bucket_count} * sizeof(std::uintptr_t), sentinel_bucket)) return false;
    std::uintptr_t check{};
    result.issue = "world or local player changed during guard validation";
    if (!get(client + 8, check) || check != context || !get(root_manager + 0x40, check) || check != root ||
        !get(context + manager_offset, check) || check != manager || !get(begin, check) || check != player ||
        !get(player + 0xb8, check) || check != result.entity ||
        !get(base + addr::engine::game_server, check) || check != server ||
        !get(base + addr::world_model::sublevel_manager, check) || check != root_manager ||
        !get(controller + 0x4c0, check) || check != server ||
        !get(manager + 0x4c8, check) || check != begin || !get(manager + 0x4d0, check) || check != end ||
        !get(tls + 0x550, check) || check != context ||
        !get(context + dispatcher_offset + 0x20, check) || check != dispatch_queue ||
        !get(dispatch_queue + 0x18b8, check) || check != buckets ||
        !get(buckets + (startup_detail::event_hash % bucket_count) * sizeof(std::uintptr_t), check) || check != selected_bucket ||
        !get(buckets + std::uintptr_t{bucket_count} * sizeof(std::uintptr_t), check) || check != sentinel_bucket) return false;
    std::uint32_t after_bucket_count{};
    std::uint8_t after_tls_active{}, after_dispatcher_active{};
    if (!get(dispatch_queue + 0x18c0, after_bucket_count) || after_bucket_count != bucket_count ||
        !get(tls + 0xb19, after_tls_active) || after_tls_active != tls_active ||
        !get(context + dispatcher_offset + 0x28, after_dispatcher_active) || after_dispatcher_active != dispatcher_active) return false;
    const auto after_description = read_world_description(GetCurrentProcess(), client + 0x198);
    if (!after_description.available || after_description.level != description.level || after_description.lm_level != description.lm_level) return false;
    result.key = {client, context, root, player, state().generation};
    result.level = std::move(level);
    result.issue = "ready";
    return true;
}
// Exact authored binding 575ea096: RCX is an eight-byte TypeInfoRef cell;
// RDX is the payload address. Native 4691310 derives its own TLS dispatcher.
// Exact POD layout is validated: one bool at offset zero. Authored K100 is an
// immutable false byte. Keep the same payload for the process lifetime because
// the native dispatcher may pass it to a listener that completes asynchronously.
bool publish(std::uintptr_t base, std::uintptr_t type) noexcept {
    static constexpr std::uint8_t force_spawn_skater = 0;
    __try {
        if (!event_type(base, type)) return false;
        reinterpret_cast<void(*)(const std::uintptr_t*, const void*)>(base + publish_rva)(&type, &force_spawn_skater);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool request_player_startup() noexcept {
    try {
        if (!opt_in() || state().failed) return false;
        state().requested.store(true, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}
void tick_player_startup(std::uintptr_t base, std::uintptr_t client, bool gameplay_ready) noexcept {
    const auto incoming_error = GetLastError();
    try {
        auto& s = state();
        if (!gameplay_ready && s.ready_before) ++s.generation;
        s.ready_before = gameplay_ready;
        if (!s.requested.load(std::memory_order_acquire) && !s.pending && !s.observation_until) { SetLastError(incoming_error); return; }
        const auto mp = multiplayer::model();
        if (!opt_in() || !gameplay_ready || mp.active || mp.lobby_joining || !supported(base)) {
            s.requested.store(false); s.pending = false; s.observation_until = 0; cancel_scan();
            message("blocked: requires the supported isolated loaded solo world");
        } else if (s.requested.exchange(false)) {
            World current;
            if (s.pending) message("an explicit initial-player request is already pending");
            else if (!world(base, client, current, true)) message(std::string("blocked: ") + current.issue);
            else {
                cancel_scan(); s.requested_world = current.key; s.requested_level = current.level;
                s.deadline = GetTickCount64() + 15000; s.pending = true;
                s.scanning.store(true);
                s.scanner = std::jthread([base](std::stop_token stop) { scan(stop, base); });
                message("looking up the authored initial-player event type; no event emitted yet");
            }
        }
        if (s.pending) {
            World current;
            const bool eligible = world(base, client, current, true);
            if (!eligible || current.key != s.requested_world || current.level != s.requested_level) {
                s.pending = false; cancel_scan();
                message(std::string("cancelled: ") + (eligible ? "native world identity changed" : current.issue));
            } else if (GetTickCount64() >= s.deadline) {
                s.pending = false; cancel_scan(); message("cancelled: event type lookup timed out; no event emitted");
            } else if (s.scan_done.load(std::memory_order_acquire)) {
                const auto type = s.found_type.load(std::memory_order_acquire);
                if (s.scanner.joinable()) s.scanner.join();
                s.pending = false;
                if (!type || !event_type(base, type)) message("blocked: exact native event metadata was not found; no event emitted");
                else if (!s.attempts.consume(current.key)) message("blocked: initial-player event was already attempted in this world generation");
                else if (publish(base, type)) {
                    s.observation_until = GetTickCount64() + 15000;
                    message("authored initial-player event published once with ForceSpawnSkater=false; awaiting actual local actor");
                } else { s.failed = true; message("native publication failed; recovery disabled for this process"); }
                s.found_type.store(0);
            }
        }
        if (s.observation_until) {
            World current;
            if (world(base, client, current, false) && current.key == s.requested_world && current.level == s.requested_level && current.entity) {
                s.observation_until = 0; message("an actual local actor is now present after event publication; BMX riding still requires separate validation");
            } else if (GetTickCount64() >= s.observation_until) {
                s.observation_until = 0; message("event published, but no matching actual local actor was observed within 15 seconds");
            }
        }
    } catch (...) {
        auto& s = state(); s.pending = false; s.requested.store(false); s.observation_until = 0;
        cancel_scan();
        try { message("recovery cancelled by a diagnostic exception"); } catch (...) {}
    }
    SetLastError(incoming_error);
}
void stop_player_startup() noexcept {
    try { auto& s = state(); s.requested.store(false); s.pending = false; s.observation_until = 0; cancel_scan(); } catch (...) {}
}
std::string player_startup_status() {
    std::lock_guard lock(state().mutex);
    return "Native initial-player recovery: " + state().message;
}
}
