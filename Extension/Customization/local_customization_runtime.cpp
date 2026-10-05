#include "Engine/Core/Log/logging.h"
#include "local_customization_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "Engine/Vfs/content_catalogs.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_customization.h"
#include <mutex>
#include <set>

namespace dingosdk::profile_runtime {
CosmeticRuntime& cosmetic_runtime() { static auto* r = new CosmeticRuntime; return *r; }

bool cosmetic_diagnostic(const char* operation, const char* reason, std::string_view id ) noexcept {
    try {
        auto& seen = cosmetic_runtime().diagnostics;
        const auto key = std::string(operation) + ':' + reason + ':' + std::string(id);
        if (seen.size() < 64 && seen.insert(key).second) {
            // An outfit that was not loaded or not saved is logged as a warning (events named
            // ..._rejected are), so a player's ordinary log says why; the rest are routine.
            const std::string_view why(reason);
            const bool routine = why == "callback_entered" || why == "unchanged" || why == "no_saved_loadout" ||
                why == "card_only_preset_recovered" || why == "card_is_not_skater_preset";
            const dingosdk::Json event{{"event", routine ? "local_cosmetic_loadout_diagnostic" : "local_cosmetic_loadout_rejected"},
                {"operation", operation}, {"reason", reason}, {"preset", id}};
            dingosdk::logging::event(dingosdk::logging::Channel::customization, event.dump().c_str());
        }
    } catch (...) {}
    return false;
}

std::uint32_t cosmetic_hash(std::string_view value) {
    if (value.empty()) return 0;
    std::uint32_t hash = 5381;
    for (unsigned char c : value) hash = hash * 33 ^ c;
    return hash;
}

bool cosmetic_text(std::uintptr_t p, std::string& value) {
    value.clear();
    for (std::size_t i = 0; i <= 255; ++i) {
        unsigned char c{};
        if (!read(p + i, c)) return false;
        if (!c) return true;
        if (c < 32 || c == 127) return false;
        value += static_cast<char>(c);
    }
    return false;
}

bool cosmetic_array(std::uintptr_t p, std::size_t stride, std::size_t limit, std::uint32_t& count) {
    if (!p || !read(p - 4, count)) return false;
    count &= 0x7fffffff;
    return count <= limit && p <= memory::highest_user_address - count * stride;
}

bool cosmetic_words(std::uintptr_t p, std::size_t limit, std::vector<std::uint32_t>& result) {
    std::uint32_t count{};
    if (!cosmetic_array(p, 4, limit, count)) return false;
    result.resize(count);
    return !count || read_bytes(p, result.data(), count * 4);
}

namespace {
std::mutex reserved_hash_mutex;
std::set<std::uint32_t> reserved_hashes;
}
bool reserved_cosmetic(std::string_view asset) {
    const auto& catalogs = content_cache::catalogs();
    if (!catalogs.available || asset.empty()) return false;
    std::string folded(asset);
    for (auto& letter : folded) if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter + ('a' - 'A'));
    return catalogs.reserved(folded);
}
bool reserved_cosmetic_hash(std::uint32_t hash) {
    std::lock_guard lock(reserved_hash_mutex);
    return hash && reserved_hashes.contains(hash);
}
bool refresh_cosmetic_catalog() {
    auto& s = local_runtime(); auto& c = cosmetic_runtime();
    std::uintptr_t manager{}, vtable{}, buckets{};
    std::uint32_t bucket_count{}, count{};
    std::uint8_t ready{};
    if (!read(s.base + addr::engine::cosmetics_manager, manager) || !manager || !read(manager, vtable) ||
        vtable != s.base + addr::engine::cosmetics_manager_vtable || !read(manager + 0xa8, ready) || ready != 1 ||
        !read(manager + 0x30, buckets) || !read(manager + 0x38, bucket_count) ||
        !read(manager + 0x3c, count) || !count || count > 8192 || !bucket_count || bucket_count > 16384)
        return false;
    if (c.item_manager == manager && c.item_count == count && !c.items.empty()) return true;
    std::map<std::string, CosmeticItemInfo, std::less<>> items;
    std::set<std::uintptr_t> seen;
    for (std::uint32_t bucket = 0; bucket < bucket_count; ++bucket) {
        std::uintptr_t node{};
        if (!read(buckets + bucket * 8ULL, node)) return false;
        while (node) {
            if (seen.size() >= count || !seen.insert(node).second) return false;
            std::uintptr_t asset{}, key_ptr{}, categories{};
            std::uint32_t hash{}, asset_hash{}, category_count{};
            if (!read(node, hash) || !read(node + 8, asset)) return false;
            asset &= ~std::uintptr_t{4};
            std::string key;
            if (!read(asset + 0x38, key_ptr) || !cosmetic_text(key_ptr, key) || key.empty() ||
                !read(asset + 0x48, asset_hash) || hash != asset_hash || hash != cosmetic_hash(key) ||
                !read(asset + 0x20, categories) || !cosmetic_array(categories, 8, 32, category_count)) return false;
            CosmeticItemInfo info{hash, {}, {}, 0};
            if (!read(asset + 0x4c, info.sort_priority)) return false;
            for (std::uint32_t i = 0; i < category_count; ++i) {
                std::uintptr_t category{}, name_ptr{}; std::uint32_t category_hash{}; std::string name;
                if (!read(categories + i * 8ULL, category)) return false;
                category &= ~std::uintptr_t{4};
                if (!read(category + 0x28, name_ptr) || !cosmetic_text(name_ptr, name) ||
                    !read(category + 0x38, category_hash) || category_hash != cosmetic_hash(name)) return false;
                info.build_kit |= name == "bkobjects";
                if (info.category.empty()) info.category = name;
                info.categories.push_back(category_hash);
            }
            if (info.build_kit) {
                const auto start = key.find('_');
                const auto end = start == std::string::npos ? start : key.find('_', start + 1);
                if (start != std::string::npos && end != std::string::npos && end > start + 1)
                    info.category = key.substr(start + 1, end - start - 1);
            }
            if (!items.emplace(key, std::move(info)).second) return false;
            if (!read(node + 0x10, node)) return false;
        }
    }
    std::uintptr_t current{}, current_buckets{}; std::uint32_t current_count{};
    if (seen.size() != count || !read(s.base + addr::engine::cosmetics_manager, current) || current != manager ||
        !read(manager + 0x30, current_buckets) || current_buckets != buckets ||
        !read(manager + 0x3c, current_count) || current_count != count) return false;
    const auto& catalogs = content_cache::catalogs();
    c.ownership_unavailable = !catalogs.available;
    std::vector<std::string> keys, objects, reserved_keys, reserved_objects;
    for (const auto& [key, info] : items) {
        std::string folded = key;
        for (auto& letter : folded) if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter + ('a' - 'A'));
        const bool held = catalogs.reserved(folded);
        (info.build_kit ? (held ? reserved_objects : objects) : (held ? reserved_keys : keys)).push_back(key);
    }
    if (!c.ownership_unavailable) {
        s.store->seed_cosmetic_inventory(keys);
        s.store->seed_object_inventory(objects);
        s.store->reconcile_inventory(reserved_keys, reserved_objects);
    }
    {
        std::set<std::uint32_t> hashes;
        for (const auto& [key, info] : items)
            if (reserved_cosmetic(key)) hashes.insert(info.hash);
        std::lock_guard lock(reserved_hash_mutex);
        reserved_hashes = std::move(hashes);
    }
    c.items = std::move(items); c.item_count = count; c.item_manager = manager;
    ++c.items_generation;
    std::ostringstream event;
    event << "{\"event\":\"local_cosmetic_catalog\",\"installed\":" << keys.size()
          << ",\"objects\":" << objects.size() << '}';
    dingosdk::logging::event(dingosdk::logging::Channel::customization, event.str().c_str());
    return true;
}

