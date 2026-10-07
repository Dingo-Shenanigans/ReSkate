#include "Engine/Core/Log/logging.h"
#include "overlay_internal.h"
#include "input_capture.h"
#include "cursor.h"
#include "park_previews.h"
#include "chat_emotes.h"
#include "Extension/VR/vr.h"

using namespace dingosdk::overlay::detail;
namespace dingosdk::overlay::detail {
State& state() { static State* value = new State; return *value; }

thread_local bool inside_present = false;

bool install(Hook& hook, void* target, void* detour) {
    if (hook.target) return hook.target == target;
    if (!target) return false;
    void* trampoline = nullptr;
    const auto created = dingosdk::hook_prepare(target, detour, &trampoline);
    if (created != dingosdk::HookOk) {
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Hook creation failed: target=%p status=%ld (%s)", target,
            static_cast<long>(created), dingosdk::hook_status_string(created));
        return false;
    }
    hook.target = target;
    hook.original = trampoline;
    const auto enabled = dingosdk::hook_enable(target);
    if (enabled != dingosdk::HookOk) {
        const auto failure = dingosdk::hook_last_failure();
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::hooks,
            "Hook activation failed: target=%p status=%ld (%s), operation=%s thread=%lu", target,
            static_cast<long>(enabled), dingosdk::hook_status_string(enabled), failure.operation, failure.thread_id);
        dingosdk::hook_remove(target);
        hook = {};
        return false;
    }
    return true;
}

bool is_input(UINT message) {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
        || (message >= WM_KEYFIRST && message <= WM_KEYLAST)
        || message == WM_INPUT;
}

bool interactive_visible(const State& s) {
    return s.visible.load() || s.console_visible.load() || s.editor_visible.load() || s.chat_visible.load();
}

void restore_input(bool hide_menu) {
    auto& s = state();
    if (hide_menu) {
        s.visible.store(false);
        s.console_visible.store(false);
        s.chat_visible.store(false);
        s.editor_visible.store(false);
        s.editor_flight.store(false);
    }
    // Also release ownership during swapchain recreation and shutdown.
    s.input_attached.store(false);
    sync_menu_cursor(true);
    request_window_cursor_sync(s.window.load());
    // Keep the pinned forwarding link until HWND destruction, including while
    // detached. The posted release must still reach its owning window thread;
    // removing the WndProc here can otherwise strand mouse capture after resize.
}

void request_stop_locked() {
    auto& s = state();
    if (s.editor_visible.load() && s.callbacks.queue_debug) {
        std::array<char, 256> result{};
        s.callbacks.queue_debug(s.callbacks.user, {dingosdk::overlay::DebugAction::set_park_editor, false}, result.data(), result.size());
    }
    s.stop.store(true);
    if (s.stop_event) SetEvent(s.stop_event);
    restore_input(true);
}

void request_stop() {
    auto& s = state();
    std::lock_guard lock(s.render_mutex);
    request_stop_locked();
}

namespace {
std::atomic<dingosdk::overlay::PresentObserver> present_before{};
std::atomic<dingosdk::overlay::PresentCompletion> present_after{};
}

// Hands the selected swapchain's frame to the present observer. Returns
// whether it ran, so the completion call matches it.
bool observe_present(IDXGISwapChain* chain, UINT flags) noexcept {
    const auto before = present_before.load(std::memory_order_acquire);
    if (!before || (flags & DXGI_PRESENT_TEST)) return false;
    auto& s = state();
    std::lock_guard lock(s.render_mutex);
    if (s.stop.load() || !s.swapchain || !s.queue || object_identity(chain).Get() != s.swapchain_identity) return false;
    // With VR's chat panel, an open chat alone keeps the VR view (it is typed on the panel).
    const bool chat_on_panel = dingosdk::vr::wants_chat_panel() && !s.visible.load() && !s.console_visible.load() &&
        !s.editor_visible.load();
    dingosdk::vr::set_ui_open(interactive_visible(s) && !chat_on_panel);
    before(s.swapchain.Get(), s.queue.Get());
    return true;
}

