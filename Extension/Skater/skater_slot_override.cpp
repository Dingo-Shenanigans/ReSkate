#include "skater_slot_override.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/skater_slot_override.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace dingosdk {
namespace {
namespace slot_addr = addr::skater_slot_override;
constexpr std::uintptr_t slot_image_size = supported_build::game_image_size;
constexpr std::uintptr_t slot_highest = memory::highest_user_address;
constexpr std::uint32_t slot_target_count = 10;
constexpr std::uint32_t free_slot_count = 3;
constexpr std::size_t slot_entry_size = 0x48;
// How long the first selection waits for saved outfits to become loadable.
constexpr std::uint64_t selection_wait_limit_ms = 60000;

using SlotSettingsLookup = std::uintptr_t (*)(std::uintptr_t, const void*);
using SlotNativeResize = void (*)(std::uintptr_t, std::uint32_t);
using SlotNativeSelected = std::int32_t (*)(std::uintptr_t);

enum class SlotSettingsType : std::size_t {
    loadout,
    game,
    count,
};

struct SlotTypeSpec {
    std::uintptr_t handle_rva;
    std::uintptr_t vtable_rva;
    std::size_t object_size;
    const char* name;
};

constexpr std::array<SlotTypeSpec,
    static_cast<std::size_t>(SlotSettingsType::count)> slot_type_specs{{
    {slot_addr::loadout_settings_handle, slot_addr::loadout_settings_vtable, 0x58, "DingoSkaterLoadoutSettings"},
    {addr::engine::game_settings_handle, addr::engine::game_settings_vtable, 0x80, "DelMarGameSettings"},
}};

struct SlotFieldSpec {
    SlotSettingsType type;
    std::uintptr_t metadata_rva;
    std::uint64_t name_hash;
    std::size_t offset;
    std::uintptr_t scalar_type_rva;
    std::size_t size;
    std::uint32_t desired;
    const char* name;
};

constexpr std::array<SlotFieldSpec, 4> slot_field_specs{{
    // Like the live game: three free presets; the rest stay locked (extra slots
    // were bought with San Van Bucks, which ReSkate does not sell).
    {SlotSettingsType::loadout, slot_addr::min_loadout_count_field, 0x3ac16662, 0x48,
        addr::engine::int32_type, 4, free_slot_count, "MinSkaterLoadoutCount"},
    {SlotSettingsType::loadout, slot_addr::force_own_max_slots_field, 0x2fe2d03d, 0x53,
        addr::engine::bool_type, 1, 0, "ForceOwnMaxSlots"},
    {SlotSettingsType::game, slot_addr::show_prototype_cas_slots_field, 0x320f5d62, 0x78,
        addr::engine::bool_type, 1, 1, "ShowPrototypeCASSlots"},
    {SlotSettingsType::game, slot_addr::show_new_character_preset_selector_field, 0xdefcd489, 0x49,
        addr::engine::bool_type, 1, 1, "ShowNewCharacterPresetSelector"},
}};

struct SlotFieldMetadata {
    std::uint64_t name_hash;
    std::uint64_t offset;
    std::uintptr_t type;
};
static_assert(sizeof(SlotFieldMetadata) == 24);

struct SlotLease {
    bool owned{};
    std::uintptr_t object{};
    std::uint32_t original{};
    std::uint32_t applied{};
};

struct SlotTypeResolution {
    bool attempted{};
    std::uintptr_t object{};
};

struct SlotSnapshot {
    std::size_t index{};
    std::uintptr_t object{};
    std::uint32_t value{};
};

struct SlotManagerSnapshot {
    bool available{};
    bool ui_ready{};
    std::uintptr_t manager{};
    std::uint32_t state{};
    std::uint32_t count{};
};

struct SlotFailure {
    std::string message;
};

struct SlotLastErrorScope {
    DWORD value{GetLastError()};
    ~SlotLastErrorScope() { SetLastError(value); }
};

struct SlotOverrideState {
    std::mutex mutex;
    std::uintptr_t base{};
    DWORD engine_thread{};
    bool initialized{};
    bool active{};
    bool requested{};
    std::array<SlotLease, slot_field_specs.size()> leases{};
    // The manager whose first selection is waiting on saved outfits, and since when.
    std::uintptr_t waiting_manager{};
    std::uint64_t waiting_since{};
    SkaterSlotOverrideObservation observation;
};

SlotOverrideState& slot_override_state() {
    static auto* state = new SlotOverrideState;
    return *state;
}

bool slot_range(std::uintptr_t address, std::size_t size) {
    return address >= 0x10000 && size && size <= slot_highest &&
        address <= slot_highest - size;
}

template<std::size_t N> bool slot_match(
    std::uintptr_t address, const std::array<unsigned char, N>& expected) {
    std::array<unsigned char, N> actual{};
    return memory::read(address, actual) && actual == expected;
}

bool slot_writable(std::uintptr_t address, std::size_t size) {
    if (!slot_range(address, size)) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (address < start || info.RegionSize > slot_highest - start ||
        address + size > start + info.RegionSize) return false;
    switch (info.Protect & 0xff) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool validate_slot_image(std::uintptr_t base) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!memory::read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
        !memory::read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != slot_image_size ||
        !slot_match(base + addr::engine::settings_lookup, slot_addr::settings_lookup_prefix) ||
        !slot_match(base + slot_addr::settings_constructor, slot_addr::settings_constructor_prefix) ||
        !slot_match(base + slot_addr::manager_constructor, slot_addr::manager_constructor_prefix) ||
        !slot_match(base + slot_addr::native_resize, slot_addr::native_resize_prefix) ||
        !slot_match(base + slot_addr::native_selected, slot_addr::native_selected_prefix) ||
        !slot_match(base + slot_addr::native_select, slot_addr::native_select_prefix)) return false;
    for (const auto& field : slot_field_specs) {
        SlotFieldMetadata metadata{};
        const auto& type = slot_type_specs[static_cast<std::size_t>(field.type)];
        if (!memory::read(base + field.metadata_rva, metadata) ||
            metadata.name_hash != field.name_hash || metadata.offset != field.offset ||
            metadata.type != base + field.scalar_type_rva ||
            field.offset + field.size > type.object_size) return false;
    }
    // MaxSkaterLoadoutCount is field-table index 2. It is observed rather than
    // leased because the manager constructor already used it for capacity.
    SlotFieldMetadata maximum{};
    return memory::read(base + slot_addr::max_loadout_count_field, maximum) &&
        maximum.name_hash == 0x4e26401d && maximum.offset == 0x4c &&
        maximum.type == base + addr::engine::int32_type;
}

