#include "native_render_resource_pool.h"

#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/render_resource_pool.h"

#include <Windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <utility>

namespace dingosdk {
namespace {
namespace contract = addr::render_resource_pool;

constexpr std::size_t record_bytes =
    native_render_resource_capacity * native_render_resource_record_size;
constexpr std::size_t metadata_bytes = native_render_resource_capacity * sizeof(std::uint32_t);
constexpr std::size_t resource_flag_bytes = native_render_resource_capacity;
constexpr std::size_t metadata_storage_bytes = metadata_bytes + resource_flag_bytes;
constexpr std::size_t index_table_capacity = native_render_resource_capacity + 1;
constexpr std::size_t index_bucket_count = native_render_resource_capacity + 1;
constexpr std::size_t index_bucket_slots = index_bucket_count + 1;
constexpr std::size_t index_node_size = 0x10;
constexpr std::size_t index_bucket_bytes = index_bucket_slots * sizeof(std::uintptr_t);
constexpr std::size_t index_node_bytes = index_table_capacity * index_node_size;
constexpr std::size_t resource_state_stride = 0x738;
constexpr std::size_t resource_state_bytes =
    native_render_resource_capacity * resource_state_stride;
constexpr std::size_t relation_masks_per_resource = 6;
constexpr std::size_t relation_mask_bytes = native_render_resource_capacity *
    relation_masks_per_resource * sizeof(std::uint64_t);
constexpr std::ptrdiff_t vector_begin_offset = contract::vector_begin_offset;
constexpr std::ptrdiff_t vector_end_offset = contract::vector_end_offset;
constexpr std::ptrdiff_t vector_capacity_offset = contract::vector_capacity_offset;
constexpr std::ptrdiff_t metadata_begin_offset = contract::metadata_begin_offset;
constexpr std::ptrdiff_t metadata_end_offset = contract::metadata_end_offset;
constexpr std::ptrdiff_t resource_state_vector_offset = contract::resource_state_vector_offset;
constexpr std::ptrdiff_t relation_mask_vector_offset = contract::relation_mask_vector_offset;
constexpr std::ptrdiff_t index_buckets_offset = 0x00;
constexpr std::ptrdiff_t index_bucket_count_offset = 0x08;
constexpr std::ptrdiff_t index_free_list_offset = 0x20;
constexpr std::ptrdiff_t index_node_cursor_offset = 0x28;
constexpr std::ptrdiff_t index_node_end_offset = 0x30;
constexpr std::ptrdiff_t index_node_stride_offset = 0x38;
static_assert(record_bytes == 0x00f00000);
static_assert(metadata_bytes == 0x00040000);
static_assert(resource_flag_bytes == 0x00010000);
static_assert(metadata_storage_bytes == 0x00050000);
static_assert(index_table_capacity == 0x10001);
static_assert(index_bucket_slots == 0x10002);
static_assert(index_node_bytes == 0x100010);
static_assert(resource_state_bytes == 0x07380000);
static_assert(relation_mask_bytes == 0x00300000);

using NativeConstructor = void* (__fastcall *)(void*, void*);
using IndexTableConstructor = void* (__fastcall *)(void*, void*, void*);

// Records and metadata every renderer built on the constructor uses; the
// resource states and relation masks only the state owner has
// (contract::state_owner_call_rva). 115.5 MB of states went to the other
// renderer as well, which never touches them.
struct Buffers {
    std::byte* records{};
    std::uint32_t* metadata{};
    std::uint8_t* resource_flags{};
    std::byte* resource_states{};
    std::uint64_t* relation_masks{};

    explicit operator bool() const noexcept { return records && metadata && resource_flags; }
    bool has_states() const noexcept { return resource_states && relation_masks; }
};

struct IndexTableBuffers {
    std::uintptr_t* buckets{};
    std::byte* nodes{};