void complete_present(bool observed) noexcept {
    if (!observed) return;
    if (const auto after = present_after.load(std::memory_order_acquire)) after();
}

HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* chain, UINT interval, UINT flags) {
    PresentGuard guard;
    if (guard.outermost) guarded_render(chain, flags);
    const bool observed = guard.outermost && observe_present(chain, flags);
    const HRESULT result = original<PresentFn>(state().present)(chain, interval, flags);
    complete_present(observed);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        ComPtr<ID3D12Device> device;
        chain->GetDevice(IID_PPV_ARGS(&device));
        record_device_failure(device.Get(), result, "Present");
        request_stop();
    }
    return result;
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* chain, UINT interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    PresentGuard guard;
    if (guard.outermost) guarded_render(chain, flags);
    const bool observed = guard.outermost && observe_present(chain, flags);
    const HRESULT result = original<Present1Fn>(state().present1)(chain, interval, flags, parameters);
    complete_present(observed);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        ComPtr<ID3D12Device> device;
        chain->GetDevice(IID_PPV_ARGS(&device));
        record_device_failure(device.Get(), result, "Present1");
        request_stop();
    }
    return result;
}

bool before_resize(IDXGISwapChain* chain) {
    auto& s = state();
    if (!s.swapchain) return true;
    const auto candidate = object_identity(chain);
    if (!candidate || candidate.Get() != s.swapchain_identity) return true;
    if (!completed(s.next_fence, 500)) {
        // Do not reset an allocator or release a buffer still used by the GPU.
        // The caller can retry resizing without changing menu visibility.
        dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics, "Resize postponed: overlay GPU work has not completed.");
        return false;
    }
    s.show_on_ready = s.visible.load();
    destroy_graphics();
    return true;
}
HRESULT STDMETHODCALLTYPE resize(IDXGISwapChain* chain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) {
    std::lock_guard lock(state().render_mutex);
    if (!before_resize(chain)) return DXGI_ERROR_WAS_STILL_DRAWING;
    return original<ResizeFn>(state().resize)(chain, count, width, height, format, flags);
}
HRESULT STDMETHODCALLTYPE resize1(IDXGISwapChain3* chain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags, const UINT* masks, IUnknown* const* queues) {
    std::lock_guard lock(state().render_mutex);
    const auto candidate = object_identity(chain);
    if (!candidate || !find_binding(candidate.Get())) {
        return original<Resize1Fn>(state().resize1)(
            chain, count, width, height, format, flags, masks, queues);
    }

    const bool selected = candidate.Get() == state().swapchain_identity;
    if (selected && !before_resize(chain)) return DXGI_ERROR_WAS_STILL_DRAWING;

    // ResizeBuffers1 may change the presentation queue. A heterogeneous queue
    // list cannot be rendered with a single queue. BufferCount == 0 retains the
    // current count, so its queue array still describes every existing buffer.
    ComPtr<ID3D12CommandQueue> replacement_queue;
    bool unsupported_queues = false;
    UINT queue_count = count;
    if (queues && queue_count == 0) {
        DXGI_SWAP_CHAIN_DESC description{};
        if (FAILED(chain->GetDesc(&description))) unsupported_queues = true;
        else queue_count = description.BufferCount;
    }
    if (queues && !unsupported_queues) {
        if (queue_count == 0 || queue_count > DXGI_MAX_SWAP_CHAIN_BUFFERS || !queues[0]
            || FAILED(queues[0]->QueryInterface(IID_PPV_ARGS(&replacement_queue)))
            || replacement_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            unsupported_queues = true;
            replacement_queue.Reset();
        } else {
            const auto first_identity = object_identity(replacement_queue.Get());
            if (!first_identity) unsupported_queues = true;
            for (UINT i = 1; i < queue_count && !unsupported_queues; ++i) {
                ComPtr<ID3D12CommandQueue> entry;
                const auto entry_identity = queues[i] ? object_identity(queues[i]) : nullptr;
                if (!entry_identity || entry_identity.Get() != first_identity.Get()
                    || FAILED(queues[i]->QueryInterface(IID_PPV_ARGS(&entry)))
                    || entry->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
                    unsupported_queues = true;
            }
            if (unsupported_queues) replacement_queue.Reset();
        }
    }
    const auto result = original<Resize1Fn>(state().resize1)(
        chain, count, width, height, format, flags, masks, queues);
    // DXGI owns the queue transition. Commit the candidate only when the real
    // resize accepts it; a failed resize keeps the previous presentation queue.
    if (SUCCEEDED(result) && replacement_queue) {
        if (selected) state().queue = replacement_queue;
        if (auto* binding = find_binding(candidate.Get()))
            binding->queue = replacement_queue;
        dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "ResizeBuffers1 accepted presentation queue change: chain=%p queue=%p buffers=%u",
            chain, replacement_queue.Get(), queue_count);
    } else if (SUCCEEDED(result) && unsupported_queues) {
        dingosdk::logging::printf(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics, "ResizeBuffers1 accepted an unsupported presentation queue list; stopping overlay drawing");
        request_stop_locked();
    }
    return result;
}

