#include "road_rash.h"
#include "road_rash_model.h"
#include "skater_items.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Extension/HallOfMeat/hall_of_meat_skater.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Skater/no_bail.h"
#include <Windows.h>
#include <atomic>
#include <bit>
#include <format>
#include <map>
#include <optional>
#include <string>

namespace dingosdk::road_rash {
namespace {
constexpr const char* enabled_preference = "RoadRash";
constexpr const char* blood_preference = "RoadRashBlood";
constexpr const char* tattoos_preference = "RoadRashCoversTattoos";
// Part of the name of the recipe slot each part's item goes into.
constexpr std::array<std::string_view, part_count> slot_names{"tattoo_legl", "tattoo_arml", "tattoo_legr", "tattoo_armr", "moles"};
constexpr std::uint64_t fall_at_most = 3000, fall_at_least = 400; // milliseconds a fall is watched for its side

struct State {
    // Any thread.
    std::atomic<bool> ready{}, on{true}, blood{true}, covers{};
    std::atomic<bool> heal{}, changed{}; // asked for from another thread, done at the next tick
    std::atomic<int> wanted_bails{-1};
    std::atomic<unsigned> bails{};
    // Client thread from here on.
    Tally tally;
    std::array<std::size_t, part_count> designs{};
    bool designs_drawn{};
    std::uint64_t chance{};
    // The bail being watched for its side.
    bool counted{};
    std::uint64_t wipeouts{}, last_bail{};
    std::optional<std::uint64_t> fall_since;
    Fall fall;
    // What is on the skater.
    std::uintptr_t skater{};
    bool dirty{true}, worn{}, missing_told{};
    bool own{};               // it wears items of ours, as a rule in a recipe of its own: the game passes
                              // nothing of the outfit on to it then
    bool afresh{};            // the marks are to be worked out from the outfit again
    std::optional<Feed> feed; // how the game feeds the skater its outfit, as last seen
    std::uintptr_t component{}; // the item component that was given a recipe
    std::uint64_t checked{}, last_error{}, given_back{}, menu_seen{};
    std::uintptr_t slots_of{};
    std::array<int, part_count> slots{};
    std::map<std::string, std::string> items; // wanted name -> installed item, empty when missing
};
State& state() {
    static auto* value = new State;
    return *value;
}

template <class... Args> void say(logging::Level level, std::format_string<Args...> text, Args&&... args) {
    logging::log(level, logging::Channel::skater, "Road Rash: {}", std::format(text, std::forward<Args>(args)...));
}

bool ours(const std::string& asset) { return lower(asset).find(item_prefix) != std::string::npos; }
// A slot with nothing to see in it: empty, or the game's own "none" item of a face layer.
bool blank(const std::string& asset) { return asset.empty() || lower(asset).ends_with("_empty"); }

// Which design each part wears: drawn afresh whenever the skater is clean.
void draw_designs(State& s) {
    if (!s.chance) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        s.chance = static_cast<std::uint64_t>(now.QuadPart) | 1;
    }
    for (auto& design : s.designs) {
        s.chance ^= s.chance << 13;
        s.chance ^= s.chance >> 7;
        s.chance ^= s.chance << 17;
        design = static_cast<std::size_t>((s.chance >> 20) % design_count);
    }
    s.designs_drawn = true;
}
void clean(State& s) {
    s.tally = {};
    s.fall_since.reset();
    s.fall = {};
    s.bails.store(0, std::memory_order_release);
    draw_designs(s);
    s.dirty = s.afresh = true;
}
std::string levels(const Tally& tally) {
    std::string out;
    for (const auto part : parts) out += std::format("{}{} {}", out.empty() ? "" : ", ", part_names[index(part)], tally.levels[index(part)]);
    return out;
}
void count_bail(State& s, Side side) {
    const auto before = s.tally.levels;
    count(s.tally, side);
    s.bails.store(s.tally.bails, std::memory_order_release);
    if (before != s.tally.levels) s.dirty = true;
    if (logging::enabled(logging::Level::debug))
        say(logging::Level::debug, "bail {} on {} (limbs hit: left {:.1f}, right {:.1f}): {}.", s.tally.bails,
            side == Side::left ? "the left side" : side == Side::right ? "the right side" : "both sides", s.fall.left, s.fall.right,
            levels(s.tally));
}

// The installed item called `name` at its end; empty when the content mod is not installed.
const std::string& item(State& s, const SkaterItems& m, const std::string& name) {
    if (const auto known = s.items.find(name); known != s.items.end()) return known->second;
    return s.items.emplace(name, installed_item(m, name)).first->second;
}
// The item a part wears at a level: of its own design or else the first, from its own set or else
// the unmirrored one, at that level or else the nearest lower one that is installed.
std::string wanted_item(State& s, const SkaterItems& m, Part part, int& level) {
    for (; level > 0; --level)
        for (const auto design : {s.designs[index(part)], std::size_t{0}})
            for (const bool mirrored : {part == Part::right_leg, false}) {
                const auto& found = item(s, m, item_name(part, mirrored, design, level, s.blood.load(std::memory_order_acquire)));
                if (!found.empty()) return found;
            }
    return {};
}
// The recipe slot of each part, by the slot's name in the skater's recipe template.
void find_slots(State& s, const SkaterItems& m, std::uintptr_t resource) {
    s.slots.fill(-1);
    const auto slots = m.ptr(resource, 0x28);
    const auto count = m.count(slots, 24, multiplayer::max_cosmetic_slots);
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = m.ptr(slots, i * 24 + 8) & ~std::uintptr_t{4};
        const auto name = lower(m.text(m.ptr(slot, 0x20)));
        for (std::size_t part = 0; part < part_count; ++part)
            if (s.slots[part] < 0 && name.find(slot_names[part]) != std::string::npos) s.slots[part] = static_cast<int>(i);
    }
    s.slots_of = resource;
}
// Makes the skater wear the marks of its levels, one item per part in the part's own slot, over
// the outfit it wears. Throws while the skater is still being built: tried again.
void wear(State& s, const SkaterItems& m, std::uintptr_t skater, std::uint64_t now) {
    const auto component = m.component(skater);
    auto recipe = m.capture(skater);
    const auto resource = m.resource(component);
    if (s.slots_of != resource) find_slots(s, m, resource);
    bool marked{};
    for (const auto& item : recipe.items) marked |= ours(item.asset);
    // Whose recipe the skater wears is read from the skater: a respawn hands over what looks like a
    // new one and is the same, items and all.
    if (const auto fed = feed(m, component); fed.follows_outfit()) {
        // The game feeds it its outfit: a new skater, or one given back, which holds our items until
        // the game has put the outfit's own back.
        SkaterItems::check(!marked || now - s.given_back > 3000, "Waiting for the game to put the outfit back.");
        s.own = marked;
        s.feed = fed;
    } else {
        // A recipe of its own: ours when our items are in it or it went to this component. Whose
        // else it could be is not ours to change.
        SkaterItems::check(s.feed && (marked || component == s.component), "The skater does not follow its outfit: left alone.");
        s.own = true;
    }
    const bool covers = s.covers.load(std::memory_order_acquire);
    bool changed{}, missing{}, worn{};
    for (const auto part : parts) {
        const auto i = index(part);
        if (s.slots[i] < 0 || static_cast<std::size_t>(s.slots[i]) >= recipe.items.size()) continue;
        auto& slot = recipe.items[static_cast<std::size_t>(s.slots[i])];
        int level = s.tally.levels[i];
        const auto wanted = level ? wanted_item(s, m, part, level) : std::string{};
        if (s.tally.levels[i] && wanted.empty()) missing = true;
        // The player's own tattoo or moles stay, unless the wounds may cover them.
        if (wanted.empty() || (!ours(slot.asset) && !blank(slot.asset) && !covers)) continue;
        worn = true;
        auto parameters = slot.parameters;
        if (part != Part::face && parameters.size() == std::tuple_size_v<Placement>) {
            const auto place = placement(part, level);
            for (std::size_t k = 0; k < place.size(); ++k) parameters[k] = std::bit_cast<std::uint32_t>(place[k]);
        }
        if (slot.asset == wanted && same_parameters(slot.parameters, parameters)) continue;
        slot.asset = wanted;
        slot.parameters = std::move(parameters);
        changed = true;
    }
    if (missing && !s.missing_told) {
        s.missing_told = true;
        say(logging::Level::info, "the skater has marks to show, but the Road Rash items are not installed (a content mod holds them).");
    }
    s.worn = worn;
    if (!changed) return;
    apply(m, component, recipe);
    s.own = true;
    s.component = component;
    say(logging::Level::debug, "marks changed after {} bail(s): {}.", s.tally.bails, levels(s.tally));
}
// Gives the skater back to the game: the outfit's own items return, the player's own tattoos and
// moles with them, and what the player changes in the game's menus reaches the skater again.
// Throws while its items are updating: tried again.
void give_back(State& s, const SkaterItems& m, std::uintptr_t skater, std::uint64_t now, const char* why) {
    const auto component = m.component(skater);
    // One the game feeds already only has to put the outfit's own items back.
    if (const auto fed = feed(m, component); fed.follows_outfit() || s.feed) {
        hand_back(m, component, fed.follows_outfit() ? fed : *s.feed);
        s.given_back = now;
        say(logging::Level::debug, "the skater is the game's again ({}).", why);
    }
    s.own = s.worn = false;
}

