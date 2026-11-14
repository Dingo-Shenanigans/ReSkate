#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "Engine/Game/Skater/teleport_observation.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

// Compile the real profile entry points so their queue selection is tested,
// rather than reconstructing the selection with a test-only bool. Runtime
// accessors supply constructed state; safe native reads use its manager slot.
// Unrelated profile/native links below fail closed and must never be invoked.
#include "Extension/Profile/local_profile_objects.cpp"

namespace profile_source_fixture { unsigned unrelated_calls{}; }
namespace dingosdk::profile_runtime {
LocalRuntime& local_runtime() { static LocalRuntime runtime; return runtime; }
PlacementsRuntime& placements_runtime() { static PlacementsRuntime runtime; return runtime; }
bool placement_session_ready() { ++profile_source_fixture::unrelated_calls; return false; }
bool begin_placement_delete(const std::set<std::uint64_t>&) { ++profile_source_fixture::unrelated_calls; return false; }
bool park_editor_owns_placements() { ++profile_source_fixture::unrelated_calls; return true; }
WorldLayersRuntime& world_layers_runtime() {
    ++profile_source_fixture::unrelated_calls;
    static WorldLayersRuntime runtime;
    return runtime;
}
}
namespace dingosdk {
bool local_surface_hit_body(std::uintptr_t, std::uint32_t, std::uintptr_t, std::array<std::uintptr_t, 2>&) {
    ++profile_source_fixture::unrelated_calls; return false;
}
namespace skater_action {
bool enter_motion_writer(actor_control::IdleCall&) noexcept { ++profile_source_fixture::unrelated_calls; return false; }
}
}