HRESULT STDMETHODCALLTYPE create(IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** output) {
    DXGI_SWAP_CHAIN_DESC windowed{};
    if (state().force_windowed && description && !description->Windowed) {
        windowed = *description;
        windowed.Windowed = TRUE;
        description = &windowed;
        dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "Windowed startup: changed initial swapchain fullscreen request to windowed");
    }
    const HRESULT result = original<CreateFn>(state().create)(factory, device, description, output);
    if (SUCCEEDED(result) && output && *output && device) {
        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)))) DingoSDKOverlayBindDx12(*output, queue.Get());
    }
    return result;
}
HRESULT STDMETHODCALLTYPE create_hwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* description, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* restrict_output, IDXGISwapChain1** output) {
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC windowed{};
    if (state().force_windowed && fullscreen && !fullscreen->Windowed) {
        windowed = *fullscreen;
        windowed.Windowed = TRUE;
        fullscreen = &windowed;
        dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "Windowed startup: changed initial HWND swapchain fullscreen request to windowed");
    }
    const HRESULT result = original<CreateHwndFn>(state().create_hwnd)(factory, device, window,
        description, fullscreen, restrict_output, output);
    if (SUCCEEDED(result) && output && *output && device) {
        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)))) DingoSDKOverlayBindDx12(*output, queue.Get());
    }
    return result;
}

HRESULT STDMETHODCALLTYPE set_fullscreen(IDXGISwapChain* chain, BOOL fullscreen, IDXGIOutput* output) {
    if (state().force_windowed && fullscreen) {
        dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "Windowed startup: redirected SetFullscreenState(TRUE) to windowed");
        fullscreen = FALSE;
        output = nullptr;
    }
    return original<FullscreenFn>(state().fullscreen)(chain, fullscreen, output);
}

