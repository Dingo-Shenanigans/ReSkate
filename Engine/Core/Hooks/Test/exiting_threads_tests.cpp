// The hook service's transactions (Engine/Core/Hooks/hooks.h) while other threads of the process
// are ending. A thread on its way out refuses to be suspended (access denied) before its handle is
// signaled. A transaction that met one failed at once, and in a game launch of 2026-11-26 that
// cost the overlay its swapchain hooks for the whole session. Here one thread starts short-lived
// threads without pause while a hook is attached and detached over and over, and every
// transaction has to succeed. With the hook service as it was, 6 to 11 transactions in a hundred
// were refused on the PC this was written on.
#include "Engine/Core/Hooks/hooks.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <thread>

namespace {
int failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}

using Function = int (*)(int);
std::atomic<Function> original{};

// Not inlined, and long enough for Detours to place its jump.
__declspec(noinline) int target(int value) {
    volatile int a = value;
    volatile int b = 3;
    return a * b + 1;
}

int replacement(int value) { return original.load()(value) + 1000; }
} // namespace

int main() {
    using namespace dingosdk;
    const auto ready = hook_initialize();
    expect(ready == HookOk || ready == HookAlreadyInitialized, "the hook service starts");
    void* relay{};
    const auto at = reinterpret_cast<void*>(&target);
    expect(hook_prepare(at, reinterpret_cast<void*>(&replacement), &relay) == HookOk && relay, "the hook is prepared");
    if (failures) return 1;
    original.store(reinterpret_cast<Function>(relay));

    std::atomic<bool> stop{false};
    std::atomic<unsigned> started{};
    std::thread starter([&] {
        while (!stop.load()) {
            // A thread that cannot be made is not this test's failure: try again.
            try { std::thread([] {}).detach(); started.fetch_add(1); }
            catch (const std::system_error&) { std::this_thread::yield(); }
        }
    });

    // 100 rounds are 200 transactions, about 25 s and a million threads on the PC this was written
    // on. The service as it was had 12 to 21 of them refused in each of six runs.
    constexpr int rounds = 100;
    unsigned refused = 0, wrong = 0;
    HookStatus first_status = HookOk;
    HookFailure first{};
    const auto note = [&](HookStatus status) {
        if (!refused++) { first_status = status; first = hook_last_failure(); }
    };
    const auto before = started.load();
    const auto began = std::chrono::steady_clock::now();
    for (int round = 0; round < rounds; ++round) {
        const auto on = hook_enable(at);
        if (on != HookOk) { note(on); continue; }
        if (target(2) != 1007) ++wrong;
        // A refused detach leaves the hook attached; the next round's attach then answers at once.
        const auto off = hook_disable(at);
        if (off != HookOk) note(off);
        else if (target(2) != 7) ++wrong;
    }
    const auto during = started.load() - before;
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    stop.store(true);
    starter.join();

    std::printf("%d rounds in %lld ms while %u threads started and ended: %u transaction(s) refused\n", rounds,
        static_cast<long long>(took), during, refused);
    if (refused)
        std::printf("first refusal: status=%ld (%s), operation=%s error=%ld thread=%lu\n", static_cast<long>(first_status),
            hook_status_string(first_status), first.operation, static_cast<long>(first.error),
            static_cast<unsigned long>(first.thread_id));
    // About ten thousand a round where this was written; ten a round is the least that still tests anything.
    expect(during >= 10u * static_cast<unsigned>(rounds), "threads kept starting and ending during the transactions");
    expect(refused == 0, "no transaction is refused by a thread that is ending");
    expect(wrong == 0, "the hook runs while attached and the original while detached");
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("hooks while threads end: all passed\n");
    return 0;
}
