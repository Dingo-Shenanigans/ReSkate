#include "texture_refresh.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/RoadRash/skater_items.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace dingosdk::texture_refresh {
namespace {
using road_rash::SkaterItems;
constexpr std::uint64_t after_menu = 500, blank_for = 300, check_after = 500, give_up_after = 10000;

bool contains_any(const std::string& text, std::initializer_list<std::string_view> parts) {
    const auto lowered = road_rash::lower(text);
    for (const auto part : parts)
        if (lowered.find(part) != std::string::npos) return true;
    return false;
}
// What is taken off: everything on the board, and on the skater what is worn over the body, by its
// slot ("cust_tops", "tattoo_armL_1" and the like) or its item. The body itself (head, face, hair,
// skin) stays.
bool refreshed(bool board, const std::string& slot, const std::string& asset) {
    return board ||
           contains_any(slot, {"top", "bottom", "shoe", "footwear", "sock", "hat", "headgear", "glass", "eyewear",
                               "outfit", "costume", "tattoo", "sticker", "decal", "glove", "accessor"}) ||
           contains_any(asset, {"tattoo", "sticker", "decal"});
}
// The name of each of the template's slots, in recipe order: the last part of its path.
std::vector<std::string> slot_names(const SkaterItems& m, std::uintptr_t component) {
    const auto slots = m.ptr(m.resource(component), 0x28);
    std::vector<std::string> out(m.count(slots, 24, multiplayer::max_cosmetic_slots));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = m.text(m.ptr(m.ptr(slots, i * 24 + 8) & ~std::uintptr_t{4}, 0x20));
        if (const auto cut = out[i].find_last_of('/'); cut != std::string::npos) out[i].erase(0, cut + 1);
    }
    return out;
}

struct Target {
    const char* what{};
    std::uintptr_t entity{}, component{};
    road_rash::Feed before;
    road_rash::CosmeticRecipe recipe; // as it was: what the game is to put back
    bool given_back{};
};
enum class Step { idle, blanked, checking };
struct State {
    std::atomic<bool> requested{}, busy{};
    // Client thread from here on.
    bool pending{};
    Step step{Step::idle};
    std::uint64_t since{}, menu_seen{}, last_error{};
    std::array<std::optional<Target>, 2> targets;
};
State& state() {
    static auto* value = new State;
    return *value;
}
template <class... Args> void say(logging::Level level, std::format_string<Args...> text, Args&&... args) {
    logging::log(level, logging::Channel::graphics, "Texture refresh: {}", std::format(text, std::forward<Args>(args)...));
}
void finish(State& s) {
    s.targets = {};
    s.step = Step::idle;
    s.busy.store(false, std::memory_order_release);
}

// The entity's recipe, with the items `refreshed` names taken off; nothing when it has none, or
// when the game does not feed it the player's outfit (someone else's recipe: left alone). Throws
// while the entity's items are updating.
std::optional<Target> prepare(const SkaterItems& m, const char* what, bool board, std::uintptr_t entity,
                              road_rash::CosmeticRecipe& blank) {
    if (!entity) return {};
    const auto component = m.component(entity);
    const auto fed = road_rash::feed(m, component);
    if (!fed.follows_outfit()) {
        say(logging::Level::info, "the {} wears a recipe of its own; left alone.", what);
        return {};
    }
    Target target{what, entity, component, fed, m.capture(entity)};
    blank = target.recipe;
    const auto names = slot_names(m, component);
    SkaterItems::check(names.size() == blank.items.size(), "Cosmetic template changed.");
    std::string off, on;
    for (std::size_t i = 0; i < blank.items.size(); ++i) {
        auto& item = blank.items[i];
        if (item.asset.empty()) continue;
        auto& list = refreshed(board, names[i], item.asset) ? off : on;
        list += (list.empty() ? "" : ", ") + names[i];
        if (&list == &off) item.asset.clear();
    }
    if (off.empty()) return {};
    say(logging::Level::info, "the {}'s items come off for a moment: {}{}{}.", what, off, on.empty() ? "" : "; left on: ", on);
    return target;
}

