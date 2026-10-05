// The OpenXR session and frame loop, driven from the game's Present.
//
// Per presented frame:
//   on_present     pair the back buffer with the camera frame that rendered it,
//                  copy it into that eye's swapchain, xrEndFrame
//   after_present  poll events, xrWaitFrame (paces the game to the headset),
//                  xrBeginFrame
#include "vr_blit.h"
#include "vr_stereo.h"
#include "vr_internal.h"
#include "Engine/Core/Log/logging.h"
#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <vector>

namespace dingosdk::vr {
namespace {
using namespace detail;
using Microsoft::WRL::ComPtr;
namespace logging = dingosdk::logging;

void log_info(const std::string& text) { logging::write(logging::Level::info, logging::Channel::graphics, "VR: " + text); }
void log_warning(const std::string& text) { logging::write(logging::Level::warning, logging::Channel::graphics, "VR: " + text); }

struct Chain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*> images; // owned by the runtime
    std::uint32_t width = 0, height = 0;
};

struct Runtime {
    // Side-by-side input submits after the game's Present returns (see on_present).
    ComPtr<IDXGISwapChain3> deferred_chain;
    bool presented_pass = false;
    UINT presented_buffer = 0;
    std::array<std::int64_t, 6> logged_sizes{};
    HMODULE loader = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, exit_requested = false, frame_begun = false;
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::array<Chain, 2> eyes;
    Chain screen;
    std::array<bool, 2> eye_ready{};
    std::array<EyeRecord, 2> eye_records{};
    ComPtr<ID3D12CommandQueue> queue;
    Blitter blitter;
    DepthStereo stereo;
    // Depth stereo renders the previous frame (its colour is kept, its depth copied at
    // the next clear): the camera record of the kept frame, and the latest record.
    std::optional<EyeRecord> kept_record, last_record;
    bool depth_pending = false; // A frame was kept; its depth arrives with the next clear.
    bool no_depth_logged = false;
    double retry_at = 0;
    std::string last_error;
    // Camera frames collected per present, newest last; the image presented
    // now belongs to the entry `frame_lag` presents back.
    std::uint64_t collected = 0;
    std::deque<std::optional<EyeRecord>> history;
    math::Anchor screen_anchor;
    std::uint64_t screen_recenter = 0;
    bool key_down = false;
    double stats_time = 0;
    std::uint64_t stats_presents = 0, stats_camera = 0, presents = 0;
    // New camera frames found per present while the VR camera is active:
    // [0] none, [1] one, [2] more. Steady pairing is all ones.
    std::array<std::uint64_t, 3> pairing{};
    double log_time = 0;
    int image_format = 0;
};
Runtime& runtime() {
    static auto* value = new Runtime;
    return *value;
}

std::mutex& status_mutex() {
    static auto* value = new std::mutex;
    return *value;
}
Status& published() {
    static auto* value = new Status;
    return *value;
}
template<class Change>
void update_status(Change&& change) {
    std::lock_guard lock(status_mutex());
    change(published());
}

const char* state_name(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_IDLE: return "idle";
    case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
    case XR_SESSION_STATE_VISIBLE: return "visible";
    case XR_SESSION_STATE_FOCUSED: return "focused";
    case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
    case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "starting";
    }
}

std::string result_text(XrResult result) {
    auto& s = shared();
    auto& r = runtime();
    if (r.instance && s.xr.result_to_string) {
        char text[XR_MAX_RESULT_STRING_SIZE]{};
        if (XR_SUCCEEDED(s.xr.result_to_string(r.instance, result, text))) return text;
    }
    return std::format("XrResult {}", static_cast<int>(result));
}

bool check(XrResult result, const char* what, std::string& error) {
    if (XR_SUCCEEDED(result)) return true;
    error = std::format("{} failed: {}.", what, result_text(result));
    return false;
}

template<std::size_t Size>
void copy_name(char (&out)[Size], const char* text) {
    const auto length = std::min(std::strlen(text), Size - 1);
    std::memcpy(out, text, length);
    out[length] = 0;
}

template<class Function>
bool load(XrInstance instance, const char* name, Function& out) {
    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(shared().xr.get_instance_proc_addr(instance, name, &function)) || !function) return false;
    out = reinterpret_cast<Function>(function);
    return true;
}

bool load_loader(std::string& error) {
    auto& r = runtime();
    if (r.loader) return true;
    // Beside ReSkate.dll (normally beside Skate.exe), then the usual search.
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&runtime), &self) && GetModuleFileNameW(self, path, MAX_PATH)) {
        std::wstring file(path);
        file = file.substr(0, file.find_last_of(L"\\/") + 1) + L"openxr_loader.dll";
        r.loader = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!r.loader) r.loader = LoadLibraryW(L"openxr_loader.dll");
    if (!r.loader) {
        error = "openxr_loader.dll was not found. Put it beside Skate.exe.";
        return false;
    }
    auto& xr = shared().xr;
    xr.get_instance_proc_addr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(r.loader, "xrGetInstanceProcAddr")));
    if (!xr.get_instance_proc_addr || !load(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", xr.enumerate_instance_extension_properties) ||
        !load(XR_NULL_HANDLE, "xrCreateInstance", xr.create_instance)) {
        error = "openxr_loader.dll is not a usable OpenXR loader.";
        FreeLibrary(r.loader);
        r.loader = nullptr;
        return false;
    }
    update_status([](Status& st) { st.loader = true; });
    return true;
}

