#pragma once
#include "Extension/Profile/runtime_internal.h"

namespace dingosdk::profile_runtime {
inline constexpr std::uint32_t player_card_template = 2169419386U;

struct PlayerCardFunctions {
    void* (*construct_recipe)(void*){};
    void (*destroy_recipe)(void*){};
    bool (*get_recipe)(std::uintptr_t, std::uint32_t, unsigned, void*){};
    void (*set_recipe)(std::uintptr_t, std::uint32_t, unsigned, const void*){};
    bool (*ready)(std::uintptr_t){};
    std::uint64_t (*get_local_info)(){};
    void* (*construct_info)(void*){};
};

struct PlayerCardRuntime {
    PlayerCardFunctions functions;
    std::uintptr_t recipe_manager{};
    // Read without the native lock by local_player_info_hook, which the game's scripts call many
    // times a frame: a lock there stalled the client update whenever another thread held it.
    std::atomic<std::uintptr_t> data_model{};
    std::uint64_t next_poll{};
    std::atomic<std::uint64_t> info_handle{};
    bool restored{}, failed{}, display_ready{}, display_pending_logged{};
    std::optional<profile::CosmeticLoadout> observed;
    std::string name_feedback;
};

PlayerCardRuntime& player_card_runtime();

void initialize_player_card_functions(std::uintptr_t base);

struct PlayerCardRecipeGuard {
    // This native value has the same layout as CosmeticNativeRecipe after resource.
    std::array<std::uintptr_t, 4> words{};
    PlayerCardRecipeGuard() { player_card_runtime().functions.construct_recipe(words.data()); }
    ~PlayerCardRecipeGuard() { player_card_runtime().functions.destroy_recipe(words.data()); }
};

bool read_player_card(std::uintptr_t manager, profile::CosmeticLoadout& result);

bool player_card_owned(const profile::CosmeticLoadout& value);

void apply_player_card(std::uintptr_t manager, const profile::CosmeticLoadout& value);

std::uint64_t local_player_info_hook();
// Whose info the local-player lookup gives instead, for a graph that asks for "the local
// player" and means someone else (the placed-object prompt: Extension/Objects/object_prompt.h).
// 0 from it is the local player as usual. Set once by the multiplayer HUD; it is called on
// every lookup, so it must answer 0 at once when no such graph is running.
inline std::atomic<std::uint64_t (*)() noexcept> player_info_override{};

bool publish_player_card(const profile::CosmeticLoadout& value);

void update_player_card();
}
