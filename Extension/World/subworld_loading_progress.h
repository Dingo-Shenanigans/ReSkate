#pragma once
#include "Engine/Core/Log/types.h"
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace dingosdk::subworld_logging {
using Emit = void (*)(logging::Level, std::string_view);
// SDK-owned observations only; a native request handle is never a completion.
struct Progress {
    std::optional<unsigned> state;
    std::uint64_t entered{};
    bool warned{};
    // True when the state changed (or was first seen).
    bool observe(bool requested, bool loaded, std::uint64_t now, std::string_view bundle,
                 std::uint64_t instance, Emit emit) {
        const unsigned value = (requested ? 1u : 0u) | (loaded ? 2u : 0u);
        const auto elapsed = state && now >= entered ? now - entered : 0;
        if (!state || *state != value) {
            const auto severity = value == 3 ? logging::Level::success :
                !state && value == 0 ? logging::Level::debug : logging::Level::info;
            emit(severity, std::format("Subworld {}: {} (instance {}, {}ms in previous observed state; requested={}, loaded={}).",
                value == 3 ? "loaded" : value == 1 ? "loading" : value == 2 ? "unloading" : "unloaded",
                bundle, instance, elapsed, requested, loaded));
            state = value; entered = now; warned = false;
            return true;
        }
        if ((value == 1 || value == 2) && !warned && elapsed >= 30000) {
            warned = true;
            emit(logging::Level::warning, std::format("Subworld still {} after {:.1f}s: {} (instance {}).",
                value == 1 ? "loading" : "unloading", static_cast<double>(elapsed) / 1000.0, bundle, instance));
        }
        return false;
    }
};
}