bool load_instance_functions(XrInstance instance) {
    auto& x = shared().xr;
    return load(instance, "xrDestroyInstance", x.destroy_instance) &&
        load(instance, "xrGetInstanceProperties", x.get_instance_properties) &&
        load(instance, "xrGetSystem", x.get_system) &&
        load(instance, "xrGetSystemProperties", x.get_system_properties) &&
        load(instance, "xrEnumerateViewConfigurationViews", x.enumerate_view_configuration_views) &&
        load(instance, "xrEnumerateEnvironmentBlendModes", x.enumerate_environment_blend_modes) &&
        load(instance, "xrGetD3D12GraphicsRequirementsKHR", x.get_d3d12_graphics_requirements) &&
        load(instance, "xrCreateSession", x.create_session) &&
        load(instance, "xrDestroySession", x.destroy_session) &&
        load(instance, "xrBeginSession", x.begin_session) &&
        load(instance, "xrEndSession", x.end_session) &&
        load(instance, "xrRequestExitSession", x.request_exit_session) &&
        load(instance, "xrPollEvent", x.poll_event) &&
        load(instance, "xrCreateReferenceSpace", x.create_reference_space) &&
        load(instance, "xrDestroySpace", x.destroy_space) &&
        load(instance, "xrLocateSpace", x.locate_space) &&
        load(instance, "xrLocateViews", x.locate_views) &&
        load(instance, "xrEnumerateSwapchainFormats", x.enumerate_swapchain_formats) &&
        load(instance, "xrCreateSwapchain", x.create_swapchain) &&
        load(instance, "xrDestroySwapchain", x.destroy_swapchain) &&
        load(instance, "xrEnumerateSwapchainImages", x.enumerate_swapchain_images) &&
        load(instance, "xrAcquireSwapchainImage", x.acquire_swapchain_image) &&
        load(instance, "xrWaitSwapchainImage", x.wait_swapchain_image) &&
        load(instance, "xrReleaseSwapchainImage", x.release_swapchain_image) &&
        load(instance, "xrWaitFrame", x.wait_frame) &&
        load(instance, "xrBeginFrame", x.begin_frame) &&
        load(instance, "xrEndFrame", x.end_frame) &&
        load(instance, "xrResultToString", x.result_to_string);
}

void destroy_chain(Chain& chain) {
    if (chain.handle) shared().xr.destroy_swapchain(chain.handle);
    chain = {};
}

void end_empty_frame() {
    auto& r = runtime();
    if (!r.frame_begun || !r.session) return;
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.displayTime = r.frame.predictedDisplayTime;
    info.environmentBlendMode = r.blend;
    (void)shared().xr.end_frame(r.session, &info);
    r.frame_begun = false;
}

// Releases everything; the next enabled present starts again after `retry` seconds.
void teardown(const std::string& reason, double retry) {
    auto& s = shared();
    auto& r = runtime();
    s.tracking.store(false, std::memory_order_release);
    s.predicted_display_time.store(0, std::memory_order_release);
    end_empty_frame();
    {
        std::unique_lock lock(s.xr_mutex); // waits for any camera still locating poses
        s.session = XR_NULL_HANDLE;
        s.local_space = s.view_space = XR_NULL_HANDLE;
    }
    r.blitter.destroy();
    r.stereo.destroy();
    r.kept_record.reset();
    r.last_record.reset();
    r.depth_pending = false;
    for (auto& eye : r.eyes) destroy_chain(eye);
    destroy_chain(r.screen);
    if (r.view) s.xr.destroy_space(r.view);
    if (r.local) s.xr.destroy_space(r.local);
    input_stop();
    if (r.session) s.xr.destroy_session(r.session);
    if (r.instance) s.xr.destroy_instance(r.instance);
    r.view = r.local = XR_NULL_HANDLE;
    r.session = XR_NULL_HANDLE;
    r.instance = XR_NULL_HANDLE;
    r.system = XR_NULL_SYSTEM_ID;
    r.state = XR_SESSION_STATE_UNKNOWN;
    r.running = r.exit_requested = r.frame_begun = false;
    r.eye_ready = {};
    r.history.clear();
    r.queue.Reset();
    r.format = DXGI_FORMAT_UNKNOWN;
    r.retry_at = seconds_now() + retry;
    {
        std::lock_guard lock(s.records_mutex);
        s.records.clear();
    }
    if (!reason.empty()) log_info(reason);
    update_status([&](Status& st) {
        const bool loader = st.loader;
        const auto present_thread = st.present_thread;
        st = {};
        st.loader = loader;
        st.present_thread = present_thread;
        st.message = reason;
    });
}

