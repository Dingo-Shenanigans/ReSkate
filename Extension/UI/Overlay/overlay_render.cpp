#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Profiling/profiler.h"
#include "overlay_internal.h"
#include "Extension/Trainer/trainer_page.h"
#include "Extension/UI/Overlay/game_images.h"
#include "plugin_overlay.h"
#include "screen_look.h"
#include "viewmodel.h"
#include "park_previews.h"
#include "chat_emotes.h"
#include "input_capture.h"
#include "cursor.h"
#include <algorithm>
#include <chrono>
#include <dxgi1_6.h>
#include <imgui_internal.h>

namespace dingosdk::overlay::detail {
void draw_plugin_dialog(); // reskate.ui.dialog (plugin_dialog.cpp)
namespace {
// The overlay's output transfer (setup_graphics), for the screen look (it grades colour in SDR only)
// and the viewmodel, with white's brightness in nits.
int output_transfer{};
float output_white_nits{80};
}

// ImGui draws into the swapchain's back buffer, so its display is the buffer, in the buffer's
// pixels. The Win32 backend sizes the display by the window's client area and places the mouse in
// it; the two differ whenever the window is measured in other units than the buffer: a window
// Windows scales for the display (a 1920x1080 client on a 4K display at 200%, borderless) or a
// buffer stretched to the window. Every mouse position queued since the last frame comes in client
// coordinates and is mapped into the buffer once.
void fit_display_to_buffer(State& s, ID3D12Resource* buffer) {
    auto& io = ImGui::GetIO();
    const auto description = buffer->GetDesc();
    const ImVec2 client = io.DisplaySize;
    const ImVec2 pixels(static_cast<float>(description.Width), static_cast<float>(description.Height));
    if (client.x <= 0 || client.y <= 0 || pixels.x <= 0 || pixels.y <= 0) return;
    io.DisplaySize = pixels;
    const ImVec2 scale(pixels.x / client.x, pixels.y / client.y);
    for (auto& event : ImGui::GetCurrentContext()->InputEventsQueue) {
        if (event.Type != ImGuiInputEventType_MousePos || event.EventId <= s.mapped_mouse_event) continue;
        s.mapped_mouse_event = event.EventId;
        if (event.MousePos.PosX == -FLT_MAX) continue; // the mouse left the window
        event.MousePos.PosX *= scale.x;
        event.MousePos.PosY *= scale.y;
    }
}

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
    s.card_texture.Reset(); s.card_upload.Reset();
    card_image_lost();
    s.menu_map_texture.Reset(); s.menu_map_upload.Reset();
    menu_map_pixels_lost();
    s.mod_icon_texture.Reset(); s.mod_icon_upload.Reset();
    mod_icon_lost();
    release_frame_grab();
    for (auto& texture : s.plugin_textures) texture.Reset();
    for (auto& upload : s.plugin_uploads) upload.Reset();
    plugin_images_lost();
    for (auto& texture : s.store_textures) texture.Reset();
    for (auto& upload : s.store_uploads) upload.Reset();
    store_icons_lost();
    screen_look::release();
    viewmodel::release();
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

// A swapchain that presents without the overlay having seen it made (on some Intel systems
// the game's menus never drew: the chain that was registered at creation is not the one that
// reaches Present). A D3D12 swapchain gives the queue it was made with, which is all a binding
// needs. Render thread, with the render lock held.
Binding* bind_presented(IDXGISwapChain* presented, IUnknown* identity) {
    auto& s = state();
    static std::atomic<unsigned> refused{};
    const auto refuse = [&](const char* why) -> Binding* {
        if (refused.fetch_add(1) < 3)
            dingosdk::logging::printf(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
                "Overlay: a swapchain it did not see created is presenting, and it cannot draw on it (%s).", why);
        return nullptr;
    };
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(presented->GetDesc(&description)) || !description.OutputWindow) return refuse("no description or window");
    if (description.BufferCount < 2 || description.BufferCount > 8) return refuse("an unusual buffer count");
    DWORD process = 0;
    GetWindowThreadProcessId(description.OutputWindow, &process);
    if (process != GetCurrentProcessId()) return refuse("another process's window");
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(presented->GetDevice(IID_PPV_ARGS(&queue))) || !queue) return refuse("it gives no D3D12 queue");
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return refuse("its queue is not a direct one");
    if (s.bindings.size() >= maximum_bindings) {
        const auto evict = std::find_if(s.bindings.begin(), s.bindings.end(),
            [&](const Binding& candidate) { return candidate.identity != s.swapchain_identity; });
        if (evict == s.bindings.end()) return refuse("too many swapchains");
        s.bindings.erase(evict);
    }
    s.bindings.push_back({identity, queue, description.OutputWindow});
    dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
        "Registered a DX12 swapchain first seen as it presented (%ux%u, %u buffers, format %d).",
        description.BufferDesc.Width, description.BufferDesc.Height, description.BufferCount,
        static_cast<int>(description.BufferDesc.Format));
    return &s.bindings.back();
}

