#include "world_shader.h"
#include "screen_shader_vs_dxil.h"
#include "plugin_shader_compile.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Hooks/patches.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Plugins/plugin_camera.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

// world_shader.h says what this does and how it finds its moment in the game's frame. Made from the world cover the
// Snow Skate mod was first drawn with: the same way into the game's frame, with the plugin's own shader in place of
// the cover's, and its data and the ground heights in place of the cover's trails, places and roof map.
namespace dingosdk::world_shader {
namespace {
using Microsoft::WRL::ComPtr;

// Slots of the D3D12 interfaces' tables (d3d12.h's declaration order).
constexpr std::size_t queue_execute = 10;
// The cbuffer (prelude): 22 float4s, in a part of its own of the upload buffer.
constexpr std::size_t cbuffer_floats = 23 * 4; // (the prelude's cbuffer: reskate_light last)
constexpr UINT64 cbuffer_bytes = 512;
// Our copy of the game's depth, taken as soon as the game has finished it each frame (snapshot_depth):
// read at the shader's moment the game's own had sometimes been used for something else by then, and
// the snow vanished for a frame or came out in stripes (2026-11-11).
constexpr DXGI_FORMAT depth_copy_format = DXGI_FORMAT_R32_FLOAT;
// Each command allocator's own shader views in our heap (a list in flight reads its own): the
// picture's copy and our depth copy. Each frame's cover is drawn afresh: blended with last frame's,
// found with the camera as the client last read it (a frame apart from the GPU's at times), everything
// that moved left an outline of itself in the snow (2026-11-10).
constexpr UINT views_each = 2;
// The world is found with the camera of 1.15 frames before this one (between the two before, blended): the
// camera is read as the shader is drawn, and the game is about that far ahead of its picture by then. With this
// frame's, world-fixed things swam as the camera moved. Tried in the game (2026-11-13): two frames before was better
// than none or one in whole frames, and finer, 1.1 to 1.25 was best; the lag varies a little with the frame rate.
constexpr float camera_delay = 1.15f;
constexpr std::size_t camera_history = 3; // the cameras kept, newest first: enough for camera_delay
constexpr std::size_t camera_floats = 12; // right, up, back, eye
// The ground heights (world_shader.h, heights_layout): measured cells round the camera are kept where their world
// position falls, so they stay as the camera moves and only the new ones are measured. The near grid is fine and
// refreshed often, the far one coarse: one grid 48 m each way showed a roof's edge move as the camera came near it.
struct HeightGrid {
    int cells;     // across, each way
    float size;    // a cell, metres
    int new_tick;  // rays a tick for cells not measured yet, the nearest first
    int again_tick; // and for measuring the others again, in turn (what changes: little)
};
constexpr std::array<HeightGrid, 2> height_grids{{
    {heights_layout.near_cells, heights_layout.near_size, 48, 8},
    {heights_layout.far_cells, heights_layout.far_size, 24, 4},
}};
// And at most this long a tick: the rays run on the game's update thread, and on a Steam Deck (Proton) the game ran in
// slow motion with them at more (2026-11-14).
constexpr double height_tick_budget_ms = 1.0;
std::atomic<std::uint64_t> height_us_total{}, height_us_most{}, height_ticks{}, height_rays{};
constexpr std::size_t height_cells_of(const HeightGrid& g) { return static_cast<std::size_t>(g.cells) * static_cast<std::size_t>(g.cells); }
// After the grids: for each block of the near grid's cells (height_block across), the highest surface over it and the
// blocks round it (four to a float4): a point under none of those has nothing over it, from one read.
constexpr int height_block = heights_layout.block;
constexpr int height_blocks = heights_layout.near_cells / height_block; // across the near grid
static_assert(heights_layout.near_cells == height_blocks * height_block, "the near grid is whole blocks");
constexpr std::size_t height_floats4 = 2 + height_cells_of(height_grids[0]) + height_cells_of(height_grids[1]) + height_blocks * height_blocks / 4;
constexpr float height_reach = 60.0f; // metres over and under the camera the rays start and end
// Each command allocator's part of the upload buffer: the cbuffer, the plugin's data, the heights.
constexpr UINT64 data_offset = cbuffer_bytes;
constexpr UINT64 heights_offset = data_offset + most_data_bytes;
constexpr UINT64 upload_slice = (heights_offset + height_floats4 * 16 + 255) / 256 * 256;
// The measurements: a column a point (ps_measure), read back by the CPU.
constexpr UINT measure_columns = 32; // the near distance's (ps_measure)
// And the picture's brightness, at a grid of 8 by 4 points (ps_measure, after the near distance's): averaged and eased on
// the CPU into reskate_light.
constexpr UINT light_columns = 32;
constexpr UINT measure_width = measure_columns + light_columns;
constexpr UINT measure_pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr UINT64 measure_slice = UINT64{measure_pitch};
// Command allocators: the depth's copy and the shader each take one a frame, and the GPU runs frames
// behind; with eight a frame now and then found none free and went without the snow (2026-11-11).
constexpr std::size_t allocator_count = 16;
// The draws inside the game's own lists (draw_inside): each its own part of the upload buffer and its own views, after
// the allocators'. Four: the GPU is done with one by the time it comes round again.
constexpr std::size_t inside_ring = 4;
// The measurements' slots in the readback buffer: one each allocator's list, then one each draw inside a game's list.
constexpr std::size_t measure_slots = allocator_count + inside_ring;
// A smaller screen-shaped depth takes the world's place only once the one known has been missed this many presents
// (a render scale below 100%, a smaller screen). At 30, the game's half-size depth (960x540 at 1080p) took over for a
// moment as the time of day's shadow and sky caches came back on (the full one is made anew then), and the near
// distance measured on it shifted the snow (2026-11-15).
constexpr unsigned smaller_after = 300;

using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

std::atomic<ExecuteFn> original_execute{};

// A shader's code: compiled (HLSL, on a thread of its own: it can take seconds) or as given.
struct Program {
    enum class State : std::uint8_t { compiling, ready, refused } state{State::compiling};
    std::string code; // DXBC or DXIL
    bool dxil{};
    std::string why;  // refused: why (said to the plugin, and once in the log)
};

// What a plugin asked for (client thread), and every shader's program, under the mutex.
struct Asked {
    std::mutex mutex;
    std::string owner, name;
    std::uint64_t key{};
    std::array<float, value_count> values{};
    bool heights{};
    std::shared_ptr<const std::string> data;
    std::uint64_t data_version{};
    std::optional<std::array<float, 3>> anchor;
    std::uint64_t anchor_until{}; // set_anchor holds for 300 ms
    std::map<std::uint64_t, std::shared_ptr<Program>> programs;
    std::optional<double> cost_ms; // the shader's average on the GPU, once known
    // The ground heights (measure_heights): a cell's (highest surface over it, x, z, 1 once measured), and where the
    // camera was last drawn from.
    std::array<std::vector<std::array<float, 4>>, 2> heights_cells{std::vector<std::array<float, 4>>(height_cells_of(height_grids[0])),
                                                                   std::vector<std::array<float, 4>>(height_cells_of(height_grids[1]))};
    std::array<std::size_t, 2> heights_next{}; // each grid's next cell of the window round the camera to measure again
    std::vector<float> heights_near_top = std::vector<float>(static_cast<std::size_t>(height_blocks) * height_blocks, 1e30f);
    std::array<int, 2> heights_near_first{}; // the near grid's first cell (x, z) when heights_near_top was worked out
    std::uint64_t heights_version{};         // counted up when any of the heights change
    std::array<float, 3> eye{};
    bool eye_known{};
};
Asked& asked() { static auto* value = new Asked; return *value; }
std::uint64_t now_us() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// A shader is known by its file and content (FNV-1a over both).
std::uint64_t key_of(std::string_view name, std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&](std::string_view part) {
        for (const unsigned char c : part) hash = (hash ^ c) * 1099511628211ull;
        hash = (hash ^ 0xffu) * 1099511628211ull;
    };
    mix(name);
    mix(bytes);
    return hash;
}
// Refuses a shader for the rest of the run, and says so once in the log. Under asked()'s lock.
void refuse(Program& program, std::string_view name, std::string why) {
    if (program.state == Program::State::refused) return;
    program.state = Program::State::refused;
    program.why = std::move(why);
    logging::write(logging::Level::warning, logging::Channel::graphics, std::format("World shader {} is off for this run: {}.", name, program.why));
}
std::atomic<bool> wanted{};
std::atomic<bool> allowed{};     // the world has been in play for a moment (allow)
// The calls followed on the game's lists while a world shader is on (each list's, counted in as it is next reset):
// what the following costs the game's recording threads, which the GPU's budget does not see. Said with the cost.
std::atomic<std::uint64_t> followed_calls{};
std::uint64_t playing_since{};   // client thread: when play began, 0 out of play
bool hdr_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

// What one of the game's command lists did while it was recorded, as far as the shader cares.
struct Notes {
    bool world{};          // drew with the world's depth and an HDR colour target
    ID3D12Resource* colour{};
    bool colour_state_known{};
    D3D12_RESOURCE_STATES colour_state{};
    bool depth_state_known{};
    D3D12_RESOURCE_STATES depth_state{}; // the main depth's state when the list ends
    bool unsafe{};         // a split or aliasing barrier on the colour or depth: no shader this frame
    std::uint64_t last_written{}; // the kind (format, flags, compute or drawn) of the last full-size float picture written on it
    // The frame's last list: the screen goes from PRESENT to a render target on it, and the float
    // picture of the finished scene it puts there (with the UI, from a layer of its own) is moved from
    // being written to being read: that picture, its format, and its state before this list.
    bool composite{};
    ID3D12Resource* final_picture{};
    D3D12_RESOURCE_STATES final_before{}, final_after{};
    DXGI_FORMAT final_format{};
    // On a list that is not the screen's (yet): the last full-size float picture of the kind locked onto moved from being
    // written to being read, and how many were (one, in play and in the replay editor, 2026-11-16).
    ID3D12Resource* last_picture{};
    D3D12_RESOURCE_STATES last_after{};
    DXGI_FORMAT last_format{};
    unsigned finished_count{};
    bool drawn_inside{}; // the plugin's shader was drawn on this list itself (draw_inside)
    std::size_t inside_slot{}; // and in which of the draws inside (inside_ring)
    const char* inside_why{}; // why it was not, where it was looked at
};
struct Shared {
    std::mutex mutex;
    std::unordered_map<ID3D12GraphicsCommandList*, Notes> notes;
    ID3D12CommandQueue* queue{};
    ID3D12Device* device{};
    // The frame: the screen's size, the main depth buffer (last seen), the world colour.
    std::uint32_t width{}, height{};
    ID3D12Resource* main_depth{};
    // A reference of ours on it while it is the world's depth: a level loaded again freed the game's,
    // and the pointer kept was read as a depth buffer after it had gone (a crash as the snowskate deck
    // went on, 2026-11-11). Let go (through `retiring`) whenever the shader rests: menus, loading.
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
    bool pending{};        // the world was drawn in a submitted batch, the shader not yet
    bool drawn{};          // the shader is drawn this frame
    std::uint64_t frame{};
    // Ours: the picture's copy, the target's view, our views, our lists.
    ComPtr<ID3D12Resource> picture;
    D3D12_RESOURCE_DESC picture_desc{};
    // What the GPU may still be using when it is replaced: kept until the fence passes the value the
    // last list that could use it signals. Freed at once, a frame in flight read freed memory and the
    // whole picture came out in stripes (2026-11-11).
    std::vector<std::pair<UINT64, ComPtr<ID3D12Pageable>>> retiring;
    // The format and size of the picture the shader goes into, the first time it goes into one: a final
    // picture of another format is let be (no new copy, pipeline or shader compile in the middle of play).
    DXGI_FORMAT chosen_format{DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3D12Resource> depth_copy;
    ComPtr<ID3D12PipelineState> depth_pipeline;
    std::uint64_t depth_copied_frame{~std::uint64_t{0}}; // the frame our depth copy was taken in
    std::array<float, 12> last_camera{};
    std::optional<plugin_camera::Projector> last_projector; // the last camera read, and when (camera_now)
    std::uint64_t last_projector_at{};
    std::uint64_t last_drawn_frame{~std::uint64_t{0}};
    ComPtr<ID3D12DescriptorHeap> views, targets;
    ComPtr<ID3D12RootSignature> root;
    // The plugin's shader's pipelines, by its program and the picture's format.
    std::map<std::pair<std::uint64_t, DXGI_FORMAT>, ComPtr<ID3D12PipelineState>> pipelines;
    // Eight: the GPU runs frames behind, and with three every other frame found none free (2026-11-10).
    std::array<ComPtr<ID3D12CommandAllocator>, allocator_count> allocators;
    std::array<UINT64, allocator_count> allocator_fence{};
    // The measurements (ps_measure): drawn into `measure`, copied to this allocator's part of the
    // readback buffer, read when the allocator is next used (the GPU is done with it then).
    ComPtr<ID3D12Resource> measure, measure_readback;
    ComPtr<ID3D12PipelineState> measure_pipeline;
    const std::uint8_t* measure_cpu{};
    std::array<bool, measure_slots> measure_waiting{};
    std::array<bool, measure_slots> measure_anchored{}; // whether the near distance was measured in it (an anchor set)
    // Whether that frame was drawn with its own depth: one drawn with an older copy (the camera moved
    // since) measured the near distance too short, and it was held (0.056 m for 0.060, 2026-11-12).
    std::array<bool, measure_slots> measure_fresh{};
    // A draw inside a game's list: the fence value once its list has run (set when the list is submitted, ~0 before).
    std::array<UINT64, inside_ring> inside_fence{};
    std::size_t inside_last_slot{};
    // The picture's brightness (reskate_light): the last measured, and eased over about half a second, and when.
    float light_raw{}, light{};
    bool light_known{};
    float far_light{}; // the open sky's: followed within a second for 15 s, then eased over about 30 s; kept while none is in view
    bool far_known{};
    std::uint64_t far_since_us{}; // when it was first taken
    std::uint64_t light_at_us{};
    // The shader's time on the GPU: a timestamp before and after each allocator's list, read back when it is free,
    // against the budget (world_budget_ms), for the shader it was drawn with.
    ComPtr<ID3D12QueryHeap> timer_heap;
    ComPtr<ID3D12Resource> timer_readback;
    const std::uint64_t* timer_cpu{};
    std::array<bool, allocator_count> timer_waiting{};
    std::array<std::uint64_t, allocator_count> timer_key{};
    UINT64 timer_frequency{};
    double gpu_ms_total{}, gpu_ms_most{};
    unsigned gpu_samples{};
    std::uint64_t budget_key{}; // the shader the sums below are for
    double budget_ms{};
    unsigned budget_frames{};
    unsigned budget_long{}; // its frames over world_frame_most_ms in this stretch (world_long_frames of them: off)
    // The heights' and the data's versions in each allocator's part of the upload buffer: copied only when they changed.
    std::array<std::uint64_t, allocator_count> heights_uploaded{}, data_uploaded{};
    std::array<std::uint64_t, inside_ring> inside_heights_uploaded{}, inside_data_uploaded{};
    std::size_t inside_next{};
    std::uint64_t inside_frame{~std::uint64_t{0}}; // the frame last drawn inside a game's list
    bool relearning{}; // a new camera: the next middle measured is taken, up or down
    // The cameras of this and the last two frames, newest first, and how many are known.
    std::array<std::array<float, camera_floats>, camera_history> cameras{};
    std::size_t cameras_known{};
    unsigned cost_frames{}; // frames since the cost was last said
    std::uint64_t cost_said_frame{}; // the frame it was last said in (the calls followed since are over these frames)
    double frame_us{};             // how long a frame takes, eased (present to present)
    std::uint64_t last_present_us{};
    // The near distance each recent frame measured, and the steady middle of them (0: none yet).
    std::deque<float> nears;
    float held_near{};
    float shown_near{}; // what the shader is given: held_near, eased into over half a second
    std::uint64_t shown_near_at{};
    unsigned near_disagreed{}; // frames in a row far from held_near
    float last_focal{};        // the camera's focal length last frame, and the frame it last changed (a new camera)
    std::uint64_t focal_changed_frame{};
    std::uint64_t measured_frames{};
    std::size_t next_allocator{};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> upload; // a slice a command allocator (upload_slice bytes each)
    std::uint8_t* upload_cpu{};
    double last_seconds{};
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value{};
    bool failed{}, said_drawn{}, said_late{}, said_depth{}, said_waiting{}, said_target{};
    unsigned presents_wanted{}; // presents while the shader was wanted and allowed (for the log)
    std::set<ID3D12Resource*> pictures_said; // full-size float pictures seen written, said once each
    // The game writes more than one full-size float picture a frame. Drawing into whichever came last
    // before the shader's moment made it flicker (2026-11-10), so it locks onto one: the picture written
    // last, after the world's depth is done, in most of 30 frames (the game's final float picture,
    // before its tone mapping and UI).
    struct Vote {
        unsigned count{};
        DXGI_FORMAT format{};
    };
    // By kind, not by picture: the game keeps two of each and takes turns with them a frame each (its
    // anti-aliasing reads last frame's), so one picture locked onto had the shader every other frame.
    std::map<std::uint64_t, Vote> votes;
    std::uint64_t frame_last{}; // the kind of this frame's last picture written after the depth
    std::uint64_t locked{};     // the kind the shader goes into, 0 before one is chosen
    bool locked_seen{};
    unsigned frames_voted{}, locked_missed{};
    // Why frames went without the shader, counted and said in the log now and then.
    const char* why{};          // the reason the last try this frame did not draw
    bool tried{};               // the shader's moment came this frame
    std::map<std::string, unsigned> outcomes;
    unsigned outcome_frames{}, outcome_reports{};
    // The finished scene's picture as a list of this frame's moved it from being written to being read, when that list
    // was not the one that takes the screen to a render target (it came in an earlier batch): the frame it was seen in,
    // the picture, its state after, its format.
    std::uint64_t finished_frame{~std::uint64_t{0}};
    std::uint64_t written_frame{~std::uint64_t{0}}; // the frame the locked kind's picture was last drawn into
    unsigned unlocked_waited{}; // frames the shader waited for the world's picture to be chosen (at most 120, once)
    std::uint64_t active_frames{}; // frames since the shader last came on (it rests out of play, in menus, and off)
    std::vector<std::uint64_t> skipped_at; // the stretch's frames without it, as active_frames counted them (the first 12)
    unsigned depth_copies{}; // copies of the depth taken this frame (more than one: it became readable more than once)
    ID3D12Resource* finished_picture{};
    D3D12_RESOURCE_STATES finished_state{};
    DXGI_FORMAT finished_format{};
    unsigned finished_count{};          // how many of the locked kind that list finished
    ID3D12Resource* finished_first{};   // the first picture it finished (of any kind)
    unsigned finished_said{};           // such frames said in the log (the first 12)
    const char* finished_why{};         // whether the shader was drawn inside that list, or why not
    // The pictures the screen's list has finished (the final scene's: the game takes turns with two), the last four, and
    // the frame each was last finished there. Only one finished there in the last four frames counts: after the game made
    // its pictures anew, one kept from before was the place of another it used for something else (its anti-aliasing's
    // last frame, it seems), the shader was drawn into that, and a square shimmered as the camera moved (2026-11-16).
    std::array<ID3D12Resource*, 4> finals{};
    std::array<std::uint64_t, 4> finals_frame{};
    std::size_t finals_next{};
};
Shared& shared() { static auto* value = new Shared; return *value; }

std::atomic<ID3D12Resource*> main_depth_now{};
std::atomic<ID3D12Resource*> colour_now{};
std::atomic<bool> active{}; // wanted, and installed
// The table of the runtime's command queues (the overlay's own queue's), where ensure_hooks puts its hook.
std::atomic<void**> queue_table{};
// And of its graphics command lists (the overlay's own list's), where the hooks that follow the game's lists go.
std::atomic<void**> list_table{};
std::atomic<bool> lists_followed{}; // all of those are in place
// Our own command list: its barriers go through the same hook as the game's and are not the game's.
std::atomic<ID3D12GraphicsCommandList*> our_list{};

void say_failed(const std::string& why) {
    auto& s = shared();
    if (s.failed) return;
    s.failed = true;
    logging::write(logging::Level::warning, logging::Channel::graphics, "World shaders unavailable: " + why);
}

// ---- hooks ------------------------------------------------------------------------------------------
// A floating-point colour format a view of this format reads as, or UNKNOWN: a typeless view takes
// its family's float format.
DXGI_FORMAT hdr_view_format(DXGI_FORMAT f) {
    if (hdr_format(f)) return f;
    if (f == DXGI_FORMAT_R16G16B16A16_TYPELESS) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (f == DXGI_FORMAT_R32G32B32A32_TYPELESS) return DXGI_FORMAT_R32G32B32A32_FLOAT;
    return DXGI_FORMAT_UNKNOWN;
}

bool record_shader(Shared& s);
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
        // The frame's last batch, which puts the finished scene on the screen with the UI (a layer of its
        // own): the shader goes into the scene's picture just before it, in a list of ours. The game draws
        // almost a whole frame in one command list, so this is the first place after the scene is done
        // (traced 2026-11-10: the screen PRESENT to render target, the 1920x1080 R11G11B10 scene and the
        // RGBA8 UI layer read, the screen back to PRESENT, all in one batch of one list).
        // A list of the batch the shader was drawn on itself (draw_inside): this frame has it.
        for (UINT i = 0; i < count && !s.drawn; ++i) {
            const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (found == s.notes.end() || !found->second.drawn_inside) continue;
            s.drawn = true;
            s.tried = true;
            s.why = nullptr;
            ++s.outcomes["drawn inside the game's list"];
            if (s.finished_said < 12) {
                ++s.finished_said;
                logging::printf(logging::Level::info, logging::Channel::graphics,
                                "World shader: drawn inside the game's list (frame %llu): it finished the picture on the list that drew it.",
                                static_cast<unsigned long long>(s.active_frames));
            }
        }
        const Notes* composite{};
        bool to_screen = false; // a list of the batch takes the screen to a render target
        for (UINT i = 0; i < count && !composite; ++i) {
            const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (found == s.notes.end() || !found->second.composite) continue;
            to_screen = true;
            // The picture the screen's list finished: one the screen takes (the game takes turns with two), kept for
            // draw_inside's choice.
            if (const auto final_picture = found->second.final_picture) {
                const auto known = std::find(s.finals.begin(), s.finals.end(), final_picture);
                if (known != s.finals.end()) {
                    s.finals_frame[static_cast<std::size_t>(known - s.finals.begin())] = s.frame;
                } else {
                    s.finals[s.finals_next] = final_picture;
                    s.finals_frame[s.finals_next] = s.frame;
                    s.finals_next = (s.finals_next + 1) % s.finals.size();
                }
            }
            if (found->second.final_picture) composite = &found->second;
        }
        // The screen's batch without the finished picture's barrier on it: on some frames the game moves the picture
        // to being read in an earlier batch of its own, and the shader went without that frame (a blink, 8 frames in
        // 600 at times, 2026-11-16). The picture an earlier batch of this frame finished is the one, in the state
        // that batch left it in.
        const bool finished_before = !composite && to_screen && s.finished_frame == s.frame && s.finished_picture;
        // And with no finished picture seen at all this frame (8 frames in the first 600 after the shader came on,
        // 2026-11-16): the picture of the kind locked onto, drawn into this frame, in the state the lists so far left it.
        const bool locked_written = !composite && !finished_before && to_screen && s.locked && s.written_frame == s.frame && s.colour &&
                                    s.colour_state_known;
        // Nothing until the world's picture is chosen (30 frames, locked), for at most 120 frames once: drawn into
        // whatever came first, the shader came on and went off again for a few frames as it settled. (Where it never
        // locks, it draws as before after that.)
        if ((composite || finished_before || locked_written) && !s.drawn && !s.locked && s.unlocked_waited < 120) {
            ++s.unlocked_waited;
            s.tried = true;
            s.why = "the world's picture not chosen yet";
        } else if ((composite || finished_before || locked_written) && !s.drawn) {
            s.tried = true;
            ID3D12Resource* const last_written = s.colour; // the picture of the kind locked onto drawn into last (for the log)
            if (!locked_written) {
                s.colour = composite ? composite->final_picture : s.finished_picture;
                s.colour_state = composite ? composite->final_before : s.finished_state;
                s.colour_state_known = true;
            }
            const DXGI_FORMAT final_format = composite ? composite->final_format : finished_before ? s.finished_format : s.colour_format;
            if (s.chosen_format == DXGI_FORMAT_UNKNOWN) s.chosen_format = final_format;
            s.colour_format = s.chosen_format;
            if (finished_before) {
                ++s.outcomes["the picture finished in an earlier batch"];
                if (s.finished_said < 12) {
                    ++s.finished_said;
                    logging::printf(logging::Level::info, logging::Channel::graphics,
                                    "World shader: the picture finished in an earlier batch (frame %llu): %u of the kind locked onto finished on "
                                    "that list; drawn into the last of them (%p, state 0x%x), the first finished was %p, the last written %p; "
                                    "not drawn inside that list: %s.",
                                    static_cast<unsigned long long>(s.active_frames), s.finished_count, static_cast<void*>(s.finished_picture),
                                    static_cast<unsigned>(s.finished_state), static_cast<void*>(s.finished_first), static_cast<void*>(last_written),
                                    s.finished_why ? s.finished_why : "not looked at");
                }
            }
            if (locked_written) ++s.outcomes["the locked picture, none finished seen"];
            s.why = s.unsafe ? "a split or aliasing barrier, or another queue" : s.depth_elsewhere ? "the depth moved on another queue"
                  : final_format != s.chosen_format ? "the final picture in another format" : nullptr;
            const bool drew = !s.why && record_shader(s);
            if (drew) {
                s.drawn = true;
                ID3D12CommandList* ours[] = {s.list.Get()};
                original(queue, 1, ours);
                queue->Signal(s.fence.Get(), ++s.fence_value);
                s.allocator_fence[(s.next_allocator + s.allocators.size() - 1) % s.allocators.size()] = s.fence_value;
                if (!s.said_drawn) {
                    s.said_drawn = true;
                    logging::printf(logging::Level::info, logging::Channel::graphics,
                                    "World shader: drawn into the finished scene (format %u) just before the game puts it on the screen with its UI.",
                                    static_cast<unsigned>(s.colour_format));
                }
            }
        }
        bool depth_finished = false;
        std::array<bool, inside_ring> inside_submitted{}; // the draws inside this batch's lists (their fence, below)
        bool any_inside = false;
        for (UINT i = 0; i < count; ++i) {
            const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (found == s.notes.end()) continue;
            const auto& n = found->second;
            if (n.drawn_inside) inside_submitted[n.inside_slot] = any_inside = true;
            if (n.depth_state_known) {
                s.depth_state_known = true;
                s.depth_state = n.depth_state;
                depth_finished = true; // copied once this batch has run, if it is readable now
            }
            if (n.unsafe) s.unsafe = true;
            // A finished picture on a list that is not the screen's: kept for the screen's batch, if it comes later.
            // The last of the kind locked onto it finished, else the first of any (before one is locked).
            if (n.final_picture && !n.composite) {
                s.finished_frame = s.frame;
                s.finished_picture = n.last_picture ? n.last_picture : n.final_picture;
                s.finished_state = n.last_picture ? n.last_after : n.final_after;
                s.finished_format = n.last_picture ? n.last_format : n.final_format;
                s.finished_count = n.finished_count;
                s.finished_first = n.final_picture;
                s.finished_why = n.drawn_inside ? "drawn inside it" : n.inside_why ? n.inside_why : "not looked at";
            }
            // A picture counts only once the world's depth is done and readable: one written before
            // that is not the lit world (the game writes one then, and the shader could never read the
            // depth at its moment, 2026-11-10). The last one written after it is kept for the vote.
            const bool depth_done = s.depth_state_known && (s.depth_state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
                                    !(s.depth_state & D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (n.last_written && depth_done) s.frame_last = n.last_written;
            if (s.locked && n.last_written == s.locked) s.locked_seen = true;
            if (n.world) {
                s.colour = n.colour;
                s.pending = !s.drawn;
                if (s.locked) s.written_frame = s.frame;
            }
            if (n.colour_state_known && n.colour && n.colour == s.colour) {
                s.colour_state_known = true;
                s.colour_state = n.colour_state;
            }
            s.notes.erase(found);
        }
        lock.unlock();
        original(queue, count, lists);
        if (any_inside) {
            // When the GPU is past this batch, the measurements drawn inside its lists can be read.
            std::lock_guard again(s.mutex);
            if (s.fence) {
                queue->Signal(s.fence.Get(), ++s.fence_value);
                for (std::size_t k = 0; k < inside_ring; ++k)
                    if (inside_submitted[k]) s.inside_fence[k] = s.fence_value;
            }
        }
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

// ReSkate's own shaders beside the plugin's: the full-screen triangle (DXBC; the DXIL build of it is
// screen_shader_vs_dxil.h), the depth's copy (ps_depth), and the near distance's measurements (ps_measure).
constexpr const char* builtin_source = R"hlsl(
cbuffer ReSkateWorld : register(b0) {
    float4 right_focal; float4 up_width; float4 back_height; float4 eye_near;
    float4 last_right; float4 last_up; float4 last_back; float4 last_eye;
    float4 anchor; float4 clock; float4 values[12]; float4 light;
};
Texture2D<float4> picture : register(t0);
Texture2D<float> depth : register(t1);
struct Interpolated { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Interpolated vs_main(uint id : SV_VertexID) {
    Interpolated o;
    const float2 p = float2((id << 1) & 2, id & 2);
    o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = p;
    return o;
}
// The game's depth into ours (t1 is the game's own here).
float ps_depth(Interpolated input) : SV_Target { return depth.Load(int3(int2(input.position.xy), 0)); }
float depth_at(int2 p) {
    p = clamp(p, int2(0, 0), int2(up_width.w, back_height.w) - 1);
    return depth.Load(int3(p, 0));
}
// The near distance the depth is over, at a point the anchor's column goes through: the point's distance along the
// view, times the depth drawn where it shows on screen (depth = near / distance), its surface taken as 0.1 m nearer
// than its middle; 0 off screen.
float measure_at(float3 target) {
    const float3 to = target - eye_near.xyz;
    const float along_view = dot(to, -back_height.xyz);
    if (along_view < 0.3) return 0;
    const float2 at = float2(up_width.w * 0.5 + dot(to, right_focal.xyz) * right_focal.w / along_view,
                             back_height.w * 0.5 - dot(to, up_width.xyz) * right_focal.w / along_view);
    if (any(at < 0) || at.x >= up_width.w || at.y >= back_height.w) return 0;
    const float there = depth_at(int2(at));
    return there > 0 ? (along_view - 0.1) * there : 0;
}
// 32 points up the anchor (a skater standing: from a little under their place to over their head), a few centimetres
// either side of its middle. Those on the anchor agree; those that miss it show something further away. The CPU keeps
// the biggest group that agrees.
float ps_measure(Interpolated input) : SV_Target {
    const uint k = (uint)input.position.x;
    // Past the near distance's: the picture's brightness, at a grid of 8 by 4 points; a point on the open sky given as
    // its brightness made negative, so the CPU can keep the sky's apart (the same whichever way the camera looks). With
    // everything past 120 m counted too, the mix of sky and fogged buildings changed as the camera turned (2026-11-16).
    if (k >= 32u) {
        const uint g = k - 32u;
        const int2 at = int2((float(g % 8u) + 0.5) / 8.0 * up_width.w, (float(g / 8u) + 0.5) / 4.0 * back_height.w);
        const float lum = dot(picture.Load(int3(at, 0)).rgb, float3(0.2126, 0.7152, 0.0722));
        const float d = depth_at(at);
        return !(d > 0) ? -max(lum, 1e-6) : lum;
    }
    if (anchor.w < 0.5) return 0;
    // Not with the camera looking steeply down (or up) the column: its points then lie over each other on screen, on the
    // skater's head and shoulders, nearer than themselves, and enough of them agreed on a near distance too short to be
    // held (a dark ball before the camera as it panned down, until it was learnt again, 2026-11-16).
    if (abs(back_height.y) > 0.6) return 0;
    const float side = (k < 16u ? -0.06 : 0.06);
    return measure_at(anchor.xyz + float3(0, -0.4 + (k % 16u) * 0.13, 0) + right_focal.xyz * side);
}
)hlsl";

bool build_root(Shared& s) {
    if (s.root) return true;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2; // t0 the picture, t1 the depth
    std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b0
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t2: the plugin's data
    parameters[2].Descriptor.ShaderRegister = 2;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t3: the ground heights
    parameters[3].Descriptor.ShaderRegister = 3;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    for (UINT i = 0; i < samplers.size(); ++i) {
        auto& sampler = samplers[i];
        sampler.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        sampler.MaxLOD = 0.0f;
        sampler.ShaderRegister = i;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = static_cast<UINT>(samplers.size());
    description.pStaticSamplers = samplers.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || !blob ||
        FAILED(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.root)))) {
        say_failed("their root signature could not be made.");
        return false;
    }
    return true;
}

// ReSkate's own shaders, compiled once on a thread of their own as soon as the hooks are in (small: well under a
// second). Until they are ready nothing is drawn.
struct Compiled {
    std::mutex mutex;
    std::map<std::string, ComPtr<ID3DBlob>> blobs; // entry -> its shader
    std::string error;
    std::atomic<int> state{}; // 0 not started, 1 compiling, 2 ready, 3 failed
};
Compiled& compiled() { static auto* value = new Compiled; return *value; }
constexpr std::pair<const char*, const char*> builtin_entries[]{{"vs_main", "vs_5_0"}, {"ps_measure", "ps_5_0"}, {"ps_depth", "ps_5_0"}};
void start_compiling() {
    auto& c = compiled();
    int idle = 0;
    if (!c.state.compare_exchange_strong(idle, 1)) return;
    try {
        std::thread([] {
            auto& done = compiled();
            std::map<std::string, ComPtr<ID3DBlob>> made;
            for (const auto& [entry, target] : builtin_entries) {
                ComPtr<ID3DBlob> blob, error;
                if (FAILED(D3DCompile(builtin_source, std::strlen(builtin_source), "world_shader", nullptr, nullptr, entry, target, 0, 0, &blob, &error)) ||
                    !blob) {
                    std::lock_guard lock(done.mutex);
                    done.error = std::string(entry) + (error ? std::string(": ") + static_cast<const char*>(error->GetBufferPointer()) : "");
                    done.state.store(3);
                    return;
                }
                made[entry] = blob;
            }
            {
                std::lock_guard lock(done.mutex);
                done.blobs = std::move(made);
            }
            done.state.store(2);
        }).detach();
    } catch (...) {
        c.state.store(0);
    }
}
// A built-in entry's shader, once compiled. Null while compiling (s.why says so), or when it failed (said once).
ComPtr<ID3DBlob> builtin(Shared& s, const char* entry) {
    auto& c = compiled();
    start_compiling();
    const int state = c.state.load();
    if (state == 3) {
        std::lock_guard lock(c.mutex);
        say_failed("ReSkate's own shaders for them did not compile: " + c.error);
        return nullptr;
    }
    if (state != 2) {
        s.why = "ReSkate's shaders are still compiling";
        return nullptr;
    }
    std::lock_guard lock(c.mutex);
    return c.blobs[entry];
}

// A pipeline of one target in `format`.
ComPtr<ID3D12PipelineState> make_pipeline(Shared& s, D3D12_SHADER_BYTECODE vertex, D3D12_SHADER_BYTECODE pixel, DXGI_FORMAT format) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = s.root.Get();
    description.VS = vertex;
    description.PS = pixel;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(s.device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline)))) return nullptr;
    return pipeline;
}
// A built-in pipeline (ps_depth, ps_measure) of one target in `format`.
bool build_single(Shared& s, const char* entry, DXGI_FORMAT format, ComPtr<ID3D12PipelineState>& pipeline) {
    if (pipeline) return true;
    if (!build_root(s)) return false;
    const auto vertex = builtin(s, "vs_main"), pixel = builtin(s, entry);
    if (!vertex || !pixel) return false;
    pipeline = make_pipeline(s, {vertex->GetBufferPointer(), vertex->GetBufferSize()}, {pixel->GetBufferPointer(), pixel->GetBufferSize()}, format);
    if (!pipeline) {
        say_failed(std::string("their pipeline for ") + entry + " was refused.");
        return false;
    }
    return true;
}
bool build_depth_pipeline(Shared& s) { return build_single(s, "ps_depth", depth_copy_format, s.depth_pipeline); }
// The plugin's shader's pipeline for the picture's format: null while ReSkate's are compiling, or when the driver
// refuses it (the program is refused then). Under asked()'s lock.
ID3D12PipelineState* plugin_pipeline(Shared& s, std::uint64_t key, Program& program, std::string_view name, DXGI_FORMAT format) {
    if (const auto found = s.pipelines.find({key, format}); found != s.pipelines.end()) return found->second.Get();
    if (!build_root(s)) return nullptr;
    D3D12_SHADER_BYTECODE vertex{screen_shader::vertex_dxil, sizeof(screen_shader::vertex_dxil)};
    ComPtr<ID3DBlob> dxbc;
    if (!program.dxil) {
        dxbc = builtin(s, "vs_main");
        if (!dxbc) return nullptr;
        vertex = {dxbc->GetBufferPointer(), dxbc->GetBufferSize()};
    }
    auto pipeline = make_pipeline(s, vertex, {program.code.data(), program.code.size()}, format);
    if (!pipeline) {
        refuse(program, name, "the driver refused it (its bindings, inputs or output are not a world shader's, or its DXIL is not signed)");
        return nullptr;
    }
    return s.pipelines.emplace(std::pair{key, format}, std::move(pipeline)).first->second.Get();
}
// Our own command list, its allocators, fence, heaps and the upload buffer: made once.
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
        s.list->SetName(L"ReSkate world shader");
        our_list.store(s.list.Get());
        D3D12_HEAP_PROPERTIES upload{};
        upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bytes{};
        bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bytes.Width = upload_slice * (s.allocators.size() + inside_ring);
        bytes.Height = 1;
        bytes.DepthOrArraySize = 1;
        bytes.MipLevels = 1;
        bytes.SampleDesc.Count = 1;
        bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* cpu{};
        if (FAILED(s.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&s.upload))) ||
            FAILED(s.upload->Map(0, nullptr, &cpu)) || !cpu) {
            say_failed("no upload buffer.");
            return false;
        }
        s.upload_cpu = static_cast<std::uint8_t*>(cpu);
        D3D12_DESCRIPTOR_HEAP_DESC views{};
        views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        views.NumDescriptors = views_each * static_cast<UINT>(s.allocators.size() + inside_ring);
        views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        D3D12_DESCRIPTOR_HEAP_DESC targets{};
        targets.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        targets.NumDescriptors = 5; // the picture, the picture drawn inside a game's list, (2 free), our depth copy, the measurements
        if (FAILED(s.device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&s.views))) || FAILED(s.device->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&s.targets)))) {
            say_failed("no descriptor heaps.");
            return false;
        }
        // The measurements' target, kept as a copy's source between frames, and where they are read.
        D3D12_HEAP_PROPERTIES gpu{};
        gpu.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC measured{};
        measured.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        measured.Width = measure_width;
        measured.Height = 1;
        measured.DepthOrArraySize = 1;
        measured.MipLevels = 1;
        measured.Format = DXGI_FORMAT_R32_FLOAT;
        measured.SampleDesc.Count = 1;
        measured.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES readback{};
        readback.Type = D3D12_HEAP_TYPE_READBACK;
        bytes.Width = measure_slice * measure_slots;
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
    // Timestamps for the shader's time on the GPU: none, and it is not drawn (it is not drawn unmeasured).
    if (!s.timer_heap && s.queue && SUCCEEDED(s.queue->GetTimestampFrequency(&s.timer_frequency)) && s.timer_frequency) {
        D3D12_QUERY_HEAP_DESC queries{};
        queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queries.Count = static_cast<UINT>(allocator_count * 2);
        D3D12_HEAP_PROPERTIES readback{};
        readback.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bytes{};
        bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bytes.Width = allocator_count * 2 * sizeof(std::uint64_t);
        bytes.Height = 1, bytes.DepthOrArraySize = 1, bytes.MipLevels = 1, bytes.SampleDesc.Count = 1;
        bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* read{};
        if (FAILED(s.device->CreateQueryHeap(&queries, IID_PPV_ARGS(&s.timer_heap))) ||
            FAILED(s.device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&s.timer_readback))) ||
            FAILED(s.timer_readback->Map(0, nullptr, &read)) || !read) {
            s.timer_heap.Reset();
            s.timer_readback.Reset();
            say_failed("their time on the GPU cannot be measured, and they are not drawn unmeasured.");
            return false;
        } else {
            s.timer_cpu = static_cast<const std::uint64_t*>(read);
        }
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
            logging::printf(logging::Level::info, logging::Channel::graphics, "World shader: not drawn; the game's depth (format %u) cannot be read.",
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
    s.device->CreateRenderTargetView(s.depth_copy.Get(), &target_view, target);

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
    ++s.depth_copies;
    return true;
}

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
    // Twenty only within two seconds of the camera's field of view changing (a new camera); else five seconds' worth.
    // The points go straight up from the anchor, and a skater leaning into a steep transition is not there: they all
    // measured the ramp behind, agreed, and were taken for a new camera, and the distance went wrong for a second
    // until it was learnt again (2026-11-15).
    if (!s.relearning && s.held_near > 0 && std::abs(measured - s.held_near) > s.held_near * 0.2f) {
        const bool new_camera = s.frame - s.focal_changed_frame < 240;
        if (++s.near_disagreed < (new_camera ? 20u : 500u)) return;
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
            logging::printf(logging::Level::debug, logging::Channel::graphics, "World shader: near distance %.5f, was %.5f.", *half, s.held_near);
        s.held_near = *half;
    }
    if (++s.measured_frames % 1800 == 300)
        logging::printf(logging::Level::debug, logging::Channel::graphics, "World shader: near distance %.5f (%zu of %u points agree).", s.held_near,
                        best_count, measure_columns);
}