bool start(ID3D12CommandQueue* queue, std::string& error) {
    auto& s = shared();
    auto& r = runtime();
    auto& x = s.xr;
    if (!load_loader(error)) return false;

    std::uint32_t count = 0;
    if (!check(x.enumerate_instance_extension_properties(nullptr, 0, &count, nullptr), "Listing OpenXR extensions", error)) return false;
    std::vector<XrExtensionProperties> extensions(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (!check(x.enumerate_instance_extension_properties(nullptr, count, &count, extensions.data()), "Listing OpenXR extensions", error))
        return false;
    if (std::none_of(extensions.begin(), extensions.end(),
            [](const XrExtensionProperties& e) { return std::strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0; })) {
        error = "The active OpenXR runtime does not support Direct3D 12 (XR_KHR_D3D12_enable).";
        return false;
    }
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    copy_name(create.applicationInfo.applicationName, "ReSkate");
    create.applicationInfo.applicationVersion = 1;
    copy_name(create.applicationInfo.engineName, "ReSkate VR");
    create.applicationInfo.engineVersion = 1;
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    const char* names[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    create.enabledExtensionCount = 1;
    create.enabledExtensionNames = names;
    if (!check(x.create_instance(&create, &r.instance), "Creating the OpenXR instance", error)) {
        r.instance = XR_NULL_HANDLE;
        error += " Is the headset's PC VR app (Virtual Desktop, Meta Horizon Link or SteamVR) running?";
        return false;
    }
    if (!load_instance_functions(r.instance)) { error = "The OpenXR runtime is missing a core function."; return false; }
    XrInstanceProperties instance{XR_TYPE_INSTANCE_PROPERTIES};
    (void)x.get_instance_properties(r.instance, &instance);
    const std::string runtime_name = std::format("{} {}.{}.{}", instance.runtimeName, XR_VERSION_MAJOR(instance.runtimeVersion),
        XR_VERSION_MINOR(instance.runtimeVersion), XR_VERSION_PATCH(instance.runtimeVersion));

    XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO};
    system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult found = x.get_system(r.instance, &system, &r.system);
    if (found == XR_ERROR_FORM_FACTOR_UNAVAILABLE) { error = "No headset is connected to " + runtime_name + "."; return false; }
    if (!check(found, "Finding the headset", error)) return false;
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    (void)x.get_system_properties(r.instance, r.system, &properties);
    std::array<XrViewConfigurationView, 2> views{XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (!check(x.enumerate_view_configuration_views(r.instance, r.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count,
            views.data()), "Reading the headset's views", error)) return false;
    std::array<XrEnvironmentBlendMode, 8> blends{};
    if (XR_SUCCEEDED(x.enumerate_environment_blend_modes(r.instance, r.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            static_cast<std::uint32_t>(blends.size()), &count, blends.data())) && count) {
        const auto end = blends.begin() + std::min<std::size_t>(count, blends.size());
        r.blend = std::find(blends.begin(), end, XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != end ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : blends[0];
    }

    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (!check(x.get_d3d12_graphics_requirements(r.instance, r.system, &requirements), "Reading the headset's GPU", error))
        return false;
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) { error = "The game's Direct3D 12 device is unavailable."; return false; }
    const LUID adapter = device->GetAdapterLuid();
    if (adapter.LowPart != requirements.adapterLuid.LowPart || adapter.HighPart != requirements.adapterLuid.HighPart) {
        error = "The game runs on a different GPU than the headset. Set skate. to the headset's GPU in Windows graphics settings.";
        return false;
    }
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device.Get();
    binding.queue = queue;
    XrSessionCreateInfo session{XR_TYPE_SESSION_CREATE_INFO};
    session.next = &binding;
    session.systemId = r.system;
    if (!check(x.create_session(r.instance, &session, &r.session), "Creating the OpenXR session", error)) {
        r.session = XR_NULL_HANDLE;
        return false;
    }
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.poseInReferenceSpace.orientation.w = 1;
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!check(x.create_reference_space(r.session, &space, &r.local), "Creating the tracking space", error)) return false;
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!check(x.create_reference_space(r.session, &space, &r.view), "Creating the head space", error)) return false;

    std::vector<std::int64_t> formats;
    if (!check(x.enumerate_swapchain_formats(r.session, 0, &count, nullptr), "Listing swapchain formats", error)) return false;
    formats.resize(count);
    if (!check(x.enumerate_swapchain_formats(r.session, count, &count, formats.data()), "Listing swapchain formats", error)) return false;
    for (const auto preferred : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT,
             DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM}) {
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(preferred)) != formats.end()) {
            r.format = preferred;
            break;
        }
    }
    if (r.format == DXGI_FORMAT_UNKNOWN) { error = "The OpenXR runtime offers no usable swapchain format."; return false; }
    if (!r.blitter.create(device.Get(), error)) return false;
    if (!r.stereo.create(device.Get(), error)) return false;
    r.queue = queue;
    {
        std::unique_lock lock(s.xr_mutex);
        s.session = r.session;
        s.local_space = r.local;
        s.view_space = r.view;
    }
    (void)input_start(r.instance, r.session, x.get_instance_proc_addr); // optional: logged when unavailable
    const std::string system_name = properties.systemName;
    log_info(std::format("{} on {}; {}x{} per eye recommended, swapchain format {}.", system_name, runtime_name,
        views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight, static_cast<int>(r.format)));
    update_status([&](Status& st) {
        st.instance = st.session = true;
        st.runtime = runtime_name;
        st.system = system_name;
        st.recommended_width = views[0].recommendedImageRectWidth;
        st.recommended_height = views[0].recommendedImageRectHeight;
        st.state = "starting";
        st.message = "Waiting for the headset.";
    });
    return true;
}