bool read_cosmetic_recipes(const void* vector, profile::CosmeticLoadout& result,
    std::vector<CosmeticNativeRecipe>& native) {
    std::uintptr_t begin{}, end{}, capacity{};
    const auto address = reinterpret_cast<std::uintptr_t>(vector);
    if (!read(address, begin) || !read(address + 8, end) || !read(address + 16, capacity) ||
        begin < 0x10000 || begin > end || end > capacity || (end - begin) % 0x28 ||
        !(end - begin) || (end - begin) / 0x28 > 16 || (capacity - begin) / 0x28 > 16) return false;
    for (auto p = begin; p < end; p += 0x28) {
        CosmeticNativeRecipe row{}; profile::CosmeticRecipe recipe;
        if (!read(p, row) || !row.resource ||
            !read(row.resource + 0x38, recipe.template_key) || !read(row.resource + 0x3c, recipe.template_version) ||
            !cosmetic_words(reinterpret_cast<std::uintptr_t>(row.scalars), 512, recipe.scalar_bits)) return false;
        std::uint32_t count{}, masks{};
        if (!cosmetic_array(reinterpret_cast<std::uintptr_t>(row.items), 0x18, 256, count) ||
            !cosmetic_array(reinterpret_cast<std::uintptr_t>(row.transient_mask), 4, 512, masks)) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            CosmeticNativeItem item{}; profile::CosmeticSlot slot;
            if (!read(reinterpret_cast<std::uintptr_t>(row.items) + i * 0x18ULL, item) ||
                !cosmetic_text(reinterpret_cast<std::uintptr_t>(item.asset), slot.asset) ||
                !cosmetic_words(reinterpret_cast<std::uintptr_t>(item.parameters), 128, slot.parameter_bits)) return false;
            if (slot.asset.empty() && item.hash) {
                for (const auto& [key, info] : cosmetic_runtime().items)
                    if (info.hash == item.hash) { slot.asset = key; break; }
            }
            if (item.hash != cosmetic_hash(slot.asset)) return false;
            slot.slot = item.slot; recipe.items.push_back(std::move(slot));
        }
        result.recipes.push_back(std::move(recipe)); native.push_back(row);
    }
    (void)profile::encode_loadout(result);
    return true;
}