// The picture's brightness from one frame's measurements (ps_measure's grid), eased over about half a second
// (reskate_light), in steps of brightness (as the eye takes them), so a plugin's shader can light with it steadily: from
// one frame's own points, a dark or bright thing passing over them as the camera turned made the light jump (the snow's
// fog and flakes flickered, 2026-11-16).
void take_light(Shared& s, const std::uint8_t* rows) {
    const auto* row = reinterpret_cast<const float*>(rows) + measure_columns;
    double sum = 0, far_sum = 0;
    unsigned n = 0, far_n = 0;
    for (UINT i = 0; i < light_columns; ++i) {
        if (!std::isfinite(row[i]) || std::abs(row[i]) >= 1e6f) continue;
        sum += std::abs(row[i]), ++n;
        if (row[i] < 0) far_sum -= row[i], ++far_n; // (a far point's brightness comes negative)
    }
    if (!n) return;
    const float raw = std::max(static_cast<float>(sum / n), 1e-5f);
    const std::uint64_t now = now_us();
    // The open sky's: taken as it is the first time, then eased over about 30 s, and kept while none is on the screen
    // (looking at the ground): so a fog drawn with it stays the same as the camera turns, and follows only the light's own
    // changes (a time of day, when the world's depth is made anew, takes it afresh). The game's sky is not as bright
    // everywhere (lighter near the horizon), and eased over 2 to 4 s it still rose and fell as the camera panned up and
    // down; with the whole picture's, with what was in view (2026-11-16).
    if (far_n >= 2) {
        const float far_raw = std::max(static_cast<float>(far_sum / far_n), 1e-5f);
        if (!s.far_known) {
            s.far_light = far_raw;
            s.far_known = true;
            s.far_since_us = now;
        } else {
            // For its first 15 s it follows within about a second (the light settling: the snow's weather had only just
            // come on, the sky still dark, and taken then the fog spent minutes getting lighter, 2026-11-16); then over
            // about 30 s.
            const float seconds = std::clamp(static_cast<float>(now - s.light_at_us) / 1e6f, 0.0f, 1.0f);
            const float over = now - s.far_since_us < 15000000 ? 1.0f : 30.0f;
            s.far_light = std::exp(std::log(s.far_light) + (std::log(far_raw) - std::log(s.far_light)) * std::min(seconds / over, 1.0f));
        }
    }
    if (!s.light_known) {
        s.light = raw;
        s.light_known = true;
    } else {
        const float seconds = std::clamp(static_cast<float>(now - s.light_at_us) / 1e6f, 0.0f, 1.0f);
        const float k = std::min(seconds / 0.5f, 1.0f);
        s.light = std::exp(std::log(std::max(s.light, 1e-5f)) + (std::log(raw) - std::log(std::max(s.light, 1e-5f))) * k);
    }
    s.light_raw = raw;
    s.light_at_us = now;
}

