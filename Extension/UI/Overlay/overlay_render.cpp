#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Profiling/profiler.h"
#include "overlay_internal.h"
#include "Extension/HallOfMeat/hom_grade.h"
#include "Extension/Trainer/trainer_page.h"
#include "Extension/HallOfMeat/hall_of_meat_hud.h"
#include "Extension/HallOfMeat/hom_art.h"
#include "park_previews.h"
#include "chat_emotes.h"
#include "input_capture.h"
#include "cursor.h"

namespace dingosdk::overlay::detail {

bool completed(UINT64 value, DWORD timeout_ms) {
    auto& s = state();
    if (!value) return true;
    if (!s.fence) return false;
    const auto done = s.fence->GetCompletedValue();
    if (done == UINT64_MAX) {
        record_device_failure(s.device.Get(), DXGI_ERROR_DEVICE_REMOVED, "Fence::GetCompletedValue");
        return false;
    }
    if (done >= value) return true;
    if (!s.fence_event || FAILED(s.fence->SetEventOnCompletion(value, s.fence_event))) return false;
    if (WaitForSingleObject(s.fence_event, timeout_ms) != WAIT_OBJECT_0) return false;
    const auto after = s.fence->GetCompletedValue();
    return after != UINT64_MAX && after >= value;
}

void destroy_graphics() {
    auto& s = state();
    clear_park_previews();
    restore_input(false);
    {
        std::lock_guard lock(s.input_mutex);
        s.input.clear();
    }
    ImGuiContext* previous = ImGui::GetCurrentContext();
    if (s.context) {
        ImGui::SetCurrentContext(s.context);
        if (s.dx12_ready) ImGui_ImplDX12_Shutdown();
        if (s.win32_ready) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(s.context);
        ImGui::SetCurrentContext(previous == s.context ? nullptr : previous);
    }
    s.context = nullptr;
    s.win32_ready = s.dx12_ready = s.ready = false;
    s.ui_was_interactive = false;
    s.frames.clear();
    hom_grade_release();
    s.commands.Reset(); s.rtvs.Reset(); s.srvs.Reset(); s.fence.Reset(); s.device.Reset();
    if (s.fence_event) CloseHandle(s.fence_event);
    s.fence_event = nullptr;
    s.next_fence = 0;
    s.submitted_draws = 0;
    s.upload_fences.clear();
}

ComPtr<IUnknown> object_identity(IUnknown* object) {
    ComPtr<IUnknown> identity;
    if (object) object->QueryInterface(IID_PPV_ARGS(&identity));
    return identity;
}

std::uint64_t client_area(HWND window) {
    RECT rectangle{};
    if (!window || !IsWindow(window) || !GetClientRect(window, &rectangle)) return 0;
    const auto width = std::max<LONG>(0, rectangle.right - rectangle.left);
    const auto height = std::max<LONG>(0, rectangle.bottom - rectangle.top);
    return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
}

Binding* find_binding(IUnknown* identity) {
    auto& bindings = state().bindings;
    const auto found = std::find_if(bindings.begin(), bindings.end(),
        [identity](const Binding& candidate) { return candidate.identity == identity; });
    return found == bindings.end() ? nullptr : &*found;
}

bool select_presented_swapchain(IDXGISwapChain* presented) {
    auto& s = state();
    const auto identity = object_identity(presented);
    if (!identity) return false;
    Binding* binding = find_binding(identity.Get());
    if (!binding) return false;

    const auto now = std::chrono::steady_clock::now();
    if (s.swapchain_identity == identity.Get()) {
        s.last_selected_present = now;
        return true;
    }

    bool replace = !s.swapchain;
    if (s.swapchain) {
        const auto candidate_area = client_area(binding->window);
        const auto selected_area = client_area(s.swapchain_window);
        const bool selected_invalid = !s.swapchain_window || !IsWindow(s.swapchain_window)
            || s.selected_window_destroyed.load();
        const bool same_window = binding->window == s.swapchain_window;
        const auto foreground = GetForegroundWindow();
        const bool selected_foreground = foreground == s.swapchain_window;
        const bool candidate_foreground = foreground == binding->window
            && binding->window != s.swapchain_window;
        const bool selected_idle = s.last_selected_present.time_since_epoch().count() != 0
            && now - s.last_selected_present > std::chrono::seconds(1)
            && candidate_area >= selected_area;
        // A game can present multiple chains into the same HWND. Once one has
        // initialized successfully, alternating presentations from a sibling
        // chain must not repeatedly tear down and recreate the ImGui backend.
        // A same-window candidate may take over only after the selected chain
        // has become invalid or has stopped presenting long enough to be stale.
        replace = selected_invalid || selected_idle;
        if (!same_window) {
            // Once a valid foreground chain is actively presenting, a larger
            // hidden or auxiliary window must not displace it. Area is only a
            // startup hint before the selected renderer becomes ready.
            replace = replace || (candidate_foreground && !selected_foreground)
                || (!s.ready && !selected_foreground && candidate_area >= selected_area);
        }
    }
    if (!replace) return false;
    if (s.next_fence && !completed(s.next_fence, render_fence_timeout_ms)) return false;

    ComPtr<IDXGISwapChain3> extended;
    if (FAILED(presented->QueryInterface(IID_PPV_ARGS(&extended)))) return false;
    if (s.swapchain) s.show_on_ready = s.visible.load();
    destroy_graphics();
    s.swapchain = std::move(extended);
    s.queue = binding->queue;
    s.swapchain_identity = identity.Get();
    s.swapchain_window = binding->window;
    s.selected_window_destroyed.store(false);
    s.last_selected_present = now;
    s.next_setup_attempt = {};
    s.failed.store(false);
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics, "Selected presenting DX12 swapchain.");
    return true;
}

