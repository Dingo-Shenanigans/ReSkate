#include "world_cover.h"
#include "world_cover_hlsl.h" // generated from world_cover.hlsl (cmake/UI.cmake)
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Hooks/patches.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Plugins/plugin_camera.h"
#include "Extension/UI/Overlay/gpu_diagnostics.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

// world_cover.h says what this does and how it finds its moment in the game's frame.
namespace dingosdk::world_cover {
namespace {
using Microsoft::WRL::ComPtr;

// Slots of the D3D12 interfaces' tables (d3d12.h's declaration order).
constexpr std::size_t device_create_rtv = 20, device_create_dsv = 21, device_copy_descriptors = 23, device_copy_simple = 24;
constexpr std::size_t list_set_targets = 46, list_begin_render_pass = 68, queue_execute = 10;
constexpr std::size_t list_compute_root_cbv = 37; // SetComputeRootConstantBufferView
// world_cover.hlsl's cbuffer: seven float4s of camera and look, then the places kept clear (one float4
// with their count and heights, three with their feet and radii), then last frame's camera (four). A
// root signature holds 64 values at most, and the descriptor table takes one.
constexpr UINT constant_count = 60;
constexpr std::size_t most_clear = 3;
// Our copy of the game's depth, taken as soon as the game has finished it each frame (snapshot_depth):
// read at the cover's moment the game's own had sometimes been used for something else by then, and
// the snow vanished for a frame or came out in stripes (2026-11-11).
constexpr DXGI_FORMAT depth_copy_format = DXGI_FORMAT_R32_FLOAT;
// Each command allocator's own shader views in our heap (a list in flight reads its own): the
// picture's copy and our depth copy. Each frame's cover is drawn afresh: blended with last frame's,
// found with the camera as the client last read it (a frame apart from the GPU's at times), everything
// that moved left an outline of itself in the snow (2026-11-10).
constexpr UINT views_each = 2;
// The cameras the measurement tries (world_cover.hlsl's ps_measure): only the one read now. Trying the
// two before as well and drawing with whichever agreed best chose a different one every few seconds
// (their agreement differed by noise), and every change moved the whole cover by a frame (2026-11-11).
constexpr std::size_t camera_candidates = 1;
// The world is found with the camera of 1.15 frames before this one (between the two before, blended): the
// camera is read as the cover is drawn, and the game is about that far ahead of its picture by then. With
// this frame's, world-fixed things (the trails) swam as the camera moved. Tried in the game (2026-11-13): two
// frames before was better than none or one in whole frames, and finer, 1.1 to 1.25 was best; the lag varies
// a little with the frame rate, so none is perfect.
constexpr float camera_delay = 1.15f;
constexpr std::size_t camera_history = 3; // the cameras kept, newest first: enough for camera_delay
constexpr std::size_t camera_floats = 12; // right, up, back, eye
// Trails through the cover (add_trail): at most this many points kept (world_cover.hlsl's MOST_TRAIL).
constexpr std::size_t most_trail = 256;
// The shaders' buffer (world_cover.hlsl's t3), one part of the upload buffer per command allocator: the
// cameras (four float4s each), then a float4 a place kept clear (how much snow shows on what stands in
// it), then the trails (two float4s of head, then two a point).
constexpr std::size_t place_floats4 = 3; // a place kept clear: its snow; a board's forward and half length; its up and half width
// The roof map (Settings::under_roofs), after the trails: two float4s of head (on, the near grid's cells
// across and cell size, the far grid's cells across; the far grid's cell size), then the near grid's cells,
// then the far grid's, a float4 each (the highest surface over its middle, which cell it is: x, z, and 1
// once measured). Each grid's cells round the camera are kept where their world position falls (x and z each
// modulo its cells across), so they stay as the camera moves and only the new ones are measured. One
// straight-down ray a cell. The near grid is fine and refreshed often, the far one coarse: one grid 48 m
// each way showed the snow under a roof go as the camera came near it (2026-11-13).
struct RoofGrid {
    int cells;    // across, each way
    float size;   // a cell, metres
    int per_tick; // rays a tick
};
constexpr std::array<RoofGrid, 2> roof_grids{{
    {128, 0.5f, 256},  // 64 m across, in about half a second
    {128, 1.5f, 128},  // 192 m across (96 m each way), in about two seconds
}};
constexpr std::size_t roof_cells_of(const RoofGrid& g) { return static_cast<std::size_t>(g.cells) * static_cast<std::size_t>(g.cells); }
constexpr std::size_t roof_floats4 = 2 + roof_cells_of(roof_grids[0]) + roof_cells_of(roof_grids[1]);
constexpr float roof_reach = 60.0f; // metres over and under the camera the rays start and end
constexpr UINT64 camera_slice = ((camera_candidates * 4 + most_clear * place_floats4 + 2 + most_trail * 2 + roof_floats4) * 16 + 255) /
                                256 * 256;
// The measurements: one row a candidate camera, a column a point (ps_measure), read back by the CPU.
constexpr UINT measure_columns = 32;
constexpr UINT measure_pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr UINT64 measure_slice = UINT64{measure_pitch} * camera_candidates;
// Command allocators: the depth's copy and the cover each take one a frame, and the GPU runs frames
// behind; with eight a frame now and then found none free and went without the snow (2026-11-11).
constexpr std::size_t allocator_count = 16;

using CreateViewFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const void*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescriptorsFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopySimpleFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using SetTargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

std::atomic<CreateViewFn> original_rtv{}, original_dsv{};
std::atomic<CopyDescriptorsFn> original_copy{};
std::atomic<CopySimpleFn> original_copy_simple{};
std::atomic<SetTargetsFn> original_targets{};
using RenderPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                              const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
std::atomic<RenderPassFn> original_render_pass{};
std::atomic<ExecuteFn> original_execute{};
// The game's camera, as it drew the frame: the compute constant buffers it binds at root parameter 0, the last
// bound_kept of them in turn (a count, and the slot each went into). Tried in the game (2026-11-13): these
// begin with the camera the frame is drawn with, 16 bytes in, its right, up and back as rows four floats
// apart and where it is in the fourth row, and are bound before the cover is recorded. The camera the game
// drew with was 0 to 2 frames older than the newest read from the game, sometimes between two of them, so a
// fixed delay was right only on average and world-fixed things (trails) still moved a little.
using RootViewFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
std::atomic<RootViewFn> original_compute_cbv{};
constexpr std::size_t bound_kept = 64;
std::array<std::atomic<std::uint64_t>, bound_kept> bound_addresses{};
std::atomic<std::uint64_t> bound_count{};

// What a plugin asked for (client thread).
struct Asked {
    std::mutex mutex;
    Settings settings;
    std::vector<Clear> clear;
    std::uint64_t clear_until{}; // keep_clear holds for 300 ms
    std::uint64_t stopping_since{}; // when the snowfall began to stop (GetTickCount64), 0 while it falls
    struct TrailPoint {
        std::array<float, 3> at{};
        float width{}, fade{};
        float tread{}; // the width of a tread pressed into it (its lines), or 0
        bool starts{}; // begins a new trail, not joined to the point before
        std::uint64_t when{};
    };
    std::deque<TrailPoint> trail;
    // The roof map (measure_roofs): a cell's (highest surface over it, x, z, 1 once measured), and where the
    // camera was last drawn from.
    std::array<std::vector<std::array<float, 4>>, 2> roofs{std::vector<std::array<float, 4>>(roof_cells_of(roof_grids[0])),
                                                           std::vector<std::array<float, 4>>(roof_cells_of(roof_grids[1]))};
    std::array<std::size_t, 2> roof_next{}; // each grid's next cell of the window round the camera to measure
    std::array<float, 3> eye{};
    bool eye_known{};
};
Asked& asked() { static auto* value = new Asked; return *value; }
std::atomic<bool> wanted{};
std::atomic<bool> allowed{};     // the world has been in play for a moment (allow)
std::atomic<bool> retired{};     // the game's frame is not one the cover can safely go into
std::uint64_t playing_since{};   // client thread: when play began, 0 out of play

// A view the game made: what it is of, and in which format.
struct View {
    ID3D12Resource* resource{};
    DXGI_FORMAT format{};
};
bool hdr_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

// What one of the game's command lists did while it was recorded, as far as the cover cares.
struct Notes {
    bool world{};          // drew with the world's depth and an HDR colour target
    ID3D12Resource* colour{};
    bool colour_state_known{};
    D3D12_RESOURCE_STATES colour_state{};
    bool depth_state_known{};
    D3D12_RESOURCE_STATES depth_state{}; // the main depth's state when the list ends
    bool unsafe{};         // a split or aliasing barrier on the colour or depth: no cover this frame
    std::uint64_t last_written{}; // the kind (format, flags, compute or drawn) of the last full-size float picture written on it
    // The frame's last list: the screen goes from PRESENT to a render target on it, and the float
    // picture of the finished scene it puts there (with the UI, from a layer of its own) is moved from
    // being written to being read: that picture, its format, and its state before this list.
    bool composite{};
    ID3D12Resource* final_picture{};
    D3D12_RESOURCE_STATES final_before{};
    DXGI_FORMAT final_format{};
};

struct Shared {
    std::mutex mutex;
    // The game's render target and depth views, by CPU handle (every heap of the device: one address space).
    std::unordered_map<SIZE_T, View> rtvs, dsvs;
    std::unordered_map<ID3D12GraphicsCommandList*, Notes> notes;
    ID3D12CommandQueue* queue{};
    ID3D12Device* device{};
    // The frame: the screen's size, the main depth buffer (last seen), the world colour.
    std::uint32_t width{}, height{};
    ID3D12Resource* main_depth{};
    // A reference of ours on it while it is the world's depth: a level loaded again freed the game's,
    // and the pointer kept was read as a depth buffer after it had gone (a crash as the snowskate deck
    // went on, 2026-11-11). Let go (through `retiring`) whenever the cover rests: menus, loading.
    ComPtr<ID3D12Resource> main_depth_hold;
    std::uint32_t depth_width{}, depth_height{};
    ID3D12Resource* colour{};
    DXGI_FORMAT colour_format{};
    bool colour_state_known{};
    D3D12_RESOURCE_STATES colour_state{};
    bool depth_state_known{};
    D3D12_RESOURCE_STATES depth_state{}; // the main depth's state after the lists submitted so far
    bool depth_elsewhere{}; // a list for another queue changed the depth's state this frame
    bool main_seen{};      // the main depth went through a barrier since the last present
    unsigned main_missed{}; // presents in a row it did not: after 30 another may take its place
    // One of the same size seen in its place, and in how many frames in a row (it takes over after 3).
    ID3D12Resource* candidate{};
    std::uint64_t candidate_frame{};
    unsigned candidate_run{};
    bool unsafe{};         // this frame's colour or depth went through a split or aliasing barrier
    bool pending{};        // the world was drawn in a submitted batch, the cover not yet
    bool drawn{};          // the cover is drawn this frame
    std::uint64_t frame{};
    // Ours: the picture's copy, the target's view, our views, our lists.
    ComPtr<ID3D12Resource> picture;
    D3D12_RESOURCE_DESC picture_desc{};
    // The cover as drawn (how much, and how far): two, one read as last frame's and one written, swapped
    // each frame; and the camera it was drawn from.
    // What the GPU may still be using when it is replaced: kept until the fence passes the value the
    // last list that could use it signals. Freed at once, a frame in flight read freed memory and the
    // whole picture came out in stripes (2026-11-11).
    std::vector<std::pair<UINT64, ComPtr<ID3D12Pageable>>> retiring;
    // The format and size of the picture the cover goes into, the first time it goes into one: a final
    // picture of another format is let be (no new copy, pipeline or shader compile in the middle of play).
    DXGI_FORMAT chosen_format{DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3D12Resource> depth_copy;
    ComPtr<ID3D12PipelineState> depth_pipeline;
    std::uint64_t depth_copied_frame{~std::uint64_t{0}}; // the frame our depth copy was taken in
    std::array<float, 12> last_camera{};
    std::uint64_t last_drawn_frame{~std::uint64_t{0}};
    ComPtr<ID3D12DescriptorHeap> views, targets;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    DXGI_FORMAT pipeline_format{DXGI_FORMAT_UNKNOWN};
    // Eight: the GPU runs frames behind, and with three every other frame found none free (2026-11-10).
    std::array<ComPtr<ID3D12CommandAllocator>, allocator_count> allocators;
    std::array<UINT64, allocator_count> allocator_fence{};
    // The measurements (ps_measure): drawn into `measure`, copied to this allocator's part of the
    // readback buffer, read when the allocator is next used (the GPU is done with it then).
    ComPtr<ID3D12Resource> measure, measure_readback;
    ComPtr<ID3D12PipelineState> measure_pipeline;
    const std::uint8_t* measure_cpu{};
    std::array<bool, allocator_count> measure_waiting{};
    // Whether that frame was drawn with its own depth: one drawn with an older copy (the camera moved
    // since) measured the near distance too short, and it was held (0.056 m for 0.060, 2026-11-12).
    std::array<bool, allocator_count> measure_fresh{};
    bool relearning{}; // a new camera: the next middle measured is taken, up or down
    // The cameras of this and the last two frames, newest first, and how many are known.
    std::array<std::array<float, camera_floats>, camera_history> cameras{};
    std::size_t cameras_known{};
    // The game's own camera (game_camera): the binds already looked through, and how many frames found it.
    std::uint64_t bound_seen{};
    unsigned game_camera_frames{}, camera_frames{}, camera_reports{};
    std::array<float, 3> camera_signs{1, 1, 1}; // which way round each of its axes is against ours
    float places_back = camera_delay; // how many frames back the places are taken (the drawn camera's, as found)
    // The places kept clear as each of those frames was drawn, newest first: a place on something that moves
    // (a deck) is drawn where the picture has it, camera_delay back, like the world.
    std::array<std::vector<Clear>, camera_history> clears{};
    // The near distance each recent frame measured, and the steady middle of them (0: none yet).
    std::deque<float> nears;
    float held_near{};
    float shown_near{}; // what the shader is given: held_near, eased into over half a second
    double fallen{}; // how far the snow has fallen, in seconds at the usual speed (each moment at its own speed)
    std::uint64_t fallen_at{};
    std::uint64_t shown_near_at{};
    unsigned near_disagreed{}; // frames in a row far from held_near
    std::uint64_t measured_frames{};
    std::size_t next_allocator{};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> camera_upload; // a slice a command allocator (camera_slice bytes each)
    std::uint8_t* camera_cpu{};
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value{};
    bool failed{}, said_drawn{}, said_late{}, said_depth{}, said_colour{}, said_waiting{}, said_target{};
    unsigned presents_wanted{}; // presents while the cover was wanted and allowed (for the log)
    std::set<std::string> bindings_said; // what the game binds with the world's depth, said once each
    std::set<ID3D12Resource*> pictures_said; // full-size float pictures seen written, said once each
    // The game writes more than one full-size float picture a frame. Drawing into whichever came last
    // before the cover's moment made it flicker (2026-11-10), so it locks onto one: the picture written
    // last, after the world's depth is done, in most of 30 frames (the game's final float picture,
    // before its tone mapping and UI).
    struct Vote {
        unsigned count{};
        DXGI_FORMAT format{};
    };
    // By kind, not by picture: the game keeps two of each and takes turns with them a frame each (its
    // anti-aliasing reads last frame's), so one picture locked onto had the cover every other frame.
    std::map<std::uint64_t, Vote> votes;
    std::uint64_t frame_last{}; // the kind of this frame's last picture written after the depth
    std::uint64_t locked{};     // the kind the cover goes into, 0 before one is chosen
    bool locked_seen{};
    unsigned frames_voted{}, locked_missed{};
    // Why frames went without the cover, counted and said in the log now and then.
    const char* why{};          // the reason the last try this frame did not draw
    bool tried{};               // the cover's moment came this frame
    std::map<std::string, unsigned> outcomes;
    unsigned outcome_frames{}, outcome_reports{};
};
Shared& shared() { static auto* value = new Shared; return *value; }

// The main depth buffer's handles, readable without the lock by the OMSetRenderTargets hook, which
// runs thousands of times a frame: the common case (another depth, or none) leaves at once.
std::array<std::atomic<SIZE_T>, 8> depth_handles{};
std::atomic<std::size_t> next_depth_handle{};
std::atomic<ID3D12Resource*> main_depth_now{};
std::atomic<ID3D12Resource*> colour_now{};
std::atomic<bool> active{}; // wanted, and installed
// Our own command list: its barriers go through the same hook as the game's and are not the game's.
std::atomic<ID3D12GraphicsCommandList*> our_list{};

void say_failed(const std::string& why) {
    auto& s = shared();
    if (s.failed) return;
    s.failed = true;
    logging::write(logging::Level::warning, logging::Channel::graphics, "World cover unavailable: " + why);
}

void remember_depth_handle(SIZE_T handle) {
    for (const auto& known : depth_handles)
        if (known.load(std::memory_order_relaxed) == handle) return;
    depth_handles[next_depth_handle.fetch_add(1) % depth_handles.size()].store(handle);
}
bool is_depth_handle(SIZE_T handle) {
    for (const auto& known : depth_handles)
        if (known.load(std::memory_order_relaxed) == handle) return true;
    return false;
}

// ---- hooks ------------------------------------------------------------------------------------------
void STDMETHODCALLTYPE hooked_rtv(ID3D12Device* device, ID3D12Resource* resource, const void* description, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    original_rtv.load()(device, resource, description, at);
    try {
        View view{resource, DXGI_FORMAT_UNKNOWN};
        if (description) view.format = static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(description)->Format;
        else if (resource) view.format = resource->GetDesc().Format;
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.rtvs[at.ptr] = view;
    } catch (...) {}
}
void STDMETHODCALLTYPE hooked_dsv(ID3D12Device* device, ID3D12Resource* resource, const void* description, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    original_dsv.load()(device, resource, description, at);
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.dsvs[at.ptr] = {resource, DXGI_FORMAT_UNKNOWN};
        if (resource && resource == main_depth_now.load()) remember_depth_handle(at.ptr);
    } catch (...) {}
}
// Views copied from one heap to another keep what they are of.
void copy_views(D3D12_DESCRIPTOR_HEAP_TYPE type, SIZE_T to, SIZE_T from, UINT count, UINT increment) {
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return;
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    auto& map = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? s.rtvs : s.dsvs;
    for (UINT i = 0; i < count; ++i) {
        const auto found = map.find(from + SIZE_T{i} * increment);
        if (found == map.end()) map.erase(to + SIZE_T{i} * increment);
        else {
            map[to + SIZE_T{i} * increment] = found->second;
            if (type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV && found->second.resource && found->second.resource == main_depth_now.load())
                remember_depth_handle(to + SIZE_T{i} * increment);
        }
    }
}
void STDMETHODCALLTYPE hooked_copy_simple(ID3D12Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE to, D3D12_CPU_DESCRIPTOR_HANDLE from,
                                          D3D12_DESCRIPTOR_HEAP_TYPE type) {
    original_copy_simple.load()(device, count, to, from, type);
    try {
        copy_views(type, to.ptr, from.ptr, count, device->GetDescriptorHandleIncrementSize(type));
    } catch (...) {}
}
void STDMETHODCALLTYPE hooked_copy(ID3D12Device* device, UINT to_count, const D3D12_CPU_DESCRIPTOR_HANDLE* to_starts, const UINT* to_sizes, UINT from_count,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* from_starts, const UINT* from_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    original_copy.load()(device, to_count, to_starts, to_sizes, from_count, from_starts, from_sizes, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return;
    try {
        // Both sides are runs of descriptors; walk them together, one descriptor at a time.
        const UINT increment = device->GetDescriptorHandleIncrementSize(type);
        UINT ti = 0, fi = 0, to_at = 0, from_at = 0;
        while (ti < to_count && fi < from_count) {
            const UINT to_size = to_sizes ? to_sizes[ti] : 1, from_size = from_sizes ? from_sizes[fi] : 1;
            copy_views(type, to_starts[ti].ptr + SIZE_T{to_at} * increment, from_starts[fi].ptr + SIZE_T{from_at} * increment, 1, increment);
            if (++to_at >= to_size) ++ti, to_at = 0;
            if (++from_at >= from_size) ++fi, from_at = 0;
        }
    } catch (...) {}
}

// A floating-point colour format a view of this format reads as, or UNKNOWN: a typeless view takes
// its family's float format.
DXGI_FORMAT hdr_view_format(DXGI_FORMAT f) {
    if (hdr_format(f)) return f;
    if (f == DXGI_FORMAT_R16G16B16A16_TYPELESS) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (f == DXGI_FORMAT_R32G32B32A32_TYPELESS) return DXGI_FORMAT_R32G32B32A32_FLOAT;
    return DXGI_FORMAT_UNKNOWN;
}

// Targets bound with the world's depth (OMSetRenderTargets or BeginRenderPass): the first colour target,
// if it is a floating-point picture the depth's size, is the world's picture.
void note_targets(ID3D12GraphicsCommandList* list, UINT count, SIZE_T first_colour, const char* how) {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    const View* view{};
    if (count) {
        const auto found = s.rtvs.find(first_colour);
        if (found != s.rtvs.end() && found->second.resource) view = &found->second;
    }
    D3D12_RESOURCE_DESC desc{};
    if (view) desc = view->resource->GetDesc();
    const DXGI_FORMAT format = view ? hdr_view_format(view->format != DXGI_FORMAT_UNKNOWN ? view->format : desc.Format) : DXGI_FORMAT_UNKNOWN;
    const bool fits = view && format != DXGI_FORMAT_UNKNOWN && desc.Width == s.depth_width && desc.Height == s.depth_height &&
                      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 && desc.DepthOrArraySize == 1;
    // What the game binds with the world's depth, each kind said once (at most 12), while no picture is found.
    if (!s.said_colour && s.bindings_said.size() < 12) {
        const std::string kind = !count ? std::format("{}: depth only", how)
                                 : !view ? std::format("{}: {} target(s), the first a view not known", how, count)
                                         : std::format("{}: {} target(s), the first {}x{} in view format {} (resource format {})", how, count, desc.Width,
                                                       desc.Height, static_cast<unsigned>(view->format), static_cast<unsigned>(desc.Format));
        if (s.bindings_said.insert(kind).second)
            logging::write(logging::Level::debug, logging::Channel::graphics, "World cover: with the world's depth, " + kind + ".");
    }
    if (!fits || s.locked) return;
    auto& n = s.notes[list];
    n.world = true;
    n.colour = view->resource;
    n.colour_state_known = true; // bound as a target: in RENDER_TARGET now
    n.colour_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s.colour_format = format;
    if (!s.said_colour) {
        s.said_colour = true;
        logging::printf(logging::Level::info, logging::Channel::graphics, "World cover: the world's picture is %llux%u, format %u, %u mip level(s) (%s).",
                        static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(format), static_cast<unsigned>(desc.MipLevels), how);
    }
    colour_now.store(n.colour);
}

void STDMETHODCALLTYPE hooked_targets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* colours, BOOL single_range,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    original_targets.load()(list, count, colours, single_range, depth);
    if (!active.load(std::memory_order_relaxed) || !depth || !is_depth_handle(depth->ptr)) return;
    try {
        note_targets(list, colours ? count : 0, count && colours ? colours[0].ptr : 0, "OMSetRenderTargets");
    } catch (...) {}
}

void STDMETHODCALLTYPE hooked_render_pass(ID3D12GraphicsCommandList4* list, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* colours,
                                          const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags) {
    original_render_pass.load()(list, count, colours, depth, flags);
    if (!active.load(std::memory_order_relaxed) || !depth || !is_depth_handle(depth->cpuDescriptor.ptr)) return;
    try {
        note_targets(list, colours ? count : 0, count && colours ? colours[0].cpuDescriptor.ptr : 0, "BeginRenderPass");
    } catch (...) {}
}

bool record_cover(Shared& s);
bool snapshot_depth(Shared& s, ID3D12CommandQueue* queue, ExecuteFn original);
bool depth_shaped(const D3D12_RESOURCE_DESC& d, std::uint32_t width, std::uint32_t height);

void STDMETHODCALLTYPE hooked_execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    const auto original = original_execute.load();
    if (!active.load(std::memory_order_relaxed) || !lists || !count) return original(queue, count, lists);
    try {
        auto& s = shared();
        std::unique_lock lock(s.mutex);
        if (queue != s.queue) {
            lock.unlock();
            return original(queue, count, lists);
        }
        bool world_here = false;
        for (UINT i = 0; i < count; ++i) {
            const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (found != s.notes.end() && found->second.world) world_here = true;
        }
        (void)world_here;
        // The frame's last batch, which puts the finished scene on the screen with the UI (a layer of its
        // own): the cover goes into the scene's picture just before it, in a list of ours. The game draws
        // almost a whole frame in one command list, so this is the first place after the scene is done
        // (traced 2026-11-10: the screen PRESENT to render target, the 1920x1080 R11G11B10 scene and the
        // RGBA8 UI layer read, the screen back to PRESENT, all in one batch of one list).
        const Notes* composite{};
        for (UINT i = 0; i < count && !composite; ++i) {
            const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (found != s.notes.end() && found->second.composite && found->second.final_picture) composite = &found->second;
        }
        if (composite && !s.drawn) {
            s.tried = true;
            s.colour = composite->final_picture;
            s.colour_state = composite->final_before;
            s.colour_state_known = true;
            if (s.chosen_format == DXGI_FORMAT_UNKNOWN) s.chosen_format = composite->final_format;
            s.colour_format = s.chosen_format;
            s.why = s.unsafe ? "a split or aliasing barrier, or another queue" : s.depth_elsewhere ? "the depth moved on another queue"
                  : composite->final_format != s.chosen_format ? "the final picture in another format" : nullptr;
            const bool drew = !s.why && record_cover(s);
            if (drew) {
                s.drawn = true;
                ID3D12CommandList* ours[] = {s.list.Get()};
                original(queue, 1, ours);
                queue->Signal(s.fence.Get(), ++s.fence_value);
                s.allocator_fence[(s.next_allocator + s.allocators.size() - 1) % s.allocators.size()] = s.fence_value;
                if (!s.said_drawn) {
                    s.said_drawn = true;
                    logging::printf(logging::Level::info, logging::Channel::graphics,
                                    "World cover: drawn into the finished scene (format %u) just before the game puts it on the screen with its UI.",
                                    static_cast<unsigned>(s.colour_format));
                }
            }
        }
        bool depth_finished = false;
        for (UINT i = 0; i < count; ++i) {
            const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (found == s.notes.end()) continue;
            const auto& n = found->second;
            if (n.depth_state_known) {
                s.depth_state_known = true;
                s.depth_state = n.depth_state;
                depth_finished = true; // copied once this batch has run, if it is readable now
            }
            if (n.unsafe) s.unsafe = true;
            // A picture counts only once the world's depth is done and readable: one written before
            // that is not the lit world (the game writes one then, and the cover could never read the
            // depth at its moment, 2026-11-10). The last one written after it is kept for the vote.
            const bool depth_done = s.depth_state_known && (s.depth_state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
                                    !(s.depth_state & D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (n.last_written && depth_done) s.frame_last = n.last_written;
            if (s.locked && n.last_written == s.locked) s.locked_seen = true;
            if (n.world) {
                s.colour = n.colour;
                s.pending = !s.drawn;
            }
            if (n.colour_state_known && n.colour && n.colour == s.colour) {
                s.colour_state_known = true;
                s.colour_state = n.colour_state;
            }
            s.notes.erase(found);
        }
        lock.unlock();
        original(queue, count, lists);
        if (depth_finished) {
            std::lock_guard again(s.mutex);
            (void)snapshot_depth(s, queue, original);
        }
    } catch (...) {
        original(queue, count, lists);
    }
}

// ---- drawing ----------------------------------------------------------------------------------------
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier); // our own list: the hook passes it on (it is not the game's)
}

bool build_root(Shared& s) {
    if (s.root) return true;
    {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2; // t0 the picture, t1 the game's depth
        std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = constant_count;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t3: the cameras
        parameters[2].Descriptor.ShaderRegister = 3;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || !blob ||
            FAILED(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.root)))) {
            say_failed("its root signature could not be made.");
            return false;
        }
    }
    return true;
}

