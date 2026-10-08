#include "graphics_tuning.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Scripting/lua_startup.h"
#include "Extension/Settings/engine_tweaks.h"
#include "Extension/Settings/named_settings.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace dingosdk::graphics_tuning {
namespace {
// The order of this table is the order of the menu rows and of the Index enum below.
enum Index : std::size_t {
    dynamic_resolution, shader_prime,
    lighting, shadow, cascades, scatter_shadows,
    scale, dof, bloom, ao, effects,
    vegetation, terrain_displacement,
    cloth_jobs, count
};
static_assert(count <= 32, "the pending-save mask is 32 bits");

// Columns: key, label, hint, group, kind, stock (the game's), initial (ReSkate's default), low-spec,
// minimum, maximum, slider text, slider text at stock. Hints say what changes on screen and what it
// buys, in plain words.
using enum Group;
constexpr std::array<Option, count> table{{
    // `perf spikes` on an AMD APU with FSR: while the engine's regulator moves the resolution (it aims
    // for 60 fps), the driver keeps compiling shaders; off, the hitch bursts were gone (6 vs ~170).
    // Off costs frames: the regulator had been rendering below FSR's own resolution.
    {"DynamicResolution", "Dynamic resolution",
     "Lets the game lower the resolution on the fly. On weak GPUs every change causes a stutter; off keeps "
     "the image steady for a few fps less.",
     smoothness, Kind::toggle, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, nullptr, nullptr},
    // `perf spikes`: the remaining hitches were the driver compiling pipelines on the render thread.
    {"ShaderPrime", "Prepare shaders while loading",
     "Builds the shaders the game has seen before during loading screens instead of mid-run. "
     "Fewer stutters, slightly longer loading. Takes full effect after a restart.",
     smoothness, Kind::toggle, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, nullptr, nullptr},
    // At ~25 fps a 60/s throttle never triggers; 15/s measurably lowered CPU load (lighting still smooth).
    {"LightingRate", "Lighting updates",
     "How often bounced light is recalculated. Fewer updates free up the CPU; lighting may lag a moment "
     "behind quick changes.",
     lighting_shadows, Kind::slider, 0.0f, 0.0f, 15.0f, 0.0f, 60.0f, "%.0f per second", "Every frame (game default)"},
    {"ShadowResolution", "Far shadow detail",
     "Sharpness of shadows in the distance. Lower saves a lot of video memory and steadies frame times.",
     lighting_shadows, Kind::slider, 6400.0f, 6400.0f, 2048.0f, 1024.0f, 6400.0f, "%.0f", "%.0f (game default)"},
    {"ShadowCascades", "Shadow layers",
     "How many shadow maps cover near to far. Fewer means less work per frame; far shadows get blurrier.",
     lighting_shadows, Kind::slider, 3.0f, 3.0f, 2.0f, 1.0f, 3.0f, "%.0f", "%.0f (game default)"},
    {"ScatterShadows", "Shadows of small props",
     "Shadows from rocks, debris and similar small objects.",
     lighting_shadows, Kind::toggle, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, nullptr, nullptr},
    // Measured as a medium gain early on, but it softens the image; the preset leaves it to the player.
    {"ResolutionScale", "Resolution scale",
     "Draws fewer pixels and scales the picture up, on top of FSR. Faster but softer; 85-90% is a mild step.",
     image_effects, Kind::slider, 100.0f, 100.0f, 100.0f, 50.0f, 100.0f, "%.0f%%", "%.0f%% (game default)"},
    {"DepthOfField", "Depth of field", "Blur on things far away or very close to the camera.",
     image_effects, Kind::toggle, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, nullptr, nullptr},
    {"Bloom", "Bloom", "Glow around bright lights and the sky.",
     image_effects, Kind::toggle, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, nullptr, nullptr},
    {"AmbientOcclusion", "Ambient occlusion", "Soft shading in corners and under objects.",
     image_effects, Kind::toggle, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, nullptr, nullptr},
    {"Effects", "Particles", "Sparks, dust and similar. Barely affects speed, so the preset keeps them.",
     image_effects, Kind::toggle, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, nullptr, nullptr},
    {"Vegetation", "Grass and plants", "Off removes plants from the world entirely.",
     world, Kind::toggle, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, nullptr, nullptr},
    {"TerrainDisplacement", "Ground detail",
     "Extra bumps and depth on ground surfaces. Off flattens them slightly.",
     world, Kind::toggle, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, nullptr, nullptr},
    {"ClothJobs", "Clothing physics jobs",
     "How many pieces clothing physics is split into. The game's 32 suits big CPUs; 4 suits a 4-core laptop.",
     cpu, Kind::slider, 32.0f, 32.0f, 4.0f, 1.0f, 32.0f, "%.0f", "%.0f (game default)"},
}};

struct Write { const char* name; std::string value; };
struct Writes { std::array<Write, 2> items; std::size_t size{}; };

std::string whole(float v) { return std::to_string(std::lround(v)); }

// The engine settings behind one option at value `v` (lighting goes through engine_tweaks instead).
// Toggles are only written away from the game's own value: "0" for ones the game has on, "1" for
// the one it has off (shader_prime).
Writes engine_writes(std::size_t index, float v) {
    switch (index) {
    case dynamic_resolution: return {{{{"Render.DynamicResolutionScaleEnable", "0"}}}, 1};
    case shadow: return {{{{"WorldRender.DistantShadowCacheResolution", whole(v)}}}, 1};
    // The scale only takes effect with the scaler enabled, so the scale is written first.
    case scale: return {{{{"DingoScalableRenderSettings.OutputResolutionScale", std::format("{:.2f}", v / 100.0f)},
                          {"DingoScalableRenderSettings.Enabled", "1"}}}, 2};
    case dof: return {{{{"PostProcess.DofForegroundEnable", "0"}}}, 1};
    case bloom: return {{{{"PostProcess.BloomEnable", "0"}}}, 1};
    case ao: return {{{{"PostProcess.DynamicAOEnable", "0"}}}, 1};
    case vegetation: return {{{{"Client.VegetationEnabled", "0"}}}, 1};
    case effects: return {{{{"Client.EffectsEnabled", "0"}}}, 1};
    case cascades: return {{{{"WorldRender.ShadowmapSliceCount", whole(v)}}}, 1};
    case scatter_shadows: return {{{{"MeshScattering.CastShadowsEnable", "0"}}}, 1};
    case terrain_displacement: return {{{{"VisualTerrain.DetailDisplacementEnable", "0"}}}, 1};
    case cloth_jobs: return {{{{"ClothSystem.ClientClothWorldThreadCount", whole(v)}}}, 1};
    case shader_prime: return {{{{"ShaderSystem.Dx12PrimeCachedPipelinesEnable", "1"},
                                 {"ShaderSystem.Dx12PrimeCachedPipelinesUseJobs", "1"}}}, 2};
    default: return {};
    }
}

struct State {
    State() { for (std::size_t i = 0; i < count; ++i) values[i].store(table[i].initial); }
    std::array<std::atomic<float>, count> values;
    std::atomic<bool> loaded{false};
    std::atomic<bool> changed{false};      // the menu changed a value: apply on the next tick
    std::atomic<std::uint32_t> unsaved{0}; // one bit per option changed since the last save
};
State& state() noexcept { static State s; return s; }

float clamp_value(std::size_t index, double v) noexcept {
    const auto& o = table[index];
    if (!std::isfinite(v)) return o.initial;
    if (o.kind == Kind::toggle) return v >= 0.5 ? 1.0f : 0.0f;
    return static_cast<float>(std::round(std::clamp(v, double(o.minimum), double(o.maximum))));
}

std::string saved_key(std::size_t index) { return std::string("Perf.") + table[index].key; }

void mark_changed(std::uint32_t mask) noexcept {
    auto& s = state();
    s.unsaved.fetch_or(mask);
    s.changed.store(true, std::memory_order_release);
}

void set_all(float Option::*field) noexcept {
    auto& s = state();
    if (!s.loaded.load()) return;
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const float v = table[i].*field;
        if (s.values[i].exchange(v) != v) mask |= 1u << i;
    }
    if (mask) mark_changed(mask);
}

