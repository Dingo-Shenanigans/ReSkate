#include "Engine/Game/BMX/pose_listener_set.h"
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>
namespace {
using Callback = void (*)(int) noexcept;
using Listeners = dingosdk::bmx::PoseListenerSet<Callback, 2>;
std::vector<int> calls;
Listeners* current{};
void first(int value) noexcept { calls.push_back(value); }
void second(int value) noexcept { calls.push_back(value + 1); }
void legacy(int value) noexcept { calls.push_back(value + 2); }
void self_remove(int value) noexcept { current->remove(&self_remove); calls.push_back(value); }
std::uintptr_t observed_interface{},observed_data{};
void detailed(std::uintptr_t animation_interface,std::uintptr_t data) noexcept {observed_interface=animation_interface;observed_data=data;}
void check(bool value, const char* reason) {
    if (!value) { std::cerr << reason << '\n'; std::exit(1); }
}
}
int main() {
    Listeners list; current = &list;
    using Detailed=void (*)(std::uintptr_t,std::uintptr_t) noexcept;
    dingosdk::bmx::PoseListenerSet<Detailed,2> published;
    check(published.add(detailed)&&published.add(detailed),"detailed listener registration is idempotent");
    published.dispatch(nullptr,std::uintptr_t{0x12340},std::uintptr_t{0x56780});
    check(observed_interface==0x12340&&observed_data==0x56780,"both borrowed publisher arguments are delivered unchanged");
    published.remove(detailed);observed_interface=observed_data=0;
    published.dispatch(nullptr,std::uintptr_t{1},std::uintptr_t{2});
    check(!observed_interface&&!observed_data,"removed detailed callback receives no new publication");
    check(!list.add(nullptr), "null registration must fail");
    check(list.add(first) && list.add(first) && list.add(second), "idempotent add consumes one slot");
    check(!list.add(legacy), "capacity is bounded");
    list.dispatch(legacy, 10);
    check(calls == std::vector<int>({10, 11, 12}), "extra pose edits precede legacy head capture");
    calls.clear(); list.dispatch(first, 20);
    check(calls == std::vector<int>({21, 20}), "same callback in legacy slot runs only once");
    list.remove(first); list.remove(first); list.remove(nullptr); calls.clear(); list.dispatch(nullptr, 30);
    check(calls == std::vector<int>({31}), "remove is idempotent and keeps other listeners");
    list.remove(second); check(list.add(self_remove), "free slot can be reused"); calls.clear();
    list.dispatch(nullptr, 40); list.dispatch(nullptr, 41);
    check(calls == std::vector<int>({40}), "callbacks can remove themselves without a dispatch lock");
    std::thread a([&] { for (int i = 0; i < 1000; ++i) list.add(first); });
    std::thread b([&] { for (int i = 0; i < 1000; ++i) list.add(first); }); a.join(); b.join();
    check(list.add(second), "concurrent duplicate registration cannot consume both slots");
    calls.clear(); list.dispatch(nullptr, 50);
    check(calls == std::vector<int>({50, 51}), "concurrent add leaves exactly one registration");
    std::cout << "BMX pose listener tests passed\n";
}