void slot_require(bool condition, std::string message) {
    if (!condition) throw SlotFailure{std::move(message)};
}

std::uintptr_t resolve_slot_type(SlotOverrideState& state, SlotSettingsType id,
    std::array<SlotTypeResolution,
        static_cast<std::size_t>(SlotSettingsType::count)>& cache) {
    const auto index = static_cast<std::size_t>(id);
    auto& cached = cache[index];
    if (cached.attempted) return cached.object;
    cached.attempted = true;
    const auto& type = slot_type_specs[index];
    std::uintptr_t registry{}, buckets{};
    std::uint32_t bucket_count{};
    if (!memory::read(state.base + addr::engine::settings_manager, registry) ||
        !slot_range(registry, 0xd8) || !memory::read(registry + 0xc8, buckets) ||
        !memory::read(registry + 0xd0, bucket_count) || !slot_range(buckets, 8) ||
        !bucket_count || bucket_count > 0x100000) return 0;
    const auto lookup = reinterpret_cast<SlotSettingsLookup>(state.base + addr::engine::settings_lookup);
    const auto object = lookup(registry,
        reinterpret_cast<const void*>(state.base + type.handle_rva));
    std::uintptr_t vtable{}, object_type{}, registry_after{};
    if (!slot_range(object, type.object_size) || !memory::read(object, vtable) ||
        !memory::read(object + 8, object_type) || vtable != state.base + type.vtable_rva ||
        object_type != state.base + type.handle_rva ||
        !memory::read(state.base + addr::engine::settings_manager, registry_after) ||
        registry_after != registry) return 0;
    cached.object = object;
    return object;
}

