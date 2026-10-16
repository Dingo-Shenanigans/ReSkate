#include "outfit_layers.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/RoadRash/road_rash.h"
#include "Extension/RoadRash/skater_items.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/World/load_error_guard.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace dingosdk::outfit_layers {
namespace {
using multiplayer::CosmeticRecipe;
using road_rash::Feed;
using road_rash::SkaterItems;

// The slots an item worn on top may go into when its own kind's is taken, in the order they are
// tried: the accessory slots, which hold one small item each and are empty more often than not; then
// the tattoo slots and the face's empty ones (make-up, freckles, acne...), which the skater loader
// also builds an item from. Not the full-body costume (it hides the body) and not the body itself,
// its hair, colours and skin. The left hand's slot comes last of the accessories: the loader finds no
// model there for most items that are not gloves.
constexpr std::array<std::string_view, 22> spare_slots{
    "cust_outfits", "cust_handwear_right", "cust_face_accessory", "cust_earwear_right",
    "cust_eyebrowwear_left", "cust_eyebrowwear_right", "cust_nosewear_left", "cust_nosewear_right",
    "cust_eyewear", "cust_hats", "cust_socks", "cust_handwear",
    "tattoo_arml_1", "tattoo_armr_1", "tattoo_chest_1", "tattoo_legl_1", "tattoo_legr_1",
    "cust_makeup", "cust_freckles", "cust_acne", "cust_vitiligo", "cust_maturity"};
// Not every item loads from every slot: a vest from the left hand's slot leaves the skater with no
// body at all (the game describes a failed load, within half a second). A layer never tried in its
// slot goes on alone, and if the game reports a failed load within this long it comes off and tries
// the next slot. What was found is kept (memory_file) so it is not tried again next time.
constexpr std::uint64_t trial_time = 900;

struct Shared {
    std::mutex mutex;
    std::vector<std::string> wanted;
    std::uint64_t serial{1};
    Status status;
};
Shared& shared() {
    static auto* value = new Shared;
    return *value;
}

using Placement = std::pair<std::size_t, std::string>; // slot, item
struct Trial {
    Placement placement;
    std::uint64_t at{}, failed_loads{};
};
struct State {
    std::atomic<bool> ready{};
    // Client thread from here on.
    std::uintptr_t skater{};
    bool own{};               // the skater wears a recipe of ours: the game passes nothing of the outfit on to it then
    bool dirty{true};
    std::optional<Feed> feed; // how the game feeds the skater its outfit, as last seen
    std::uintptr_t component{};
    std::uint64_t given_back{}, menu_seen{}, checked{}, last_error{};
    std::vector<Placement> placed;            // on the skater now
    std::optional<Trial> trial;               // the one placed last, not yet known to load
    std::set<std::string> good, bad;          // "slot name|item" (lower case): loaded / did not load
    bool memory_read{};
    std::map<std::uintptr_t, std::vector<std::uintptr_t>> native; // catalog item -> its own categories
    std::string last_note;
};
State& state() {
    static auto* value = new State;
    return *value;
}

template <class... Args> void say(logging::Level level, std::format_string<Args...> text, Args&&... args) {
    logging::log(level, logging::Channel::skater, "Outfit layers: {}", std::format(text, std::forward<Args>(args)...));
}
std::string lower(std::string_view text) { return road_rash::lower(text); }
bool blank(const std::string& asset) { return asset.empty() || lower(asset).ends_with("_empty"); }

// Which items load from which slots, kept between runs: beside the collections file
// (RESKATE_COLLECTIONS_FILE), or beside Skate.exe. A line each: "+slot|item" or "-slot|item".
std::filesystem::path memory_file() {
    std::array<wchar_t, 32768> buffer{};
    if (const auto length = GetEnvironmentVariableW(L"RESKATE_COLLECTIONS_FILE", buffer.data(), static_cast<DWORD>(buffer.size()));
        length && length < buffer.size())
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path() / L"ReSkate.layer_slots.txt";
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path() / L"ReSkate.layer_slots.txt";
}
void read_memory(State& s) {
    if (std::exchange(s.memory_read, true)) return;
    try {
        std::ifstream in(memory_file());
        for (std::string line; std::getline(in, line);) {
            if (line.size() < 4 || (line[0] != '+' && line[0] != '-')) continue;
            (line[0] == '+' ? s.good : s.bad).insert(lower(line.substr(1)));
        }
    } catch (...) {}
}
void remember(State& s, bool loads, const std::string& key) {
    (loads ? s.good : s.bad).insert(key);
    try {
        std::ofstream out(memory_file(), std::ios::app);
        out << (loads ? '+' : '-') << key << '\n';
    } catch (...) {}
}
std::string pair_key(std::string_view slot, std::string_view item) { return lower(slot) + "|" + lower(item); }

struct SlotInfo {
    std::string name;
    std::uint32_t category{};
    std::uintptr_t category_pointer{};
};
std::vector<SlotInfo> slot_infos(const SkaterItems& m, std::uintptr_t resource) {
    std::vector<SlotInfo> out;
    const auto slots = m.ptr(resource, 0x28);
    const auto count = m.count(slots, 24, multiplayer::max_cosmetic_slots);
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = m.ptr(slots, i * 24 + 8) & ~std::uintptr_t{4};
        SlotInfo info;
        info.name = m.text(m.ptr(slot, 0x20));
        info.category_pointer = m.ptr(slot, 0x30);
        info.category = m.get<std::uint32_t>(info.category_pointer & ~std::uintptr_t{4}, 0x38);
        out.push_back(std::move(info));
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
        if ((m.ptr(old, i * 8) & ~std::uintptr_t{4}) == (wanted & ~std::uintptr_t{4})) return;
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

void set_status(unsigned worn, std::string note) {
    auto& s = state();
    if (!note.empty() && note != s.last_note) say(logging::Level::info, "{}", note);
    s.last_note = note;
    auto& sh = shared();
    std::lock_guard lock(sh.mutex);
    sh.status.worn = worn;
    sh.status.note = std::move(note);
}

void give_back(State& s, const SkaterItems& m, std::uintptr_t skater, std::uint64_t now, const char* why) {
    const auto component = m.component(skater);
    if (const auto fed = road_rash::feed(m, component); fed.follows_outfit() || s.feed) {
        road_rash::hand_back(m, component, fed.follows_outfit() ? fed : *s.feed);
        s.given_back = now;
        say(logging::Level::debug, "the skater is the game's again ({}).", why);
    }
    s.own = false;
    s.trial.reset();
}

// Brings the skater's layers in line with what is wanted, over the outfit: layers no longer wanted
// come off, those known to load in a free slot go on together, and at most one never tried in its
// slot goes on (a trial). Throws while the skater is still being built: tried again.
void wear(State& s, const SkaterItems& m, std::uintptr_t skater, std::uint64_t now, const std::vector<std::string>& wanted) {
    const auto component = m.component(skater);
    auto recipe = m.capture(skater);
    const auto slots = slot_infos(m, m.resource(component));
    m.check(slots.size() == recipe.items.size(), "The skater's recipe and its template differ.");
    const auto ours = [&](std::size_t i) {
        return std::any_of(s.placed.begin(), s.placed.end(), [&](const auto& p) { return p.first == i && recipe.items[i].asset == p.second; });
    };
    bool marked{};
    for (std::size_t i = 0; i < recipe.items.size(); ++i) marked |= ours(i);
    bool changed{};
    if (const auto fed = road_rash::feed(m, component); fed.follows_outfit()) {
        // The game feeds it its outfit: a new skater, or one given back, which holds our layers until
        // the game has put the outfit's own items back. What is left of ours comes off here.
        SkaterItems::check(!marked || now - s.given_back > 3000, "Waiting for the game to put the outfit back.");
        s.feed = fed;
        s.own = false;
        for (std::size_t i = 0; i < recipe.items.size(); ++i)
            if (ours(i)) recipe.items[i].asset.clear(), changed = true;
        s.placed.clear();
        s.trial.reset();
    } else {
        SkaterItems::check(s.feed && (marked || component == s.component), "The skater wears a recipe that is not ours: left alone.");
        s.own = true;
        for (const auto& [i, item] : s.placed)
            if (i >= recipe.items.size() || recipe.items[i].asset != item) {
                give_back(s, m, skater, now, "the game changed the skater's items");
                return;
            }
    }
    // Layers no longer wanted come off: their slots were empty in the outfit.
    const auto is_wanted = [&](const std::string& item) {
        return std::any_of(wanted.begin(), wanted.end(), [&](const std::string& w) { return lower(w) == lower(item); });
    };
    std::erase_if(s.placed, [&](const Placement& p) {
        if (is_wanted(p.second)) return false;
        recipe.items[p.first].asset.clear();
        changed = true;
        return true;
    });
    std::set<std::size_t> used;
    for (const auto& p : s.placed) used.insert(p.first);
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
        auto known = s.native.find(asset);
        if (known == s.native.end()) known = s.native.emplace(asset, categories_of(m, asset)).first;
        const auto& own = known->second;
        const auto own_kind = [&](std::size_t i) {
            return std::find(own.begin(), own.end(), slots[i].category_pointer & ~std::uintptr_t{4}) != own.end();
        };
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
        if (!own_kind(slot)) add_category(m, asset, slots[slot].category_pointer);
        recipe.items[slot].asset = name;
        used.insert(slot);
        s.placed.emplace_back(slot, name);
        if (proven == candidates.end()) trial = Placement{slot, name};
        changed = true;
    }
    std::string note;
    for (const auto& one : left) note += (note.empty() ? "not worn: " : ", ") + one;
    if (waiting) note += std::format("{}{} more to try on", note.empty() ? "" : "; ", waiting);
    set_status(static_cast<unsigned>(s.placed.size()), note);
    if (!changed) return;
    const auto failed_loads = load_error_guard::described();
    if (s.placed.empty() && !s.own) return; // nothing of ours on a skater the game feeds
    road_rash::apply(m, component, recipe);
    s.own = true;
    s.component = component;
    if (trial) s.trial = Trial{*trial, now, failed_loads};
    std::string where;
    for (const auto& [i, item] : s.placed) where += std::format("{}{} in {}", where.empty() ? "" : ", ", item, slots[i].name);
    say(logging::Level::info, "{} on top{}{}.", s.placed.size(), where.empty() ? "" : ": ", where);
}

// A layer on trial: once its time is up, it stays (it loaded) or comes off (the game described a
// failed load meanwhile) and that slot is not tried for it again this run.
void judge(State& s, const SkaterItems& m, std::uintptr_t skater) {
    const auto trial = *s.trial;
    const auto component = m.component(skater);
    const auto slots = slot_infos(m, m.resource(component));
    const auto key = pair_key(trial.placement.first < slots.size() ? slots[trial.placement.first].name : std::string{}, trial.placement.second);
    if (load_error_guard::described() == trial.failed_loads) {
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
    road_rash::apply(m, component, recipe);
    s.trial.reset();
    s.dirty = true;
    say(logging::Level::info, "{} does not load from {}; another slot is tried.", trial.placement.second,
        trial.placement.first < slots.size() ? slots[trial.placement.first].name : std::string("its slot"));
}
}

void set_layers(std::vector<std::string> items) {
    auto& sh = shared();
    std::lock_guard lock(sh.mutex);
    if (items == sh.wanted) return;
    sh.wanted = std::move(items);
    ++sh.serial;
}
std::vector<std::string> layers() {
    auto& sh = shared();
    std::lock_guard lock(sh.mutex);
    return sh.wanted;
}
Status status() {
    auto& sh = shared();
    std::lock_guard lock(sh.mutex);
    auto out = sh.status;
    out.wanted = static_cast<unsigned>(sh.wanted.size());
    return out;
}

bool start(std::uintptr_t base) noexcept {
    auto& s = state();
    constexpr auto prefix = addr::native_cosmetics::recipe_copy_prefix;
    std::array<unsigned char, prefix.size()> actual{};
    if (!memory::peek(base + addr::native_cosmetics::recipe_copy, actual) || actual != prefix) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Outfit layers cannot change the skater's items: the native recipe copy did not match.");
        return false;
    }
    s.ready.store(true, std::memory_order_release);
    return true;
}

