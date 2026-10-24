#include "Engine/Core/Log/logging.h"
#include "gpu_diagnostics.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Debug/crash_report.h"

#include <dxgi1_4.h>
#include <ShlObj.h>
#include <wrl/client.h>
#include <intrin.h>
#include <format>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
struct Log {
    std::atomic<bool> removal_recorded{};
    std::atomic<bool> breadcrumbs{};
};
Log& log() { static auto* value = new Log; return *value; }

// After the GPU stops responding, this many launches record its breadcrumbs: enough for one of them
// to catch the next stop, where it comes every few sessions.
constexpr int breadcrumb_launches = 3;
// %LOCALAPPDATA%\ReSkate\gpu-breadcrumbs.txt: how many launches are still to record them.
std::filesystem::path breadcrumbs_note() {
    PWSTR local{};
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local)))
        path = std::filesystem::path(local) / L"ReSkate" / L"gpu-breadcrumbs.txt";
    CoTaskMemFree(local);
    return path;
}
int breadcrumbs_left() {
    const auto path = breadcrumbs_note();
    int left{};
    if (FILE* file{}; !path.empty() && _wfopen_s(&file, path.c_str(), L"r") == 0 && file) {
        if (fscanf_s(file, "%d", &left) != 1) left = 0;
        std::fclose(file);
    }
    return std::clamp(left, 0, breadcrumb_launches);
}
void set_breadcrumbs_left(int left) {
    const auto path = breadcrumbs_note();
    if (path.empty()) return;
    std::error_code error;
    if (left <= 0) {
        std::filesystem::remove(path, error);
        return;
    }
    std::filesystem::create_directories(path.parent_path(), error);
    if (FILE* file{}; _wfopen_s(&file, path.c_str(), L"w") == 0 && file) {
        std::fprintf(file, "%d\n", left);
        std::fclose(file);
    }
}
}

bool gpu_breadcrumbs() noexcept { return log().breadcrumbs.load(); }

void note_gpu_stopped(DWORD exception_code) noexcept {
    const auto error = GetLastError();
    switch (exception_code) {
    case static_cast<DWORD>(DXGI_ERROR_DEVICE_REMOVED):
    case static_cast<DWORD>(DXGI_ERROR_DEVICE_HUNG):
    case static_cast<DWORD>(DXGI_ERROR_DEVICE_RESET):
    case static_cast<DWORD>(DXGI_ERROR_DRIVER_INTERNAL_ERROR):
        try { set_breadcrumbs_left(breadcrumb_launches); } catch (...) {}
        break;
    default:
        break;
    }
    SetLastError(error);
}

bool graphics_option_enabled(const wchar_t* name) noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(name, value, 2) == 1 && value[0] == L'1';
}


void initialize_graphics_diagnostics() noexcept {
    const auto error = GetLastError();
    try {
        const bool diagnostic = graphics_option_enabled(L"RESKATE_GPU_DIAGNOSTICS");
        dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "Graphics startup: gpu_diagnostics=%d", diagnostic);
        // The launches after one in which the GPU stopped responding record its breadcrumbs, for
        // the report of the next stop. This one counts as one of them.
        const int left = breadcrumbs_left();
        if (left > 0) {
            set_breadcrumbs_left(left - 1);
            dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
                "GPU breadcrumbs are on for this launch: the GPU stopped responding in a recent session (%d more launch(es) after this one).",
                left - 1);
        }
        log().breadcrumbs = diagnostic || left > 0;
        if (log().breadcrumbs) {
            // DRED must be configured before the game creates its D3D12 device.
            // Do not enable the debug layer or change the game's device flags.
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
            const auto result = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
            dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "DRED settings result=0x%08lx", static_cast<unsigned long>(result));
            if (SUCCEEDED(result)) {
                settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
        }
    } catch (...) {}
    SetLastError(error);
}

namespace {
// D3D12_AUTO_BREADCRUMB_OP, by value.
const char* operation_name(D3D12_AUTO_BREADCRUMB_OP op) noexcept {
    static constexpr const char* names[]{"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced",
        "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource",
        "CopyTiles", "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView",
        "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission",
        "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
        "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1",
        "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand",
        "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand",
        "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata", "Barrier",
        "BeginCommandList", "DispatchGraph", "SetProgram"};
    const auto index = static_cast<std::size_t>(op);
    return index < std::size(names) ? names[index] : "?";
}
// D3D12_DRED_ALLOCATION_TYPE, by value.
const char* allocation_type(D3D12_DRED_ALLOCATION_TYPE type) noexcept {
    switch (static_cast<int>(type)) {
    case 19: return "command queue"; case 20: return "command allocator"; case 21: return "pipeline state";
    case 22: return "command list"; case 23: return "fence"; case 24: return "descriptor heap"; case 25: return "heap";
    case 27: return "query heap"; case 28: return "command signature"; case 29: return "pipeline library";
    case 34: return "resource"; case 35: return "pass"; case 42: return "state object"; case 43: return "meta command";
    default: return "other";
    }
}
std::string narrow(const char* a, const wchar_t* w) {
    if (a && *a) return a;
    std::string text;
    for (; w && *w && text.size() < 200; ++w) text.push_back(*w < 128 ? static_cast<char>(*w) : '?');
    return text.empty() ? std::string("(unnamed)") : text;
}
void allocations(const char* what, const D3D12_DRED_ALLOCATION_NODE1* node) {
    unsigned shown{}, total{};
    for (; node; node = node->pNext, ++total)
        if (shown < 24) {
            ++shown;
            dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "  %s: %s \"%s\"", what,
                allocation_type(node->AllocationType), narrow(node->ObjectNameA, node->ObjectNameW).c_str());
        }
    if (total > shown)
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "  (%u more %s)", total - shown, what);
}
}

namespace {
// How much video memory the game uses against what Windows lets it have, once a second: said when
// it moves by a quarter of a gigabyte, and when it passes 90% or all of the budget. Past the
// budget, Windows moves the game's memory out of the card, and a game that does not bring it back
// before use reads memory that is not there. The last figures before a GPU failure say whether
// that is how it came about.
void watch_video_memory(LUID adapter_luid) {
    SetThreadDescription(GetCurrentThread(), L"ReSkate video memory watch");
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(adapter_luid, IID_PPV_ARGS(&adapter)))) return;
    using dingosdk::logging::Level;
    using dingosdk::logging::Channel;
    constexpr double gigabyte = 1024.0 * 1024.0 * 1024.0;
    std::uint64_t said{};
    bool was_near{}, was_over{};
    for (;;) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
            SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)) && local.Budget) {
            const auto moved = local.CurrentUsage > said ? local.CurrentUsage - said : said - local.CurrentUsage;
            const bool now_near = local.CurrentUsage * 10 >= local.Budget * 9, now_over = local.CurrentUsage > local.Budget;
            if (moved >= (256ull << 20) || now_near != was_near || now_over != was_over) {
                said = local.CurrentUsage;
                was_near = now_near;
                was_over = now_over;
                dingosdk::logging::printf(now_over ? Level::warning : Level::info, Channel::graphics,
                    "Video memory: %.2f GB of a %.2f GB budget (%.0f%%)%s; %.2f GB of system memory for the GPU.",
                    local.CurrentUsage / gigabyte, local.Budget / gigabyte, 100.0 * local.CurrentUsage / local.Budget,
                    now_over ? ", over it: Windows is moving the game's memory out of the card" : "", shared.CurrentUsage / gigabyte);
            }
        }
        Sleep(1000);
    }
}
}

