#include "native_cosmetics.h"
#include "native_cosmetics_layout.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_cosmetics.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Customization/local_player_card_runtime.h"
#include "Extension/World/texture_handle_limit.h"
#include <Windows.h>
#include <atomic>
#include <format>
#include <mutex>
#include <set>
#include <tuple>
#include "outfit_shuffle.h"

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
// The catalog the shuffle draws from: what is installed, less the items only their owners may
// wear. It is names only, so it is kept until the number of installed items changes (a mod
// applied live). Client thread.
template <class Memory> const ShuffleCatalog &shuffle_catalog(const Memory &memory) {
    static ShuffleCatalog kept;
    static std::uintptr_t manager{};
    static std::uint32_t total{};
    const auto now = memory.ptr(memory.base, addr::engine::cosmetics_manager);
    const auto count = memory.template get<std::uint32_t>(now, 0x3c);
    if (kept.size() && now == manager && count == total) return kept;
    std::vector<ShuffleCandidate> items;
    for (auto &item : memory.catalog())
        if (!profile_runtime::reserved_cosmetic(item.name)) items.push_back({std::move(item.name), std::move(item.categories)});
    kept = ShuffleCatalog(std::move(items));
    manager = now;
    total = count;
    return kept;
}
// Every item put on a skater or board in this level, shuffled or not, and the local player's
// own. The game keeps an item's textures once they are loaded, so near its limit of streamed
// textures the shuffle picks only from these (loaded_items). A new level (another context) starts
// it again: the level's load may have let them go, and a smaller pool is the safe side to err on.
// Engine thread.
struct Loaded {
    std::uintptr_t context{};
    std::set<std::string> names;
};
Loaded &loaded() {
    static auto *kept = new Loaded;
    return *kept;
}
void note_loaded(const CosmeticRecipe &recipe) {
    for (const auto &item : recipe.items)
        if (!item.asset.empty()) loaded().names.insert(item.asset);
}
// The items loaded this level, sorted, with whatever the local player's skater or board
// (`local_entity`) wears now.
template <class Memory> std::vector<std::string> loaded_items(const Memory &memory, std::uintptr_t local_entity) {
    try {
        note_loaded(memory.capture(local_entity));
    } catch (const std::exception &) {
        // (Their outfit is updating: what was noted before is still loaded.)
    }
    const auto &names = loaded().names;
    return {names.begin(), names.end()};
}
} // namespace
void note_cosmetics_level(std::uintptr_t context) {
    auto &kept = loaded();
    if (kept.context == context) return;
    kept.context = context;
    kept.names.clear();
}
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
std::string describe_cosmetic_slots(std::uintptr_t base, const NativeFrame &local) {
    if (!local.ready || !local.board_entity) return "Waiting for the local skater and skateboard.";
    const CosmeticMemory memory{readable, base};
    std::string out;
    try {
        const auto &catalog = shuffle_catalog(memory);
        for (const auto [what, entity] : {std::pair{"Skater", local.entity}, std::pair{"Board", local.board_entity}}) {
            const auto resource = memory.resource(memory.component(entity));
            const auto categories = memory.slot_categories(resource);
            const auto recipe = memory.capture(entity);
            const auto slots = memory.ptr(resource, 0x28);
            out += std::format("{} (recipe key {}): {} slots, {} worn items\n", what, recipe.key, categories.size(),
                               recipe.items.size());
            for (std::size_t i = 0; i < categories.size(); ++i) {
                // The template's slot id (as fit_slots reads it), and what the skater wears in it.
                const auto id = memory.get<std::uint32_t>(memory.ptr(slots, i * 24 + 8) & ~std::uintptr_t{4}, 0x4c);
                const auto worn = std::find_if(recipe.items.begin(), recipe.items.end(),
                                               [&](const CosmeticSlot &item) { return item.slot == id; });
                const auto *pool = catalog.in_category(categories[i]);
                out += std::format("  [{:2}] slot 0x{:08x}  category 0x{:08x}  {}  fits {}{}\n", i, id, categories[i],
                                   worn == recipe.items.end() || worn->asset.empty() ? std::string("wearing nothing")
                                                                                     : "wearing \"" + worn->asset + "\"",
                                   pool ? pool->size() : 0, shuffles_slot(id) ? "  shuffled" : "");
            }
        }
    } catch (const std::exception &e) {
        out += std::string("Cosmetics: ") + e.what();
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
void apply_cosmetic_recipe(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t local_entity,
                           const CosmeticRecipe &recipe, const Shuffle &shuffle) {
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
    // Shuffled outfits: the slots whose item this PC lacks, noted before they are swapped for
    // stand-ins below (shuffle_slots says what each mode does with them).
    std::vector<bool> lacking;
    if (shuffle.active()) {
        lacking.assign(usable.items.size(), false);
        for (std::size_t i = 0; i < usable.items.size(); ++i)
            lacking[i] = !usable.items[i].asset.empty() && !memory.item_installed(resource, i, usable.items[i]);
    }
    const auto reserved = [](const std::string &asset) { return profile_runtime::reserved_cosmetic(asset); };
    for (const auto &[missing, replacement] :
         memory.substitute_missing(resource, usable, default_cosmetic_items, reserved)) {
        static std::mutex logged_mutex;
        static std::set<std::string> logged;
        std::lock_guard lock(logged_mutex);
        // Peers choose these names: remember (and log) a bounded number.
        if (!reserved(missing) && logged.size() < 256 && logged.insert(missing).second)
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Multiplayer: a player wears \"{}\", which is not installed here; showing {} instead.",
                         missing, replacement.empty() ? "nothing" : replacement);
    }
    // Shuffled outfits and boards: the slots the mode covers (every worn item, or just those this
    // PC lacks) take a random installed item that fits, in place of what is there now. Near the game's limit of streamed
    // textures, only an item already put on a skater this level: it brings no textures of its own. Whatever
    // goes wrong, the outfit is shown with the stand-ins.
    const auto shuffled = shuffle.active() ? shuffle_slots(shuffle.mode, usable, lacking) : std::vector<bool>{};
    if (std::find(shuffled.begin(), shuffled.end(), true) != shuffled.end()) {
        auto as_sent = usable;
        try {
            const auto textures = texture_handle_limit::use();
            const auto &catalog = shuffle_catalog(memory);
            if (!textures.known || shuffle_texture_room(textures.taken, textures.limit))
                shuffle_recipe(usable, memory.slot_categories(resource), catalog, shuffle.seed, shuffled);
            else
                shuffle_recipe(usable, memory.slot_categories(resource), catalog.only(loaded_items(memory, local_entity)),
                               shuffle.seed, shuffled);
            memory.validate(resource, usable);
        } catch (const std::exception &e) {
            usable = std::move(as_sent);
            static std::atomic<unsigned> reported{};
            if (reported.fetch_add(1) < 3)
                logging::log(logging::Level::warning, logging::Channel::runtime,
                             "Multiplayer: a player's {} could not be shuffled, so it is shown with stand-ins: {}",
                             recipe.key == board_recipe_key ? "board" : "outfit", e.what());
        }
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
    note_loaded(usable);
    logging::log(
        logging::Level::debug, logging::Channel::runtime,
        "Multiplayer: peer cosmetics queued; entity={:#x}, component={:#x}, template={:#x}, slots={}.",
        entity, c, usable.key, usable.items.size());
}
} // namespace dingosdk::multiplayer
