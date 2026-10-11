#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/runtime_internal.h"
#include "local_park_rotation.h"
#include "local_world_layers.h"
#include "Engine/Game/Build/20260929/world_layers.h"

namespace dingosdk::profile_runtime {
// Exact-build SubWorldReference adapter; analysis/seasonal-world-layers.md.

// Parent first, then the populated leaf. Never dispatch from ImGui/Present.

// Keep constructor capture and streaming plans tied to the audited catalog.

bool world_layer_uses_slot(unsigned choice, unsigned slot) {
    for (int at = static_cast<int>(world_leaf(choice)); at >= 0; at = world_parent(at))
        if (static_cast<unsigned>(at) == slot) return true;
    return false;
}

WorldLayersRuntime& world_layers_runtime() { static auto* r = new WorldLayersRuntime; return *r; }
void emit_subworld(logging::Level level, std::string_view message) {
    logging::write(level, logging::Channel::subworld, message);
}

bool seasonal_name_equal(std::string_view a, std::string_view b) {
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [&](unsigned char x, unsigned char y) { return lower(x) == lower(y); });
}

void capture_seasonal_node(std::uintptr_t entity) {
    auto& r = world_layers_runtime();
    std::uintptr_t vt{}, data{}, owner{}, context{};
    std::string bundle;
    if (!read(entity, vt) || vt != local_runtime().base + addr::world_layers::subworld_reference_vtable ||
        !read(entity + 0x38, data) || data > memory::highest_user_address - 0xe0 ||
        !read(entity + 0x30, owner) || owner > memory::highest_user_address - 0x28 ||
        !read(owner + 0x20, context) || !context ||
        !identifier(reinterpret_cast<const void*>(data + 0xd8), bundle)) return;
    {
        std::lock_guard lock(r.lifetime_mutex);
        if (r.subworld_logs.size() < 512 || r.subworld_logs.contains(entity)) {
            auto& row = r.subworld_logs[entity];
            row = {};
            row.identity = SeasonalNode{entity, data, context, ++r.streaming_generation};
            row.bundle = bundle;
            logging::log(logging::Level::debug, logging::Channel::subworld,
                "Subworld reference created: {} (instance {}).", bundle, row.identity.generation);
            SeasonalState state;
            if (seasonal_state(row.identity, state))
                row.progress.observe(state.requested, state.loaded, GetTickCount64(), bundle, row.identity.generation, &emit_subworld);
        } else if (++r.streaming_capacity_drops == 1) {
            logging::write(logging::Level::warning, logging::Channel::subworld,
                "Subworld observation capacity reached (512 references); some streaming states will be omitted.");
        }
    }
    for (unsigned slot = 0; slot < world_layer_nodes().size(); ++slot) {
        if (!seasonal_name_equal(bundle, seasonal_bundle(slot))) continue;
        std::lock_guard lock(r.lifetime_mutex);
        if (r.nodes.size() >= world_layer_nodes().size() * 4) return;
        r.nodes[entity] = SeasonalNode{entity, data, context, ++r.generation, slot, {}, 0};
        dingosdk::logging::event(dingosdk::logging::Channel::world, dingosdk::Json{{"event", "local_world_layer_observed"}, {"slot", slot},
            {"entity", entity}, {"bundle", bundle}, {"context", context}, {"generation", r.generation}}.dump().c_str());
        return;
    }
}

std::uintptr_t seasonal_construct_hook(std::uintptr_t entity, std::uintptr_t info, std::uintptr_t data) {
    auto& r = world_layers_runtime();
    const auto result = r.construct(entity, info, data);
    if (r.active.load(std::memory_order_acquire)) {
        PreserveError preserve;
        try { capture_seasonal_node(entity); } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"local_world_layer_capture_failed\"}"); }
    }
    return result;
}

std::uintptr_t seasonal_destroy_hook(std::uintptr_t entity, unsigned flags) {
    auto& r = world_layers_runtime();
    {
        PreserveError preserve;
        std::lock_guard lock(r.lifetime_mutex);
        if (const auto found = r.subworld_logs.find(entity); found != r.subworld_logs.end()) {
            logging::log(logging::Level::debug, logging::Channel::subworld,
                "Destroying subworld reference: {} (instance {}).", found->second.bundle, found->second.identity.generation);
            r.subworld_logs.erase(found);
        }
        r.nodes.erase(entity);
    }
    return r.destroy(entity, flags);
}

