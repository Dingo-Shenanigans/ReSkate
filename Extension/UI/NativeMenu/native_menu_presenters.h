#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

// Pure helpers for adapting a live native page (throwdown_menu.cpp), after
// SmokeyDawg58's native_menu_ownership.h and native_hud_values.h.
namespace dingosdk::multiplayer::menu_data {
// Register the private identity before following references. Native menus can
// share mutable labels and refer back to a model already being copied.
template<class Key, class Model> class UiCloneCache {
    std::map<Key, Model> copies_;
public:
    template<class Create, class Follow> Model clone(const Key& key, Create&& create, Follow&& follow) {
        if (const auto found = copies_.find(key); found != copies_.end()) return found->second;
        const auto model = create();
        copies_.emplace(key, model);
        follow(model);
        return model;
    }
};

// Native array copies have independent storage and native stack items animate
// between menu ticks. Only the Content presenters' identities prove a remount.
template<class Presenter> bool matching_presenters(std::span<const std::byte> live, std::span<const std::byte> saved,
                                                   std::size_t stride, std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<Presenter>);
    if (!stride || offset > stride || sizeof(Presenter) > stride - offset || live.size() % stride || saved.size() % stride)
        throw std::invalid_argument("Native presenter array layout differs.");
    if (live.size() != saved.size()) return false;
    for (std::size_t i = 0; i < live.size(); i += stride) {
        Presenter current{}, expected{};
        std::memcpy(&current, live.data() + i + offset, sizeof(current));
        std::memcpy(&expected, saved.data() + i + offset, sizeof(expected));
        if (current.blueprint != expected.blueprint || current.data.record != expected.data.record ||
            current.data.handle != expected.data.handle) return false;
    }
    return true;
}
// The array to publish so `live` presents `saved`'s presenters, or none when
// it already does. With the same slots every live visibility, transition and
// focus byte is kept; a different number of slots needs the saved array.
template<class Presenter> std::optional<std::vector<std::byte>> remount_presenters(
        std::span<const std::byte> live, std::span<const std::byte> saved, std::size_t stride, std::size_t offset) {
    if (matching_presenters<Presenter>(live, saved, stride, offset)) return std::nullopt;
    if (live.size() != saved.size()) return std::vector<std::byte>(saved.begin(), saved.end());
    std::vector<std::byte> next(live.begin(), live.end());
    for (std::size_t i = 0; i < next.size(); i += stride)
        std::memcpy(next.data() + i + offset, saved.data() + i + offset, sizeof(Presenter));
    return next;
}

// A native slider's normalized 0..1 value as a whole choice on its own steps,
// and back.
inline int native_slider_choice(float normalized, int minimum, int maximum, int step) {
    if (!std::isfinite(normalized) || maximum <= minimum || step < 1) return minimum;
    const auto steps = (maximum - minimum) / step;
    const auto index = static_cast<int>(std::lround(std::clamp(normalized, 0.f, 1.f) * static_cast<float>(steps)));
    return minimum + std::min(index, steps) * step;
}
inline float native_slider_position(int choice, int minimum, int maximum) {
    if (maximum <= minimum) return 0.f;
    return static_cast<float>(std::clamp(choice, minimum, maximum) - minimum) / static_cast<float>(maximum - minimum);
}
} // namespace dingosdk::multiplayer::menu_data
