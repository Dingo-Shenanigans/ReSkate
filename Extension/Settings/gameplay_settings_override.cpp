#include "gameplay_settings_override.h"
#include "gameplay_settings_internal.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/debug_settings_access.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/gameplay_settings.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Core/Text/ascii.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dingosdk {
using namespace gameplay_settings_detail;
namespace {
constexpr std::uintptr_t settings_image_size = supported_build::game_image_size;

struct FieldMetadata {
    std::uint64_t name_hash;
    std::uint64_t offset;
    std::uintptr_t type;
};
static_assert(sizeof(FieldMetadata) == 24);

struct LastErrorScope {
    DWORD value{GetLastError()};
    ~LastErrorScope() { SetLastError(value); }
};

template<std::size_t N> bool settings_match(
    std::uintptr_t address, const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> actual{};
    return memory::read(address, actual) && actual == expected;
}

bool validate_settings_image(std::uintptr_t base) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!memory::read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 0x100000 || !memory::read(base + dos.e_lfanew, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.SizeOfImage != settings_image_size ||
        !settings_match(base + addr::engine::settings_lookup, addr::debug_settings_access::settings_lookup_prefix) ||
        // The dedicated getter supplies independent runtime evidence for the
        // fast-travel byte in addition to its reflection record.
        !settings_match(base + gameplay::fast_travel_getter, gameplay::fast_travel_getter_prefix)) return false;
    for (const auto& field : field_specs) {
        if (field.metadata_rva) {
            FieldMetadata metadata{};
            if (!memory::read(base + field.metadata_rva, metadata) ||
                metadata.name_hash != field.name_hash || metadata.offset != field.offset ||
                metadata.type != base + addr::engine::bool_type) return false;
        }
        const auto& type = type_specs[static_cast<std::size_t>(field.type)];
        if (field.offset >= type.object_size) return false;
    }
    return true;
}

bool exact_variable_name(const std::string& incoming, const char* expected) {
    std::size_t length{};
    while (expected[length]) ++length;
    if (incoming.size() != length) return false;
    for (std::size_t i = 0; i < length; ++i)
        if (ascii_lower(incoming[i]) != ascii_lower(expected[i])) return false;
    return true;
}

std::size_t variable_index(const std::string& name) {
    for (std::size_t i = 0; i < field_specs.size(); ++i)
        if (exact_variable_name(name, field_specs[i].name)) return i;
    return field_specs.size();
}

void apply_engine_variable_request(SettingsState& state, const EngineVariableRequest& request) {
    ++state.requests;
    const auto index = variable_index(request.name);
    settings_require(index < field_specs.size(),
        "Unknown engine variable; only exact validated names are accepted.");
    settings_require(request.action == EngineVariableAction::set ||
        request.action == EngineVariableAction::restore,
        "Unknown engine variable action.");
    if (request.action == EngineVariableAction::restore) restore_variable(state, index);
    else set_variable(state, index, request.value);
}

void apply_request(SettingsState& state, const overlay::OfflineFeatureRequest& request) {
    const auto value = group_index(request.group);
    settings_require(value <= group_index(overlay::OfflineFeatureGroup::restore_all),
        "Unknown gameplay settings request.");
    ++state.requests;
    if (request.group == overlay::OfflineFeatureGroup::restore_all) {
        std::string issues;
        for (std::size_t i = 0; i < feature_group_count; ++i) {
            try { restore_group(state, static_cast<overlay::OfflineFeatureGroup>(i)); }
            catch (const SettingsGuard& guard) {
                if (!issues.empty()) issues += ' ';
                issues += guard.message;
            }
        }
        settings_require(issues.empty(), issues);
        state.model.status = "Restored all process-local gameplay settings.";
        return;
    }
    if (request.enabled) enable_group(state, request.group);
    else restore_group(state, request.group);
}
}