bool setup_graphics() {
    auto& s = state();
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(s.swapchain->GetDesc(&description)) || description.BufferCount < 2
        || description.BufferCount > 8 || description.SampleDesc.Count != 1
        || !description.OutputWindow) return false;
    dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "Swapchain setup: chain=%p queue=%p size=%ux%u buffers=%u format=%u usage=0x%x flags=0x%x effect=%u windowed=%d",
        s.swapchain.Get(), s.queue.Get(), description.BufferDesc.Width, description.BufferDesc.Height,
        description.BufferCount, static_cast<unsigned>(description.BufferDesc.Format),
        description.BufferUsage, description.Flags, static_cast<unsigned>(description.SwapEffect),
        description.Windowed);
    if ((description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
         description.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) ||
        !(description.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) ||
        (description.BufferUsage & DXGI_USAGE_READ_ONLY)) {
        dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics, "Unsupported or read-only swapchain; overlay drawing disabled.");
        s.failed = true;
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(description.OutputWindow, &pid);
    if (pid != GetCurrentProcessId()) return false;
    if (FAILED(s.swapchain->GetDevice(IID_PPV_ARGS(&s.device)))) return false;
    const auto adapter = s.device->GetAdapterLuid();
    dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "Device=%p adapter_luid=%08lx:%08lx node_count=%u",
        s.device.Get(), static_cast<unsigned long>(adapter.HighPart),
        adapter.LowPart, s.device->GetNodeCount());
    for (const auto* name : {L"sl.interposer.dll", L"sl.dlss_g.dll", L"nvngx_dlssg.dll",
                            L"GameOverlayRenderer64.dll", L"DiscordHook64.dll", L"RTSSHooks64.dll"})
        dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "Graphics module %ls loaded=%d", name, GetModuleHandleW(name) != nullptr);
    if (s.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        dingosdk::logging::printf(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics, "Graphics setup rejected a non-direct presentation queue");
        return false;
    }
    ComPtr<ID3D12Device> queue_device;
    const auto queue_device_result = s.queue->GetDevice(IID_PPV_ARGS(&queue_device));
    if (FAILED(queue_device_result)) {
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Graphics setup could not read the presentation queue device: result=0x%08lx",
            static_cast<unsigned long>(queue_device_result));
        return false;
    }
    const auto queue_adapter = queue_device->GetAdapterLuid();
    if (queue_adapter.HighPart != adapter.HighPart || queue_adapter.LowPart != adapter.LowPart) {
        dingosdk::logging::printf(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics, "Graphics setup rejected a presentation queue from adapter=%08lx:%08lx",
            static_cast<unsigned long>(queue_adapter.HighPart), queue_adapter.LowPart);
        return false;
    }
    const auto swapchain_device_identity = object_identity(s.device.Get());
    const auto queue_device_identity = object_identity(queue_device.Get());
    dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "Presentation queue device=%p adapter_luid=%08lx:%08lx com_identity_match=%d",
        queue_device.Get(), static_cast<unsigned long>(queue_adapter.HighPart), queue_adapter.LowPart,
        swapchain_device_identity && queue_device_identity
            && swapchain_device_identity.Get() == queue_device_identity.Get());
    // Streamline can expose separate COM wrapper identities for a swapchain
    // device and the device returned by its accepted presentation queue. The
    // queue was captured from successful DXGI creation/resize, so adapter and
    // queue-type validation are the portable checks here.
    const auto format = description.BufferDesc.Format;
    if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM
        && format != DXGI_FORMAT_R10G10B10A2_UNORM && format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = description.BufferCount;
    auto graphics_result = s.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s.rtvs));
    if (FAILED(graphics_result)) {
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Graphics setup failed at RTV heap creation: result=0x%08lx",
            static_cast<unsigned long>(graphics_result));
        return false;
    }
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    graphics_result = s.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s.srvs));
    if (FAILED(graphics_result)) {
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Graphics setup failed at SRV heap creation: result=0x%08lx",
            static_cast<unsigned long>(graphics_result));
        return false;
    }
    s.frames.resize(description.BufferCount);
    s.upload_fences.assign(description.BufferCount, 0);
    auto handle = s.rtvs->GetCPUDescriptorHandleForHeapStart();
    const auto stride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < description.BufferCount; ++i) {
        auto& frame = s.frames[i];
        graphics_result = s.swapchain->GetBuffer(i, IID_PPV_ARGS(&frame.buffer));
        if (FAILED(graphics_result)) {
            dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Graphics setup failed at GetBuffer[%u]: result=0x%08lx",
                i, static_cast<unsigned long>(graphics_result));
            return false;
        }
        graphics_result = s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&frame.allocator));
        if (FAILED(graphics_result)) {
            dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Graphics setup failed at command allocator[%u]: result=0x%08lx",
                i, static_cast<unsigned long>(graphics_result));
            return false;
        }
        const auto buffer_desc = frame.buffer->GetDesc();
        ComPtr<IDXGIResource> dxgi_resource;
        DXGI_USAGE usage{};
        const bool usage_available = SUCCEEDED(frame.buffer.As(&dxgi_resource)) &&
            SUCCEEDED(dxgi_resource->GetUsage(&usage));
        dingosdk::logging::printf(dingosdk::logging::Level::debug, dingosdk::logging::Channel::graphics, "Backbuffer[%u]=%p size=%llux%u flags=0x%x usage_available=%d usage=0x%x",
            i, frame.buffer.Get(), buffer_desc.Width, buffer_desc.Height,
            static_cast<unsigned>(buffer_desc.Flags), usage_available, usage);
        if (!(buffer_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
            (usage_available && (usage & DXGI_USAGE_READ_ONLY))) {
            dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics, "Backbuffer is not writable as a render target; overlay drawing disabled.");
            s.failed = true;
            return false;
        }
        frame.rtv = handle;
        s.device->CreateRenderTargetView(frame.buffer.Get(), nullptr, handle);
        handle.ptr += stride;
    }
    graphics_result = s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s.frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&s.commands));
    if (FAILED(graphics_result)) {
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Graphics setup failed at command list creation: result=0x%08lx",
            static_cast<unsigned long>(graphics_result));
        return false;
    }
    graphics_result = s.commands->Close();
    if (FAILED(graphics_result)) {
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Graphics setup failed while closing the command list: result=0x%08lx",
            static_cast<unsigned long>(graphics_result));
        return false;
    }
    graphics_result = s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence));
    if (FAILED(graphics_result)) {
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Graphics setup failed at fence creation: result=0x%08lx",
            static_cast<unsigned long>(graphics_result));
        return false;
    }
    s.commands->SetName(L"ReSkate overlay draw");
    s.fence->SetName(L"ReSkate overlay fence");
    s.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.fence_event) return false;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    s.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(s.context);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    dingosdk::overlay::load_skate_fonts(s.menu);
    // Thumbnails are read from the game's own data at startup; the read is
    // normally long finished by the time the first frame gets here.
    // Emotes reserve their room before the park previews build the atlas, and fill it after.
    const auto emote_count = reserve_chat_emotes(*ImGui::GetIO().Fonts, std::chrono::seconds(5));
    const auto hom_art_count = reserve_hom_art(*ImGui::GetIO().Fonts);
    const auto preview_count = load_park_previews(*ImGui::GetIO().Fonts, std::chrono::seconds(5));
    fill_chat_emotes(*ImGui::GetIO().Fonts);
    fill_hom_art(*ImGui::GetIO().Fonts);
    (void)hom_art_count;
    if (emote_count)
        dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
            "Chat emotes: %zu ready.", emote_count);
    dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
        "Park library: %zu object thumbnails loaded from the game data.", preview_count);
    s.win32_ready = ImGui_ImplWin32_Init(description.OutputWindow);
    ImGui_ImplDX12_InitInfo info;
    info.Device = s.device.Get();
    info.CommandQueue = s.queue.Get();
    info.NumFramesInFlight = static_cast<int>(description.BufferCount);
    info.RTVFormat = format;
    info.SrvDescriptorHeap = s.srvs.Get();
    info.LegacySingleSrvCpuDescriptor = s.srvs->GetCPUDescriptorHandleForHeapStart();
    info.LegacySingleSrvGpuDescriptor = s.srvs->GetGPUDescriptorHandleForHeapStart();
    if (s.win32_ready) s.dx12_ready = ImGui_ImplDX12_Init(&info);
    // Font upload uses the exact swapchain queue, and this pinned backend waits
    // for its own upload fence before returning from device-object creation.
    const bool objects = s.dx12_ready && ImGui_ImplDX12_CreateDeviceObjects();
    ImGui::SetCurrentContext(previous);
    if (!objects) {
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Graphics setup failed in ImGui initialization: win32=%d dx12=%d objects=%d",
            s.win32_ready, s.dx12_ready, objects);
        return false;
    }
    s.window.store(description.OutputWindow);
    {
        std::lock_guard lock(s.window_hook_mutex);
        auto& hook = s.window_hooks[description.OutputWindow];
        if (!hook.installed) {
            const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(description.OutputWindow, GWLP_WNDPROC));
            if (current == window_proc) return false;
            hook.previous = current;
            SetLastError(0);
            const auto proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(description.OutputWindow,
                GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(window_proc)));
            if (!proc && GetLastError() != 0) return false;
            hook.previous = proc;
            hook.installed = true;
        }
    }
    s.input_attached.store(true);
    s.visible.store(s.show_on_ready);
    sync_menu_cursor();
    if (s.console_visible.load()) s.console_focus_requested.store(true);
    s.ready = true;
    s.selected_window_destroyed.store(false);
    const auto keys = dingosdk::launcher::overlay_keys();
    dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
        "DX12 overlay ready. " + dingosdk::launcher::key_name(keys.menu) + " toggles the menu; " +
        dingosdk::launcher::key_name(keys.console) + " toggles the console.");
    return true;
}

