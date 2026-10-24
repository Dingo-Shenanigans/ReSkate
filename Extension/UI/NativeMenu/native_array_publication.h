#pragma once
#include <functional>
#include <type_traits>

namespace dingosdk::multiplayer::menu_data {
// A byte snapshot of a native row still borrows its strings, delegates and
// nested arrays. Native array publication resizes/destroys destination rows
// before it copies replacements. Retain the complete source into an owned
// temporary first, and release that temporary only after publication returns.
template<class Acquire, class Stage, class Publish, class Release>
void publish_retained_array(Acquire&& acquire, Stage&& stage, Publish&& publish, Release&& release) {
    auto owner = std::invoke(acquire);
    using Owner = decltype(owner);
    static_assert(std::is_nothrow_invocable_v<Release&, Owner&>, "Native transient array release must not throw.");
    struct Guard {
        Owner& owner;
        Release& release;
        ~Guard() noexcept { std::invoke(release, owner); }
    } guard{owner, release};
    // Acquisition does not resolve/copy fields: the guard must already exist
    // if validation or a partially completed native typed copy fails.
    std::invoke(stage, owner);
    std::invoke(publish, owner);
}
} // namespace dingosdk::multiplayer::menu_data