namespace {
using DrawInstanced = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexedInstanced = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
std::atomic<DrawInstanced> original_draw{};
std::atomic<DrawIndexedInstanced> original_draw_indexed{};
constexpr std::uint64_t large_draw = 10'000'000;

std::string code_place(void* address) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module) || !module)
        return std::format("{:#x}", reinterpret_cast<std::uintptr_t>(address));
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c) if (*c == L'\\') name = c + 1;
    std::string text;
    for (; *name; ++name) text.push_back(*name < 128 ? static_cast<char>(*name) : '?');
    return std::format("{}+{:#x}", text, reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module));
}
// Said the first time a caller makes a large draw, and again whenever it doubles its largest.
__declspec(noinline) void note_large_draw(const char* kind, bool indexed, UINT count, UINT instances, void* caller) noexcept {
    static std::mutex mutex;
    static std::array<std::pair<void*, std::uint64_t>, 64> seen{};
    const auto size = std::uint64_t{count} * instances;
    try {
        std::lock_guard lock(mutex);
        auto* slot = &seen.back();
        for (auto& entry : seen)
            if (entry.first == caller || !entry.first) { slot = &entry; break; }
        if (slot->first == caller && size < slot->second * 2) return;
        *slot = {caller, size};
        std::array<void*, 8> frames{};
        const auto depth = CaptureStackBackTrace(1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
        std::string stack;
        for (USHORT i = 0; i < depth; ++i) stack += " < " + code_place(frames[i]);
        dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
            "Large draw: {} of {} {} x {} instance(s) = {} from{}", kind, count,
            indexed ? "indices" : "vertices", instances, size, stack);
    } catch (...) {}
}
void STDMETHODCALLTYPE hooked_draw(ID3D12GraphicsCommandList* list, UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) {
    if (std::uint64_t{vertices} * instances >= large_draw) note_large_draw("DrawInstanced", false, vertices, instances, _ReturnAddress());
    original_draw.load(std::memory_order_relaxed)(list, vertices, instances, first_vertex, first_instance);
}
void STDMETHODCALLTYPE hooked_draw_indexed(ID3D12GraphicsCommandList* list, UINT indices, UINT instances, UINT first_index, INT base_vertex,
    UINT first_instance) {
    if (std::uint64_t{indices} * instances >= large_draw) note_large_draw("DrawIndexedInstanced", true, indices, instances, _ReturnAddress());
    original_draw_indexed.load(std::memory_order_relaxed)(list, indices, instances, first_index, base_vertex, first_instance);
}
}

namespace {
// Where the game's GPU memory came from and where it went: every heap and resource the game's
// device makes, with the code that made it and, once its last reference goes, the code that let
// it go. A page fault at memory freed shortly before is then said with both, which names the
// engine system that freed memory the GPU was still using.
struct Frames {
    std::array<void*, 14> at{};
    USHORT depth{};
};
struct Allocation {
    bool heap{};
    void* object{};
    D3D12_RESOURCE_DIMENSION dimension{};
    std::uint64_t bytes{}, gpu{};  // (gpu: the first GPU address, 0 while it is not known)
    void* in_heap{};
    std::uint64_t offset{};
    bool ranged{};  // (in Memory::ranges, under gpu)
    Frames created, freed;
    ULONGLONG freed_at{};
};
constexpr std::size_t freed_kept = 32768;
struct Memory {
    std::mutex mutex;
    std::unordered_map<void*, Allocation> live;
    std::vector<Allocation> freed = std::vector<Allocation>(freed_kept);
    std::uint64_t freed_count{}, made_count{};
    ID3D12Device* device{};
    // The GPU address ranges that exist: committed and reserved buffers, and heaps once a buffer
    // placed in one has given its address. Read on every checked bind; a removal bumps the
    // generation, which drops each thread's cached range.
    std::shared_mutex ranges_mutex;
    std::map<std::uint64_t, std::pair<std::uint64_t, void*>> ranges;
    std::atomic<std::uint64_t> generation{};
};
Memory& gpu_memory() { static auto* value = new Memory; return *value; }

// (Callers hold Memory::mutex: it is taken before ranges_mutex.)
void add_range(Allocation& a) {
    if (!a.gpu || !a.bytes || a.ranged) return;
    auto& m = gpu_memory();
    std::unique_lock lock(m.ranges_mutex);
    m.ranges[a.gpu] = {a.bytes, a.object};
    a.ranged = true;
}
void remove_range(const Allocation& a) {
    if (!a.ranged) return;
    auto& m = gpu_memory();
    std::unique_lock lock(m.ranges_mutex);
    if (const auto found = m.ranges.find(a.gpu); found != m.ranges.end() && found->second.second == a.object) m.ranges.erase(found);
    m.generation.fetch_add(1, std::memory_order_relaxed);
}

Frames capture_frames() noexcept {
    Frames frames;
    frames.depth = CaptureStackBackTrace(2, static_cast<DWORD>(frames.at.size()), frames.at.data(), nullptr);
    return frames;
}

// Release, as the runtime's objects share it: a few distinct functions at most (heaps and
// resources), each hooked once, each with its own forwarder.
using Release = ULONG (STDMETHODCALLTYPE*)(IUnknown*);
constexpr std::size_t release_slots = 4;
std::array<std::atomic<void*>, release_slots> release_targets{};
std::array<std::atomic<Release>, release_slots> release_originals{};
void note_release(IUnknown* object) noexcept {
    try {
        auto& m = gpu_memory();
        Frames frames = capture_frames();
        std::lock_guard lock(m.mutex);
        const auto found = m.live.find(object);
        if (found == m.live.end()) return;
        remove_range(found->second);
        auto& slot = m.freed[m.freed_count++ % freed_kept];
        slot = found->second;
        slot.freed = frames;
        slot.freed_at = GetTickCount64();
        m.live.erase(found);
    } catch (...) {}
}
template<std::size_t N> ULONG STDMETHODCALLTYPE hooked_release(IUnknown* object) {
    const auto count = release_originals[N].load(std::memory_order_relaxed)(object);
    if (!count) note_release(object);
    return count;
}
constexpr Release release_forwarders[release_slots]{&hooked_release<0>, &hooked_release<1>, &hooked_release<2>, &hooked_release<3>};
void watch_release(IUnknown* object) noexcept {
    static std::mutex mutex;
    auto* const target = (*reinterpret_cast<void***>(object))[2];
    std::lock_guard lock(mutex);
    for (std::size_t i = 0; i < release_slots; ++i) {
        if (release_targets[i].load() == target) return;
        if (release_targets[i].load()) continue;
        void* original{};
        if (hook_prepare(target, reinterpret_cast<void*>(release_forwarders[i]), &original) != HookOk) return;
        release_originals[i].store(reinterpret_cast<Release>(original));
        release_targets[i].store(target);
        hook_enable(target);
        return;
    }
}

void note_heap(void** out, const D3D12_HEAP_DESC* desc) noexcept {
    if (!out || !*out || !desc) return;
    try {
        auto* const object = static_cast<IUnknown*>(*out);
        Allocation made{true, object, D3D12_RESOURCE_DIMENSION_UNKNOWN, desc->SizeInBytes};
        made.created = capture_frames();
        watch_release(object);
        auto& m = gpu_memory();
        std::lock_guard lock(m.mutex);
        m.live[object] = made;
        ++m.made_count;
    } catch (...) {}
}
void note_resource(void** out, void* heap, std::uint64_t offset) noexcept {
    if (!out || !*out) return;
    try {
        auto* const object = static_cast<IUnknown*>(*out);
        ComPtr<ID3D12Resource> resource;
        if (FAILED(object->QueryInterface(IID_PPV_ARGS(&resource)))) return;
        const auto desc = resource->GetDesc();
        Allocation made{false, object, desc.Dimension};
        made.in_heap = heap;
        made.offset = offset;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            made.bytes = desc.Width;
            made.gpu = resource->GetGPUVirtualAddress();
        } else if (auto* const device = gpu_memory().device) {
            // (UINT64_MAX: the runtime could not size it.)
            if (const auto size = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes; size != UINT64_MAX) made.bytes = size;
        }
        made.created = capture_frames();
        watch_release(object);
        auto& m = gpu_memory();
        std::lock_guard lock(m.mutex);
        // (A buffer placed in a heap gives the heap's GPU address, and the heap's gives a texture's.)
        if (const auto found = heap ? m.live.find(heap) : m.live.end(); found != m.live.end()) {
            if (made.gpu && !found->second.gpu) {
                found->second.gpu = made.gpu - offset;
                add_range(found->second);
            } else if (!made.gpu && found->second.gpu) {
                made.gpu = found->second.gpu + offset;
            }
        }
        if (!heap) add_range(made);
        m.live[object] = made;
        ++m.made_count;
    } catch (...) {}
}

