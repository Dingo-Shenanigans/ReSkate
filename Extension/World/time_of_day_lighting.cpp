#include "time_of_day_lighting.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Settings/named_settings.h"
#include <atomic>

namespace dingosdk::time_of_day_lighting {
namespace {
std::atomic<std::uint64_t> changed{}; // when a time layer last changed (0: never)
Hold& hold() { static auto* value = new Hold; return *value; } // the settings may still be held at exit
}

void time_layer_changed(std::uint64_t now) noexcept { changed.store(now ? now : 1, std::memory_order_relaxed); }

void tick(std::uint64_t now) noexcept {
    try {
        auto& h = hold();
        const auto at = changed.load(std::memory_order_relaxed);
        if (!at && !h.holding()) return;
        const bool was = h.holding();
        h.step(now, at,
            [](const char* name) { return named_setting_value(name); },
            [](const char* name, const std::string& value) {
                return !change_named_setting(name, value, false).starts_with("error: ");
            });
        if (was != h.holding())
            logging::log(logging::Level::info, logging::Channel::world, "Time of day: the shadow and sky caches are {}.",
                h.holding() ? "off while the new time's light is worked out" : "back as they were");
    } catch (...) {}
}
}
