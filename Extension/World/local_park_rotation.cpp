#include "Engine/Core/Log/logging.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "level_loading.h"
#include "Extension/UI/Overlay/overlay.h"
#include "local_park_rotation.h"
#include "local_world_layers.h"
#include "Engine/Game/Abi/native_string_read.h"
#include "Engine/Game/Build/20260929/park_rotation.h"

namespace dingosdk::profile_runtime {
// Authored BAM park-pack event adapter. See analysis/rotating-parks.md.

ParkRuntime& park_runtime() { static auto* r = new ParkRuntime; return *r; }

void reset_park_session() {
    auto& r = park_runtime();
    r.owner = r.context = 0; r.sent = {}; r.pending_lot = static_cast<unsigned>(park_lots.size());
    r.next_request = 0; r.model.ready = false;
}

bool park_owner_ready(std::uintptr_t manager, std::uintptr_t& context, bool require_park) {
    const auto base = local_runtime().base;
    std::uintptr_t current{}, vtable{}, reference{};
    std::uint32_t context_offset{};
    std::uint8_t ready{}, live{};
    // The native notification resolves manager+8 by adding the registered
    // context offset. Reject stale handles before entering its unchecked read.
    if (!read(base + addr::park_rotation::manager, current) || manager != current || !manager ||
        !read(manager, vtable) || vtable != base + addr::park_rotation::manager_vtable ||
        (require_park && (!read(manager + 0x42, ready) || ready != 1)) ||
        !read(manager + 8, reference) || !reference ||
        !read(base + addr::park_rotation::context_offset, context_offset) || context_offset > 0x1000000 ||
        reference > UINTPTR_MAX - context_offset - 0x28) return false;
    context = reference + context_offset;
    return read(context + 0x28, live) && live == 1;
}

void observe_park_readiness(std::uintptr_t manager, std::uint64_t now) noexcept {
    auto& r = park_runtime();
    if (now < r.next_observation) return;
    r.next_observation = now + 250;
    std::uintptr_t current{}, vt{};
    std::uint8_t ready{};
    if (!read(local_runtime().base + addr::park_rotation::manager, current) || current != manager ||
        !read(manager, vt) || vt != local_runtime().base + addr::park_rotation::manager_vtable ||
        !read(manager + 0x42, ready) || ready > 1) return;
    if (r.observed_owner != manager) {
        r.observed_owner = manager; r.observed_ready.reset();
    }
    if (!r.observed_ready || *r.observed_ready != (ready != 0)) {
        const auto elapsed = r.observed_ready && now >= r.observed_since ? now - r.observed_since : 0;
        logging::log(ready ? logging::Level::success : logging::Level::info, logging::Channel::park,
            "Park manager {} ({}ms in previous observed state).", ready ? "ready" : "waiting for park content", elapsed);
        r.observed_ready = ready != 0; r.observed_since = now; r.readiness_warned = false;
    } else if (!ready && !r.readiness_warned && now >= r.observed_since && now - r.observed_since >= 30000) {
        r.readiness_warned = true;
        const auto destination = last_level_destination();
        // Custom maps have none of the game's park lots, so the park manager can
        // wait forever on a level that loaded fine. Only a level that never
        // became active is a real failure worth an on-screen error.
        if (level_content_active()) {
            logging::write(logging::Level::info, logging::Channel::park,
                "Park manager has no park content to wait for; the level itself is active." +
                (destination.empty() ? std::string{} : " Level: " + destination + "."));
            return;
        }
        // A custom level whose content never arrives sits here: name it and
        // the mod behind it, so the wait can be traced to a folder.
        logging::write(logging::Level::warning, logging::Channel::park,
            "Park manager still waiting for content after 30 seconds." +
            (destination.empty() ? std::string{}
                                 : " Level: " + destination + custom_level_hint(destination) + "."));
        if (const auto notice = custom_level_notice(destination); !notice.empty())
            overlay::notify(overlay::NoticeLevel::error, "This map is not loading", notice);
    }
}

// Log notifications from both the engine and SDK at their shared native entry.
// A returned notification queues graph work; it does not prove a park loaded.
void park_notify_hook(std::uintptr_t manager, const NativeString* zone, const NativeString* park) {
    PreserveError native_error;
    const auto incoming_error = native_error.error;
    auto& r = park_runtime();
    bool observed{};
    std::string zone_name, park_name;
    try {
        if (r.active.load(std::memory_order_acquire)) {
            observed = game::read_native_identifier(reinterpret_cast<std::uintptr_t>(zone), zone_name) &&
                game::read_native_identifier(reinterpret_cast<std::uintptr_t>(park), park_name);
            if (observed)
                logging::log(logging::Level::info, logging::Channel::park,
                    "Park {} event: lot={}, layout={}.", park_name.empty() ? "clear" : "load",
                    zone_name.empty() ? "<empty>" : zone_name, park_name.empty() ? "<empty lot>" : park_name);
            else logging::write(logging::Level::debug, logging::Channel::park, "Park event names unavailable.");
        }
    } catch (...) {}
    const auto started = GetTickCount64();
    SetLastError(incoming_error);
    r.original_notify(manager, zone, park);
    native_error.error = GetLastError();
    if (observed) {
        const auto elapsed = GetTickCount64() - started;
        logging::log(elapsed >= 1000 ? logging::Level::warning : logging::Level::debug, logging::Channel::park,
            "Park event handler returned after {}ms: lot={}, layout={}; awaiting subworld streaming.",
            elapsed, zone_name, park_name.empty() ? "<empty lot>" : park_name);
    }
}

void update_park_rotation(std::uintptr_t manager, std::uint64_t now) {
    auto& r = park_runtime();
    observe_park_readiness(manager, now);
    std::uintptr_t context{};
    if (!park_owner_ready(manager, context)) { reset_park_session(); return; }
    if (r.owner != manager || r.context != context) reset_park_session();
    r.owner = manager; r.context = context; r.model.ready = true;
    if (r.launch_randomization.consume(r.model.ready, r.model.controlled_by_host))
        load_random_local_parks();
    // Serialize clear/load across ticks. The native graph's empty-name branch
    // unloads every family at this lot, including a previously selected family.
    if (now < r.next_request) return;
    // Updated only after Store commits; avoid copying the full cosmetic/profile
    // document on every server frame when no park selection has changed.
    const auto& choices = r.model.choices;
    if (r.pending_lot < park_lots.size()) {
        const auto lot = r.pending_lot;
        const auto layout = choices[lot];
        const auto native_layout = park_native_name(layout);
        if (!layout.empty() && layout != "empty") {
            NativeString zone{std::string(park_lots[lot].key)}, park{native_layout};
            r.notify(manager, &zone, &park);
        }
        r.sent[lot] = layout; r.clear_unset[lot] = false;
        r.pending_lot = static_cast<unsigned>(park_lots.size());
        r.model.feedback = r.model.controlled_by_host ? "Host's park selection applied." :
            "Park selection applied.";
        dingosdk::logging::event(dingosdk::logging::Channel::world, dingosdk::Json{{"event", "local_park_selection_sent"}, {"lot", park_lots[lot].key},
            {"layout", layout}, {"native_layout", native_layout}, {"context", context}}.dump().c_str());
        r.next_request = now + 1000;
        return;
    }
    for (unsigned lot = 0; lot < choices.size(); ++lot) {
        if (!r.clear_unset[lot] && (choices[lot].empty() || choices[lot] == r.sent[lot])) continue;
        NativeString zone{std::string(park_lots[lot].key)}, empty{std::string{}};
        r.notify(manager, &zone, &empty);
        r.pending_lot = lot; r.next_request = now + 1000;
        r.model.feedback = "Changing " + std::string(park_lots[lot].label) + "...";
        dingosdk::logging::event(dingosdk::logging::Channel::world, dingosdk::Json{{"event", "local_park_clear_sent"}, {"lot", park_lots[lot].key}}.dump().c_str());
        return;
    }
}

void park_tick_hook(std::uintptr_t manager, float delta) {
    auto& r = park_runtime(); auto& s = local_runtime();
    r.tick(manager, delta);
    if (!r.active.load(std::memory_order_acquire) || !s.active.load(std::memory_order_acquire)) return;
    PreserveError preserve;
    try {
        std::lock_guard lock(s.native_mutex);
        update_park_rotation(manager, GetTickCount64());
        update_world_layers(manager, GetTickCount64());
        update_placement_poses();
    } catch (...) {
        dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"local_park_update_failed\"}");
    }
}