// The fall: its side from what the limbs hit, for as long as the body is a ragdoll.
void watch_fall(State& s, const NoBailSkater& skater, std::uint64_t now, bool wiping_out) {
    bool ragdoll = wiping_out;
    if (hall_of_meat::skater_available()) {
        const auto step = hall_of_meat::read_step(skater, 0.0f, false);
        add(s.fall, step.body);
        ragdoll = step.ragdoll.value_or(wiping_out) || wiping_out;
    }
    const auto lasted = now - *s.fall_since;
    if (lasted < fall_at_least || (ragdoll && lasted < fall_at_most)) return;
    count_bail(s, side(s.fall));
    s.fall_since.reset();
    s.fall = {};
}
}

bool start(std::uintptr_t base) noexcept {
    auto& s = state();
    if (s.ready.load(std::memory_order_acquire)) return true;
    constexpr auto prefix = addr::native_cosmetics::recipe_copy_prefix;
    std::array<unsigned char, prefix.size()> actual{};
    if (!memory::peek(base + addr::native_cosmetics::recipe_copy, actual) || actual != prefix) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Road Rash cannot change the skater's items: the native recipe copy did not match.");
        return false;
    }
    if (!no_bail_available()) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Road Rash cannot follow the skater: its bails come through No Bail's hooks, which did not start.");
        return false;
    }
    s.on.store(profile_runtime::local_preference(enabled_preference).value_or(true), std::memory_order_release);
    s.blood.store(profile_runtime::local_preference(blood_preference).value_or(true), std::memory_order_release);
    s.covers.store(profile_runtime::local_preference(tattoos_preference).value_or(false), std::memory_order_release);
    s.ready.store(true, std::memory_order_release);
    logging::log(logging::Level::info, logging::Channel::skater, "Road Rash ready ({}, blood {}).", s.on.load() ? "on" : "off",
        s.blood.load() ? "on" : "off");
    return true;
}