// The engine only accepts resolutions from the monitor's mode list. With a
// launcher window size (the VR setup), square sizes from low to high end are
// added to that list, plus the requested size, so they can be picked in the
// game's own resolution menu.
constexpr UINT vr_sizes[]{1440, 1600, 1800, 2016, 2304, 2592, 2880, 3240, 3600, 3840};
std::vector<std::pair<UINT, UINT>> extra_modes() {
    const auto& s = state();
    std::vector<std::pair<UINT, UINT>> sizes;
    if (!s.forced_width) return sizes;
    for (const auto size : vr_sizes) sizes.emplace_back(size, size);
    if (s.forced_width != s.forced_height || std::find(std::begin(vr_sizes), std::end(vr_sizes), s.forced_width) == std::end(vr_sizes))
        sizes.emplace_back(s.forced_width, s.forced_height);
    return sizes;
}
template<class Mode, class Function, class Output>
HRESULT modes_with_extra(Function list, Output* output, DXGI_FORMAT format, UINT flags, UINT* count, Mode* modes) {
    const auto extra = extra_modes();
    if (extra.empty() || !count) return list(output, format, flags, count, modes);
    const auto added = static_cast<UINT>(extra.size());
    if (!modes) {
        const HRESULT result = list(output, format, flags, count, nullptr);
        if (SUCCEEDED(result) && *count) *count += added;
        return result;
    }
    if (*count <= added) return list(output, format, flags, count, modes);
    UINT real = *count - added;
    const HRESULT result = list(output, format, flags, &real, modes);
    *count = real;
    if (FAILED(result) || !real) return result;
    // Each extra size copies the format, refresh rate and scaling of the largest real mode.
    const Mode model = modes[real - 1];
    UINT total = real;
    for (const auto& [width, height] : extra) {
        if (std::any_of(modes, modes + total, [&](const Mode& m) { return m.Width == width && m.Height == height; })) continue;
        modes[total] = model;
        modes[total].Width = width;
        modes[total].Height = height;
        ++total;
    }
    std::stable_sort(modes, modes + total, [](const Mode& a, const Mode& b) {
        return a.Width != b.Width ? a.Width < b.Width : a.Height < b.Height;
    });
    *count = total;
    static std::atomic<bool> logged{};
    if (!logged.exchange(true))
        dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
            "Display modes: added %u VR sizes (%ux%u to %ux%u) to the game's resolution list.", total - real,
            vr_sizes[0], vr_sizes[0], vr_sizes[std::size(vr_sizes) - 1], vr_sizes[std::size(vr_sizes) - 1]);
    return result;
}
HRESULT STDMETHODCALLTYPE display_modes(IDXGIOutput* output, DXGI_FORMAT format, UINT flags, UINT* count, DXGI_MODE_DESC* modes) {
    return modes_with_extra(original<ModeListFn>(state().modes), output, format, flags, count, modes);
}
HRESULT STDMETHODCALLTYPE display_modes1(IDXGIOutput1* output, DXGI_FORMAT format, UINT flags, UINT* count, DXGI_MODE_DESC1* modes) {
    return modes_with_extra(original<ModeList1Fn>(state().modes1), output, format, flags, count, modes);
}
void install_mode_hooks_locked(IDXGIFactory* base) {
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    if (FAILED(base->EnumAdapters(0, &adapter)) || FAILED(adapter->EnumOutputs(0, &output))) return;
    auto** table = *reinterpret_cast<void***>(output.Get());
    install(state().modes, table[8], reinterpret_cast<void*>(display_modes));
    ComPtr<IDXGIOutput1> output1;
    if (SUCCEEDED(output.As(&output1))) {
        table = *reinterpret_cast<void***>(output1.Get());
        install(state().modes1, table[19], reinterpret_cast<void*>(display_modes1));
    }
}

HRESULT WINAPI factory(REFIID iid, void** output) {
    const HRESULT result = original<FactoryFn>(state().factory)(iid, output);
    if (FAILED(result) || !output || !*output || state().stop.load()) return result;
    ComPtr<IDXGIFactory> base;
    auto* object = static_cast<IUnknown*>(*output);
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&base)))) return result;
    std::lock_guard lock(state().hook_mutex);
    auto** table = *reinterpret_cast<void***>(base.Get());
    const bool basic = install(state().create, table[10], reinterpret_cast<void*>(create));
    ComPtr<IDXGIFactory2> extended;
    bool hwnd = true;
    if (SUCCEEDED(base.As(&extended))) {
        table = *reinterpret_cast<void***>(extended.Get());
        hwnd = install(state().create_hwnd, table[15], reinterpret_cast<void*>(create_hwnd));
    }
    if (state().forced_width) install_mode_hooks_locked(base.Get());
    if (!basic || !hwnd) dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics, "DXGI factory differs from the hooked implementation; unsupported path ignored.");
    return result;
}

bool install_factory_hook_locked() {
    auto& s = state();
    if (s.factory.target) return true;
    constexpr const wchar_t* providers[] = {L"sl.interposer.dll", L"dxgi.dll"};
    for (const auto* provider : providers) {
        const HMODULE module = GetModuleHandleW(provider);
        if (!module) continue;
        const auto address = reinterpret_cast<void*>(GetProcAddress(module, "CreateDXGIFactory1"));
        if (address && install(s.factory, address, reinterpret_cast<void*>(factory))) return true;
    }
    return false;
}