// The shaders, compiled once on a thread of their own as soon as the hooks are in (start_compiling): compiled
// as the snow first came on, on the thread the game draws with, nothing was drawn while they compiled, and
// with the roof work that was long enough (25 s) for a black screen and the game leaving the level
// (2026-11-14). Until they are ready the cover waits.
struct Compiled {
    std::mutex mutex;
    std::map<std::string, ComPtr<ID3DBlob>> blobs; // entry -> its shader
    std::string error;
    std::atomic<int> state{}; // 0 not started, 1 compiling, 2 ready, 3 failed
};
Compiled& compiled() { static auto* value = new Compiled; return *value; }
void start_compiling() {
    auto& c = compiled();
    int idle = 0;
    if (!c.state.compare_exchange_strong(idle, 1)) return;
    try {
        std::thread([] {
            auto& done = compiled();
            const auto length = std::strlen(world_cover_hlsl);
            const auto started = GetTickCount64();
            const std::pair<const char*, const char*> entries[]{{"vs_main", "vs_5_0"}, {"ps_main", "ps_5_0"}, {"ps_measure", "ps_5_0"},
                                                                {"ps_depth", "ps_5_0"}};
            for (const auto& [entry, target] : entries) {
                ComPtr<ID3DBlob> blob, error;
                if (FAILED(D3DCompile(world_cover_hlsl, length, "world_cover", nullptr, nullptr, entry, target, 0, 0, &blob, &error)) || !blob) {
                    std::lock_guard lock(done.mutex);
                    done.error = std::string(entry) + (error ? std::string(": ") + static_cast<const char*>(error->GetBufferPointer()) : "");
                    done.state.store(3);
                    return;
                }
                std::lock_guard lock(done.mutex);
                done.blobs[entry] = blob;
            }
            done.state.store(2);
            logging::printf(logging::Level::info, logging::Channel::graphics, "World cover: its shaders compiled in %.1f s, apart from the game's drawing.",
                            static_cast<double>(GetTickCount64() - started) / 1000.0);
        }).detach();
    } catch (...) {
        c.state.store(0);
    }
}
// The vertex shader and `entry`'s, once compiled. False while they are compiling (s.why says so), or when they
// failed (said once).
bool shaders_for(Shared& s, const char* entry, ComPtr<ID3DBlob>& vertex, ComPtr<ID3DBlob>& pixel) {
    auto& c = compiled();
    start_compiling();
    const int state = c.state.load();
    if (state == 3) {
        std::lock_guard lock(c.mutex);
        say_failed("its shader did not compile: " + c.error);
        return false;
    }
    if (state != 2) {
        s.why = "its shaders are still compiling";
        return false;
    }
    std::lock_guard lock(c.mutex);
    vertex = c.blobs["vs_main"], pixel = c.blobs[entry];
    return vertex && pixel;
}

