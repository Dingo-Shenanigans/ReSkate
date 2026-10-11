#include "Engine/Core/Log/logging.h"
#include "runtime_internal.h"
#include "Extension/Rendering/local_graphics_controls.h"
#include "Extension/World/local_park_rotation.h"
#include "Extension/World/local_population_controls.h"
#include "Extension/World/local_world_controls.h"
#include "Extension/World/local_world_layers.h"

namespace dingosdk {
using namespace profile_runtime;
GraphicsControlsModel local_profile_graphics_controls() {
    auto& r = graphics_runtime(); std::lock_guard lock(r.mutex);
    auto result = r.model; result.available = r.active && local_runtime().active;
    return result;
}
void update_local_graphics_controls() noexcept {
    if (!local_runtime().active.load(std::memory_order_acquire)) return;
    PreserveError preserve;
    try { update_graphics_controls(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"graphics_controls_update_failed\"}"); }
}
bool set_local_graphics_control(std::string_view key, int value) {
    auto& s = local_runtime(); auto& r = graphics_runtime();
    std::lock_guard native(s.native_mutex); std::lock_guard lock(r.mutex);
    if (!s.active || !r.active || value < -1 || value > 1) return false;
    auto choices = r.model.choices;
    if (key == "reset") choices = {};
    else {
        const auto it = std::find(graphics_keys.begin(), graphics_keys.end(), key);
        if (it == graphics_keys.end()) return false;
        choices.effects[it - graphics_keys.begin()] = value;
    }
    try {
        s.store->save_graphics_controls(choices);
        r.model.choices = choices; r.last_update = 0;
        r.disabled_video_effects.store(graphics_video_mask(choices), std::memory_order_release);
        r.model.status = "Saved. Applying graphics controls.";
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","graphics_control_saved"},{"key",key},{"value",value}}.dump().c_str());
        return true;
    } catch (...) { r.model.status = "Graphics controls could not be saved."; return false; }
}
WorldControlsModel local_profile_world_controls() {
    auto& r = world_control_runtime(); std::lock_guard lock(r.mutex);
    auto result = r.model;
    result.environment_available = r.environment_active && local_runtime().active;
    result.population_available = r.population_active && local_runtime().active;
    for (unsigned i = 0; i < 2; ++i)
        result.population_ready[i] = result.population_ready[i] && GetTickCount64() - population_runtime().observed_at[i] < 3000;
    return result;
}
void update_local_world_controls(WorldMap map) noexcept {
    if (!local_runtime().active.load(std::memory_order_acquire)) return;
    PreserveError preserve;
    try { update_environment_controls(map); update_population_controls(); }
    catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::profile, "{\"event\":\"world_controls_update_failed\"}"); }
}
bool set_local_atmosphere_control(std::string_view key, std::string_view value) {
    auto& s=local_runtime(); auto& r=world_control_runtime();
    std::lock_guard native(s.native_mutex); std::lock_guard lock(r.mutex);
    const auto* control=find_atmosphere_control(key);
    if (!s.active || !r.environment_active || !control) return false;
    const bool reset=value=="default";
    AtmosphereValue v;
    if (!reset && !parse_atmosphere_value(*control,value,v)) return false;
    if (!reset && control->type==AtmosphereType::texture) {
        const auto assets=r.textures.find(control->property);
        if (assets==r.textures.end() || !assets->second.contains(v.texture)) return false;
    }
    auto c=r.model.choices;
    if (reset) c.atmosphere.erase(std::string(key)); else c.atmosphere[std::string(key)]=std::move(v);
    try {
        s.store->save_world_controls(c);r.model.choices=std::move(c);r.last_update=0;
        r.model.status="Saved. Applying environment controls.";
        return true;
    } catch (...) { r.model.status="World controls could not be saved.";return false; }
}
bool reset_local_atmosphere_controls(unsigned kind) {
    auto& s=local_runtime();auto& r=world_control_runtime();
    std::lock_guard native(s.native_mutex);std::lock_guard lock(r.mutex);
    if (!s.active || !r.environment_active || kind>2) return false;
    auto c=r.model.choices;
    for (const auto& control : atmosphere_controls)
        if (atmosphere_kind(control.property)==kind) c.atmosphere.erase(control.key);
    if (kind==0) { c.fog=-1;c.fog_distance=-1; }
    if (kind==1) { c.sky_brightness=-1;c.clouds=-1; }
    if (kind==2) { c.wind_strength=-1;c.wind_direction=-1; }
    try {
        s.store->save_world_controls(c);r.model.choices=std::move(c);r.last_update=0;
        r.model.status="Saved. Restoring environment defaults.";
        return true;
    } catch (...) { r.model.status="World controls could not be saved.";return false; }
}
bool set_local_world_control(std::string_view key, float value) {
    auto& s = local_runtime(); auto& r = world_control_runtime();
    std::lock_guard native(s.native_mutex); std::lock_guard lock(r.mutex);
    if (!s.active || !std::isfinite(value) || value < -1 || value > 360) return false;
    auto c = r.model.choices;
    if (key == "traffic" || key == "pedestrians" || key == "fog") {
        if (std::floor(value) != value) return false;
        if (key == "traffic") c.traffic = static_cast<int>(value);
        if (key == "pedestrians") c.pedestrians = static_cast<int>(value);
        if (key == "fog") c.fog = static_cast<int>(value);
    } else if (key == "fog_distance") c.fog_distance = value;
    else if (key == "sky_brightness") c.sky_brightness = value;
    else if (key == "clouds") c.clouds = value;
    else if (key == "wind_strength") c.wind_strength = value;
    else if (key == "wind_direction") c.wind_direction = value;
    else if (key == "reset_environment") { const auto traffic = c.traffic, pedestrians = c.pedestrians; c = {}; c.traffic = traffic; c.pedestrians = pedestrians; }
    else return false;
    if (!valid_world_controls(c)) return false;
    const bool population = key == "traffic" || key == "pedestrians";
    if (population ? !r.population_active : !r.environment_active) return false;
    try {
        s.store->save_world_controls(c); r.model.choices = c; r.last_update = 0; r.diagnostic_due = GetTickCount64() + 6000;
        r.model.status = "Saved. Applying world controls.";
        dingosdk::logging::event(dingosdk::logging::Channel::profile, dingosdk::Json{{"event","world_control_saved"},{"key",key},{"value",value}}.dump().c_str());
        return true;
    } catch (...) { r.model.status = "World controls could not be saved."; return false; }
}