void handle_events() {
    auto& s = shared();
    auto& r = runtime();
    std::string stop_reason;
    double retry = 5;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (r.instance && s.xr.poll_event(r.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            r.state = changed.state;
            update_status([&](Status& st) {
                st.state = state_name(changed.state);
                st.focused = changed.state == XR_SESSION_STATE_FOCUSED;
            });
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                std::string error;
                if (check(s.xr.begin_session(r.session, &begin), "Starting the VR session", error)) {
                    r.running = true;
                    recenter();
                    log_info("Session running.");
                    update_status([](Status& st) { st.running = true; st.message.clear(); });
                } else {
                    stop_reason = error;
                    break;
                }
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                s.tracking.store(false, std::memory_order_release);
                (void)s.xr.end_session(r.session);
                r.running = false;
                update_status([](Status& st) { st.running = false; });
            } else if (changed.state == XR_SESSION_STATE_EXITING) {
                stop_reason = r.exit_requested ? "VR off." : "The headset ended the VR session.";
                retry = r.exit_requested ? 0 : 10;
                break;
            } else if (changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                stop_reason = "The headset connection was lost.";
                break;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            stop_reason = "The OpenXR runtime is shutting down.";
            break;
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            recenter();
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (!stop_reason.empty()) teardown(stop_reason, retry);
}

bool ensure_chain(Chain& chain, std::uint32_t width, std::uint32_t height, std::string& error) {
    auto& r = runtime();
    auto& x = shared().xr;
    if (chain.handle && chain.width == width && chain.height == height) return true;
    if (chain.handle) {
        if (!r.blitter.wait_idle(2000)) { error = "The GPU did not finish copying to the headset."; return false; }
        destroy_chain(chain);
    }
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    create.format = static_cast<std::int64_t>(r.format);
    create.sampleCount = 1;
    create.width = width;
    create.height = height;
    create.faceCount = 1;
    create.arraySize = 1;
    create.mipCount = 1;
    if (!check(x.create_swapchain(r.session, &create, &chain.handle), "Creating a headset swapchain", error)) {
        chain.handle = XR_NULL_HANDLE;
        return false;
    }
    std::uint32_t count = 0;
    if (!check(x.enumerate_swapchain_images(chain.handle, 0, &count, nullptr), "Listing swapchain images", error)) return false;
    std::vector<XrSwapchainImageD3D12KHR> images(count, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (!check(x.enumerate_swapchain_images(chain.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "Listing swapchain images", error)) return false;
    chain.images.clear();
    for (const auto& image : images) chain.images.push_back(image.texture);
    chain.width = width;
    chain.height = height;
    return true;
}

// The comfort vignette for the eye images now (the camera sets it per frame).
float eye_vignette() { return shared().vignette.load(std::memory_order_acquire); }

bool copy_into(Chain& chain, ID3D12Resource* image, std::string& error, float u_offset = 0, float u_scale = 1,
    float vignette = 0) {
    auto& r = runtime();
    auto& x = shared().xr;
    std::uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!check(x.acquire_swapchain_image(chain.handle, &acquire, &index), "Acquiring a swapchain image", error)) return false;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100'000'000; // 100 ms
    XrResult waited = XR_TIMEOUT_EXPIRED;
    for (int attempt = 0; attempt < 5 && waited == XR_TIMEOUT_EXPIRED; ++attempt) waited = x.wait_swapchain_image(chain.handle, &wait);
    if (waited != XR_SUCCESS) {
        error = "Waiting for a swapchain image failed: " + result_text(waited) + ".";
        return false; // An image that was never waited on cannot be released.
    }
    const bool copied = index < chain.images.size() && r.blitter.copy(r.queue.Get(), image, chain.images[index], r.format, error, u_offset, u_scale, vignette);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult released = x.release_swapchain_image(chain.handle, &release);
    if (copied && !check(released, "Releasing a swapchain image", error)) return false;
    return copied;
}

// Acquires both eyes' images, renders the depth stereo pass into them, releases them.
bool render_stereo_into(std::array<Chain*, 2> eyes, const StereoParams& params, std::string& error) {
    auto& r = runtime();
    auto& x = shared().xr;
    std::array<std::uint32_t, 2> index{};
    std::array<bool, 2> acquired{};
    bool ok = true;
    for (std::size_t e = 0; e < 2 && ok; ++e) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!check(x.acquire_swapchain_image(eyes[e]->handle, &acquire, &index[e]), "Acquiring a swapchain image", error)) {
            ok = false;
            break;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000; // 100 ms
        XrResult waited = XR_TIMEOUT_EXPIRED;
        for (int attempt = 0; attempt < 5 && waited == XR_TIMEOUT_EXPIRED; ++attempt) waited = x.wait_swapchain_image(eyes[e]->handle, &wait);
        if (waited != XR_SUCCESS) {
            error = "Waiting for a swapchain image failed: " + result_text(waited) + ".";
            ok = false; // Never waited on: not released.
            break;
        }
        acquired[e] = index[e] < eyes[e]->images.size();
        ok = acquired[e];
    }
    if (ok) ok = r.stereo.render(r.queue.Get(), {eyes[0]->images[index[0]], eyes[1]->images[index[1]]}, r.format, params, false, error);
    for (std::size_t e = 0; e < 2; ++e)
        if (acquired[e]) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            (void)x.release_swapchain_image(eyes[e]->handle, &release);
        }
    return ok;
}

void poll_recenter_key(int key) {
    auto& r = runtime();
    if (!key) return;
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (down && !r.key_down) {
        DWORD process = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &process);
        if (process == GetCurrentProcessId()) {
            recenter();
            log_info("Recentred.");
        }
    }
    r.key_down = down;
}