// The pass that copies the game's depth into ours (ps_depth): one target, the depth as a float.
// A pipeline of one target in `format`, with world_cover.hlsl's `entry` (ps_depth, ps_measure).
bool build_single(Shared& s, const char* entry, DXGI_FORMAT format, ComPtr<ID3D12PipelineState>& pipeline) {
    if (pipeline) return true;
    if (!build_root(s)) return false;
    ComPtr<ID3DBlob> vertex, pixel;
    if (!shaders_for(s, entry, vertex, pixel)) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = s.root.Get();
    description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    if (FAILED(s.device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline)))) {
        say_failed(std::string("its pipeline for ") + entry + " was refused.");
        return false;
    }
    return true;
}
bool build_depth_pipeline(Shared& s) { return build_single(s, "ps_depth", depth_copy_format, s.depth_pipeline); }

bool build_pipeline(Shared& s, DXGI_FORMAT format) {
    if (s.pipeline && s.pipeline_format == format) return true;
    if (s.pipeline) s.retiring.push_back({s.fence_value, s.pipeline});
    s.pipeline.Reset();
    if (!build_root(s)) return false;
    ComPtr<ID3DBlob> vertex, pixel;
    if (!shaders_for(s, "ps_main", vertex, pixel)) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = s.root.Get();
    description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    if (FAILED(s.device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&s.pipeline)))) {
        say_failed("its pipeline was refused for the world's picture's format.");
        return false;
    }
    s.pipeline_format = format;
    return true;
}

// Our own command list, its allocators, fence, heaps and the cameras' upload buffer: made once.
bool build_list(Shared& s) {
    if (!s.list) {
        for (auto& allocator : s.allocators)
            if (FAILED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
                say_failed("no command allocator.");
                return false;
            }
        if (FAILED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocators[0].Get(), nullptr, IID_PPV_ARGS(&s.list))) ||
            FAILED(s.list->Close()) || FAILED(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)))) {
            say_failed("no command list.");
            return false;
        }
        s.list->SetName(L"ReSkate world cover");
        our_list.store(s.list.Get());
        D3D12_HEAP_PROPERTIES upload{};
        upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bytes{};
        bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bytes.Width = camera_slice * s.allocators.size();
        bytes.Height = 1;
        bytes.DepthOrArraySize = 1;
        bytes.MipLevels = 1;
        bytes.SampleDesc.Count = 1;
        bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* cpu{};
        if (FAILED(s.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&s.camera_upload))) ||
            FAILED(s.camera_upload->Map(0, nullptr, &cpu)) || !cpu) {
            say_failed("no buffer for the cameras.");
            return false;
        }
        s.camera_cpu = static_cast<std::uint8_t*>(cpu);
        D3D12_DESCRIPTOR_HEAP_DESC views{};
        views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        views.NumDescriptors = views_each * static_cast<UINT>(s.allocators.size());
        views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        D3D12_DESCRIPTOR_HEAP_DESC targets{};
        targets.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        targets.NumDescriptors = 5; // the picture, (1, 2 free), our depth copy, the measurements
        if (FAILED(s.device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&s.views))) || FAILED(s.device->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&s.targets)))) {
            say_failed("no descriptor heaps.");
            return false;
        }
        // The measurements' target, kept as a copy's source between frames, and where they are read.
        D3D12_HEAP_PROPERTIES gpu{};
        gpu.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC measured{};
        measured.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        measured.Width = measure_columns;
        measured.Height = static_cast<UINT>(camera_candidates);
        measured.DepthOrArraySize = 1;
        measured.MipLevels = 1;
        measured.Format = DXGI_FORMAT_R32_FLOAT;
        measured.SampleDesc.Count = 1;
        measured.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES readback{};
        readback.Type = D3D12_HEAP_TYPE_READBACK;
        bytes.Width = measure_slice * s.allocators.size();
        void* read{};
        if (FAILED(s.device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &measured, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                                     IID_PPV_ARGS(&s.measure))) ||
            FAILED(s.device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&s.measure_readback))) ||
            FAILED(s.measure_readback->Map(0, nullptr, &read)) || !read) {
            say_failed("no buffer for its measurements.");
            return false;
        }
        s.measure_cpu = static_cast<const std::uint8_t*>(read);
    }
    return true;
}

