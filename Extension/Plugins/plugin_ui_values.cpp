#include "plugin_ui_values.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/ui_values.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace dingosdk::plugins::ui_values {
namespace {
namespace md = multiplayer::menu_data;
namespace abi = addr::ui_values;
// Aliases can outlive plugins and manager generations. Retain bounded symbol
// reservations process-wide even across unload/reload.
Reservations& owned() { static auto* values = new Reservations; return *values; }
std::uintptr_t manager_of(std::uintptr_t base) {
    std::uintptr_t ui{}, manager{};
    if (!base || !memory::peek(base + addr::engine::ui_manager, ui) || !ui ||
        !memory::peek(ui + abi::models_offset, manager)) return 0;
    return manager;
}
}
std::string publish(std::uintptr_t base, std::string_view plugin, const Values& values) {
    try {
        auto& reservations = owned();
        if (const auto error = reservations.reserve(plugin, values); !error.empty()) return error;
        if (values.empty()) return {};
        const auto manager = manager_of(base);
        if (!manager) return "unavailable here";
        // Do not take the exclusive model lock, recheck code bytes or reinsert
        // aliases on every tick when all floats are already current.
        if (reservations.unchanged(plugin, values, manager)) return {};
        const auto& native = game::native_data().models;
        if (!native.find || !native.create || !native.publish) return "unavailable here";
        std::array<unsigned char, abi::alias_insert_prefix.size()> prefix{};
        if (!memory::peek(base + abi::alias_insert, prefix) || prefix != abi::alias_insert_prefix)
            return "UI alias insertion bytes differ";
        game::ModelWriteLock lock(manager);
        const md::Context context(base, manager);
        const auto type = base + addr::engine::float_type;
        md::require(md::kind(type) == 19 && md::size(type) == sizeof(float), "UI float type differs");
        const auto label = game::native_name_hash("ReSkate.Plugin.UI");
        for (const auto& [name, value] : values) {
            const auto id = symbol(plugin, name);
            const auto* prior = reservations.find(id);
            if (prior && prior->manager == manager && prior->handle &&
                prior->published && prior->last_value == value) continue;
            const std::uint64_t native_id = id;
            struct Result { std::uintptr_t node{}, bucket{}; std::uint8_t inserted{}; } result;
            using Insert = Result* (*)(std::uintptr_t, Result*, std::uint8_t, const std::uint64_t*);
            reinterpret_cast<Insert>(base + abi::alias_insert)(manager + abi::aliases_offset, &result, 0, &native_id);
            md::require(result.node && md::read<std::uint64_t>(result.node) == native_id, "UI alias insertion failed");
            const auto alias = md::read<std::uint64_t>(result.node + abi::alias_value_offset);
            const bool ours = prior && prior->manager == manager && prior->handle == alias && prior->handle;
            // Refuse any alias or typed model owned by the stock UI or another producer.
            if (alias && !ours) return "UI symbol already belongs to another model";
            auto handle = native.find(manager, type, native_id, label, 0);
            if (handle && (!ours || handle != alias)) return "UI typed model already exists";
            if (!handle) handle = native.create(manager, type, native_id, label, false, 2);
            md::require(handle && context.type_of(handle) == type, "UI model handle/type differs");
            if (alias != handle)
                std::memcpy(reinterpret_cast<void*>(result.node + abi::alias_value_offset), &handle, sizeof(handle));
            context.set_if_changed(md::Value{handle, type}, value);
            reservations.mark_published(id, manager, handle, value);
        }
        return {};
    } catch (const std::exception& error) { return error.what(); }
}
void release(std::uintptr_t base, std::string_view plugin) noexcept {
    try {
        const auto manager = manager_of(base);
        if (!manager || !game::native_data().models.publish) return;
        auto& reservations = owned();
        // Only the handles this plugin published into the current UI manager: none, and the model
        // lock is not taken. Nothing new is made while unloading.
        const auto ours = [&](const Reservation& record) {
            return record.plugin == plugin && record.manager == manager && record.handle && record.published;
        };
        const auto& records = reservations.records();
        if (std::none_of(records.begin(), records.end(), [&](const auto& entry) { return ours(entry.second); })) return;
        game::ModelWriteLock lock(manager);
        const md::Context context(base, manager);
        const auto type = base + addr::engine::float_type;
        if (md::kind(type) != 19 || md::size(type) != sizeof(float)) return;
        // Zero is what a mod's UI reads as off.
        for (const auto& [id, record] : records) {
            if (!ours(record)) continue;
            try {
                if (context.type_of(record.handle) != type) continue;
                context.set_if_changed(md::Value{record.handle, type}, 0.f);
                reservations.mark_published(id, manager, record.handle, 0.f);
            } catch (...) { /* a stale handle: the rest are still zeroed */ }
        }
    } catch (...) { /* unloading never throws */ }
}
}