void publish_rates(double now) {
    auto& s = shared();
    auto& r = runtime();
    if (r.stats_time == 0) r.stats_time = now;
    const double elapsed = now - r.stats_time;
    if (elapsed < 1.0) return;
    const auto camera = s.camera_frames.load(std::memory_order_relaxed);
    const float present_hz = static_cast<float>(static_cast<double>(r.presents - r.stats_presents) / elapsed);
    const float camera_hz = static_cast<float>(static_cast<double>(camera - r.stats_camera) / elapsed);
    r.stats_presents = r.presents;
    r.stats_camera = camera;
    r.stats_time = now;
    const auto period = s.display_period.load(std::memory_order_relaxed);
    const float fov = s.camera_fov_degrees.load(std::memory_order_relaxed);
    const auto camera_thread = s.camera_thread.load(std::memory_order_relaxed);
    // One line every 5 s while the session runs, so a log alone shows how VR behaved.
    if (r.running && now - r.log_time >= 5.0) {
        r.log_time = now;
        Status snapshot;
        {
            std::lock_guard lock(status_mutex());
            snapshot = published();
        }
        log_info(std::format("stats: state {}, headset {:.0f} Hz, presents {:.1f}/s, camera frames {:.1f}/s, "
            "pairing none/one/more {}/{}/{}, eye images {}, repeats {}, frames {}, {}, image {}x{} format {}, render FOV {:.1f}, "
            "frame lag {}, camera thread {}, present thread {}",
            snapshot.state, period > 0 ? 1e9 / static_cast<double>(period) : 0.0, present_hz, camera_hz,
            r.pairing[0], r.pairing[1], r.pairing[2], snapshot.eye_images, snapshot.repeated_images, snapshot.frames_submitted,
            camera_active(now) ? "VR camera" : snapshot.theater_active ? "flat screen" : "idle",
            snapshot.image_width, snapshot.image_height, r.image_format, fov, current_settings().frame_lag, camera_thread,
            GetCurrentThreadId()));
        r.pairing = {};
    }
    update_status([&](Status& st) {
        st.present_hz = present_hz;
        st.camera_hz = camera_hz;
        st.display_hz = period > 0 ? static_cast<float>(1e9 / static_cast<double>(period)) : 0.0f;
        st.render_fov_degrees = fov;
        st.camera_thread = camera_thread;
        st.present_thread = GetCurrentThreadId();
    });
}