bool finite_position(const std::array<float, 3>& position) {
    return std::all_of(position.begin(), position.end(), [](float value) { return std::isfinite(value); });
}

void draw_menu() {
    auto& s = state();
    const auto now = std::chrono::steady_clock::now();
    // This only copies the published model; native world/catalog discovery stays
    // on the client thread at its existing cadence.
    const auto interval = std::chrono::milliseconds(16);
    // Chat alone shows nothing from the model: polling it then would only make
    // the game thread wait on the host's lock.
    const bool model_shown = s.visible.load() || s.console_visible.load() || s.editor_visible.load();
    if (model_shown && now - s.last_model >= interval) {
        s.last_model = now;
        if (s.callbacks.read_model) {
            // Hand the host the model already held: it copies only what changed since.
            dingosdk::overlay::Model next = std::move(s.model);
            next.console_log.clear();
            s.callbacks.read_model(s.callbacks.user, next);
            // Bound the UI work even if a corrupt/native catalog is returned.
            if (next.levels.size() > 128) next.levels.resize(128);
            for (auto& level : next.levels)
                if (level.start_points.size() > 64) level.start_points.resize(64);
            if (next.offline.variables.size() > 512) next.offline.variables.resize(512);
            for (auto& variable : next.offline.variables)
                if (variable.name.size() > 256) variable.name.resize(256);
            if (next.console_log.size() > maximum_console_lines)
                next.console_log.erase(next.console_log.begin(), next.console_log.end() - maximum_console_lines);
            for (auto& line : next.console_log)
                if (line.text.size() > maximum_console_line_length)
                    line.text.resize(maximum_console_line_length);
            next.debug.camera_position_valid &= finite_position(next.debug.camera_position);
            next.debug.skater_position_valid &= finite_position(next.debug.skater_position);
            // The callback owns this fresh batch. Move it into ingestion rather
            // than copying every retained log string a second time each poll.
            ingest_console_log(std::move(next.console_log));
            s.model = std::move(next);
        }
    }
    // Nothing but the chat: the held model may be old, so it must not reopen the editor.
    if (!model_shown) {
        s.editor_flight.store(false);
        return;
    }
    const bool editor_was_visible = s.editor_visible.exchange(s.model.debug.park_editor);
    if (s.model.debug.park_editor) {
        if (!editor_was_visible) s.editor.exit_pending = false;
        bool visible = s.visible.load();
        s.editor.title_font = s.menu.title;
        s.editor.bold_font = s.menu.bold;
        s.editor.heading_font = s.menu.heading;
        s.editor.ui_scale = std::clamp(s.model.menu_scale, dingosdk::min_menu_scale, dingosdk::max_menu_scale);
        static const auto close_key = dingosdk::launcher::key_cap(dingosdk::launcher::overlay_keys().menu);
        s.editor.close_key = close_key;
        ImGui::PushFont(s.menu.body);
        const bool flight = dingosdk::overlay::draw_park_editor(s.editor, s.model, s.callbacks,
            s.console_visible.load(), s.editor_exit_requested.exchange(false), visible);
        ImGui::PopFont();
        s.visible.store(visible);
        s.editor_flight.store(flight);
        s.editor_input_time.store(GetTickCount64());
        return;
    }
    s.editor_flight.store(false);
    if (!s.visible.load()) return;
    bool visible = true;
    dingosdk::overlay::draw_skate_menu(s.menu, s.model, s.callbacks, visible);
    if (!visible) s.visible.store(false);
}

