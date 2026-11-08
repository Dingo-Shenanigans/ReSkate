#pragma once
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

// skate.'s bottom left HUD stack (HudViewModel's DpadScoringStack, Engine/Game/Build/20260929/hud_corner.h)
// as the SDK's items go into it: the d-pad's own stack found, and an item of ours put in or taken out
// by its key while the game's items stay as they are. hud_corner.cpp's cover and score_line_hud.cpp's
// score line use it. Client thread, under the model lock (game::ModelWriteLock).
namespace dingosdk::hud_stack {
using multiplayer::menu_data::Context;
using multiplayer::menu_data::Value;
// The most items read from the stack to find the d-pad or put an item of ours in (it holds the d-pad,
// and the score HUD during a line).
inline constexpr unsigned most_items = 16;
// The most read to find or take an item of ours out: a stack that grew past most_items while ours
// was in must never keep it in.
inline constexpr unsigned most_items_out = 64;

// Runs `use(context)` on the game's UI model registry under its model lock, which the game's writers
// take too. False while there is no registry. The SDK's HUD corner and score line share it.
template <class Use> bool with_ui_models(std::uintptr_t base, Use&& use) {
    if (!base) return false;
    namespace menu_data = multiplayer::menu_data;
    const auto ui = menu_data::read<std::uintptr_t>(base + addr::engine::ui_manager);
    const auto manager = ui ? menu_data::read<std::uintptr_t>(ui + addr::engine::ui_model_registry) : 0;
    if (!manager) return false;
    const Context context(base, manager);
    game::ModelWriteLock lock(manager);
    std::forward<Use>(use)(context);
    return true;
}

// An asset's name, or "" for none.
std::string asset_name(std::uintptr_t asset);
// The widget a stack item shows: its Content's blueprint's name.
std::string widget_of(const Context& context, const Value& item);
// A stack's Items array.
Value items_of(const Context& context, const Value& stack);
// The DpadScoringStack that holds the d-pad (HUDDpad_Widget), or none while the HUD is not loaded.
Value find_dpad_stack(const Context& context);
// Where the stack's item with `key` is, if it holds one (read up to most_items_out).
std::optional<unsigned> find_key(const Context& context, const Value& stack, std::uint32_t key);
// Puts `item` (a stack item model of ours) last into the stack's items (one of up to most_items).
void insert_item(const Context& context, const Value& stack, const Value& item);
// Takes the item with `key` out of the stack's items (read up to most_items_out); false when it held none.
bool remove_key(const Context& context, const Value& stack, std::uint32_t key);
} // namespace dingosdk::hud_stack