// Our own list, heaps, the picture's copy: made the first time, and the copy again when the
// picture changes size or format.
bool build_resources(Shared& s, const D3D12_RESOURCE_DESC& colour) {
    if (!build_list(s)) return false;
    if (!s.picture || s.picture_desc.Width != colour.Width || s.picture_desc.Height != colour.Height || s.picture_desc.Format != colour.Format) {
        if (s.picture) s.retiring.push_back({s.fence_value, s.picture});
        s.picture.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = colour.Width;
        desc.Height = colour.Height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = colour.Format;
        desc.SampleDesc.Count = 1;
        if (FAILED(s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.picture)))) {
            say_failed("no copy of the world's picture could be made.");
            return false;
        }
        s.picture_desc = desc;
        s.last_drawn_frame = ~std::uint64_t{0};
    }
    return true;
}

// Whether this device lets a view read a typed resource in another format of its family.
bool casting_typed(ID3D12Device* device) {
    static const bool supported = [device] {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
        return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options))) &&
               options.CastingFullyTypedFormatSupported;
    }();
    return supported;
}

// The view format the game's depth is read through: its plane of depth as a float, or UNKNOWN.
DXGI_FORMAT depth_read_format(Shared& s, const D3D12_RESOURCE_DESC& d) {
    if (d.Format == DXGI_FORMAT_R32G8X24_TYPELESS) return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    if (d.Format == DXGI_FORMAT_R32_TYPELESS) return DXGI_FORMAT_R32_FLOAT;
    // A depth made in a typed depth format (the game's is D32_FLOAT_S8X24_UINT, and it reads it in
    // shaders itself), read through the same family's float format where the device allows it.
    if (casting_typed(s.device)) {
        if (d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        if (d.Format == DXGI_FORMAT_D32_FLOAT) return DXGI_FORMAT_R32_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

// Under the lock, right after a batch of the game's in which its depth became readable: our list
// draws it into our copy (read where it is, in the state the game left it; nothing of the game's is
// moved or written). The cover reads the copy later in the frame. The copy taken last before the
// cover is the one it reads.
bool snapshot_depth(Shared& s, ID3D12CommandQueue* queue, ExecuteFn original) {
    if (s.failed || !s.main_depth || !s.device || !s.depth_state_known || !(s.depth_state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) ||
        (s.depth_state & (D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) || s.depth_elsewhere)
        return false;
    const auto d = s.main_depth->GetDesc();
    // Still the depth buffer it was found as.
    if (!depth_shaped(d, s.depth_width, s.depth_height) || d.Width != s.depth_width || d.Height != s.depth_height) return false;
    const DXGI_FORMAT read = depth_read_format(s, d);
    if (read == DXGI_FORMAT_UNKNOWN) {
        if (!s.said_depth) {
            s.said_depth = true;
            logging::printf(logging::Level::info, logging::Channel::graphics, "World cover: not drawn; the game's depth (format %u) cannot be read.",
                            static_cast<unsigned>(d.Format));
        }
        return false;
    }
    if (!build_list(s) || !build_depth_pipeline(s)) return false;
    // Our copy, the depth's size; one replaced is kept until the GPU is done with it.
    if (!s.depth_copy || s.depth_copy->GetDesc().Width != d.Width || s.depth_copy->GetDesc().Height != d.Height) {
        if (s.depth_copy) s.retiring.push_back({s.fence_value, s.depth_copy});
        s.depth_copy.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = d.Width;
        desc.Height = d.Height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = depth_copy_format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&s.depth_copy)))) {
            say_failed("no copy of the depth could be made.");
            return false;
        }
    }
    const std::size_t at = s.next_allocator;
    if (s.fence->GetCompletedValue() < s.allocator_fence[at]) return false;
    if (FAILED(s.allocators[at]->Reset()) || FAILED(s.list->Reset(s.allocators[at].Get(), nullptr))) return false;
    s.next_allocator = (at + 1) % s.allocators.size();
    const UINT view_step = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto view = s.views->GetCPUDescriptorHandleForHeapStart();
    view.ptr += SIZE_T{view_step} * (views_each * at + 1); // t1 of this allocator's views
    D3D12_SHADER_RESOURCE_VIEW_DESC depth_view{};
    depth_view.Format = read;
    depth_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth_view.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(s.main_depth, &depth_view, view);
    D3D12_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = depth_copy_format;
    target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    auto target = s.targets->GetCPUDescriptorHandleForHeapStart();
    target.ptr += SIZE_T{s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)} * 3;
    original_rtv.load()(s.device, s.depth_copy.Get(), &target_view, target); // not through our hook: it takes the lock held here

    auto* list = s.list.Get();
    transition(list, s.depth_copy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12DescriptorHeap* heaps[] = {s.views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(s.root.Get());
    list->SetPipelineState(s.depth_pipeline.Get());
    auto table = s.views->GetGPUDescriptorHandleForHeapStart();
    table.ptr += UINT64{view_step} * views_each * at;
    list->SetGraphicsRootDescriptorTable(1, table);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(d.Width), static_cast<float>(d.Height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list, s.depth_copy.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* ours[] = {list};
    original(queue, 1, ours);
    queue->Signal(s.fence.Get(), ++s.fence_value);
    s.allocator_fence[at] = s.fence_value;
    s.depth_copied_frame = s.frame;
    return true;
}

// Records our list: the picture copied, then drawn back with the cover over it. Under the lock.
// One frame's measurements (ps_measure), as read back: the biggest group of them that agree (within
// 4% of each other) is the skater, and its middle is this frame's near distance; the distance held is
// the middle of the last 45 frames'. The game's near distance stays the same for a camera, so one held
// is steadier than any one frame's; a different camera's comes through within a second.
void take_measurements(Shared& s, const std::uint8_t* rows) {
    const auto* row = reinterpret_cast<const float*>(rows);
    std::vector<float> values;
    for (UINT i = 0; i < measure_columns; ++i)
        if (std::isfinite(row[i]) && row[i] > 1e-5f && row[i] < 100.0f) values.push_back(row[i]);
    std::sort(values.begin(), values.end());
    std::size_t best_from = 0, best_count = 0;
    for (std::size_t from = 0, to = 0; from < values.size(); ++from) {
        while (to < values.size() && values[to] <= values[from] * 1.04f) ++to;
        if (to - from > best_count) best_from = from, best_count = to - from;
    }
    if (best_count < 5) return;
    const float measured = values[best_from + best_count / 2];
    // The game's near distance is about 0.06 m; ten times less was measured on something else and held,
    // and the snow before the camera went wrong with it (2026-11-11).
    if (measured < 0.02f || measured > 1.0f) return;
    // A frame far from the distance held is let be, unless twenty in a row say so (a new camera): one
    // that measured the wrong thing moved the whole cover for a moment, and the snow at the bottom of the
    // screen thinned out with it (2026-11-11).
    if (!s.relearning && s.held_near > 0 && std::abs(measured - s.held_near) > s.held_near * 0.2f) {
        if (++s.near_disagreed < 20) return;
        // A new camera: its own distance, from the start. The one held stays until the new one is known
        // (and is then eased into): set to nothing meanwhile, the snow went to each frame's own guess and
        // jumped (2026-11-12).
        s.nears.clear();
        s.relearning = true;
    }
    s.near_disagreed = 0;
    s.nears.push_back(measured);
    // About three seconds of them: a short spell of wrong ones (a change of time of day, a new depth
    // being found) no longer moves the middle.
    while (s.nears.size() > 400) s.nears.pop_front();
    std::vector<float> sorted(s.nears.begin(), s.nears.end());
    const auto half = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
    std::nth_element(sorted.begin(), half, sorted.end());
    // Only once a few frames agree on it. A measurement only ever comes out too far, never too near: the
    // depth is read on the skater's surface, which is a little before the point measured. So the
    // distance held only goes down, to the lowest middle measured (they were 0.060 to 0.072 m, the
    // game's own about 0.060), and in time it stops changing. The world is rebuilt from it: one that
    // went up and down with the measurements made the snow's pattern jump on the ground, plainest
    // while the snow was half melted (2026-11-12).
    if (sorted.size() >= (s.relearning ? 30u : 5u) && (s.held_near <= 0 || s.relearning || *half < s.held_near * 0.997f)) {
        s.relearning = false;
        if (s.held_near > 0)
            logging::printf(logging::Level::debug, logging::Channel::graphics, "World cover: near distance %.5f, was %.5f.", *half, s.held_near);
        s.held_near = *half;
    }
    if (++s.measured_frames % 1800 == 300)
        logging::printf(logging::Level::debug, logging::Channel::graphics, "World cover: near distance %.5f (%zu of %u points agree).", s.held_near,
                        best_count, measure_columns);
}

void STDMETHODCALLTYPE hooked_compute_cbv(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    original_compute_cbv.load(std::memory_order_relaxed)(list, index, address);
    if (index != 0 || !active.load(std::memory_order_relaxed)) return;
    const auto n = bound_count.fetch_add(1, std::memory_order_relaxed);
    bound_addresses[n % bound_kept].store(address, std::memory_order_relaxed);
}

// The camera the game drew this frame with, from the compute constant buffers bound since the last frame
// (the newest 32): those that hold a camera, unit axes as rows from 16 bytes in, where it is in the fourth row.
// Of those close to `camera` (the one read from the game, delayed: within about 14 degrees and 2 m), the one
// most of them hold, the newest of any as many: a frame's buffers hold two or three cameras a little apart
// (the game easing its camera between updates), and with the newest close one, now one, now another, the
// trails jiggled as the camera moved (2026-11-14). Each axis is taken the way round `camera` has it. (In the
// replay editor, whose camera read from the game is not the one it draws with, none is close: the trails
// there were not helped by taking the one most held, 2026-11-14.)
// Its buffer is read through the GPU memory watch (gpu_diagnostics.h). False when there is none.
bool game_camera(Shared& s, std::array<float, camera_floats>& camera) {
    const auto end = bound_count.load();
    constexpr std::uint64_t looked_most = 32;
    const auto begin = std::max(s.bound_seen, end > looked_most ? end - looked_most : std::uint64_t{0});
    s.bound_seen = end;
    std::vector<std::array<float, camera_floats>> held; // as the buffers have them, newest first
    std::array<float, 20> f{};
    for (auto n = end; n > begin; --n) {
        const auto address = bound_addresses[(n - 1) % bound_kept].load(std::memory_order_relaxed);
        if (!address || overlay::read_gpu_buffer(address, f.data(), sizeof(f)) < sizeof(f)) continue;
        const float* const rows = f.data() + 4;
        std::array<float, camera_floats> one{};
        bool ok = true;
        for (std::size_t a = 0; a < 3 && ok; ++a) {
            float length = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const float v = rows[a * 4 + c];
                ok = ok && std::isfinite(v);
                length += v * v;
                one[a * 3 + c] = v;
            }
            ok = ok && std::abs(length - 1.0f) < 0.02f;
        }
        for (std::size_t c = 0; c < 3 && ok; ++c) {
            ok = std::isfinite(rows[12 + c]) && std::abs(rows[12 + c]) < 1e6f;
            one[9 + c] = rows[12 + c];
        }
        if (ok) held.push_back(one);
    }
    const auto turned = [&](const std::array<float, camera_floats>& one) {
        auto out = one;
        for (std::size_t a = 0; a < 3; ++a)
            for (std::size_t c = 0; c < 3; ++c) out[a * 3 + c] *= s.camera_signs[a];
        return out;
    };
    const auto same = [](const std::array<float, camera_floats>& a, const std::array<float, camera_floats>& b) {
        for (std::size_t k = 0; k < camera_floats; ++k)
            if (std::abs(a[k] - b[k]) > (k >= 9 ? 0.01f : 0.001f)) return false;
        return true;
    };
    // The one most of `which` hold (newest first, so the newest of any as many), and how many hold it.
    const auto most_held = [&](const std::vector<std::size_t>& which) {
        std::size_t best = which.front(), best_count = 0;
        for (const auto i : which) {
            std::size_t count = 0;
            for (const auto j : which) count += same(held[i], held[j]) ? 1 : 0;
            if (count > best_count) best = i, best_count = count;
        }
        return std::pair{best, best_count};
    };
    // Those close to the camera read from the game.
    std::vector<std::size_t> close_ones;
    std::array<float, 3> close_signs{};
    for (std::size_t i = 0; i < held.size(); ++i) {
        const auto& one = held[i];
        std::array<float, 3> signs{};
        bool close = true;
        for (std::size_t a = 0; a < 3 && close; ++a) {
            float along = 0;
            for (std::size_t c = 0; c < 3; ++c) along += one[a * 3 + c] * camera[a * 3 + c];
            signs[a] = along < 0 ? -1.0f : 1.0f;
            close = along * signs[a] > 0.97f;
        }
        for (std::size_t c = 0; c < 3 && close; ++c) close = std::abs(one[9 + c] - camera[9 + c]) < 2.0f;
        if (!close) continue;
        if (close_ones.empty()) close_signs = signs;
        close_ones.push_back(i);
    }
    if (!close_ones.empty()) {
        s.camera_signs = close_signs;
        camera = turned(held[most_held(close_ones).first]);
        return true;
    }
    return false;
}

// How many frames back the camera the game drew with is among those read from the game (blended between the
// two nearest), or -1 when it is none of them (the replay editor) or they are all the same (a camera at rest).
float drawn_frames_back(const Shared& s, const std::array<float, camera_floats>& drawn) {
    std::array<float, camera_history> off{};
    for (std::size_t k = 0; k < s.cameras_known; ++k) {
        float worst = 0;
        for (std::size_t i = 0; i < camera_floats; ++i)
            worst = std::max(worst, std::abs(drawn[i] - s.cameras[k][i]) * (i >= 9 ? 0.1f : 1.0f)); // (10 cm as a degree's turn, about)
        off[k] = worst;
    }
    if (s.cameras_known < 2) return -1;
    const auto nearest = static_cast<std::size_t>(std::min_element(off.begin(), off.begin() + s.cameras_known) - off.begin());
    const float spread = *std::max_element(off.begin(), off.begin() + s.cameras_known) - off[nearest];
    if (off[nearest] > 0.05f || spread < 1e-4f) return -1;
    // Toward the neighbour it is nearer to, by how near.
    std::size_t other = nearest;
    if (nearest > 0) other = nearest - 1;
    if (nearest + 1 < s.cameras_known && (nearest == 0 || off[nearest + 1] < off[nearest - 1])) other = nearest + 1;
    const float t = off[nearest] / std::max(off[nearest] + off[other], 1e-6f);
    return static_cast<float>(nearest) + (static_cast<float>(other) - static_cast<float>(nearest)) * t;
}

bool record_cover(Shared& s) {
    s.why = "nothing to draw into";
    if (s.failed || !s.colour || !s.main_depth) return false;
    // Our copy of the game's depth, taken this frame as soon as the game had finished it.
    // Else the last frame's, still there: a frame now and then had none (the depth not readable when
    // its batch went), and the snow went off for it; a frame's difference is far less to see (2026-11-11).
    // Up to six frames old: the few a new depth of the same size waits to take over (two missed, three
    // seen), and one more.
    if (!s.depth_copy || s.depth_copied_frame > s.frame || s.frame - s.depth_copied_frame > 6) {
        s.why = "no copy of this frame's depth";
        return false;
    }
    if (s.depth_copied_frame != s.frame) ++s.outcomes["drawn with the last frame's depth"];
    const auto colour_desc = s.colour->GetDesc();
    s.why = "the picture and depth differ in size, shape or state";
    if (colour_desc.Width != s.depth_width || colour_desc.Height != s.depth_height) return false;
    // Ours draws into it: a picture made only for compute (no render target views) is left alone.
    if (!(colour_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
        if (!s.said_target) {
            s.said_target = true;
            logging::printf(logging::Level::info, logging::Channel::graphics,
                            "World cover: not drawn; the world's picture (format %u, flags 0x%x) takes no render target.",
                            static_cast<unsigned>(colour_desc.Format), static_cast<unsigned>(colour_desc.Flags));
        }
        return false;
    }
    // One plain 2D picture, which CopyResource can copy into ours as it is.
    if (colour_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || colour_desc.MipLevels != 1 || colour_desc.DepthOrArraySize != 1 ||
        colour_desc.SampleDesc.Count != 1)
        return false;
    // Only from states a graphics queue's own work leaves it in: never while compute writes it.
    constexpr auto usable = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s.colour_state == D3D12_RESOURCE_STATE_COMMON || (s.colour_state & ~usable) != 0) return false;
    const auto projector = plugin_camera::projector(static_cast<float>(colour_desc.Width), static_cast<float>(colour_desc.Height));
    if (!projector) {
        s.why = "no camera";
        return false;
    }
    if (!build_resources(s, colour_desc) || !build_pipeline(s, s.colour_format) ||
        !build_single(s, "ps_measure", DXGI_FORMAT_R32_FLOAT, s.measure_pipeline))
        return false;
    // What was replaced and the GPU has finished with goes now.
    if (s.fence) {
        const UINT64 done = s.fence->GetCompletedValue();
        std::erase_if(s.retiring, [&](const auto& r) { return r.first <= done; });
    }
    // An allocator the GPU has finished with, or none this frame.
    const std::size_t at = s.next_allocator;
    if (s.fence->GetCompletedValue() < s.allocator_fence[at]) {
        s.why = "no command allocator free";
        return false;
    }
    // What this allocator's last list measured: the GPU is done with it.
    if (s.measure_waiting[at] && s.measure_fresh[at] && s.measure_cpu) take_measurements(s, s.measure_cpu + at * measure_slice);
    s.measure_waiting[at] = false;
    s.why = "recording failed";
    if (FAILED(s.allocators[at]->Reset()) || FAILED(s.list->Reset(s.allocators[at].Get(), nullptr))) return false;
    s.next_allocator = (at + 1) % s.allocators.size();
    // This frame's camera, newest first (the measurements try the first camera_candidates of them).
    {
        const auto& m = projector->m;
        for (std::size_t c = camera_history - 1; c > 0; --c) s.cameras[c] = s.cameras[c - 1];
        s.cameras[0] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]};
        s.cameras_known = std::min(s.cameras_known + 1, camera_history);
    }
    // The camera the picture was drawn with (camera_delay): between the two kept either side of it, blended
    // (its axes made unit again); the oldest kept, until there are enough.
    const float back = std::min(camera_delay, static_cast<float>(s.cameras_known - 1));
    const auto delay = static_cast<std::size_t>(back);
    const std::size_t older = std::min(delay + 1, s.cameras_known - 1);
    const float blend = back - static_cast<float>(delay);
    std::array<float, camera_floats> camera{};
    for (std::size_t k = 0; k < camera_floats; ++k) camera[k] = s.cameras[delay][k] + (s.cameras[older][k] - s.cameras[delay][k]) * blend;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        float* const v = camera.data() + axis * 3;
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length > 1e-6f) v[0] /= length, v[1] /= length, v[2] /= length;
    }
    // The camera the game drew this frame with, where it can be read; else the delayed one above.
    const bool own_camera = game_camera(s, camera);
    s.game_camera_frames += own_camera ? 1 : 0;
    // The places kept clear (the deck) from the same moment as the camera the game drew with, where it is one
    // of those read: a fixed delay showed the ground at the deck's front or back as it turned (2026-11-13).
    if (const float back_found = own_camera ? drawn_frames_back(s, camera) : -1.0f; back_found >= 0) s.places_back = back_found;
    else if (!own_camera) s.places_back = camera_delay;
    if (++s.camera_frames >= 600) {
        if (s.camera_reports < 6) {
            ++s.camera_reports;
            logging::printf(logging::Level::info, logging::Channel::graphics,
                            "World cover: the game's own camera was read in %u of the last 600 frames (else the one %.2f frames back).",
                            s.game_camera_frames, camera_delay);
        }
        s.camera_frames = s.game_camera_frames = 0;
    }

    Settings settings;
    std::vector<Clear> clear;
    const std::uint64_t now = GetTickCount64();
    float stopping_seconds = -1; // how long the snowfall has been stopping, or -1 while it falls
    {
        auto& a = asked();
        std::lock_guard lock(a.mutex);
        settings = a.settings;
        if (now < a.clear_until) clear = a.clear;
        if (a.stopping_since) stopping_seconds = static_cast<float>(now - a.stopping_since) / 1000.0f;
        // The trails, after the cameras and the places: a head (how many points, the box they lie in),
        // then each point (where, width; how strong it still is, fading to nothing over its time).
        std::erase_if(a.trail, [&](const auto& t) { return now - t.when > static_cast<std::uint64_t>(t.fade * 1000); });
        float* const head = reinterpret_cast<float*>(s.camera_cpu + at * camera_slice) + (camera_candidates * 4 + most_clear * place_floats4) * 4;
        std::size_t count = 0;
        std::array<float, 6> box{1e30f, 1e30f, -1e30f, -1e30f, 1e30f, -1e30f}; // min x, min z, max x, max z, min y, max y
        for (const auto& t : a.trail) {
            const float strength = 1.0f - static_cast<float>(now - t.when) / (t.fade * 1000.0f);
            float* const point = head + 8 + count * 8;
            point[0] = t.at[0], point[1] = t.at[1], point[2] = t.at[2], point[3] = t.width;
            point[4] = std::clamp(strength, 0.0f, 1.0f), point[5] = t.tread, point[6] = t.starts ? 1.0f : 0.0f, point[7] = 0;
            box = {std::min(box[0], t.at[0]), std::min(box[1], t.at[2]), std::max(box[2], t.at[0]), std::max(box[3], t.at[2]),
                   std::min(box[4], t.at[1]), std::max(box[5], t.at[1])};
            ++count;
        }
        head[0] = static_cast<float>(count), head[1] = box[0], head[2] = box[1], head[3] = box[2];
        head[4] = box[3], head[5] = box[4], head[6] = box[5], head[7] = 0;
        // The roof map, after the trails (measure_roofs fills it), and where it is measured round.
        float* const roof = head + 8 + most_trail * 8;
        roof[0] = settings.under_roofs ? 0.0f : 1.0f;
        roof[1] = static_cast<float>(roof_grids[0].cells), roof[2] = roof_grids[0].size, roof[3] = static_cast<float>(roof_grids[1].cells);
        roof[4] = roof_grids[1].size, roof[5] = roof[6] = roof[7] = 0;
        if (!settings.under_roofs) {
            std::memcpy(roof + 8, a.roofs[0].data(), a.roofs[0].size() * sizeof(a.roofs[0][0]));
            std::memcpy(roof + 8 + a.roofs[0].size() * 4, a.roofs[1].data(), a.roofs[1].size() * sizeof(a.roofs[1][0]));
        }
        a.eye = {camera[9], camera[10], camera[11]};
        a.eye_known = true;
    }
    // The places as the picture has them: camera_delay back, between the two kept either side, blended
    // where they are the same places. With this tick's, a deck turning was ahead of the deck drawn, and the
    // ground at its front corner took the deck's light dusting (2026-11-13).
    for (std::size_t c = camera_history - 1; c > 0; --c) s.clears[c] = std::move(s.clears[c - 1]);
    s.clears[0] = clear;
    {
        const float places_back = std::min(s.places_back, static_cast<float>(s.cameras_known - 1));
        const auto places_at = static_cast<std::size_t>(places_back);
        const float places_blend = places_back - static_cast<float>(places_at);
        const auto& near_one = s.clears[places_at];
        const auto& far_one = s.clears[std::min(places_at + 1, s.cameras_known - 1)];
        clear = near_one;
        if (far_one.size() == near_one.size()) {
            const auto mix = [&](std::array<float, 3>& to, const std::array<float, 3>& from) {
                for (std::size_t k = 0; k < 3; ++k) to[k] += (from[k] - to[k]) * places_blend;
            };
            const auto unit = [](std::array<float, 3>& v) {
                const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (length > 1e-6f) v[0] /= length, v[1] /= length, v[2] /= length;
            };
            for (std::size_t i = 0; i < clear.size(); ++i) {
                if ((clear[i].length > 0) != (far_one[i].length > 0)) continue;
                mix(clear[i].foot, far_one[i].foot);
                if (clear[i].length > 0) {
                    mix(clear[i].forward, far_one[i].forward), mix(clear[i].up, far_one[i].up);
                    unit(clear[i].up);
                    // Forward square to up again, then unit.
                    const auto& u = clear[i].up;
                    auto& f = clear[i].forward;
                    const float along = f[0] * u[0] + f[1] * u[1] + f[2] * u[2];
                    for (std::size_t k = 0; k < 3; ++k) f[k] -= u[k] * along;
                    unit(f);
                }
            }
        }
    }
    // The candidate cameras for ps_measure, into this command allocator's part of the upload buffer.
    {
        auto* const out = reinterpret_cast<float*>(s.camera_cpu + at * camera_slice);
        for (std::size_t c = 0; c < camera_candidates; ++c) {
            const auto& from = c == 0 ? camera : s.cameras[std::min(c + delay, s.cameras_known - 1)];
            float* const to = out + c * 4 * 4;
            for (std::size_t v = 0; v < 4; ++v) {
                to[v * 4] = from[v * 3], to[v * 4 + 1] = from[v * 3 + 1], to[v * 4 + 2] = from[v * 3 + 2], to[v * 4 + 3] = 0;
            }
        }
        // Each place kept clear: how much snow shows in it (0: none); then a board-shaped one's forward and
        // half its length, its up and half its width (0: an upright cylinder).
        float* const places = out + camera_candidates * 4 * 4;
        for (std::size_t c = 0; c < most_clear; ++c) {
            float* const to = places + c * place_floats4 * 4;
            std::fill(to, to + place_floats4 * 4, 0.0f);
            if (c >= clear.size()) continue;
            to[0] = clear[c].snow;
            if (clear[c].length > 0) {
                for (std::size_t k = 0; k < 3; ++k) to[4 + k] = clear[c].forward[k], to[8 + k] = clear[c].up[k];
                to[7] = clear[c].length * 0.5f;
                to[11] = clear[c].radius;
            }
        }
    }
    // Views: the picture's copy as the target's format reads it, and the game's depth as it is.
    const UINT view_step = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto view = s.views->GetCPUDescriptorHandleForHeapStart();
    view.ptr += SIZE_T{view_step} * views_each * at; // this allocator's own views
    D3D12_SHADER_RESOURCE_VIEW_DESC picture_view{};
    picture_view.Format = s.colour_format;
    picture_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    picture_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    picture_view.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(s.picture.Get(), &picture_view, view);
    view.ptr += view_step;
    D3D12_SHADER_RESOURCE_VIEW_DESC depth_view{};
    depth_view.Format = depth_copy_format;
    depth_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth_view.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(s.depth_copy.Get(), &depth_view, view);
    // Last frame's camera counts (for the flakes' streaks) only if the cover was drawn on the frame
    // before this one.
    const bool have_last = s.last_drawn_frame + 1 == s.frame;
    // The target: the picture. Made without our hook: it takes the lock held here.
    D3D12_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = s.colour_format;
    target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const UINT target_step = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto target = s.targets->GetCPUDescriptorHandleForHeapStart();
    original_rtv.load()(s.device, s.colour, &target_view, target);

    auto* list = s.list.Get();
    const auto before = s.colour_state;
    transition(list, s.colour, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(s.picture.Get(), s.colour);
    transition(list, s.picture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, s.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // A change of the distance held slides the snow's pattern over half a second instead of jumping it.
    if (s.held_near <= 0 || s.shown_near <= 0 || now < s.shown_near_at) {
        s.shown_near = s.held_near;
    } else {
        const float step = std::min(static_cast<float>(now - s.shown_near_at) / 150.0f, 1.0f);
        s.shown_near += (s.held_near - s.shown_near) * step;
        if (std::abs(s.shown_near - s.held_near) < s.held_near * 1e-4f) s.shown_near = s.held_near;
    }
    s.shown_near_at = now;
    // The snow's fall, at the speed asked for now: a change of speed carries on from where the flakes are.
    // Once an hour or so it starts again (as the time it was taken from did), so the GPU's float holds it.
    if (s.fallen_at && now > s.fallen_at) s.fallen += static_cast<double>(std::min<std::uint64_t>(now - s.fallen_at, 250)) / 1000.0 * settings.fall_speed;
    s.fallen_at = now;
    if (s.fallen > 3600.0) s.fallen = 0;
    const auto& m = camera; // right, up, back, eye: three a row
    const float width = static_cast<float>(colour_desc.Width), height = static_cast<float>(colour_desc.Height);
    std::array<float, constant_count> constants{
        m[0], m[1], m[2], projector->focal,
        m[3], m[4], m[5], width,
        m[6], m[7], m[8], height,
        m[9], m[10], m[11], s.shown_near, // the near distance measured (0: not yet, the shader measures its own)
        settings.colour[0], settings.colour[1], settings.colour[2], settings.amount,
        settings.facing, settings.softness, settings.brightness, settings.breakup,
        settings.sparkle, static_cast<float>(s.fallen), settings.snowfall, settings.fall_speed,
        static_cast<float>(std::min(clear.size(), most_clear)), 0, 0, 0, // then each place's height, below
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // the places' feet and radii, below
        s.last_camera[0], s.last_camera[1], s.last_camera[2], have_last ? 1.0f : 0.0f,
        s.last_camera[3], s.last_camera[4], s.last_camera[5], 0,
        s.last_camera[6], s.last_camera[7], s.last_camera[8], settings.left,
        s.last_camera[9], s.last_camera[10], s.last_camera[11], stopping_seconds};
    for (std::size_t i = 0; i < clear.size() && i < most_clear; ++i) {
        constants[29 + i] = clear[i].height;
        constants[32 + i * 4] = clear[i].foot[0];
        constants[33 + i * 4] = clear[i].foot[1];
        constants[34 + i * 4] = clear[i].foot[2];
        constants[35 + i * 4] = clear[i].radius;
    }
    s.last_camera = camera;
    ID3D12DescriptorHeap* heaps[] = {s.views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(s.root.Get());
    list->SetPipelineState(s.pipeline.Get());
    list->SetGraphicsRoot32BitConstants(0, constant_count, constants.data(), 0);
    auto table = s.views->GetGPUDescriptorHandleForHeapStart();
    table.ptr += UINT64{view_step} * views_each * at;
    list->SetGraphicsRootDescriptorTable(1, table);
    list->SetGraphicsRootShaderResourceView(2, s.camera_upload->GetGPUVirtualAddress() + at * camera_slice);
    const D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(colour_desc.Width), static_cast<LONG>(colour_desc.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    // The measurements, with the same constants and views, copied out for the CPU (take_measurements).
    {
        auto measure_target = s.targets->GetCPUDescriptorHandleForHeapStart();
        measure_target.ptr += SIZE_T{target_step} * 4;
        D3D12_RENDER_TARGET_VIEW_DESC measure_view{};
        measure_view.Format = DXGI_FORMAT_R32_FLOAT;
        measure_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        original_rtv.load()(s.device, s.measure.Get(), &measure_view, measure_target);
        transition(list, s.measure.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->SetPipelineState(s.measure_pipeline.Get());
        const D3D12_VIEWPORT measure_viewport{0, 0, static_cast<float>(measure_columns), static_cast<float>(camera_candidates), 0, 1};
        const D3D12_RECT small_scissor{0, 0, static_cast<LONG>(measure_columns), static_cast<LONG>(camera_candidates)};
        list->RSSetViewports(1, &measure_viewport);
        list->RSSetScissorRects(1, &small_scissor);
        list->OMSetRenderTargets(1, &measure_target, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        transition(list, s.measure.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = s.measure_readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset = at * measure_slice;
        to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, measure_columns, static_cast<UINT>(camera_candidates), 1, measure_pitch};
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = s.measure.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        s.measure_waiting[at] = true;
        s.measure_fresh[at] = s.depth_copied_frame == s.frame;
    }

    transition(list, s.colour, D3D12_RESOURCE_STATE_RENDER_TARGET, before);
    transition(list, s.picture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (FAILED(list->Close())) return false;
    s.last_drawn_frame = s.frame;
    s.why = nullptr;
    return true;
}

bool depth_shaped(const D3D12_RESOURCE_DESC& d, std::uint32_t width, std::uint32_t height) {
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || d.SampleDesc.Count != 1 ||
        d.DepthOrArraySize != 1 || !width || !height)
        return false;
    if (d.Format != DXGI_FORMAT_R32G8X24_TYPELESS && d.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT && d.Format != DXGI_FORMAT_R32_TYPELESS &&
        d.Format != DXGI_FORMAT_D32_FLOAT)
        return false;
    // The screen's shape (a render scale below 100% gives a smaller one), at least half its width:
    // the shadow maps are square.
    const double screen = static_cast<double>(width) / height, ours = static_cast<double>(d.Width) / d.Height;
    return std::abs(ours - screen) < 0.02 * screen && d.Width * 2 >= width;
}

} // namespace

void set(const Settings& settings) noexcept {
    auto& a = asked();
    {
        std::lock_guard lock(a.mutex);
        if (settings.snowfall_stopping && !a.stopping_since) a.stopping_since = GetTickCount64();
        if (!settings.snowfall_stopping) a.stopping_since = 0;
        a.settings = settings;
    }
    wanted.store(true);
}
void clear() noexcept { wanted.store(false); }
void add_trail(const std::array<float, 3>& at, float width, float fade, float tread, bool starts) noexcept {
    try {
        auto& a = asked();
        std::lock_guard lock(a.mutex);
        if (!a.trail.empty() && !starts) {
            const auto& last = a.trail.back().at;
            const float dx = at[0] - last[0], dy = at[1] - last[1], dz = at[2] - last[2];
            if (dx * dx + dy * dy + dz * dz < 0.05f * 0.05f) return;
        }
        a.trail.push_back({at, width, fade, tread, starts, GetTickCount64()});
        while (a.trail.size() > most_trail) a.trail.pop_front();
    } catch (...) {}
}
void measure_roofs(GroundTop ground_top) noexcept {
    try {
        if (!ground_top || !wanted.load()) return;
        auto& a = asked();
        std::array<float, 3> eye{};
        std::array<std::size_t, 2> next{};
        {
            std::lock_guard lock(a.mutex);
            if (a.settings.under_roofs || !a.eye_known) return;
            eye = a.eye, next = a.roof_next;
        }
        // Each grid's window of cells round the camera, measured in turn: the highest surface straight down
        // over each cell's middle. Nothing there (collision not loaded, or open sky to below) is no roof.
        std::array<std::vector<std::pair<std::size_t, std::array<float, 4>>>, 2> found;
        for (std::size_t g = 0; g < roof_grids.size(); ++g) {
            const auto& grid = roof_grids[g];
            const int first_x = static_cast<int>(std::floor(eye[0] / grid.size)) - grid.cells / 2;
            const int first_z = static_cast<int>(std::floor(eye[2] / grid.size)) - grid.cells / 2;
            const auto cells = roof_cells_of(grid);
            found[g].reserve(static_cast<std::size_t>(grid.per_tick));
            for (int k = 0; k < grid.per_tick; ++k) {
                const auto n = (next[g] + static_cast<std::size_t>(k)) % cells;
                const int cell_x = first_x + static_cast<int>(n % static_cast<std::size_t>(grid.cells));
                const int cell_z = first_z + static_cast<int>(n / static_cast<std::size_t>(grid.cells));
                const float x = (static_cast<float>(cell_x) + 0.5f) * grid.size, z = (static_cast<float>(cell_z) + 0.5f) * grid.size;
                const auto top = ground_top(x, z, eye[1] + roof_reach, eye[1] - roof_reach);
                const auto slot = static_cast<std::size_t>(((cell_x % grid.cells) + grid.cells) % grid.cells +
                                                           (((cell_z % grid.cells) + grid.cells) % grid.cells) * grid.cells);
                found[g].push_back({slot, {top ? *top : -1e30f, static_cast<float>(cell_x), static_cast<float>(cell_z), 1.0f}});
            }
            next[g] = (next[g] + static_cast<std::size_t>(grid.per_tick)) % cells;
        }
        std::lock_guard lock(a.mutex);
        for (std::size_t g = 0; g < found.size(); ++g)
            for (const auto& [slot, cell] : found[g]) a.roofs[g][slot] = cell;
        a.roof_next = next;
    } catch (...) {}
}
void keep_clear(std::vector<Clear> places) noexcept {
    try {
        if (places.size() > most_clear) places.resize(most_clear);
        auto& a = asked();
        std::lock_guard lock(a.mutex);
        a.clear = std::move(places);
        a.clear_until = a.clear.empty() ? 0 : GetTickCount64() + 300;
    } catch (...) {}
}
void allow(bool playing) noexcept {
    const std::uint64_t now = GetTickCount64();
    static std::uint64_t last_allowed{}; // when the cover was last allowed
    static std::uint64_t stopped_since{}; // when the level stopped being in play (0: it is)
    if (!playing) {
        // Off only once it has been so for a quarter of a second: the game said so for a single frame
        // now and then, and the cover went off, and its depth was given up and found again, so the snow
        // blinked out for a moment (2026-11-12). A menu opened is off a moment later.
        if (!stopped_since) stopped_since = now;
        if (allowed.load() && now - stopped_since < 250) return;
        playing_since = 0;
        allowed.store(false);
        return;
    }
    stopped_since = 0;
    // Back within a few seconds of being drawn (a teleport, a menu opened and closed): at once. The
    // wait is for the front page and a level loading, which take longer; a teleport took the snow
    // away for two seconds (2026-11-11).
    if (!playing_since) playing_since = last_allowed && now - last_allowed < 5000 ? now - 2000 : now;
    const bool yes = now - playing_since >= 2000;
    if (yes) last_allowed = now;
    allowed.store(yes);
}
bool on() noexcept { return wanted.load(); }

void install(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) noexcept {
    if (!device || !queue || !list) return;
    start_compiling(); // ready long before the snow comes on
    auto& s = shared();
    {
        std::lock_guard lock(s.mutex);
        s.device = device;
        s.queue = queue;
    }
    static std::atomic<bool> hooked{};
    if (hooked.exchange(true)) return;
    // Each function is hooked where it is; where its code cannot be (Proton's D3D12, vkd3d-proton, refused
    // CopyDescriptors on a Steam Deck, 2026-11-13, and the cover never came on), its slot in the
    // interface's table is pointed at ours instead: calls through the interface, which are what the game
    // makes, come to us the same.
    const auto hook = [](void** table, std::size_t slot, void* detour, auto& original, const char* name) {
        using Fn = std::remove_reference_t<decltype(original.load())>;
        void* const target = table[slot];
        void* trampoline{};
        if (hook_prepare(target, detour, &trampoline) == HookOk) {
            original.store(reinterpret_cast<Fn>(trampoline));
            if (hook_enable(target) == HookOk) return true;
            original.store(nullptr);
        }
        void* previous{};
        if (patch_table_slot(name, &table[slot], target, detour, &previous) == HookOk) {
            original.store(reinterpret_cast<Fn>(previous));
            logging::printf(logging::Level::info, logging::Channel::graphics, "World cover: %s is followed through its table slot.", name);
            return true;
        }
        logging::printf(logging::Level::warning, logging::Channel::graphics, "World cover: %s hook could not be put in place.", name);
        return false;
    };
    auto* const device_table = *reinterpret_cast<void***>(device);
    auto* const list_table = *reinterpret_cast<void***>(list);
    auto* const queue_table = *reinterpret_cast<void***>(queue);
    const bool ok = hook(device_table, device_create_rtv, reinterpret_cast<void*>(&hooked_rtv), original_rtv, "CreateRenderTargetView") &&
                    hook(device_table, device_create_dsv, reinterpret_cast<void*>(&hooked_dsv), original_dsv, "CreateDepthStencilView") &&
                    hook(device_table, device_copy_descriptors, reinterpret_cast<void*>(&hooked_copy), original_copy, "CopyDescriptors") &&
                    hook(device_table, device_copy_simple, reinterpret_cast<void*>(&hooked_copy_simple), original_copy_simple, "CopyDescriptorsSimple") &&
                    hook(list_table, list_set_targets, reinterpret_cast<void*>(&hooked_targets), original_targets, "OMSetRenderTargets") &&
                    hook(queue_table, queue_execute, reinterpret_cast<void*>(&hooked_execute), original_execute, "ExecuteCommandLists");
    // The game's own camera (game_camera): without it, the delayed camera is used, as before.
    if (ok) (void)hook(list_table, list_compute_root_cbv, reinterpret_cast<void*>(&hooked_compute_cbv), original_compute_cbv,
                       "SetComputeRootConstantBufferView");
    // Render passes (ID3D12GraphicsCommandList4) bind targets too; only when the runtime's lists have them.
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (ok && SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))) && list4) {
        auto* const table4 = *reinterpret_cast<void***>(list4.Get());
        (void)hook(table4, list_begin_render_pass, reinterpret_cast<void*>(&hooked_render_pass), original_render_pass, "BeginRenderPass");
    }
    if (!ok) say_failed("its D3D12 hooks are not all in place.");
    else logging::write(logging::Level::info, logging::Channel::graphics, "World cover: hooks installed.");
}

void present(std::uint32_t width, std::uint32_t height) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        const bool ready = original_execute.load() && original_targets.load() && !s.failed && s.queue && !retired.load();
        active.store(ready && wanted.load() && allowed.load());
        if (!active.load()) {
            s.notes.clear();
            // The world's depth is found again when the cover is back: a level loaded meanwhile has a
            // new one, and the old one is the game's to free.
            if (s.main_depth) {
                if (s.main_depth_hold) s.retiring.push_back({s.fence_value, s.main_depth_hold});
                s.main_depth_hold.Reset();
                s.main_depth = nullptr;
                s.candidate = nullptr;
                s.candidate_run = 0;
                main_depth_now.store(nullptr);
                for (auto& handle : depth_handles) handle.store(0);
                s.depth_state_known = false;
                s.depth_width = s.depth_height = 0;
                s.main_missed = 0;
                s.colour = nullptr;
                s.depth_copied_frame = ~std::uint64_t{0};
            }
            if (s.fence) {
                const UINT64 done = s.fence->GetCompletedValue();
                std::erase_if(s.retiring, [&](const auto& r) { return r.first <= done; });
            }
            return;
        }
        if (s.pending && !s.drawn && !s.said_late) {
            s.said_late = true;
            logging::write(logging::Level::info, logging::Channel::graphics,
                           "World cover: the world's last drawing was in the frame's last batch, so the cover had no place before post-processing.");
        }
        s.width = width, s.height = height;
        s.main_missed = s.main_seen ? 0 : s.main_missed + 1;
        // How this frame went, counted; every 600 frames (the first eight times) said in the log.
        if (s.main_depth) {
            ++s.outcomes[s.drawn ? "drawn" : !s.tried ? "the frame's last batch (to the screen) not seen" : (s.why ? s.why : "not drawn")];
            if (++s.outcome_frames >= 600) {
                if (s.outcome_reports < 8) {
                    ++s.outcome_reports;
                    std::string line = "World cover: the last 600 frames:";
                    for (const auto& [what, count] : s.outcomes) line += std::format(" {} {};", what, count);
                    logging::write(logging::Level::debug, logging::Channel::graphics, line);
                }
                s.outcomes.clear();
                s.outcome_frames = 0;
            }
        }
        s.tried = false;
        s.why = nullptr;
        // The picture the cover goes into: voted for over 30 frames, then kept while it is still written.
        if (s.frame_last) {
            ++s.votes[s.frame_last].count;
            ++s.frames_voted;
        }
        if (s.locked) {
            s.locked_missed = s.locked_seen ? 0 : s.locked_missed + 1;
            if (s.locked_missed > 120) {
                logging::write(logging::Level::info, logging::Channel::graphics, "World cover: the world's picture is no longer written; choosing again.");
                s.locked = 0;
                colour_now.store(nullptr);
                s.votes.clear();
                s.frames_voted = 0;
            }
        } else if (s.frames_voted >= 30) {
            const auto best = std::max_element(s.votes.begin(), s.votes.end(),
                                               [](const auto& a, const auto& b) { return a.second.count < b.second.count; });
            if (best != s.votes.end() && best->second.count >= 20 && best->second.format != DXGI_FORMAT_UNKNOWN) {
                s.locked = best->first;
                s.colour_format = best->second.format;
                s.locked_missed = 0;
                s.said_colour = true;
                logging::printf(logging::Level::debug, logging::Channel::graphics,
                                "World cover: the world's picture is the one written last after the depth in %u of %u frames (format %u).", best->second.count,
                                s.frames_voted, static_cast<unsigned>(best->second.format));
            }
            s.votes.clear();
            s.frames_voted = 0;
        }
        s.frame_last = 0;
        s.locked_seen = false;
        s.pending = s.drawn = s.depth_elsewhere = s.unsafe = s.main_seen = false;
        s.colour_state_known = false;
        ++s.frame;
        // Notes left from lists recorded but never submitted would otherwise grow without end.
        if (s.notes.size() > 4096) s.notes.clear();
        // Wanted for ten seconds or so and never drawn: say what of the frame was found.
        if (!s.said_drawn && !s.said_waiting && ++s.presents_wanted == 600) {
            s.said_waiting = true;
            logging::printf(logging::Level::info, logging::Channel::graphics,
                            "World cover: not drawn yet; world depth %s, world picture %s, depth views known %u, render target views known %zu.",
                            s.main_depth ? "found" : "not found", s.said_colour ? "found" : "not found",
                            static_cast<unsigned>(std::count_if(depth_handles.begin(), depth_handles.end(), [](const auto& h) { return h.load() != 0; })),
                            s.rtvs.size());
        }
    } catch (...) {}
}

