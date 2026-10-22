#include "filmer.h"
#include <chrono>
#include <cmath>
#include <mutex>

// The snapshot the menu page and the look read. Kept apart from filmer.cpp so the overlay library
// links without the game-facing half.
namespace dingosdk::filmer {
namespace {
struct Feed {
    std::mutex mutex;
    std::shared_ptr<const View> view = std::make_shared<const View>();
};
Feed &feed() {
    static auto *value = new Feed;
    return *value;
}
} // namespace

std::shared_ptr<const View> view() noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    return f.view;
}

void publish(std::shared_ptr<const View> next) noexcept {
    if (!next) return;
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    f.view = std::move(next);
}

double now() noexcept {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

const char *mode_name(Mode mode) noexcept {
    switch (mode) {
    case Mode::follow: return "follow";
    case Mode::film: return "film";
    case Mode::off: break;
    }
    return "off";
}

const char *style_name(Style style) noexcept {
    switch (style) {
    case Style::behind: return "behind";
    case Style::side: return "side";
    case Style::ahead: return "ahead";
    case Style::automatic: break;
    }
    return "auto";
}

const char *lens_name(Lens lens) noexcept { return lens == Lens::long_lens ? "long" : "fisheye"; }

void apply_look_preset(Look &look, LookPreset preset) noexcept {
    const bool anywhere = look.anywhere;
    look = {};
    look.anywhere = anywhere;
    switch (preset) {
    case LookPreset::clean: look.enabled = false; break;
    case LookPreset::worn:
        look.age = 0.8f;
        look.date = true;
        break;
    case LookPreset::vx:
    case LookPreset::custom: break;
    }
}

LookPreset look_preset(const Look &look) noexcept {
    if (!look.enabled) return LookPreset::clean;
    for (const auto preset : {LookPreset::vx, LookPreset::worn}) {
        Look want;
        apply_look_preset(want, preset);
        if (look.fisheye == want.fisheye && std::abs(look.bend - want.bend) < 0.005f && look.bars == want.bars && look.hood == want.hood &&
            look.grade == want.grade && look.soft == want.soft && look.scanlines == want.scanlines && look.grain == want.grain &&
            look.date == want.date && look.viewfinder == want.viewfinder && std::abs(look.age - want.age) < 0.005f &&
            look.dropouts == want.dropouts && look.head_switching == want.head_switching && look.tracking == want.tracking &&
            look.smear == want.smear && look.iris == want.iris)
            return preset;
    }
    return LookPreset::custom;
}
} // namespace dingosdk::filmer