bool all_at(float Option::*field) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (state().values[i].load(std::memory_order_relaxed) != table[i].*field) return false;
    return true;
}

// The profile is open before the game's first update tick (startup stops otherwise).
void load() noexcept {
    auto& s = state();
    std::string summary;
    for (std::size_t i = 0; i < count; ++i) {
        try {
            if (const auto saved = profile_runtime::local_value(saved_key(i)); saved && saved->is_number()) {
                s.values[i].store(clamp_value(i, saved->get<double>()));
                summary += std::format("{}{}={}", summary.empty() ? "" : ", ", table[i].key, s.values[i].load());
            }
        } catch (...) {}
    }
    s.loaded.store(true);
    logging::log(logging::Level::info, logging::Channel::graphics, "Performance settings: {}.",
        summary.empty() ? std::string("game defaults") : summary);
}

// Doubles only: a saved key keeps the type it was first written with. One transaction.
void save(std::uint32_t mask) noexcept {
    try {
        std::vector<std::pair<std::string, Json>> values;
        for (std::size_t i = 0; i < count; ++i)
            if (mask & (1u << i)) values.emplace_back(saved_key(i), Json(double(state().values[i].load())));
        profile_runtime::set_local_values(values);
    } catch (...) { /* Saving is best effort; the values already apply this session. */ }
}

