#include "filmer_outfit.h"
#include "Extension/Skater/skater_items.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace dingosdk::filmer::outfit {
namespace {
using skater_items::SkaterItems;
struct Worn {
    bool on{};
    std::uintptr_t entity{}, component{};
    std::string item;                      // what we put on (the installed name)
    std::optional<skater_items::Feed> before; // how the game fed the skater its outfit before; empty: it did not
    std::string original;                  // what the slot held before
    std::vector<std::uint32_t> original_parameters;
    std::uint64_t last_wanted{}, retry_after{};
    std::string last_error;
    std::mutex status_mutex;
    std::string status = "the camcorder is not worn";
};
Worn &worn() {
    static auto *value = new Worn;
    return *value;
}
void say(Worn &w, std::string text) {
    std::lock_guard lock(w.status_mutex);
    w.status = std::move(text);
}
// The hand-wear slot: the one whose name says hand wear, the left hand's when there are two (the
// fingerless glove is left hand wear).
std::string slot_word = "hand"; // client thread
int hand_slot(const SkaterItems &m, std::uintptr_t resource) {
    const auto table = m.ptr(resource, 0x28);
    const auto count = m.count(table, 24, multiplayer::max_cosmetic_slots);
    int found = -1;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = m.ptr(table, i * 24 + 8) & ~std::uintptr_t{4};
        const auto name = skater_items::lower(m.text(m.ptr(slot, 0x20)));
        if (name.find(slot_word) == std::string::npos) continue;
        if (found < 0 || name.find("left") != std::string::npos) found = static_cast<int>(i);
    }
    return found;
}
void put_on(Worn &w, const SkaterItems &m, std::uintptr_t entity, const std::string &wanted) {
    const auto component = m.component(entity);
    auto recipe = m.capture(entity); // throws while the skater's items are still updating: tried again later
    const int slot = hand_slot(m, m.resource(component));
    SkaterItems::check(slot >= 0 && static_cast<std::size_t>(slot) < recipe.items.size(), std::format("the skater has no outfit slot named like '{}'", slot_word).c_str());
    const auto item = skater_items::installed_item(m, skater_items::lower(wanted));
    if (item.empty()) throw std::runtime_error(std::format("{} is not installed (build the camcorder with ReSkate Studio)", wanted));
    auto &hand = recipe.items[static_cast<std::size_t>(slot)];
    if (!w.on) {
        const auto fed = skater_items::feed(m, component);
        w.before = fed.follows_outfit() ? std::optional<skater_items::Feed>(fed) : std::nullopt;
        w.original = hand.asset;
        w.original_parameters = hand.parameters;
    }
    w.on = true;
    w.entity = entity;
    w.component = component;
    w.item = item;
    if (hand.asset == item) return;
    hand.asset = item;
    skater_items::apply(m, component, recipe);
    logging::log(logging::Level::info, logging::Channel::skater, "Filmer: the camcorder ({}) goes in your hand.", item);
}
void take_off(Worn &w, const SkaterItems &m, std::uintptr_t entity) {
    const auto component = m.component(entity);
    if (w.before && component == w.component) {
        skater_items::hand_back(m, component, *w.before); // the game puts the player's whole outfit back
    } else {
        auto recipe = m.capture(entity);
        const int slot = hand_slot(m, m.resource(component));
        SkaterItems::check(slot >= 0 && static_cast<std::size_t>(slot) < recipe.items.size(), std::format("the skater has no outfit slot named like '{}'", slot_word).c_str());
        auto &hand = recipe.items[static_cast<std::size_t>(slot)];
        if (hand.asset == w.item) {
            hand.asset = w.original;
            hand.parameters = w.original_parameters;
            skater_items::apply(m, component, recipe);
        }
    }
    logging::log(logging::Level::info, logging::Channel::skater, "Filmer: the camcorder is put away.");
    const auto keep = w.retry_after;
    w.on = false;
    w.entity = w.component = 0;
    w.item.clear();
    w.before.reset();
    w.original.clear();
    w.original_parameters.clear();
    w.retry_after = keep;
}
void forget(Worn &w) {
    w.on = false;
    w.entity = w.component = 0;
    w.item.clear();
    w.before.reset();
    w.original.clear();
    w.original_parameters.clear();
}
} // namespace

void tick(bool wanted_now, std::string_view item_name, std::string_view slot) noexcept {
    auto &w = worn();
    {
        const auto word = skater_items::lower(slot.empty() ? std::string_view("hand") : slot);
        // Another slot asked for: what is on comes off first, from the slot it is in.
        if (word != slot_word && !w.on) slot_word = word;
        else if (word != slot_word) wanted_now = false;
    }
    const auto now = GetTickCount64();
    if (wanted_now) w.last_wanted = now;
    // A moment's grace, so a pause or a cut does not take it off and put it on again.
    const bool wanted = w.last_wanted && now - w.last_wanted < 1500;
    if (now < w.retry_after) return;
    if (!wanted && !w.on) return;
    NoBailSkater skater;
    if (!no_bail_skater(skater)) { forget(w); return; } // no skater (a level change): it comes back as the profile has it
    if (w.on && skater.entity != w.entity) { forget(w); return; }
    try {
        const SkaterItems m{skater_items::readable, skater.base};
        const std::string item(item_name.empty() ? default_item : item_name);
        if (wanted) {
            if (!w.on || skater_items::lower(w.item) != skater_items::lower(skater_items::installed_item(m, skater_items::lower(item)))) put_on(w, m, skater.entity, item);
            say(w, "the camcorder (" + w.item + ") is in your hand");
        } else {
            take_off(w, m, skater.entity);
            say(w, "the camcorder is put away");
        }
        w.last_error.clear();
    } catch (const std::exception &error) {
        w.retry_after = now + 500;
        if (w.last_error != error.what()) {
            w.last_error = error.what();
            say(w, std::string("the camcorder cannot be worn: ") + error.what());
            logging::log(logging::Level::info, logging::Channel::skater, "Filmer camcorder: {}", error.what());
        }
    } catch (...) {
        w.retry_after = now + 500;
    }
}

std::string status() {
    auto &w = worn();
    std::lock_guard lock(w.status_mutex);
    return w.status;
}
} // namespace dingosdk::filmer::outfit