// The game image as a flat screen in front of the recentred view.
bool screen_layer(const Settings& options, ID3D12Resource* image, std::uint32_t width, std::uint32_t height,
    XrCompositionLayerQuad& quad, std::string& error) {
    auto& s = shared();
    auto& r = runtime();
    const auto generation = s.recenter_generation.load(std::memory_order_acquire);
    if (generation != r.screen_recenter) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(s.xr.locate_space(r.view, r.local, r.frame.predictedDisplayTime, &location)) &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            auto head = to_pose(location.pose);
            if (!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) head.position = {};
            r.screen_anchor = math::anchor_from(head);
            r.screen_recenter = generation;
        }
    }
    if (!ensure_chain(r.screen, width, height, error) || !copy_into(r.screen, image, error)) return false;
    const auto facing = math::yaw(r.screen_anchor.heading);
    const auto ahead = first_person::rotate(facing, {0, 0, -options.theater_distance});
    math::Pose pose{facing, first_person::add(r.screen_anchor.position, ahead)};
    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = r.local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = r.screen.handle;
    quad.subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)}};
    quad.pose = to_xr(pose);
    quad.size = {options.theater_width, options.theater_width * static_cast<float>(height) / static_cast<float>(width)};
    return true;
}
}

Status status() {
    const bool enabled = current_settings().enabled;
    std::lock_guard lock(status_mutex());
    auto result = published();
    result.camera_active = enabled && result.running && camera_active(seconds_now());
    if (!enabled && !result.instance) result.state = "off";
    return result;
}