bool cosmetic_slot_categories(std::uintptr_t resource, std::map<std::uint32_t, std::uint32_t>& result) {
    // RecipeTemplate.Slots is a 24-byte array (port name, slot asset, save flag).
    // RecipeSlotAsset identifies its slot separately from its accepted Category.
    // Tattoos, gestures, stickers and several color/right-side slots share categories.
    std::uintptr_t slots{}; std::uint32_t count{};
    if (!read(resource + 0x28, slots) || !cosmetic_array(slots, 24, 256, count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t slot{}, type{}, name{}, category{}, category_name{};
        std::uint32_t hash{}, category_hash{}; std::string text, category_text;
        if (!read(slots + i * 24ULL + 8, slot)) return false;
        slot &= ~std::uintptr_t{4};
        if (!read(slot + 8, type) || type != local_runtime().base + addr::engine::cosmetic_slot_type ||
            !read(slot + 0x20, name) || !cosmetic_text(name, text) || text.empty() ||
            !read(slot + 0x4c, hash) || hash != cosmetic_hash(text) ||
            !read(slot + 0x30, category)) return false;
        category &= ~std::uintptr_t{4};
        if (!read(category + 0x28, category_name) || !cosmetic_text(category_name, category_text) || category_text.empty() ||
            !read(category + 0x38, category_hash) || category_hash != cosmetic_hash(category_text) ||
            !result.emplace(hash, category_hash).second) return false;
    }
    return true;
}

bool complete_starter_recipes(const profile::CosmeticLoadout& value) {
    if (value.recipes.size() != starter_templates.size()) return false;
    for (std::size_t i = 0; i < starter_templates.size(); ++i)
        if (value.recipes[i].template_key != starter_templates[i].key ||
            value.recipes[i].template_version != starter_templates[i].version || value.recipes[i].items.empty()) return false;
    return true;
}

bool initialize_missing_starter(const std::vector<CosmeticNativeRecipe>& recipes) {
    auto& s = local_runtime(); auto& c = cosmetic_runtime();
    if (!c.initialize_starter || recipes.size() != starter_templates.size()) return false;
    std::uintptr_t manager{}, categories{}, binding{}, model{};
    std::uint32_t state{}, count{};
    if (!read(s.base + addr::engine::loadout_manager, manager) || !manager || !read(manager, state) || state != 0 ||
        !read(manager + 8, categories) || !cosmetic_array(categories, 16, 64, count) || count ||
        !read(manager + 0x198, model) || !model || !read(manager + 0x1b0, binding) || !binding) return false;
    // On a new offline profile the pre-spawn configuration may never arrive.
    // Borrow the complete authored character setup from InitRecipeTemplates,
    // never the account-wide RIP Card callback. Configure the native manager, which
    // copies the definitions and continues its normal selection state machine.
    struct Definition { std::uintptr_t resource; const std::uintptr_t* presets; };
    static_assert(sizeof(Definition) == 16);
    CosmeticBorrowedArray<std::uintptr_t> no_presets(0);
    CosmeticBorrowedArray<Definition> definitions(recipes.size());
    std::set<std::uintptr_t> unique;
    for (std::size_t i = 0; i < recipes.size(); ++i) {
        const auto resource = recipes[i].resource;
        std::uintptr_t type{}, name{}; std::uint32_t key{}, version{}; std::string text;
        if (!read(resource + 8, type) || type != s.base + addr::local_customization::recipe_template_type || !unique.insert(resource).second ||
            !read(resource + 0x20, name) || !cosmetic_text(name, text) || text != starter_templates[i].name ||
            !read(resource + 0x38, key) || key != starter_templates[i].key ||
            !read(resource + 0x3c, version) || version != starter_templates[i].version) return false;
        definitions.data()[i] = {resource, no_presets.data()};
    }
    const auto entries = definitions.data(); const std::uintptr_t no_creation_preset{};
    c.initialize_starter(false, &entries, &no_creation_preset);
    if (!read(manager + 8, categories) || !cosmetic_array(categories, 16, 64, count) || count != recipes.size()) return false;
    dingosdk::logging::event(dingosdk::logging::Channel::customization, dingosdk::Json{{"event", "local_starter_profile_initialized"}, {"templates", count}}.dump().c_str());
    return true;
}

void initialize_starter_from_expression(std::uintptr_t vm, std::uint32_t pc) noexcept {
    auto& s = local_runtime();
    if (pc || !s.active.load(std::memory_order_acquire)) return;
    PreserveError preserve;
    try {
        std::uintptr_t instance{}, resource{}, current{}, constants{};
        std::uint32_t key{}; std::array<std::uint32_t, 10> layout{};
        // Exact installed InitRecipeTemplates expression. All three assets are
        // live in its constant page; the native setup copies them synchronously.
        // Every expression the game starts passes here: the live VM's fields are
        // peeked, not read with a system call each (profiled 2026-10-01).
        if (!memory::peek(vm + 0x38, current) || !memory::peek(current + 0x10, key) || key != 0x6ba0e33e ||
            !read(vm + 0x30, instance) || !read(instance, resource) || current != resource ||
            !read(resource + 0x20, layout) ||
            layout != std::array<std::uint32_t, 10>{48,232,10,123,3,0,1,10,0,0} ||
            !read(instance + 48, constants) || !constants) return;
        std::lock_guard lock(s.native_mutex);
        std::vector<CosmeticNativeRecipe> recipes(3);
        for (std::size_t i = 0; i < recipes.size(); ++i) {
            if (!read(constants + 8 + i * 8, recipes[i].resource)) return;
            recipes[i].resource &= ~std::uintptr_t{4};
        }
        const bool initialized = initialize_missing_starter(recipes);
        if (cosmetic_runtime().diagnostics.insert("starter_template_expression").second)
            dingosdk::logging::event(dingosdk::logging::Channel::customization, dingosdk::Json{{"event", "local_starter_template_expression"}, {"initialized", initialized}}.dump().c_str());
    } catch (...) { cosmetic_diagnostic("starter", "expression_setup_failed"); }
}

bool recover_card_only_preset(std::string_view id, const profile::CosmeticLoadout& saved,
    const profile::CosmeticLoadout& defaults) {
    // The b227/4c2b candidates could save a RIP Card as a skater preset. Only
    // that exact shape is recoverable; ordinary incompatible outfits stay protected.
    if (id == "profile" || !complete_starter_recipes(defaults)) return false;
    try { profile::validate_player_card(saved); } catch (...) { return false; }
    auto& s = local_runtime();
    if (!s.store->player_card()) s.store->save_player_card(saved);
    cosmetic_runtime().blocked_loadouts.erase(std::string(id));
    cosmetic_diagnostic("load", "card_only_preset_recovered", id);
    // Leave the invalid preset on disk until a complete native outfit is saved.
    return true;
}

bool load_cosmetic_hook(const void* wrapper, void* destination) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) return s.load_cosmetic(wrapper, destination);
    PreserveError preserve;
    std::lock_guard lock(s.native_mutex);
    std::string id;
    try {
        if (!identifier(wrapper, id)) return cosmetic_diagnostic("load", "invalid_preset_id");
        const auto saved = s.store->cosmetic_loadout(id);
        if (!saved) {
            profile::CosmeticLoadout starter; std::vector<CosmeticNativeRecipe> native;
            const bool valid = read_cosmetic_recipes(destination, starter, native);
            if (cosmetic_runtime().diagnostics.insert("starter_recipe:" + id).second) {
                dingosdk::Json counts = dingosdk::Json::array();
                if (valid) for (const auto& recipe : starter.recipes)
                    counts.push_back({{"template", recipe.template_key}, {"version", recipe.template_version},
                        {"items", recipe.items.size()}, {"scalars", recipe.scalar_bits.size()}});
                dingosdk::logging::event(dingosdk::logging::Channel::customization, dingosdk::Json{{"event", "local_starter_recipe_observed"}, {"preset", id},
                    {"valid", valid}, {"recipes", counts}}.dump().c_str());
            }
            return cosmetic_diagnostic("load", "no_saved_loadout", id);
        }
        auto& c = cosmetic_runtime();
        c.blocked_loadouts.insert(id);
        profile::CosmeticLoadout defaults; std::vector<CosmeticNativeRecipe> native;
        if (!read_cosmetic_recipes(destination, defaults, native)) return cosmetic_diagnostic("load", "invalid_native_recipe", id);
        if (recover_card_only_preset(id, *saved, defaults)) return false;
        // The cosmetics manager cannot be read for the first seconds of a level
        // load, which is when the game asks for every slot up to the selected
        // one. The catalog read before the load says the same thing (what is
        // installed does not change with the level), so the outfit is checked
        // against that one. Only with no catalog read yet this session is the
        // load refused; the selection that asks is held back until one is
        // (local_customization_outfits_loadable).
        if (!refresh_cosmetic_catalog() && c.items.empty()) return cosmetic_diagnostic("load", "catalog_not_ready", id);
        if (saved->recipes.size() != defaults.recipes.size()) return cosmetic_diagnostic("load", "recipe_count_mismatch", id);
        const auto snapshot_shared = s.store->shared_snapshot();
        const auto& snapshot = *snapshot_shared;
        std::vector<CosmeticBorrowedArray<std::uint32_t>> words;
        std::vector<CosmeticBorrowedArray<CosmeticNativeItem>> item_arrays;
        words.reserve(16 * 257); item_arrays.reserve(16);
        std::vector<CosmeticRuntime::HeldSlot> held;
        for (std::size_t i = 0; i < saved->recipes.size(); ++i) {
            const auto& recipe = saved->recipes[i]; const auto& expected = defaults.recipes[i];
            if (recipe.template_key != expected.template_key || recipe.template_version != expected.template_version ||
                recipe.scalar_bits.size() != expected.scalar_bits.size())
                return cosmetic_diagnostic("load", "recipe_template_mismatch", id);
            std::map<std::uint32_t, std::uint32_t> slot_categories;
            if (!cosmetic_slot_categories(native[i].resource, slot_categories))
                return cosmetic_diagnostic("load", "invalid_slot_definitions", id);
            words.emplace_back(recipe.scalar_bits.size());
            std::copy(recipe.scalar_bits.begin(), recipe.scalar_bits.end(), words.back().data());
            native[i].scalars = words.back().data();
            // The template says which slots there are and in what order; the saved
            // outfit is matched to it slot by slot. A mod that adds slots to a
            // template (a wheel slot per wheel on the board) changes that list
            // under every outfit saved before it, and again when it is switched
            // off: a slot the outfit has no item for starts on its default, and a
            // saved slot the template no longer has is held, so the next save
            // keeps it for when the slot is back.
            std::map<std::uint32_t, const profile::CosmeticSlot*> saved_slots;
            for (const auto& item : recipe.items) saved_slots.emplace(item.slot, &item);
            std::size_t added{};
            item_arrays.emplace_back(expected.items.size());
            for (std::size_t j = 0; j < expected.items.size(); ++j) {
                const auto match = saved_slots.find(expected.items[j].slot);
                const auto* slot = match == saved_slots.end() ? &expected.items[j] : match->second;
                if (match == saved_slots.end()) ++added;
                else saved_slots.erase(match);
                if (slot != &expected.items[j] && !slot->asset.empty()) {
                    // An item that is no longer installed (a removed mod, a catalog
                    // change) or no longer fits its slot falls back to the slot's
                    // default. Rejecting the whole outfit would also block every
                    // later save to this preset, so outfits could never be fixed.
                    const auto found = c.items.find(slot->asset);
                    const auto category = slot_categories.find(slot->slot);
                    const char* problem = found == c.items.end() ? "is no longer installed" :
                        category == slot_categories.end() ||
                        std::find(found->second.categories.begin(), found->second.categories.end(), category->second) ==
                            found->second.categories.end() ? "no longer fits its slot" : nullptr;
                    if (problem) {
                        dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::customization,
                            "Saved outfit \"{}\": item {} {}; showing the default for that slot and keeping the item "
                            "saved for when it is installed again.", id, slot->asset, problem);
                        held.push_back({i, *slot, expected.items[j].asset});
                        slot = &expected.items[j];
                    } else {
                        const auto& inventory = snapshot.customization.value("inventory", dingosdk::Json::object());
                        if (inventory.contains(slot->asset) && !inventory.at(slot->asset).get<bool>())
                            slot = &expected.items[j];
                    }
                }
                words.emplace_back(slot->parameter_bits.size());
                std::copy(slot->parameter_bits.begin(), slot->parameter_bits.end(), words.back().data());
                item_arrays.back().data()[j] = {slot->asset.c_str(), words.back().data(), cosmetic_hash(slot->asset),
                    expected.items[j].slot};
            }
            for (const auto& [hash, item] : saved_slots) held.push_back({i, *item, {}});
            if (added || !saved_slots.empty())
                dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::customization,
                    "Saved outfit \"{}\": its slots differ from the game's (a mod that adds or removes slots): {} "
                    "new slot(s) start on their default, {} saved slot(s) the game does not have now are kept.",
                    id, added, saved_slots.size());
            native[i].items = item_arrays.back().data();
        }
        s.copy_cosmetic(destination, native.data(), native.data() + native.size());
        c.blocked_loadouts.erase(id);
        if (held.empty()) c.held_slots.erase(id);
        else c.held_slots[id] = std::move(held);
        dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_cosmetic_loadout_loaded\"}");
        return true;
    } catch (...) {
        if (!id.empty()) cosmetic_runtime().blocked_loadouts.insert(id);
        dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_cosmetic_loadout_load_failed\"}");
        return false;
    }
}