WorldLayersModel local_profile_world_layers() {
    auto& s = local_runtime(); auto& r = world_layers_runtime();
    std::lock_guard lock(s.native_mutex);
    auto result = r.model;
    result.available = s.active.load(std::memory_order_acquire) && r.active.load(std::memory_order_acquire) &&
        park_runtime().active.load(std::memory_order_acquire);
    return result;
}
void set_local_world_layer_map_hint(WorldMap map) noexcept {
    world_layers_runtime().map_hint.store(map, std::memory_order_release);
}
void apply_host_world_layers(bool forced, const WorldLayerChoices& choices) {
    auto& s = local_runtime(); auto& r = world_layers_runtime();
    std::lock_guard lock(s.native_mutex);
    if (r.model.controlled_by_host == forced && (!forced || r.model.choices == choices)) return;
    if (!r.lobby_override.apply(r.model, forced, choices)) return;
    r.next_poll = 0;
    r.model.feedback = forced ? "World layers follow the host's choices for this session."
                              : "Restoring your local world layer choices.";
}
bool set_local_world_layer(std::string_view layer, std::string_view mode) {
    auto& s = local_runtime(); auto& r = world_layers_runtime();
    std::lock_guard lock(s.native_mutex);
    if (r.model.controlled_by_host) {
        r.model.feedback = "The session host is forcing world layer sync.";
        return false;
    }
    if (!s.active.load(std::memory_order_acquire) || !r.active.load(std::memory_order_acquire) ||
        !park_runtime().active.load(std::memory_order_acquire)) return false;
    try {
        const auto it = std::find_if(world_layers().begin(), world_layers().end(), [&](const auto& row) { return row.key == layer; });
        if (it == world_layers().end() || !valid_world_layer_mode(mode) || world_layer_kept_off(it->key)) {
            r.model.feedback = "Choose a valid world layer and mode."; return false;
        }
        const auto index = static_cast<unsigned>(it - world_layers().begin());
        if (!r.model.ready || r.model.map != it->map || !r.model.supported[index]) {
            r.model.feedback = "That world layer is not available in the active level.";
            return false;
        }
        s.store->save_world_layer_choice(index, mode);
        r.model.choices[index] = mode;
        std::lock_guard lifetime(r.lifetime_mutex);
        for (auto& [entity, node] : r.nodes) {
            (void)entity;
            if (world_layer_uses_slot(index, node.slot))
                if (node.pending && GetTickCount64() - node.requested_at > 30000) node.pending.reset();
        }
        r.next_poll = 0;
        r.model.feedback = "World choice saved. Applies when its map is ready.";
        return true;
    } catch (...) { r.model.feedback = "World choice could not be saved."; return false; }
}
bool restore_local_world_layers(std::string_view map) {
    auto& s = local_runtime(); auto& r = world_layers_runtime();
    std::lock_guard lock(s.native_mutex);
    if (r.model.controlled_by_host) {
        r.model.feedback = "The session host is forcing world layer sync.";
        return false;
    }
    if (!s.active.load(std::memory_order_acquire) || !r.active.load(std::memory_order_acquire) ||
        !park_runtime().active.load(std::memory_order_acquire)) return false;
    WorldMap selected = WorldMap::none;
    for (const auto slot : world_map_anchors())
        if (world_map_key(world_layer_nodes()[slot].map) == map) selected = world_layer_nodes()[slot].map;
    if (selected == WorldMap::none) { r.model.feedback = "Choose a valid world map."; return false; }
    try {
        // One durable transaction; never publish a partially reset map.
        s.store->restore_world_layers(selected);
        for (unsigned i = 0; i < world_layers().size(); ++i)
            if (world_layers()[i].map == selected) r.model.choices[i] = "default";
        std::lock_guard lifetime(r.lifetime_mutex);
        for (auto& [entity, node] : r.nodes) {
            (void)entity;
            if (world_layer_nodes()[node.slot].map == selected && node.pending &&
                GetTickCount64() - node.requested_at > 30000) node.pending.reset();
        }
        r.next_poll = 0;
        r.model.feedback = std::string(world_map_label(selected)) + " defaults restored. Layers return to game control.";
        return true;
    } catch (...) { r.model.feedback = "World defaults could not be saved."; return false; }
}
ParksModel local_profile_parks() {
    auto& s = local_runtime();
    std::lock_guard lock(s.native_mutex);
    auto result = park_runtime().model;
    result.available = s.active.load(std::memory_order_acquire) && park_runtime().active.load(std::memory_order_acquire);
    return result;
}
bool set_local_park(std::string_view lot, std::string_view layout) {
    auto& s = local_runtime(); auto& r = park_runtime();
    std::lock_guard lock(s.native_mutex);
    if (!s.active.load(std::memory_order_acquire) || !r.active.load(std::memory_order_acquire)) return false;
    if (r.model.controlled_by_host) {
        r.model.feedback = "The lobby host controls community park layouts.";
        return false;
    }
    try {
        const auto it = std::find_if(park_lots.begin(), park_lots.end(), [&](const auto& row) { return row.key == lot; });
        if (it == park_lots.end() || layout.empty()) {
            r.model.feedback = "Choose a valid park location and layout.";
            return false;
        }
        const auto index = static_cast<unsigned>(it - park_lots.begin());
        s.store->save_park_choice(index, layout);
        r.launch_randomization.pending = false;
        r.model.choices[index] = layout;
        r.sent[index].clear();
        r.model.feedback = "Park choice saved. It loads when BAM's park controller is ready.";
        return true;
    } catch (...) {
        r.model.feedback = "Park choice could not be saved. Check the profile and layout.";
        return false;
    }
}
bool load_random_local_parks() {
    auto& s = local_runtime(); auto& r = park_runtime();
    std::lock_guard lock(s.native_mutex);
    if (!s.active || !r.active || !s.store) return false;
    if (r.model.controlled_by_host) {
        r.model.feedback = "The lobby host controls community park layouts.";
        return false;
    }
    try {
        auto choices = random_park_choices();
        // Publish only after all three choices are durably saved together.
        s.store->save_park_choices(choices);
        r.model.choices = std::move(choices);
        r.sent = {}; r.clear_unset = {};
        r.launch_randomization.pending = false;
        // Respect an in-flight clear/load delay; the pending load uses the new choice.
        if (r.pending_lot >= park_lots.size()) r.next_request = 0;
        r.model.feedback = "Random park choices saved. Loading when the park controller is ready.";
        return true;
    } catch (...) {
        r.model.feedback = "Random park choices could not be saved.";
        return false;
    }
}
bool set_local_park_randomize_on_launch(bool enabled) {
    auto& s = local_runtime(); auto& r = park_runtime();
    std::lock_guard lock(s.native_mutex);
    if (!s.active || !r.active || !s.store) return false;
    try {
        s.store->set_user_value("ReSkate.RandomizeParksOnLaunch", enabled);
        r.model.randomize_on_launch = enabled;
        // Enabling schedules the next launch, never an unexpected mid-session load.
        if (!enabled) r.launch_randomization.pending = false;
        r.model.feedback = enabled ? "Parks will randomize on the next game launch when you control the layouts." :
            "Randomize on Launch is off. Your saved layouts will be used.";
        return true;
    } catch (...) {
        r.model.feedback = "Randomize on Launch could not be saved.";
        return false;
    }
}
void set_lobby_park_mode(bool active, bool guest) {
    auto& s = local_runtime(); auto& r = park_runtime();
    std::lock_guard lock(s.native_mutex);
    if (r.lobby_active == active && r.model.controlled_by_host == (active && guest)) return;
    if (!s.active.load(std::memory_order_acquire) || !r.active.load(std::memory_order_acquire)) return;
    // Joining a host cancels this launch's roll even before a park controller
    // can tick. Leaving the lobby must restore saved choices without rerolling.
    if (active && guest) r.launch_randomization.pending = false;
    auto choices = r.model.choices;
    if (!active) choices = profile::park_choices(*s.store->shared_snapshot());
    else if (!guest) {
        // An unselected host slot means an empty lot for this session. Apply it
        // locally too, so guests cannot retain a different saved layout.
        for (auto& choice : choices) if (choice.empty()) choice = "empty";
    }
    for (unsigned lot = 0; lot < choices.size(); ++lot)
        if (choices[lot] != r.model.choices[lot]) {
            r.sent[lot].clear();
            r.clear_unset[lot] = choices[lot].empty();
        }
    r.model.choices = std::move(choices);
    r.lobby_active = active;
    r.model.controlled_by_host = active && guest;
    if (r.pending_lot >= park_lots.size()) r.next_request = 0;
    r.model.feedback = active ? (guest ? "Waiting for the host's park settings." :
        "Park choices are shared with everyone in this lobby.") : "Restoring your local park choices.";
}
void apply_host_park_choices(const ParkChoices& choices) {
    auto& s = local_runtime(); auto& r = park_runtime();
    std::lock_guard lock(s.native_mutex);
    if (!r.lobby_active || !r.model.controlled_by_host) return;
    for (unsigned lot = 0; lot < choices.size(); ++lot)
        if (choices[lot].empty() || !valid_park(lot, choices[lot]))
            throw std::invalid_argument("Invalid host park selection");
    if (choices == r.model.choices) {
        if (r.model.feedback == "Waiting for the host's park settings.")
            r.model.feedback = "Park layouts follow the lobby host.";
        return;
    }
    for (unsigned lot = 0; lot < choices.size(); ++lot)
        if (choices[lot] != r.model.choices[lot]) {
            r.sent[lot].clear(); r.clear_unset[lot] = false;
        }
    r.model.choices = choices;
    if (r.pending_lot >= park_lots.size()) r.next_request = 0;
    r.model.feedback = "Applying the host's park choices when BAM is ready...";
}
}
