#include "local_buildkit_labels.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/Progression/local_challenge_runtime.h"
#include "Extension/Localization/text_overrides.h"

namespace dingosdk::profile_runtime {
// Missing labels in the installed Build Kit assets. Existing translations win.

BuildKitTextFunctions& buildkit_text_functions() { static BuildKitTextFunctions f; return f; }

std::string_view buildkit_fallback(const char* key) {
    if (!local_runtime().active.load(std::memory_order_acquire)) return {};
    std::array<char, 128> id{};
    for (std::size_t size = 0; size < id.size(); ++size) {
        if (!read(reinterpret_cast<std::uintptr_t>(key) + size, id[size])) return {};
        if (id[size]) continue;
        const std::string_view value(id.data(), size);
        for (const auto& entry : buildkit_labels) if (value == entry.id) return entry.text;
        return challenge_title_fallback(value);
    }
    return {};
}

std::uintptr_t buildkit_text_exists(const char* key) {
    const auto result = buildkit_text_functions().exists(key);
    PreserveError preserve;
    if (!(result & 0xff) && !buildkit_fallback(key).empty()) return (result & ~std::uintptr_t{0xff}) | 1;
    return result;
}

std::uintptr_t buildkit_text_translate(const char* key, char* output, std::uint32_t capacity) {
    const auto result = buildkit_text_functions().translate(key, output, capacity);
    PreserveError preserve;
    if (result & 0xff) {
        text_overrides::observe_translation(key, output, capacity);
        return result;
    }
    const auto fallback = buildkit_fallback(key);
    if (fallback.empty() || capacity <= fallback.size()) return result;
    SIZE_T written{};
    if (!WriteProcessMemory(GetCurrentProcess(), output, fallback.data(), fallback.size() + 1, &written) ||
        written != fallback.size() + 1) return result;
    return (result & ~std::uintptr_t{0xff}) | 1;
}
}