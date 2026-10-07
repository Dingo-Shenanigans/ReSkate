// "Feet only" (vr.hide_body / vr.hide_body_foot 4): while VR runs, the local skater wears the
// Feet Only costume (the NxRoot FeetOnly mod) so first person shows only the shoes. Only the
// costume slot of the live recipe changes; the saved outfit is never written, and other players
// still get it (vr.others_see_outfit). Views that show the whole skater (third person, flip &
// bail, grabs) take the costume off, and the slot's saved item goes back when VR stops.
#include "vr_costume.h"
#include "vr.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Extension/Multiplayer/Remote/native_cosmetics_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Remote/native_cosmetics.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include <Windows.h>
#include <atomic>
#include <format>
#include <optional>

namespace dingosdk::vr {
namespace {
constexpr const char* costume_asset = "Own_Feet_Only_Costume";

bool readable(std::uintptr_t p, void* out, std::size_t size) {
    return size ? memory::peek_bytes(p, out, size) : p >= 0x10000;
}
using Memory = multiplayer::CosmeticMemory<decltype(&readable)>;

std::atomic<int> installed{-1}; // -1 unknown (no skater seen yet)

// The slot VR controls, from the first time the option asks for the costume until VR stops.
struct Swap {
    std::uintptr_t entity{};
    std::size_t index{};                // the costume's slot in the recipe
    multiplayer::CosmeticSlot saved;    // the slot as the saved outfit has it
    std::optional<std::string> pending; // the item last applied, until a check sees it
    std::uint64_t applied_at{};
    int attempts{};
};
std::optional<Swap> swap;
std::uint64_t next_check{}, given_up_until{};

void say(logging::Level level, const std::string& text) {
    logging::log(level, logging::Channel::graphics, "VR costume: {}", text);
}

// A swap rebuilds the whole outfit. The clothes come back in ~30 ms while still loaded, but a
// few seconds behind the costume unloads them (measured: 0.45-0.85 s to load again). A
// hidden second skater, 50 m under the player, wears the clothes so they stay
// loaded (the costume itself loads in ~0.1 s). It lives in the last remote-player slot: a
// session fills slots from 0 and only touches the ones in use, so it never reaches this one.
struct Keeper {
    bool shown = false;
    std::optional<multiplayer::CosmeticRecipe> board;
    std::string detail, logged;
};
Keeper keeper;
constexpr std::size_t keeper_slot = multiplayer::max_remote_players - 1;
void keeper_stop(std::uintptr_t base) noexcept {
    if (!keeper.shown) return;
    const multiplayer::PeerScope scope(keeper_slot);
    multiplayer::remove_remote(base);
    keeper = {};
    say(logging::Level::info, "keeper removed.");
}
void keeper_wear(std::uintptr_t base, std::uintptr_t client, const multiplayer::NativeFrame& local,
    const multiplayer::CosmeticRecipe& outfit) {
    if (!keeper.board) {
        std::string detail;
        const auto captured = multiplayer::capture_cosmetics(base, local, detail);
        if (!captured) return; // the board or the outfit is still settling
        keeper.board = captured->board;
    }
    const multiplayer::PeerScope scope(keeper_slot);
    multiplayer::Pose pose;
    pose.root = local.pose.root;
    pose.root.position[1] -= 50.0f;
    if (!multiplayer::show_remote(base, client, local, pose, keeper.detail)) {
        if (keeper.logged != keeper.detail) say(logging::Level::info, "keeper: " + (keeper.logged = keeper.detail));
        return;
    }
    if (!keeper.shown) say(logging::Level::info, "keeper created.");
    keeper.shown = true;
    std::string detail;
    multiplayer::update_remote_cosmetics(base, local, {outfit, *keeper.board, {}}, detail);
    if (keeper.logged != detail) say(logging::Level::info, "keeper: " + (keeper.logged = detail));
}

// Whether the game finished building this actor's first outfit: a copy while the skater was
// still being created was overwritten by that build, and twice left the costume unbuilt.
bool settled(const Memory& memory, std::uintptr_t entity) {
    static std::uintptr_t seen_entity{};
    static std::uint64_t seen_at{};
    const auto now = GetTickCount64();
    if (entity != seen_entity) {
        seen_entity = entity;
        seen_at = now;
    }
    if (now - seen_at < 3000) return false;
    // And first person has run for 2 s (the VR camera drives the view).
    static std::uint64_t camera_since{};
    if (!status().camera_active) {
        camera_since = 0;
        return false;
    }
    if (!camera_since) camera_since = now;
    if (now - camera_since < 2000) return false;
    const auto appearance = multiplayer::read_native_component(&readable, entity, memory.base + addr::engine::skater_appearance_vtable);
    return memory.ptr(appearance, 0x98) > memory.ptr(appearance, 0x90); // its controllers exist
}

// The template slot that takes the costume, or none when the mod is not installed.
std::optional<std::size_t> costume_slot(const Memory& memory, std::uintptr_t res, const multiplayer::CosmeticRecipe& r) {
    for (std::size_t i = 0; i < r.items.size(); ++i)
        if (memory.item_installed(res, i, {r.items[i].slot, costume_asset, r.items[i].parameters})) return i;
    return std::nullopt;
}

// The native deep copy the remote-skater code uses, on the local component and without
// changing its appearance mode, so the profile's outfit model stays attached.
void apply(const Memory& memory, std::uintptr_t component, const multiplayer::CosmeticRecipe& recipe) {
    memory.validate(memory.resource(component), recipe);
    constexpr auto prefix = addr::native_cosmetics::recipe_copy_prefix;
    memory.check(memory.get<std::array<std::uint8_t, prefix.size()>>(memory.base, addr::native_cosmetics::recipe_copy) == prefix,
        "Native cosmetic copy function differs.");
    multiplayer::BorrowedCosmeticRecipe borrowed(recipe);
    reinterpret_cast<void (*)(std::uintptr_t, const multiplayer::NativeCosmeticRecipe*)>(
        memory.base + addr::native_cosmetics::recipe_copy)(component, &borrowed.value);
}
} // namespace

bool feet_only_installed() noexcept { return installed.load() == 1; }

// Multiplayer sends this view's outfit; other players get the saved item in the costume's slot
// instead (vr.others_see_outfit). Client update thread, like the tick.
void others_outfit(multiplayer::CosmeticRecipe& skater) noexcept {
    if (!swap || !settings().others_see_outfit) return;
    if (swap->index < skater.items.size() && skater.items[swap->index].slot == swap->saved.slot)
        skater.items[swap->index] = swap->saved;
}

void tick_costume(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    const auto now = GetTickCount64();
    static const bool registered = (multiplayer::set_outfit_for_others(&others_outfit), true);
    (void)registered;
    if (!ready || now < next_check) return; // pausing and loading keep the swap
    next_check = now + 100;
    const auto options = settings();
    const bool running = status().running;
    // In use when either "You see" choice is Feet only; worn when the current one is.
    const int seen = on_board_view() ? options.hide_body : options.hide_body_foot;
    const bool option = options.hide_body == Limits::hide_feet_only || options.hide_body_foot == Limits::hide_feet_only;
    const bool feet = running && option && options.view_mode == 0 && !whole_body_view() && seen == Limits::hide_feet_only;
    if (ready && (!running || !option)) keeper_stop(base);
    // Nothing to do without the option or VR, once the menu has had its one look at the catalog.
    if ((!option || !running) && !swap && installed >= 0) return;
    try {
        const auto local = multiplayer::capture_local(base, client, false);
        if (!local.ready) return;
        const Memory memory{&readable, base};
        const auto component = memory.component(local.entity);
        const auto res = memory.resource(component);
        const auto current = memory.capture(local.entity); // throws while an update is pending
        const auto slot = costume_slot(memory, res, current);
        installed = slot ? 1 : 0;
        if (swap && swap->entity != local.entity) swap.reset(); // a new actor builds from the saved outfit
        if (!slot) {
            static bool warned = false;
            if (option && running && !warned) say(logging::Level::warning, "FeetOnly mod not installed.");
            warned = warned || (option && running);
            return;
        }
        const auto& now_item = current.items[*slot];
        const auto dressed = [&](const multiplayer::CosmeticSlot& value) {
            auto r = current;
            r.items[*slot] = value;
            return r;
        };
        if (!swap) {
            if (!running || !option || !settled(memory, local.entity)) return;
            swap = Swap{local.entity, *slot, now_item};
        }
        // What the slot should hold: the costume, or the body (the saved item, or empty when
        // the saved outfit is the costume), or after VR the saved item.
        auto wanted = swap->saved;
        if (feet) wanted.asset = costume_asset;
        else if (running && wanted.asset == costume_asset) wanted.asset.clear();
        if (running && option) {
            auto clothes = swap->saved;
            if (clothes.asset == costume_asset) clothes.asset.clear();
            keeper_wear(base, client, local, dressed(clothes));
        }
        if (now_item == wanted) {
            if (swap->pending)
                say(logging::Level::debug, std::format("slot {} shows \"{}\" after {} ms.", *slot, wanted.asset, now - swap->applied_at));
            swap->pending.reset();
            swap->attempts = 0;
            if (!running) swap.reset();
            return;
        }
        if (now < given_up_until) return;
        if (swap->pending == wanted.asset) {
            // The game rebuilds the outfit a moment after a copy; wait for it before trying again.
            if (now - swap->applied_at < 1500) return;
            if (++swap->attempts > 3) {
                say(logging::Level::warning, std::format("the game keeps replacing slot {}; trying again in 30 s.", *slot));
                given_up_until = now + 30000;
                swap->attempts = 0;
                return;
            }
        } else {
            swap->attempts = 0;
        }
        apply(memory, component, dressed(wanted));
        swap->pending = wanted.asset;
        swap->applied_at = now;
        say(logging::Level::debug, std::format("slot {} \"{}\" -> \"{}\".", *slot, now_item.asset, wanted.asset));
    } catch (const std::exception& e) {
        // Usually a pending outfit update; the next check retries. Logged when the reason changes.
        static std::string last;
        if (last != e.what()) say(logging::Level::debug, last = e.what());
    } catch (...) {}
}
} // namespace dingosdk::vr