using namespace dingosdk::teleport_observation;
namespace {
unsigned checks{}, groups{};
void check(bool value, std::string_view name) {
    ++checks;
    if (!value) throw std::runtime_error(std::string(name));
}
struct Reader {
    Queue* queue{};
    Identity current{true, 0x1000};
    std::uintptr_t manager{0x2000};
    std::uint32_t phase{};
    unsigned identity_calls{}, manager_calls{}, phase_calls{};
    bool manager_ok{true}, phase_ok{true}, throw_read{}, mutate_read{}, retire_read{}, change_base{};
    bool replace_manager{}, become_busy{}, lifecycle_aba{};
    std::uint32_t get_error() const noexcept { return GetLastError(); }
    void set_error(std::uint32_t value) const noexcept { SetLastError(value); }
    Identity identity() noexcept { ++identity_calls; return current; }
    bool read_manager(std::uintptr_t base, std::uintptr_t& out) {
        ++manager_calls;
        check(base == 0x1000, "native manager uses captured profile base");
        SetLastError(222);
        if (throw_read) throw std::runtime_error("constructed read fault");
        if (mutate_read) {
            auto mutation = queue->mutate();
            mutation.write().position_teleport_at = 42;
        }
        if (retire_read) current.active = false;
        if (lifecycle_aba) {
            auto mutation = queue->mutate(); mutation.lifecycle_changed();
            current.active = false;
            current.active = true;
        }
        if (change_base) current.base = 0x3000;
        out = replace_manager && manager_calls % 2 == 0 ? manager + 0x100 : manager;
        return manager_ok;
    }
    bool read_phase(std::uintptr_t value, std::uint32_t& out) noexcept {
        ++phase_calls;
        if (value != manager) return false;
        SetLastError(333);
        out = become_busy && phase_calls % 2 == 0 ? 1u : phase;
        return phase_ok;
    }
};
Result observe(Queue& queue, Reader& reader) {
    SetLastError(12345);
    const auto result = queue.observe(reader);
    check(GetLastError() == 12345, "observer preserves LastError on every result");
    return result;
}
void categories() {
    ++groups;
    Queue queue; Reader reader{&queue};
    auto result = observe(queue, reader);
    check(result.known && !result.pending && result.revision == 1, "fresh native idle is known");
    reader.phase = 2;
    check(observe(queue, reader).pending, "fresh native nonzero phase is pending");
    check(reader.manager_calls == 4 && reader.phase_calls == 4, "idle reads never cache native phase");
    reader.current.active = false;
    check(observe(queue, reader).observation == Observation::inactive, "inactive profile is unavailable");
    reader.current.active = true; reader.manager_ok = false;
    check(observe(queue, reader).observation == Observation::manager_read_failed, "manager failure is distinguished");
    reader.manager_ok = true; reader.manager = 0;
    check(observe(queue, reader).observation == Observation::manager_null, "manager null is distinguished");
    reader.manager = 0x2000; reader.phase_ok = false;
    check(observe(queue, reader).observation == Observation::phase_read_failed, "phase failure is distinguished");
    reader.phase_ok = true; reader.throw_read = true;
    check(observe(queue, reader).observation == Observation::exception, "exception is unavailable");
}
void queues_and_reentrance() {
    ++groups;
    Queue queue; Reader reader{&queue};
    {
        auto mutation = queue.mutate();
        mutation.write().position_teleport = std::array<float, 16>{};
        mutation.write().position_teleport_at = 100;
    }
    auto result = observe(queue, reader);
    check(result.known && result.pending && !reader.manager_calls, "actual position queue is pending without native reads");
    {
        auto mutation = queue.mutate();
        mutation.write().position_teleport.reset();
        mutation.write().teleport_token = 77;
    }
    result = observe(queue, reader);
    check(result.known && !result.pending, "placement token queue preserves existing observation semantics");
    {
        auto dispatch = queue.mutate();
        dispatch.begin_dispatch();
        dispatch.write().teleport_token = 0;
        result = observe(queue, reader);
        check(result.known && result.pending, "same-thread placement callback after token clear sees dispatch pending");
        {
            auto nested = queue.mutate(); nested.begin_dispatch();
            check(observe(queue, reader).pending, "nested dispatch stays pending");
            nested.write().position_teleport = std::array<float, 16>{};
            nested.write().position_teleport_at = 200;
        }
        check(observe(queue, reader).pending, "outer dispatch remains busy after nested dispatch returns");
    }
    {
        auto read = queue.mutate();
        check(read.read().position_teleport.has_value() && read.read().position_teleport_at == 200,
            "nested enqueue survives outer dispatcher teardown");
    }
    check(observe(queue, reader).pending, "nested queued position remains pending");
}
void both_accepted_queues_can_be_observed() {
    ++groups;
    Queue queue; Reader reader{&queue};
    const auto original = observe(queue, reader);
    {
        auto mutation = queue.mutate();
        mutation.write().teleport_token = 77;
        mutation.write().teleport_queued_at = 100;
    }
    const auto position_only = observe(queue, reader);
    check(position_only.known && !position_only.pending,
        "position-only observer retains its existing placement-token semantics");
    const auto native_reads = reader.manager_calls + reader.phase_calls;
    SetLastError(12345);
    const auto both = queue.observe(reader, true);
    check(GetLastError() == 12345 && both.known && both.pending,
        "paired observer reports accepted placement work and preserves LastError");
    check(reader.manager_calls + reader.phase_calls == native_reads,
        "an accepted placement request unnecessarily read a native manager");
    check(!same_idle(original, position_only),
        "placement enqueue must advance the shared revision even for position-only observations");
    {
        auto dispatch = queue.mutate();
        dispatch.begin_dispatch();
        dispatch.write().teleport_token = 0;
        check(queue.observe(reader, true).pending,
            "clearing a placement token cannot hide its current dispatch");
    }
    const auto completed = queue.observe(reader, true);
    check(completed.known && !completed.pending && !same_idle(position_only, completed),
        "completed placement dispatch reports new idle history without reviving an old witness");
}
void per_request_equipment() {
    ++groups;
    Queue queue;
    auto mutation = queue.mutate();
    auto& state = mutation.write();
    std::array<float, 16> first{}, second{};
    first[12] = 10; second[12] = 20;
    state.set_position(first, true, 100);
    check(state.position_teleport == first && state.position_teleport_on_board == true &&
        state.position_teleport_at == 100, "explicit board option is coupled to its exact queued transform");
    state.set_position(second, false, 200);
    check(state.position_teleport == second && state.position_teleport_on_board == false &&
        state.position_teleport_at == 200, "explicit false is retained with the replacement transform");
    state.set_position(first, std::nullopt, 300);
    check(state.position_teleport == first && !state.position_teleport_on_board.has_value() &&
        state.position_teleport_at == 300, "legacy enqueue clears the prior explicit option");
    state.set_position(second, true, 400);
    const auto dispatched_option = state.position_teleport_on_board;
    state.clear_position();
    check(!state.position_teleport && !state.position_teleport_on_board && state.position_teleport_at == 400,
        "consume or expiry clears both fields without renewing the original stamp");
    state.set_position(first, std::nullopt, 500);
    check(!state.position_teleport_on_board && state.position_teleport == first,
        "a later legacy request cannot inherit the cleared option");
    check(dispatched_option == true, "a captured dispatch option is not changed by a later enqueue");
}
void dispatch_destruction() {
    ++groups;
    Queue queue; Reader reader{&queue};
    unsigned stages{};
    try {
        auto dispatch = queue.mutate(); dispatch.begin_dispatch();
        dispatch.write().position_teleport.reset();
        check(observe(queue, reader).pending, "clear-before-constructor interval is pending"); ++stages;
        struct Transition {
            Queue& queue; Reader& reader; unsigned& stages;
            ~Transition() {
                check(observe(queue, reader).pending, "native transition destructor remains inside busy scope"); ++stages;
            }
        } transition{queue, reader, stages};
        check(observe(queue, reader).pending, "native teleport body remains inside busy scope"); ++stages;
        throw std::runtime_error("constructed native C++ unwind");
    } catch (const std::runtime_error&) {}
    check(stages == 3, "constructor/body/destructor observations executed");
    const auto result = observe(queue, reader);
    check(result.known && !result.pending, "dispatch depth drains after transition teardown and C++ unwind");
}
void contention() {
    ++groups;
    Queue queue; Reader reader{&queue};
    std::recursive_mutex broad;
    std::promise<void> broad_held, release_broad;
    auto release = release_broad.get_future();
    std::thread unrelated([&] { std::lock_guard lock(broad); broad_held.set_value(); release.wait(); });
    broad_held.get_future().wait();
    const auto independent = observe(queue, reader);
    release_broad.set_value(); unrelated.join();
    check(independent.known && !independent.pending, "unrelated broad-profile holder cannot refuse narrow observer");
    std::promise<void> queue_held, release_queue;
    auto queue_release = release_queue.get_future();
    std::thread writer([&] { std::lock_guard owner(broad); auto mutation = queue.mutate();
        queue_held.set_value(); queue_release.wait(); });
    queue_held.get_future().wait();
    const auto refused = observe(queue, reader);
    release_queue.set_value(); writer.join();
    check(!refused.known && refused.pending && refused.observation == Observation::lock_refused,
        "real other-thread queue guard remains Try-only unavailable");
}
void fences_and_exhaustion() {
    ++groups;
    Queue queue; Reader reader{&queue}; reader.mutate_read = true;
    auto result = observe(queue, reader);
    check(!result.known && result.pending && result.observation == Observation::queue_changed,
        "recursive mutation during native read cannot publish idle");
    reader.mutate_read = false; reader.retire_read = true;
    check(observe(queue, reader).observation == Observation::inactive, "profile retirement closes observation");
    reader.retire_read = false; reader.current.active = true; reader.change_base = true;
    check(observe(queue, reader).observation == Observation::queue_changed, "base replacement closes observation");
    Queue exhausted(std::numeric_limits<std::uint64_t>::max()); Reader limit_reader{&exhausted};
    check(observe(exhausted, limit_reader).known, "last nonzero revision can be observed before mutation");
    { auto mutation = exhausted.mutate(); mutation.write().position_teleport_at = 1; }
    check(observe(exhausted, limit_reader).observation == Observation::queue_exhausted, "revision never wraps after last mutation");
    { auto mutation = exhausted.mutate(); mutation.write().position_teleport = std::array<float, 16>{}; }
    check(!observe(exhausted, limit_reader).known, "later producer mutation cannot clear exhausted observation state");
    Queue zero(0); Reader invalid{&zero};
    check(observe(zero, invalid).observation == Observation::queue_exhausted, "zero initial revision is unavailable");
    Queue endpoint; Reader close{&endpoint}; close.replace_manager = true;
    check(observe(endpoint, close).observation == Observation::queue_changed, "manager replacement within observation refuses idle");
    close.replace_manager = false; close.become_busy = true;
    check(observe(endpoint, close).pending, "native busy at closing read remains pending");
    close.become_busy = false;
    const auto before = observe(endpoint, close);
    check(same_idle(before, observe(endpoint, close)), "unchanged endpoints qualify transient same-idle witness");
    close.manager += 0x100;
    check(!same_idle(before, observe(endpoint, close)), "same queue revision at different manager cannot qualify witness");
    close.manager = before.manager;
    { auto mutation = endpoint.mutate(); mutation.write().position_teleport = std::array<float, 16>{}; }
    { auto mutation = endpoint.mutate(); mutation.write().position_teleport.reset(); }
    check(!same_idle(before, observe(endpoint, close)), "enqueue-clear ABA after an idle observation changes witness");
    Queue lifecycle; Reader replaced{&lifecycle}; replaced.lifecycle_aba = true;
    check(observe(lifecycle, replaced).observation == Observation::queue_changed,
        "guarded false-true lifecycle interval cannot masquerade as same active/base");
    { auto mutation = lifecycle.mutate(); mutation.lifecycle_changed(); }
    replaced.lifecycle_aba = false;
    check(observe(lifecycle, replaced).known, "fresh operation can observe the new stable lifecycle");
}
void qualification_composition() {
    ++groups;
    Queue queue; Reader reader{&queue};
    // This is only the two actual helper endpoints and the same predicate
    // consumed by action_validate. It makes no fake owner/native completion.
    const auto closed_idle = [&](auto between) {
        const auto opening = observe(queue, reader);
        between();
        const auto closing = observe(queue, reader);
        return same_idle(opening, closing);
    };
    check(closed_idle([] {}), "unchanged idle can close a transient controls observation");
    check(!closed_idle([&] {
        { auto mutation = queue.mutate(); mutation.write().position_teleport = std::array<float, 16>{}; }
        { auto mutation = queue.mutate(); mutation.write().position_teleport.reset(); }
    }), "enqueue-clear between opening and full recapture closure prevents qualification");
    check(!closed_idle([&] {
        auto lifecycle = queue.mutate(); lifecycle.lifecycle_changed();
        reader.current.active = false;
        reader.current.active = true;
    }), "stamped same-value lifecycle replacement between endpoints prevents qualification");
    check(!closed_idle([&] { reader.manager += 0x100; }),
        "different native manager after owner/world recapture prevents qualification");
    check(!closed_idle([&] { reader.phase = 1; }),
        "native phase becoming busy at closing endpoint prevents qualification");
    reader.phase = 0;
    check(!closed_idle([&] { reader.current.active = false; }),
        "inactive closing lifecycle cannot become a qualified controls result");
    reader.current.active = true;
    check(closed_idle([] {}), "a new stable operation does not reuse any previous opening result");
}
void production_profile_wiring() {
    ++groups;
    using namespace dingosdk;
    auto& runtime = profile_runtime::local_runtime();
    auto& queue = profile_runtime::placements_runtime().teleport_queue;
    std::uint32_t native_phase{};
    std::uintptr_t manager_slot = reinterpret_cast<std::uintptr_t>(&native_phase);
    // The real observer adds the supported-build manager offset. Point that
    // resulting read at this process's constructed slot, not a native image.
    runtime.base = reinterpret_cast<std::uintptr_t>(&manager_slot) - addr::local_placements::teleport_manager;
    runtime.active.store(true, std::memory_order_release);
    bool pending{};
    LocalTeleportObservation observation;
    const auto actor_observe = [&] {
        Result result;
        SetLastError(12345);
        const auto known = profile_runtime::observe_local_skater_teleport(pending, observation, result);
        check(GetLastError() == 12345, "production actor observer preserves LastError");
        check(known == result.known && pending == result.pending,
            "production actor wrapper publishes the actual queue result");
        return result;
    };
    const auto idle = actor_observe();
    check(idle.known && !idle.pending && same_idle(idle, actor_observe()),
        "production actor endpoints qualify unchanged native idle");
    {
        auto mutation = queue.mutate();
        mutation.write().teleport_token = 77;
        mutation.write().teleport_queued_at = 100;
    }
    const auto opening = actor_observe();
    const auto closing = actor_observe();
    check(opening.known && opening.pending && closing.known && closing.pending && !same_idle(opening, closing),
        "placement accepted before opening cannot qualify actor admission at either real endpoint");
    SetLastError(12345);
    check(try_local_skater_teleport_pending(pending, observation) && !pending &&
        observation == LocalTeleportObservation::known && GetLastError() == 12345,
        "production position-dispatch query ignores only its own waiting placement token");
    check(try_local_skater_teleport_pending(pending) && !pending && !local_skater_teleport_pending(),
        "both legacy pending overloads retain the position/native-dispatch contract");
    {
        auto dispatch = queue.mutate();
        dispatch.begin_dispatch();
        check(actor_observe().pending && try_local_skater_teleport_pending(pending) && pending,
            "real actor and dispatch queries both observe placement dispatch before token clear");
        dispatch.write().teleport_token = 0;
        check(actor_observe().pending && try_local_skater_teleport_pending(pending) && pending &&
            local_skater_teleport_pending(),
            "real queries retain dispatch busy after its placement token is cleared");
    }
    const auto completed = actor_observe();
    check(completed.known && !completed.pending && !same_idle(idle, completed),
        "production actor observer gets new idle history after placement dispatch drains");
    {
        auto mutation = queue.mutate();
        mutation.write().set_position({}, std::nullopt, 200);
    }
    check(actor_observe().pending && try_local_skater_teleport_pending(pending) && pending,
        "production actor and dispatch queries both include accepted position work");
    { auto mutation = queue.mutate(); mutation.write().clear_position(); }
    std::promise<void> held, release;
    auto released = release.get_future();
    std::thread producer([&] { auto mutation = queue.mutate(); held.set_value(); released.wait(); });
    held.get_future().wait();
    const auto refused = actor_observe();
    release.set_value(); producer.join();
    check(!refused.known && refused.pending && refused.observation == Observation::lock_refused,
        "production actor wrapper cannot wait for a competing actual queue mutation");
    runtime.active.store(false, std::memory_order_release);
    const auto inactive = actor_observe();
    check(!inactive.known && inactive.pending && observation == LocalTeleportObservation::inactive,
        "production actor wrapper fails closed after profile retirement");
    check(!try_local_skater_teleport_pending(pending, observation) && pending &&
        observation == LocalTeleportObservation::inactive,
        "production position-dispatch query also fails closed after profile retirement");
    check(profile_source_fixture::unrelated_calls == 0,
        "production pending observations never invoke unrelated native/profile operations");
    runtime.base = 0;
}
}
int main() {
    try {
        categories(); queues_and_reentrance(); dispatch_destruction(); contention(); fences_and_exhaustion();
        qualification_composition(); per_request_equipment(); both_accepted_queues_can_be_observed();
        production_profile_wiring();
        std::cout << "PASS " << groups << " groups " << checks << " checks; constructed SDK queue/native-read facts only\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
