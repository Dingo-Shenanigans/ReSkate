#include "plugin_ui_values.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/ui_values.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace dingosdk::plugins::ui_values {
namespace {
namespace md = multiplayer::menu_data;
namespace abi = addr::ui_values;
struct Owned {
    std::string plugin, name;
    std::uintptr_t manager{};
    std::uint64_t handle{};
};
// Aliases may be retained by loaded UI assets. Keep the bounded reservations for
// the process, including across plugin reload, rather than leaving dangling refs.
std::map<std::uint32_t, Owned>& owned() { static auto* values = new std::map<std::uint32_t, Owned>; return *values; }
std::uintptr_t manager_of(std::uintptr_t base) {
    std::uintptr_t ui{}, manager{};
    if (!base || !memory::peek(base + addr::engine::ui_manager, ui) || !ui ||
        !memory::peek(ui + abi::models_offset, manager)) return 0;
    return manager;
}
}
std::string publish(std::uintptr_t base, std::string_view plugin, const Values& values) {
    try {
        if (values.size() > maximum_values) return "at most 32 UI values per plugin";
        if (!distinct_symbols(plugin, values)) return "UI symbol collision or reserved symbol";
        auto& reservations = owned();
        std::size_t count = static_cast<std::size_t>(std::count_if(reservations.begin(), reservations.end(),
            [&](const auto& entry) { return entry.second.plugin == plugin; }));
        std::size_t added = 0;
        for (const auto& [name, value] : values) {
            if (!valid_name(name) || !std::isfinite(value) || std::fabs(value) > maximum_magnitude) return "invalid UI value";
            const auto id = symbol(plugin, name);
            if (!id) return "UI symbol is reserved";
            const auto at = reservations.find(id);
            if (at == reservations.end()) { ++added; ++count; }
            else if (at->second.plugin != plugin || at->second.name != name) return "UI symbol collision";
        }
        if (count > maximum_values || reservations.size() + added > 256) return "UI value limit reached";
        if (values.empty()) return {};
        // Failed native registration also consumes its reservation, so repeatedly
        // trying new names cannot grow the engine's alias table without bound.
        for (const auto& [name, value] : values) {
            (void)value;
            reservations.try_emplace(symbol(plugin, name), Owned{std::string(plugin), name});
        }
        const auto& native = game::native_data().models;
        const auto manager = manager_of(base);
        if (!manager || !native.find || !native.create || !native.publish) return "unavailable here";
        std::array<unsigned char, abi::alias_insert_prefix.size()> prefix{};
        if (!memory::peek(base + abi::alias_insert, prefix) || prefix != abi::alias_insert_prefix)
            return "UI alias insertion bytes differ";
        game::ModelWriteLock lock(manager);
        const md::Context context(base, manager);
        const auto type = base + addr::engine::float_type;
        md::require(md::kind(type) == 19 && md::size(type) == sizeof(float), "UI float type differs");
        const auto label = game::native_name_hash("ReSkate.Plugin.UI");
        for (const auto& [name, value] : values) {
            const std::uint64_t id = symbol(plugin, name);
            struct Result { std::uintptr_t node{}, bucket{}; std::uint8_t inserted{}; } result;
            using Insert = Result* (*)(std::uintptr_t, Result*, std::uint8_t, const std::uint64_t*);
            reinterpret_cast<Insert>(base + abi::alias_insert)(manager + abi::aliases_offset, &result, 0, &id);
            md::require(result.node && md::read<std::uint64_t>(result.node) == id, "UI alias insertion failed");
            const auto alias = md::read<std::uint64_t>(result.node + abi::alias_value_offset);
            const auto prior = reservations.find(static_cast<std::uint32_t>(id));
            const bool ours = prior != reservations.end() && prior->second.manager == manager && prior->second.handle == alias;
            // A hash collision with stock or another producer must never become a
            // way to write a model owned by the game, even if its type is float.
            if (alias && !ours) return "UI symbol already belongs to another model";
            auto handle = native.find(manager, type, id, label, 0);
            if (handle && (!ours || handle != alias)) return "UI typed model already exists";
            if (!handle) handle = native.create(manager, type, id, label, false, 2);
            md::require(handle && context.type_of(handle) == type, "UI model handle/type differs");
            auto& record = reservations[static_cast<std::uint32_t>(id)];
            record = {std::string(plugin), name, manager, handle};
            if (alias != handle) std::memcpy(reinterpret_cast<void*>(result.node + abi::alias_value_offset), &handle, sizeof(handle));
            context.set_if_changed(md::Value{handle, type}, value);
        }
        return {};
    } catch (const std::exception& error) { return error.what(); }
}
void release(std::uintptr_t base, std::string_view plugin) noexcept {
    try {
        Values neutral;
        for (const auto& [id, record] : owned()) {
            (void)id;
            if (record.plugin == plugin) neutral.emplace(record.name, 0.f);
        }
        (void)publish(base, plugin, neutral);
    } catch (...) {}
}
}
