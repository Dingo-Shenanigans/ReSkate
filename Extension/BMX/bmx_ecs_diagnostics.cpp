#include "bmx_ecs_diagnostics.h"
#include "bmx_ecs_diagnostics_guard.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/image_identity.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <sstream>

namespace dingosdk::bmx {
namespace {
struct PreserveError { DWORD value{GetLastError()}; ~PreserveError() { SetLastError(value); } };
template<class T> bool get(std::uintptr_t p, T& value) noexcept { return memory::peek(p, value); }
bool enabled(std::uintptr_t base) noexcept {
    try {
        std::array<wchar_t, 2> value{};
        return GetEnvironmentVariableW(L"RESKATE_BMX_FLOW_DIAGNOSTICS", value.data(), static_cast<DWORD>(value.size())) == 1 &&
            value[0] == L'1' && user_data::development_requested() && user_data::override_root().has_value() &&
            supported_build::running_image_matches(base);
    } catch (...) { return false; }
}
struct Context {
    std::uintptr_t context{}, dispatcher{}, queue{};
    std::uint32_t realm{}, bucket_count{};
    std::uint8_t dispatcher_active{};
    bool available{};
    bool operator==(const Context&) const = default;
};
Context context_snapshot(std::uintptr_t base, std::uintptr_t context) noexcept {
    Context result;
    result.context = context;
    std::uint32_t realm_offset{}, dispatcher_offset{};
    if (context % 8 || !get(base + addr::engine::context_type_offset, realm_offset) || realm_offset > 0x1000000 ||
        !get(base + 0x77dd848, dispatcher_offset) || dispatcher_offset > 0x1000000 ||
        !ecs_detail::span(context, std::size_t{realm_offset} + 4) ||
        !ecs_detail::span(context, std::size_t{dispatcher_offset} + 0x29) ||
        !get(context + realm_offset, result.realm)) return result;
    result.dispatcher = context + dispatcher_offset;
    if (!get(result.dispatcher + 0x28, result.dispatcher_active) || !get(result.dispatcher + 0x20, result.queue) ||
        result.queue % 8 || !ecs_detail::span(result.queue, 0x18c4) || !get(result.queue + 0x18c0, result.bucket_count) ||
        !result.bucket_count || result.bucket_count > 65536) return result;
    result.available = true;
    return result;
}
struct Tls {
    std::uintptr_t array{}, storage{}, context{};
    std::uint32_t index{};
    std::uint8_t active{};
    bool available{};
    bool operator==(const Tls&) const = default;
};
Tls tls_snapshot(std::uintptr_t base) noexcept {
    Tls result;
    result.array = static_cast<std::uintptr_t>(__readgsqword(0x58));
    if (!get(base + addr::engine::tls_index, result.index) || result.index > 4095 ||
        !ecs_detail::span(result.array, (std::size_t{result.index} + 1) * 8) ||
        !get(result.array + std::uintptr_t{result.index} * 8, result.storage) ||
        !ecs_detail::span(result.storage, 0xb1a) || !get(result.storage + 0xb19, result.active) ||
        !get(result.storage + 0x550, result.context)) return result;
    result.available = true;
    return result;
}
struct Initializer {
    std::atomic<unsigned> captured{};
    Context context;
    Tls tls;
    std::uint64_t tick{};
    DWORD thread{};
    bool coherent{};
};
Initializer& initializer() { static Initializer value; return value; }
void write_context(std::ostringstream& out, const Context& value) {
    out << "{\"available\":" << value.available << ",\"identity\":" << value.context << ",\"realm\":" << value.realm
        << ",\"dispatcher_active\":" << unsigned{value.dispatcher_active} << ",\"queue_identity\":" << value.queue
        << ",\"bucket_count\":" << value.bucket_count << '}';
}
struct LevelClassReference {
    const char* status{"unavailable"};
    bool coherent{}, present{};
};
LevelClassReference level_class_reference(std::uintptr_t base, const Context& context, const Tls& tls) noexcept {
    LevelClassReference out;
    if (!context.available || context.realm != 0xbf0f9789 || !tls.available || tls.active != 1 || tls.context != context.context) return out;
    std::uintptr_t global{}, service{}, level{}, reference{};
    std::uint32_t offset{};
    if (!get(base + 0x73c7cb0, global) || global != context.context ||
        !get(base + 0x714c5a0, offset) || offset > 0x1000000 || offset % 8 ||
        !ecs_detail::span(global, std::size_t{offset} + 8) || !get(global + offset, service)) return out;
    if (!service) { out.status = "service_absent"; return out; }
    if (service % 8 || !ecs_detail::span(service, 0x28) || !get(service + 0x20, level) ||
        level % 8 || !ecs_detail::span(level, 0xa8) || !get(level + 0xa0, reference)) return out;
    std::uintptr_t global_again{}, service_again{}, level_again{}, reference_again{};
    std::uint32_t offset_again{};
    if (!get(base + 0x73c7cb0, global_again) || global_again != global ||
        !get(base + 0x714c5a0, offset_again) || offset_again != offset ||
        !get(global + offset, service_again) || service_again != service ||
        !get(service + 0x20, level_again) || level_again != level ||
        !get(level + 0xa0, reference_again) || reference_again != reference ||
        context != context_snapshot(base, context.context) || tls != tls_snapshot(base)) {
        out.status = "changed"; return out;
    }
    out.status = "complete"; out.coherent = true; out.present = reference != 0;
    return out;
}
std::uint32_t image_size(std::uintptr_t base) noexcept {
    std::int32_t offset{}; std::uint32_t size{};
    if (!get(base + 0x3c, offset) || offset < 0x40 || offset > 0x1000 ||
        !get(base + static_cast<std::uintptr_t>(offset) + 0x50, size)) return 0;
    return size;
}
}
void observe_recipe_initializer_context() noexcept {
    PreserveError preserve;
    auto& result = initializer();
    if (result.captured.load(std::memory_order_acquire)) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!enabled(base)) return;
    unsigned expected{};
    if (!result.captured.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
    result.tick = GetTickCount64(); result.thread = GetCurrentThreadId();
    result.tls = tls_snapshot(base);
    result.context = context_snapshot(base, result.tls.context);
    result.coherent = result.tls.available && result.context.available &&
        result.tls == tls_snapshot(base) && result.context == context_snapshot(base, result.tls.context);
    result.captured.store(2, std::memory_order_release);
}
std::string ecs_startup_diagnostics(std::uintptr_t base, std::uintptr_t client) {
    PreserveError preserve;
    if (!enabled(base)) return "{\"ecs_startup\":{\"enabled\":false}}";
    std::uintptr_t vtable{}, context{};
    const bool valid_client = get(client, vtable) && vtable == base + addr::engine::client_vtable && get(client + 8, context);
    const auto live = context_snapshot(base, valid_client ? context : 0);
    const auto tls = tls_snapshot(base);
    std::uint32_t capability_offset{}, capability_offset_again{};
    auto capabilities = live.available && get(base + 0x73c7ce0, capability_offset)
        ? ecs_detail::inspect_capabilities(context, capability_offset,
            [](std::uintptr_t p, void* out, std::size_t n) noexcept { return memory::peek_bytes(p, out, n); })
        : ecs_detail::Capabilities{};
    if (!get(base + 0x73c7ce0, capability_offset_again) || capability_offset_again != capability_offset) {
        capabilities.coherent = false; capabilities.status = "offset_changed_or_unavailable";
    }
    auto level_class = level_class_reference(base, live, tls);
    const auto profile_query = ecs_detail::inspect_profile_query(base + 0x71eff00,
        [](std::uintptr_t p, void* out, std::size_t n) noexcept { return memory::peek_bytes(p, out, n); });
    const auto native_query = ecs_detail::inspect_native_profile_query(base, profile_query,
        [](std::uintptr_t p, void* out, std::size_t n) noexcept { return memory::peek_bytes(p, out, n); });
    auto bucket = live.available && live.dispatcher_active == 1
        ? ecs_detail::inspect_bucket(live.queue, [](std::uintptr_t p, void* out, std::size_t n) noexcept { return memory::peek_bytes(p, out, n); })
        : ecs_detail::Bucket{};
    std::uintptr_t current_vtable{}, current_context{};
    const bool stable = valid_client && get(client, current_vtable) && current_vtable == vtable &&
        get(client + 8, current_context) && current_context == context && live == context_snapshot(base, context);
    if (!stable) {
        bucket.coherent = false; bucket.status = "context_changed_or_unavailable";
        capabilities.coherent = false; capabilities.status = "context_changed_or_unavailable";
        level_class.coherent = false; level_class.status = "context_changed_or_unavailable";
    }
    std::ostringstream out;
    const auto size = image_size(base);
    out << "{\"ecs_startup\":{\"enabled\":true,\"event_hash\":" << ecs_detail::initial_event_hash
        << ",\"client_valid\":" << valid_client << ",\"context\":";
    write_context(out, live);
    out << ",\"capabilities\":{\"status\":\"" << capabilities.status << "\",\"coherent\":" << capabilities.coherent
        << ",\"count\":" << capabilities.count << ",\"data_model\":";
    if (capabilities.coherent) out << capabilities.data_model; else out << "null";
    out << ",\"schematics\":";
    if (capabilities.coherent) out << capabilities.schematics; else out << "null";
    out << "},\"level_class_reference\":{\"status\":\"" << level_class.status << "\",\"coherent\":" << level_class.coherent
        << ",\"opaque_reference_present\":";
    if (level_class.coherent) out << level_class.present; else out << "null";
    out << '}';
    out << ",\"profile_settings_query\":{\"status\":\"" << profile_query.status << "\",\"coherent\":" << profile_query.coherent
        << ",\"manager_present\":";
    if (profile_query.coherent) out << (profile_query.manager != 0); else out << "null";
    out << ",\"group_zero_query_present\":";
    if (profile_query.coherent) out << (profile_query.object != 0); else out << "null";
    out << ",\"query_vtable_rva\":";
    if (profile_query.coherent && profile_query.vtable >= base && profile_query.vtable - base < size)
        out << profile_query.vtable - base;
    else out << "null";
    out << ",\"query_function_rva\":";
    if (profile_query.coherent && profile_query.function >= base && profile_query.function - base < size)
        out << profile_query.function - base;
    else out << "null";
    out << ",\"native_request\":{\"status\":\"" << native_query.status << "\",\"coherent\":" << native_query.coherent;
    if (native_query.coherent) out << ",\"completed\":" << unsigned{native_query.completed}
        << ",\"storage_present\":" << native_query.storage_present
        << ",\"list_request_id\":" << native_query.list_request << ",\"read_request_id\":" << native_query.read_request
        << ",\"load_state\":" << native_query.load_state << ",\"save_state\":" << native_query.save_state
        << ",\"deferred_load\":" << unsigned{native_query.deferred_load} << ",\"deferred_save\":" << unsigned{native_query.deferred_save};
    out << "}}";
    out << ",\"current_thread\":" << GetCurrentThreadId() << ",\"tls_available\":" << tls.available
        << ",\"tls_active\":" << unsigned{tls.active} << ",\"tls_context_matches_client\":" << (tls.available && tls.context == context)
        << ",\"bucket\":{\"status\":\"" << bucket.status << "\",\"coherent\":" << bucket.coherent
        << ",\"visited\":" << bucket.visited << ",\"matches\":" << bucket.matches
        << ",\"invalid_observers\":" << bucket.invalid_observers << ",\"observer_vtable_rvas\":[";
    for (std::uint32_t i = 0; i < bucket.matches; ++i) {
        if (i) out << ',';
        const auto table = bucket.receivers[i].vtable;
        if (table >= base && table - base < size) out << table - base;
        else out << "null";
    }
    out << "]},\"recipe_initializer\":{\"capture_state\":";
    auto& initial = initializer();
    const auto capture = initial.captured.load(std::memory_order_acquire);
    out << capture;
    if (capture == 2) {
        out << ",\"thread\":" << initial.thread << ",\"age_ms\":" << GetTickCount64() - initial.tick
            << ",\"coherent\":" << initial.coherent << ",\"tls_available\":" << initial.tls.available
            << ",\"tls_active\":" << unsigned{initial.tls.active} << ",\"context\":";
        write_context(out, initial.context);
        out << ",\"context_matches_client\":" << (initial.context.available && live.available && initial.context.context == live.context)
            << ",\"queue_matches_client\":" << (initial.context.available && live.available && initial.context.queue == live.queue);
    }
    out << "}}}";
    return out.str();
}
}