bool select_presented_swapchain(IDXGISwapChain* presented) {
    auto& s = state();
    const auto identity = object_identity(presented);
    if (!identity) return false;
    Binding* binding = find_binding(identity.Get());
    if (!binding) binding = bind_presented(presented, identity.Get());
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
    dingosdk::overlay::record_gpu_identity(s.device.Get());
    dingosdk::overlay::watch_device_removal(s.device.Get());
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
        && format != DXGI_FORMAT_R10G10B10A2_UNORM && format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        // Said once for each: with this the overlay never draws, and nothing else says why.
        static std::atomic<int> said{-1};
        if (said.exchange(static_cast<int>(format)) != static_cast<int>(format))
            dingosdk::logging::printf(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
                "Overlay: the game presents in a format the overlay does not draw to (DXGI format %d); its menus stay hidden.",
                static_cast<int>(format));
        return false;
    }

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
    // ImGui's atlas, the map download card's icon, the menu's map pictures, an item's mod icon,
    // plugins' pictures (upload_image), the screen look's, the Mod Manager's icons and a viewmodel's pictures
    heap.NumDescriptors = viewmodel_first_slot + viewmodel_slots;
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
    dingosdk::overlay::watch_draws(s.commands.Get());
    dingosdk::overlay::watch_addresses(s.commands.Get());
    install_depth_probe(s.commands.Get());
    s.fence->SetName(L"ReSkate overlay fence");
    s.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.fence_event) return false;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    s.context = ImGui::CreateContext();
    s.mapped_mouse_event = 0; // a new context numbers its events afresh
    ImGui::SetCurrentContext(s.context);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    dingosdk::overlay::load_skate_fonts(s.menu);
    // Thumbnails are read from the game's own data at startup; the read is
    // normally long finished by the time the first frame gets here.
    // Emotes and Hall of Meat's images reserve their room before the park previews build the atlas, and fill it after.
    // (Five seconds for the three together, not each: this is the game's render thread, and three
    // reads still under way at the first frame held it up to fifteen.)
    const auto waited_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto left = [&] {
        return std::max(std::chrono::milliseconds(0),
            std::chrono::duration_cast<std::chrono::milliseconds>(waited_until - std::chrono::steady_clock::now()));
    };
    const auto emote_count = reserve_chat_emotes(*ImGui::GetIO().Fonts, left());
    reserve_game_images(*ImGui::GetIO().Fonts, left());
    const auto preview_count = load_park_previews(*ImGui::GetIO().Fonts, left());
    fill_chat_emotes(*ImGui::GetIO().Fonts);
    fill_game_images(*ImGui::GetIO().Fonts);
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
    // ImGui draws sRGB colours. A game in HDR presents another kind of buffer, where those same
    // numbers are far too bright and washed out: scRGB (FP16, linear) or HDR10 (10 bits, BT.2020
    // with the PQ curve, while the display is in HDR). The backend's shader converts for those.
    {
        bool hdr_display{};
        ComPtr<IDXGIOutput> output;
        ComPtr<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 display{};
        if (SUCCEEDED(s.swapchain->GetContainingOutput(&output)) && output && SUCCEEDED(output.As(&output6)) &&
            SUCCEEDED(output6->GetDesc1(&display)))
            hdr_display = display.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        constexpr float hdr_white_nits = 200.0f; // how bright the overlay's white is on an HDR display
        const int transfer = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1
                           : format == DXGI_FORMAT_R10G10B10A2_UNORM && hdr_display ? 2 : 0;
        ImGui_ImplDX12_SetOutputTransfer(transfer, hdr_display ? hdr_white_nits : 80.0f);
        output_transfer = transfer;
        output_white_nits = hdr_display ? hdr_white_nits : 80.0f;
        dingosdk::logging::printf(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
            "Overlay output: %s (buffer format %d, display %s).",
            transfer == 1 ? "scRGB" : transfer == 2 ? "HDR10" : "SDR", static_cast<int>(format), hdr_display ? "in HDR" : "in SDR");
    }
    // Font upload uses the exact swapchain queue, and this pinned backend waits
    // for its own upload fence before returning from device-object creation.
    const bool objects = s.dx12_ready && ImGui_ImplDX12_CreateDeviceObjects();
    if (objects && !ImGui_ImplDX12_HasOutputTransfer())
        dingosdk::logging::write(dingosdk::logging::Level::warning, dingosdk::logging::Channel::graphics,
            "Overlay output: this system could not build the HDR-aware shader; drawing in SDR, so an HDR game shows the overlay off-colour.");
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