using CreateCommitted = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateHeap = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
using CreatePlaced = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateCommitted1 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using CreateHeap1 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*, REFIID, void**);
using CreateCommitted2 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using CreatePlaced1 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateCommitted3 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32,
    const DXGI_FORMAT*, REFIID, void**);
using CreatePlaced2 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
    D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID, void**);
std::atomic<CreateCommitted> original_committed{};
std::atomic<CreateHeap> original_heap{};
std::atomic<CreatePlaced> original_placed{};
std::atomic<CreateCommitted1> original_committed1{};
std::atomic<CreateHeap1> original_heap1{};
std::atomic<CreateCommitted2> original_committed2{};
std::atomic<CreatePlaced1> original_placed1{};
std::atomic<CreateCommitted3> original_committed3{};
std::atomic<CreatePlaced2> original_placed2{};

// A resource creation that holds up its thread (the driver waiting on the GPU inside it): said with
// what was made and which game code asked, at most 40 times. A 170 ms stall every 5 s was one of
// these (2026-10-17): the game's batched GPU uploads (Skate 0x1446b3f20) making their upload buffer.
std::uint64_t creation_clock() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
template<class Desc>
void note_slow_creation(std::uint64_t started, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS heap_flags, const Desc* desc,
                        void* caller) noexcept {
    const auto ms = static_cast<double>(creation_clock() - started) / 1e6;
    if (ms < 20) return;
    static std::atomic<unsigned> said{};
    if (said.fetch_add(1, std::memory_order_relaxed) >= 40) return;
    try {
        const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto at = reinterpret_cast<std::uintptr_t>(caller);
        const auto where = at > image && at - image < 0x10000000 ? std::format("Skate 0x{:x}", 0x140000000 + (at - image))
                                                                   : std::format("0x{:x}", at);
        static constexpr const char* dimensions[]{"unknown", "buffer", "1D texture", "2D texture", "3D texture"};
        const auto dimension = desc && static_cast<unsigned>(desc->Dimension) < 5 ? dimensions[desc->Dimension] : "unknown";
        dingosdk::logging::log(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
            "CreateCommittedResource took {:.1f} ms: heap type {}, heap flags {:#x}; a {} {} x {} x {}, {} mips, format {}, "
            "flags {:#x}; asked for by {}.", ms, properties ? static_cast<int>(properties->Type) : -1, static_cast<unsigned>(heap_flags),
            dimension, desc ? desc->Width : 0, desc ? desc->Height : 0, desc ? desc->DepthOrArraySize : 0, desc ? desc->MipLevels : 0,
            desc ? static_cast<int>(desc->Format) : 0, desc ? static_cast<unsigned>(desc->Flags) : 0u, where);
    } catch (...) {}
}

