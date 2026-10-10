#pragma once
#include "overlay.h"
#include "Engine/Core/Console/command_registry.h"
#include "Extension/Skater/free_flight.h"
#include "gpu_diagnostics.h"
#include "skate_menu.h"
#include "skate_style.h"

#include <Windows.h>
#include <Xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "Engine/Core/Hooks/hooks.h"
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace dingosdk::overlay::detail {
using Microsoft::WRL::ComPtr;

using dingosdk::overlay::record_device_failure;

using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);

using CreateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

using CreateHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

using FullscreenFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);

using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
    const UINT*, IUnknown* const*);

using DllNotificationFn = void(CALLBACK*)(ULONG, const void*, void*);

using RegisterDllNotificationFn = LONG(NTAPI*)(ULONG, DllNotificationFn, void*, void**);

using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

using SetCursorPosFn = BOOL(WINAPI*)(int, int);

struct Hook { void* target = nullptr; void* original = nullptr; };

struct Frame {
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fence = 0;
};

struct Input { HWND window; UINT message; WPARAM wparam; LPARAM lparam; };

struct Binding {
    IUnknown* identity = nullptr;
    ComPtr<ID3D12CommandQueue> queue;
    HWND window = nullptr;
};

using ConsoleToken = dingosdk::console::CompletionToken;

struct ConsoleRow {
    dingosdk::ConsoleLogLine entry;
    std::string formatted, plain;
};

// Retained intentionally for process lifetime: hook trampolines and callbacks
// must never point at C++ objects destroyed by static teardown or DLL unload.

struct State {
    std::recursive_mutex render_mutex;
    std::mutex hook_mutex;
    std::mutex input_mutex;
    std::recursive_mutex cursor_mutex;
    bool cursor_released = false;
    bool cursor_clip_pending = false;
    RECT cursor_clip{};
    std::deque<Input> input;
    std::array<bool, 256> game_keys{}; // Window thread only; forwarded message state.
    unsigned game_mouse_buttons = 0;
    std::atomic<bool> visible{false};
    std::atomic<bool> editor_visible{false}, editor_exit_requested{false}, editor_flight{false};
    std::atomic<ULONGLONG> editor_input_time{0};
    // Raw mouse seen on the game window (WM_INPUT). The park editor's look and
    // buttons use it because Skate's controller mode stops the legacy cursor
    // and button state from updating until the game is paused.
    std::atomic<long> raw_mouse_x{0}, raw_mouse_y{0};
    std::atomic<unsigned> raw_mouse_buttons{0}; // physical: 1 left, 2 right, 4 middle
    std::atomic<ULONGLONG> raw_mouse_time{0};
    // The mouse registration in effect has RIDEV_NOLEGACY: Windows does not
    // move the cursor, so the overlay moves it from raw input while it owns it.
    std::atomic<bool> mouse_legacy_blocked{false};
    std::atomic<bool> console_visible{false};
    std::atomic<bool> console_character_pending{false};
    std::atomic<bool> console_escape_pending{false};
    std::atomic<bool> console_focus_requested{false};
    // Multiplayer text chat (chat_overlay.cpp): T opens it while in a session.
    std::atomic<bool> chat_visible{false}, chat_available{false};
    std::atomic<bool> chat_character_pending{false}, chat_escape_pending{false}, chat_focus_requested{false};
    std::atomic<bool> chat_command_requested{false}; // opened with "/": the box starts with it
    // The Multiplayer page drawn in the game's pause menu (hub_page.cpp). hub_pointer: it is
    // on screen, so the overlay watches the mouse (position, buttons, wheel) while the game
    // keeps it. hub_typing: one of its text boxes has the keyboard, which the overlay takes
    // for that long, as for chat.
    std::atomic<bool> hub_pointer{false}, hub_typing{false};
    std::atomic<bool> input_attached{false};
    std::atomic<bool> stop{false};
    std::atomic<HWND> window{nullptr};
    struct WindowHook { WNDPROC previous{}; bool installed{}; };
    std::recursive_mutex window_hook_mutex;
    std::map<HWND, WindowHook> window_hooks;
    std::atomic<bool> started{false};
    bool ready = false;
    bool ui_was_interactive = false;
    std::uint32_t mapped_mouse_event = 0; // Render thread: the last queued mouse position mapped into the back buffer.
    std::atomic<bool> failed{false};
    bool win32_ready = false;
    bool dx12_ready = false;
    std::atomic<bool> freecam_controller_active{false};
    bool show_on_ready = false;
    bool force_windowed = false;
    HANDLE stop_event = nullptr;
    HANDLE provider_event = nullptr;
    void* dll_notification_cookie = nullptr;
    Hook factory, create, create_hwnd, present, present1, resize, resize1, fullscreen;
    Hook clip_cursor, set_cursor_pos;
    std::vector<Binding> bindings;
    IUnknown* swapchain_identity = nullptr;
    HWND swapchain_window = nullptr;
    std::atomic<bool> selected_window_destroyed{false};
    // "ReSkate loaded" is queued as a notice the first time the overlay draws.
    bool loaded_notice_posted = false;
    std::chrono::steady_clock::time_point last_selected_present{};
    std::chrono::steady_clock::time_point next_setup_attempt{};
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12DescriptorHeap> rtvs, srvs;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 next_fence = 0;
    std::uint64_t submitted_draws = 0;
    std::uint64_t rendered_frames = 0;
    std::vector<Frame> frames;
    std::vector<UINT64> upload_fences;
    ImGuiContext* context = nullptr;
    dingosdk::overlay::CallbacksV3 callbacks;
    dingosdk::overlay::Model model;
    std::chrono::steady_clock::time_point last_model{};
    dingosdk::overlay::SkateMenu menu;
    dingosdk::overlay::ParkEditorUI editor;
    std::deque<ConsoleRow> console_lines;
    ImGuiTextFilter console_filter;
    bool console_show_info = true, console_show_warnings = true, console_show_errors = true;
    bool console_timestamps = true;
    std::array<bool, dingosdk::logging::context_count> console_contexts = [] {
        std::array<bool, dingosdk::logging::context_count> value{}; value.fill(true); return value;
    }();
    std::array<bool, dingosdk::logging::channel_count> console_sources = [] {
        std::array<bool, dingosdk::logging::channel_count> value{}; value.fill(true); return value;
    }();
    int console_cursor = 0;
    std::string console_clicked_completion;
    int console_clicked_cursor = 0;
    std::vector<std::string> console_suggestions;
    std::vector<dingosdk::console::Suggestion> console_suggestion_rows;
    float console_suggestions_fraction = 0.65f;
    bool console_resizing_suggestions = false;
    bool console_layout_valid = false;
    ImVec2 console_position{}, console_size{};
    std::array<char, 1024> console_input{};
    std::deque<std::string> console_history;
    int console_history_position = -1;
    std::string console_history_draft;
    std::uint64_t console_log_sequence = 0;
    bool console_autoscroll = true;
    bool console_scroll_to_bottom = false;
    bool console_force_scroll = false;
    bool console_was_at_bottom = true;
    bool console_input_active = false;
    std::vector<std::string> completion_candidates;
    std::size_t completion_index = 0;
    int completion_begin = 0;
    int completion_end = 0;
    std::string completion_last_buffer;

};