    explicit operator bool() const noexcept { return buckets && nodes; }
};

std::atomic<NativeConstructor> original_constructor{};
std::atomic<IndexTableConstructor> original_index_table_constructor{};
std::mutex install_mutex;
std::mutex allocation_mutex;
Buffers reserved_buffers{};
std::array<IndexTableBuffers, 2> reserved_index_tables{};
std::size_t next_reserved_index_table{};
std::byte* resource_flag_stubs{};
std::uintptr_t installed_base{};
// Where the state owner's constructor call returns to in the game.
std::atomic<std::uintptr_t> state_owner_return{};
// The renderer that has resource states, once constructed.
std::atomic<std::uintptr_t> state_owner{};

bool read_memory(const void* source, void* destination, std::size_t size) noexcept {
    SIZE_T count{};
    return ReadProcessMemory(GetCurrentProcess(), source, destination, size, &count) && count == size;
}

bool matches(std::uintptr_t address, std::span<const std::uint8_t> expected) noexcept {
    if (expected.empty()) return true;
    std::array<std::uint8_t, 32> actual{};
    if (expected.size() > actual.size()) return false;
    return read_memory(reinterpret_cast<const void*>(address), actual.data(), expected.size()) &&
        std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

bool validate_contract(std::uintptr_t base) noexcept {
    if (!base || base > std::numeric_limits<std::uintptr_t>::max() - supported_build::game_image_size)
        return false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read_memory(reinterpret_cast<const void*>(base), &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        dos.e_lfanew > 0x1000 ||
        !read_memory(reinterpret_cast<const void*>(base + dos.e_lfanew), &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != supported_build::game_image_size ||
        !matches(base + contract::constructor_rva, contract::constructor_entry) ||
        !matches(base + contract::index_table_constructor_rva,
            contract::index_table_constructor_entry) ||
        !matches(base + contract::resource_flags_priority_rva,
            contract::resource_flags_read_priority) ||
        !matches(base + contract::resource_flags_visibility_rva,
            contract::resource_flags_read_visibility)) return false;
    // The state owner's call to the constructor: an E8 call whose target is it.
    std::array<std::uint8_t, 5> call{};
    std::int32_t displacement{};
    if (!read_memory(reinterpret_cast<const void*>(base + contract::state_owner_call_rva), call.data(), call.size()) ||
        call[0] != 0xe8) return false;
    std::memcpy(&displacement, call.data() + 1, sizeof(displacement));
    if (contract::state_owner_call_rva + call.size() + displacement != contract::constructor_rva) return false;
    for (const auto& patch : contract::patches)
        if (patch.expected.size() != patch.replacement.size() ||
            !matches(base + patch.rva, patch.expected)) return false;
    return true;
}

void free_buffers(Buffers& buffers) noexcept;

Buffers allocate_buffers(bool states) noexcept {
    Buffers result{};
    result.records = static_cast<std::byte*>(VirtualAlloc(
        nullptr, record_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!result.records) return {};
    result.metadata = static_cast<std::uint32_t*>(VirtualAlloc(
        nullptr, metadata_storage_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!result.metadata) {
        VirtualFree(result.records, 0, MEM_RELEASE);
        return {};
    }
    result.resource_flags = reinterpret_cast<std::uint8_t*>(result.metadata) + metadata_bytes;
    if (!states) return result;
    result.resource_states = static_cast<std::byte*>(VirtualAlloc(
        nullptr, resource_state_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    result.relation_masks = !result.resource_states ? nullptr : static_cast<std::uint64_t*>(VirtualAlloc(
        nullptr, relation_mask_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!result.has_states()) {
        free_buffers(result);
        return {};
    }
    return result;
}

IndexTableBuffers allocate_index_table_buffers() noexcept {
    IndexTableBuffers result{};
    result.buckets = static_cast<std::uintptr_t*>(VirtualAlloc(
        nullptr, index_bucket_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!result.buckets) return {};
    result.nodes = static_cast<std::byte*>(VirtualAlloc(
        nullptr, index_node_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!result.nodes) {
        VirtualFree(result.buckets, 0, MEM_RELEASE);
        return {};
    }
    // Native lookup tables keep an all-ones marker immediately after the
    // addressable bucket range.
    result.buckets[index_bucket_count] = std::numeric_limits<std::uintptr_t>::max();
    return result;
}

void free_buffers(Buffers& buffers) noexcept {
    if (buffers.relation_masks) VirtualFree(buffers.relation_masks, 0, MEM_RELEASE);
    if (buffers.resource_states) VirtualFree(buffers.resource_states, 0, MEM_RELEASE);
    if (buffers.metadata) VirtualFree(buffers.metadata, 0, MEM_RELEASE);
    if (buffers.records) VirtualFree(buffers.records, 0, MEM_RELEASE);
    buffers = {};
}

void free_index_table_buffers(IndexTableBuffers& buffers) noexcept {
    if (buffers.nodes) VirtualFree(buffers.nodes, 0, MEM_RELEASE);
    if (buffers.buckets) VirtualFree(buffers.buckets, 0, MEM_RELEASE);
    buffers = {};
}

void free_reserved_allocations() noexcept {
    free_buffers(reserved_buffers);
    for (auto& table : reserved_index_tables) free_index_table_buffers(table);
    next_reserved_index_table = 0;
}

Buffers take_buffers(bool states) noexcept {
    std::lock_guard lock(allocation_mutex);
    // Two renderers are built on the constructor: the first takes the reserved
    // records and metadata, the state owner the reserved states, whichever
    // comes first. Another instance costs an isolated allocation of its own
    // and avoids aliasing records.
    Buffers result{};
    if (reserved_buffers) {
        result.records = std::exchange(reserved_buffers.records, nullptr);
        result.metadata = std::exchange(reserved_buffers.metadata, nullptr);
        result.resource_flags = std::exchange(reserved_buffers.resource_flags, nullptr);
    } else {
        result = allocate_buffers(false);
        if (!result) return {};
    }
    if (!states) return result;
    if (reserved_buffers.has_states()) {
        result.resource_states = std::exchange(reserved_buffers.resource_states, nullptr);
        result.relation_masks = std::exchange(reserved_buffers.relation_masks, nullptr);
        return result;
    }
    const auto fresh = allocate_buffers(true);
    if (!fresh) { free_buffers(result); return {}; }
    result.resource_states = fresh.resource_states;
    result.relation_masks = fresh.relation_masks;
    VirtualFree(fresh.metadata, 0, MEM_RELEASE);
    VirtualFree(fresh.records, 0, MEM_RELEASE);
    return result;
}

IndexTableBuffers take_index_table_buffers() noexcept {
    std::lock_guard lock(allocation_mutex);
    if (next_reserved_index_table < reserved_index_tables.size()) {
        auto& reserved = reserved_index_tables[next_reserved_index_table++];
        const auto result = reserved;
        reserved = {};
        return result;
    }
    return allocate_index_table_buffers();
}

template<class T>
void store_field(void* owner, std::ptrdiff_t offset, T value) noexcept {
    std::memcpy(static_cast<std::byte*>(owner) + offset, &value, sizeof(value));
}

template<class T>
T load_field(void* owner, std::ptrdiff_t offset) noexcept {
    T result{};
    std::memcpy(&result, static_cast<std::byte*>(owner) + offset, sizeof(result));
    return result;
}

[[noreturn]] void allocation_failure() noexcept {
    RaiseFailFastException(nullptr, nullptr, 0);
    std::terminate();
}

void* __fastcall construct_index_table(void* self, void* first, void* second) {
    const auto incoming_error = GetLastError();
    const auto original = original_index_table_constructor.load(std::memory_order_acquire);
    if (!self || !original) allocation_failure();
    SetLastError(incoming_error);
    auto* const result = original(self, first, second);

    // The native constructor leaves an empty 8,193-node table in inline
    // storage. Redirect that empty table before any resource can insert.
    const auto buffers = take_index_table_buffers();
    if (!buffers) allocation_failure();
    auto* const node_end = buffers.nodes + index_node_bytes;
    store_field(self, index_buckets_offset, buffers.buckets);
    store_field(self, index_bucket_count_offset,
        static_cast<std::uint32_t>(index_bucket_count));
    store_field(self, index_free_list_offset, static_cast<std::byte*>(nullptr));
    store_field(self, index_node_cursor_offset, buffers.nodes);
    store_field(self, index_node_end_offset, node_end);
    store_field(self, index_node_stride_offset, static_cast<std::size_t>(index_node_size));
    return result;
}

void* __fastcall construct_native_renderer(void* self, void* configuration) {
    const auto incoming_error = GetLastError();
    // (The hook is jumped to, so this returns where the game's call does.)
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool states = caller == state_owner_return.load(std::memory_order_acquire);
    const auto buffers = take_buffers(states);
    const auto original = original_constructor.load(std::memory_order_acquire);
    if (!self || !buffers || (states && !buffers.has_states()) || !original) allocation_failure();
    logging::log(logging::Level::info, logging::Channel::graphics,
        "Render-resource pool: renderer built from Skate.exe+{:#x}, {}.", caller - installed_base,
        states ? "with the enlarged resource states" : "without resource states (it has none)");

    auto* const record_end = buffers.records + record_bytes;
    auto* const metadata_end = buffers.metadata + native_render_resource_capacity;
    auto* const relation_mask_begin = reinterpret_cast<std::byte*>(buffers.relation_masks);

    // The patched constructor loads its initial vector pointer from +0x90,
    // constructs the native default record there, and advances +0x98 itself.
    store_field(self, vector_begin_offset, buffers.records);
    // Both renderers read the metadata through these pointers (render_resource_pool.h). The other
    // renderer was allocated 0x2daa00 bytes, and they were written 18 MB past its end, into
    // whatever lay there (a crash in store_field when nothing did): it is allocated the state
    // owner's size now (the patch at 0x030c64cc), so they are its own.
    store_field(self, metadata_begin_offset, buffers.metadata);
    store_field(self, metadata_end_offset, metadata_end);
    if (states) {
        store_field(self, resource_state_vector_offset, buffers.resource_states);
        store_field(self, relation_mask_vector_offset, relation_mask_begin);
    }
    SetLastError(incoming_error);
    auto* const result = original(self, configuration);

    // The side-array fields are initialized by later renderer setup on the
    // stock build. Keep the external values authoritative across construction.
    store_field(self, metadata_begin_offset, buffers.metadata);
    store_field(self, metadata_end_offset, metadata_end);
    if (states) state_owner.store(reinterpret_cast<std::uintptr_t>(self), std::memory_order_release);
    if (load_field<std::byte*>(self, vector_begin_offset) != buffers.records ||
        load_field<std::byte*>(self, vector_capacity_offset) != record_end ||
        load_field<std::byte*>(self, vector_end_offset) < buffers.records ||
        load_field<std::byte*>(self, vector_end_offset) > record_end)
        allocation_failure();
    return result;
}

bool write_bytes(std::uintptr_t address, std::span<const std::uint8_t> bytes) noexcept {
    DWORD previous{};
    auto* const destination = reinterpret_cast<void*>(address);
    if (!VirtualProtect(destination, bytes.size(), PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(destination, bytes.data(), bytes.size());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), destination, bytes.size()) != FALSE;
    DWORD discarded{};
    const bool restored = VirtualProtect(destination, bytes.size(), previous, &discarded) != FALSE;
    return flushed && restored;
}

constexpr std::size_t resource_flag_stub_page_bytes = 0x1000;
constexpr std::size_t priority_stub_offset = 0x00;
constexpr std::size_t visibility_stub_offset = 0x40;

bool relative_jump(std::uintptr_t instruction, std::uintptr_t destination,
    std::int32_t& displacement) noexcept {
    const auto delta = static_cast<std::int64_t>(destination) -
        static_cast<std::int64_t>(instruction + 5);
    if (delta < std::numeric_limits<std::int32_t>::min() ||
        delta > std::numeric_limits<std::int32_t>::max()) return false;
    displacement = static_cast<std::int32_t>(delta);
    return true;
}

template<std::size_t N>
bool write_stub(std::byte* destination, std::span<const std::uint8_t, N> prefix,
    std::uintptr_t return_address) noexcept {
    std::array<std::uint8_t, N + 5> bytes{};
    std::memcpy(bytes.data(), prefix.data(), prefix.size());
    bytes[N] = 0xe9;
    std::int32_t displacement{};
    const auto jump = reinterpret_cast<std::uintptr_t>(destination) + N;
    if (!relative_jump(jump, return_address, displacement)) return false;
    std::memcpy(bytes.data() + N + 1, &displacement, sizeof(displacement));
    std::memcpy(destination, bytes.data(), bytes.size());
    return true;
}

bool prepare_resource_flag_stubs(std::uintptr_t base) noexcept {
    resource_flag_stubs = static_cast<std::byte*>(
        memory::allocate_near_image(base, supported_build::game_image_size, resource_flag_stub_page_bytes));
    if (!resource_flag_stubs) return false;
    std::memset(resource_flag_stubs, 0xcc, resource_flag_stub_page_bytes);
    if (!write_stub(resource_flag_stubs + priority_stub_offset,
            std::span{contract::priority_stub_prefix},
            base + contract::resource_flags_priority_rva +
                contract::resource_flags_read_priority.size()) ||
        !write_stub(resource_flag_stubs + visibility_stub_offset,
            std::span{contract::visibility_stub_prefix},
            base + contract::resource_flags_visibility_rva +
                contract::resource_flags_read_visibility.size())) {
        VirtualFree(resource_flag_stubs, 0, MEM_RELEASE);
        resource_flag_stubs = nullptr;
        return false;
    }
    DWORD previous{};
    if (!VirtualProtect(resource_flag_stubs, resource_flag_stub_page_bytes,
            PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), resource_flag_stubs,
            resource_flag_stub_page_bytes)) {
        VirtualFree(resource_flag_stubs, 0, MEM_RELEASE);
        resource_flag_stubs = nullptr;
        return false;
    }
    return true;
}

template<std::size_t N>
bool install_generated_jump(std::uintptr_t address, std::byte* stub,
    std::span<const std::uint8_t, N> expected) noexcept {
    std::array<std::uint8_t, N> replacement{};
    replacement.fill(0x90);
    replacement[0] = 0xe9;
    std::int32_t displacement{};
    if (!relative_jump(address, reinterpret_cast<std::uintptr_t>(stub), displacement)) return false;
    std::memcpy(replacement.data() + 1, &displacement, sizeof(displacement));
    return matches(address, expected) && write_bytes(address, replacement) &&
        matches(address, replacement);
}

bool install_resource_flag_access_redirects(std::uintptr_t base, std::size_t& applied) noexcept {
    applied = 0;
    if (!install_generated_jump(base + contract::resource_flags_priority_rva,
            resource_flag_stubs + priority_stub_offset,
            std::span{contract::resource_flags_read_priority})) return false;
    ++applied;
    if (!install_generated_jump(base + contract::resource_flags_visibility_rva,
            resource_flag_stubs + visibility_stub_offset,
            std::span{contract::resource_flags_read_visibility})) return false;
    ++applied;
    return true;
}

void restore_resource_flag_access_redirects(std::uintptr_t base, std::size_t count) noexcept {
    if (count >= 2) (void)write_bytes(base + contract::resource_flags_visibility_rva,
        contract::resource_flags_read_visibility);
    if (count >= 1) (void)write_bytes(base + contract::resource_flags_priority_rva,
        contract::resource_flags_read_priority);
}

void free_resource_flag_stubs() noexcept {
    if (resource_flag_stubs) VirtualFree(resource_flag_stubs, 0, MEM_RELEASE);
    resource_flag_stubs = nullptr;
}

void restore_patches(std::uintptr_t base, std::size_t count) noexcept {
    while (count) {
        --count;
        const auto& patch = contract::patches[count];
        (void)write_bytes(base + patch.rva, patch.expected);
    }
}
} // namespace

void report_native_render_resources() noexcept {
    const auto owner = state_owner.load(std::memory_order_acquire);
    std::uintptr_t begin{}, end{};
    if (!owner || !memory::peek(owner + vector_begin_offset, begin) || !memory::peek(owner + vector_end_offset, end) ||
        end < begin || end > begin + record_bytes)
        return;
    const auto used = (end - begin) / native_render_resource_record_size;
    static std::size_t said{};
    static bool warned{}, full{};
    // (A new high, in steps of 1/32: how close a level came is worth having in a log.)
    if (used < said + native_render_resource_capacity / 32 && !(used >= native_render_resource_capacity && !full)) return;
    said = used;
    const auto percent = used * 100 / native_render_resource_capacity;
    if (used >= native_render_resource_capacity) {
        full = true;
        logging::log(logging::Level::error, logging::Channel::graphics,
            "Render-resource pool: all {} records are taken; the level needs more than the game can address.", used);
    } else if (percent >= 90 && !warned) {
        warned = true;
        logging::log(logging::Level::warning, logging::Channel::graphics, "Render-resource pool: {} of {} records taken ({}%).", used,
            native_render_resource_capacity, percent);
    } else {
        logging::log(logging::Level::info, logging::Channel::graphics, "Render-resource pool: {} of {} records taken ({}%).", used,
            native_render_resource_capacity, percent);
    }
}

bool start_native_render_resource_pool(std::uintptr_t base, std::string& error) {
    std::lock_guard lock(install_mutex);
    error.clear();
    if (installed_base) {
        if (installed_base == base) return true;
        error = "Native render-resource pool support is already installed on another image";
        return false;
    }
    if (!validate_contract(base)) {
        error = "Native render-resource pool contract does not match the supported game";
        return false;
    }

    {
        std::lock_guard allocation_lock(allocation_mutex);
        reserved_buffers = allocate_buffers(true);
        for (auto& table : reserved_index_tables) table = allocate_index_table_buffers();
        next_reserved_index_table = 0;
        if (!reserved_buffers || !reserved_index_tables[0] || !reserved_index_tables[1]) {
            free_reserved_allocations();
            error = "Cannot reserve the enlarged native render-resource pools (Windows error " +
                std::to_string(GetLastError()) + ")";
            return false;
        }
    }
    if (!prepare_resource_flag_stubs(base)) {
        std::lock_guard allocation_lock(allocation_mutex);
        free_reserved_allocations();
        error = "Cannot reserve nearby executable redirects for native resource flags";
        return false;
    }

    auto* const target = reinterpret_cast<void*>(base + contract::constructor_rva);
    auto* const index_target = reinterpret_cast<void*>(base + contract::index_table_constructor_rva);
    NativeConstructor original{};
    IndexTableConstructor original_index{};
    const auto prepared = hook_prepare(target, reinterpret_cast<void*>(&construct_native_renderer),
        reinterpret_cast<void**>(&original));
    if (prepared != HookOk || !original) {
        if (prepared == HookOk) (void)hook_remove(target);
        std::lock_guard allocation_lock(allocation_mutex);
        free_reserved_allocations();
        free_resource_flag_stubs();
        error = "Cannot create native render-resource constructor hook (Detours hook service status " +
            std::to_string(prepared) + ")";
        return false;
    }
    const auto index_prepared = hook_prepare(index_target,
        reinterpret_cast<void*>(&construct_index_table), reinterpret_cast<void**>(&original_index));
    if (index_prepared != HookOk || !original_index) {
        if (index_prepared == HookOk) (void)hook_remove(index_target);
        (void)hook_remove(target);
        std::lock_guard allocation_lock(allocation_mutex);
        free_reserved_allocations();
        free_resource_flag_stubs();
        error = "Cannot create native resource-index table hook (Detours hook service status " +
            std::to_string(index_prepared) + ")";
        return false;
    }

    std::size_t applied{};
    for (const auto& patch : contract::patches) {
        if (!write_bytes(base + patch.rva, patch.replacement) ||
            !matches(base + patch.rva, patch.replacement)) {
            restore_patches(base, applied + 1);
            (void)hook_remove(index_target);
            (void)hook_remove(target);
            std::lock_guard allocation_lock(allocation_mutex);
            free_reserved_allocations();
            free_resource_flag_stubs();
            error = "Cannot install native render-resource pool instruction redirects";
            return false;
        }
        ++applied;
    }

    std::size_t generated_applied{};
    if (!install_resource_flag_access_redirects(base, generated_applied)) {
        restore_resource_flag_access_redirects(base, generated_applied);
        restore_patches(base, applied);
        (void)hook_remove(index_target);
        (void)hook_remove(target);
        std::lock_guard allocation_lock(allocation_mutex);
        free_reserved_allocations();
        free_resource_flag_stubs();
        error = "Cannot install native per-resource flag access redirects";
        return false;
    }

    state_owner_return.store(base + contract::state_owner_call_rva + 5, std::memory_order_release);
    original_constructor.store(original, std::memory_order_release);
    original_index_table_constructor.store(original_index, std::memory_order_release);
    const auto queued_index = hook_queue_enable(index_target);
    const auto queued_renderer = queued_index == HookOk ? hook_queue_enable(target) : queued_index;
    const auto enabled = queued_renderer == HookOk ? hook_apply_queued() : queued_renderer;
    if (enabled != HookOk) {
        original_constructor.store(nullptr, std::memory_order_release);
        original_index_table_constructor.store(nullptr, std::memory_order_release);
        restore_resource_flag_access_redirects(base, generated_applied);
        restore_patches(base, applied);
        (void)hook_remove(index_target);
        (void)hook_remove(target);
        std::lock_guard allocation_lock(allocation_mutex);
        free_reserved_allocations();
        free_resource_flag_stubs();
        error = "Cannot enable native render-resource constructor hooks (Detours hook service status " +
            std::to_string(enabled) + ")";
        return false;
    }

    installed_base = base;
    return true;
}
} // namespace dingosdk