void reset_world_layer_session() {
    auto& r = world_layers_runtime();
    r.context = 0; r.root_generation = 0; r.next_poll = 0;
    r.original.assign(world_layer_nodes().size(), std::nullopt);
    r.managed.assign(world_layer_nodes().size(), false); r.model.map = WorldMap::none;
    r.model.ready = false; r.model.clear_live();
}

bool seasonal_state(const SeasonalNode& node, SeasonalState& state) {
    std::uintptr_t vt{}, data{}, owner{}, context{}, handle{};
    std::uint8_t flags{};
    if (!read(node.entity, vt) || vt != local_runtime().base + addr::world_layers::subworld_reference_vtable ||
        !read(node.entity + 0x38, data) || data != node.data ||
        !read(node.entity + 0x30, owner) || !read(owner + 0x20, context) || context != node.context ||
        !read(node.entity + 0x80, handle) || !read(node.entity + 0x148, flags)) return false;
    state = {handle != 0, (flags & 1) != 0};
    return true;
}

// This is the same fingerprinted void(entity, event*) handler used by the
// world-layer adapter. Inspect at the native boundary, then forward once. No
// observer lock is held across the original; native dispatch can reenter.
void seasonal_event_hook(std::uintptr_t entity, const SeasonalEvent* event) {
    PreserveError native_error;
    const auto incoming_error = native_error.error;
    auto& r = world_layers_runtime();
    std::string bundle;
    bool observed{};
    std::uint32_t id{};
    const char* action{"other"};
    {
        PreserveError preserve;
        try {
            SeasonalEvent value{};
            std::uintptr_t vt{}, data{};
            if (r.active.load(std::memory_order_acquire) && read(reinterpret_cast<std::uintptr_t>(event), value) &&
                value.vtable == local_runtime().base + addr::world_layers::subworld_event_vtable &&
                read(entity, vt) && vt == local_runtime().base + addr::world_layers::subworld_reference_vtable && read(entity + 0x38, data) &&
                data <= memory::highest_user_address - 0xe0 &&
                identifier(reinterpret_cast<const void*>(data + 0xd8), bundle)) {
                id = value.id;
                const bool known = id == 0xd27bc17e || id == 0x21f3f677;
                action = id == 0xd27bc17e ? "load" : id == 0x21f3f677 ? "unload" : "other";
                observed = true;
                logging::log(known ? logging::Level::info : logging::Level::debug, logging::Channel::subworld,
                    "Subworld {} event (0x{:08x}): {}.", action, id, bundle);
            }
        } catch (...) {}
    }
    const auto started = GetTickCount64();
    SetLastError(incoming_error);
    r.original_event(entity, event);
    native_error.error = GetLastError();
    if (observed) {
        const auto elapsed = GetTickCount64() - started;
        logging::log(elapsed >= 1000 ? logging::Level::warning : logging::Level::debug,
            logging::Context::engine, logging::Channel::subworld,
            "Subworld {} event (0x{:08x}) handler returned after {}ms: {}; streaming completion is observed separately.",
            action, id, elapsed, bundle);
    }
}

