#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/runtime_internal.h"
#include "local_graphics_controls.h"
#include "Extension/World/local_world_controls.h"
#include "Engine/Game/Build/20260929/visual_environment.h"

namespace dingosdk::profile_runtime {
// Renderer settings and native filmic property contracts: analysis/graphics-controls.md.

namespace {
bool writable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION m{};
    return VirtualQuery(reinterpret_cast<void*>(address), &m, sizeof(m)) && m.State == MEM_COMMIT &&
        !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
}

GraphicsRuntime& graphics_runtime() { static auto* r = new GraphicsRuntime; return *r; }

std::uint32_t graphics_video_mask(const GraphicsControls& choices) {
    constexpr std::array<unsigned, 3> bits{2, 1, 0x10};
    std::uint32_t mask{};
    for (unsigned i = 0; i < bits.size(); ++i) if (choices.effects[i] == 0) mask |= bits[i];
    return mask;
}

GraphicsControls graphics_effective_choices(const GraphicsRuntime& r) {
    auto choices = r.model.choices;
    if (r.vr_override) choices.effects[1] = choices.effects[2] = 0; // vignette, chromatic aberration
    return choices;
}

void set_graphics_vr_override(bool active) noexcept {
    auto& r = graphics_runtime();
    std::lock_guard lock(r.mutex);
    if (r.vr_override == active) return;
    r.vr_override = active;
    r.disabled_video_effects.store(graphics_video_mask(graphics_effective_choices(r)), std::memory_order_release);
    r.last_update = 0; // Re-apply on the next update.
}

void graphics_video_setup(const std::uintptr_t* params, std::uintptr_t context, std::uintptr_t output) {
    auto& r = graphics_runtime();
    std::array<std::uintptr_t, 7> copy{};
    std::uint32_t mask{};
    bool changed = false;
    {
        PreserveError preserve;
        // This pass is freshly constructed by DingoVideoFilterRenderPassModule.
        // Its mask is owned by this invocation, rather than a shared VE asset.
        std::uintptr_t pass{}, vt{}, module{}, module_vt{};
        std::uint32_t cached_mask{};
        if (r.active.load(std::memory_order_acquire) && output >= 0x860 &&
            read(context + 0x18, pass) && pass == output - 0x860 &&
            read(pass, vt) && vt == local_runtime().base + video_filter_pass_vtable &&
            read(pass + 0x898, module) && read(module, module_vt) && module_vt == local_runtime().base + video_filter_module_vtable &&
            read(reinterpret_cast<std::uintptr_t>(params), copy) && read(copy[5], mask) &&
            read(pass + 0x890, cached_mask) && mask == cached_mask && (mask & ~0x117u) == 0) {
            r.video_observed_at.store(GetTickCount64(), std::memory_order_release);
            const auto filtered = mask & ~r.disabled_video_effects.load(std::memory_order_acquire);
            if (filtered != mask) {
                SIZE_T written{};
                if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(pass + 0x890), &filtered, sizeof(filtered), &written) && written == sizeof(filtered)) {
                    mask = filtered;
                    copy[5] = reinterpret_cast<std::uintptr_t>(&mask);
                    changed = true;
                }
            }
        }
    }
    r.video_setup(changed ? copy.data() : params, context, output);
}

bool graphics_filmic_identity(const GraphicsFilmic& n) {
    const auto base = local_runtime().base;
    std::uintptr_t vt{}, data{}, type{}, collection{}, parent{}, parent_data{};
    return read(n.component, vt) && vt == base + filmic_vtable &&
        read(n.component + 8, data) && data == n.data && read(data + 8, type) && type == base + filmic_data_type &&
        read(n.component + 0x18, collection) && read(collection, parent) && parent == n.parent &&
        read(parent, vt) && vt == base + addr::visual_environment::entity_vtable && read(parent + 0x48, parent_data) &&
        read(parent_data + 8, type) && type == base + addr::visual_environment::entity_data_type;
}

std::uintptr_t graphics_filmic_construct(std::uintptr_t factory, std::uintptr_t info) {
    auto& r = graphics_runtime();
    const auto result = r.construct(factory, info);
    PreserveError preserve;
    if (r.active.load(std::memory_order_acquire)) {
        try {
            GraphicsFilmic n; n.component = result;
            std::uintptr_t collection{};
            if (read(result + 8, n.data) && read(result + 0x18, collection) && read(collection, n.parent) && graphics_filmic_identity(n)) {
                std::lock_guard lock(r.mutex);
                if (r.nodes.size() < 4096) r.nodes[result] = n;
                r.last_update = 0;
            }
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::graphics, "{\"event\":\"graphics_component_capture_failed\"}"); }
    }
    return result;
}