void on_client_tick(std::uintptr_t) noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return;
    try {
        read_memory(s);
        std::vector<std::string> wanted;
        std::uint64_t serial{};
        {
            auto& sh = shared();
            std::lock_guard lock(sh.mutex);
            wanted = sh.wanted;
            serial = sh.serial;
        }
        static std::uint64_t seen_serial{};
        if (serial != seen_serial) {
            seen_serial = serial;
            s.dirty = true;
        }
        NoBailSkater skater;
        if (!no_bail_skater(skater)) {
            // A respawn or a level change: the next skater is built from the profile, nothing of ours on it.
            s.skater = 0;
            s.dirty = true;
            return;
        }
        if (skater.entity != s.skater) {
            s.skater = skater.entity;
            s.dirty = true;
        }
        if (wanted.empty() && !s.own) {
            if (!s.placed.empty() || shared().status.worn) {
                s.placed.clear();
                set_status(0, {});
            }
            return;
        }
        const auto now = GetTickCount64();
        const SkaterItems m{road_rash::readable, skater.base};
        // Road Rash dresses the skater the same way, once it has marks to show; the two would undo
        // each other's recipes.
        const bool rash = road_rash::enabled() && road_rash::bails() > 0;
        const char* why = wanted.empty() && s.placed.empty() ? "nothing to wear on top" : rash ? "Road Rash has marks on" : nullptr;
        if (!why && multiplayer::sample_game_ui_state(skater.base).in_menu) {
            why = "a game menu is up";
            s.menu_seen = now;
        }
        if (why) {
            if (s.own) give_back(s, m, skater.entity, now, why);
            if (rash && !wanted.empty()) set_status(0, "Road Rash has marks on the skater: the layers wait until it heals or is off");
            s.dirty = true;
            return;
        }
        if (s.trial) {
            if (now - s.trial->at >= trial_time) judge(s, m, skater.entity);
            return;
        }
        // Not before the game has had a moment for what it still does to the skater as its menus
        // close (it passes through between screens). While layers are on, a look every few seconds
        // whether the game has put the outfit's items over them or built the skater anew.
        if (now - s.given_back < 300 || now - s.menu_seen < 150 || !(s.dirty || now - s.checked > 3000)) return;
        s.checked = now;
        wear(s, m, skater.entity, now, wanted);
        s.dirty = false;
    } catch (const std::exception& error) {
        const auto now = GetTickCount64();
        if (now - s.last_error > 10000 && logging::enabled(logging::Level::debug)) {
            s.last_error = now;
            say(logging::Level::debug, "waiting ({}).", error.what());
        }
    } catch (...) {}
}