bool ensure_seasonal_state(std::uintptr_t entity, bool wanted, std::uint64_t now,
                           bool& issued, std::string& status) {
    auto& r = world_layers_runtime();
    const auto found = r.nodes.find(entity);
    SeasonalState state;
    if (found == r.nodes.end() || !seasonal_state(found->second, state)) { status = "Waiting for layer"; return false; }
    auto& node = found->second;
    const auto reached = [&](bool on) { return on ? state.requested && state.loaded : !state.requested && !state.loaded; };
    if (node.pending && reached(*node.pending)) {
        dingosdk::logging::event(dingosdk::logging::Channel::world, dingosdk::Json{{"event", "local_world_layer_completed"}, {"slot", node.slot},
            {"bundle", seasonal_bundle(node.slot)}, {"enabled", *node.pending}}.dump().c_str());
        node.pending.reset();
    }
    if (node.pending) {
        status = now - node.requested_at > 30000 ? "Streaming did not finish. Apply to retry." : "Streaming...";
        return false;
    }
    if (reached(wanted)) return true;
    // An engine-initiated auto-load is still in flight. Wait for it rather than
    // claiming success from a non-null request handle or repeatedly reloading.
    if (wanted && state.requested) { status = "Loading..."; return false; }
    node.pending = wanted; node.requested_at = now;
    const auto slot = node.slot;
    SeasonalEvent event{local_runtime().base + addr::world_layers::subworld_event_vtable, wanted ? 0xd27bc17eu : 0x21f3f677u};
    issued = true; status = wanted ? "Loading..." : "Unloading...";
    dingosdk::logging::event(dingosdk::logging::Channel::world, dingosdk::Json{{"event", "local_world_layer_request"}, {"slot", slot}, {"bundle", seasonal_bundle(slot)},
        {"entity", entity}, {"enabled", wanted}, {"requested", state.requested}, {"loaded", state.loaded}}.dump().c_str());
    // The native handler clones this 16-byte event into its own arena. It never
    // retains our stack pointer; the base event destructor is a no-op.
    r.event(entity, &event);
    return false; // loading may create/destroy other captured references
}