// False when lighting could not be set yet (the GI hook is not ready), so it is tried again.
bool apply_lighting() {
    namespace et = engine_tweaks;
    static float applied = 0.0f; // the game's own (every frame) until something else is chosen
    static std::string last_error;
    const float wanted = state().values[lighting].load();
    if (wanted == applied) return true;
    if (!et::available(et::Tweak::gi)) return false;
    std::string error;
    if (et::set(et::Tweak::gi, wanted, error)) {
        applied = wanted;
        last_error.clear();
        logging::log(logging::Level::info, logging::Channel::graphics, "Performance settings: lighting updates {}.",
            wanted > 0 ? std::format("{}/s", wanted) : std::string("every frame"));
        return true;
    }
    if (error != last_error) {
        last_error = error;
        logging::log(logging::Level::warning, logging::Channel::graphics, "Performance settings: lighting not applied: {}", error);
    }
    return false;
}

// One pass over every option. False when something could not be applied yet. Each named setting is
// looked up by name in the engine's catalog (a few thousand entries), which is why this only runs
// when something can have changed rather than on a timer.
bool apply_all() {
    auto& s = state();
    // Whether an override of ours is in place, so a value set back to the game's own is restored once.
    static std::array<bool, count> held{};
    static std::array<std::string, count> last{};
    bool complete = true;
    for (std::size_t i = 0; i < count; ++i) {
        if (i == lighting) continue;
        const float v = s.values[i].load();
        const bool want = v != table[i].stock;
        if (!want && !held[i]) continue;
        bool failed = false;
        std::string replies;
        const auto writes = engine_writes(i, v);
        for (std::size_t w = 0; w < writes.size; ++w) {
            const auto& [name, text] = writes.items[w];
            // Restoring a value the engine has since replaced is fine: the engine's newer one stays.
            const auto reply = want ? change_named_setting(name, text, false) : change_named_setting(name, {}, true);
            failed = failed || reply.starts_with("error: ");
            if (!reply.ends_with("(unchanged)")) replies += (replies.empty() ? "" : " | ") + reply;
        }
        held[i] = want || failed; // a failed restore is tried again next pass
        complete = complete && !failed;
        if (!replies.empty() && replies != last[i]) {
            last[i] = replies;
            logging::log(failed ? logging::Level::warning : logging::Level::info, logging::Channel::graphics,
                "Performance settings: {}: {}", table[i].key, replies);
        }
    }
    return apply_lighting() && complete;
}

bool level_active(unsigned client_state) noexcept { return client_state == 13 || client_state == 21; }