void park_construct_hook(std::uintptr_t context) {
    park_runtime().construct(context);
    if (!park_runtime().active.load(std::memory_order_acquire)) return;
    PreserveError preserve;
    std::lock_guard lock(local_runtime().native_mutex);
    reset_park_session(); // also handles allocators reusing a previous manager
    auto& r = park_runtime();
    r.observed_owner = 0; r.observed_ready.reset(); r.next_observation = 0;
    logging::write(logging::Level::info, logging::Channel::park, "Park context constructed; waiting for park content.");
}

void start_park_rotation() noexcept {
    auto& r = park_runtime(); const auto base = local_runtime().base;
    const auto& sites = addr::park_rotation::sites;
    std::vector<void*> created;
    try {
        r.model.choices = profile::park_choices(*local_runtime().store->shared_snapshot());
        r.model.randomize_on_launch = local_preference("RandomizeParksOnLaunch").value_or(false);
        r.launch_randomization.pending = r.model.randomize_on_launch;
        for (const auto& site : sites) {
            std::array<unsigned char, 16> bytes{};
            if (!read(base + site.rva, bytes) || bytes != site.bytes)
                throw std::runtime_error("Park adapter fingerprint mismatch");
        }
        const auto hook = [&](unsigned i, auto detour, auto& original) {
            auto* target = reinterpret_cast<void*>(base + sites[i].rva);
            if (hook_prepare(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) != HookOk)
                throw std::runtime_error("Cannot prepare park adapter");
            created.push_back(target);
        };
        r.notify = reinterpret_cast<ParkRuntime::Notify>(base + sites[2].rva);
        hook(0, &park_tick_hook, r.tick); hook(1, &park_construct_hook, r.construct);
        for (auto* target : created)
            if (hook_enable(target) != HookOk) throw std::runtime_error("Cannot enable park adapter");
        auto* notify_target = reinterpret_cast<void*>(base + sites[2].rva);
        void* notify_original{};
        bool notify_logging{};
        if (hook_prepare(notify_target, reinterpret_cast<void*>(&park_notify_hook), &notify_original) == HookOk) {
            r.original_notify = reinterpret_cast<ParkRuntime::Notify>(notify_original);
            notify_logging = hook_enable(notify_target) == HookOk;
        }
        logging::write(notify_logging ? logging::Level::info : logging::Level::warning, logging::Channel::park,
            notify_logging ? "Native park load/clear event logging enabled." :
                "Park event hook unavailable; park readiness sampling remains enabled.");
        r.active.store(true, std::memory_order_release);
        dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"local_parks_initialized\",\"active\":true}");
    } catch (...) {
        for (auto* target : created) hook_disable(target);
        r.model.feedback = "Park selection is unavailable in this build.";
        dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"local_parks_initialized\",\"active\":false}");
    }
}
}
