#include "Engine/Core/Log/logging.h"
#include "overlay_internal.h"
#include "input_capture.h"
#include "cursor.h"
#include "park_previews.h"
#include "chat_emotes.h"

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
    return s.visible.load() || s.console_visible.load() || s.editor_visible.load() || s.chat_visible.load() ||
           s.hub_typing.load() || s.prompt_pointer.load() || s.item_search_visible.load();
}

void restore_input(bool hide_menu) {
    auto& s = state();
    if (hide_menu) {
        s.visible.store(false);
        s.console_visible.store(false);
        s.chat_visible.store(false);
        s.hub_typing.store(false);
        s.item_search_visible.store(false);
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

HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* chain, UINT interval, UINT flags) {
    PresentGuard guard;
    if (guard.outermost) guarded_render(chain, flags);
    const HRESULT result = original<PresentFn>(state().present)(chain, interval, flags);
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
    const HRESULT result = original<Present1Fn>(state().present1)(chain, interval, flags, parameters);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        ComPtr<ID3D12Device> device;
        chain->GetDevice(IID_PPV_ARGS(&device));
        record_device_failure(device.Get(), result, "Present1");
        request_stop();
    }
    return result;
}

template<std::size_t I>
HRESULT STDMETHODCALLTYPE present_more(IDXGISwapChain* chain, UINT interval, UINT flags) {
    PresentGuard guard;
    if (guard.outermost) guarded_render(chain, flags);
    return original<PresentFn>(state().present_more[I])(chain, interval, flags);
}
template<std::size_t I>
HRESULT STDMETHODCALLTYPE present1_more(IDXGISwapChain1* chain, UINT interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    PresentGuard guard;
    if (guard.outermost) guarded_render(chain, flags);
    return original<Present1Fn>(state().present1_more[I])(chain, interval, flags, parameters);
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
template<std::size_t I>
HRESULT STDMETHODCALLTYPE resize_more(IDXGISwapChain* chain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) {
    std::lock_guard lock(state().render_mutex);
    if (!before_resize(chain)) return DXGI_ERROR_WAS_STILL_DRAWING;
    return original<ResizeFn>(state().resize_more[I])(chain, count, width, height, format, flags);
}
template<std::size_t I>
HRESULT STDMETHODCALLTYPE resize1_more(IDXGISwapChain3* chain, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags, const UINT* masks, IUnknown* const* queues) {
    std::lock_guard lock(state().render_mutex);
    if (!before_resize(chain)) return DXGI_ERROR_WAS_STILL_DRAWING;
    return original<Resize1Fn>(state().resize1_more[I])(chain, count, width, height, format, flags, masks, queues);
}
// The module an address is in, for the log.
std::string module_of_address(void* address) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) || !module) return "unknown";
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c) if (*c == L'\\') name = c + 1;
    std::string text;
    for (; *name; ++name) text += static_cast<char>(*name);
    return text;
}
// Hooks the presentation and resize of the swapchain under a wrapper, when the game was given
// one: a presentation that does not go through the wrapper still reaches the overlay then.
// Returns that swapchain's identity (null when `chain` is no wrapper): it presents with the
// queue the wrapper was made with, and does not give one itself when asked.
ComPtr<IUnknown> hook_wrapped_swapchain(IDXGISwapChain* chain) {
    auto& s = state();
    // Streamline's wrappers give the object they wrap for this interface (its
    // StreamlineRetrieveBaseInterface); anything else says no.
    static constexpr GUID base_interface{0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
    ComPtr<IUnknown> under;
    if (FAILED(chain->QueryInterface(base_interface, reinterpret_cast<void**>(under.GetAddressOf()))) || !under) return {};
    ComPtr<IDXGISwapChain> plain;
    ComPtr<IDXGISwapChain3> extended;
    if (FAILED(under.As(&plain)) || FAILED(under.As(&extended)) || plain.Get() == chain) return {};
    auto** base = *reinterpret_cast<void***>(plain.Get());
    auto** table = *reinterpret_cast<void***>(extended.Get());
    const auto add = [](Hook& first, auto& more, void* target, const auto& detours) {
        if (first.target == target) return true;
        for (const auto& hook : more) if (hook.target == target) return true;
        for (std::size_t slot = 0; slot < more.size(); ++slot)
            if (!more[slot].target) return install(more[slot], target, detours[slot]);
        return false;
    };
    const std::array presents{reinterpret_cast<void*>(present_more<0>), reinterpret_cast<void*>(present_more<1>), reinterpret_cast<void*>(present_more<2>)};
    const std::array presents1{reinterpret_cast<void*>(present1_more<0>), reinterpret_cast<void*>(present1_more<1>), reinterpret_cast<void*>(present1_more<2>)};
    const std::array resizes{reinterpret_cast<void*>(resize_more<0>), reinterpret_cast<void*>(resize_more<1>), reinterpret_cast<void*>(resize_more<2>)};
    const std::array resizes1{reinterpret_cast<void*>(resize1_more<0>), reinterpret_cast<void*>(resize1_more<1>), reinterpret_cast<void*>(resize1_more<2>)};
    const bool fresh = std::none_of(s.present_more.begin(), s.present_more.end(), [&](const Hook& hook) { return hook.target == base[8]; }) &&
                       s.present.target != base[8];
    const bool okay = add(s.present, s.present_more, base[8], presents) && add(s.present1, s.present1_more, table[22], presents1) &&
                      add(s.resize, s.resize_more, base[13], resizes) && add(s.resize1, s.resize1_more, table[39], resizes1);
    if (fresh || !okay)
        dingosdk::logging::printf(okay ? dingosdk::logging::Level::info : dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
            okay ? "The game's swapchain is a wrapper (%s) over %s's: both are watched for presentations."
                 : "The game's swapchain is a wrapper (%s) over %s's, which could not be hooked as well.",
            module_of_address((*reinterpret_cast<void***>(chain))[8]).c_str(), module_of_address(base[8]).c_str());
    return okay ? object_identity(plain.Get()) : ComPtr<IUnknown>{};
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
    // The swapchain under a wrapper is the same chain to draw on, by the same queue: when it is
    // the one that presents (on Intel's GPUs it is), the overlay draws there.
    if (const auto under = hook_wrapped_swapchain(chain)) {
        if (auto* alias = find_binding(under.Get())) {
            alias->queue = queue;
            alias->window = description.OutputWindow;
            if (s.swapchain_identity == under.Get()) s.queue = queue;
        } else if (s.bindings.size() < maximum_bindings) {
            s.bindings.push_back({under.Get(), queue, description.OutputWindow});
        }
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
    dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
        "Registered DX12 swapchain and its creation queue. (Its Present is %s's.)", module_of_address(base[8]).c_str());
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