// The measurements (ps_measure: the near distance's, then the picture's brightness) drawn into `measure` with the
// constants and views already set for the plugin's shader, and copied to the readback buffer's `slot`. `move` takes a
// resource from one state to another on `list` (our own list's barrier, or straight to a game list's).
template <typename Move>
void record_measure(Shared& s, ID3D12GraphicsCommandList* list, std::size_t slot, Move&& move) {
    auto measure_target = s.targets->GetCPUDescriptorHandleForHeapStart();
    measure_target.ptr += SIZE_T{s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)} * 4;
    D3D12_RENDER_TARGET_VIEW_DESC measure_view{};
    measure_view.Format = DXGI_FORMAT_R32_FLOAT;
    measure_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    s.device->CreateRenderTargetView(s.measure.Get(), &measure_view, measure_target);
    move(s.measure.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->SetPipelineState(s.measure_pipeline.Get());
    const D3D12_VIEWPORT measure_viewport{0, 0, static_cast<float>(measure_width), 1, 0, 1};
    const D3D12_RECT small_scissor{0, 0, static_cast<LONG>(measure_width), 1};
    list->RSSetViewports(1, &measure_viewport);
    list->RSSetScissorRects(1, &small_scissor);
    list->OMSetRenderTargets(1, &measure_target, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    move(s.measure.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = s.measure_readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Offset = slot * measure_slice;
    to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, measure_width, 1, 1, measure_pitch};
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = s.measure.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}

// Records our list: the picture copied, then drawn back through the plugin's shader. Under the lock.
// The camera the picture was drawn with (camera_delay), this frame's taken into the history first: between the two kept
// either side of it, blended (its axes made unit again); the oldest kept, until there are enough.
std::array<float, camera_floats> picture_camera(Shared& s, const plugin_camera::Projector& projector) {
    {
        const auto& m = projector.m;
        for (std::size_t c = camera_history - 1; c > 0; --c) s.cameras[c] = s.cameras[c - 1];
        s.cameras[0] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]};
        s.cameras_known = std::min(s.cameras_known + 1, camera_history);
    }
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
    return camera;
}