void release() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        active.store(false);
        // The overlay waits for the GPU before it tears down; ours went on the same queue.
        our_list.store(nullptr);
        s.list.Reset();
        s.camera_upload.Reset();
        s.camera_cpu = nullptr;
        for (auto& allocator : s.allocators) allocator.Reset();
        s.allocator_fence = {};
        s.fence.Reset();
        s.fence_value = 0;
        s.picture.Reset();
        s.retiring.clear(); // the overlay waits for the GPU before it tears down
        s.depth_copy.Reset();
        s.depth_pipeline.Reset();
        s.measure.Reset();
        s.measure_readback.Reset();
        s.measure_cpu = nullptr;
        s.measure_pipeline.Reset();
        s.measure_waiting = {};
        s.measure_fresh = {};
        s.relearning = false;
        s.cameras_known = 0;
        s.clears = {};
        s.nears.clear();
        s.held_near = 0;
        s.shown_near = 0;
        s.near_disagreed = 0;
        s.measured_frames = 0;
        s.depth_copied_frame = ~std::uint64_t{0};
        s.chosen_format = DXGI_FORMAT_UNKNOWN;
        s.last_drawn_frame = ~std::uint64_t{0};
        s.views.Reset();
        s.targets.Reset();
        s.pipeline.Reset();
        s.root.Reset();
        s.pipeline_format = DXGI_FORMAT_UNKNOWN;
        s.notes.clear();
        s.rtvs.clear();
        s.dsvs.clear();
        s.device = nullptr;
        s.queue = nullptr;
        s.colour = nullptr;
        s.main_depth = nullptr;
        s.candidate = nullptr;
        s.candidate_run = 0;
        s.main_depth_hold.Reset();
        main_depth_now.store(nullptr);
        s.depth_state_known = false;
        s.failed = false;
    } catch (...) {}
}

