#include "Engine/Core/Hooks/callback_listeners.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <semaphore>
#include <thread>

namespace {
using Callback = void (*)(std::uintptr_t) noexcept;
using Listeners = dingosdk::CallbackListeners<Callback, 2>;
std::array<int, 16> calls{};
std::size_t count{}, assertions{};
std::uintptr_t argument{};
Listeners* changing{};
std::binary_semaphore entered{0}, resume{0};

void check(bool condition, const char* why) {
    ++assertions;
    if (!condition) { std::fprintf(stderr, "%s\n", why); std::exit(1); }
}
void record(int value) noexcept {
    if (count == calls.size()) std::abort();
    calls[count++] = value;
}
void first(std::uintptr_t value) noexcept { argument = value; record(1); }
void second(std::uintptr_t) noexcept { record(2); }
void third(std::uintptr_t) noexcept { record(3); }
void change(std::uintptr_t) noexcept {
    record(1);
    changing->remove(&second);
    check(changing->add(&third), "A callback can register without a dispatch-held lock");
}
void blocking(std::uintptr_t value) noexcept {
    argument = value;
    record(1);
    entered.release();
    resume.acquire();
}
void reset() { calls = {}; count = 0; argument = 0; }

void registration_and_capacity() {
    reset(); Listeners listeners;
    check(!listeners.add(nullptr), "Null registration is refused");
    listeners.remove(nullptr);
    check(listeners.add(&first), "First callback registers");
    check(listeners.add(&first), "Duplicate add is idempotent");
    check(listeners.add(&second), "Duplicate did not consume capacity");
    check(!listeners.add(&third), "A full registry refuses rather than replacing an owner");
    listeners.dispatch(std::uintptr_t{42});
    check(count == 2 && calls[0] == 1 && calls[1] == 2 && argument == 42,
        "Dispatch calls each registered observer once with the actual argument");
    listeners.remove(&first); listeners.remove(&first);
    check(listeners.add(&third), "Removed capacity is reusable");
    reset(); listeners.dispatch(std::uintptr_t{7});
    check(count == 2 && calls[0] == 3 && calls[1] == 2,
        "Removal does not replace an unrelated observer");
    reset();
    // A remove/re-add can move one observer between separate atomic slot reads.
    dingosdk::callback_listeners_detail::dispatch(std::array<Callback, 2>{&first, &first}, std::uintptr_t{51});
    check(count == 1 && calls[0] == 1 && argument == 51,
        "A hybrid slot snapshot cannot deliver the same observer twice");
}

void callback_mutation_snapshot() {
    reset(); Listeners listeners; changing = &listeners;
    check(listeners.add(&change) && listeners.add(&second), "Mutation callbacks register");
    listeners.dispatch(std::uintptr_t{});
    check(count == 2 && calls[0] == 1 && calls[1] == 2,
        "Removal during dispatch preserves the already captured snapshot");
    reset(); listeners.dispatch(std::uintptr_t{});
    check(count == 2 && calls[0] == 1 && calls[1] == 3,
        "New callback first appears in the next snapshot");
    changing = nullptr;
}

void concurrent_removal_snapshot() {
    reset(); Listeners listeners;
    check(listeners.add(&blocking) && listeners.add(&second), "Concurrent fixture registers");
    std::thread worker([&] { listeners.dispatch(std::uintptr_t{99}); });
    entered.acquire();
    listeners.remove(&second);
    check(listeners.add(&third), "Registration completes while a callback remains in flight");
    listeners.remove(&blocking);
    resume.release(); worker.join();
    check(count == 2 && calls[0] == 1 && calls[1] == 2 && argument == 99,
        "An in-flight snapshot survives concurrent removal without adding the new observer");
    reset(); listeners.dispatch(std::uintptr_t{});
    check(count == 1 && calls[0] == 3, "Future dispatch sees only the surviving observer");
}

void idle_epoch_and_overlap() {
    dingosdk::CallbackEpoch epoch;
    std::uint64_t before{}, after{};
    check(epoch.idle(before) && before, "Initial idle epoch is available");
    epoch.enter();
    check(!epoch.idle(after) && !after, "An active invocation refuses a reader");
    epoch.enter(); epoch.leave();
    check(!epoch.idle(after) && !after, "One exit cannot clear an overlapping invocation");
    epoch.leave();
    check(epoch.idle(after) && after != before, "Completed invocations invalidate the prior epoch");
    before = after; epoch.enter(); epoch.leave();
    check(epoch.idle(after) && before != after,
        "An invocation entirely between two owner reads is still detected");
}

void registration_during_invocation() {
    reset(); Listeners listeners; dingosdk::CallbackEpoch epoch;
    std::uint64_t before{}, after{};
    check(epoch.idle(before), "Pre-registration idle epoch exists");
    epoch.enter();
    listeners.dispatch(std::uintptr_t{}); // The old empty snapshot misses this observer.
    check(listeners.add(&first), "Observer may register during an old invocation");
    check(!epoch.idle(after), "Owner capture cannot admit through that unobserved invocation");
    epoch.leave();
    check(epoch.idle(after) && after != before && count == 0,
        "The always-on epoch covers the invocation the observer did not receive");
    epoch.enter(); listeners.dispatch(std::uintptr_t{8}); epoch.leave();
    check(count == 1 && argument == 8, "Later invocations notify the permanent observer");
}

void invalid_pairing_fails_closed() {
    dingosdk::CallbackEpoch epoch;
    std::uint64_t out{};
    epoch.leave();
    check(!epoch.idle(out) && !out, "An unbalanced exit never publishes an idle epoch");
    epoch.enter(); epoch.leave();
    check(!epoch.idle(out), "A corrupted invocation counter cannot recover by accident");
}
} // namespace

int main() {
    registration_and_capacity();
    callback_mutation_snapshot();
    concurrent_removal_snapshot();
    idle_epoch_and_overlap();
    registration_during_invocation();
    invalid_pairing_fails_closed();
    std::printf("callback_listeners: 6 cases, %zu assertions\n", assertions);
}
