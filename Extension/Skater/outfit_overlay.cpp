#include "outfit_overlay.h"
#include "Extension/Skater/skater_items.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/content_catalogs.h"
#include "Extension/World/load_error_guard.h"
#include "Extension/Skater/layer_materials.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <bit>
#include <cmath>
#include <set>
#include <format>
#include <map>
#include <optional>

namespace dingosdk::outfit_overlay {
namespace {
using skater_items::SkaterItems;
using skater_items::lower;
constexpr std::string_view allowed[] = {"tattoo", "moles", "eyewear", "hand"};

struct Wanted {
    std::string owner, item;
    std::vector<float> placement;
    bool covers{};
};
// The slots an item worn on top may go into when its own kind's is taken, in the order they are
// tried: the accessory slots, which hold one small item each and are empty more often than not; then
// the tattoo slots and the face's empty ones (make-up, freckles, acne...), which the skater loader
// also builds an item from. Not the full-body costume (it hides the body) and not the body itself,
// its hair, colours and skin. The left hand's slot comes last of the accessories: the loader finds no
// model there for most items that are not gloves. (Pull request 269, Outfit layers.)
constexpr std::array<std::string_view, 22> spare_slots{
    "cust_outfits", "cust_handwear_right", "cust_face_accessory", "cust_earwear_right",
    "cust_eyebrowwear_left", "cust_eyebrowwear_right", "cust_nosewear_left", "cust_nosewear_right",
    "cust_eyewear", "cust_hats", "cust_socks", "cust_handwear",
    "tattoo_arml_1", "tattoo_armr_1", "tattoo_chest_1", "tattoo_legl_1", "tattoo_legr_1",
    "cust_makeup", "cust_freckles", "cust_acne", "cust_vitiligo", "cust_maturity"};
// A vest from the left hand's slot leaves the skater with no body at all (the game describes a failed
// load within half a second): a layer on trial comes off if the game describes one within this long.
constexpr std::uint64_t trial_time = 900;
using Placement = std::pair<std::size_t, std::string>; // slot, item
struct Trial {
    Placement placement;
    std::uint64_t at{}, failed_loads{};
};
struct SlotInfo {
    std::string name;
    std::uintptr_t category{}; // its category's pointer, tag bit cleared
};

struct State {
    std::map<std::string, Wanted> wanted; // by slot name
    std::map<std::string, std::vector<std::string>> layers; // by owner
    std::vector<Placement> placed;        // layers on the skater now
    std::optional<Trial> trial;           // the one placed last, not yet known to load
    std::set<std::string> good, bad;      // "slot|item" (lower case): loaded / did not load
    bool memory_read{};
    std::map<std::uintptr_t, std::vector<std::uintptr_t>> native_categories; // catalog item -> its own
    std::string layer_note, logged_note, logged_wait; // the notes last put in the log
    std::map<std::string, std::string> found; // installed items by ending
    bool own{}, dirty{}, worn{};
    std::uintptr_t skater{}, component{}, slots_of{};
    std::map<std::string, int> slot_index; // by slot name, for slots_of
    std::optional<skater_items::Feed> feed; // how the game fed the skater before a recipe of ours
    std::set<std::string> ours;          // items we put on
    std::uint64_t given_back{}, menu_seen{}, checked{}, applied{}, last_error{};
    std::string status{"nothing worn"};
};
State& state() {
    static auto* s = new State;
    return *s;
}
bool blank(const std::string& asset) { return asset.empty() || lower(asset).ends_with("_empty"); }
int slot_of(State& s, const SkaterItems& m, std::uintptr_t resource, const std::string& wanted) {
    if (s.slots_of != resource) {
        s.slot_index.clear();
        s.slots_of = resource;
        const auto table = m.ptr(resource, 0x28);
        const auto count = m.count(table, 24, multiplayer::max_cosmetic_slots);
        for (std::size_t i = 0; i < count; ++i) {
            const auto slot = m.ptr(table, i * 24 + 8) & ~std::uintptr_t{4};
            s.slot_index.emplace(lower(m.text(m.ptr(slot, 0x20))), static_cast<int>(i));
        }
    }
    for (const auto& [name, index] : s.slot_index)
        if (name.find(wanted) != std::string::npos) return index;
    return -1;
}
void give_back(State& s, const SkaterItems& m, std::uintptr_t skater, std::uint64_t now);
// Which items load from which slots, kept between runs beside Skate.exe: a line each, "+slot|item"
// or "-slot|item".
std::filesystem::path memory_file() {
    std::array<wchar_t, 32768> buffer{};
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path() / L"ReSkate.layer_slots.txt";
}
void read_memory(State& s) {
    if (std::exchange(s.memory_read, true)) return;
    try {
        std::ifstream in(memory_file());
        for (std::string line; std::getline(in, line) && s.good.size() + s.bad.size() < 65536;) {
            if (line.size() < 4 || (line[0] != '+' && line[0] != '-')) continue;
            (line[0] == '+' ? s.good : s.bad).insert(lower(line.substr(1)));
        }
    } catch (...) {}
}
void remember(State& s, bool loads, const std::string& key) {
    if (!(loads ? s.good : s.bad).insert(key).second) return;
    try {
        std::ofstream out(memory_file(), std::ios::app);
        out << (loads ? '+' : '-') << key << '\n';
    } catch (...) {}
}
std::string pair_key(std::string_view slot, std::string_view item) { return lower(slot) + "|" + lower(item); }

std::vector<SlotInfo> slot_infos(const SkaterItems& m, std::uintptr_t resource) {
    std::vector<SlotInfo> out;
    const auto table = m.ptr(resource, 0x28);
    const auto count = m.count(table, 24, multiplayer::max_cosmetic_slots);
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = m.ptr(table, i * 24 + 8) & ~std::uintptr_t{4};
        out.push_back({m.text(m.ptr(slot, 0x20)), m.ptr(slot, 0x30) & ~std::uintptr_t{4}});
    }
    return out;
}
// The catalog asset of an installed item, by its name.
std::uintptr_t catalog_asset(const SkaterItems& m, const std::string& name) {
    const auto manager = m.ptr(m.base, addr::engine::cosmetics_manager);
    m.check(m.ptr(manager) == m.base + addr::engine::cosmetics_manager_vtable && m.get<std::uint8_t>(manager, 0xa8) == 1,
            "Cosmetic catalog is not ready.");
    const auto buckets = m.ptr(manager, 0x30);
    const auto size = m.get<std::uint32_t>(manager, 0x38), total = m.get<std::uint32_t>(manager, 0x3c);
    m.check(size && size <= 16384 && total && total <= 8192, "Cosmetic catalog exceeds bounds.");
    const auto hash = multiplayer::cosmetic_asset_hash(name);
    auto node = m.ptr(buckets, (hash % size) * 8ULL);
    for (std::uint32_t visited = 0; node && visited < total; ++visited, node = m.ptr(node, 0x10))
        if (m.get<std::uint32_t>(node) == hash) return m.ptr(node, 8) & ~std::uintptr_t{4};
    return 0;
}
std::vector<std::uintptr_t> categories_of(const SkaterItems& m, std::uintptr_t asset) {
    std::vector<std::uintptr_t> out;
    for (const auto category : m.array<std::uintptr_t>(m.ptr(asset, 0x20), 32)) out.push_back(category & ~std::uintptr_t{4});
    return out;
}
// Gives a catalog item one more category: a new array, its own categories and `wanted` after them, in
// memory of our own that is never freed (the item points at it for the rest of the game), laid out as
// the game's: the count in the 4 bytes before the first element.
void add_category(const SkaterItems& m, std::uintptr_t asset, std::uintptr_t wanted) {
    const auto old = m.ptr(asset, 0x20);
    const auto count = m.count(old, 8, 32);
    for (std::size_t i = 0; i < count; ++i)
        if ((m.ptr(old, i * 8) & ~std::uintptr_t{4}) == wanted) return;
    auto* block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 16 + (count + 1) * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    SkaterItems::check(block != nullptr, "No memory for the item's categories.");
    const auto elements = reinterpret_cast<std::uintptr_t>(block + 16);
    const std::uint32_t n = static_cast<std::uint32_t>(count + 1);
    std::memcpy(block + 12, &n, 4);
    for (std::size_t i = 0; i < count; ++i) {
        const auto one = m.ptr(old, i * 8);
        std::memcpy(block + 16 + i * 8, &one, 8);
    }
    std::memcpy(block + 16 + count * 8, &wanted, 8);
    SIZE_T done{};
    SkaterItems::check(WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(asset + 0x20), &elements, sizeof(elements), &done) &&
                           done == sizeof(elements),
                       "Cannot write the item's categories.");
}
// The appearance presets of an installed item, each with the material its own builder puts it on
// (layer_materials.h), from its catalog entry: the asset's name ("Items/cust_bottoms/Own_..."), its
// item data (+0x28) and that data's AssetPaths (+0x98: {name, type} pairs of 16 bytes).
void add_presets(const SkaterItems& m, const std::string& item, std::vector<std::pair<std::string, const char*>>& out) {
    const auto asset = catalog_asset(m, item);
    if (!asset) return;
    const auto name = m.text(m.ptr(asset, 0x18));
    const auto first = name.find('/'), second = first == std::string::npos ? first : name.find('/', first + 1);
    if (second == std::string::npos) return;
    const auto folder = std::string_view(name).substr(first + 1, second - first - 1);
    const auto data = m.ptr(asset, 0x28);
    if (!data) return;
    const auto paths = m.ptr(data, 0x98);
    const auto count = m.count(paths, 16, 32);
    for (std::size_t i = 0; i < count; ++i)
        if (const auto material = layer_materials::material_of(folder, m.get<std::int32_t>(paths, i * 16 + 8)))
            out.emplace_back(m.text(m.ptr(paths, i * 16)), material);
}
// Every owner's layers, in one list: owners by name, each in its own order, none twice.
std::vector<std::string> all_layers(const State& s) {
    std::vector<std::string> out;
    for (const auto& [owner, items] : s.layers)
        for (const auto& item : items)
            if (std::none_of(out.begin(), out.end(), [&](const std::string& o) { return lower(o) == lower(item); })) out.push_back(item);
    return out;
}