// The ReSkate menu on a controller: while it is open the pad drives ImGui's navigation (the game
// does not see the pad then), and nothing of it reaches the overlay otherwise. X is left out: ImGui
// holds it for its window switcher.
void feed_menu_gamepad(bool menu_open) {
    auto& io = ImGui::GetIO();
    // Holding a direction in the full screen menu waits before it repeats, then moves at a pace
    // the eye can follow (ImGui's own, made for a keyboard, races through a list).
    const bool slow = menu_open && dingosdk::overlay::menu_fullscreen_shown().load();
    io.KeyRepeatDelay = slow ? 0.42f : 0.275f;
    io.KeyRepeatRate = slow ? 0.16f : 0.05f;
    // A page with its own focus (the full screen ONLINE page) takes the D-pad and the arrow keys itself.
    if (menu_open && dingosdk::overlay::menu_page_owns_pad().load()) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    struct Button { unsigned bit; ImGuiKey key; };
    static constexpr Button buttons[]{
        {0x0001, ImGuiKey_GamepadDpadUp}, {0x0002, ImGuiKey_GamepadDpadDown}, {0x0004, ImGuiKey_GamepadDpadLeft},
        {0x0008, ImGuiKey_GamepadDpadRight}, {0x0010, ImGuiKey_GamepadStart}, {0x0020, ImGuiKey_GamepadBack},
        {0x0040, ImGuiKey_GamepadL3}, {0x0080, ImGuiKey_GamepadR3}, {0x0100, ImGuiKey_GamepadL1}, {0x0200, ImGuiKey_GamepadR1},
        {0x1000, ImGuiKey_GamepadFaceDown}, {0x2000, ImGuiKey_GamepadFaceRight}, {0x8000, ImGuiKey_GamepadFaceUp}};
    static bool fed = false;
    const auto pad = menu_open ? dingosdk::overlay::read_gamepad() : dingosdk::overlay::Gamepad{};
    {
        // A button pressed, a trigger pulled or a stick pushed in the full screen menu hands it to
        // the controller: the pointer hides until the mouse is used again.
        static unsigned before{};
        const bool used = pad.valid && ((pad.buttons & ~before) != 0 || pad.left_trigger > 0.5f || pad.right_trigger > 0.5f ||
                                        std::abs(pad.left_x) > 0.5f || std::abs(pad.left_y) > 0.5f || std::abs(pad.right_x) > 0.5f ||
                                        std::abs(pad.right_y) > 0.5f);
        before = pad.valid ? pad.buttons : 0;
        if (used && slow) dingosdk::overlay::menu_pad_in_charge().store(true);
        if (!menu_open) dingosdk::overlay::menu_pad_in_charge().store(false);
    }
    if (!pad.valid && !fed) return;
    for (const auto& button : buttons) io.AddKeyEvent(button.key, (pad.buttons & button.bit) != 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, pad.left_trigger > 0.5f, pad.left_trigger);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, pad.right_trigger > 0.5f, pad.right_trigger);
    // The left stick moves like the D-pad, past a dead zone.
    constexpr float dead = 0.35f;
    const auto stick = [&](ImGuiKey key, float value) {
        const float past = std::clamp((value - dead) / (1.0f - dead), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, value > dead, past);
    };
    stick(ImGuiKey_GamepadLStickLeft, -pad.left_x);
    stick(ImGuiKey_GamepadLStickRight, pad.left_x);
    stick(ImGuiKey_GamepadLStickUp, pad.left_y);
    stick(ImGuiKey_GamepadLStickDown, -pad.left_y);
    // The right stick scrolls the full screen menu's page.
    stick(ImGuiKey_GamepadRStickUp, pad.right_y);
    stick(ImGuiKey_GamepadRStickDown, -pad.right_y);
    fed = pad.valid;
    if (fed && !(menu_open && dingosdk::overlay::menu_page_owns_pad().load())) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    } else if (fed) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    }
}