std::uint32_t read_slot_scalar(const SlotFieldSpec& field, std::uintptr_t object) {
    std::uint32_t value{};
    if (field.size == 1) {
        std::uint8_t byte{};
        slot_require(memory::read(object + field.offset, byte) && byte <= 1,
            std::string(field.name) + " is unavailable.");
        value = byte;
    } else {
        slot_require(field.size == 4 && memory::read(object + field.offset, value) &&
            value <= slot_target_count, std::string(field.name) + " is unavailable.");
    }
    return value;
}

bool write_slot_scalar(SlotOverrideState& state, const SlotSnapshot& snapshot,
    std::uint32_t value) {
    const auto& field = slot_field_specs[snapshot.index];
    const auto address = snapshot.object + field.offset;
    if (!slot_writable(address, field.size)) return false;
    SIZE_T count{};
    const auto source = field.size == 1 ? static_cast<const void*>(
        reinterpret_cast<const std::uint8_t*>(&value)) : static_cast<const void*>(&value);
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
            source, field.size, &count) || count != field.size) return false;
    const auto actual = read_slot_scalar(field, snapshot.object);
    if (actual != value) return false;
    ++state.observation.writes;
    return true;
}

std::array<SlotSnapshot, slot_field_specs.size()> snapshot_slot_fields(
    SlotOverrideState& state) {
    std::array<SlotTypeResolution,
        static_cast<std::size_t>(SlotSettingsType::count)> cache{};
    std::array<SlotSnapshot, slot_field_specs.size()> result{};
    for (std::size_t i = 0; i < slot_field_specs.size(); ++i) {
        const auto& field = slot_field_specs[i];
        const auto object = resolve_slot_type(state, field.type, cache);
        slot_require(object != 0,
            std::string(slot_type_specs[static_cast<std::size_t>(field.type)].name) +
            " is not initialized.");
        result[i] = {i, object, read_slot_scalar(field, object)};
    }
    const auto loadout = resolve_slot_type(state, SlotSettingsType::loadout, cache);
    std::uint32_t maximum{};
    slot_require(loadout && memory::read(loadout + 0x4c, maximum) &&
        maximum == slot_target_count,
        "MaxSkaterLoadoutCount is not the inspected value 10.");
    return result;
}

void release_replaced_slot_leases(SlotOverrideState& state,
    const std::array<SlotSnapshot, slot_field_specs.size()>& snapshots) {
    for (const auto& snapshot : snapshots) {
        auto& lease = state.leases[snapshot.index];
        if (lease.owned && lease.object != snapshot.object) {
            lease = {};
            state.observation.status =
                "Released a replaced settings identity without a stale write.";
        }
    }
}

bool any_slot_lease(const SlotOverrideState& state) {
    return std::any_of(state.leases.begin(), state.leases.end(),
        [](const SlotLease& lease) { return lease.owned; });
}

void apply_slot_settings(SlotOverrideState& state) {
    auto snapshots = snapshot_slot_fields(state);
    release_replaced_slot_leases(state, snapshots);
    for (const auto& snapshot : snapshots) {
        const auto& field = slot_field_specs[snapshot.index];
        const auto& lease = state.leases[snapshot.index];
        slot_require(!lease.owned || lease.object == snapshot.object,
            std::string(field.name) + " settings object changed.");
        slot_require(!lease.owned || snapshot.value == lease.original ||
            snapshot.value == lease.applied,
            std::string(field.name) + " changed outside ReSkate.");
        slot_require(snapshot.value == field.desired ||
            slot_writable(snapshot.object + field.offset, field.size),
            std::string(field.name) + " is not writable.");
    }

    std::array<bool, slot_field_specs.size()> newly_owned{};
    for (const auto& snapshot : snapshots) {
        const auto& field = slot_field_specs[snapshot.index];
        auto& lease = state.leases[snapshot.index];
        if (snapshot.value == field.desired) continue;
        if (!lease.owned) {
            lease = {true, snapshot.object, snapshot.value, field.desired};
            newly_owned[snapshot.index] = true;
        }
        if (write_slot_scalar(state, snapshot, field.desired)) continue;

        bool rollback_complete = true;
        for (std::size_t i = slot_field_specs.size(); i-- > 0;) {
            if (!newly_owned[i]) continue;
            auto& rollback = state.leases[i];
            const SlotSnapshot prior{i, rollback.object,
                read_slot_scalar(slot_field_specs[i], rollback.object)};
            const bool restored = prior.value == rollback.original ||
                (prior.value == rollback.applied &&
                    write_slot_scalar(state, prior, rollback.original));
            if (restored) rollback = {};
            else rollback_complete = false;
        }
        slot_require(false, rollback_complete ?
            "Customization settings write failed; prior values were restored." :
            "Customization settings write failed and a restoration lease remains active.");
    }
}

