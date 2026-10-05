#pragma once
#include "Extension/Profile/runtime_internal.h"

namespace dingosdk::profile_runtime {
struct CosmeticItemInfo {
    std::uint32_t hash{};
    std::vector<std::uint32_t> categories;
    std::string category;
    std::int32_t sort_priority{};
    bool build_kit{};
};

struct CosmeticRuntime {
    void (*initialize_starter)(bool, const void*, const void*){};
    std::uintptr_t item_manager{};
    std::uint32_t item_count{};
    std::map<std::string, CosmeticItemInfo, std::less<>> items;
    // Presets whose last load was refused. A save to one is refused too, so the
    // standard skater the game shows in its place is never written over the
    // outfit on disk.
    std::set<std::string> blocked_loadouts;
    // Saved outfit items that were not installed when their preset loaded (a
    // disabled or removed costume mod). The slot shows its default this session;
    // saves keep the original item unless the player changes that slot. Also
    // saved slots the template did not have then (a mod that adds slots,
    // switched off): those have no slot to show in, and saves carry them along.
    struct HeldSlot { std::size_t recipe{}; profile::CosmeticSlot saved; std::string fallback; };
    std::map<std::string, std::vector<HeldSlot>, std::less<>> held_slots;
    std::set<std::string> diagnostics;
    DWORD update_thread{};
    // Bumped whenever `items` is rebuilt (a live mod apply changes what is
    // installed); the published catalog is replaced when it falls behind.
    std::uint64_t items_generation{};
    std::uint64_t published_generation{};
    std::uintptr_t published_manager{};
    std::uintptr_t published_inventory{};
    std::uintptr_t inventory_data_model{};
    std::uint32_t published_inventory_count{};
    bool catalog_failed{};
    bool ownership_unavailable{};
    std::int32_t observed_selection{-1};
};

CosmeticRuntime& cosmetic_runtime();

bool cosmetic_diagnostic(const char* operation, const char* reason, std::string_view id = {}) noexcept;

std::uint32_t cosmetic_hash(std::string_view value);

bool cosmetic_text(std::uintptr_t p, std::string& value);

bool cosmetic_array(std::uintptr_t p, std::size_t stride, std::size_t limit, std::uint32_t& count);

bool cosmetic_words(std::uintptr_t p, std::size_t limit, std::vector<std::uint32_t>& result);

bool refresh_cosmetic_catalog();
bool reserved_cosmetic(std::string_view asset);
bool reserved_cosmetic_hash(std::uint32_t hash);

struct CosmeticNativeItem {
    const char* asset{};
    const std::uint32_t* parameters{};
    std::uint32_t hash{}, slot{};
};

struct CosmeticNativeRecipe {
    std::uintptr_t resource{};
    const std::uint32_t* scalars{};
    const CosmeticNativeItem* items{};
    const std::uint32_t* transient_mask{};
    std::uint32_t transient_word{};
    std::uint8_t transient_flag{};
    std::array<std::byte, 3> padding{};
};

static_assert(sizeof(CosmeticNativeItem) == 0x18 && sizeof(CosmeticNativeRecipe) == 0x28);

bool read_cosmetic_recipes(const void* vector, profile::CosmeticLoadout& result,
    std::vector<CosmeticNativeRecipe>& native);

template<class T> struct CosmeticBorrowedArray {
    std::vector<std::uint64_t> storage;
    explicit CosmeticBorrowedArray(std::size_t count) : storage((8 + count * sizeof(T) + 7) / 8) {
        const auto size = static_cast<std::uint32_t>(count);
        std::memcpy(storage.data(), &size, 4);
        std::memcpy(reinterpret_cast<std::byte*>(storage.data()) + 4, &size, 4);
    }
    T* data() { return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(storage.data()) + 8); }
};

bool cosmetic_slot_categories(std::uintptr_t resource, std::map<std::uint32_t, std::uint32_t>& result);

struct StarterTemplate {
    const char* name;
    std::uint32_t key, version;
};

inline constexpr std::array starter_templates{
    StarterTemplate{"CASRecipeTemplate", 2759515148U, 2},
    StarterTemplate{"SkateboardRecipeTemplate", 1583459055U, 1},
    StarterTemplate{"MiscUserDataRecipeTemplate", 995494888U, 1},
};

bool complete_starter_recipes(const profile::CosmeticLoadout& value);

bool initialize_missing_starter(const std::vector<CosmeticNativeRecipe>& recipes);

void initialize_starter_from_expression(std::uintptr_t vm, std::uint32_t pc) noexcept;

bool recover_card_only_preset(std::string_view id, const profile::CosmeticLoadout& saved,
    const profile::CosmeticLoadout& defaults);

// The game calls this with a temporary of its own (a slot's default recipes) and
// builds the slot from what is left in it: the saved outfit when this returns
// true, the standard skater when it returns false. It asks once, when the slot
// is first needed after a level load, and there is no record to fill in later.
bool load_cosmetic_hook(const void* wrapper, void* destination);

void save_cosmetic_hook(std::uintptr_t manager, void* record, const char* raw_id, const char* reason, bool changed_only);
}