void on_client_tick(std::uintptr_t client) noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return;
    try {
        if (!s.designs_drawn) draw_designs(s);
        if (s.heal.exchange(false, std::memory_order_acq_rel)) clean(s);
        if (s.changed.exchange(false, std::memory_order_acq_rel)) s.dirty = s.afresh = true;
        if (const int wanted = s.wanted_bails.exchange(-1, std::memory_order_acq_rel); wanted >= 0) {
            clean(s);
            s.tally.bails = static_cast<unsigned>(wanted);
            s.tally.left = s.tally.right = static_cast<float>(wanted) / 2;
            grow(s.tally);
            s.bails.store(s.tally.bails, std::memory_order_release);
        }
        NoBailSkater skater;
        if (!no_bail_skater(skater)) {
            // No local skater (a respawn, a level change). After a level change the game builds the
            // next one from the profile, with nothing of ours on it; the count stays, so the marks
            // go back on.
            s.skater = 0;
            s.fall_since.reset();
            s.fall = {};
            s.counted = false;
            s.slots_of = 0;
            s.dirty = true;
            return;
        }
        if (skater.entity != s.skater) {
            s.skater = skater.entity;
            s.dirty = true;
        }
        const bool on = s.on.load(std::memory_order_acquire);
        if (!on && !s.own) return; // switched off, and the skater is the game's: nothing to do
        const auto now = GetTickCount64();
        bool down{};
        if (on) {
            watch_physics_state(client, skater.entity);
            const auto watch = watched_physics_state();
            if (watch.valid) {
                const bool wiping_out = watch.state == addr::no_bail::wipeout_physics_state;
                if (!s.counted) {
                    s.wipeouts = watch.wipeouts;
                    s.counted = true;
                }
                // One slam can enter the wipeout more than once.
                if (watch.wipeouts != s.wipeouts) {
                    s.wipeouts = watch.wipeouts;
                    if (!s.fall_since && now - s.last_bail > 1500) {
                        s.last_bail = now;
                        s.fall_since = now;
                        s.fall = {};
                    }
                }
                if (s.fall_since) watch_fall(s, skater, now, wiping_out);
                down = wiping_out && now - s.last_bail < 6000;
            }
        } else {
            s.fall_since.reset();
        }
        bool marks{};
        if (on)
            for (const int level : s.tally.levels) marks |= level > 0;
        // The game's menus change the outfit, and a skater that wears a recipe of ours gets none of
        // it. So while a menu is up the skater is the game's, as it is with nothing to show or when
        // the marks are worked out again; they go back on, over the outfit as it is by then.
        const char* why = !marks ? "nothing to show" : s.afresh ? "the marks are worked out again" : nullptr;
        if (!why && multiplayer::sample_game_ui_state(skater.base).in_menu) {
            why = "a game menu is up";
            s.menu_seen = now;
        }
        if (why) {
            if (s.own) give_back(s, SkaterItems{readable, skater.base}, skater.entity, now, why);
            s.afresh = false;
            s.dirty = true;
            return;
        }
        // New marks go on once the skater is up again, and not before the game has had a second for
        // what it still does to the skater as a menu closes. While some are on, a look every few
        // seconds whether the game has put the outfit's items over them or built the skater anew.
        if (down || s.fall_since || now - s.given_back < 300 || now - s.menu_seen < 1000 ||
            !(s.dirty || (s.worn && now - s.checked > 3000)))
            return;
        s.checked = now;
        wear(s, SkaterItems{readable, skater.base}, skater.entity, now);
        s.dirty = false;
    } catch (const std::exception& error) {
        // The skater is being rebuilt, or the catalog is not ready: tried again at a later tick.
        const auto now = GetTickCount64();
        if (now - s.last_error > 10000 && logging::enabled(logging::Level::debug)) {
            s.last_error = now;
            say(logging::Level::debug, "waiting ({}).", error.what());
        }
    } catch (...) { /* Marks are never worth the client tick. */ }
}

