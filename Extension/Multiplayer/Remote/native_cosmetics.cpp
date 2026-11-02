#include "native_cosmetics.h"
#include "native_cosmetics_layout.h"
#include "stand_ins.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_cosmetics.h"
#include "Engine/Vfs/content_catalogs.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Customization/local_player_card_runtime.h"
#include <Windows.h>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace dingosdk::multiplayer {
namespace {
// Catalog, recipe and item records of loaded assets: a guarded copy, not a system
// call per read (a capture or apply reads a few hundred of them).
bool readable(std::uintptr_t p, void *out, std::size_t size) {
    return size ? memory::peek_bytes(p, out, size) : p >= 0x10000;
}
template <class T> void put(std::uintptr_t p, const T &value) {
    SIZE_T done{};
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(p), &value, sizeof(value), &done) ||
        done != sizeof(value))
        throw std::runtime_error("Cannot update the remote cosmetic component.");
}
bool reserved(const std::string &asset) { return profile_runtime::reserved_cosmetic(asset); }
// The installed items that may stand in, by the category of slot they go in: every item in the
// catalog but the paid store copies. Walked once per catalog (a live mod apply makes a new one),
// on the client update thread, the only one that dresses peers.
template <class Memory> std::vector<std::string> catalog_for(const Memory &memory, std::uintptr_t base, std::uint32_t category) {
    static std::mutex mutex;
    static std::uintptr_t cached_manager{};
    static std::uint32_t cached_total{};
    static std::map<std::uint32_t, std::vector<std::string>> by_category;
    const auto manager = memory.ptr(base, addr::engine::cosmetics_manager);
    const auto total = memory.template get<std::uint32_t>(manager, 0x3c);
    std::lock_guard lock(mutex);
    if (manager != cached_manager || total != cached_total) {
        by_category.clear();
        cached_manager = cached_total = 0;
        for (const auto &item : memory.catalog())
            if (!reserved(item.name))
                for (const auto c : item.categories) by_category[c].push_back(item.name);
        // The walk follows the catalog's hash buckets; sorted, "first" means the same item on
        // every PC whatever order the catalog was filled in.
        for (auto &[c, names] : by_category) std::sort(names.begin(), names.end());
        cached_manager = manager;
        cached_total = total;
    }
    const auto found = by_category.find(category);
    return found == by_category.end() ? std::vector<std::string>{} : found->second;
}
std::uint64_t outfit_hash(const CosmeticRecipe &recipe) {
    auto h = stand_ins::text_hash({}, recipe.key);
    for (const auto &item : recipe.items) h = stand_ins::text_hash(item.asset, stand_ins::mix(h ^ item.slot));
    return h;
}
} // namespace
std::optional<Appearance> capture_cosmetics(std::uintptr_t base, const NativeFrame &local,
                                            std::string &detail) {
    try {
        if (!local.ready || !local.board_entity)
            throw std::runtime_error("Waiting for the local skater and skateboard.");
        const CosmeticMemory memory{readable, base};
        Appearance out{memory.capture(local.entity), memory.capture(local.board_entity)};
        // Read the local profile's native UI record; never serialize model handles.
        const auto &card = profile_runtime::player_card_runtime();
        const auto handle = card.functions.get_local_info ? profile_runtime::local_player_info_hook() : 0;
        if (handle && card.data_model) {
            game::ModelWriteLock lock(card.data_model);
            const auto value = game::native_data().models.value(card.data_model, handle, 0, 0);
            if (value) {
                readable(value + 0xd0, &out.card.background, sizeof(out.card.background));
                readable(value + 0xc4, &out.card.emblem, sizeof(out.card.emblem));
                readable(value + 0xd4, &out.card.title, sizeof(out.card.title));
            }
        }
        if (!valid_appearance(out))
            throw std::runtime_error("Local cosmetic recipe exceeds the multiplayer limits.");
        detail.clear();
        return out;
    } catch (const std::exception &e) {
        detail = std::string("Cosmetics: ") + e.what();
        return {};
    }
}
void apply_cosmetic_recipe(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t local_entity,
                           const CosmeticRecipe &recipe) {
    const CosmeticMemory memory{readable, base};
    const auto c = memory.component(entity), local = memory.component(local_entity);
    memory.check(entity != local_entity && c != local && memory.ptr(entity) == memory.ptr(local_entity) &&
                     memory.ptr(entity, 0x20) == memory.ptr(local_entity, 0x20),
                 "Remote cosmetic ownership differs.");
    memory.check((recipe.key == skater_recipe_key &&
                  memory.ptr(entity) == base + addr::engine::skater_entity_vtable && !memory.ptr(entity, 0xf8)) ||
                     (recipe.key == board_recipe_key &&
                      memory.ptr(entity) == base + addr::engine::board_entity_vtable),
                 "Remote cosmetic actor type differs.");
    memory.check(memory.get<std::uint32_t>(c, 0x130) < 3, "Cannot apply peer cosmetics to a local player.");
    const auto resource = memory.resource(c);
    memory.check(resource == memory.resource(local), "Remote cosmetic template ownership differs.");
    // Items this PC does not have (a player's cosmetics mod) would reject the
    // whole outfit: they become the matching default, or an empty slot.
    auto usable = recipe;
    // A slot the peer's game does not have takes this player's own parameters for it: they
    // are the right shape for the slot, and it is shown empty.
    const auto own = [&](std::uint32_t slot, std::size_t size) {
        std::vector<std::uint32_t> out;
        try {
            for (const auto &item : memory.array<NativeCosmeticItem>(memory.ptr(local, 0x158), max_cosmetic_slots))
                if (item.slot == slot) {
                    out = memory.array<std::uint32_t>(reinterpret_cast<std::uintptr_t>(item.parameters),
                                                      max_cosmetic_parameters);
                    break;
                }
        } catch (const std::exception &) {
            out.clear();
        }
        if (out.size() != size) out.assign(size, 0);
        return out;
    };
    if (const auto [empty, left_out] = memory.fit_slots(resource, usable, own); empty || left_out) {
        static std::mutex logged_mutex;
        static std::set<std::tuple<std::uint32_t, std::size_t, std::size_t>> logged;
        std::lock_guard lock(logged_mutex);
        if (logged.size() < 32 && logged.emplace(recipe.key, empty, left_out).second)
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Multiplayer: a player's {} has different cosmetic slots from this game's (a mod adds or "
                         "removes some): {} of this game's slots left empty, {} of their items left out.",
                         recipe.key == board_recipe_key ? "board" : "skater", empty, left_out);
    }
    // A plugin's stand-ins (stand_ins.h), then the game's default, then nothing. Only while the
    // content catalogs that name the paid store copies are there: without them no item can be
    // told apart from one the store sells, so only the defaults stand in.
    std::vector<std::pair<std::string, std::string>> swaps;
    const auto policy = stand_ins::current();
    if (policy && stand_ins::covers(*policy, recipe.key == board_recipe_key) && content_cache::catalogs().available) {
        const auto outfit = outfit_hash(recipe);
        swaps = memory.substitute_missing_each(
            resource, usable,
            [&](std::size_t index, const CosmeticSlot &missing) {
                std::vector<std::string> candidates;
                try {
                    candidates = policy->items.empty() ? catalog_for(memory, base, memory.slot_category(resource, index))
                                                       : policy->items;
                } catch (const std::exception &) {
                    candidates.clear(); // a catalog not ready yet: the defaults this time
                }
                candidates = stand_ins::order(*policy, std::move(candidates), outfit, missing.slot, missing.asset);
                candidates.insert(candidates.end(), default_cosmetic_items.begin(), default_cosmetic_items.end());
                // Excluded after the shuffle: what is left keeps its order, so blocking an item
                // changes the stand-in only of the players who were shown it.
                std::erase_if(candidates, [&](const std::string &c) { return stand_ins::excluded(*policy, c); });
                return candidates;
            },
            reserved);
    } else {
        swaps = memory.substitute_missing(resource, usable, default_cosmetic_items, reserved);
    }
    for (const auto &[missing, replacement] : swaps) {
        static std::mutex logged_mutex;
        static std::set<std::string> logged;
        std::lock_guard lock(logged_mutex);
        // Peers choose these names: remember (and log) a bounded number.
        if (!reserved(missing) && logged.size() < 256 && logged.insert(missing).second)
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Multiplayer: a player wears \"{}\", which is not installed here; showing {} instead.",
                         missing, replacement.empty() ? "nothing" : replacement);
    }
    memory.validate(resource, usable);
    constexpr auto copy_prefix = addr::native_cosmetics::recipe_copy_prefix;
    memory.check(memory.get<std::array<std::uint8_t, copy_prefix.size()>>(
                     base, addr::native_cosmetics::recipe_copy) == copy_prefix,
                 "Native cosmetic copy function differs.");
    BorrowedCosmeticRecipe borrowed(usable);
    put(c + 0x134, std::uint32_t{0}); // Explicit recipe; native mode update unsubscribes the local model.
    reinterpret_cast<void (*)(std::uintptr_t, const NativeCosmeticRecipe *)>(
        base + addr::native_cosmetics::recipe_copy)(c, &borrowed.value);
    // The native copy owns strings and all arrays after this call. Its dirty
    // flags schedule the normal mesh/appearance update, including mode cleanup.
    memory.check(memory.get<std::uint32_t>(c, 0x13c) == 2 && memory.get<std::uint8_t>(c, 0x10a) == 1 &&
                     memory.get<std::uint8_t>(c, 0x10d) == 1 &&
                     memory.ptr(c, 0x158) != reinterpret_cast<std::uintptr_t>(borrowed.value.items),
                 "Native cosmetic copy did not take ownership.");
    logging::log(
        logging::Level::debug, logging::Channel::runtime,
        "Multiplayer: peer cosmetics queued; entity={:#x}, component={:#x}, template={:#x}, slots={}.",
        entity, c, usable.key, usable.items.size());
}
} // namespace dingosdk::multiplayer