bool initialize_gameplay_settings_override(
    std::uintptr_t image_base, bool authored_offline_route_active) noexcept {
    LastErrorScope error;
    try {
        auto& state = settings_state();
        std::lock_guard lock(state.mutex);
        if (state.attempted)
            return state.active && state.base == image_base && authored_offline_route_active;
        state.attempted = true;
        state.base = image_base;
        state.active = authored_offline_route_active && validate_settings_image(image_base);
        state.model.available = state.active;
        reset_variable_model(state);
        state.model.status = !authored_offline_route_active ?
            "Gameplay settings require the exact authored-offline route." :
            (state.active ? "Waiting for the game's settings registry." :
                "Gameplay settings fingerprints do not match this executable.");
        state.detail = state.model.status;
        return state.active;
    } catch (...) { return false; }
}

GameplaySettingsOverrideObservation update_gameplay_settings_override(
    const overlay::OfflineFeatureRequest* request) noexcept {
    LastErrorScope error;
    auto& state = settings_state();
    try {
        std::lock_guard lock(state.mutex);
        if (!state.active) return observation_locked(state);
        if (!state.engine_thread) state.engine_thread = GetCurrentThreadId();
        if (state.engine_thread != GetCurrentThreadId()) {
            ++state.rejected;
            state.model.status = "Gameplay settings update rejected outside the recorded game thread.";
            return observation_locked(state);
        }
        if (request) {
            try { apply_request(state, *request); }
            catch (const SettingsGuard& guard) { ++state.rejected; state.model.status = guard.message; }
            catch (...) { ++state.rejected; state.model.status = "Gameplay settings request raised an exception."; }
        }
        refresh_model(state);
        return observation_locked(state);
    } catch (...) {
        try {
            std::lock_guard lock(state.mutex);
            ++state.rejected;
            state.model.status = "Gameplay settings observation failed.";
            return observation_locked(state);
        } catch (...) { return {}; }
    }
}

GameplaySettingsOverrideObservation update_gameplay_engine_variable(
    const EngineVariableRequest& request) noexcept {
    LastErrorScope error;
    auto& state = settings_state();
    try {
        std::lock_guard lock(state.mutex);
        if (!state.active) return observation_locked(state);
        if (!state.engine_thread) state.engine_thread = GetCurrentThreadId();
        if (state.engine_thread != GetCurrentThreadId()) {
            ++state.rejected;
            state.model.status =
                "Engine variable update rejected outside the recorded game thread.";
            return observation_locked(state);
        }
        try { apply_engine_variable_request(state, request); }
        catch (const SettingsGuard& guard) {
            ++state.rejected;
            state.model.status = guard.message;
        }
        catch (...) {
            ++state.rejected;
            state.model.status = "Engine variable request raised an exception.";
        }
        refresh_model(state);
        return observation_locked(state);
    } catch (...) {
        try {
            std::lock_guard lock(state.mutex);
            ++state.rejected;
            state.model.status = "Engine variable observation failed.";
            return observation_locked(state);
        } catch (...) { return {}; }
    }
}

std::vector<EngineVariableDefinition> gameplay_engine_variable_definitions() {
    std::vector<EngineVariableDefinition> result;
    for (const auto& field : field_specs) {
        std::string title;
        const std::string name(field.name);
        for (std::size_t i = 0; i < name.size(); ++i) {
            const char c = name[i];
            if (i && c >= 'A' && c <= 'Z' && name[i - 1] >= 'a' && name[i - 1] <= 'z') title += ' ';
            title += c;
        }
        const std::string type(type_specs[static_cast<std::size_t>(field.type)].name);
        result.push_back({name, type + "." + name, title + " (" + type + ")"});
    }
    return result;
}

GameplaySettingsOverrideObservation gameplay_settings_override_observation() {
    LastErrorScope error;
    auto& state = settings_state();
    std::lock_guard lock(state.mutex);
    if (state.model.variables.size() != field_specs.size()) reset_variable_model(state);
    return observation_locked(state);
}
}