void CALLBACK dll_notification(ULONG reason, const void*, void*) {
    // LDR_DLL_NOTIFICATION_REASON_LOADED is 1. Do only the loader-lock-safe
    // event signal here; the worker performs module inspection and hook setup.
    if (reason == 1 && state().provider_event) SetEvent(state().provider_event);
}

DWORD WINAPI factory_bootstrap(void*) noexcept {
    auto& s = state();
    while (!s.stop.load()) {
        {
            std::lock_guard lock(s.hook_mutex);
            if (s.stop.load()) return 0;
            if (install_factory_hook_locked()) {
                dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics, "DXGI provider hooked; waiting for a DX12 swapchain.");
                return 0;
            }
        }
        const HANDLE events[] = {s.stop_event, s.provider_event};
        const DWORD count = s.provider_event ? 2 : 1;
        if (WaitForMultipleObjects(count, events, FALSE, 25) == WAIT_OBJECT_0) return 0;
    }
    return 0;
}

bool start_overlay(const dingosdk::overlay::CallbacksV3* callbacks) {
    auto& s = state();
    std::lock_guard lock(s.hook_mutex);
    if (s.started || s.stop.load()) return false;
    dingosdk::overlay::initialize_graphics_diagnostics();
    s.force_windowed = dingosdk::overlay::graphics_option_enabled(L"RESKATE_FORCE_WINDOWED");
    if (s.force_windowed) {
        const auto size = [](const wchar_t* name) -> UINT {
            wchar_t text[16]{};
            const auto count = GetEnvironmentVariableW(name, text, 16);
            const auto value = count && count < 16 ? std::wcstoul(text, nullptr, 10) : 0ul;
            return value >= 320 && value <= 16384 ? static_cast<UINT>(value) : 0;
        };
        s.forced_width = size(L"RESKATE_WINDOW_WIDTH");
        s.forced_height = size(L"RESKATE_WINDOW_HEIGHT");
        if (!s.forced_width || !s.forced_height) s.forced_width = s.forced_height = 0;
    }
    {
        wchar_t game[32768]{};
        const auto length = GetModuleFileNameW(nullptr, game, 32768);
        if (length && length < 32768)
        {
            dingosdk::overlay::start_park_previews(std::filesystem::path(std::wstring(game, length)).parent_path());
            dingosdk::overlay::start_chat_emotes();
        }
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&start_overlay), &pinned)) return false;
    const auto status = dingosdk::hook_initialize();
    if (status != dingosdk::HookOk && status != dingosdk::HookAlreadyInitialized) return false;
    const auto user32 = GetModuleHandleW(L"user32.dll");
    if (!user32 || !install(s.clip_cursor, reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor")),
            reinterpret_cast<void*>(clip_cursor)) ||
        !install(s.set_cursor_pos, reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos")),
            reinterpret_cast<void*>(set_cursor_pos)) || !install_input_capture()) return false;
    if (!s.stop_event) s.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s.stop_event) return false;
    if (callbacks) s.callbacks = *callbacks;
    s.started = true;
    if (install_factory_hook_locked()) {
        dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics, "Waiting for the game's DX12 swapchain creation.");
        return true;
    }
    if (!s.provider_event) s.provider_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s.provider_event && !s.dll_notification_cookie) {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto register_notification = ntdll
            ? reinterpret_cast<RegisterDllNotificationFn>(
                GetProcAddress(ntdll, "LdrRegisterDllNotification"))
            : nullptr;
        if (register_notification)
            register_notification(0, dll_notification, nullptr, &s.dll_notification_cookie);
    }
    const HANDLE thread = CreateThread(nullptr, 0, factory_bootstrap, nullptr, 0, nullptr);
    if (!thread) {
        s.started = false;
        s.callbacks = {};
        return false;
    }
    CloseHandle(thread);
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics, "DXGI provider not loaded yet; deferred hook bootstrap started.");
    return true;
}
}