State& state();

extern thread_local bool inside_present;

inline constexpr DWORD render_fence_timeout_ms = 2000;

inline constexpr std::size_t maximum_bindings = 16;

inline constexpr std::size_t maximum_console_lines = 2048;

inline constexpr std::size_t maximum_console_line_length = 8300;

struct PresentGuard {
    bool outermost = !inside_present;
    PresentGuard() { if (outermost) inside_present = true; }
    ~PresentGuard() { if (outermost) inside_present = false; }
};

template<typename Function> Function original(const Hook& hook) {
    return reinterpret_cast<Function>(hook.original);
}

bool install(Hook& hook, void* target, void* detour);

bool is_input(UINT message);

bool interactive_visible(const State& s);

bool owns_menu_cursor(const State& s);

// Gives input back to the game; hide_menu also closes every overlay surface (overlay.cpp).
void restore_input(bool hide_menu);

// The game window's procedure while the overlay is attached (overlay_input.cpp).
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp);

// Swapchain selection, device objects and the per-frame draw (overlay_render.cpp).
bool completed(UINT64 value, DWORD timeout_ms);
void destroy_graphics();
ComPtr<IUnknown> object_identity(IUnknown* object);
Binding* find_binding(IUnknown* identity);
void guarded_render(IDXGISwapChain* chain, UINT flags) noexcept;

// The command console (overlay_console.cpp).
void ingest_console_log(std::vector<dingosdk::overlay::ConsoleLogLine> lines);
void draw_console();

// Corner notices (overlay_notices.cpp).
bool notices_pending();
void draw_notices();

// Multiplayer text chat in the bottom-right corner (chat_overlay.cpp).
// chat_pending also refreshes the feed, so it runs every presented frame.
bool chat_pending();
void draw_chat();
// Game debug text (game_text_overlay.cpp): polled every presented frame.
bool game_text_pending();
void draw_game_text();
// ReSkate's S.K.A.T.E. throwdown HUD (skate_hud_overlay.cpp): polled every presented frame.
bool skate_hud_pending();
void draw_skate_hud();
// ReSkate's nametags (nametag_overlay.cpp): polled every presented frame.
bool nametags_pending();
void draw_nametags();
// The profiler's HUD (drawn whenever it is on) and window (only in an interactive frame),
// perf_overlay.cpp.
bool perf_hud_pending();
void draw_perf_hud();
void draw_perf_window();
// Queues a private multiplayer action from outside the menu (multiplayer_menu.cpp).
bool queue_multiplayer_action(const char* action, const std::string& argument);
bool queue_multiplayer_action(const char* action, const std::string& argument, const std::string& password);
// The pause menu's Multiplayer page (hub_page.cpp): polled every presented frame.
bool hub_page_pending();
void draw_hub_page();
}