// A part of an upload buffer for one draw of the plugin's shader: the cbuffer, and the plugin's data and the heights where
// they changed since they last went into that part (a part never written holds whatever the buffer had, so each is
// written once at least). Under both locks.
void fill_slice(Shared& s, Asked& a, std::uint8_t* slice, std::uint64_t& data_uploaded, std::uint64_t& heights_uploaded,
                const std::array<float, camera_floats>& camera, float focal, float width, float height, bool have_last) {
    const std::uint64_t now = GetTickCount64();
    const bool anchored = a.anchor && now < a.anchor_until;
    const std::size_t data_floats4 = a.data ? std::min(a.data->size(), most_data_bytes) / 16 : 0;
    if (a.data && data_uploaded != a.data_version + 1) {
        data_uploaded = a.data_version + 1;
        std::memcpy(slice + data_offset, a.data->data(), data_floats4 * 16);
    }
    if (a.heights) {
        float* const head = reinterpret_cast<float*>(slice + heights_offset);
        head[0] = 1.0f;
        head[1] = static_cast<float>(height_grids[0].cells), head[2] = height_grids[0].size, head[3] = static_cast<float>(height_grids[1].cells);
        head[4] = height_grids[1].size, head[5] = static_cast<float>(a.heights_near_first[0]), head[6] = static_cast<float>(a.heights_near_first[1]);
        head[7] = 0;
        if (heights_uploaded != a.heights_version + 1) {
            heights_uploaded = a.heights_version + 1;
            float* to = head + 8;
            std::memcpy(to, a.heights_cells[0].data(), a.heights_cells[0].size() * sizeof(a.heights_cells[0][0]));
            to += a.heights_cells[0].size() * 4;
            std::memcpy(to, a.heights_cells[1].data(), a.heights_cells[1].size() * sizeof(a.heights_cells[1][0]));
            to += a.heights_cells[1].size() * 4;
            std::memcpy(to, a.heights_near_top.data(), a.heights_near_top.size() * sizeof(float));
        }
    } else {
        reinterpret_cast<float*>(slice + heights_offset)[0] = 0.0f;
    }
    a.eye = {camera[9], camera[10], camera[11]};
    a.eye_known = true;
    // A change of the distance held slides over half a second instead of jumping.
    if (s.held_near <= 0 || s.shown_near <= 0 || now < s.shown_near_at) {
        s.shown_near = s.held_near;
    } else {
        const float step = std::min(static_cast<float>(now - s.shown_near_at) / 150.0f, 1.0f);
        s.shown_near += (s.held_near - s.shown_near) * step;
        if (std::abs(s.shown_near - s.held_near) < s.held_near * 1e-4f) s.shown_near = s.held_near;
    }
    s.shown_near_at = now;
    const double seconds = static_cast<double>(now_us()) / 1e6;
    const float frame_seconds = static_cast<float>(std::clamp(seconds - s.last_seconds, 0.0, 0.25));
    s.last_seconds = seconds;
    const auto& m = camera; // right, up, back, eye: three a row
    const auto& l = s.last_camera;
    std::array<float, cbuffer_floats> constants{
        m[0], m[1], m[2], focal,
        m[3], m[4], m[5], width,
        m[6], m[7], m[8], height,
        m[9], m[10], m[11], s.shown_near, // the near distance measured (0: not yet)
        l[0], l[1], l[2], have_last ? 1.0f : 0.0f,
        l[3], l[4], l[5], 0,
        l[6], l[7], l[8], 0,
        l[9], l[10], l[11], 0,
        anchored ? (*a.anchor)[0] : 0.0f, anchored ? (*a.anchor)[1] : 0.0f, anchored ? (*a.anchor)[2] : 0.0f, anchored ? 1.0f : 0.0f,
        static_cast<float>(clock_seconds()), frame_seconds, static_cast<float>(data_floats4), a.heights ? 1.0f : 0.0f};
    for (std::size_t i = 0; i < value_count; ++i) constants[40 + i] = a.values[i];
    // reskate_light: the picture's brightness eased, the last measured, 1 once measured.
    constants[88] = s.light_known ? s.light : 0.0f;
    constants[89] = s.light_known ? s.light_raw : 0.0f;
    constants[90] = s.light_known ? 1.0f : 0.0f;
    constants[91] = s.far_known ? s.far_light : 0.0f;
    std::memcpy(slice, constants.data(), sizeof(constants));
    s.last_camera = camera;
}

// The camera for the shader: the game's now, else, for up to 5 s, the last one read for a picture of this size. While
// the game hitched (froze for a moment, then came back) no camera was read, and the shader went for 240 frames
// (2026-11-16); the camera does not move while the game is frozen.
std::optional<plugin_camera::Projector> camera_now(Shared& s, float width, float height) {
    const std::uint64_t now = GetTickCount64();
    if (auto projector = plugin_camera::projector(width, height)) {
        s.last_projector = projector;
        s.last_projector_at = now;
        return projector;
    }
    if (s.last_projector && s.last_projector->width == width && s.last_projector->height == height && now - s.last_projector_at < 5000) {
        ++s.outcomes["drawn with the last camera (none read)"];
        return s.last_projector;
    }
    return std::nullopt;
}