extern "C" bool DingoSDKOverlayStart(const dingosdk::overlay::Callbacks* callbacks) {
    dingosdk::overlay::CallbacksV3 upgraded;
    if (callbacks) {
        upgraded.user = callbacks->user;
        upgraded.read_model = callbacks->read_model;
        upgraded.queue_load = callbacks->queue_load;
        upgraded.queue_debug = callbacks->queue_debug;
    }
    return start_overlay(&upgraded);
}

extern "C" bool DingoSDKOverlayStartV2(const dingosdk::overlay::CallbacksV2* callbacks) {
    dingosdk::overlay::CallbacksV3 upgraded;
    if (callbacks) {
        upgraded.user = callbacks->user;
        upgraded.read_model = callbacks->read_model;
        upgraded.queue_load = callbacks->queue_load;
        upgraded.queue_debug = callbacks->queue_debug;
        upgraded.queue_offline_feature = callbacks->queue_offline_feature;
    }
    return start_overlay(&upgraded);
}

extern "C" bool DingoSDKOverlayStartV3(const dingosdk::overlay::CallbacksV3* callbacks) {
    return start_overlay(callbacks);
}

extern "C" bool DingoSDKOverlayBindDx12(IDXGISwapChain* chain, ID3D12CommandQueue* queue) {
    if (!chain || !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    auto& s = state();
    std::lock_guard render_lock(s.render_mutex);
    std::lock_guard hook_lock(s.hook_mutex);
    if (!s.started || s.stop.load()) return false;
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(chain->GetDesc(&description)) || !description.OutputWindow
        || description.BufferCount < 2 || description.BufferCount > 8) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(description.OutputWindow, &process);
    if (process != GetCurrentProcessId()) return false;
    ComPtr<IDXGISwapChain3> extended;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&extended)))) return false;
    const auto identity = object_identity(chain);
    if (!identity) return false;
    auto** base = *reinterpret_cast<void***>(chain);
    auto** table = *reinterpret_cast<void***>(extended.Get());
    // COM method indices are the Windows SDK's IDXGISwapChain[1/3] ABI.
    if (!install(s.present, base[8], reinterpret_cast<void*>(present))
        || (s.force_windowed && !install(s.fullscreen, base[10], reinterpret_cast<void*>(set_fullscreen)))
        || !install(s.resize, base[13], reinterpret_cast<void*>(resize))
        || !install(s.present1, table[22], reinterpret_cast<void*>(present1))
        || !install(s.resize1, table[39], reinterpret_cast<void*>(resize1))) {
        dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics, "Swapchain hook setup incomplete; candidate ignored.");
        return false;
    }

    if (auto* existing = find_binding(identity.Get())) {
        existing->queue = queue;
        existing->window = description.OutputWindow;
        if (s.swapchain_identity == identity.Get()) s.queue = queue;
        return true;
    }
    if (s.bindings.size() >= maximum_bindings) {
        const auto evict = std::find_if(s.bindings.begin(), s.bindings.end(),
            [&](const Binding& candidate) { return candidate.identity != s.swapchain_identity; });
        if (evict == s.bindings.end()) return false;
        s.bindings.erase(evict);
    }
    s.bindings.push_back({identity.Get(), queue, description.OutputWindow});
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics, "Registered DX12 swapchain and its creation queue.");
    return true;
}

extern "C" void DingoSDKOverlayRequestStop() {
    request_stop();
}

bool dingosdk::overlay::keyboard_shortcuts_allowed() noexcept {
    auto &s = state();
    return s.started.load() && !s.stop.load() && !interactive_visible(s);
}

extern "C" void DingoSDKOverlayGetStatus(dingosdk::overlay::Status* output) {
    if (!output) return;
    auto& s = state();
    std::lock_guard lock(s.render_mutex);
    *output = {s.started.load(), s.swapchain != nullptr, s.ready, s.visible.load(),
        s.stop.load(), s.failed.load(), s.rendered_frames};
}

namespace dingosdk::overlay {
void set_present_observer(PresentObserver before, PresentCompletion after) noexcept {
    // Under the render lock, so no Present is between its two calls.
    std::lock_guard lock(detail::state().render_mutex);
    detail::present_after.store(after, std::memory_order_release);
    detail::present_before.store(before, std::memory_order_release);
}
}
