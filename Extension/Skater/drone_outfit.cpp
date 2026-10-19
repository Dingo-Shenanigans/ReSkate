#include "drone_outfit.h"
#include "Extension/RoadRash/skater_items.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/drone_flight.h"
#include "Extension/RoadRash/road_rash.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <format>
#include <optional>
#include <stdexcept>

namespace dingosdk::drone::outfit {
namespace {
using road_rash::SkaterItems;
NoBailSkater local_skater() {
    NoBailSkater skater;
    if (!no_bail_skater(skater)) throw std::runtime_error("No local skater right now (load into a level first).");
    return skater;
}
}

// ---- Goggles while flying.
namespace {
struct Worn {
    bool on{};                          // our eyewear is on the skater
    std::uintptr_t entity{}, component{};
    std::string item;                   // what we put on
    std::optional<road_rash::Feed> before; // how the game fed the skater its outfit before; empty: it did not
    std::string original;               // what the eyewear slot held before
    std::vector<std::uint32_t> original_parameters;
    std::uint64_t last_flying{}, retry_after{};
    std::string last_error;
};
Worn& worn() { static auto* value = new Worn; return *value; }

int eyewear_slot(const SkaterItems& m, std::uintptr_t resource) {
    const auto table = m.ptr(resource, 0x28);
    const auto count = m.count(table, 24, multiplayer::max_cosmetic_slots);
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = m.ptr(table, i * 24 + 8) & ~std::uintptr_t{4};
        if (road_rash::lower(m.text(m.ptr(slot, 0x20))).find("eyewear") != std::string::npos) return static_cast<int>(i);
    }
    return -1;
}
void put_on(Worn& w, const SkaterItems& m, std::uintptr_t entity, const std::string& item) {
    const auto component = m.component(entity);
    auto recipe = m.capture(entity); // throws while the skater's items are still updating: tried again later
    const int slot = eyewear_slot(m, m.resource(component));
    SkaterItems::check(slot >= 0 && static_cast<std::size_t>(slot) < recipe.items.size(), "The skater has no eyewear slot.");
    SkaterItems::check(!road_rash::installed_item(m, road_rash::lower(item)).empty(), "That eyewear is not installed.");
    auto& eyes = recipe.items[static_cast<std::size_t>(slot)];
    if (!w.on) {
        const auto fed = road_rash::feed(m, component);
        w.before = fed.follows_outfit() ? std::optional<road_rash::Feed>(fed) : std::nullopt;
        w.original = eyes.asset;
        w.original_parameters = eyes.parameters;
    }
    if (eyes.asset == item) { w.on = true; w.entity = entity; w.component = component; w.item = item; return; }
    eyes.asset = item;
    road_rash::apply(m, component, recipe);
    w.on = true;
    w.entity = entity;
    w.component = component;
    w.item = item;
    logging::log(logging::Level::info, logging::Channel::skater, "Drone: the pilot puts on {}.", item);
}
void take_off(Worn& w, const SkaterItems& m, std::uintptr_t entity) {
    const auto component = m.component(entity);
    // Road Rash keeps the skater on a recipe of its own: put only the eyewear back, so its marks stay.
    if (w.before && component == w.component && !road_rash::enabled()) {
        road_rash::hand_back(m, component, *w.before); // the game puts the player's whole outfit back
    } else {
        auto recipe = m.capture(entity);
        const int slot = eyewear_slot(m, m.resource(component));
        SkaterItems::check(slot >= 0 && static_cast<std::size_t>(slot) < recipe.items.size(), "The skater has no eyewear slot.");
        auto& eyes = recipe.items[static_cast<std::size_t>(slot)];
        if (eyes.asset == w.item) {
            eyes.asset = w.original;
            eyes.parameters = w.original_parameters;
            road_rash::apply(m, component, recipe);
        }
    }
    logging::log(logging::Level::info, logging::Channel::skater, "Drone: the pilot takes the eyewear off.");
    w = Worn{};
}
}

