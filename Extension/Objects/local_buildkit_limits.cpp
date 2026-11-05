#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/buildkit_limits.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "local_buildkit_limits.h"
#include "network_object_runtime.h"
#include "Extension/Profile/runtime_internal.h"

namespace dingosdk::profile_runtime {
// Offline building configuration, loaded before publishing local-profile readiness.

BuildKitLimits buildkit_limits_from(const profile::Snapshot& snapshot) {
    BuildKitLimits result;
    const auto section = snapshot.extensions.find("object_dropper");
    if (section == snapshot.extensions.end() || !section->contains("limits")) return result;
    const auto& limits = section->at("limits");
    if (!limits.value("enabled", false)) return result;
    result.max_objects = limits.value("max_objects", 0U);
    result.radius = limits.value("radius_metres", 0.0f);
    result.enabled = result.max_objects >= 1 && result.max_objects <= 1024 &&
        std::isfinite(result.radius) && result.radius >= 1 && result.radius <= 5000;
    return result;
}

BuildKitLimitsRuntime& buildkit_limits_runtime() { static BuildKitLimitsRuntime value; return value; }

bool buildkit_limits_active() {
    return local_runtime().active.load(std::memory_order_acquire) && buildkit_limits_runtime().config.enabled;
}

bool buildkit_settings_identity(std::uintptr_t object) {
    const auto base = local_runtime().base;
    std::uintptr_t vtable{}, type{}, metadata{}, fields{};
    std::uint16_t count{}, size{};
    std::array<unsigned char, 12> identity{};
    constexpr std::array<unsigned char, 12> expected_identity{
        0x73,0x2c,0x3d,0x5e,0xb9,0xc7,0x42,0x85,0x37,0x6b,0x90,0x5a};
    std::uint32_t layout{};
    if (!read(object, vtable) || vtable != base + addr::buildkit_limits::object_persistence_settings_vtable ||
        !read(object + 8, type) || type != base + addr::buildkit_limits::object_persistence_settings_type ||
        !read(type, metadata) || !read(metadata + 6, size) || size != 0xa0 ||
        !read(metadata + 0xc, identity) || identity != expected_identity ||
        !read(metadata + 0x2c, layout) || layout != 0x7619daa1 ||
        !read(metadata + 0x2a, count) || count != 41 ||
        !read(metadata + 0x38, fields)) return false;
    // Validate the exact class GUID/layout and both indexed native field tokens.
    // The separate runtime name-descriptor table is not a stable build contract.
    for (const auto index : {5U, 7U}) {
        std::uintptr_t offset{}, field_type{};
        std::uint64_t token{};
        if (!read(fields + index * 0x18, token) || token != (index == 5 ? 0xd7588b55ULL : 0x045e6e4cULL) ||
            !read(fields + index * 0x18 + 8, offset) || offset != (index == 5 ? 0x84 : 0x78) ||
            !read(fields + index * 0x18 + 16, field_type) ||
            field_type != base + (index == 5 ? addr::engine::uint32_type : addr::engine::float_type))
            return false;
    }
    return true;
}

bool apply_buildkit_limits(std::uintptr_t object) {
    if (!buildkit_limits_active() || !buildkit_settings_identity(object)) return false;
    const auto& config = buildkit_limits_runtime().config;
    return write_buildkit_setting(object + 0x84, config.max_objects + network_object_extra_budget()) &&
        write_buildkit_setting(object + 0x78, config.radius);
}

void update_buildkit_limits() {
    if (!buildkit_limits_active()) return;
    auto& runtime = buildkit_limits_runtime();
    std::uintptr_t manager{}, buckets{};
    std::uint32_t count{};
    if (!read(local_runtime().base + addr::engine::settings_manager, manager) || !manager ||
        !read(manager + 0xc8, buckets) || !buckets ||
        !read(manager + 0xd0, count) || !count || count > 0x100000) return;
    const auto object = runtime.lookup(manager, reinterpret_cast<const void*>(
        local_runtime().base + addr::buildkit_limits::object_persistence_settings_type));
    if (!apply_buildkit_limits(object)) {
        if (object && runtime.rejected_settings != object) {
            runtime.rejected_settings = object;
            dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_buildkit_limits_rejected\",\"reason\":\"settings_layout_or_write\"}");
        }
        return;
    }
    if (runtime.logged_settings == object) return;
    runtime.logged_settings = object;
    std::ostringstream event;
    event << "{\"event\":\"local_buildkit_limits_applied\",\"max_objects\":" << runtime.config.max_objects
          << ",\"radius_metres\":" << runtime.config.radius << '}';
    dingosdk::logging::event(dingosdk::logging::Channel::objects, event.str().c_str());
}

void buildkit_grabber_settings_hook(std::uintptr_t system, const void* settings) {
    auto& runtime = buildkit_limits_runtime();
    // Current native input is 0x34 bytes; its radius moved from +0x18 to +0x04.
    alignas(16) std::array<std::byte, 0x34> copy{};
    const void* input = settings;
    {
        PreserveError preserve;
        if (buildkit_limits_active() && read(reinterpret_cast<std::uintptr_t>(settings), copy)) {
            float radius{}; std::memcpy(&radius, copy.data() + 0x04, sizeof(radius));
            // The native clamp treats nonpositive distances as unlimited. Keep that behavior.
            if (std::isfinite(radius) && radius > 0 && radius < runtime.config.radius) {
                std::memcpy(copy.data() + 0x04, &runtime.config.radius, sizeof(radius));
                input = copy.data();
                if (!runtime.logged_cursor.exchange(true)) {
                    // Logging is optional and must never prevent native placement.
                    try {
                        std::ostringstream event;
                        event << "{\"event\":\"local_buildkit_cursor_radius_applied\",\"previous_metres\":" << radius
                              << ",\"radius_metres\":" << runtime.config.radius << '}';
                        dingosdk::logging::event(dingosdk::logging::Channel::objects, event.str().c_str());
                    } catch (...) {}
                }
            }
        }
    }
    runtime.grabber_settings(system, input);
}
}