bool record_shader(Shared& s) {
    s.why = "nothing to draw into";
    if (s.failed || !s.colour || !s.main_depth) return false;
    // Our copy of the game's depth, taken this frame as soon as the game had finished it. Else the last frame's, still
    // there, up to six frames old: a frame now and then had none (the depth not readable when its batch went).
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
                            "World shader: not drawn; the world's picture (format %u, flags 0x%x) takes no render target.",
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
    const auto projector = camera_now(s, static_cast<float>(colour_desc.Width), static_cast<float>(colour_desc.Height));
    if (!projector) {
        s.why = "no camera";
        return false;
    }
    if (std::abs(projector->focal - s.last_focal) > s.last_focal * 0.01f) s.focal_changed_frame = s.frame;
    s.last_focal = projector->focal;
    if (!build_resources(s, colour_desc) || !build_single(s, "ps_measure", DXGI_FORMAT_R32_FLOAT, s.measure_pipeline)) return false;
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
    auto& a = asked();
    std::lock_guard asked_lock(a.mutex);
    // What this allocator's last list measured: the GPU is done with it.
    if (s.measure_waiting[at] && s.measure_cpu) {
        const std::uint8_t* const rows = s.measure_cpu + at * measure_slice;
        take_light(s, rows);
        if (s.measure_anchored[at] && s.measure_fresh[at]) take_measurements(s, rows);
    }
    s.measure_waiting[at] = false;
    // And how long its shader took on the GPU, against the budget.
    if (s.timer_waiting[at] && s.timer_cpu && s.timer_frequency) {
        const auto start = s.timer_cpu[at * 2], end = s.timer_cpu[at * 2 + 1];
        const auto key = s.timer_key[at];
        const auto program = a.programs.find(key);
        if (end > start && program != a.programs.end()) {
            const double ms = static_cast<double>(end - start) * 1000.0 / static_cast<double>(s.timer_frequency);
            s.gpu_ms_total += ms, s.gpu_ms_most = std::max(s.gpu_ms_most, ms), ++s.gpu_samples;
            if (key != s.budget_key) s.budget_key = key, s.budget_ms = 0, s.budget_frames = 0, s.budget_long = 0;
            const std::string name = key == a.key ? a.name : std::string("(an earlier one)");
            if (ms > world_frame_most_ms) {
                // Not at the first: the time is the GPU's from the list's start to its end, and one frame of it can be
                // long for a reason that is not the shader's (the game's window left and come back to, another program
                // on the GPU). A long frame is left out of the average, and counted.
                if (++s.budget_long >= world_long_frames)
                    refuse(*program->second, name, std::format("{} frames of it in {} took over {:.0f} ms on the GPU (the last {:.1f})", world_long_frames,
                                                               world_budget_frames, world_frame_most_ms, ms));
            } else {
                s.budget_ms += ms;
                if (++s.budget_frames >= world_budget_frames) {
                    const double average = s.budget_ms / s.budget_frames;
                    s.budget_ms = 0, s.budget_frames = 0, s.budget_long = 0;
                    if (key == a.key) a.cost_ms = average;
                    if (average > world_budget_ms)
                        refuse(*program->second, name, std::format("it took {:.2f} ms a frame on the GPU (at most {:.1f})", average, world_budget_ms));
                }
            }
        }
    }
    s.timer_waiting[at] = false;
    // The plugin's shader, ready.
    const auto found = a.programs.find(a.key);
    if (found == a.programs.end() || found->second->state != Program::State::ready) {
        s.why = "the plugin's shader is not ready (compiling, or refused)";
        return false;
    }
    auto* const pipeline = plugin_pipeline(s, a.key, *found->second, a.name, s.colour_format);
    if (!pipeline) {
        if (!s.why) s.why = "the plugin's shader has no pipeline";
        return false;
    }
    s.why = "recording failed";
    if (FAILED(s.allocators[at]->Reset()) || FAILED(s.list->Reset(s.allocators[at].Get(), nullptr))) return false;
    s.list->EndQuery(s.timer_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(at * 2));
    s.next_allocator = (at + 1) % s.allocators.size();
    const auto camera = picture_camera(s, *projector);
    if (++s.cost_frames >= 600) {
        s.cost_frames = 0;
        // What it costs: on the GPU a frame, and the heights' rays on the game's update thread a tick.
        const auto ticks = height_ticks.exchange(0), rays = height_rays.exchange(0), height_us = height_us_total.exchange(0),
                   height_most = height_us_most.exchange(0);
        // And the following of the game's lists, on its recording threads: how many of their calls a frame.
        const auto followed = followed_calls.exchange(0);
        const auto followed_frames = s.frame > s.cost_said_frame ? s.frame - s.cost_said_frame : 0;
        s.cost_said_frame = s.frame;
        logging::printf(logging::Level::info, logging::Channel::graphics,
                        "World shader cost: on the GPU %.2f ms a frame (at most %.2f) over %u frames; the ground heights %.2f ms a tick (at most %.2f), "
                        "%.1f rays a tick, over %llu ticks; %.0f calls on the game's lists followed a frame. The light given: the picture's %.4f "
                        "(this frame %.4f), the sky's %.4f%s.",
                        s.gpu_samples ? s.gpu_ms_total / s.gpu_samples : 0.0, s.gpu_ms_most, s.gpu_samples,
                        ticks ? static_cast<double>(height_us) / 1000.0 / static_cast<double>(ticks) : 0.0, static_cast<double>(height_most) / 1000.0,
                        ticks ? static_cast<double>(rays) / static_cast<double>(ticks) : 0.0, static_cast<unsigned long long>(ticks),
                        followed_frames ? static_cast<double>(followed) / static_cast<double>(followed_frames) : 0.0,
                        static_cast<double>(s.light), static_cast<double>(s.light_raw), static_cast<double>(s.far_light),
                        s.far_known ? "" : " (no sky seen yet)");
        s.gpu_ms_total = s.gpu_ms_most = 0;
        s.gpu_samples = 0;
    }
    const std::uint64_t now = GetTickCount64();
    const bool anchored = a.anchor && now < a.anchor_until;
    // This allocator's part of the upload buffer.
    std::uint8_t* const slice = s.upload_cpu + at * upload_slice;
    // Views: the picture's copy as the target's format reads it, and our copy of the depth.
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
    // Last frame's camera counts only if the shader was drawn on the frame before this one.
    const bool have_last = s.last_drawn_frame + 1 == s.frame;
    // The target: the picture.
    D3D12_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = s.colour_format;
    target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const auto target = s.targets->GetCPUDescriptorHandleForHeapStart();
    s.device->CreateRenderTargetView(s.colour, &target_view, target);

    auto* list = s.list.Get();
    const auto before = s.colour_state;
    transition(list, s.colour, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(s.picture.Get(), s.colour);
    transition(list, s.picture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, s.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const float width = static_cast<float>(colour_desc.Width), height = static_cast<float>(colour_desc.Height);
    fill_slice(s, a, slice, s.data_uploaded[at], s.heights_uploaded[at], camera, projector->focal, width, height, have_last);
    const auto upload = s.upload->GetGPUVirtualAddress() + at * upload_slice;
    ID3D12DescriptorHeap* heaps[] = {s.views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(s.root.Get());
    list->SetPipelineState(pipeline);
    list->SetGraphicsRootConstantBufferView(0, upload);
    auto table = s.views->GetGPUDescriptorHandleForHeapStart();
    table.ptr += UINT64{view_step} * views_each * at;
    list->SetGraphicsRootDescriptorTable(1, table);
    list->SetGraphicsRootShaderResourceView(2, upload + data_offset);
    list->SetGraphicsRootShaderResourceView(3, upload + heights_offset);
    const D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(colour_desc.Width), static_cast<LONG>(colour_desc.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    list->EndQuery(s.timer_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(at * 2 + 1));

    // The measurements, with the same constants and views, copied out for the CPU (take_light, take_measurements): every
    // frame, for the picture's brightness; the near distance's only with an anchor set.
    record_measure(s, list, at, [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to_state) { transition(list, r, from, to_state); });

    transition(list, s.colour, D3D12_RESOURCE_STATE_RENDER_TARGET, before);
    transition(list, s.picture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResolveQueryData(s.timer_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(at * 2), 2, s.timer_readback.Get(),
                           at * 2 * sizeof(std::uint64_t));
    if (FAILED(list->Close())) return false;
    // Only once the list is closed (it goes to the GPU now): one that failed to close never runs, and its slot read
    // back next time would be an older frame's, taken as this one's.
    s.measure_waiting[at] = true;
    s.measure_anchored[at] = anchored;
    s.measure_fresh[at] = s.depth_copied_frame == s.frame;
    s.timer_waiting[at] = true;
    s.timer_key[at] = a.key;
    s.last_drawn_frame = s.frame;
    s.why = nullptr;
    return true;
}

// ---- drawn inside the game's own list, on the frames there is no other moment ------------------------------------
// On some frames the game finishes the world's picture and post-processes it in one batch, before the batch that takes
// the screen to a render target: there is no moment between batches before the picture is used, and the shader went
// without that frame (a blink, about one frame in 6,000, 2026-11-16). On those frames it is drawn inside that list, just
// before the game's own barrier that finishes the picture, and the list is left as the game had it: what the game set on
// its graphics lists (pipeline, root signature and arguments, descriptor heaps, viewports, scissors, render targets,
// topology) is followed while a world shader is on, from each list's Reset, and put back after the draw. A list whose
// state is not all known (reset before the following began, or one that ran a bundle) is never drawn into.
// Slots of ID3D12GraphicsCommandList's table (d3d12.h's declaration order).
constexpr std::size_t list_reset = 10, list_clear_state = 11, list_topology = 20, list_viewports = 21, list_scissors = 22, list_pipeline = 25,
                      list_bundle = 27, list_heaps = 28, list_root_signature = 30, list_table_slot = 32, list_constant = 34, list_constants = 36,
                      list_cbv = 38, list_srv = 40, list_uav = 42, list_targets = 46;
// The compute ones, each just before its graphics one.
constexpr std::size_t list_compute_root_signature = 29, list_compute_table = 31, list_compute_constant = 33, list_compute_constants = 35,
                      list_compute_cbv = 37, list_compute_srv = 39, list_compute_uav = 41;

struct RootArg {
    std::uint8_t kind{}; // 0 none, 1 a table, 2 constants, 3 a constant buffer, 4 a shader resource, 5 an unordered access
    std::uint64_t value{};
    std::vector<UINT> constants;
};
struct ListState {
    bool complete{};
    unsigned calls{}; // the calls followed since its Reset: how much the game recorded on it
    std::uint64_t reset{}; // which Reset of the list this follows (list_resets)
    ID3D12PipelineState* pipeline{};
    UINT heap_count{};
    std::array<ID3D12DescriptorHeap*, 2> heaps{};
    // The graphics root signature and its arguments [0], and the compute ones [1]. Both are put back: our descriptor heap
    // set for the draw leaves the game's tables of either undefined, and the game's post-processing after it runs in
    // compute (left out, the picture came out wrong after a draw inside, 2026-11-16).
    std::array<ID3D12RootSignature*, 2> root{};
    std::array<std::vector<RootArg>, 2> args;
    bool has_topology{};
    D3D12_PRIMITIVE_TOPOLOGY topology{};
    UINT viewport_count{};
    std::array<D3D12_VIEWPORT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    UINT scissor_count{};
    std::array<D3D12_RECT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
    bool has_targets{};
    UINT target_count{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    BOOL single{};
    bool has_depth{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
};
std::atomic<bool> tracking{}; // a world shader is on: the game's lists are followed
// Each recording thread's lists (a list is recorded on one thread from its Reset to its Close; one that moves to another
// thread has no known state there).
thread_local std::unordered_map<ID3D12GraphicsCommandList*, ListState> list_states;
thread_local bool drawing_inside{}; // our own calls on a game's list: not the game's state
// Each list's latest Reset, on whatever thread: what a thread followed of a list since reset on another is not its state.
std::mutex list_resets_mutex;
std::unordered_map<ID3D12GraphicsCommandList*, std::uint64_t> list_resets;
std::uint64_t list_resets_count{};
std::uint64_t note_reset(ID3D12GraphicsCommandList* list) {
    std::lock_guard lock(list_resets_mutex);
    return list_resets[list] = ++list_resets_count;
}
bool latest_reset(ID3D12GraphicsCommandList* list, std::uint64_t reset) {
    std::lock_guard lock(list_resets_mutex);
    const auto found = list_resets.find(list);
    return found != list_resets.end() && found->second == reset;
}
ListState& state_of(ID3D12GraphicsCommandList* list) {
    auto& st = list_states[list];
    ++st.calls; // every hook that follows the list comes through here
    return st;
}
bool following() { return tracking.load(std::memory_order_relaxed) && !drawing_inside; }
RootArg& arg_of(ListState& st, bool compute, UINT index) {
    auto& args = st.args[compute ? 1 : 0];
    if (args.size() <= index) args.resize(index + 1);
    return args[index];
}

using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using ClearStateFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using TopologyFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
using ViewportsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using ScissorsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using PipelineFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using BundleFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
using HeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using RootSignatureFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using TableFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using ConstantFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using ConstantsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using AddressFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using TargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
std::atomic<ResetFn> original_reset{};
std::atomic<ClearStateFn> original_clear_state{};
std::atomic<TopologyFn> original_topology{};
std::atomic<ViewportsFn> original_viewports{};
std::atomic<ScissorsFn> original_scissors{};
std::atomic<PipelineFn> original_pipeline{};
std::atomic<BundleFn> original_bundle{};
std::atomic<HeapsFn> original_heaps{};
std::atomic<RootSignatureFn> original_root_signature{}, original_compute_root_signature{};
std::atomic<TableFn> original_table{}, original_compute_table{};
std::atomic<ConstantFn> original_constant{}, original_compute_constant{};
std::atomic<ConstantsFn> original_constants{}, original_compute_constants{};
std::atomic<AddressFn> original_cbv{}, original_srv{}, original_uav{}, original_compute_cbv{}, original_compute_srv{}, original_compute_uav{};
std::atomic<TargetsFn> original_targets{};

HRESULT STDMETHODCALLTYPE hooked_reset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline) {
    const HRESULT result = original_reset.load()(list, allocator, pipeline);
    if (following()) {
        auto& st = state_of(list);
        followed_calls.fetch_add(st.calls, std::memory_order_relaxed);
        st = ListState{};
        st.complete = SUCCEEDED(result);
        st.reset = note_reset(list);
        st.pipeline = pipeline;
    } else if (!drawing_inside) {
        list_states.erase(list);
        if (tracking.load(std::memory_order_relaxed)) (void)note_reset(list);
    }
    return result;
}
void STDMETHODCALLTYPE hooked_clear_state(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
    original_clear_state.load()(list, pipeline);
    if (following()) {
        auto& st = state_of(list);
        const bool complete = st.complete;
        st = ListState{};
        st.complete = complete;
        st.pipeline = pipeline;
    }
}
void STDMETHODCALLTYPE hooked_topology(ID3D12GraphicsCommandList* list, D3D12_PRIMITIVE_TOPOLOGY topology) {
    original_topology.load()(list, topology);
    if (following()) {
        auto& st = state_of(list);
        st.has_topology = true;
        st.topology = topology;
    }
}
void STDMETHODCALLTYPE hooked_viewports(ID3D12GraphicsCommandList* list, UINT count, const D3D12_VIEWPORT* viewports) {
    original_viewports.load()(list, count, viewports);
    if (following()) {
        auto& st = state_of(list);
        st.viewport_count = std::min<UINT>(count, static_cast<UINT>(st.viewports.size()));
        if (viewports) std::copy_n(viewports, st.viewport_count, st.viewports.begin());
    }
}
void STDMETHODCALLTYPE hooked_scissors(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RECT* rects) {
    original_scissors.load()(list, count, rects);
    if (following()) {
        auto& st = state_of(list);
        st.scissor_count = std::min<UINT>(count, static_cast<UINT>(st.scissors.size()));
        if (rects) std::copy_n(rects, st.scissor_count, st.scissors.begin());
    }
}
void STDMETHODCALLTYPE hooked_pipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
    original_pipeline.load()(list, pipeline);
    if (following()) state_of(list).pipeline = pipeline;
}
void STDMETHODCALLTYPE hooked_bundle(ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList* bundle) {
    original_bundle.load()(list, bundle);
    if (following()) state_of(list).complete = false; // what the bundle set is not known
}
void STDMETHODCALLTYPE hooked_heaps(ID3D12GraphicsCommandList* list, UINT count, ID3D12DescriptorHeap* const* heaps) {
    original_heaps.load()(list, count, heaps);
    if (following()) {
        auto& st = state_of(list);
        st.heap_count = std::min<UINT>(count, 2);
        for (UINT i = 0; i < st.heap_count; ++i) st.heaps[i] = heaps ? heaps[i] : nullptr;
    }
}
template <std::atomic<RootSignatureFn>& original, bool compute>
void STDMETHODCALLTYPE hooked_root_signature(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root) {
    original.load()(list, root);
    if (following()) {
        auto& st = state_of(list);
        auto& current = st.root[compute ? 1 : 0];
        if (current != root) st.args[compute ? 1 : 0].clear(); // another root signature: its arguments are set afresh
        current = root;
    }
}
template <std::atomic<TableFn>& original, bool compute>
void STDMETHODCALLTYPE hooked_table(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    original.load()(list, index, table);
    if (following()) {
        auto& arg = arg_of(state_of(list), compute, index);
        arg.kind = 1, arg.value = table.ptr;
    }
}
template <std::atomic<ConstantFn>& original, bool compute>
void STDMETHODCALLTYPE hooked_constant(ID3D12GraphicsCommandList* list, UINT index, UINT value, UINT offset) {
    original.load()(list, index, value, offset);
    if (following()) {
        auto& arg = arg_of(state_of(list), compute, index);
        if (arg.kind != 2) arg.constants.clear();
        arg.kind = 2;
        if (arg.constants.size() <= offset) arg.constants.resize(offset + 1);
        arg.constants[offset] = value;
    }
}
template <std::atomic<ConstantsFn>& original, bool compute>
void STDMETHODCALLTYPE hooked_constants(ID3D12GraphicsCommandList* list, UINT index, UINT count, const void* values, UINT offset) {
    original.load()(list, index, count, values, offset);
    if (following()) {
        auto& arg = arg_of(state_of(list), compute, index);
        if (arg.kind != 2) arg.constants.clear();
        arg.kind = 2;
        if (arg.constants.size() < offset + count) arg.constants.resize(offset + count);
        if (values) std::memcpy(arg.constants.data() + offset, values, std::size_t{count} * sizeof(UINT));
    }
}
template <std::atomic<AddressFn>& original, std::uint8_t kind, bool compute>
void STDMETHODCALLTYPE hooked_address(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    original.load()(list, index, address);
    if (following()) {
        auto& arg = arg_of(state_of(list), compute, index);
        arg.kind = kind, arg.value = address;
    }
}
void STDMETHODCALLTYPE hooked_targets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL single,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    original_targets.load()(list, count, targets, single, depth);
    if (following()) {
        auto& st = state_of(list);
        st.has_targets = true;
        st.target_count = std::min<UINT>(count, static_cast<UINT>(st.targets.size()));
        st.single = single;
        // One handle and a range from it, or a handle each.
        const UINT handles = single ? std::min<UINT>(st.target_count, 1) : st.target_count;
        if (targets) std::copy_n(targets, handles, st.targets.begin());
        st.has_depth = depth != nullptr;
        if (depth) st.depth = *depth;
    }
}

// The game's state put back on its list, as it had it before our draw (our own calls, so not followed).
void restore(ID3D12GraphicsCommandList* list, const ListState& st) {
    if (st.heap_count) list->SetDescriptorHeaps(st.heap_count, st.heaps.data());
    if (st.pipeline) list->SetPipelineState(st.pipeline);
    if (st.root[0]) {
        list->SetGraphicsRootSignature(st.root[0]);
        for (UINT i = 0; i < st.args[0].size(); ++i) {
            const auto& arg = st.args[0][i];
            switch (arg.kind) {
            case 1: list->SetGraphicsRootDescriptorTable(i, D3D12_GPU_DESCRIPTOR_HANDLE{arg.value}); break;
            case 2:
                if (!arg.constants.empty()) list->SetGraphicsRoot32BitConstants(i, static_cast<UINT>(arg.constants.size()), arg.constants.data(), 0);
                break;
            case 3: list->SetGraphicsRootConstantBufferView(i, arg.value); break;
            case 4: list->SetGraphicsRootShaderResourceView(i, arg.value); break;
            case 5: list->SetGraphicsRootUnorderedAccessView(i, arg.value); break;
            default: break;
            }
        }
    }
    // The compute ones: not changed by our draw, but its descriptor heap left their tables undefined.
    if (st.root[1]) {
        list->SetComputeRootSignature(st.root[1]);
        for (UINT i = 0; i < st.args[1].size(); ++i) {
            const auto& arg = st.args[1][i];
            switch (arg.kind) {
            case 1: list->SetComputeRootDescriptorTable(i, D3D12_GPU_DESCRIPTOR_HANDLE{arg.value}); break;
            case 2:
                if (!arg.constants.empty()) list->SetComputeRoot32BitConstants(i, static_cast<UINT>(arg.constants.size()), arg.constants.data(), 0);
                break;
            case 3: list->SetComputeRootConstantBufferView(i, arg.value); break;
            case 4: list->SetComputeRootShaderResourceView(i, arg.value); break;
            case 5: list->SetComputeRootUnorderedAccessView(i, arg.value); break;
            default: break;
            }
        }
    }
    if (st.has_topology) list->IASetPrimitiveTopology(st.topology);
    if (st.viewport_count) list->RSSetViewports(st.viewport_count, st.viewports.data());
    if (st.scissor_count) list->RSSetScissorRects(st.scissor_count, st.scissors.data());
    if (st.has_targets) list->OMSetRenderTargets(st.target_count, st.target_count ? st.targets.data() : nullptr, st.single, st.has_depth ? &st.depth : nullptr);
    else list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
}

// The plugin's shader drawn inside a game's list, into `picture` (the world's, of the kind locked onto, in `before`, which
// the game's own barrier is about to move to being read). Under s's lock, on the game's recording thread. True when drawn.
bool draw_inside(Shared& s, ID3D12GraphicsCommandList* list, ID3D12Resource* picture, D3D12_RESOURCE_STATES before,
                 void(STDMETHODCALLTYPE* barrier)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)) {
    if (!barrier || !following() || s.failed || !s.device || !s.locked || !s.picture || !s.depth_copy || !s.root || !s.upload_cpu || !s.views ||
        !s.targets)
        return false;
    const auto known = list_states.find(list);
    if (known == list_states.end() || !known->second.complete || !latest_reset(list, known->second.reset)) return false;
    if (s.depth_copied_frame > s.frame || s.frame - s.depth_copied_frame > 6) return false;
    const auto desc = picture->GetDesc();
    if (desc.Width != s.picture_desc.Width || desc.Height != s.picture_desc.Height || desc.Format != s.picture_desc.Format ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
        return false;
    const float width = static_cast<float>(desc.Width), height = static_cast<float>(desc.Height);
    const auto projector = camera_now(s, width, height);
    if (!projector) return false;
    auto& a = asked();
    std::lock_guard asked_lock(a.mutex);
    const auto found = a.programs.find(a.key);
    if (found == a.programs.end() || found->second->state != Program::State::ready) return false;
    auto* const pipeline = plugin_pipeline(s, a.key, *found->second, a.name, s.colour_format);
    if (!pipeline) return false;
    const std::size_t slot = s.inside_next;
    s.inside_next = (slot + 1) % inside_ring;
    // This slot's last measurements, once the game's list they were in has run (its fence); until then none taken here.
    // Only the picture's brightness: the near distance is not taken from a draw inside a game's list, whose depth copy can
    // be the last frame's under this frame's camera (taken from it, the distance was learnt wrong and the snow round the
    // skater went, 2026-11-16). Drawn inside on every frame, the light was never measured at all.
    const std::size_t measured_at = s.allocators.size() + slot;
    bool measure_here = s.measure && s.measure_pipeline && s.measure_cpu && s.fence;
    if (s.measure_waiting[measured_at]) {
        if (s.inside_fence[slot] != ~UINT64{0} && s.fence && s.fence->GetCompletedValue() >= s.inside_fence[slot]) {
            const std::uint8_t* const rows = s.measure_cpu + measured_at * measure_slice;
            take_light(s, rows);
            s.measure_waiting[measured_at] = false;
        } else {
            measure_here = false;
            if (s.inside_fence[slot] == ~UINT64{0}) s.measure_waiting[measured_at] = false; // its list was never submitted
        }
    }
    const auto camera = picture_camera(s, *projector);
    const bool have_last = s.last_drawn_frame + 1 == s.frame;
    const std::size_t part = s.allocators.size() + slot; // the upload buffer's parts after the allocators'
    std::uint8_t* const slice = s.upload_cpu + part * upload_slice;
    fill_slice(s, a, slice, s.inside_data_uploaded[slot], s.inside_heights_uploaded[slot], camera, projector->focal, width, height, have_last);
    // Views: the picture's copy and our copy of the depth, in this draw's own two after the allocators'.
    const UINT view_step = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT first_view = views_each * static_cast<UINT>(part);
    auto view = s.views->GetCPUDescriptorHandleForHeapStart();
    view.ptr += SIZE_T{view_step} * first_view;
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
    // The target: the picture (the RTV heap's second slot).
    D3D12_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = s.colour_format;
    target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    auto target = s.targets->GetCPUDescriptorHandleForHeapStart();
    target.ptr += SIZE_T{s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
    s.device->CreateRenderTargetView(picture, &target_view, target);

    drawing_inside = true;
    const auto move = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (from == to) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        barrier(list, 1, &b); // straight to the game's list, past the depth tap
    };
    move(picture, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(s.picture.Get(), picture);
    move(s.picture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    move(picture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto upload = s.upload->GetGPUVirtualAddress() + part * upload_slice;
    ID3D12DescriptorHeap* heaps[] = {s.views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(s.root.Get());
    list->SetPipelineState(pipeline);
    list->SetGraphicsRootConstantBufferView(0, upload);
    auto table = s.views->GetGPUDescriptorHandleForHeapStart();
    table.ptr += UINT64{view_step} * first_view;
    list->SetGraphicsRootDescriptorTable(1, table);
    list->SetGraphicsRootShaderResourceView(2, upload + data_offset);
    list->SetGraphicsRootShaderResourceView(3, upload + heights_offset);
    const D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    if (measure_here) {
        record_measure(s, list, measured_at, move);
        s.measure_waiting[measured_at] = true;
        s.measure_anchored[measured_at] = false; // (its near distance's points are not read)
        s.measure_fresh[measured_at] = false;
        s.inside_fence[slot] = ~UINT64{0}; // set when its list is submitted
    }
    s.inside_last_slot = slot;
    move(picture, D3D12_RESOURCE_STATE_RENDER_TARGET, before);
    move(s.picture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    restore(list, known->second);
    drawing_inside = false;
    s.last_drawn_frame = s.frame;
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
// The hooks and ReSkate's own shaders' compile, the first time a plugin asks for a world shader (and the overlay's
// graphics are up): a player whose plugins draw none has none of them. Installed at the overlay's start, every player had
// them. One function, through the table every one of the runtime's queues shares (found through the overlay's
// own): ExecuteCommandLists, the shader's moment (the rest it sees through the depth tap's ResourceBarrier hook).
// The game is running by then, so in a HookBatch. It is hooked where it is; where its code cannot be (Proton's
// D3D12, vkd3d-proton, refused one on a Steam Deck, 2026-11-13), its slot in the table is pointed at ours instead:
// calls through the interface, which are what the game makes, come to us the same.
static void ensure_hooks() noexcept {
    static std::mutex mutex;
    static bool hooked{};
    std::lock_guard once(mutex);
    void** const queues = queue_table.load();
    if (hooked || !queues) return;
    hooked = true;
    try {
        start_compiling();
        const auto hook = [](void** table, std::size_t slot, void* detour, auto& original, const char* name) {
            using Fn = std::remove_reference_t<decltype(original.load())>;
            void* const target = table[slot];
            void* trampoline{};
            if (hook_prepare(target, detour, &trampoline) == HookOk) {
                original.store(reinterpret_cast<Fn>(trampoline));
                if (hook_enable(target) == HookOk) return true; // (queued: the batch attaches it)
                original.store(nullptr);
            }
            // What the detour passes on to, stored before the slot is pointed at the detour: the game's render threads
            // come through these slots thousands of times a frame, and one that came between the two called nothing.
            original.store(reinterpret_cast<Fn>(target));
            if (patch_table_slot(name, &table[slot], target, detour) == HookOk) {
                logging::printf(logging::Level::info, logging::Channel::graphics, "World shader: %s is followed through its table slot.", name);
                return true;
            }
            original.store(nullptr);
            logging::printf(logging::Level::warning, logging::Channel::graphics, "World shader: %s hook could not be put in place.", name);
            return false;
        };
        bool ok = false;
        {
            HookBatch batch;
            ok = hook(queues, queue_execute, reinterpret_cast<void*>(&hooked_execute), original_execute, "ExecuteCommandLists");
            // What the game sets on its lists, for a draw inside one (draw_inside). Without all of them it is never drawn
            // inside one, and the shader goes on as before.
            if (void** const lists = list_table.load(); ok && lists) {
                bool all = true;
                all = hook(lists, list_reset, reinterpret_cast<void*>(&hooked_reset), original_reset, "Reset") && all;
                all = hook(lists, list_clear_state, reinterpret_cast<void*>(&hooked_clear_state), original_clear_state, "ClearState") && all;
                all = hook(lists, list_topology, reinterpret_cast<void*>(&hooked_topology), original_topology, "IASetPrimitiveTopology") && all;
                all = hook(lists, list_viewports, reinterpret_cast<void*>(&hooked_viewports), original_viewports, "RSSetViewports") && all;
                all = hook(lists, list_scissors, reinterpret_cast<void*>(&hooked_scissors), original_scissors, "RSSetScissorRects") && all;
                all = hook(lists, list_pipeline, reinterpret_cast<void*>(&hooked_pipeline), original_pipeline, "SetPipelineState") && all;
                all = hook(lists, list_bundle, reinterpret_cast<void*>(&hooked_bundle), original_bundle, "ExecuteBundle") && all;
                all = hook(lists, list_heaps, reinterpret_cast<void*>(&hooked_heaps), original_heaps, "SetDescriptorHeaps") && all;
                all = hook(lists, list_root_signature, reinterpret_cast<void*>(&hooked_root_signature<original_root_signature, false>),
                           original_root_signature, "SetGraphicsRootSignature") && all;
                all = hook(lists, list_table_slot, reinterpret_cast<void*>(&hooked_table<original_table, false>), original_table,
                           "SetGraphicsRootDescriptorTable") && all;
                all = hook(lists, list_constant, reinterpret_cast<void*>(&hooked_constant<original_constant, false>), original_constant,
                           "SetGraphicsRoot32BitConstant") && all;
                all = hook(lists, list_constants, reinterpret_cast<void*>(&hooked_constants<original_constants, false>), original_constants,
                           "SetGraphicsRoot32BitConstants") && all;
                all = hook(lists, list_cbv, reinterpret_cast<void*>(&hooked_address<original_cbv, 3, false>), original_cbv,
                           "SetGraphicsRootConstantBufferView") && all;
                all = hook(lists, list_srv, reinterpret_cast<void*>(&hooked_address<original_srv, 4, false>), original_srv,
                           "SetGraphicsRootShaderResourceView") && all;
                all = hook(lists, list_uav, reinterpret_cast<void*>(&hooked_address<original_uav, 5, false>), original_uav,
                           "SetGraphicsRootUnorderedAccessView") && all;
                all = hook(lists, list_compute_root_signature, reinterpret_cast<void*>(&hooked_root_signature<original_compute_root_signature, true>),
                           original_compute_root_signature, "SetComputeRootSignature") && all;
                all = hook(lists, list_compute_table, reinterpret_cast<void*>(&hooked_table<original_compute_table, true>), original_compute_table,
                           "SetComputeRootDescriptorTable") && all;
                all = hook(lists, list_compute_constant, reinterpret_cast<void*>(&hooked_constant<original_compute_constant, true>),
                           original_compute_constant, "SetComputeRoot32BitConstant") && all;
                all = hook(lists, list_compute_constants, reinterpret_cast<void*>(&hooked_constants<original_compute_constants, true>),
                           original_compute_constants, "SetComputeRoot32BitConstants") && all;
                all = hook(lists, list_compute_cbv, reinterpret_cast<void*>(&hooked_address<original_compute_cbv, 3, true>), original_compute_cbv,
                           "SetComputeRootConstantBufferView") && all;
                all = hook(lists, list_compute_srv, reinterpret_cast<void*>(&hooked_address<original_compute_srv, 4, true>), original_compute_srv,
                           "SetComputeRootShaderResourceView") && all;
                all = hook(lists, list_compute_uav, reinterpret_cast<void*>(&hooked_address<original_compute_uav, 5, true>), original_compute_uav,
                           "SetComputeRootUnorderedAccessView") && all;
                all = hook(lists, list_targets, reinterpret_cast<void*>(&hooked_targets), original_targets, "OMSetRenderTargets") && all;
                lists_followed.store(all);
            }
        }
        if (!ok) say_failed("its D3D12 hooks are not all in place.");
        else logging::write(logging::Level::info, logging::Channel::graphics, "World shader: hooks installed, as a plugin asked for a world shader.");
    } catch (...) {}
}

// HLSL compiled on a thread of its own; the program is ready or refused when it is done.
static void compile(std::shared_ptr<Program> program, std::string name, std::shared_ptr<const std::string> bytes) {
    std::thread([program = std::move(program), name = std::move(name), bytes = std::move(bytes)] {
        // As the screen shader's is (plugin_shader_compile.h): DXIL with the game's own compiler where it loads, else
        // DXBC. It was DXBC with the older compiler always, which took 65 s on a large shader (2026-11-15).
        plugin_shader_compile::Result made;
        try {
            made = plugin_shader_compile::compile_pixel_shader(std::string(prelude) + *bytes, name);
        } catch (...) {
            made.error = "an exception was raised";
        }
        auto& a = asked();
        std::lock_guard lock(a.mutex);
        if (!made.code.empty()) {
            program->code = std::move(made.code);
            program->dxil = made.dxil; // (its pipeline takes the DXIL vertex shader then: plugin_pipeline)
            program->state = Program::State::ready;
        } else {
            refuse(*program, name, "it did not compile: " + made.error);
        }
    }).detach();
}

std::string set(std::string_view owner, std::string_view name, std::shared_ptr<const std::string> bytes, Kind kind,
                const std::array<float, value_count>& values, bool heights) noexcept {
    try {
        if (!bytes) return "it has no content";
        auto& a = asked();
        std::shared_ptr<Program> start; // an HLSL program to compile, outside the lock
        {
            std::lock_guard lock(a.mutex);
            if (wanted.load() && a.owner != owner) return "another plugin's world shader is on";
            const std::uint64_t key = key_of(name, *bytes);
            auto& program = a.programs[key];
            if (!program) {
                program = std::make_shared<Program>();
                if (kind == Kind::hlsl) {
                    start = program;
                } else {
                    program->code = *bytes;
                    program->dxil = kind == Kind::dxil;
                    program->state = Program::State::ready;
                }
            }
            if (program->state == Program::State::refused) return program->why;
            if (a.owner != owner || a.key != key) a.cost_ms.reset();
            if (a.owner != owner) a.data.reset(), a.anchor.reset();
            a.owner = std::string(owner);
            a.name = std::string(name);
            a.key = key;
            a.heights = heights;
            for (std::size_t i = 0; i < value_count; ++i) a.values[i] = std::isfinite(values[i]) ? values[i] : 0.0f;
        }
        if (start) compile(std::move(start), std::string(name), std::move(bytes));
        wanted.store(true);
        ensure_hooks();
        return {};
    } catch (...) {
        return "it could not be taken";
    }
}
void clear(std::string_view owner) noexcept {
    auto& a = asked();
    std::lock_guard lock(a.mutex);
    if (!owner.empty() && a.owner != owner) return;
    wanted.store(false);
    a.owner.clear();
    a.data.reset();
    a.anchor.reset();
    a.heights = false;
    a.cost_ms.reset();
}
std::string set_data(std::string_view owner, std::shared_ptr<const std::string> bytes) noexcept {
    auto& a = asked();
    std::lock_guard lock(a.mutex);
    if (!wanted.load() || a.owner != owner) return "the plugin has no world shader on";
    if (bytes && (bytes->size() % 16 != 0 || bytes->size() > most_data_bytes)) return "a world shader's data is whole float4s, at most 64 KB";
    a.data = std::move(bytes);
    ++a.data_version;
    return {};
}
void set_anchor(std::string_view owner, std::optional<std::array<float, 3>> at) noexcept {
    auto& a = asked();
    std::lock_guard lock(a.mutex);
    if (a.owner != owner) return;
    a.anchor = at;
    a.anchor_until = at ? GetTickCount64() + 300 : 0;
}
double clock_seconds() noexcept { return std::fmod(static_cast<double>(now_us()) / 1e6, 1000.0); }
std::optional<double> cost_ms(std::string_view owner) noexcept {
    auto& a = asked();
    std::lock_guard lock(a.mutex);
    if (a.owner != owner) return std::nullopt;
    return a.cost_ms;
}
void measure_heights(GroundTop ground_top) noexcept {
    try {
        if (!ground_top) return;
        auto& a = asked();
        std::array<float, 3> eye{};
        std::array<std::size_t, 2> next{};
        bool measuring = wanted.load() && allowed.load();
        {
            std::lock_guard lock(a.mutex);
            measuring = measuring && a.heights && a.eye_known;
            eye = a.eye, next = a.heights_next;
        }
        // What this thread has measured, by slot: which cell, and its highest surface (the same as a.heights_cells,
        // without its lock). And each grid's window offsets, nearest the middle first.
        struct Known {
            std::vector<std::array<int, 2>> cell;
            std::vector<float> top;
            std::vector<bool> measured;
            std::vector<std::array<int, 2>> nearest_first;
            std::array<int, 2> complete_at{}; // the window's first cell when all of it was last found measured
            bool complete{};
        };
        static std::array<Known, 2> known;
        static std::uint64_t near_top_at{};
        static bool near_changed{};
        static bool any_measured{};
        // Not measuring (out of play: a menu, a level loading; or no shader asking for them), all of it is forgotten:
        // another level's heights are not this one's.
        if (!measuring) {
            if (any_measured) {
                any_measured = false;
                for (auto& k : known) {
                    std::fill(k.measured.begin(), k.measured.end(), false);
                    k.complete = false;
                }
                std::lock_guard lock(a.mutex);
                for (auto& grid_cells : a.heights_cells) std::fill(grid_cells.begin(), grid_cells.end(), std::array<float, 4>{});
                std::fill(a.heights_near_top.begin(), a.heights_near_top.end(), 1e30f);
                ++a.heights_version;
            }
            return;
        }
        // This tick's rays: at most height_tick_budget_ms of them, and how long they took (said every 600 frames).
        struct Timed {
            std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
            std::uint64_t rays{};
            double ms() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(); }
            ~Timed() {
                const auto us = static_cast<std::uint64_t>(ms() * 1000.0);
                height_us_total.fetch_add(us), height_rays.fetch_add(rays), height_ticks.fetch_add(1);
                for (auto most = height_us_most.load(); us > most && !height_us_most.compare_exchange_weak(most, us);) {}
            }
        } timed;
        std::array<std::vector<std::pair<std::size_t, std::array<float, 4>>>, 2> found;
        std::array<int, 2> near_first{};
        for (std::size_t g = 0; g < height_grids.size(); ++g) {
            const auto& grid = height_grids[g];
            const auto cells = height_cells_of(grid);
            auto& k = known[g];
            if (k.cell.empty()) {
                k.cell.assign(cells, {0, 0});
                k.top.assign(cells, -1e30f);
                k.measured.assign(cells, false);
                for (int dz = 0; dz < grid.cells; ++dz)
                    for (int dx = 0; dx < grid.cells; ++dx) k.nearest_first.push_back({dx, dz});
                const int middle = grid.cells / 2;
                std::stable_sort(k.nearest_first.begin(), k.nearest_first.end(), [&](const auto& p, const auto& q) {
                    return (p[0] - middle) * (p[0] - middle) + (p[1] - middle) * (p[1] - middle) <
                           (q[0] - middle) * (q[0] - middle) + (q[1] - middle) * (q[1] - middle);
                });
            }
            const int first_x = static_cast<int>(std::floor(eye[0] / grid.size)) - grid.cells / 2;
            const int first_z = static_cast<int>(std::floor(eye[2] / grid.size)) - grid.cells / 2;
            if (g == 0) near_first = {first_x, first_z};
            const auto slot_of = [&](int cell_x, int cell_z) {
                return static_cast<std::size_t>(((cell_x % grid.cells) + grid.cells) % grid.cells +
                                                (((cell_z % grid.cells) + grid.cells) % grid.cells) * grid.cells);
            };
            // One cell's ray, if the tick's time allows (the clock is read here, before a ray, only: read for each
            // of the 16,384 cells looked over, the ones skipped too, it took the budget on a slow CPU). False when
            // the time is up: the rest next tick.
            bool out_of_time = false;
            const auto measure = [&](int cell_x, int cell_z) {
                if (out_of_time || timed.ms() >= height_tick_budget_ms) {
                    out_of_time = true;
                    return false;
                }
                ++timed.rays;
                any_measured = true;
                const float x = (static_cast<float>(cell_x) + 0.5f) * grid.size, z = (static_cast<float>(cell_z) + 0.5f) * grid.size;
                const auto top = ground_top(x, z, eye[1] + height_reach, eye[1] - height_reach);
                const auto slot = slot_of(cell_x, cell_z);
                const float value = top ? *top : -1e30f;
                if (k.measured[slot] && k.cell[slot] == std::array<int, 2>{cell_x, cell_z} && k.top[slot] == value) return true; // no change
                k.cell[slot] = {cell_x, cell_z}, k.top[slot] = value, k.measured[slot] = true;
                found[g].push_back({slot, {value, static_cast<float>(cell_x), static_cast<float>(cell_z), 1.0f}});
                if (g == 0) near_changed = true;
                return true;
            };
            // The window's cells not measured yet, the nearest first (none to look for while the window has not
            // moved since all of it was).
            if (!k.complete || k.complete_at != std::array<int, 2>{first_x, first_z}) {
                int left = grid.new_tick;
                for (const auto& [dx, dz] : k.nearest_first) {
                    if (left <= 0) break;
                    const int cell_x = first_x + dx, cell_z = first_z + dz;
                    const auto slot = slot_of(cell_x, cell_z);
                    if (k.measured[slot] && k.cell[slot] == std::array<int, 2>{cell_x, cell_z}) continue;
                    if (!measure(cell_x, cell_z)) break;
                    --left;
                }
                k.complete = left > 0 && !out_of_time;
                k.complete_at = {first_x, first_z};
            }
            // And some again, in turn.
            for (int again = 0; again < grid.again_tick; ++again) {
                const auto n = (next[g] + static_cast<std::size_t>(again)) % cells;
                if (!measure(first_x + static_cast<int>(n % static_cast<std::size_t>(grid.cells)), first_z + static_cast<int>(n / static_cast<std::size_t>(grid.cells))))
                    break;
            }
            next[g] = (next[g] + static_cast<std::size_t>(grid.again_tick)) % cells;
        }
        // The near grid's highest surface over each block and those round it (height_block), at most four times a
        // second: a cell of the window not measured yet counts as something over everything (unknown).
        std::vector<float> near_top;
        const auto now = GetTickCount64();
        if (near_changed && now - near_top_at >= 250) {
            near_changed = false;
            near_top_at = now;
            const auto& grid = height_grids[0];
            const auto& k = known[0];
            const int n = grid.cells;
            // The highest of each block, then of each block and those round it (blocks wrap round: across the
            // window's edge it only takes in more, so it is never too low).
            std::vector<float> block_top(static_cast<std::size_t>(height_blocks) * height_blocks, -1e30f);
            for (int sz = 0; sz < n; ++sz)
                for (int sx = 0; sx < n; ++sx) {
                    // The window's cell in this slot.
                    const int cell_x = near_first[0] + (((sx - near_first[0]) % n) + n) % n;
                    const int cell_z = near_first[1] + (((sz - near_first[1]) % n) + n) % n;
                    const auto slot = static_cast<std::size_t>(sx + sz * n);
                    const float value = k.measured[slot] && k.cell[slot] == std::array<int, 2>{cell_x, cell_z} ? k.top[slot] : 1e30f;
                    auto& block = block_top[static_cast<std::size_t>(sx / height_block + (sz / height_block) * height_blocks)];
                    block = std::max(block, value);
                }
            near_top.assign(block_top.size(), -1e30f);
            for (int bz = 0; bz < height_blocks; ++bz)
                for (int bx = 0; bx < height_blocks; ++bx) {
                    float most = -1e30f;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx)
                            most = std::max(most, block_top[static_cast<std::size_t>((bx + dx + height_blocks) % height_blocks +
                                                                                      ((bz + dz + height_blocks) % height_blocks) * height_blocks)]);
                    near_top[static_cast<std::size_t>(bx + bz * height_blocks)] = most;
                }
        }
        if (found[0].empty() && found[1].empty() && near_top.empty()) {
            std::lock_guard lock(a.mutex);
            a.heights_next = next;
            return;
        }
        std::lock_guard lock(a.mutex);
        for (std::size_t g = 0; g < found.size(); ++g)
            for (const auto& [slot, cell] : found[g]) a.heights_cells[g][slot] = cell;
        if (!near_top.empty()) {
            a.heights_near_top = std::move(near_top);
            a.heights_near_first = near_first;
        }
        a.heights_next = next;
        ++a.heights_version;
    } catch (...) {}
}
void allow(bool playing, bool in_menu) noexcept {
    const std::uint64_t now = GetTickCount64();
    static std::uint64_t last_allowed{}; // when the shader was last allowed
    static std::uint64_t stopped_since{}; // when the level stopped being in play (0: it is)
    static bool said_off{};
    if (!playing) {
        // Off only once it has been so for a quarter of a second: the game said so for a single frame
        // now and then, and the shader went off, and its depth was given up and found again, so the snow
        // blinked out for a moment (2026-11-12). A menu opened is off a moment later. The game not ready without a
        // menu (a time of day's lighting loading as the weather changed, with the world still drawn) only after 2.5 s:
        // a quarter of a second took the shader off and on again while the world went on (2026-11-16).
        if (!stopped_since) stopped_since = now;
        if (allowed.load() && now - stopped_since < (in_menu ? 250u : 2500u)) return;
        if (allowed.load() && !said_off) {
            said_off = true;
            logging::printf(logging::Level::info, logging::Channel::graphics, "World shader: resting (%s).",
                            in_menu ? "a game menu is open" : "the game is not ready");
        }
        playing_since = 0;
        allowed.store(false);
        return;
    }
    if (said_off && stopped_since) {
        said_off = false;
        logging::printf(logging::Level::info, logging::Channel::graphics, "World shader: back after %llu ms.",
                        static_cast<unsigned long long>(now - stopped_since));
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
    auto& s = shared();
    {
        std::lock_guard lock(s.mutex);
        s.device = device;
        s.queue = queue;
    }
    // Only where the hook would go: it goes in when a plugin first asks for a world shader (ensure_hooks).
    queue_table.store(*reinterpret_cast<void***>(queue));
    list_table.store(*reinterpret_cast<void***>(list));
    if (wanted.load()) ensure_hooks(); // one asked for before the overlay's graphics were up
}

void present(std::uint32_t width, std::uint32_t height) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        const bool ready = original_execute.load() && !s.failed && s.queue;
        active.store(ready && wanted.load() && allowed.load());
        tracking.store(active.load() && lists_followed.load());
        if (!active.load()) {
            s.active_frames = 0;
            s.notes.clear();
            // The world's depth is found again when the shader is back: a level loaded meanwhile has a
            // new one, and the old one is the game's to free.
            if (s.main_depth) {
                if (s.main_depth_hold) s.retiring.push_back({s.fence_value, s.main_depth_hold});
                s.main_depth_hold.Reset();
                s.main_depth = nullptr;
                s.candidate = nullptr;
                s.candidate_run = 0;
                s.finals = {}; // (pictures known from before are not trusted after a rest)
                s.far_known = false; // (and the sky's light settles afresh when it is back)
                main_depth_now.store(nullptr);
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
                           "World shader: the world's last drawing was in the frame's last batch, so the shader had no place before post-processing.");
        }
        s.width = width, s.height = height;
        if (const auto t = now_us(); s.last_present_us && t > s.last_present_us && t - s.last_present_us < 200000) {
            const double step = static_cast<double>(t - s.last_present_us);
            s.frame_us = s.frame_us > 0 ? s.frame_us + (step - s.frame_us) * 0.1 : step;
        }
        s.last_present_us = now_us();
        s.main_missed = s.main_seen ? 0 : s.main_missed + 1;
        // How this frame went, counted; every 600 frames (the first eight times) said in the log.
        if (s.main_depth) {
            ++s.outcomes[s.drawn ? "drawn" : !s.tried ? "the frame's last batch (to the screen) not seen" : (s.why ? s.why : "not drawn")];
            if (!s.drawn && !s.tried && s.finished_frame != s.frame) ++s.outcomes["no finished picture seen this frame"];
            if (!s.drawn && !s.tried && s.skipped_at.size() < 12) s.skipped_at.push_back(s.active_frames);
            if (s.depth_copies > 1) ++s.outcomes["the depth copied more than once"];
            if (++s.outcome_frames >= 600) {
                // Said whenever a frame of them went without the shader (a blink of it, 2026-11-16), else the first
                // eight times at debug.
                // Any frame without it, or drawn into a picture found another way (the snow vanished for an instant on
                // frames counted as drawn, 2026-11-16). Not for the depth copied twice (the game makes it readable twice
                // nearly every frame) or the last frame's copy (a frame or two a stretch): normal, said with the rest.
                bool missed = !s.outcomes.contains("drawn");
                for (const auto& [what, count] : s.outcomes)
                    if (what != "drawn" && what != "the depth copied more than once" && what != "drawn with the last frame's depth") missed = true;
                if (missed || s.outcome_reports < 8) {
                    if (!missed) ++s.outcome_reports;
                    std::string line = "World shader: the last 600 frames:";
                    for (const auto& [what, count] : s.outcomes) line += std::format(" {} {};", what, count);
                    if (!s.skipped_at.empty()) {
                        line += " skipped at frames";
                        for (const auto at : s.skipped_at) line += std::format(" {}", at);
                        line += " after it came on;";
                    }
                    logging::write(missed ? logging::Level::info : logging::Level::debug, logging::Channel::graphics, line);
                }
                s.outcomes.clear();
                s.skipped_at.clear();
                s.outcome_frames = 0;
            }
        }
        s.tried = false;
        s.why = nullptr;
        // The picture the shader goes into: voted for over 30 frames, then kept while it is still written.
        if (s.frame_last) {
            ++s.votes[s.frame_last].count;
            ++s.frames_voted;
        }
        if (s.locked) {
            s.locked_missed = s.locked_seen ? 0 : s.locked_missed + 1;
            if (s.locked_missed > 120) {
                logging::write(logging::Level::info, logging::Channel::graphics, "World shader: the world's picture is no longer written; choosing again.");
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
                logging::printf(logging::Level::debug, logging::Channel::graphics,
                                "World shader: the world's picture is the one written last after the depth in %u of %u frames (format %u).", best->second.count,
                                s.frames_voted, static_cast<unsigned>(best->second.format));
            }
            s.votes.clear();
            s.frames_voted = 0;
        }
        s.frame_last = 0;
        s.depth_copies = 0;
        ++s.active_frames;
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
                            "World shader: not drawn yet; world depth %s, world picture %s.", s.main_depth ? "found" : "not found",
                            s.locked ? "found" : "not found");
        }
    } catch (...) {}
}
void release() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        active.store(false);
        tracking.store(false);
        // The overlay waits for the GPU before it tears down; ours went on the same queue.
        our_list.store(nullptr);
        s.list.Reset();
        s.upload.Reset();
        s.upload_cpu = nullptr;
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
        s.measure_anchored = {};
        s.inside_fence = {};
        s.light_known = s.far_known = false;
        s.light = s.light_raw = s.far_light = 0;
        s.timer_heap.Reset();
        s.timer_readback.Reset();
        s.timer_cpu = nullptr;
        s.timer_waiting = {};
        s.relearning = false;
        s.cameras_known = 0;
        s.nears.clear();
        s.held_near = 0;
        s.shown_near = 0;
        s.near_disagreed = 0;
        s.last_focal = 0;
        s.focal_changed_frame = 0;
        s.measured_frames = 0;
        s.depth_copied_frame = ~std::uint64_t{0};
        s.chosen_format = DXGI_FORMAT_UNKNOWN;
        s.last_drawn_frame = ~std::uint64_t{0};
        s.views.Reset();
        s.targets.Reset();
        s.pipelines.clear();
        s.root.Reset();
        s.notes.clear();
        s.heights_uploaded = {}; // the next upload buffer is new: the heights and data go into each of its parts again
        s.data_uploaded = {};
        s.inside_heights_uploaded = {};
        s.inside_data_uploaded = {};
        s.inside_frame = ~std::uint64_t{0};
        s.timer_key = {};
        s.budget_key = 0, s.budget_ms = 0, s.budget_frames = 0, s.budget_long = 0;
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
void on_barrier(ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& b, BarrierFn original) noexcept {
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
                        n.final_after = b.Transition.StateAfter;
                        n.final_format = f;
                    }
                    if (finished && !n.composite && f != DXGI_FORMAT_UNKNOWN && d.Width == s.depth_width && d.Height == s.depth_height &&
                        d.MipLevels == 1 && d.DepthOrArraySize == 1) {
                        const std::uint64_t kind = std::uint64_t{static_cast<std::uint32_t>(f)} | (std::uint64_t{static_cast<std::uint32_t>(d.Flags)} << 8) |
                                                   ((b.Transition.StateBefore & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ? (std::uint64_t{1} << 40) : 0);
                        if (!s.locked || kind == s.locked) {
                            n.last_picture = resource;
                            n.last_after = b.Transition.StateAfter;
                            n.last_format = f;
                            ++n.finished_count;
                        }
                        // One of the pictures the screen takes (the final scene's, as the screen's list has finished it on the
                        // frames before), finished on a list before the screen's barrier: there may be no moment between
                        // batches before the game reads it, so the shader is drawn here, on this list, just before the game's
                        // barrier. Every finished one is looked at, not only a list's first. Only those: the game finishes
                        // pictures of the same kind on the way (the lit scene, before more passes and the water and glass
                        // drawn into the final one), and drawn into the first of them on every frame, the water drawn after
                        // it covered the shader, in a square (2026-11-16).
                        const auto known_final = std::find(s.finals.begin(), s.finals.end(), resource);
                        const bool final_one = known_final != s.finals.end() &&
                                               s.frame - s.finals_frame[static_cast<std::size_t>(known_final - s.finals.begin())] <= 4;
                        const auto depth_state = n.depth_state_known ? n.depth_state : s.depth_state;
                        const bool depth_done = (n.depth_state_known || s.depth_state_known) && (depth_state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
                                                !(depth_state & D3D12_RESOURCE_STATE_DEPTH_WRITE);
                        if (s.locked && kind == s.locked && final_one && !n.drawn_inside) {
                            n.inside_why = s.drawn || s.inside_frame == s.frame ? "already drawn this frame"
                                         : !depth_done ? "the depth not done"
                                         : s.unsafe || n.unsafe || s.depth_elsewhere ? "a split or aliasing barrier, or another queue"
                                         : b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? "a barrier on one subresource"
                                         : nullptr;
                            if (!n.inside_why) {
                                if (draw_inside(s, list, resource, b.Transition.StateBefore, original)) {
                                    n.drawn_inside = true;
                                    n.inside_slot = s.inside_last_slot;
                                    s.inside_frame = s.frame;
                                } else {
                                    n.inside_why = "its state on that list not all known, or not ready";
                                }
                            }
                        } else if (s.locked && kind == s.locked && !final_one && !n.inside_why) {
                            n.inside_why = "not a picture the screen takes";
                        }
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
        // written before a batch with none is the one the shader goes into: still before the game's
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
            // One of the kind the shader goes into, just written: this frame's picture.
            if (s.locked && kind == s.locked) {
                n.world = true;
                n.colour = resource;
                n.colour_state_known = true;
                n.colour_state = b.Transition.StateAfter;
                colour_now.store(resource);
            }
            if (s.pictures_said.size() < 8 && s.pictures_said.insert(resource).second)
                logging::printf(logging::Level::debug, logging::Channel::graphics,
                                "World shader: a full-size float picture written (%llux%u, format %u, flags 0x%x, state 0x%x to 0x%x).",
                                static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.Flags),
                                static_cast<unsigned>(b.Transition.StateBefore), static_cast<unsigned>(b.Transition.StateAfter));
            return;
        }
        // The main depth buffer: where it goes, so our list knows whether it can read it as it is. Its copy is never
        // recorded on the game's lists (one put on them hung the GPU).
        if (resource == main_depth_now.load(std::memory_order_relaxed)) {
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            // Seen only as it is finished (from being written to being read): the game went on moving the one it had
            // made before for something else after making the world's anew, so it was never missed, the new one never
            // took over, and the shader went without a copy of the depth for the 300 presents a smaller one waits, then
            // took the 960x540 one (the snow off for seconds at a time, 2026-11-16).
            constexpr auto depth_read = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if ((b.Transition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE) && (b.Transition.StateAfter & depth_read)) s.main_seen = true;
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
        // not been seen for smaller_after presents (the screen got smaller, or a render scale below 100%).
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
        } else if (s.main_depth && desc.Width * desc.Height <= UINT64{s.depth_width} * s.depth_height && s.main_missed < smaller_after) {
            return;
        }
        s.candidate = nullptr;
        s.candidate_run = 0;
        // None yet (the shader was resting): the screen's own size first; the 960x540 one came by first
        // and the shader went into a picture of another size until the big one took over (2026-11-11).
        if (!s.main_depth && desc.Width < s.width && s.main_missed < smaller_after) return;
        // A new main depth buffer (a resize, a level): its views are looked for afresh; and the game has made its pictures
        // anew too, so the screen's ones known from before are forgotten.
        s.finals = {};
        s.far_known = false; // (the sky's light taken afresh: a time of day changed, most often)
        if (s.main_depth_hold) s.retiring.push_back({s.fence_value, s.main_depth_hold});
        s.main_depth_hold = resource;
        s.main_depth = resource;
        main_depth_now.store(resource);
        s.depth_state_known = false;
        s.main_seen = true;
        s.main_missed = 0;
        s.depth_width = static_cast<std::uint32_t>(desc.Width);
        s.depth_height = desc.Height;
        logging::printf(logging::Level::info, logging::Channel::graphics,
                        "World shader: the world's depth is %llux%u, format %u, %u mip level(s), barrier on subresource %s, now in state 0x%x.",
                        static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.MipLevels),
                        b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? "all" : "0",
                        static_cast<unsigned>(b.Transition.StateAfter));
        auto& n = s.notes[list];
        n.depth_state_known = true;
        n.depth_state = b.Transition.StateAfter;
    } catch (...) {}
}
} // namespace dingosdk::world_shader