void wear(State& s, const SkaterItems& m, std::uintptr_t skater, std::uint64_t now) {
    const auto component = m.component(skater);
    auto recipe = m.capture(skater);
    const auto resource = m.resource(component);
    bool marked{};
    for (const auto& item : recipe.items) marked |= s.ours.contains(item.asset);
    bool changed{}, worn{};
    const auto placed_here = [&](std::size_t i) {
        return std::any_of(s.placed.begin(), s.placed.end(), [&](const Placement& p) { return p.first == i && recipe.items[i].asset == p.second; });
    };
    if (const auto fed = skater_items::feed(m, component); fed.follows_outfit()) {
        SkaterItems::check(!marked || now - s.given_back > 3000, "Waiting for the game to put the outfit back.");
        s.feed = fed;
        // A new skater, or one given back that still holds layers until the game has put the
        // outfit's own items back: what is left of them comes off here.
        for (std::size_t i = 0; i < recipe.items.size(); ++i)
            if (placed_here(i)) recipe.items[i].asset.clear(), changed = true;
        s.placed.clear();
        s.trial.reset();
    } else {
        SkaterItems::check(s.feed && (marked || component == s.component), "The skater does not follow its outfit: left alone.");
        for (const auto& [i, item] : s.placed)
            if (i >= recipe.items.size() || recipe.items[i].asset != item) {
                give_back(s, m, skater, now); // the game changed the skater's items: it is the game's again
                return;
            }
    }
    std::set<std::size_t> used;
    for (const auto& [slot_name, want] : s.wanted) {
        const int i = slot_of(s, m, resource, slot_name);
        if (i < 0 || static_cast<std::size_t>(i) >= recipe.items.size()) continue;
        auto& slot = recipe.items[static_cast<std::size_t>(i)];
        if (!want.covers && !blank(slot.asset) && !s.ours.contains(slot.asset)) continue; // the player's own stays
        worn = true;
        used.insert(static_cast<std::size_t>(i));
        auto parameters = slot.parameters;
        for (std::size_t k = 0; k < want.placement.size() && k < parameters.size(); ++k) parameters[k] = std::bit_cast<std::uint32_t>(want.placement[k]);
        if (slot.asset == want.item && slot.parameters == parameters) continue;
        slot.asset = want.item;
        slot.parameters = std::move(parameters);
        s.ours.insert(want.item);
        changed = true;
    }
    // Layers no longer wanted come off (their slots were empty in the outfit); the rest keep theirs.
    const auto wanted = all_layers(s);
    const auto is_wanted = [&](const std::string& item) {
        return std::any_of(wanted.begin(), wanted.end(), [&](const std::string& w) { return lower(w) == lower(item); });
    };
    std::erase_if(s.placed, [&](const Placement& p) {
        if (is_wanted(p.second) && !used.contains(p.first)) return false;
        recipe.items[p.first].asset.clear();
        changed = true;
        return true;
    });
    for (const auto& p : s.placed) used.insert(p.first);
    const auto slots = wanted.empty() ? std::vector<SlotInfo>{} : slot_infos(m, resource);
    SkaterItems::check(wanted.empty() || slots.size() == recipe.items.size(), "The skater's recipe and its template differ.");
    std::optional<Placement> trial;
    std::vector<std::string> left;
    unsigned waiting{};
    for (const auto& name : wanted) {
        if (std::any_of(s.placed.begin(), s.placed.end(), [&](const Placement& p) { return lower(p.second) == lower(name); })) continue;
        if (std::any_of(recipe.items.begin(), recipe.items.end(), [&](const auto& item) { return lower(item.asset) == lower(name); }))
            continue; // the outfit wears it already
        const auto asset = catalog_asset(m, name);
        if (!asset) {
            left.push_back(name + " (not installed)");
            continue;
        }
        // Its own categories, as the catalog had them before any of ours were added.
        auto known = s.native_categories.find(asset);
        if (known == s.native_categories.end()) known = s.native_categories.emplace(asset, categories_of(m, asset)).first;
        const auto& own = known->second;
        const auto own_kind = [&](std::size_t i) { return std::find(own.begin(), own.end(), slots[i].category) != own.end(); };
        // The slots it may go into, in order: its own kind's, then the spare ones; free, and not known to fail.
        std::vector<std::size_t> candidates;
        const auto consider = [&](std::size_t i) {
            if (!used.contains(i) && blank(recipe.items[i].asset) && !s.bad.contains(pair_key(slots[i].name, name)) &&
                std::find(candidates.begin(), candidates.end(), i) == candidates.end())
                candidates.push_back(i);
        };
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (own_kind(i) && std::find(spare_slots.begin(), spare_slots.end(), lower(slots[i].name)) != spare_slots.end()) consider(i);
        for (const auto spare : spare_slots)
            for (std::size_t i = 0; i < slots.size(); ++i)
                if (lower(slots[i].name) == spare) consider(i);
        if (candidates.empty()) {
            left.push_back(name + " (no free slot)");
            continue;
        }
        // Known to load there, or its own kind's slot: no trial.
        const auto proven = std::find_if(candidates.begin(), candidates.end(),
                                         [&](std::size_t i) { return own_kind(i) || s.good.contains(pair_key(slots[i].name, name)); });
        std::size_t slot{};
        if (proven != candidates.end()) slot = *proven;
        else if (!trial && !s.trial) slot = candidates.front();
        else {
            ++waiting; // one trial at a time
            continue;
        }
        if (!own_kind(slot)) add_category(m, asset, slots[slot].category);
        recipe.items[slot].asset = name;
        used.insert(slot);
        s.placed.emplace_back(slot, name);
        s.ours.insert(name);
        if (proven == candidates.end()) trial = Placement{slot, name};
        changed = true;
    }
    std::string note;
    for (const auto& one : left) note += (note.empty() ? "not worn: " : ", ") + one;
    if (waiting) note += std::format("{}{} more to try on", note.empty() ? "" : "; ", waiting);
    if (note != s.logged_note && !note.empty())
        logging::log(logging::Level::info, logging::Channel::skater, "Outfit layers: {}.", note);
    s.logged_note = note;
    s.layer_note = std::move(note);
    s.worn = worn || !s.placed.empty();
    if (!changed) return;
    // Before the skater is built again: which presets are of items on top, and their own materials.
    std::vector<std::pair<std::string, const char*>> presets;
    for (const auto& [i, item] : s.placed) {
        try {
            add_presets(m, item, presets);
        } catch (const std::exception&) {}
    }
    layer_materials::set(std::move(presets));
    const auto failed_loads = load_error_guard::described();
    skater_items::apply(m, component, recipe);
    s.own = true;
    s.component = component;
    s.applied = now;
    if (trial) s.trial = Trial{*trial, now, failed_loads};
    if (!wanted.empty() || !s.placed.empty()) {
        std::string where;
        for (const auto& [i, item] : s.placed)
            where += std::format("{}{} in {}", where.empty() ? "" : ", ", item, i < slots.size() ? slots[i].name : std::string("its slot"));
        logging::log(logging::Level::info, logging::Channel::skater, "Outfit layers: {} on top{}{}{}.", s.placed.size(), where.empty() ? "" : ": ", where,
                     trial ? std::format(" ({} on trial in its slot)", trial->second) : std::string());
    }
}
// A layer on trial: once its time is up, it stays (it loaded) or comes off (the game described a
// failed load meanwhile) and that slot is not tried for it again.
void judge(State& s, const SkaterItems& m, std::uintptr_t skater, std::uint64_t now) {
    const auto trial = *s.trial;
    const auto component = m.component(skater);
    const auto slots = slot_infos(m, m.resource(component));
    const auto key = pair_key(trial.placement.first < slots.size() ? slots[trial.placement.first].name : std::string{}, trial.placement.second);
    if (load_error_guard::described() == trial.failed_loads) {
        logging::log(logging::Level::info, logging::Channel::skater, "Outfit layers: {} loads from {}.", trial.placement.second,
                     trial.placement.first < slots.size() ? slots[trial.placement.first].name : std::string("its slot"));
        remember(s, true, key);
        s.trial.reset();
        s.dirty = true;
        return;
    }
    remember(s, false, key);
    auto recipe = m.capture(skater); // throws while the game still works on the failed build: tried again
    if (trial.placement.first < recipe.items.size() && recipe.items[trial.placement.first].asset == trial.placement.second)
        recipe.items[trial.placement.first].asset.clear();
    std::erase(s.placed, trial.placement);
    skater_items::apply(m, component, recipe);
    s.applied = now;
    s.trial.reset();
    s.dirty = true;
    logging::log(logging::Level::info, logging::Channel::skater, "Outfit layers: {} does not load from {}; another slot is tried.",
                 trial.placement.second, trial.placement.first < slots.size() ? slots[trial.placement.first].name : std::string("its slot"));
}
void give_back(State& s, const SkaterItems& m, std::uintptr_t skater, std::uint64_t now) {
    const auto component = m.component(skater);
    if (const auto fed = skater_items::feed(m, component); fed.follows_outfit() || s.feed) {
        skater_items::hand_back(m, component, fed.follows_outfit() ? fed : *s.feed);
        s.given_back = now;
    }
    s.own = s.worn = false;
    s.placed.clear();
    s.trial.reset();
}
}