void on_client_tick() noexcept {
    auto& w = worn();
    const auto now = GetTickCount64();
    if (now < w.retry_after) return;
    const int choice = static_cast<int>(std::lround(knobs().value[kGoggles].load(std::memory_order_relaxed)));
    if (flying()) w.last_flying = now;
    // A moment's grace, so a pause or a reset does not take the goggles off and on again.
    const bool airborne = w.last_flying && now - w.last_flying < 1500;
    const bool wanted = airborne && choice >= 1 && static_cast<std::size_t>(choice) <= eyewear_count;
    if (!wanted && !w.on) return;
    NoBailSkater skater;
    if (!no_bail_skater(skater)) { w = Worn{}; return; } // no skater (a level change): it comes back as the profile has it
    if (w.on && skater.entity != w.entity) { w = Worn{}; return; } // a new skater: ours is gone with the old one
    try {
        const SkaterItems m{road_rash::readable, skater.base};
        if (wanted) {
            const std::string item = eyewear[static_cast<std::size_t>(choice - 1)].item;
            if (!w.on || w.item != item) put_on(w, m, skater.entity, item);
        } else {
            take_off(w, m, skater.entity);
        }
        w.last_error.clear();
    } catch (const std::exception& error) {
        w.retry_after = now + 500;
        if (w.last_error != error.what()) {
            w.last_error = error.what();
            logging::log(logging::Level::info, logging::Channel::skater, "Drone goggles: {}", error.what());
        }
    } catch (...) {
        w.retry_after = now + 500;
    }
}

std::vector<std::string> find_items(std::string_view word, std::size_t limit, std::size_t& total) {
    total = 0;
    const auto skater = local_skater();
    const SkaterItems m{road_rash::readable, skater.base};
    const auto wanted = road_rash::lower(word);
    const auto manager = m.ptr(m.base, addr::engine::cosmetics_manager);
    m.check(m.ptr(manager) == m.base + addr::engine::cosmetics_manager_vtable && m.get<std::uint8_t>(manager, 0xa8) == 1,
            "The cosmetic catalog is not ready.");
    const auto buckets = m.ptr(manager, 0x30);
    const auto size = m.get<std::uint32_t>(manager, 0x38), count = m.get<std::uint32_t>(manager, 0x3c);
    m.check(size && size <= 16384 && count && count <= 8192, "The cosmetic catalog exceeds its bounds.");
    std::vector<std::string> out;
    std::uint32_t visited{};
    for (std::uint32_t bucket = 0; bucket < size && visited < count; ++bucket) {
        auto node = m.ptr(buckets, bucket * 8ULL);
        for (std::uint32_t chain = 0; node && chain < count && visited < count; ++chain, ++visited, node = m.ptr(node, 0x10)) {
            const auto asset = m.ptr(node, 8) & ~std::uintptr_t{4};
            if (!asset) continue;
            auto name = m.text(m.ptr(asset, 0x38));
            if (name.empty() || road_rash::lower(name).find(wanted) == std::string::npos) continue;
            ++total;
            if (out.size() < limit) out.push_back(std::move(name));
        }
    }
    return out;
}

std::vector<std::string> slots() {
    const auto skater = local_skater();
    const SkaterItems m{road_rash::readable, skater.base};
    const auto recipe = m.capture(skater.entity);
    const auto resource = m.resource(m.component(skater.entity));
    const auto table = m.ptr(resource, 0x28);
    const auto count = m.count(table, 24, multiplayer::max_cosmetic_slots);
    std::vector<std::string> out;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = m.ptr(table, i * 24 + 8) & ~std::uintptr_t{4};
        const auto name = m.text(m.ptr(slot, 0x20));
        const auto& held = i < recipe.items.size() ? recipe.items[i].asset : std::string{};
        out.push_back(std::format("{:>2}  {:<28} {}", i, name, held.empty() ? "-" : held));
    }
    return out;
}
}