void update_world_layers(std::uintptr_t manager, std::uint64_t now) {
    auto& r = world_layers_runtime();
    if (!r.active.load(std::memory_order_acquire)) return;
    std::lock_guard lifetime(r.lifetime_mutex);
    // Observe streaming before the park-ready guard: a stuck layer must remain
    // visible even when its parent world is not ready for SDK overrides yet.
    if (now >= r.next_streaming_poll) {
        r.next_streaming_poll = now + 250;
        // Rotate through a bounded number of references per tick, including
        // engine-created parks and other subworlds outside the seasonal catalog.
        const auto count = (std::min)(r.subworld_logs.size(), std::size_t{64});
        auto next = r.subworld_logs.upper_bound(r.streaming_cursor);
        for (std::size_t i = 0; i < count; ++i) {
            if (next == r.subworld_logs.end()) next = r.subworld_logs.begin();
            auto& [entity, row] = *next++;
            r.streaming_cursor = entity;
            SeasonalState state;
            if (seasonal_state(row.identity, state))
                row.progress.observe(state.requested, state.loaded, now, row.bundle, row.identity.generation, &emit_subworld);
        }
    }
    std::uintptr_t event_context{}, context{};
    // Grom has no BAM park-ready flag. Keep the same exact manager/context
    // checks, but select the map from its live, root-owned references instead.
    if (!park_owner_ready(manager, event_context, false) || !read(manager + 8, context)) {
        reset_world_layer_session(); return;
    }
    if (now < r.next_poll) return;
    r.next_poll = now + 250;
    std::vector<std::uintptr_t> targets(world_layer_nodes().size());
    for (const auto& [entity, node] : r.nodes) {
        SeasonalState state;
        if (node.context != context || !seasonal_state(node, state)) continue;
        if (targets[node.slot]) {
            r.model.ready = false; r.model.map = WorldMap::none;
            r.model.feedback = "Multiple world controllers found; no changes sent."; return;
        }
        targets[node.slot] = entity;
    }
    std::vector<bool> present(targets.size());
    for (unsigned slot = 0; slot < targets.size(); ++slot)
        present[slot] = targets[slot] != 0;
    // During a map transition controllers from both families may briefly
    // coexist. Wait rather than route choices into a departing world. Studio
    // levels are allowed to omit the retail familys canonical root, so a
    // unique set of top-level donor controllers is also a valid map identity.
    const auto selection = select_world_layer_map(
        present, r.map_hint.load(std::memory_order_acquire));
    if (selection.map == WorldMap::none) { reset_world_layer_session(); return; }
    const auto anchor = static_cast<unsigned>(selection.root);
    const auto map = selection.map;
    const auto root_generation = r.nodes.at(targets[anchor]).generation;
    if (r.context != context || r.root_generation != root_generation) {
        reset_world_layer_session(); r.context = context; r.root_generation = root_generation;
        dingosdk::logging::event(dingosdk::logging::Channel::world, dingosdk::Json{
            {"event", "local_world_layer_map_selected"},
            {"map", world_map_key(map)}, {"anchor_slot", anchor},
            {"canonical_anchor", selection.canonical}}.dump().c_str());
    }
    r.model.ready = true; r.model.map = map;
    // Publish a separate native snapshot for Default checkboxes. Compute it
    // before dispatch, which can synchronously destroy captured references.
    std::vector<std::optional<bool>> observed(targets.size());
    for (unsigned slot = 0; slot < targets.size(); ++slot) {
        if (world_layer_nodes()[slot].map != map) continue;
        const auto parent = world_parent(static_cast<int>(slot));
        if (parent >= 0 && observed[parent] == false) { observed[slot] = false; continue; }
        if (parent >= 0 && !observed[parent].has_value()) continue;
        SeasonalState state;
        if (targets[slot] && seasonal_state(r.nodes.at(targets[slot]), state))
            observed[slot] = state.requested || state.loaded;
    }
    r.model.enabled.assign(world_layers().size(), std::nullopt); r.model.supported.assign(world_layers().size(), false);
    r.model.status.resize(world_layers().size());
    for (unsigned i = 0; i < world_layers().size(); ++i)
        if (world_layers()[i].map == map) {
            r.model.supported[i] = world_layer_supported(i, present);
            if (r.model.supported[i]) r.model.enabled[i] = observed[world_leaf(i)];
        }
    // Refresh untouched baselines, allowing normal native auto-load to finish.
    // Children materialized by an overridden parent retain their original
    // baseline across destruction/recreation; AutoLoad covers their first tick.
    for (unsigned slot = 0; slot < targets.size(); ++slot) {
        if (world_layer_nodes()[slot].map != map) continue;
        const auto parent = world_parent(static_cast<int>(slot));
        const bool parent_owned = parent >= 0 && r.managed[parent];
        SeasonalState state;
        if (targets[slot] && (!r.original[slot] || (!r.managed[slot] && !parent_owned)) &&
            seasonal_state(r.nodes.at(targets[slot]), state))
            r.original[slot] = state.requested || state.loaded || (parent_owned && world_autoload(slot));
    }
    std::vector<std::optional<bool>> desired(targets.size());
    std::vector<bool> explicit_choice(targets.size());
    for (unsigned slot = 0; slot < targets.size(); ++slot)
        if (r.managed[slot]) desired[slot] = r.original[slot];
    // A disabled container takes precedence over enabled children. Otherwise
    // a later row could silently turn it back on or leave the plan oscillating.
    std::vector<bool> disabled(targets.size());
    // A layer ReSkate keeps off (world_layers.h) is "off" whatever the player or a host chose.
    const auto choice_of = [&](unsigned i) -> std::string_view {
        return world_layer_kept_off(world_layers()[i].key) ? std::string_view("off") : std::string_view(r.model.choices[i]);
    };
    for (unsigned i = 0; i < world_layers().size(); ++i)
        if (world_layers()[i].map == map && choice_of(i) == "off") {
            const auto slot = world_switch(i);
            desired[slot] = false; explicit_choice[slot] = disabled[slot] = true;
        }
    for (unsigned i = 0; i < world_layers().size(); ++i) {
        if (world_layers()[i].map != map) { r.model.status[i] = "Available in " + std::string(world_map_label(world_layers()[i].map)); continue; }
        if (choice_of(i) != "on") continue;
        bool blocked{};
        for (int at = static_cast<int>(world_leaf(i)); at >= 0; at = world_parent(at)) blocked = blocked || disabled[at];
        if (blocked) continue;
        for (int at = static_cast<int>(world_leaf(i)); at >= 0; at = world_parent(at)) {
            desired[at] = true; explicit_choice[at] = true;
        }
    }
    // Baselines must be captured before acquiring ownership, especially for
    // autoloaded systems that were already present before the first override.
    for (unsigned slot = 0; slot < targets.size(); ++slot)
        if (explicit_choice[slot] && r.original[slot]) r.managed[slot] = true;
    std::vector<bool> done(targets.size()), inactive(targets.size());
    std::vector<std::string> status(targets.size(), "Waiting for layer");
    bool issued{};
    for (unsigned slot = 0; slot < targets.size(); ++slot) {
        if (world_layer_nodes()[slot].map != map) continue;
        const auto parent = world_parent(static_cast<int>(slot));
        if (parent >= 0) {
            if (!done[parent]) { status[slot] = status[parent]; continue; }
            if (inactive[parent]) {
                done[slot] = inactive[slot] = true; status[slot] = "Disabled";
                if (!explicit_choice[slot]) r.managed[slot] = false;
                continue;
            }
        }
        if (!targets[slot]) continue;
        SeasonalState state;
        if (!seasonal_state(r.nodes.at(targets[slot]), state)) continue;
        if (!desired[slot]) {
            done[slot] = state.requested == state.loaded;
            inactive[slot] = done[slot] && !state.loaded;
            status[slot] = done[slot] ? "Default" : "Loading...";
        } else {
            done[slot] = ensure_seasonal_state(targets[slot], *desired[slot], now, issued, status[slot]);
            inactive[slot] = done[slot] && !*desired[slot];
            if (done[slot]) {
                status[slot] = inactive[slot] ? "Disabled" : "Loaded";
                if (!explicit_choice[slot]) r.managed[slot] = false;
            }
        }
        // Dispatch can synchronously destroy/recreate children. Rebuild the
        // target snapshot next tick instead of dereferencing the old addresses.
        if (issued) break;
    }
    for (unsigned i = 0; i < world_layers().size(); ++i) {
        if (world_layers()[i].map != map) continue;
        const auto choice = choice_of(i);
        const auto slot = choice == "off" ? world_switch(i) : world_leaf(i);
        r.model.status[i] = done[slot] ? (choice == "default" ? "Default" : inactive[slot] ? "Disabled" : "Loaded") : status[slot];
    }
}