void save_cosmetic_hook(std::uintptr_t manager, void* record, const char* raw_id, const char* reason, bool changed_only) {
    auto& s = local_runtime();
    if (!s.active.load(std::memory_order_acquire)) { s.save_cosmetic(manager, record, raw_id, reason, changed_only); return; }
    PreserveError preserve;
    std::lock_guard lock(s.native_mutex);
    try {
        cosmetic_diagnostic("save", "callback_entered");
        std::uintptr_t current{}, begin{}, end{}; std::string id;
        const auto p = reinterpret_cast<std::uintptr_t>(record);
        if (!read(s.base + addr::engine::loadout_manager, current) || current != manager || !manager ||
            !read(manager + 0x18, begin) || !read(manager + 0x20, end) || begin > end ||
            (end - begin) % 0x48 || (end - begin) / 0x48 > 10 || p < begin || p >= end || (p - begin) % 0x48) {
            cosmetic_diagnostic("save", "invalid_manager_or_record"); return;
        }
        if (!cosmetic_text(reinterpret_cast<std::uintptr_t>(raw_id), id) || id.empty()) {
            cosmetic_diagnostic("save", "invalid_preset_id"); return;
        }
        if (cosmetic_runtime().blocked_loadouts.contains(id)) {
            cosmetic_diagnostic("save", "previous_load_rejected", id); return;
        }
        profile::CosmeticLoadout value; std::vector<CosmeticNativeRecipe> native;
        if (!read_cosmetic_recipes(record, value, native)) {
            cosmetic_diagnostic("save", "invalid_native_recipe", id); return;
        }
        if (value.recipes.size() == 1 && value.recipes.front().template_key == 2169419386U) {
            cosmetic_diagnostic("save", "card_is_not_skater_preset", id); return;
        }
        // Keep items from costume mods that are not installed right now, and
        // whole slots a mod added that is switched off: a slot still showing the
        // stand-in default saves the original item, while a slot the player
        // changed saves the new choice and releases the hold.
        if (const auto held = cosmetic_runtime().held_slots.find(id); held != cosmetic_runtime().held_slots.end()) {
            std::erase_if(held->second, [&](const CosmeticRuntime::HeldSlot& h) {
                if (h.recipe >= value.recipes.size()) return true;
                for (auto& item : value.recipes[h.recipe].items) {
                    if (item.slot != h.saved.slot) continue;
                    if (item.asset != h.fallback) return true;
                    item = h.saved;
                    return false;
                }
                // The game has no such slot now (the mod that added it is off):
                // the saved item rides along until the slot is back.
                value.recipes[h.recipe].items.push_back(h.saved);
                return false;
            });
            if (held->second.empty()) cosmetic_runtime().held_slots.erase(held);
        }
        const auto previous = s.store->cosmetic_loadout(id);
        s.store->save_cosmetic_loadout(id, value);
        // The native success callback copies this same vector to the baseline.
        // Only acknowledge after the JSON transaction has completed.
        std::uintptr_t first{}, last{};
        if (!read(p, first) || !read(p + 8, last)) {
            cosmetic_diagnostic("save", "saved_but_baseline_unavailable", id); return;
        }
        s.copy_cosmetic(reinterpret_cast<void*>(p + 0x20), reinterpret_cast<const void*>(first), reinterpret_cast<const void*>(last));
        if (!previous || *previous != value) dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_cosmetic_loadout_saved\"}");
        else cosmetic_diagnostic("save", "unchanged", id);
    } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::customization, "{\"event\":\"local_cosmetic_loadout_save_failed\"}"); }
}
}