void start(State& s, std::uintptr_t base, std::uintptr_t client, std::uint64_t now) {
    const auto local = multiplayer::capture_local(base, client, false);
    if (!local.ready) {
        // No skater in play: the next one is composited at the quality as it is now.
        s.pending = false;
        return finish(s);
    }
    const SkaterItems m{road_rash::readable, base};
    std::array<road_rash::CosmeticRecipe, 2> blank;
    // Both are read before either is changed, so a skater still being built changes neither.
    std::array<std::optional<Target>, 2> targets{prepare(m, "skater", false, local.entity, blank[0]),
                                                 prepare(m, "board", true, local.board_entity, blank[1])};
    s.pending = false;
    if (!targets[0] && !targets[1]) {
        say(logging::Level::info, "nothing composited to refresh.");
        return finish(s);
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (!targets[i]) continue;
        try {
            road_rash::apply(m, targets[i]->component, blank[i]);
            s.targets[i] = std::move(targets[i]);
        } catch (const std::exception& error) {
            say(logging::Level::warning, "could not take the {}'s items off ({}).", targets[i]->what, error.what());
        }
    }
    if (!s.targets[0] && !s.targets[1]) return finish(s);
    s.step = Step::blanked;
    s.since = now;
}

// Gives each entity its outfit back once its blank recipe is in. Throws while one is updating.
void give_back(State& s, const SkaterItems& m, std::uint64_t now) {
    for (auto& target : s.targets) {
        if (!target || target->given_back) continue;
        if (m.component(target->entity) != target->component) { // rebuilt meanwhile: the game's already
            target.reset();
            continue;
        }
        road_rash::hand_back(m, target->component, target->before);
        target->given_back = true;
    }
    s.step = Step::checking;
    s.since = now;
}

// The game has put the items back, or else they are put back as they were. Throws while an
// entity's items are updating: checked again.
void check(State& s, const SkaterItems& m) {
    for (auto& target : s.targets) {
        if (!target) continue;
        if (m.component(target->entity) == target->component) {
            if (m.capture(target->entity).items == target->recipe.items) {
                say(logging::Level::info, "the {}'s textures were composited again.", target->what);
            } else {
                // Not expected: the outfit would otherwise stay without them. As they were, as a
                // recipe of its own; a respawn or another item gives it back to the game.
                road_rash::apply(m, target->component, target->recipe);
                say(logging::Level::warning, "the game did not put the {}'s items back; they were put back as they were.",
                    target->what);
            }
        }
        target.reset();
    }
    finish(s);
}
} // namespace

void request() noexcept { state().requested.store(true, std::memory_order_release); }
bool busy() noexcept { return state().busy.load(std::memory_order_acquire); }

void on_client_tick(std::uintptr_t base, std::uintptr_t client) noexcept {
    auto& s = state();
    const auto now = GetTickCount64();
    try {
        if (s.requested.exchange(false, std::memory_order_acq_rel) && !s.pending) {
            // Busy from now on, so Road Rash gives the skater back to the game before it starts.
            s.pending = true;
            s.busy.store(true, std::memory_order_release);
            s.menu_seen = now;
            say(logging::Level::info, "texture settings changed; the skater's and board's textures are composited again "
                "once no menu is up.");
        }
        if (s.step == Step::idle) {
            if (!s.pending) return;
            if (multiplayer::sample_game_ui_state(base).in_menu) {
                s.menu_seen = now;
                return;
            }
            if (now - s.menu_seen < after_menu) return;
            return start(s, base, client, now);
        }
        const SkaterItems m{road_rash::readable, base};
        if (now - s.since > give_up_after) {
            say(logging::Level::warning, "the skater's items did not finish updating; given up.");
            for (auto& target : s.targets)
                if (target && !target->given_back) try {
                        road_rash::apply(m, target->component, target->recipe);
                    } catch (...) {}
            return finish(s);
        }
        if (s.step == Step::blanked && now - s.since >= blank_for) give_back(s, m, now);
        else if (s.step == Step::checking && now - s.since >= check_after) check(s, m);
    } catch (const std::exception& error) {
        // Items still updating, or the skater being built: tried again at a later tick.
        if (now - s.last_error > 5000 && logging::enabled(logging::Level::debug)) {
            s.last_error = now;
            say(logging::Level::debug, "waiting ({}).", error.what());
        }
    } catch (...) {}
}
}
