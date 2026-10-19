#include "spatial_cull_ids.h"

#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/entity_pages.h"
#include "Engine/Game/Build/20260929/spatial_cull_ids.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

namespace dingosdk {
namespace {
namespace contract = addr::spatial_cull_ids;
static_assert(spatial_cull_id_target == addr::entity_pages::capacity);

using MakeList = void (*)(void* list, void* allocator);
using SettingsLookup = std::uintptr_t (*)(std::uintptr_t manager, std::uintptr_t type);
std::atomic<MakeList> original_make_list{};
std::uintptr_t installed_base{};
std::mutex install_mutex;

bool matches(std::uintptr_t address, const auto& expected) noexcept {
    std::array<std::uint8_t, 32> actual{};
    return expected.size() <= actual.size() && memory::read_bytes(address, actual.data(), expected.size()) &&
        std::equal(expected.begin(), expected.end(), actual.begin());
}

// The list reads the setting right after this, on the same thread; nothing else reads it.
void make_list(void* list, void* allocator) {
    const auto incoming_error = GetLastError();
    std::uintptr_t manager{};
    if (memory::peek(installed_base + contract::settings_manager, manager) && manager) {
        const auto settings = reinterpret_cast<SettingsLookup>(installed_base + contract::settings_lookup)(
            manager, installed_base + contract::settings_type);
        if (settings) {
            auto* size = reinterpret_cast<std::uint32_t*>(settings + contract::initial_cull_id_list_size);
            if (*size < spatial_cull_id_target) {
                logging::log(logging::Level::info, logging::Channel::world,
                    "Spatial cull IDs raised: {} -> {}", *size, spatial_cull_id_target);
                *size = spatial_cull_id_target;
            }
        }
    }
    SetLastError(incoming_error);
    original_make_list.load(std::memory_order_acquire)(list, allocator);
}
} // namespace

bool start_spatial_cull_ids(std::uintptr_t base, std::string& error) {
    std::lock_guard lock(install_mutex);
    error.clear();
    if (installed_base) {
        if (installed_base == base) return true;
        error = "Spatial cull IDs are already raised on another image";
        return false;
    }
    if (!matches(base + contract::make_list_rva, contract::make_list_entry) ||
        !matches(base + contract::settings_type_load_rva, contract::settings_type_load) ||
        !matches(base + contract::size_read_rva, contract::size_read)) {
        error = "the cull-ID list does not match the supported game";
        return false;
    }
    auto* target = reinterpret_cast<void*>(base + contract::make_list_rva);
    MakeList original{};
    const auto prepared = hook_prepare(target, reinterpret_cast<void*>(&make_list), reinterpret_cast<void**>(&original));
    if (prepared != HookOk || !original) {
        if (prepared == HookOk) hook_remove(target);
        error = "its hook could not be prepared";
        return false;
    }
    original_make_list.store(original, std::memory_order_release);
    installed_base = base;
    if (hook_enable(target) != HookOk) {
        hook_remove(target);
        installed_base = 0;
        error = "its hook could not be attached";
        return false;
    }
    return true;
}
} // namespace dingosdk
