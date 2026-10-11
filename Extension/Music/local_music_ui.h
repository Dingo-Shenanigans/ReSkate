#pragma once
#include "Engine/Game/Abi/native_data.h"
#include "Extension/Customization/local_cosmetic_catalog.h"
#include "local_music_assets.h"
#include "Extension/Profile/runtime_internal.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/local_music.h"

namespace dingosdk::profile_runtime {
// The root installs its initialize hook from this contract.
using addr::local_music::music_ui_initialize_contract;

using MusicUiInitialize = void (*)(std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, const std::uint32_t*, const void*);
using MusicFavoriteChange = std::int32_t (*)(std::uintptr_t, std::uint8_t, std::uint64_t, std::uint64_t);

struct MusicUiFunctions {
    MusicUiInitialize initialize{};
    MusicFavoriteChange favorite_change{};
    std::int32_t (*favorite_apply)(std::uintptr_t, std::uint8_t, std::uint64_t, std::uint8_t){};
    void* (*construct_playlist)(void*, const std::uintptr_t*){};
    void* (*construct_song)(void*, const std::uintptr_t*){};
    void (*playlists)(std::uintptr_t, CosmeticShared*){};
    void (*songs)(std::uintptr_t, CosmeticShared*){};
    void* (*insert)(std::uintptr_t, void*, const void*, const void*){};
    void (*complete)(const void*){};
    std::array<std::uintptr_t, 3> allocator{};
};

void music_release_weak(std::uintptr_t owner) noexcept;

struct MusicUiPending {
    std::uintptr_t manager{}, model{}, owner{}, delegate{};
    std::array<std::uint64_t, 7> contexts{};
    std::uint32_t favorites{};
    DWORD thread{};
    std::uint64_t generation{}, next_poll{};
    bool waiting_logged{};
    ~MusicUiPending() {
        if (delegate) game::native_data().values.destroy_delegate(&delegate);
        music_release_weak(owner);
    }
};

struct MusicUiRuntime {
    MusicUiFunctions functions;
    std::unique_ptr<MusicUiPending> pending;
    // Retains the weak owner and identity of the locally hydrated generation.
    std::unique_ptr<MusicUiPending> initialized;
    std::uint64_t generation{};
    bool updating{}, restoring{};
};

MusicUiRuntime& music_ui_runtime();

bool initialize_music_functions(std::uintptr_t base);

bool music_ui_identity(const MusicUiPending& pending);

bool music_ui_current(const MusicUiPending& pending);

CosmeticShared music_ui_lease(const MusicUiPending& pending);

bool music_ui_text(std::string_view value, std::size_t limit = 255);

bool music_ui_catalog_valid(const MusicCatalog& catalog, const std::string& favorites);

std::string music_ui_wire(std::string_view id, std::string_view artist, std::string_view title,
    const std::vector<std::string>* members, std::string_view name = {}, std::string_view artwork = {});

CosmeticShared music_ui_message(const std::string& wire, bool playlist);

struct MusicUiMessages {
    std::vector<CosmeticShared> items;
    ~MusicUiMessages() { for (auto& item : items) cosmetic_release(item); }
    void publish(std::uintptr_t manager, void (*function)(std::uintptr_t, CosmeticShared*)) {
        std::array<std::uintptr_t, 3> vector{reinterpret_cast<std::uintptr_t>(items.data()),
            reinterpret_cast<std::uintptr_t>(items.data() + items.size()), reinterpret_cast<std::uintptr_t>(items.data() + items.size())};
        CosmeticShared borrowed{vector.data(), nullptr}; function(manager, &borrowed);
    }
};

bool music_ui_has_context(std::uintptr_t map, const std::string& id);
std::int32_t music_favorite_change_hook(std::uintptr_t manager, std::uint8_t favorite,
    std::uint64_t context, std::uint64_t song_parent);

void music_ui_initialize_hook(std::uint64_t all, std::uint64_t hidden, std::uint64_t featured,
    std::uint64_t liked, std::uint64_t discovered, std::uint64_t favorites, std::uint64_t songs,
    const std::uint32_t* handle, const void* callback);

void update_music_catalog();

}