bool allowed_slot(std::string_view slot) noexcept {
    const auto name = lower(slot);
    return std::any_of(std::begin(allowed), std::end(allowed), [&](std::string_view a) { return name.find(a) != std::string::npos; });
}

std::string set(std::string owner, std::string slot, std::string item, std::vector<float> placement, bool covers) {
    auto& s = state();
    slot = lower(slot);
    if (!allowed_slot(slot)) return "only tattoo, moles, eyewear and hand wear slots";
    if (const auto taken = s.wanted.find(slot); taken != s.wanted.end() && taken->second.owner != owner) return taken->second.owner + " has that slot";
    if (!content_cache::catalogs().available) return "the paid items are not known yet; try again in a moment";
    if (profile_runtime::reserved_cosmetic(item)) return "that is a paid store item";
    if (placement.size() > 8 || !std::all_of(placement.begin(), placement.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 100; }))
        return "a placement is at most 8 finite numbers";
    s.wanted[slot] = {std::move(owner), std::move(item), std::move(placement), covers};
    s.dirty = true;
    return {};
}
std::string set_layers(std::string owner, std::vector<std::string> items) {
    auto& s = state();
    if (!items.empty() && !content_cache::catalogs().available) return "the paid items are not known yet; try again in a moment";
    for (const auto& item : items) {
        if (item.empty()) return "an item has no name";
        if (profile_runtime::reserved_cosmetic(item)) return item + " is a paid store item";
    }
    auto& mine = s.layers[owner];
    if (mine == items) {
        if (items.empty()) s.layers.erase(owner);
        return {};
    }
    mine = std::move(items);
    if (mine.empty()) s.layers.erase(owner);
    s.dirty = true;
    return {};
}
Layers layers(std::string_view owner) {
    auto& s = state();
    Layers out;
    if (const auto found = s.layers.find(std::string(owner)); found != s.layers.end()) out.items = found->second;
    for (const auto& item : out.items)
        if (std::any_of(s.placed.begin(), s.placed.end(), [&](const Placement& p) { return lower(p.second) == lower(item); }))
            out.worn.push_back(item);
    if (!out.items.empty()) out.note = s.layer_note;
    return out;
}
void clear(std::string_view owner, std::string_view slot) {
    auto& s = state();
    const auto name = lower(slot);
    if (std::erase_if(s.wanted, [&](const auto& w) { return w.second.owner == owner && (name.empty() || w.first == name); })) s.dirty = true;
}
std::string find_item(std::string_view ending) {
    auto& s = state();
    const auto key = lower(ending);
    if (const auto known = s.found.find(key); known != s.found.end()) return known->second;
    NoBailSkater skater;
    if (!no_bail_skater(skater)) return {};
    try {
        auto name = skater_items::installed_item(SkaterItems{skater_items::readable, skater.base}, key);
        if (s.found.size() < 512) s.found.emplace(key, name);
        return name;
    } catch (...) {
        return {};
    }
}
std::vector<Slot> slots() {
    std::vector<Slot> out;
    NoBailSkater skater;
    if (!no_bail_skater(skater)) return out;
    try {
        const SkaterItems m{skater_items::readable, skater.base};
        const auto recipe = m.capture(skater.entity);
        const auto resource = m.resource(m.component(skater.entity));
        const auto table = m.ptr(resource, 0x28);
        const auto count = m.count(table, 24, multiplayer::max_cosmetic_slots);
        for (std::size_t i = 0; i < count; ++i) {
            const auto slot = m.ptr(table, i * 24 + 8) & ~std::uintptr_t{4};
            Slot view{m.text(m.ptr(slot, 0x20)), i < recipe.items.size() ? recipe.items[i].asset : std::string{}, {}};
            if (i < recipe.items.size())
                for (const auto word : recipe.items[i].parameters) view.parameters.push_back(std::bit_cast<float>(word));
            out.push_back(std::move(view));
        }
    } catch (...) {
    }
    return out;
}