void restore_slot_settings(SlotOverrideState& state) {
    auto snapshots = snapshot_slot_fields(state);
    release_replaced_slot_leases(state, snapshots);
    for (const auto& snapshot : snapshots) {
        const auto& field = slot_field_specs[snapshot.index];
        const auto& lease = state.leases[snapshot.index];
        slot_require(!lease.owned || lease.object == snapshot.object,
            std::string(field.name) + " settings object changed.");
        slot_require(!lease.owned || snapshot.value == lease.applied ||
            snapshot.value == lease.original,
            std::string(field.name) + " changed outside ReSkate.");
        slot_require(!lease.owned || snapshot.value == lease.original ||
            slot_writable(snapshot.object + field.offset, field.size),
            std::string(field.name) + " is not writable for restoration.");
    }
    for (const auto& snapshot : snapshots) {
        auto& lease = state.leases[snapshot.index];
        if (!lease.owned) continue;
        if (snapshot.value == lease.applied) {
            slot_require(write_slot_scalar(state, snapshot, lease.original),
                std::string(slot_field_specs[snapshot.index].name) +
                " could not be restored.");
            ++state.observation.restores;
        }
        lease = {};
    }
}

SlotManagerSnapshot read_slot_manager(const SlotOverrideState& state) {
    SlotManagerSnapshot snapshot;
    std::uintptr_t manager{};
    if (!memory::read(state.base + addr::engine::loadout_manager, manager) || !manager)
        return snapshot;
    std::uint32_t machine_state{}, count{};
    std::uintptr_t begin{}, end{}, capacity{}, context{}, vtable{}, begin_method{}, end_method{};
    if (!slot_range(manager, 0x1c8) || !memory::read(manager, machine_state) ||
        machine_state > 7 || !memory::read(manager + 0x150, count) ||
        count > slot_target_count || !memory::read(manager + 0x18, begin) ||
        !memory::read(manager + 0x20, end) || !memory::read(manager + 0x28, capacity) ||
        !begin || begin > end || end > capacity ||
        (end - begin) % slot_entry_size || (capacity - begin) % slot_entry_size ||
        (capacity - begin) / slot_entry_size > slot_target_count ||
        !memory::read(manager + 0x198, context) || !slot_range(context, 8) ||
        !memory::read(context, vtable) ||
        vtable < state.base || vtable > state.base + slot_image_size - 0x68 ||
        !memory::read(vtable + 0x58, begin_method) || !memory::read(vtable + 0x60, end_method) ||
        begin_method < state.base || begin_method >= state.base + slot_image_size ||
        end_method < state.base || end_method >= state.base + slot_image_size) return snapshot;
    std::uintptr_t manager_after{};
    if (!memory::read(state.base + addr::engine::loadout_manager, manager_after) ||
        manager_after != manager) return snapshot;
    snapshot.available = true;
    std::uint64_t root{}, slots{};
    snapshot.ui_ready = memory::read(manager + 0x1a0, root) && root != 0 &&
        memory::read(manager + 0x1a8, slots) && slots != 0;
    snapshot.manager = manager;
    snapshot.state = machine_state;
    snapshot.count = count;
    return snapshot;
}

void maintain_slot_count(SlotOverrideState& state) {
    auto manager = read_slot_manager(state);
    state.observation.manager_available = manager.available;
    state.observation.slot_count = manager.count;
    state.observation.manager_state = manager.state;
    state.observation.ui_ready = manager.ui_ready;
    if (!manager.available) {
        state.observation.status = "Waiting for the game's skater loadout manager.";
        return;
    }
    // The slot list publisher (slot_addr::slot_list_publisher) publishes only the slot list
    // through +198/+1a8. The constructor
    // creates these bindings before waiting for remote inventory (state 2).
    // Waiting for state 3 prevents any slots from appearing in an offline game.
    if (!manager.ui_ready) {
        state.observation.status = "Waiting for the skater preset UI bindings.";
        return;
    }
    if (manager.count == slot_target_count) {
        state.observation.status = "Ten process-local skater preset slots are available.";
        return;
    }
    reinterpret_cast<SlotNativeResize>(state.base + slot_addr::native_resize)(manager.manager, slot_target_count);
    ++state.observation.native_resizes;
    auto after = read_slot_manager(state);
    slot_require(after.available && after.manager == manager.manager &&
        after.count == slot_target_count,
        "The native skater slot refresh did not publish ten slots.");
    state.observation.manager_available = true;
    state.observation.slot_count = after.count;
    state.observation.status = "Restored ten process-local skater preset slots.";
}