std::uintptr_t graphics_filmic_destroy(std::uintptr_t component, unsigned flags) {
    auto& r = graphics_runtime();
    { PreserveError preserve; std::lock_guard lock(r.mutex); r.nodes.erase(component); }
    return r.destroy(component, flags);
}

bool graphics_write_byte(std::uintptr_t address, std::uint8_t value) {
    if (!writable(address)) return false;
    SIZE_T written{}; std::uint8_t after{};
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, 1, &written) &&
        written == 1 && read(address, after) && after == value;
}

bool graphics_settings_identity(std::uintptr_t object) {
    const auto base = local_runtime().base;
    std::uintptr_t vt{}, type{};
    return read(object, vt) && vt >= base && vt < base + supported_build::game_image_size &&
        read(object + 8, type) && type == base + post_process_settings_type;
}

bool graphics_settings_update() {
    auto& r = graphics_runtime(); const auto base = local_runtime().base;
    std::uintptr_t manager{}, buckets{}; unsigned count{};
    if (!read(base + addr::engine::settings_manager, manager) || !read(manager + 0xc8, buckets) || !buckets ||
        !read(manager + 0xd0, count) || !count || count > 0x100000) return false;
    const auto object = r.lookup(manager, reinterpret_cast<const void*>(base + post_process_settings_type));
    if (!graphics_settings_identity(object)) return false;
    constexpr std::array<unsigned, 2> offsets{0x31e, 0x352};
    bool ok = true, changed = false;
    // The native renderer settings service caches PostProcess copies by
    // DdcDirtyVersion. Changing the Boolean alone leaves those copies stale
    // until a camera/pause transition rebuilds them.
    const auto version = object + 0x1a8;
    // The settings object only changes on a reload: its version word is checked once per object
    // rather than with a VirtualQuery on every update.
    if (object != r.settings) {
        if (!writable(version)) return false;
        r.settings = object; r.settings_fields = {};
    }
    for (unsigned i = 0; i < offsets.size(); ++i) {
        auto& lease = r.settings_fields[i]; std::uint8_t current{};
        if (!read(object + offsets[i], current) || current > 1) { ok = false; continue; }
        if (lease.owned && current != lease.applied) lease = {};
        if (!lease.owned) lease.original = current;
        const int choice = graphics_effective_choices(r).effects[i];
        const auto wanted = choice < 0 ? lease.original : static_cast<std::uint8_t>(choice);
        if (current != wanted && (!graphics_settings_identity(object) || !graphics_write_byte(object + offsets[i], wanted))) {
            ok = false; continue;
        }
        changed |= current != wanted;
        lease.applied = wanted; lease.owned = choice >= 0;
        r.model.ready[i] = true; r.model.enabled[i] = wanted != 0;
    }
    if (changed) InterlockedIncrement(reinterpret_cast<volatile LONG*>(version));
    return ok;
}

bool graphics_filmic_update(GraphicsFilmic& n, unsigned field) {
    auto& r = graphics_runtime(); auto& lease = n.fields[field];
    const unsigned index = field + 1;
    const unsigned offset = field == 0 ? 0x97 : 0x95;
    const unsigned bit = field == 0 ? 0x100 : 2;
    const std::uint32_t hash = field == 0 ? 0x028a3397 : 0x2ae38bab;
    std::uint8_t current{}; std::uint32_t mask{};
    if (!read(n.component + offset, current) || current > 1 || !read(n.component + 0x5c, mask)) return false;
    if (lease.owned && current != lease.applied) lease = {};
    if (!lease.owned) { lease.original = current; lease.original_flag = (mask & bit) != 0; }
    const int choice = graphics_effective_choices(r).effects[index];
    const auto desired = choice < 0 ? lease.original : static_cast<std::uint8_t>(choice);
    if (choice >= 0 ? !lease.owned || current != desired : lease.owned) {
        const EnvironmentChange change{hash, 0, &desired};
        r.property(n.component, &change); // Includes the renderer ECS notification.
        if (choice < 0) {
            if (!read(n.component + 0x5c, mask)) return false;
            mask = lease.original_flag ? mask | bit : mask & ~bit;
            const EnvironmentChange flags{0xd421d5af, 0, &mask};
            r.property(n.component, &flags);
        }
        if (!read(n.component + offset, current) || current != desired) return false;
        lease.applied = desired; lease.owned = choice >= 0;
    }
    r.model.ready[index] = true;
    r.model.enabled[index] = r.model.enabled[index] || current != 0;
    return true;
}