// Pixels made into a texture in the SRV heap's slot `slot`: the map download card's icon (1),
// the menu's map pictures (2) or the item card's mod icon (3). Recorded on the frame's command list ahead of ImGui's draws, so
// the copy is done before anything samples it. Returns the texture's id for ImGui, or 0. Nothing
// here is fatal: without it a card shows a plain tile.
ImTextureID upload_image(State& s, const CardPixels& pixels, UINT slot, ComPtr<ID3D12Resource>& texture_out,
                         ComPtr<ID3D12Resource>& upload_out) {
    texture_out.Reset();
    upload_out.Reset();
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = pixels.width;
    texture.Height = pixels.height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT rows{};
    UINT64 row_bytes{}, total{};
    s.device->GetCopyableFootprints(&texture, 0, 1, 0, &layout, &rows, &row_bytes, &total);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> made, upload;
    if (!total || rows != pixels.height || row_bytes < std::uint64_t{pixels.width} * 4 ||
        FAILED(s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&made))) ||
        FAILED(s.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upload))))
        return {};
    void* mapped{};
    const D3D12_RANGE none{};
    if (FAILED(upload->Map(0, &none, &mapped)) || !mapped) return {};
    for (UINT row = 0; row < pixels.height; ++row)
        std::memcpy(static_cast<unsigned char*>(mapped) + layout.Offset + std::size_t{row} * layout.Footprint.RowPitch,
                    pixels.rgba.data() + std::size_t{row} * pixels.width * 4, std::size_t{pixels.width} * 4);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = made.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = layout;
    s.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER ready{};
    ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ready.Transition.pResource = made.Get();
    ready.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ready.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    ready.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    s.commands->ResourceBarrier(1, &ready);
    const auto step = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = s.srvs->GetCPUDescriptorHandleForHeapStart();
    auto gpu = s.srvs->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += std::size_t{step} * slot;
    gpu.ptr += std::uint64_t{step} * slot;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = texture.Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(made.Get(), &view, cpu);
    texture_out = std::move(made);
    upload_out = std::move(upload);
    // Drawable from the next frame on: this frame's draws were made before the copy was recorded.
    return static_cast<ImTextureID>(gpu.ptr);
}

void upload_card_image(State& s) {
    CardPixels pixels;
    if (!take_card_pixels(pixels) || pixels.rgba.empty() || !s.device || !s.srvs) return;
    // Frames still on the GPU may sample the icon this one replaces.
    if (s.card_texture && s.next_fence && !completed(s.next_fence, render_fence_timeout_ms)) return;
    if (const auto id = upload_image(s, pixels, 1, s.card_texture, s.card_upload)) card_image_uploaded(id, pixels.serial);
}

// The menu's map pictures (menu_map_pictures.cpp), when a new set is waiting.
void upload_menu_map_image(State& s) {
    if (!s.device || !s.srvs) return;
    if (s.menu_map_texture && s.next_fence && !completed(s.next_fence, render_fence_timeout_ms)) return;
    CardPixels pixels;
    if (!take_menu_map_pixels(pixels) || pixels.rgba.empty()) return;
    if (const auto id = upload_image(s, pixels, 2, s.menu_map_texture, s.menu_map_upload)) menu_map_pixels_uploaded(id, pixels.serial);
}