void maintain_initial_selection(SlotOverrideState& state, std::uint32_t preferred, bool outfits_loadable) {
    auto& observation = state.observation;
    observation.initial_binding_ready = observation.initial_appearance_ready = false;
    observation.initial_category_count = 0;
    const auto manager = read_slot_manager(state);
    if (!manager.available || !manager.ui_ready || !manager.count) return;
    std::uintptr_t selected_binding{}, appearance{}, categories{};
    std::uint32_t category_count{};
    if (!memory::read(manager.manager + 0x1b0, selected_binding) || !selected_binding) return;
    observation.initial_binding_ready = true;
    const auto selected = reinterpret_cast<SlotNativeSelected>(state.base + slot_addr::native_selected)(manager.manager);
    state.observation.selected_slot = selected;
    if (selected != -1) return; // Never replace a user's selected/edited preset.
    observation.initial_appearance_ready = memory::read(state.base + addr::engine::appearance_manager, appearance) && slot_range(appearance, 8);
    if (memory::read(manager.manager + 8, categories) && slot_range(categories, 16) &&
        memory::read(categories - 4, category_count)) observation.initial_category_count = category_count & 0x7fffffff;
    if (!observation.initial_appearance_ready || !observation.initial_category_count ||
        observation.initial_category_count > 64) return;
    // Selecting a slot makes the game build every slot up to it there and then,
    // each from its saved outfit or, when that cannot be loaded, as a standard
    // skater; it does not ask again until the next level. So the selection
    // waits until saved outfits can be loaded. A catalog that never turns up
    // must not leave the player with no slot at all, hence the limit.
    if (!outfits_loadable) {
        const auto now = GetTickCount64();
        if (state.waiting_manager != manager.manager) {
            state.waiting_manager = manager.manager;
            state.waiting_since = now;
        }
        if (now - state.waiting_since < selection_wait_limit_ms) {
            observation.status = "Waiting for the cosmetics catalog before restoring the saved skater.";
            return;
        }
    }
    state.waiting_manager = 0;  // the next level's manager can be given this one's address
    // The first native selection restores the requested saved preset, or slot 0.
    // It constructs any missing slots from the loaded appearance
    // categories. With previous index -1, it does not save a previous preset or
    // send the selection to Profile/telemetry. It publishes the selector binding.
    const auto index = preferred < manager.count ? preferred : 0;
    reinterpret_cast<SlotNativeResize>(state.base + slot_addr::native_select)(manager.manager, index);
    state.observation.selected_slot = reinterpret_cast<SlotNativeSelected>(state.base + slot_addr::native_selected)(manager.manager);
    if (state.observation.selected_slot == static_cast<std::int32_t>(index)) ++state.observation.initial_selections;
}

