#include "plugin_lights.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/light_tables.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>
#include <mutex>

namespace dingosdk::plugin_lights {
namespace {
namespace lights = addr::light_tables;

struct alignas(16) Info {
    std::array<unsigned char, lights::info_size> bytes{};
    template <class T> void put(std::size_t at, T value) { std::memcpy(bytes.data() + at, &value, sizeof(T)); }
};
// The renderer's description of a light to make (built in the game by 0x4c71f40, then shown and given no parent).
struct alignas(16) Create {
    const char* name{};
    std::uint8_t kind{};
    std::array<std::uint8_t, 7> pad0{};
    std::array<float, 16> transform{};
    std::uint8_t shown{};   // +0x50: 1, or the light is hidden
    std::uint8_t flag{};    // +0x51: world_light
    std::array<std::uint8_t, 2> pad1{};
    std::uint32_t parent{}; // +0x54: the spatial node it follows (none)
    std::uint8_t pad2{};
    std::array<std::uint8_t, 7> pad3{};
    void* extra{};          // +0x60: 12 bytes the game fills with nothing on this build
    void* info{};           // +0x68
};
static_assert(sizeof(Create) == 0x70 && offsetof(Create, transform) == 0x10 && offsetof(Create, shown) == 0x50 && offsetof(Create, parent) == 0x54 &&
              offsetof(Create, extra) == 0x60 && offsetof(Create, info) == 0x68);
struct alignas(16) Extra {
    std::array<std::uint8_t, 16> bytes{};
};
using InitFn = void(__fastcall*)(void* info);
using CreateFn = std::uint32_t(__fastcall*)(void* renderer, Create* description);
using TransformFn = void(__fastcall*)(void* renderer, std::uint32_t id, const float* transform);
using UpdateFn = void(__fastcall*)(void* renderer, std::uint32_t id, void* info);
using DestroyFn = void(__fastcall*)(void* renderer, std::uint32_t id);
using ReleaseFn = void(__fastcall*)(void* what);

struct Made {
    std::uintptr_t renderer{};
    std::uint32_t id{};
    Light light;
};
struct State {
    std::mutex mutex;
    int checked{}; // 0 not yet, 1 known, -1 not this build's
    std::map<std::pair<std::string, std::string>, Made, std::less<>> made;
    std::uint32_t failures{};
};
State& state() {
    static auto* s = new State;
    return *s;
}

// The game's calls, each guarded: a fault fails the call, never the game. (No objects to unwind in them.)
bool call_init(std::uintptr_t base, Info* info) noexcept {
    __try {
        reinterpret_cast<InitFn>(base + lights::fn_info_init)(info->bytes.data());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void call_release(std::uintptr_t base, Info* info) noexcept {
    __try {
        reinterpret_cast<ReleaseFn>(base + lights::fn_release_handle)(info->bytes.data() + lights::info_handle_a);
        reinterpret_cast<ReleaseFn>(base + lights::fn_release_handle)(info->bytes.data() + lights::info_handle_b);
        reinterpret_cast<ReleaseFn>(base + lights::fn_release_ref)(info->bytes.data() + lights::info_reference);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
std::uint32_t call_create(std::uintptr_t base, std::uintptr_t renderer, Create* description) noexcept {
    __try {
        return reinterpret_cast<CreateFn>(base + lights::fn_create)(reinterpret_cast<void*>(renderer), description);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool call_transform(std::uintptr_t base, std::uintptr_t renderer, std::uint32_t id, const float* transform) noexcept {
    __try {
        reinterpret_cast<TransformFn>(base + lights::fn_transform)(reinterpret_cast<void*>(renderer), id, transform);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_update(std::uintptr_t base, std::uintptr_t renderer, std::uint32_t id, Info* info) noexcept {
    __try {
        reinterpret_cast<UpdateFn>(base + lights::fn_update)(reinterpret_cast<void*>(renderer), id, info->bytes.data());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_destroy(std::uintptr_t base, std::uintptr_t renderer, std::uint32_t id) noexcept {
    __try {
        reinterpret_cast<DestroyFn>(base + lights::fn_destroy)(reinterpret_cast<void*>(renderer), id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The renderer, when it is one of the class known (none while the game has none).
std::uintptr_t renderer_now(std::uintptr_t base) noexcept {
    std::uintptr_t renderer{}, vtable{};
    if (!memory::peek(base + lights::renderer, renderer) || !renderer || !memory::peek(renderer, vtable) || vtable != base + lights::renderer_vtable)
        return 0;
    return renderer;
}
// Whether this build's light renderer is the one known: the info's defaults and the spot lights' change by their first
// bytes, and the renderer's four slots by the functions they hold (read from its class, which needs no renderer made).
bool check(State& s, std::uintptr_t base) noexcept {
    if (s.checked) return s.checked > 0;
    bool ok = true;
    std::string why;
    std::array<unsigned char, lights::info_init_prefix.size()> init{};
    std::array<unsigned char, lights::update_prefix.size()> update{};
    if (!memory::peek(base + lights::fn_info_init, init) || init != lights::info_init_prefix) ok = false, why = "the info's defaults";
    if (ok && (!memory::peek(base + lights::fn_update, update) || update != lights::update_prefix)) ok = false, why = "its spot lights' change";
    const std::array<std::pair<std::uintptr_t, std::uintptr_t>, 4> slots{{{lights::slot_destroy, lights::fn_destroy}, {lights::slot_create, lights::fn_create},
                                                                          {lights::slot_transform, lights::fn_transform}, {lights::slot_update, lights::fn_update}}};
    for (const auto& [slot, fn] : slots) {
        std::uintptr_t at{};
        if (ok && (!memory::peek(base + lights::renderer_vtable + slot, at) || at != base + fn)) ok = false, why = std::format("its slot +0x{:x}", slot);
    }
    s.checked = ok ? 1 : -1;
    if (!ok)
        logging::log(logging::Level::warning, logging::Channel::graphics,
                     "Plugin lights: this game build's light renderer is not the one known ({} differs); plugins make no lights.", why);
    return ok;
}
void fill(Info& info, const Light& w) {
    info.put(lights::info_colour, w.colour[0]);
    info.put(lights::info_colour + 4, w.colour[1]);
    info.put(lights::info_colour + 8, w.colour[2]);
    info.put(lights::info_intensity, std::max(w.lumens, 0.0f));
    info.put(lights::info_unit, std::uint32_t{0}); // lumens
    info.put(lights::info_radius, std::clamp(w.range, 0.5f, 200.0f));
    const float beam = std::clamp(w.beam, 1.0f, 179.0f);
    info.put(lights::info_outer, beam);
    info.put(lights::info_inner, std::clamp(w.core, 0.0f, beam));
    info.put(lights::info_cast_shadows, w.shadows ? lights::shadows_from_low : lights::shadows_never);
    info.put(lights::info_shadows_on, static_cast<std::uint8_t>(w.shadows ? 1 : 0));
    info.put(lights::info_shadow_near, 0.05f);
    info.put(lights::info_enabled, static_cast<std::uint8_t>(1));
    info.put(lights::info_mobility, std::uint32_t{0}); // dynamic: it moves
}
bool same_colour(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::abs(a[0] - b[0]) < 1e-4f && std::abs(a[1] - b[1]) < 1e-4f && std::abs(a[2] - b[2]) < 1e-4f;
}
// Takes a light away, from the renderer it was made in (one made since never had it). Under the mutex.
void destroy(std::uintptr_t base, const Made& m) noexcept {
    if (const auto renderer = renderer_now(base); renderer && renderer == m.renderer) (void)call_destroy(base, renderer, m.id);
}
} // namespace

std::string set(std::uintptr_t base, std::string_view owner, std::string_view name, std::optional<Light> light) noexcept {
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        const auto key = std::pair{std::string(owner), std::string(name)};
        auto found = s.made.find(key);
        if (!light) {
            if (found != s.made.end()) {
                destroy(base, found->second);
                s.made.erase(found);
            }
            return {};
        }
        if (!base || !check(s, base)) return "this game build's light renderer is not the one known";
        const auto renderer = renderer_now(base);
        if (!renderer) return "the game has no light renderer now (no level)";
        if (found != s.made.end() && found->second.renderer != renderer) {
            s.made.erase(found); // made in a renderer that is gone
            found = s.made.end();
        }
        if (found == s.made.end()) {
            std::size_t own = 0;
            for (const auto& [k, m] : s.made) own += k.first == owner;
            if (own >= most_each) return std::format("a plugin has at most {} lights", most_each);
            if (s.made.size() >= most_lights) return std::format("plugins have at most {} lights together", most_lights);
            if (s.failures >= 8) return "the game's renderer would not make lights";
            Info info;
            Extra extra;
            if (!call_init(base, &info)) {
                ++s.failures;
                return "the light's description would not start";
            }
            info.put(lights::info_marks_a, lights::made_marks_a);
            info.put(lights::info_marks_b, lights::made_marks_b);
            info.put(lights::info_marks_c, lights::made_marks_c);
            fill(info, *light);
            Create description;
            description.name = "ReSkate plugin light";
            description.kind = lights::kind_pbr_spot;
            description.transform = light->transform;
            description.shown = 1;
            description.flag = lights::world_light;
            description.extra = &extra;
            description.info = info.bytes.data();
            const auto id = call_create(base, renderer, &description);
            call_release(base, &info);
            if (!id || (id >> 29) != lights::kind_pbr_spot) {
                ++s.failures;
                logging::log(logging::Level::warning, logging::Channel::graphics, "Plugin lights: the renderer would not make {}'s light {} (0x{:08x}).", owner,
                             name, id);
                return "the game's renderer would not make it";
            }
            s.made.emplace(key, Made{renderer, id, *light});
            return {};
        }
        // Changed: only what changed, marked; moved where it moved.
        auto& m = found->second;
        Info info;
        if (call_init(base, &info)) {
            fill(info, *light);
            std::uint32_t a = 0, b = 0;
            if (!same_colour(light->colour, m.light.colour)) a |= lights::mark_colour;
            if (std::abs(light->lumens - m.light.lumens) > 0.01f) a |= lights::mark_intensity | lights::mark_unit;
            if (std::abs(light->range - m.light.range) > 1e-3f) a |= lights::mark_radius;
            if (std::abs(light->beam - m.light.beam) > 1e-3f || std::abs(light->core - m.light.core) > 1e-3f) a |= lights::mark_outer | lights::mark_inner;
            if (light->shadows != m.light.shadows) a |= lights::mark_cast_shadows, b |= lights::mark_b_shadows_on | lights::mark_b_shadow_near;
            if (a || b) {
                info.put(lights::info_marks_a, a);
                info.put(lights::info_marks_b, b);
                info.put(lights::info_marks_c, std::uint8_t{0});
                if (call_update(base, renderer, m.id, &info)) {
                    const auto transform = m.light.transform;
                    m.light = *light;
                    m.light.transform = transform;
                }
            }
            call_release(base, &info);
        }
        if (light->transform != m.light.transform && call_transform(base, renderer, m.id, light->transform.data())) m.light.transform = light->transform;
        return {};
    } catch (...) {
        return "it could not be made";
    }
}

void clear(std::uintptr_t base, std::string_view owner) noexcept {
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        for (auto it = s.made.begin(); it != s.made.end();) {
            if (it->first.first == owner) {
                destroy(base, it->second);
                it = s.made.erase(it);
            } else {
                ++it;
            }
        }
    } catch (...) {}
}

void tick(std::uintptr_t base) noexcept {
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.made.empty()) return;
        const auto renderer = base ? renderer_now(base) : 0;
        std::erase_if(s.made, [&](const auto& m) { return m.second.renderer != renderer; });
    } catch (...) {}
}
} // namespace dingosdk::plugin_lights