void on_present(IDXGISwapChain3* chain, ID3D12CommandQueue* queue) noexcept {
    try {
        auto& s = shared();
        auto& r = runtime();
        const double now = seconds_now();
        if (!r.presented_pass) {
            ++r.presents;
            publish_rates(now);
            DXGI_SWAP_CHAIN_DESC1 frame{};
            if (SUCCEEDED(chain->GetDesc1(&frame))) depth_on_present(frame.Width, frame.Height);
        }
        const auto options = current_settings();
        if (!options.enabled) {
            if (r.instance && !r.running) teardown("VR off.", 0);
            else if (r.instance && !r.exit_requested) {
                r.exit_requested = true;
                s.tracking.store(false, std::memory_order_release);
                (void)s.xr.request_exit_session(r.session);
                update_status([](Status& st) { st.message = "Leaving VR."; });
            }
        } else if (!r.instance) {
            if (now < r.retry_at) return;
            std::string error;
            if (!start(queue, error)) {
                if (error != r.last_error) log_warning(error);
                r.last_error = error;
                teardown({}, 5);
                update_status([&](Status& st) { st.message = error; });
                return;
            }
            r.last_error.clear();
        } else if (r.queue.Get() != queue) {
            teardown("The game recreated its swapchain; restarting VR.", 0);
            return;
        }
        if (options.enabled) poll_recenter_key(options.recenter_key);
        if (!r.frame_begun) return;
        // Side-by-side input: a stereo shader (ReShade) draws into the frame inside the
        // game's Present, after this hook runs. Submit once Present returns instead, from
        // the buffer just presented (after_present).
        if (options.enabled && options.stereo_mode == 1 && !r.presented_pass) {
            r.deferred_chain = chain;
            return;
        }
        const UINT buffer = r.presented_pass ? r.presented_buffer : chain->GetCurrentBackBufferIndex();

        std::vector<XrCompositionLayerBaseHeader*> layers;
        std::array<XrCompositionLayerProjectionView, 2> views{};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        std::string error;
        ComPtr<ID3D12Resource> image;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        const bool have_image = SUCCEEDED(chain->GetDesc1(&desc)) &&
            SUCCEEDED(chain->GetBuffer(buffer, IID_PPV_ARGS(&image))) &&
            desc.Width > 0 && desc.Height > 0;
        if (have_image) r.image_format = static_cast<int>(desc.Format);
        if (have_image) {
            // Why the image is the size it is: compare the window, the monitor and the back buffer.
            HWND window = nullptr;
            RECT client{}, outer{}, monitor_area{};
            UINT dpi = 0;
            const bool iconic = SUCCEEDED(chain->GetHwnd(&window)) && window && IsIconic(window);
            if (window && !iconic) {
                GetClientRect(window, &client);
                GetWindowRect(window, &outer);
                dpi = GetDpiForWindow(window);
                MONITORINFO monitor{sizeof(MONITORINFO)};
                if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) monitor_area = monitor.rcMonitor;
            }
            const std::array<std::int64_t, 6> sizes{desc.Width, desc.Height, outer.right - outer.left, outer.bottom - outer.top,
                client.right - client.left, client.bottom - client.top};
            if (sizes != r.logged_sizes) {
                r.logged_sizes = sizes;
                log_info(std::format("back buffer {}x{}; window {}x{}, client {}x{}{}; monitor {}x{}; dpi {}.", desc.Width, desc.Height,
                    sizes[2], sizes[3], sizes[4], sizes[5], iconic ? " (minimized)" : "", monitor_area.right - monitor_area.left,
                    monitor_area.bottom - monitor_area.top, dpi));
            }
        }
        if (have_image) s.image_aspect.store(static_cast<float>(desc.Width) / static_cast<float>(desc.Height), std::memory_order_release);
        set_depth_copy(options.enabled && options.stereo_mode == 2);
        bool failed = false;
        if (options.enabled && !r.exit_requested && r.frame.shouldRender && have_image && Blitter::supported_source(desc.Format)) {
            // Pair this image with the camera frame that rendered it.
            std::optional<EyeRecord> newest;
            {
                std::lock_guard lock(s.records_mutex);
                std::size_t fresh = 0;
                for (const auto& record : s.records)
                    if (record.sequence > r.collected) { newest = record; ++fresh; }
                if (newest) r.collected = newest->sequence;
                if (camera_active(now)) ++r.pairing[std::min<std::size_t>(fresh, 2)];
            }
            r.history.push_back(newest);
            while (r.history.size() > static_cast<std::size_t>(options.frame_lag) + 1) r.history.pop_front();
            const auto match = r.history.size() == static_cast<std::size_t>(options.frame_lag) + 1 ? r.history.front() : std::nullopt;
            // Menus are drawn into the image, so they only read on the flat screen.
            const bool vr_camera = camera_active(now) && !s.ui_open.load(std::memory_order_acquire) &&
                !s.game_menu.load(std::memory_order_acquire);
            if (!vr_camera) r.eye_ready = {};
            if (match) r.last_record = match;
            if (vr_camera && options.stereo_mode == 2) {
                // Depth stereo: the kept previous frame and its depth (copied at this frame's clear)
                // make both eyes; then this frame is kept for the next Present.
                const auto depth = depth_texture();
                if (r.depth_pending && depth) {
                    // This frame's clear copied the kept frame's depth: snapshot it with its colour.
                    if (r.stereo.keep_depth(r.queue.Get(), depth.Get(), options.edge_widening, error)) r.depth_pending = false;
                    else failed = true;
                }
                if (!failed && r.stereo.has_kept() && r.stereo.has_kept_depth() && !r.depth_pending && r.kept_record &&
                    r.kept_record->both) {
                    const auto& record = *r.kept_record;
                    StereoParams params;
                    float separation = 0;
                    for (std::size_t i = 0; i < 3; ++i)
                        separation += (record.right.position[i] - record.left.position[i]) * (record.right.position[i] - record.left.position[i]);
                    separation = std::sqrt(separation) * options.world_scale;
                    // Reversed Z: depth = near / distance; the game's near plane is about 6 cm (measured).
                    constexpr float near_plane = 0.06f;
                    const float tangent = std::tan(record.fov.right);
                    params.scale = tangent > 0 ? options.depth_strength * separation / (4 * near_plane * tangent) : 0.0f;
                    // Search only as far as the nearest geometry can shift (reversed-Z depth up to ~0.15).
                    params.max_shift = std::min(0.04f, params.scale * 0.15f);
                    params.background_fill = options.gap_fill == 1;
                    params.vignette = eye_vignette();
                    if (ensure_chain(r.eyes[0], desc.Width, desc.Height, error) && ensure_chain(r.eyes[1], desc.Width, desc.Height, error) &&
                        render_stereo_into({&r.eyes[0], &r.eyes[1]}, params, error)) {
                        for (std::uint32_t v = 0; v < 2; ++v) {
                            r.eye_ready[v] = true;
                            r.eye_records[v] = record;
                            r.eye_records[v].view = v;
                            r.eye_records[v].pose = v ? record.right : record.left;
                        }
                        update_status([](Status& st) { st.eye_images += 2; });
                    } else failed = true;
                }
                // No scene depth (yet): both eyes get the plain image rather than nothing.
                if (!failed && !r.stereo.has_kept_depth() && match && match->both) {
                    if (!r.no_depth_logged) {
                        r.no_depth_logged = true;
                        log_warning("depth stereo has no scene depth yet; showing the image to both eyes without depth.");
                    }
                    for (std::uint32_t v = 0; v < 2 && !failed; ++v) {
                        if (ensure_chain(r.eyes[v], desc.Width, desc.Height, error) && copy_into(r.eyes[v], image.Get(), error, 0, 1, eye_vignette())) {
                            r.eye_ready[v] = true;
                            r.eye_records[v] = *match;
                            r.eye_records[v].view = v;
                            r.eye_records[v].pose = v ? match->right : match->left;
                        } else failed = true;
                    }
                }
                // Only frames with a new camera record: the in-between frames (about a third at
                // 90 Hz) need not match the last record, and pairing them with it shook the view.
                if (!failed && match) {
                    if (r.stereo.keep(r.queue.Get(), image.Get(), error)) {
                        r.kept_record = match;
                        r.depth_pending = true;
                    } else failed = true;
                }
            } else if (vr_camera && match && match->both) {
                // Side-by-side input: the left half is the left eye, the right half the right eye.
                const auto half = static_cast<std::uint32_t>(desc.Width / 2);
                for (std::uint32_t v = 0; v < 2 && !failed; ++v) {
                    auto& eye = r.eyes[v];
                    if (ensure_chain(eye, half, desc.Height, error) && copy_into(eye, image.Get(), error, v * 0.5f, 0.5f, eye_vignette())) {
                        r.eye_ready[v] = true;
                        r.eye_records[v] = *match;
                        r.eye_records[v].view = v;
                        r.eye_records[v].pose = v ? match->right : match->left;
                    } else failed = true;
                }
                if (!failed) update_status([](Status& st) { st.eye_images += 2; });
            } else if (vr_camera && match) {
                auto& eye = r.eyes[match->view];
                if (ensure_chain(eye, desc.Width, desc.Height, error) && copy_into(eye, image.Get(), error, 0, 1, eye_vignette())) {
                    if (r.eyes[match->view ^ 1u].width != desc.Width || r.eyes[match->view ^ 1u].height != desc.Height)
                        r.eye_ready[match->view ^ 1u] = false;
                    r.eye_ready[match->view] = true;
                    r.eye_records[match->view] = *match;
                    update_status([](Status& st) { ++st.eye_images; });
                } else failed = true;
            } else if (vr_camera) {
                update_status([](Status& st) { ++st.repeated_images; });
            }
            if (!failed && vr_camera && r.eye_ready[0] && r.eye_ready[1]) {
                for (std::uint32_t v = 0; v < 2; ++v) {
                    auto& view = views[v];
                    view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    view.pose = to_xr(r.eye_records[v].pose);
                    const auto& fov = r.eye_records[v].fov;
                    view.fov = {fov.left, fov.right, fov.up, fov.down};
                    view.subImage.swapchain = r.eyes[v].handle;
                    view.subImage.imageRect = {{0, 0},
                        {static_cast<std::int32_t>(r.eyes[v].width), static_cast<std::int32_t>(r.eyes[v].height)}};
                }
                projection.space = r.local;
                projection.viewCount = 2;
                projection.views = views.data();
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection));
            } else if (!failed && !vr_camera) {
                if (screen_layer(options, image.Get(), desc.Width, desc.Height, quad, error))
                    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
                else failed = true;
            }
            update_status([&](Status& st) {
                st.image_width = desc.Width;
                st.image_height = desc.Height;
                st.theater_active = !vr_camera;
            });
        }
        image.Reset();
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = r.frame.predictedDisplayTime;
        end.environmentBlendMode = r.blend;
        end.layerCount = failed ? 0 : static_cast<std::uint32_t>(layers.size());
        end.layers = layers.data();
        const XrResult ended = s.xr.end_frame(r.session, &end);
        r.frame_begun = false;
        if (failed) {
            teardown("Copying to the headset failed: " + error, 5);
            return;
        }
        if (XR_FAILED(ended)) {
            teardown("Submitting the frame failed: " + result_text(ended) + ".", 5);
            return;
        }
        update_status([](Status& st) { ++st.frames_submitted; });
    } catch (...) {
        teardown("Unexpected error in the VR frame.", 5);
    }
}