void format_slot_observation(SlotOverrideState& state) {
    auto& out = state.observation;
    out.initialized = state.initialized;
    out.available = state.active;
    out.requested = state.requested;
    out.settings_owned = any_slot_lease(state);
    out.target_slot_count = slot_target_count;
    std::ostringstream detail;
    detail << "offline skater slots: requested=" << out.requested
           << ", settings_owned=" << out.settings_owned
           << ", selectors=" << out.selectors_enabled
           << ", manager=" << out.manager_available
           << ", count=" << out.slot_count << '/' << out.target_slot_count;
    out.detail = detail.str();
    std::ostringstream json;
    json << "{\"event\":\"skater_slot_observation\",\"initialized\":" << (out.initialized ? "true" : "false")
         << ",\"available\":" << (out.available ? "true" : "false")
         << ",\"requested\":" << (out.requested ? "true" : "false")
         << ",\"settings_owned\":" << (out.settings_owned ? "true" : "false")
         << ",\"manager_available\":" << (out.manager_available ? "true" : "false")
         << ",\"selectors_enabled\":" << (out.selectors_enabled ? "true" : "false")
         << ",\"slot_count\":" << out.slot_count
         << ",\"manager_state\":" << out.manager_state
         << ",\"ui_ready\":" << (out.ui_ready ? "true" : "false")
         << ",\"selected_slot\":" << out.selected_slot
         << ",\"initial_selections\":" << out.initial_selections
         << ",\"initial_binding_ready\":" << (out.initial_binding_ready ? "true" : "false")
         << ",\"initial_appearance_ready\":" << (out.initial_appearance_ready ? "true" : "false")
         << ",\"initial_category_count\":" << out.initial_category_count
         << ",\"target_slot_count\":" << out.target_slot_count
         << ",\"writes\":" << out.writes
         << ",\"restores\":" << out.restores
         << ",\"native_resizes\":" << out.native_resizes
         << ",\"rejected\":" << out.rejected << '}';
    out.json = json.str();
}

void refresh_slot_observation(SlotOverrideState& state) {
    auto& out = state.observation;
    out.selectors_enabled = false;
    if (state.active) {
        try {
            const auto snapshots = snapshot_slot_fields(state);
            out.selectors_enabled = snapshots[1].value == 1 &&
                snapshots[2].value == 1 && snapshots[3].value == 1;
        } catch (...) {
            out.selectors_enabled = false;
        }
        const auto manager = read_slot_manager(state);
        out.manager_available = manager.available;
        out.manager_state = manager.state;
        out.ui_ready = manager.ui_ready;
        if (manager.available) out.slot_count = manager.count;
    }
    format_slot_observation(state);
}
}

bool initialize_skater_slot_override(
    std::uintptr_t image_base, bool authored_offline_route_active) noexcept {
    SlotLastErrorScope preserve_error;
    auto& state = slot_override_state();
    std::scoped_lock lock(state.mutex);
    state.base = image_base;
    state.engine_thread = 0;
    state.initialized = true;
    state.active = authored_offline_route_active && validate_slot_image(image_base);
    state.requested = false;
    state.leases = {};
    state.waiting_manager = 0;
    state.waiting_since = 0;
    state.observation = {};
    state.observation.initialized = true;
    state.observation.available = state.active;
    state.observation.target_slot_count = slot_target_count;
    state.observation.status = state.active ?
        "Process-local skater slot override is ready." :
        (authored_offline_route_active ?
            "Skater slot override rejected the Skate.exe build identity." :
            "Skater slot override requires the authored-offline route.");
    format_slot_observation(state);
    return state.active;
}

SkaterSlotOverrideObservation update_skater_slot_override(
    SkaterSlotOverrideAction action, std::uint32_t initial_preset, bool outfits_loadable) noexcept {
    SlotLastErrorScope preserve_error;
    auto& state = slot_override_state();
    std::scoped_lock lock(state.mutex);
    if (!state.active) {
        format_slot_observation(state);
        return state.observation;
    }
    const auto thread = GetCurrentThreadId();
    if (!state.engine_thread) state.engine_thread = thread;
    if (state.engine_thread != thread) {
        ++state.observation.rejected;
        state.observation.status =
            "Rejected skater slot update from a different thread.";
        format_slot_observation(state);
        return state.observation;
    }
    try {
        if (action == SkaterSlotOverrideAction::enable) state.requested = true;
        if (action == SkaterSlotOverrideAction::restore) {
            restore_slot_settings(state);
            state.requested = false;
            state.observation.status =
                "Restored customization settings; session slots were retained.";
        } else if (state.requested) {
            apply_slot_settings(state);
            maintain_slot_count(state);
            maintain_initial_selection(state, initial_preset, outfits_loadable);
        }
    } catch (const SlotFailure& failure) {
        ++state.observation.rejected;
        state.observation.status = failure.message;
    } catch (...) {
        ++state.observation.rejected;
        state.observation.status = "Skater slot override rejected an unexpected state.";
    }
    refresh_slot_observation(state);
    return state.observation;
}

SkaterSlotOverrideObservation skater_slot_override_observation() {
    auto& state = slot_override_state();
    std::scoped_lock lock(state.mutex);
    return state.observation;
}
}