HRESULT STDMETHODCALLTYPE hooked_committed(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const auto started = creation_clock();
    const auto result = original_committed.load()(device, properties, flags, desc, state, clear, riid, out);
    note_slow_creation(started, properties, flags, desc, _ReturnAddress());
    if (SUCCEEDED(result)) note_resource(out, nullptr, 0);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_heap(ID3D12Device* device, const D3D12_HEAP_DESC* desc, REFIID riid, void** out) {
    const auto result = original_heap.load()(device, desc, riid, out);
    if (SUCCEEDED(result)) note_heap(out, desc);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_placed(ID3D12Device* device, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const auto result = original_placed.load()(device, heap, offset, desc, state, clear, riid, out);
    if (SUCCEEDED(result)) note_resource(out, heap, offset);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_committed1(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
    REFIID riid, void** out) {
    const auto started = creation_clock();
    const auto result = original_committed1.load()(device, properties, flags, desc, state, clear, session, riid, out);
    note_slow_creation(started, properties, flags, desc, _ReturnAddress());
    if (SUCCEEDED(result)) note_resource(out, nullptr, 0);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_heap1(ID3D12Device* device, const D3D12_HEAP_DESC* desc, ID3D12ProtectedResourceSession* session,
    REFIID riid, void** out) {
    const auto result = original_heap1.load()(device, desc, session, riid, out);
    if (SUCCEEDED(result)) note_heap(out, desc);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_committed2(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
    REFIID riid, void** out) {
    const auto started = creation_clock();
    const auto result = original_committed2.load()(device, properties, flags, desc, state, clear, session, riid, out);
    note_slow_creation(started, properties, flags, desc, _ReturnAddress());
    if (SUCCEEDED(result)) note_resource(out, nullptr, 0);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_placed1(ID3D12Device* device, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const auto result = original_placed1.load()(device, heap, offset, desc, state, clear, riid, out);
    if (SUCCEEDED(result)) note_resource(out, heap, offset);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_committed3(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
    UINT32 formats, const DXGI_FORMAT* castable, REFIID riid, void** out) {
    const auto started = creation_clock();
    const auto result = original_committed3.load()(device, properties, flags, desc, layout, clear, session, formats, castable, riid, out);
    note_slow_creation(started, properties, flags, desc, _ReturnAddress());
    if (SUCCEEDED(result)) note_resource(out, nullptr, 0);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_placed2(ID3D12Device* device, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
    D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear, UINT32 formats, const DXGI_FORMAT* castable, REFIID riid, void** out) {
    const auto result = original_placed2.load()(device, heap, offset, desc, layout, clear, formats, castable, riid, out);
    if (SUCCEEDED(result)) note_resource(out, heap, offset);
    return result;
}
using CreateReserved = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateReserved1 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using CreateReserved2 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_BARRIER_LAYOUT,
    const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
std::atomic<CreateReserved> original_reserved{};
std::atomic<CreateReserved1> original_reserved1{};
std::atomic<CreateReserved2> original_reserved2{};
HRESULT STDMETHODCALLTYPE hooked_reserved(ID3D12Device* device, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const auto result = original_reserved.load()(device, desc, state, clear, riid, out);
    if (SUCCEEDED(result)) note_resource(out, nullptr, 0);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_reserved1(ID3D12Device* device, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, REFIID riid, void** out) {
    const auto result = original_reserved1.load()(device, desc, state, clear, session, riid, out);
    if (SUCCEEDED(result)) note_resource(out, nullptr, 0);
    return result;
}
HRESULT STDMETHODCALLTYPE hooked_reserved2(ID3D12Device* device, const D3D12_RESOURCE_DESC* desc, D3D12_BARRIER_LAYOUT layout,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, UINT32 formats, const DXGI_FORMAT* castable, REFIID riid,
    void** out) {
    const auto result = original_reserved2.load()(device, desc, layout, clear, session, formats, castable, riid, out);
    if (SUCCEEDED(result)) note_resource(out, nullptr, 0);
    return result;
}
bool watch_device_views(ID3D12Device* device) noexcept;

// Slots of ID3D12Device and its later versions (d3d12.h): CreateCommittedResource 27, CreateHeap
// 28, CreatePlacedResource 29, CreateReservedResource 30; Device4 CreateCommittedResource1 53,
// CreateHeap1 54, CreateReservedResource1 55; Device8 CreateCommittedResource2 69,
// CreatePlacedResource1 70; Device10 CreateCommittedResource3 76, CreatePlacedResource2 77,
// CreateReservedResource2 78.
template<class F> bool hook_slot(IUnknown* object, std::size_t slot, void* replacement, std::atomic<F>& original) {
    auto* const target = (*reinterpret_cast<void***>(object))[slot];
    void* trampoline{};
    if (hook_prepare(target, replacement, &trampoline) != HookOk) return false;
    original.store(reinterpret_cast<F>(trampoline));
    return hook_enable(target) == HookOk;
}
void watch_device_memory(IUnknown* made) noexcept {
    static std::atomic<bool> watching{};
    ComPtr<ID3D12Device> device;
    if (!made || FAILED(made->QueryInterface(IID_PPV_ARGS(&device))) || watching.exchange(true)) return;
    gpu_memory().device = device.Get();  // (the game keeps its device for the life of the process)
    bool on = hook_slot(device.Get(), 27, reinterpret_cast<void*>(&hooked_committed), original_committed);
    on &= hook_slot(device.Get(), 28, reinterpret_cast<void*>(&hooked_heap), original_heap);
    on &= hook_slot(device.Get(), 29, reinterpret_cast<void*>(&hooked_placed), original_placed);
    on &= hook_slot(device.Get(), 30, reinterpret_cast<void*>(&hooked_reserved), original_reserved);
    if (ComPtr<ID3D12Device4> device4; SUCCEEDED(device.As(&device4))) {
        on &= hook_slot(device4.Get(), 53, reinterpret_cast<void*>(&hooked_committed1), original_committed1);
        on &= hook_slot(device4.Get(), 54, reinterpret_cast<void*>(&hooked_heap1), original_heap1);
        on &= hook_slot(device4.Get(), 55, reinterpret_cast<void*>(&hooked_reserved1), original_reserved1);
    }
    if (ComPtr<ID3D12Device8> device8; SUCCEEDED(device.As(&device8))) {
        on &= hook_slot(device8.Get(), 69, reinterpret_cast<void*>(&hooked_committed2), original_committed2);
        on &= hook_slot(device8.Get(), 70, reinterpret_cast<void*>(&hooked_placed1), original_placed1);
    }
    if (ComPtr<ID3D12Device10> device10; SUCCEEDED(device.As(&device10))) {
        on &= hook_slot(device10.Get(), 76, reinterpret_cast<void*>(&hooked_committed3), original_committed3);
        on &= hook_slot(device10.Get(), 77, reinterpret_cast<void*>(&hooked_placed2), original_placed2);
        on &= hook_slot(device10.Get(), 78, reinterpret_cast<void*>(&hooked_reserved2), original_reserved2);
    }
    on &= watch_device_views(device.Get());
    dingosdk::logging::write(on ? dingosdk::logging::Level::info : dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
        on ? "GPU diagnostics: the code that makes and frees each GPU heap and resource is kept, for a page fault's report."
           : "GPU diagnostics: some of the GPU memory watch could not start.");
}

using CreateDevice = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
std::atomic<CreateDevice> original_create_device{};
HRESULT WINAPI hooked_create_device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** device) {
    const auto result = original_create_device.load()(adapter, level, riid, device);
    if (SUCCEEDED(result) && device && *device) watch_device_memory(static_cast<IUnknown*>(*device));
    return result;
}

std::string frames_text(const Frames& frames) {
    std::string text;
    for (USHORT i = 0; i < frames.depth; ++i) text += " < " + code_place(frames.at[i]);
    return text.empty() ? std::string(" (unknown)") : text;
}
const char* allocation_kind(const Allocation& a) {
    if (a.heap) return "heap";
    switch (a.dimension) {
    case D3D12_RESOURCE_DIMENSION_BUFFER: return "buffer";
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D: return "1D texture";
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D: return "2D texture";
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D: return "3D texture";
    default: return "resource";
    }
}
void say_allocation(const char* what, const Allocation& a, ULONGLONG now) {
    using dingosdk::logging::Level;
    using dingosdk::logging::Channel;
    dingosdk::logging::log(Level::error, Channel::graphics, "  {}: {} of {} bytes at {:#x}{}{}", what, allocation_kind(a), a.bytes, a.gpu,
        a.in_heap ? std::format(" (offset {:#x} in a heap)", a.offset) : std::string(),
        a.freed_at ? std::format(", freed {} ms before the report", now - a.freed_at) : std::string());
    dingosdk::logging::log(Level::error, Channel::graphics, "    made by{}", frames_text(a.created));
    if (a.freed_at) dingosdk::logging::log(Level::error, Channel::graphics, "    freed by{}", frames_text(a.freed));
}

// Whether a GPU address lies in memory that exists now (0, "none", always does). Each thread keeps
// the last range it found until a range goes away.
thread_local std::uint64_t cached_start{}, cached_end{}, cached_generation{~0ull};
bool known_address(std::uint64_t address) noexcept {
    if (!address) return true;
    auto& m = gpu_memory();
    if (m.generation.load(std::memory_order_relaxed) == cached_generation && address >= cached_start && address < cached_end) return true;
    std::shared_lock lock(m.ranges_mutex);
    auto found = m.ranges.upper_bound(address);
    if (found == m.ranges.begin()) return false;
    --found;
    if (address - found->first >= found->second.first) return false;
    cached_start = found->first;
    cached_end = found->first + found->second.first;
    cached_generation = m.generation.load(std::memory_order_relaxed);
    return true;
}
// A GPU address handed to D3D12 that lies in no memory there is now: said once for each piece of
// code that does it (up to 64 of them), with what was there before, if anything was.
__declspec(noinline) void note_bad_address(const char* api, std::uint64_t address, void* caller) noexcept {
    static std::mutex mutex;
    static std::array<void*, 64> seen{};
    static std::atomic<std::uint64_t> total{};
    try {
        const auto count = ++total;
        {
            std::lock_guard lock(mutex);
            auto* slot = static_cast<void**>(nullptr);
            for (auto& entry : seen)
                if (entry == caller) return;
                else if (!entry) { slot = &entry; break; }
            if (!slot) return;
            *slot = caller;
        }
        using dingosdk::logging::Level;
        using dingosdk::logging::Channel;
        std::array<void*, 14> frames{};
        const auto depth = CaptureStackBackTrace(1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
        std::string stack;
        for (USHORT i = 0; i < depth; ++i) stack += " < " + code_place(frames[i]);
        dingosdk::logging::log(Level::error, Channel::graphics, "Bad GPU address: {} given {:#x}, which is in no memory that exists (bad address {} so far); from{}",
            api, address, count, stack);
        auto& m = gpu_memory();
        std::lock_guard lock(m.mutex);
        const auto now = GetTickCount64();
        const auto kept = std::min<std::uint64_t>(m.freed_count, freed_kept);
        for (std::uint64_t i = 0; i < kept; ++i) {
            const auto& a = m.freed[(m.freed_count - 1 - i) % freed_kept];
            if (a.gpu && a.bytes && address >= a.gpu && address - a.gpu < a.bytes) {
                say_allocation("it was in", a, now);
                return;
            }
        }
        // (Nearest memory below it, to tell a stale address from a wild one.)
        std::shared_lock ranges(m.ranges_mutex);
        if (auto below = m.ranges.upper_bound(address); below != m.ranges.begin()) {
            --below;
            dingosdk::logging::log(Level::error, Channel::graphics, "  Nothing was freed there; the nearest memory below ends {:#x} bytes before it.",
                address - (below->first + below->second.first));
        } else {
            dingosdk::logging::write(Level::error, Channel::graphics, "  Nothing was freed there, and there is no memory below it.");
        }
    } catch (...) {}
}
inline void check_address(const char* api, std::uint64_t address, void* caller) noexcept {
    if (!known_address(address)) note_bad_address(api, address, caller);
}

// Command-list slots (d3d12.h): SetCompute/GraphicsRootConstantBufferView 37/38,
// ...ShaderResourceView 39/40, ...UnorderedAccessView 41/42, IASetIndexBuffer 43,
// IASetVertexBuffers 44; ID3D12GraphicsCommandList4 BuildRaytracingAccelerationStructure 72.
using RootView = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using SetIndexBuffer = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
using SetVertexBuffers = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
using BuildAccelerationStructure = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, UINT, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*);
constexpr const char* root_view_names[6]{"SetComputeRootConstantBufferView", "SetGraphicsRootConstantBufferView",
    "SetComputeRootShaderResourceView", "SetGraphicsRootShaderResourceView", "SetComputeRootUnorderedAccessView",
    "SetGraphicsRootUnorderedAccessView"};
std::array<std::atomic<RootView>, 6> original_root_views{};
std::atomic<SetIndexBuffer> original_index_buffer{};
std::atomic<SetVertexBuffers> original_vertex_buffers{};
std::atomic<BuildAccelerationStructure> original_build{};
template<std::size_t N> void STDMETHODCALLTYPE hooked_root_view(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    check_address(root_view_names[N], address, _ReturnAddress());
    original_root_views[N].load(std::memory_order_relaxed)(list, index, address);
}
constexpr RootView root_view_forwarders[6]{&hooked_root_view<0>, &hooked_root_view<1>, &hooked_root_view<2>, &hooked_root_view<3>,
    &hooked_root_view<4>, &hooked_root_view<5>};
void STDMETHODCALLTYPE hooked_index_buffer(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW* view) {
    if (view) check_address("IASetIndexBuffer", view->BufferLocation, _ReturnAddress());
    original_index_buffer.load(std::memory_order_relaxed)(list, view);
}
void STDMETHODCALLTYPE hooked_vertex_buffers(ID3D12GraphicsCommandList* list, UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
    for (UINT i = 0; views && i < count; ++i) check_address("IASetVertexBuffers", views[i].BufferLocation, _ReturnAddress());
    original_vertex_buffers.load(std::memory_order_relaxed)(list, start, count, views);
}
void STDMETHODCALLTYPE hooked_build(ID3D12GraphicsCommandList4* list, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc,
    UINT postbuild_count, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* postbuild) {
    if (desc) {
        void* const caller = _ReturnAddress();
        check_address("BuildRaytracingAccelerationStructure (destination)", desc->DestAccelerationStructureData, caller);
        check_address("BuildRaytracingAccelerationStructure (scratch)", desc->ScratchAccelerationStructureData, caller);
        check_address("BuildRaytracingAccelerationStructure (source)", desc->SourceAccelerationStructureData, caller);
        const auto& inputs = desc->Inputs;
        if (inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
            if (inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY)
                check_address("BuildRaytracingAccelerationStructure (instances)", inputs.InstanceDescs, caller);
        } else {
            for (UINT i = 0; i < inputs.NumDescs; ++i) {
                const auto* geometry = inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? inputs.pGeometryDescs + i
                    : inputs.ppGeometryDescs ? inputs.ppGeometryDescs[i] : nullptr;
                if (!geometry) continue;
                if (geometry->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
                    check_address("BuildRaytracingAccelerationStructure (vertices)", geometry->Triangles.VertexBuffer.StartAddress, caller);
                    check_address("BuildRaytracingAccelerationStructure (indices)", geometry->Triangles.IndexBuffer, caller);
                    check_address("BuildRaytracingAccelerationStructure (transform)", geometry->Triangles.Transform3x4, caller);
                } else if (geometry->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
                    check_address("BuildRaytracingAccelerationStructure (boxes)", geometry->AABBs.AABBs.StartAddress, caller);
                }
            }
        }
    }
    original_build.load(std::memory_order_relaxed)(list, desc, postbuild_count, postbuild);
}

// How far into each descriptor heap the game writes: every view it makes and every descriptor it
// copies lands at an index of some heap. A heap's furthest index is said as it passes each power
// of two from 4,096 (with the code that took it there from 32,768 on), as it passes 75% and 90% of
// the heap and its end, every 5 seconds while a large heap's grows, and with a GPU failure's
// report.
struct DescriptorHeap {
    std::uint64_t start{}, count{}, increment{};
    D3D12_DESCRIPTOR_HEAP_TYPE type{};
    bool visible{};
    unsigned number{};
    std::atomic<std::uint64_t> furthest{};
    std::atomic<unsigned> said_bits{}, said_share{};  // (said_share: 1 at 75%, 2 at 90%, 3 past the end)
    std::uint64_t summarised{};
};
struct Descriptors {
    std::shared_mutex mutex;
    std::map<std::uint64_t, DescriptorHeap*> heaps;  // (by first CPU handle; kept for the process)
    unsigned made{};
};
Descriptors& descriptors() { static auto* value = new Descriptors; return *value; }
const char* descriptor_type(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    switch (type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return "CBV/SRV/UAV";
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return "sampler";
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return "render target";
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return "depth stencil";
    default: return "?";
    }
}
__declspec(noinline) void note_descriptor_share(DescriptorHeap& heap, std::uint64_t index, unsigned share) noexcept {
    try {
        std::array<void*, 14> frames{};
        const auto depth = CaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
        std::string stack;
        for (USHORT i = 0; i < depth; ++i) stack += " < " + code_place(frames[i]);
        dingosdk::logging::log(share == 3 ? dingosdk::logging::Level::error : dingosdk::logging::Level::warning,
            dingosdk::logging::Channel::graphics, "Descriptor heap {} ({}, {}, {} descriptors): written up to index {}, {}; from{}", heap.number,
            descriptor_type(heap.type), heap.visible ? "shader-visible" : "CPU only", heap.count, index,
            share == 3 ? "PAST ITS END" : share == 2 ? "past 90% of it" : "past 75% of it", stack);
    } catch (...) {}
}
__declspec(noinline) void note_descriptor_reach(DescriptorHeap& heap, std::uint64_t index, unsigned bits) noexcept {
    try {
        std::string stack;
        if (index >= 32768) {
            std::array<void*, 14> frames{};
            const auto depth = CaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
            stack = "; from";
            for (USHORT i = 0; i < depth; ++i) stack += " < " + code_place(frames[i]);
        }
        dingosdk::logging::log(index >= 65536 ? dingosdk::logging::Level::warning : dingosdk::logging::Level::info,
            dingosdk::logging::Channel::graphics, "Descriptor heap {} ({}, {}, {} descriptors): written up to index {} (past {}){}", heap.number,
            descriptor_type(heap.type), heap.visible ? "shader-visible" : "CPU only", heap.count, index, 1ull << (bits - 1), stack);
    } catch (...) {}
}
void note_descriptors(std::uint64_t handle, std::uint64_t count) noexcept {
    if (!handle || !count) return;
    auto& d = descriptors();
    DescriptorHeap* heap{};
    {
        std::shared_lock lock(d.mutex);
        auto found = d.heaps.upper_bound(handle);
        if (found == d.heaps.begin()) return;
        heap = (--found)->second;
    }
    if (!heap->increment || handle - heap->start >= heap->count * heap->increment) return;
    const auto index = (handle - heap->start) / heap->increment + count - 1;
    auto furthest = heap->furthest.load(std::memory_order_relaxed);
    while (index > furthest && !heap->furthest.compare_exchange_weak(furthest, index, std::memory_order_relaxed)) {}
    if (index <= furthest) return;
    if (const unsigned share = index >= heap->count ? 3 : index * 10 >= heap->count * 9 ? 2 : index * 4 >= heap->count * 3 ? 1 : 0; share) {
        auto said = heap->said_share.load(std::memory_order_relaxed);
        while (share > said)
            if (heap->said_share.compare_exchange_weak(said, share, std::memory_order_relaxed)) {
                note_descriptor_share(*heap, index, share);
                break;
            }
    }
    if (index < 4096) return;
    unsigned bits{};
    for (auto value = index; value; value >>= 1) ++bits;
    auto said = heap->said_bits.load(std::memory_order_relaxed);
    while (bits > said)
        if (heap->said_bits.compare_exchange_weak(said, bits, std::memory_order_relaxed)) {
            note_descriptor_reach(*heap, index, bits);
            return;
        }
}
void say_descriptor_heaps() noexcept {
    try {
        auto& d = descriptors();
        std::shared_lock lock(d.mutex);
        for (const auto& [start, heap] : d.heaps)
            if (heap->furthest.load() >= 1024 || heap->count >= 65536)
                dingosdk::logging::log(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics,
                    "Descriptor heap {} ({}, {}, {} descriptors): written up to index {}.", heap->number, descriptor_type(heap->type),
                    heap->visible ? "shader-visible" : "CPU only", heap->count, heap->furthest.load());
    } catch (...) {}
}

// Device slots: CreateDescriptorHeap 14, CreateConstantBufferView 17, CreateShaderResourceView 18,
// CreateUnorderedAccessView 19, CopyDescriptors 23, CopyDescriptorsSimple 24.
using CreateDescriptorHeap = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using CreateCbv = void (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateSrv = void (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateUav = void (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescriptors = void (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
    const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescriptorsSimple = void (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_DESCRIPTOR_HEAP_TYPE);
std::atomic<CreateDescriptorHeap> original_descriptor_heap{};
std::atomic<CreateCbv> original_cbv{};
std::atomic<CreateSrv> original_srv{};
std::atomic<CreateUav> original_uav{};
std::atomic<CopyDescriptors> original_copy{};
std::atomic<CopyDescriptorsSimple> original_copy_simple{};
HRESULT STDMETHODCALLTYPE hooked_descriptor_heap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) {
    const auto result = original_descriptor_heap.load()(device, desc, riid, out);
    if (SUCCEEDED(result) && desc && out && *out) {
        try {
            ComPtr<ID3D12DescriptorHeap> made;
            if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&made)))) {
                auto* heap = new DescriptorHeap;
                heap->start = made->GetCPUDescriptorHandleForHeapStart().ptr;
                heap->count = desc->NumDescriptors;
                heap->increment = device->GetDescriptorHandleIncrementSize(desc->Type);
                heap->type = desc->Type;
                heap->visible = desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                auto& d = descriptors();
                {
                    std::unique_lock lock(d.mutex);
                    heap->number = ++d.made;
                    d.heaps[heap->start] = heap;
                }
                if (heap->count >= 4096 || heap->visible || heap->number <= 16)
                    dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
                        "Descriptor heap {} made: {}, {}, {} descriptors.", heap->number, descriptor_type(heap->type),
                        heap->visible ? "shader-visible" : "CPU only", heap->count);
            }
        } catch (...) {}
    }
    return result;
}
void STDMETHODCALLTYPE hooked_cbv(ID3D12Device* device, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (desc) check_address("CreateConstantBufferView", desc->BufferLocation, _ReturnAddress());
    note_descriptors(handle.ptr, 1);
    original_cbv.load(std::memory_order_relaxed)(device, desc, handle);
}
void STDMETHODCALLTYPE hooked_srv(ID3D12Device* device, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
        check_address("CreateShaderResourceView (acceleration structure)", desc->RaytracingAccelerationStructure.Location, _ReturnAddress());
    note_descriptors(handle.ptr, 1);
    original_srv.load(std::memory_order_relaxed)(device, resource, desc, handle);
}
void STDMETHODCALLTYPE hooked_uav(ID3D12Device* device, ID3D12Resource* resource, ID3D12Resource* counter,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    note_descriptors(handle.ptr, 1);
    original_uav.load(std::memory_order_relaxed)(device, resource, counter, desc, handle);
}
void STDMETHODCALLTYPE hooked_copy(ID3D12Device* device, UINT ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* starts, const UINT* sizes,
    UINT source_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* source_starts, const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    for (UINT i = 0; starts && i < ranges; ++i) note_descriptors(starts[i].ptr, sizes ? sizes[i] : 1);
    original_copy.load(std::memory_order_relaxed)(device, ranges, starts, sizes, source_ranges, source_starts, source_sizes, type);
}
void STDMETHODCALLTYPE hooked_copy_simple(ID3D12Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
    D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    note_descriptors(destination.ptr, count);
    original_copy_simple.load(std::memory_order_relaxed)(device, count, destination, source, type);
}
// Every 5 seconds: the large heaps whose furthest index has grown since they were last said, so a
// hang that ends the process before its report still leaves how full they were.
void summarise_descriptor_heaps() {
    SetThreadDescription(GetCurrentThread(), L"ReSkate descriptor heap watch");
    for (;;) {
        Sleep(5000);
        try {
            auto& d = descriptors();
            std::shared_lock lock(d.mutex);
            for (const auto& [start, heap] : d.heaps) {
                const auto furthest = heap->furthest.load(std::memory_order_relaxed);
                if (heap->count < 65536 || furthest == heap->summarised) continue;
                heap->summarised = furthest;
                dingosdk::logging::log(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
                    "Descriptor heap {} ({}, {}): {} of {} used at the furthest ({}%).", heap->number, descriptor_type(heap->type),
                    heap->visible ? "shader-visible" : "CPU only", furthest + 1, heap->count, (furthest + 1) * 100 / heap->count);
            }
        } catch (...) {}
    }
}
bool watch_device_views(ID3D12Device* device) noexcept {
    try {
        std::thread(summarise_descriptor_heaps).detach();
    } catch (...) {}
    return hook_slot(device, 14, reinterpret_cast<void*>(&hooked_descriptor_heap), original_descriptor_heap) &
           hook_slot(device, 17, reinterpret_cast<void*>(&hooked_cbv), original_cbv) &
           hook_slot(device, 18, reinterpret_cast<void*>(&hooked_srv), original_srv) &
           hook_slot(device, 19, reinterpret_cast<void*>(&hooked_uav), original_uav) &
           hook_slot(device, 23, reinterpret_cast<void*>(&hooked_copy), original_copy) &
           hook_slot(device, 24, reinterpret_cast<void*>(&hooked_copy_simple), original_copy_simple);
}
}

void watch_addresses(ID3D12GraphicsCommandList* list) noexcept {
    static std::atomic<bool> watching{};
    const auto error = GetLastError();
    if (!list || !graphics_option_enabled(L"RESKATE_GPU_DIAGNOSTICS") || !gpu_memory().made_count || watching.exchange(true)) {
        SetLastError(error);
        return;
    }
    bool on = true;
    for (std::size_t i = 0; i < 6; ++i) on &= hook_slot(list, 37 + i, reinterpret_cast<void*>(root_view_forwarders[i]), original_root_views[i]);
    on &= hook_slot(list, 43, reinterpret_cast<void*>(&hooked_index_buffer), original_index_buffer);
    on &= hook_slot(list, 44, reinterpret_cast<void*>(&hooked_vertex_buffers), original_vertex_buffers);
    if (ComPtr<ID3D12GraphicsCommandList4> list4; SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))))
        on &= hook_slot(list4.Get(), 72, reinterpret_cast<void*>(&hooked_build), original_build);
    dingosdk::logging::write(on ? dingosdk::logging::Level::info : dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
        on ? "GPU diagnostics: GPU addresses bound for draws, dispatches and ray-tracing builds are checked against the memory that exists."
           : "GPU diagnostics: some of the GPU address checks could not start.");
    SetLastError(error);
}

void watch_gpu_memory() noexcept {
    static std::atomic<bool> started{};
    const auto error = GetLastError();
    if (!graphics_option_enabled(L"RESKATE_GPU_DIAGNOSTICS") || started.exchange(true)) {
        SetLastError(error);
        return;
    }
    // (d3d12.dll is ReSkate's own import, so it is loaded; the game finds D3D12CreateDevice there.)
    auto* const create = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateDevice"));
    void* trampoline{};
    if (!create || hook_prepare(create, reinterpret_cast<void*>(&hooked_create_device), &trampoline) != HookOk) {
        dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
            "GPU diagnostics: the GPU memory watch could not start.");
        SetLastError(error);
        return;
    }
    original_create_device.store(reinterpret_cast<CreateDevice>(trampoline));
    hook_enable(create);
    SetLastError(error);
}

void describe_gpu_address(std::uint64_t address) noexcept {
    try {
        using dingosdk::logging::Level;
        using dingosdk::logging::Channel;
        auto& m = gpu_memory();
        std::lock_guard lock(m.mutex);
        if (!m.made_count) return;
        const auto now = GetTickCount64();
        const auto kept = std::min<std::uint64_t>(m.freed_count, freed_kept);
        // A heap's GPU address, live or freed, for resources placed in it whose own is not known.
        // (A freed heap is found by its old pointer, which a newer object may since have taken.)
        std::unordered_map<void*, std::uint64_t> freed_heaps;
        for (std::uint64_t i = 0; i < kept; ++i)
            if (const auto& a = m.freed[i]; a.heap && a.gpu) freed_heaps.try_emplace(a.object, a.gpu);
        const auto heap_gpu = [&](void* heap) -> std::uint64_t {
            if (const auto found = m.live.find(heap); found != m.live.end() && found->second.gpu) return found->second.gpu;
            if (const auto found = freed_heaps.find(heap); found != freed_heaps.end()) return found->second;
            return 0;
        };
        const auto covers = [&](const Allocation& a) {
            auto gpu = a.gpu;
            if (!gpu && a.in_heap) if (const auto base = heap_gpu(a.in_heap)) gpu = base + a.offset;
            return gpu && a.bytes && address >= gpu && address - gpu < a.bytes;
        };
        dingosdk::logging::log(Level::error, Channel::graphics, "GPU memory watch: {} heaps and resources live, {} freed since the device was made.",
            m.live.size(), m.freed_count);
        unsigned said{};
        for (const auto& [object, a] : m.live)
            if (covers(a) && said < 6) { ++said; say_allocation("live there", a, now); }
        for (std::uint64_t i = 0; i < kept && said < 12; ++i) {
            const auto& a = m.freed[(m.freed_count - 1 - i) % freed_kept];
            if (covers(a)) { ++said; say_allocation("freed there", a, now); }
        }
        if (said) return;
        // The address is in nothing whose GPU address is known (textures in heaps with no buffer):
        // the last heaps and resources freed, which is what DRED's "freed there recently" names.
        dingosdk::logging::write(Level::error, Channel::graphics, "  Nothing known covers that address; the last freed heaps and resources:");
        unsigned heaps{}, resources{};
        for (std::uint64_t i = 0; i < kept && (heaps < 4 || resources < 4); ++i) {
            const auto& a = m.freed[(m.freed_count - 1 - i) % freed_kept];
            if (a.heap ? heaps++ < 4 : resources++ < 4) say_allocation("freed", a, now);
        }
    } catch (...) {}
}

void watch_draws(ID3D12GraphicsCommandList* list) noexcept {
    static std::atomic<bool> watching{};
    const auto error = GetLastError();
    if (!list || !graphics_option_enabled(L"RESKATE_GPU_DIAGNOSTICS") || watching.exchange(true)) {
        SetLastError(error);
        return;
    }
    // (Slots 12 and 13 of ID3D12GraphicsCommandList: after IUnknown, ID3D12Object, GetDevice,
    // GetType, Close, Reset and ClearState.)
    auto* const table = *reinterpret_cast<void***>(list);
    void* draw_original{};
    void* indexed_original{};
    const bool draw = hook_prepare(table[12], reinterpret_cast<void*>(&hooked_draw), &draw_original) == HookOk;
    if (draw) original_draw.store(reinterpret_cast<DrawInstanced>(draw_original));
    const bool indexed = hook_prepare(table[13], reinterpret_cast<void*>(&hooked_draw_indexed), &indexed_original) == HookOk;
    if (indexed) original_draw_indexed.store(reinterpret_cast<DrawIndexedInstanced>(indexed_original));
    const bool on = (draw && hook_enable(table[12]) == HookOk) | (indexed && hook_enable(table[13]) == HookOk);
    dingosdk::logging::write(on ? dingosdk::logging::Level::info : dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
        on ? "GPU diagnostics: draws of more than ten million vertices are said in the log."
           : "GPU diagnostics: the draw watch could not start.");
    SetLastError(error);
}

namespace {
std::atomic<std::uint64_t> render_luid{};
}

bool render_adapter_luid(LUID& luid) noexcept {
    const auto value = render_luid.load();
    if (!value) return false;
    luid.LowPart = static_cast<DWORD>(value);
    luid.HighPart = static_cast<LONG>(value >> 32);
    return true;
}

void record_gpu_identity(ID3D12Device* device) noexcept {
    static std::atomic<bool> recorded{};
    const auto error = GetLastError();
    if (device) {
        const auto luid = device->GetAdapterLuid();
        render_luid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
    }
    try {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 description{};
        if (device && !recorded.exchange(true) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&description))) {
            // The user-mode driver's version, as DXGI reports it for the IDXGIDevice interface.
            std::wstring driver;
            if (LARGE_INTEGER version{}; SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
                driver = std::format(L"{}.{}.{}.{}", HIWORD(version.HighPart), LOWORD(version.HighPart), HIWORD(version.LowPart),
                    LOWORD(version.LowPart));
            dingosdk::crash_report::set_gpu(description.Description, driver);
            dingosdk::crash_report::set_gpu_breadcrumbs(gpu_breadcrumbs());
            dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
                std::wstring(L"Graphics card: ") + description.Description + (driver.empty() ? L"" : L", driver " + driver) + L".");
        }
    } catch (...) {}
    SetLastError(error);
}

