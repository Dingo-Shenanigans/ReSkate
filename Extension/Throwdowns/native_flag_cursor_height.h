#pragma once
#include <cmath>
#include <optional>
namespace dingosdk::multiplayer {
// The native flag picker uses this movement ceiling independently of its
// ground validator. Retain the latest native value and restore only our edit:
// the registration asks for its own ceiling (kit::flag::Options::cursor_height).
class NativeFlagCursorHeight {
    float previous{},raised{};bool owned{};
public:
    std::optional<float> apply(float current,float wanted) noexcept {
        if(!std::isfinite(wanted) || wanted<=0)return {};
        if(owned && current==raised && raised==wanted)return {};
        // A new ceiling replaces ours: the native value under it is still the one to restore.
        if(owned && current==raised) {raised=wanted;return wanted;}
        owned=false;
        if(!std::isfinite(current) || current<0 || current>=wanted)return {};
        previous=current;raised=wanted;owned=true;return wanted;
    }
    std::optional<float> restore(float current) noexcept {
        const bool ours=owned && current==raised;owned=false;
        return ours?std::optional{previous}:std::nullopt;
    }
};
}