// Runs once, after the game's own startup script and before its shader system loads the pipeline
// cache (the profile is already open: startup waits for it). Returns whether anything was set.
bool startup_settings(const lua_startup::Context& context) {
    try {
        // Not saved yet: ReSkate's default (`initial`, off like the game's own).
        const auto saved = profile_runtime::local_value(saved_key(shader_prime));
        const double wanted = saved && saved->is_number() ? saved->get<double>() : table[shader_prime].initial;
        if (wanted < 0.5) return false;
        const bool applied = context.execute(
            "ShaderSystem=ShaderSystem or {}\n"
            "ShaderSystem.Dx12PrimeCachedPipelinesEnable=true\n"
            "ShaderSystem.Dx12PrimeCachedPipelinesUseJobs=true");
        logging::log(applied ? logging::Level::info : logging::Level::warning, logging::Channel::graphics,
            "Performance settings: compile cached shaders while loading {}.", applied ? "set at startup" : "could not be set at startup");
        return applied;
    } catch (...) {
        return false;
    }
}
} // namespace

bool start_startup_settings(std::uintptr_t base, std::string& error) noexcept {
    try {
        return lua_startup::add_callback(base, &startup_settings, error);
    } catch (...) {
        error = "Cannot register the startup settings";
        return false;
    }
}

std::span<const Option> options() noexcept { return table; }
bool loaded() noexcept { return state().loaded.load(); }
float value(std::size_t index) noexcept { return index < count ? state().values[index].load(std::memory_order_relaxed) : 0.0f; }

void set_value(std::size_t index, float v) noexcept {
    auto& s = state();
    if (index >= count || !s.loaded.load()) return;
    const float clamped = clamp_value(index, v);
    if (s.values[index].exchange(clamped) != clamped) mark_changed(1u << index);
}

void apply_low_spec() noexcept { set_all(&Option::low_spec); }
void restore_defaults() noexcept { set_all(&Option::initial); }
bool is_low_spec() noexcept { return all_at(&Option::low_spec); }
bool is_default() noexcept { return all_at(&Option::initial); }

void tick(unsigned client_state) noexcept {
    try {
        auto& s = state();
        // Times in GetTickCount64 milliseconds; 0 = nothing due.
        static ULONGLONG apply_at = 0, confirm_at = 0, save_at = 0;
        static unsigned last_state = ~0u, retries = 0;
        constexpr unsigned max_retries = 30; // one a second; a level becoming active starts over

        const bool menu_change = s.changed.load(std::memory_order_acquire);
        const bool new_state = client_state != last_state;
        // The usual tick: nothing changed and nothing due.
        if (!menu_change && !new_state && !apply_at && !confirm_at && !save_at && s.loaded.load(std::memory_order_relaxed))
            return;

        const auto now = GetTickCount64();
        if (!s.loaded.load()) {
            load();
            apply_at = now;
        }
        if (new_state) {
            last_state = client_state;
            // The engine can put its own values back while a level loads: apply once the level is
            // active, and check once more after its environment has settled.
            if (level_active(client_state)) {
                apply_at = now + 500;
                confirm_at = now + 5000;
                retries = 0;
            }
        }
        if (menu_change) {
            s.changed.store(false, std::memory_order_relaxed);
            apply_at = now;
            retries = 0;
            save_at = now + 1000; // after the last change, so a burst of clicks is one write
        }

        if (save_at && now >= save_at) {
            save_at = 0;
            if (const auto mask = s.unsaved.exchange(0)) save(mask);
        }
        const bool due = (apply_at && now >= apply_at) || (confirm_at && now >= confirm_at);
        if (!due) return;
        if (apply_at && now >= apply_at) apply_at = 0;
        if (confirm_at && now >= confirm_at) confirm_at = 0;
        if (!apply_all()) {
            // Not ready yet (settings still resolving during a load): try again shortly.
            if (++retries <= max_retries) apply_at = now + 1000;
            else if (retries == max_retries + 1)
                logging::write(logging::Level::warning, logging::Channel::graphics,
                    "Performance settings: some values could not be applied; retrying when a level becomes active.");
        } else {
            retries = 0;
        }
    } catch (...) {}
}
} // namespace dingosdk::graphics_tuning