void watch_device_removal(ID3D12Device* device) noexcept {
    static std::atomic<bool> watching{};
    const auto error = GetLastError();
    if (!device || watching.exchange(true)) {
        SetLastError(error);
        return;
    }
    // A fence asked to signal at UINT64_MAX does so only when the device is removed (the documented
    // way to hear of it). Kept, with the device, for the life of the process.
    struct Watch {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Fence> fence;
        HANDLE event{}, wait{};
    };
    auto* watch = new Watch{device};
    watch->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!watch->event || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&watch->fence))) ||
        FAILED(watch->fence->SetEventOnCompletion(UINT64_MAX, watch->event)) ||
        !RegisterWaitForSingleObject(&watch->wait, watch->event, [](void* context, BOOLEAN) {
            auto* const watched = static_cast<Watch*>(context);
            record_device_failure(watched->device.Get(), watched->device->GetDeviceRemovedReason(), "device removed");
        }, watch, INFINITE, WT_EXECUTEONLYONCE)) {
        dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
            "GPU diagnostics: the device-removal watch could not start; failures are recorded when the overlay next draws.");
    }
    if (gpu_breadcrumbs()) try {
        std::thread(watch_video_memory, device->GetAdapterLuid()).detach();
    } catch (...) {}
    SetLastError(error);
}