void tick() noexcept {
    auto& s = state();
    if (s.wanted.empty() && s.layers.empty() && !s.own) {
        if (!s.placed.empty()) layer_materials::set({});
        s.placed.clear();
        s.layer_note.clear();
        return;
    }
    try {
        NoBailSkater skater;
        if (!no_bail_skater(skater)) {
            // A respawn or a level change: the next skater comes from the profile, with nothing of ours.
            s.skater = s.slots_of = 0;
            s.own = s.worn = false;
            s.feed.reset();
            s.placed.clear();
            s.trial.reset();
            s.dirty = true;
            return;
        }
        if (skater.entity != s.skater) {
            s.skater = skater.entity;
            s.own = false;
            s.feed.reset();
            s.placed.clear();
            s.trial.reset();
            s.dirty = true;
        }
        const auto now = GetTickCount64();
        const SkaterItems m{skater_items::readable, skater.base};
        const bool menu = multiplayer::sample_game_ui_state(skater.base).in_menu;
        if (menu) s.menu_seen = now;
        const bool nothing = s.wanted.empty() && s.layers.empty();
        if (nothing || menu) {
            if (s.own) give_back(s, m, skater.entity, now);
            s.status = nothing ? "nothing worn" : "a game menu is up: the outfit is the game's";
            s.dirty = true;
            return;
        }
        read_memory(s);
        if (s.trial) {
            if (now - s.trial->at >= trial_time) judge(s, m, skater.entity, now);
            return;
        }
        // Not before the game has had a second after a menu, nor more than once a second; while worn,
        // a look every 3 s whether the game put its own items over ours.
        if (now - s.given_back < 300 || now - s.menu_seen < 1000 || now - s.applied < 1000 || !(s.dirty || (s.worn && now - s.checked > 3000)))
            return;
        s.checked = now;
        wear(s, m, skater.entity, now);
        s.dirty = false;
        s.status = std::format("{} item(s) worn", s.wanted.size() + s.placed.size());
    } catch (const std::exception& error) {
        s.status = std::string("waiting: ") + error.what();
        // Once for each reason, while there are items on top: what keeps them off.
        if (!s.layers.empty() && s.logged_wait != error.what()) {
            s.logged_wait = error.what();
            logging::log(logging::Level::info, logging::Channel::skater, "Outfit layers: waiting ({}).", error.what());
        }
    } catch (...) {
    }
}
std::string status() { return state().status; }
}