// The item card's mod icon (item_browser_overlay.cpp), when another is waiting.
void upload_mod_icon(State& s) {
    if (!s.device || !s.srvs) return;
    if (s.mod_icon_texture && s.next_fence && !completed(s.next_fence, render_fence_timeout_ms)) return;
    CardPixels pixels;
    if (!take_mod_icon_pixels(pixels) || pixels.rgba.empty()) return;
    if (const auto id = upload_image(s, pixels, 3, s.mod_icon_texture, s.mod_icon_upload)) mod_icon_uploaded(id, pixels.serial);
}

// A plugin's picture (plugin_images.cpp), one a frame, into the slot the cache chose.
void upload_plugin_image(State& s) {
    if (!s.device || !s.srvs) return;
    auto next = take_plugin_image();
    if (!next) return;
    const auto slot = static_cast<std::size_t>(next->slot);
    // A texture in the slot may still be sampled by frames on the GPU.
    if (s.plugin_textures[slot] && s.next_fence && !completed(s.next_fence, render_fence_timeout_ms)) {
        plugin_image_uploaded(next->path, next->slot, {}, true);
        return;
    }
    const auto id = upload_image(s, next->pixels, plugin_image_first_slot + static_cast<UINT>(slot), s.plugin_textures[slot],
                                 s.plugin_uploads[slot]);
    plugin_image_uploaded(next->path, next->slot, id);
}

// A Thunderstore icon for the Mod Manager page (store_icons.cpp), one a frame, into the slot it was given.
void upload_store_icon(State& s) {
    if (!s.device || !s.srvs) return;
    auto next = take_store_icon();
    if (!next) return;
    const auto slot = static_cast<std::size_t>(next->slot);
    if (s.store_textures[slot] && s.next_fence && !completed(s.next_fence, render_fence_timeout_ms)) {
        store_icon_uploaded(next->url, next->slot, {}, true);
        return;
    }
    const auto id = upload_image(s, next->pixels, store_icon_first_slot + static_cast<UINT>(slot), s.store_textures[slot], s.store_uploads[slot]);
    store_icon_uploaded(next->url, next->slot, id);
}