void update_graphics_controls() {
    auto& r = graphics_runtime(); std::lock_guard lock(r.mutex);
    if (!r.active.load(std::memory_order_acquire)) return;
    if (!r.thread) r.thread = GetCurrentThreadId();
    if (r.thread != GetCurrentThreadId()) return;
    const auto now = GetTickCount64();
    if (r.last_update && now - r.last_update < 250) return;
    r.last_update = now;
    r.model.ready = {}; r.model.enabled = {}; r.model.filmic_components = 0;
    bool ok = graphics_settings_update();
    for (auto& [_, n] : r.nodes) {
        std::uint16_t handle{};
        if (!graphics_filmic_identity(n) || !read(n.component + 0x30, handle) || !handle) continue;
        ++r.model.filmic_components;
        for (unsigned i = 0; i < 2; ++i) ok &= graphics_filmic_update(n, i);
    }
    const bool video = now - r.video_observed_at.load(std::memory_order_acquire) < 2000;
    if (video) r.model.ready[2] = true;
    r.model.status = !ok ? "Waiting for renderer settings." : !r.model.filmic_components && !video ?
        "Graphics saved. Chromatic aberration will apply when a camera effect loads." : "Graphics controls active.";
}

void start_graphics_controls() noexcept {
    auto& r = graphics_runtime(); const auto base = local_runtime().base;
    std::vector<void*> created;
    try {
        r.model.choices = profile::graphics_controls(*local_runtime().store->shared_snapshot());
        r.disabled_video_effects = graphics_video_mask(graphics_effective_choices(r));
        for (const auto& site : graphics_sites) {
            std::array<unsigned char, 32> bytes{};
            if (!read(base + site.rva, bytes) || bytes != site.bytes) throw std::runtime_error("Graphics fingerprint mismatch");
        }
        // Independent reflected offsets and Boolean types for the global renderer switches.
        constexpr std::array<std::array<std::uintptr_t, 4>, 3> fields{{
            {film_grain_enable_field, 0xa976a4ad, 0x31e, addr::engine::bool_type},
            {vignette_enable_field, 0x3b5256dd, 0x352, addr::engine::bool_type},
            {ddc_dirty_version_field, 0xe676bb9a, 0x1a8, addr::engine::uint32_type}}}; // DdcDirtyVersion: Uint32
        for (const auto& f : fields) {
            std::array<std::uintptr_t, 3> m{};
            if (!read(base + f[0], m) || m[0] != f[1] || m[1] != f[2] || m[2] != base + f[3])
                throw std::runtime_error("Graphics metadata mismatch");
        }
        std::uintptr_t destroy{}, property{};
        if (!read(base + filmic_vtable + 8, destroy) || destroy != base + graphics_sites[1].rva ||
            !read(base + filmic_vtable + 0x18, property) || property != base + graphics_sites[2].rva)
            throw std::runtime_error("Graphics vtable mismatch");
        r.property = reinterpret_cast<GraphicsRuntime::Property>(property);
        r.lookup = reinterpret_cast<GraphicsRuntime::Lookup>(base + graphics_sites[3].rva);
        const auto hook = [&](unsigned i, auto detour, auto& original) {
            auto* target = reinterpret_cast<void*>(base + graphics_sites[i].rva);
            if (hook_prepare(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) != HookOk)
                throw std::runtime_error("Cannot prepare graphics controls");
            created.push_back(target);
        };
        hook(0, &graphics_filmic_construct, r.construct); hook(1, &graphics_filmic_destroy, r.destroy);
        hook(4, &graphics_video_setup, r.video_setup);
        for (auto* target : created) if (hook_enable(target) != HookOk) throw std::runtime_error("Cannot enable graphics controls");
        r.active.store(true, std::memory_order_release);
        dingosdk::logging::event(dingosdk::logging::Channel::graphics, "{\"event\":\"graphics_controls_initialized\",\"active\":true}");
    } catch (...) {
        for (auto* target : created) hook_disable(target);
        r.model.status = "Graphics controls are unavailable in this build.";
        dingosdk::logging::event(dingosdk::logging::Channel::graphics, "{\"event\":\"graphics_controls_initialized\",\"active\":false}");
    }
}
}