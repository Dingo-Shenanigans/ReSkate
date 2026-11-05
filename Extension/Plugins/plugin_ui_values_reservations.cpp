#include "plugin_ui_values.h"
#include <cmath>

namespace dingosdk::plugins::ui_values {
std::string Reservations::reserve(std::string_view plugin, const Values& values) {
    if (values.size() > maximum_values) return "at most 32 UI values per plugin";
    if (!distinct_symbols(plugin, values)) return "UI symbol collision or reserved symbol";
    std::size_t own = 0;
    for (const auto& [id, record] : records_) {
        (void)id;
        if (record.plugin == plugin) ++own;
    }
    std::size_t added = 0;
    for (const auto& [name, value] : values) {
        if (!valid_name(name) || !std::isfinite(value) || std::fabs(value) > maximum_magnitude)
            return "invalid UI value";
        const auto id = symbol(plugin, name);
        const auto at = records_.find(id);
        if (at == records_.end()) ++added;
        else if (at->second.plugin != plugin || at->second.name != name) return "UI symbol collision";
    }
    if (own + added > maximum_values || records_.size() + added > maximum_process_values)
        return "UI value limit reached";
    // Native registration can fail after this; retaining reservations prevents
    // repeatedly trying new names from growing the native alias table unboundedly.
    for (const auto& [name, value] : values) {
        (void)value;
        records_.try_emplace(symbol(plugin, name), Reservation{std::string(plugin), name});
    }
    return {};
}
const Reservation* Reservations::find(std::uint32_t id) const noexcept {
    const auto at = records_.find(id);
    return at == records_.end() ? nullptr : &at->second;
}
bool Reservations::unchanged(std::string_view plugin, const Values& values, std::uintptr_t manager) const noexcept {
    if (!manager) return false;
    for (const auto& [name, value] : values) {
        const auto* record = find(symbol(plugin, name));
        if (!record || record->plugin != plugin || record->name != name ||
            record->manager != manager || !record->handle || !record->published || record->last_value != value)
            return false;
    }
    return true;
}
void Reservations::mark_published(std::uint32_t id, std::uintptr_t manager, std::uint64_t handle, float value) noexcept {
    const auto at = records_.find(id);
    if (at == records_.end()) return;
    auto& record = at->second;
    record.manager = manager;
    record.handle = handle;
    record.last_value = value;
    record.published = true;
}
}