void after_present() noexcept {
    try {
        auto& s = shared();
        auto& r = runtime();
        if (!r.instance) return;
        if (r.deferred_chain) {
            // The frame just presented now holds the stereo shader's output.
            const auto chain = std::move(r.deferred_chain);
            DXGI_SWAP_CHAIN_DESC1 desc{};
            if (SUCCEEDED(chain->GetDesc1(&desc)) && desc.BufferCount) {
                r.presented_buffer = (chain->GetCurrentBackBufferIndex() + desc.BufferCount - 1) % desc.BufferCount;
                r.presented_pass = true;
                on_present(chain.Get(), r.queue.Get());
                r.presented_pass = false;
            }
            if (!r.instance) return;
        }
        handle_events();
        if (r.instance && r.running) input_sync(r.session, r.state == XR_SESSION_STATE_FOCUSED);
        if (!r.instance || !r.running || r.frame_begun) return;
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        r.frame = {XR_TYPE_FRAME_STATE};
        XrResult result = s.xr.wait_frame(r.session, &wait, &r.frame);
        if (XR_FAILED(result)) {
            teardown("Waiting for the headset failed: " + result_text(result) + ".", 5);
            return;
        }
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        result = s.xr.begin_frame(r.session, &begin);
        if (XR_FAILED(result)) {
            teardown("Starting a headset frame failed: " + result_text(result) + ".", 5);
            return;
        }
        r.frame_begun = true;
        s.display_period.store(r.frame.predictedDisplayPeriod, std::memory_order_release);
        s.predicted_display_time.store(r.frame.predictedDisplayTime, std::memory_order_release);
        s.tracking.store(!r.exit_requested, std::memory_order_release);
    } catch (...) {
        teardown("Unexpected error while pacing VR frames.", 5);
    }
}
}