void render(IDXGISwapChain* presented, UINT flags) {
    auto& s = state();
    if (flags & DXGI_PRESENT_TEST) return;
    dingosdk::profiler::record_present();
    DINGO_PROFILE_ZONE("present/overlay");
    std::lock_guard lock(s.render_mutex);
    // Said once: a log without this line is a game whose presentations never reach the overlay.
    if (static bool said{}; !std::exchange(said, true))
        dingosdk::logging::write(dingosdk::logging::Level::info, dingosdk::logging::Channel::graphics,
            "The game's first presentation reached the overlay.");
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
    const bool map_download_frame = map_download_pending();
    const bool mod_apply_frame = mod_apply_card_pending();
    const bool game_text_frame = game_text_pending();
    const bool skate_hud_frame = skate_hud_pending();
    const bool nametag_frame = nametags_pending();
    const bool plugins_frame = plugins_pending();
    const bool item_browser_frame = item_browser_pending();
    const bool perf_frame = perf_hud_pending() || trainer_hud_pending();
    const bool hub_frame = hub_page_pending();
    const bool grab_frame = frame_grab_pending() || depth_probe_pending();
    // A plugin's screen look is drawn under every frame it asks for (reskate.screen.look).
    const bool look_frame = screen_look::pending();
    // And a plugin's viewmodel (reskate.viewmodel), or its pictures still going up to the GPU.
    const bool viewmodel_frame = viewmodel::pending();
    if (trainer_open_requested()) s.visible.store(true);
    if (dingosdk::overlay::menu_open_flag().exchange(false)) s.visible.store(true); // `menu`, or its controller bind
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
            !nametag_frame && !plugins_frame && !item_browser_frame && !perf_frame && !hub_frame && !map_download_frame && !mod_apply_frame && !look_frame &&
            !viewmodel_frame && !grab_frame) return;
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
    // A screen capture takes the game's frame before anything of ReSkate's is drawn on it.
    {
        const auto size = frame.buffer->GetDesc();
        const bool grabbing = frame_grab_pending();
        depth_tap_present(s, static_cast<std::uint32_t>(size.Width), size.Height, grabbing);
        if (grab_frame) {
            if (grabbing) grab_presented_frame(s, frame.buffer.Get());
            depth_probe_present(s, static_cast<std::uint32_t>(size.Width), size.Height);
        }
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
    // (Watching the pointer for the pause menu's browser keeps the mouse state between frames,
    // or every frame would look like a fresh click.)
    if (!interactive) { ImGui::GetIO().ClearInputKeys(); if (!hub_frame) ImGui::GetIO().ClearInputMouse(); }
    feed_menu_gamepad(interactive && (s.visible.load() || s.plugin_dialog_visible.load()) && !s.editor_visible.load());
    ImGui::GetIO().MouseDrawCursor = owns_menu_cursor(s);
    ImGui_ImplDX12_NewFrame();
    { OverlayInputAccess access; ImGui_ImplWin32_NewFrame(); }
    update_menu_pointer();
    fit_display_to_buffer(s, frame.buffer.Get());
    ImGui::NewFrame();
    if (menu_frame) {
        draw_menu();
        draw_plugin_screen();
        draw_console();
        draw_perf_window();
    }
    draw_hub_page();
    draw_plugins();
    draw_nametags();
    draw_game_text();
    draw_skate_hud();
    draw_perf_hud();
    draw_trainer_hud();
    draw_notices();
    draw_item_browser();
    draw_plugin_dialog();
    draw_chat();
    if (mod_apply_frame) draw_mod_apply_card();
    draw_map_download();
    sync_menu_cursor(); // close buttons also change visibility, without a key message
    ImGui::Render();
    if (ImGui::GetDrawData()->TotalVtxCount == 0 && !look_frame && !viewmodel_frame) { ImGui::SetCurrentContext(previous); return; }
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
    upload_card_image(s);
    upload_menu_map_image(s);
    upload_mod_icon(s);
    upload_plugin_image(s);
    upload_store_icon(s);
    if (viewmodel_frame) {
        // Under the screen look (a camcorder films the gun too) and ImGui. Left in PRESENT, as found.
        ID3D12DescriptorHeap* viewmodel_heaps[] = {s.srvs.Get()};
        s.commands->SetDescriptorHeaps(1, viewmodel_heaps);
        viewmodel::Target target;
        target.device = s.device.Get();
        target.commands = s.commands.Get();
        target.buffer = frame.buffer.Get();
        target.rtv = frame.rtv;
        target.srvs = s.srvs.Get();
        target.slot = viewmodel_first_slot;
        target.slots = viewmodel_slots;
        target.format = frame.buffer->GetDesc().Format;
        target.transfer = output_transfer;
        target.white_nits = output_white_nits;
        target.fence = s.next_fence + 1; // what this frame's commands signal (below)
        target.gpu_done = [] {
            const auto& fence = state().fence;
            return fence ? fence->GetCompletedValue() : std::uint64_t{0};
        };
        (void)viewmodel::record(target);
    }
    if (look_frame) {
        // Drawn first, so ImGui draws over it. It leaves the back buffer in PRESENT, as it found it.
        ID3D12DescriptorHeap* look_heaps[] = {s.srvs.Get()};
        s.commands->SetDescriptorHeaps(1, look_heaps);
        screen_look::Target look;
        look.device = s.device.Get();
        look.commands = s.commands.Get();
        look.buffer = frame.buffer.Get();
        look.rtv = frame.rtv;
        look.srvs = s.srvs.Get();
        look.slot = screen_look_slot;
        look.format = frame.buffer->GetDesc().Format;
        look.transfer = output_transfer;
        look.wait_idle = [] { return completed(state().next_fence, render_fence_timeout_ms); };
        (void)screen_look::record(look);
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.buffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
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