void on_barrier(ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& b, BarrierFn /*original*/) noexcept {
    if (!active.load(std::memory_order_relaxed) || list == our_list.load(std::memory_order_relaxed)) return;
    try {
        auto* const colour = colour_now.load(std::memory_order_relaxed);
        // Memory aliased onto (or off) the world's colour: it is not ours to touch this frame.
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            if (colour && (b.Aliasing.pResourceBefore == colour || b.Aliasing.pResourceAfter == colour)) {
                auto& s = shared();
                std::lock_guard lock(s.mutex);
                s.notes[list].unsafe = true;
            }
            return;
        }
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) return;
        auto* const resource = b.Transition.pResource;
        // The frame's last list (Notes::composite): the screen made a render target, and the finished
        // scene's float picture moved from being written to being read on the same list.
        if (list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE) {
            const bool to_screen = b.Transition.StateBefore == D3D12_RESOURCE_STATE_PRESENT && (b.Transition.StateAfter & D3D12_RESOURCE_STATE_RENDER_TARGET);
            const bool finished = (b.Transition.StateBefore & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) &&
                                  (b.Transition.StateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
                                  !(b.Transition.StateAfter & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            if (to_screen || finished) {
                const auto d = resource->GetDesc();
                auto& s = shared();
                std::lock_guard lock(s.mutex);
                if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1) {
                    auto& n = s.notes[list];
                    if (to_screen && d.Width == s.width && d.Height == s.height) n.composite = true;
                    const DXGI_FORMAT f = hdr_view_format(d.Format);
                    if (finished && !n.final_picture && f != DXGI_FORMAT_UNKNOWN && d.Width == s.depth_width && d.Height == s.depth_height &&
                        d.MipLevels == 1 && d.DepthOrArraySize == 1) {
                        n.final_picture = resource;
                        n.final_before = b.Transition.StateBefore;
                        n.final_format = f;
                    }
                }
            }
        }
        constexpr auto writes = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        constexpr auto reads = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_COPY_SOURCE;
        const bool written = (b.Transition.StateBefore & writes) && (b.Transition.StateAfter & reads) && !(b.Transition.StateAfter & writes);
        // The world's colour changing state on this list: where it ends up when the list is submitted.
        if (resource == colour) {
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                // Another queue works on the picture this frame, unsynchronised with a list of ours.
                s.unsafe = true;
                return;
            }
            auto& n = s.notes[list];
            if (!n.colour) n.colour = resource;
            // Half-way through a split barrier the picture is in no state at all.
            if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) n.unsafe = true;
            if (written) {
                n.world = true; // the picture was drawn into on this list
                n.last_written = s.locked;
            }
            n.colour_state_known = true;
            n.colour_state = b.Transition.StateAfter;
            return;
        }
        // The world's lit picture as the game finishes writing it, drawn into or by compute (the game
        // lights the world in compute: no float target is ever bound with the world's depth, 2026-11-10):
        // a full-size, floating-point picture going from being written to being read. The last one
        // written before a batch with none is the one the cover goes into: still before the game's
        // tone mapping (its output is not floating point) and its UI.
        if (written && b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE && resource != main_depth_now.load(std::memory_order_relaxed) &&
            list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            const auto desc = resource->GetDesc();
            const DXGI_FORMAT format = hdr_view_format(desc.Format);
            if (format == DXGI_FORMAT_UNKNOWN || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 ||
                desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
                return;
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            if (!s.depth_width || desc.Width != s.depth_width || desc.Height != s.depth_height) return;
            // Its kind: format, flags, and whether it was written by compute or drawn into.
            const std::uint64_t kind = std::uint64_t{static_cast<std::uint32_t>(format)} | (std::uint64_t{static_cast<std::uint32_t>(desc.Flags)} << 8) |
                                       ((b.Transition.StateBefore & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ? (std::uint64_t{1} << 40) : 0);
            auto& n = s.notes[list];
            n.last_written = kind;
            if (s.votes.size() < 16 || s.votes.contains(kind)) s.votes[kind].format = format;
            // One of the kind the cover goes into, just written: this frame's picture.
            if (s.locked && kind == s.locked) {
                n.world = true;
                n.colour = resource;
                n.colour_state_known = true;
                n.colour_state = b.Transition.StateAfter;
                colour_now.store(resource);
            }
            if (s.pictures_said.size() < 8 && s.pictures_said.insert(resource).second)
                logging::printf(logging::Level::debug, logging::Channel::graphics,
                                "World cover: a full-size float picture written (%llux%u, format %u, flags 0x%x, state 0x%x to 0x%x).",
                                static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.Flags),
                                static_cast<unsigned>(b.Transition.StateBefore), static_cast<unsigned>(b.Transition.StateAfter));
            return;
        }
        // The main depth buffer: where it goes, so our list knows whether it can read it as it is. Nothing
        // is ever recorded on the game's lists (a copy of the depth put on them hung the GPU).
        if (resource == main_depth_now.load(std::memory_order_relaxed)) {
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            s.main_seen = true;
            if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                s.depth_elsewhere = true; // another queue moves it: not this frame
                return;
            }
            auto& n = s.notes[list];
            if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) n.unsafe = true;
            n.depth_state_known = true;
            n.depth_state = b.Transition.StateAfter;
            return;
        }
        // Which depth buffer is the world's: the first screen-shaped one each frame that goes from being
        // written to being read, as the depth tap finds it.
        if (!(b.Transition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE)) return;
        constexpr auto read = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (!(b.Transition.StateAfter & read) || b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) return;
        // The whole buffer, or its depth plane alone (subresource 0): the game moves the planes apart.
        if (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != 0) return;
        if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        if (!s.device) return;
        const auto desc = resource->GetDesc();
        if (!depth_shaped(desc, s.width, s.height)) return;
        // The largest such buffer is the world's: the game keeps smaller, screen-shaped ones too (a
        // 960x540 one at 1080p, 2026-11-10). A smaller one takes over only once the one known has
        // not been seen for 30 presents (the screen got smaller).
        // One of the same size takes over once the one known has been missed for two presents and it has
        // been seen in three frames in a row: the game makes the world's depth anew when some of its
        // settings change (the shadow and sky caches, held off after a change of time of day), and the
        // snow went off for the 30 presents it waited. Taken after two alone, a depth the game used for
        // half a second meanwhile was taken first, and the snow thinned out until the real one came
        // (2026-11-12).
        const bool same_size = desc.Width == s.depth_width && desc.Height == s.depth_height;
        if (s.main_depth && same_size && resource != s.main_depth) {
            if (resource != s.candidate) s.candidate = resource, s.candidate_run = 1;
            else if (s.candidate_frame + 1 == s.frame) ++s.candidate_run;
            else if (s.candidate_frame != s.frame) s.candidate_run = 1;
            s.candidate_frame = s.frame;
            if (s.main_missed < 2 || s.candidate_run < 3) return;
        } else if (s.main_depth && desc.Width * desc.Height <= UINT64{s.depth_width} * s.depth_height && s.main_missed < 30) {
            return;
        }
        s.candidate = nullptr;
        s.candidate_run = 0;
        // None yet (the cover was resting): the screen's own size first; the 960x540 one came by first
        // and the cover went into a picture of another size until the big one took over (2026-11-11).
        if (!s.main_depth && desc.Width < s.width && s.main_missed < 30) return;
        // A new main depth buffer (a resize, a level): its views are looked for afresh.
        if (s.main_depth_hold) s.retiring.push_back({s.fence_value, s.main_depth_hold});
        s.main_depth_hold = resource;
        s.main_depth = resource;
        main_depth_now.store(resource);
        s.depth_state_known = false;
        s.main_seen = true;
        s.main_missed = 0;
        for (auto& handle : depth_handles) handle.store(0);
        for (const auto& [handle, view] : s.dsvs)
            if (view.resource == resource) remember_depth_handle(handle);
        s.depth_width = static_cast<std::uint32_t>(desc.Width);
        s.depth_height = desc.Height;
        logging::printf(logging::Level::info, logging::Channel::graphics,
                        "World cover: the world's depth is %llux%u, format %u, %u mip level(s), barrier on subresource %s, now in state 0x%x.",
                        static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.MipLevels),
                        b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? "all" : "0",
                        static_cast<unsigned>(b.Transition.StateAfter));
        auto& n = s.notes[list];
        n.depth_state_known = true;
        n.depth_state = b.Transition.StateAfter;
    } catch (...) {}
}
} // namespace dingosdk::world_cover