// What the GPU was doing when it stopped: from DRED, when RESKATE_GPU_DIAGNOSTICS (the launcher's
// --gpu-diagnostics) turned it on before the device was made. A command list whose last completed
// breadcrumb is short of its count was still running; the operation after the last completed one
// is where it stopped, and the markers (the game's profiling events) before it name the pass.
void record_device_failure(ID3D12Device* device, HRESULT result,
    const char* operation) noexcept {
    const auto error = GetLastError();
    try {
        if (!device || log().removal_recorded.exchange(true)) {
            SetLastError(error);
            return;
        }
        using dingosdk::logging::Level;
        using dingosdk::logging::Channel;
        const auto reason = device->GetDeviceRemovedReason();
        dingosdk::logging::printf(Level::error, Channel::graphics, "GPU failure: operation=%s result=0x%08lx removed_reason=0x%08lx",
            operation, static_cast<unsigned long>(result), static_cast<unsigned long>(reason));
        // The next launches record what the GPU was doing, for the report of the next stop.
        if (reason != S_OK) set_breadcrumbs_left(breadcrumb_launches);
        ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
            dingosdk::logging::write(Level::error, Channel::graphics, "DRED is unavailable on this device.");
            SetLastError(error);
            return;
        }
        if (ComPtr<ID3D12DeviceRemovedExtendedData2> dred2; SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred2))))
            dingosdk::logging::printf(Level::error, Channel::graphics, "DRED device state %d (3 hung, 6 fault, 7 page fault).",
                static_cast<int>(dred2->GetDeviceState()));
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        if (const auto status = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs); FAILED(status)) {
            dingosdk::logging::printf(Level::error, Channel::graphics,
                "DRED breadcrumbs unavailable (0x%08lx): the next %d launches record them.", static_cast<unsigned long>(status),
                breadcrumb_launches);
        } else {
            unsigned finished{}, running{}, waiting{};
            for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
                const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                if (done >= node->BreadcrumbCount) { ++finished; continue; }
                // (A list with nothing done was queued behind the one at fault: counted, not shown.)
                if (!done) { ++waiting; continue; }
                if (++running > 8) continue;
                dingosdk::logging::printf(Level::error, Channel::graphics, "Still running: list \"%s\" on queue \"%s\", %u of %u operations completed.",
                    narrow(node->pCommandListDebugNameA, node->pCommandListDebugNameW).c_str(),
                    narrow(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW).c_str(), done, node->BreadcrumbCount);
                // The markers (and events) up to where it stopped: the last few name the pass.
                std::array<UINT, 6> marked{};
                std::size_t marks{};
                for (UINT c = 0; c < node->BreadcrumbContextsCount; ++c)
                    if (node->pBreadcrumbContexts[c].BreadcrumbIndex <= done) marked[marks++ % marked.size()] = c;
                for (std::size_t m = marks > marked.size() ? marks - marked.size() : 0; m < marks; ++m) {
                    const auto& context = node->pBreadcrumbContexts[marked[m % marked.size()]];
                    dingosdk::logging::printf(Level::error, Channel::graphics, "  marker at op %u: %s", context.BreadcrumbIndex,
                        narrow(nullptr, context.pContextString).c_str());
                }
                if (!node->pCommandHistory) continue;
                const UINT begin = done > 6 ? done - 6 : 0;
                const UINT end = std::min<UINT>(node->BreadcrumbCount, done + 4);
                for (UINT op = begin; op < end; ++op)
                    dingosdk::logging::printf(Level::error, Channel::graphics, "  %s op %u: %s", op == done ? ">>" : "  ", op,
                        operation_name(node->pCommandHistory[op]));
            }
            dingosdk::logging::printf(Level::error, Channel::graphics, "DRED: %u command list(s) partway through, %u not started, %u finished.",
                running, waiting, finished);
        }
        say_descriptor_heaps();
        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
            dingosdk::logging::printf(Level::error, Channel::graphics, "GPU page fault at 0x%llx.", fault.PageFaultVA);
            allocations("still allocated there", fault.pHeadExistingAllocationNode);
            allocations("freed there recently", fault.pHeadRecentFreedAllocationNode);
            describe_gpu_address(fault.PageFaultVA);
            if (!fault.pHeadExistingAllocationNode && !fault.pHeadRecentFreedAllocationNode)
                dingosdk::logging::write(Level::error, Channel::graphics,
                    "  No object is allocated at that address, or was freed there recently: the GPU was given an address that belongs to nothing.");
        } else {
            dingosdk::logging::write(Level::error, Channel::graphics, "DRED: no GPU page fault recorded.");
        }
    } catch (...) {}
    SetLastError(error);
}
}