void render(IDXGISwapChain* presented, UINT flags) {
    auto& s = state();
    if (flags & DXGI_PRESENT_TEST) return;
    dingosdk::profiler::record_present();
    std::lock_guard lock(s.render_mutex);
    if (s.stop.load()) {
        if (!s.swapchain || completed(s.next_fence, 0)) {
            restore_input(true);
            destroy_graphics();
            s.queue.Reset();
            s.swapchain.Reset();
            s.swapchain_identity = nullptr;
            s.swapchain_window = nullptr;
            s.bindings.clear();
            s.callbacks = {};
        }
        return;
    }
    if (!select_presented_swapchain(presented)) return;
    if (s.failed.load()) return;
    if (!s.ready) {
        const auto now = std::chrono::steady_clock::now();
        if (now < s.next_setup_attempt) return;
        if (!setup_graphics()) {
            dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics, "Graphics setup incomplete; retrying on a later presentation.");
            destroy_graphics();
            s.next_setup_attempt = now + std::chrono::milliseconds(250);
            return;
        }
    }
    // Keep swapchain/window selection, initialization, stop and resize/fence
    // handling alive while hidden, but do no per-frame UI or backbuffer work --
    // beyond the loaded toast, which draws nothing that takes input.
    // Input hotkeys/controller state are handled independently of ImGui frames.
    // Refreshes the chat feed every frame, which also decides whether T opens it.
    const bool chat_frame = chat_pending();
    const bool game_text_frame = game_text_pending();
    const bool skate_hud_frame = skate_hud_pending();
    const bool nametag_frame = nametags_pending();
    const bool perf_frame = perf_hud_pending() || trainer_hud_pending() || hall_of_meat_hud_pending();
    if (trainer_open_requested()) s.visible.store(true);
    const bool menu_frame = interactive_visible(s);
    if (!menu_frame) {
        if (s.ui_was_interactive) {
            s.ui_was_interactive = false;
            { std::lock_guard input_lock(s.input_mutex); s.input.clear(); }
            ImGuiContext* previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(s.context);
            auto& io = ImGui::GetIO();
            io.ClearEventsQueue(); io.ClearInputKeys(); io.ClearInputMouse();
            io.MouseDrawCursor = false;
            ImGui::SetCurrentContext(previous);
            s.console_input_active = false;
            sync_menu_cursor();
        }
        // Hidden, the overlay still draws while a notice or chat line is on screen.
        if (s.loaded_notice_posted && !notices_pending() && !chat_frame && !game_text_frame && !skate_hud_frame &&
            !nametag_frame && !perf_frame) return;
    } else if (!s.ui_was_interactive) {
        s.ui_was_interactive = true;
        s.last_model = {}; // Reopening immediately reads fresh state.
    }
    const UINT index = s.swapchain->GetCurrentBackBufferIndex();
    if (index >= s.frames.size()) { s.failed = true; restore_input(true); return; }
    auto& frame = s.frames[index];
    // Interposers can replace their underlying buffers. Never submit writes
    // to a cached resource unless it is still the current presented buffer.
    ComPtr<ID3D12Resource> current_buffer;
    const auto current_result = s.swapchain->GetBuffer(index, IID_PPV_ARGS(&current_buffer));
    const auto current_identity = object_identity(current_buffer.Get());
    const auto cached_identity = object_identity(frame.buffer.Get());
    if (FAILED(current_result) || !current_identity || !cached_identity ||
        current_identity.Get() != cached_identity.Get()) {
        dingosdk::logging::printf(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Backbuffer identity changed before draw: index=%u cached=%p current=%p; disabling overlay",
            index, frame.buffer.Get(), current_buffer.Get());
        s.failed = true;
        restore_input(true);
        return;
    }
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.context);
    {
        std::deque<Input> pending;
        { std::lock_guard input_lock(s.input_mutex); pending.swap(s.input); }
        for (const auto& input : pending)
        {
            OverlayInputAccess access;
            ImGui_ImplWin32_WndProcHandler(input.window, input.message, input.wparam, input.lparam);
        }
    }
    const bool interactive = interactive_visible(s);
    sync_menu_cursor();
    if (!interactive) { ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse(); }
    ImGui::GetIO().MouseDrawCursor = owns_menu_cursor(s);
    ImGui_ImplDX12_NewFrame();
    { OverlayInputAccess access; ImGui_ImplWin32_NewFrame(); }
    update_menu_pointer();
    ImGui::NewFrame();
    if (menu_frame) {
        draw_menu();
        draw_console();
        draw_perf_window();
    }
    draw_nametags();
    draw_game_text();
    draw_skate_hud();
    draw_perf_hud();
    draw_trainer_hud();
    draw_hall_of_meat_hud();
    draw_notices();
    draw_chat();
    sync_menu_cursor(); // close buttons also change visibility, without a key message
    ImGui::Render();
    if (ImGui::GetDrawData()->TotalVtxCount == 0) { ImGui::SetCurrentContext(previous); return; }
    const auto upload_slot = static_cast<std::size_t>(s.submitted_draws % s.upload_fences.size());
    // The ImGui backend rotates upload buffers by submitted draw count, while
    // command allocators rotate by swapchain index. Wait for both exact owners
    // so every presented frame receives UI instead of intermittently skipping.
    if (!completed(frame.fence, render_fence_timeout_ms)
        || !completed(s.upload_fences[upload_slot], render_fence_timeout_ms)) {
        ImGui::SetCurrentContext(previous);
        s.failed = true;
        restore_input(true);
        dingosdk::logging::write(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Overlay GPU fence timed out; rendering disabled.");
        return;
    }
    if (FAILED(frame.allocator->Reset()) || FAILED(s.commands->Reset(frame.allocator.Get(), nullptr))) {
        ImGui::SetCurrentContext(previous); s.failed = true; restore_input(true); return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.buffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    // Hall of Meat's colour pass (Skate 3's colour matrix on the game picture) leaves the buffer
    // as a render target itself; otherwise it becomes one here.
    if (!hom_grade_record(s.device.Get(), s.commands.Get(), frame.buffer.Get(), frame.rtv, hom_colour_strength().load()))
        s.commands->ResourceBarrier(1, &barrier);
    s.commands->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {s.srvs.Get()};
    s.commands->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), s.commands.Get());
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    s.commands->ResourceBarrier(1, &barrier);
    ImGui::SetCurrentContext(previous);
    if (FAILED(s.commands->Close())) { s.failed = true; restore_input(true); return; }
    ID3D12CommandList* lists[] = {s.commands.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    const UINT64 signal = ++s.next_fence;
    if (FAILED(s.queue->Signal(s.fence.Get(), signal))) { s.failed = true; restore_input(true); return; }
    frame.fence = signal;
    s.upload_fences[upload_slot] = signal;
    ++s.submitted_draws;
    ++s.rendered_frames;
}

void guarded_render(IDXGISwapChain* chain, UINT flags) noexcept {
    try { render(chain, flags); }
    catch (...) { state().failed = true; restore_input(true); dingosdk::logging::write(dingosdk::logging::Level::error, dingosdk::logging::Channel::graphics, "Overlay exception; disabled."); }
}
}