void start_world_layers() noexcept {
    auto& r = world_layers_runtime(); const auto base = local_runtime().base;
    const auto& sites = addr::world_layers::sites;
    std::vector<void*> created;
    try {
        r.model.choices = profile::world_layer_choices(*local_runtime().store->shared_snapshot());
        for (const auto& site : sites) {
            std::array<unsigned char, 16> bytes{};
            if (!read(base + site.rva, bytes) || bytes != site.bytes) throw std::runtime_error("World layer fingerprint mismatch");
        }
        std::uintptr_t clone{};
        if (!read(base + addr::world_layers::subworld_event_vtable + 0x18, clone) || clone != base + sites[3].rva) throw std::runtime_error("World event clone mismatch");
        const auto hook = [&](unsigned i, auto detour, auto& original) {
            auto* target = reinterpret_cast<void*>(base + sites[i].rva);
            if (hook_prepare(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) != HookOk)
                throw std::runtime_error("Cannot prepare world layer adapter");
            created.push_back(target);
        };
        r.event = reinterpret_cast<WorldLayersRuntime::Event>(base + sites[2].rva);
        hook(0, &seasonal_construct_hook, r.construct); hook(1, &seasonal_destroy_hook, r.destroy);
        for (auto* target : created)
            if (hook_enable(target) != HookOk) throw std::runtime_error("Cannot enable world layer adapter");
        // Logging is optional: a failed observation hook must not disable the
        // existing world-layer controls. Publish the trampoline before enable.
        auto* event_target = reinterpret_cast<void*>(base + sites[2].rva);
        void* event_original{};
        bool event_logging{};
        if (hook_prepare(event_target, reinterpret_cast<void*>(&seasonal_event_hook), &event_original) == HookOk) {
            r.original_event = reinterpret_cast<WorldLayersRuntime::Event>(event_original);
            event_logging = hook_enable(event_target) == HookOk;
            // Retain the trampoline if enable failed; a callback may have entered.
        }
        logging::write(event_logging ? logging::Level::info : logging::Level::warning,
            logging::Channel::subworld, event_logging ? "Native subworld load/unload event logging enabled." :
                "Subworld event hook unavailable; streaming state sampling remains enabled.");
        r.active.store(true, std::memory_order_release);
        dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"local_world_layers_initialized\",\"active\":true}");
    } catch (...) {
        for (auto* target : created) hook_disable(target);
        r.model.feedback = "World layers are unavailable in this build.";
        dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"local_world_layers_initialized\",\"active\":false}");
    }
}
}