bool available() noexcept { return state().ready.load(std::memory_order_acquire); }
bool enabled() noexcept { return available() && state().on.load(std::memory_order_acquire); }
bool blood() noexcept { return state().blood.load(std::memory_order_acquire); }
bool covers_tattoos() noexcept { return state().covers.load(std::memory_order_acquire); }

void set_enabled(bool enabled) noexcept {
    auto& s = state();
    s.on.store(enabled, std::memory_order_release);
    // Off: the marks go, and so does the count.
    (enabled ? s.changed : s.heal).store(true, std::memory_order_release);
    profile_runtime::set_local_preference(enabled_preference, enabled);
}
void set_blood(bool blood) noexcept {
    auto& s = state();
    s.blood.store(blood, std::memory_order_release);
    s.changed.store(true, std::memory_order_release);
    profile_runtime::set_local_preference(blood_preference, blood);
}
void set_covers_tattoos(bool covers) noexcept {
    auto& s = state();
    s.covers.store(covers, std::memory_order_release);
    s.changed.store(true, std::memory_order_release);
    profile_runtime::set_local_preference(tattoos_preference, covers);
}
void heal() noexcept { state().heal.store(true, std::memory_order_release); }
void set_bails(unsigned count) noexcept { state().wanted_bails.store(static_cast<int>((std::min)(count, 1000u)), std::memory_order_release); }
unsigned bails() noexcept { return state().bails.load(std::memory_order_acquire); }
}