std::string slots() {
    try {
        NoBailSkater skater;
        SkaterItems::check(no_bail_skater(skater), "There is no local skater.");
        const SkaterItems m{road_rash::readable, skater.base};
        const auto component = m.component(skater.entity);
        const auto infos = slot_infos(m, m.resource(component));
        const auto recipe = m.capture(skater.entity);
        std::string out = std::format("{} slots:", infos.size());
        for (std::size_t i = 0; i < infos.size(); ++i) {
            const auto line = std::format("[{}] {} category {}: {}", i, infos[i].name, infos[i].category,
                                          i < recipe.items.size() && !recipe.items[i].asset.empty() ? recipe.items[i].asset : "-");
            say(logging::Level::info, "{}", line);
            out += "\n" + line;
        }
        return out;
    } catch (const std::exception& error) {
        return std::string("Outfit layers: ") + error.what();
    }
}

std::string find(std::string_view part) {
    try {
        NoBailSkater skater;
        SkaterItems::check(no_bail_skater(skater), "There is no local skater.");
        const SkaterItems m{road_rash::readable, skater.base};
        const auto wanted = lower(part);
        const auto manager = m.ptr(m.base, addr::engine::cosmetics_manager);
        const auto buckets = m.ptr(manager, 0x30);
        const auto size = m.get<std::uint32_t>(manager, 0x38), total = m.get<std::uint32_t>(manager, 0x3c);
        std::vector<std::string> names;
        std::uint32_t visited{};
        for (std::uint32_t bucket = 0; bucket < size && visited < total; ++bucket) {
            auto node = m.ptr(buckets, bucket * 8ULL);
            for (std::uint32_t chain = 0; node && chain < total && visited < total; ++chain, ++visited, node = m.ptr(node, 0x10)) {
                const auto asset = m.ptr(node, 8) & ~std::uintptr_t{4};
                if (!asset) continue;
                auto name = m.text(m.ptr(asset, 0x38));
                if (lower(name).find(wanted) != std::string::npos) names.push_back(std::move(name));
            }
        }
        std::sort(names.begin(), names.end());
        std::string out = std::format("{} installed cosmetics have \"{}\" in their name", names.size(), part);
        for (std::size_t i = 0; i < names.size() && i < 40; ++i) out += "\n" + names[i];
        say(logging::Level::info, "{}", out);
        return out;
    } catch (const std::exception& error) {
        return std::string("Outfit layers: ") + error.what();
    }
}

std::string item_named(std::string_view part) {
    const auto list = find(part);
    // find lists the matches after its first line; an exact name wins, else the first.
    std::vector<std::string> names;
    for (std::size_t at = list.find('\n'); at != std::string::npos;) {
        const auto end = list.find('\n', at + 1);
        names.push_back(list.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1));
        at = end;
    }
    for (const auto& name : names)
        if (lower(name) == lower(part)) return name;
    return names.empty() ? std::string{} : names.front();
}
}
