#pragma once
// State shared between the camera (engine thread) and the OpenXR frame loop
// (Present thread). Not part of the VR module's public interface.
#include "vr.h"
#include "vr_math.h"
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <shared_mutex>

namespace dingosdk::vr::detail {
// OpenXR entry points, resolved through openxr_loader.dll. Written once by the
// Present thread before `instance` is published; read-only afterwards.
struct Functions {
    PFN_xrGetInstanceProcAddr get_instance_proc_addr{};
    PFN_xrEnumerateInstanceExtensionProperties enumerate_instance_extension_properties{};
    PFN_xrCreateInstance create_instance{};
    PFN_xrDestroyInstance destroy_instance{};
    PFN_xrGetInstanceProperties get_instance_properties{};
    PFN_xrGetSystem get_system{};
    PFN_xrGetSystemProperties get_system_properties{};
    PFN_xrEnumerateViewConfigurationViews enumerate_view_configuration_views{};
    PFN_xrEnumerateEnvironmentBlendModes enumerate_environment_blend_modes{};
    PFN_xrGetD3D12GraphicsRequirementsKHR get_d3d12_graphics_requirements{};
    PFN_xrCreateSession create_session{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrBeginSession begin_session{};
    PFN_xrEndSession end_session{};
    PFN_xrRequestExitSession request_exit_session{};
    PFN_xrPollEvent poll_event{};
    PFN_xrCreateReferenceSpace create_reference_space{};
    PFN_xrDestroySpace destroy_space{};
    PFN_xrLocateSpace locate_space{};
    PFN_xrLocateViews locate_views{};
    PFN_xrEnumerateSwapchainFormats enumerate_swapchain_formats{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainImages enumerate_swapchain_images{};
    PFN_xrAcquireSwapchainImage acquire_swapchain_image{};
    PFN_xrWaitSwapchainImage wait_swapchain_image{};
    PFN_xrReleaseSwapchainImage release_swapchain_image{};
    PFN_xrWaitFrame wait_frame{};
    PFN_xrBeginFrame begin_frame{};
    PFN_xrEndFrame end_frame{};
    PFN_xrResultToString result_to_string{};
};

// One camera frame: which OpenXR view it rendered, and with what pose and
// frustum, so the image it produces can be submitted for that view.
struct EyeRecord {
    std::uint64_t sequence = 0;
    std::uint32_t view = 0;
    math::Pose pose; // tracking space (LOCAL), not recentered
    math::Angles fov;
    // Side-by-side input: one image holds both eyes, rendered from between them.
    bool both = false;
    math::Pose left, right;
};

struct Shared {
    std::mutex settings_mutex;
    Settings settings;
    std::atomic<bool> settings_changed{false};

    Functions xr;
    // Held exclusively by the Present thread while the session or its spaces
    // are created or destroyed; shared by the camera while it locates poses.
    std::shared_mutex xr_mutex;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local_space = XR_NULL_HANDLE, view_space = XR_NULL_HANDLE;
    // Session running and frame timing known; checked again under xr_mutex.
    std::atomic<bool> tracking{false};
    std::atomic<bool> ui_open{false}, game_menu{false};
    std::atomic<XrTime> predicted_display_time{0};
    std::atomic<XrDuration> display_period{0};
    // Width / height of the game image.
    std::atomic<float> image_aspect{16.0f / 9.0f};

    // Camera frames waiting for their image; newest last.
    std::mutex records_mutex;
    std::deque<EyeRecord> records;
    std::uint64_t next_sequence = 1;

    std::atomic<double> camera_time{-100.0};      // last VR camera write
    std::atomic<float> camera_fov_degrees{0.0f};  // native FOV for that write
    std::atomic<std::uint64_t> camera_frames{0};
    std::atomic<DWORD> camera_thread{0};
    // Bumped by recenter(); each consumer keeps the value it last applied.
    std::atomic<std::uint64_t> recenter_generation{1};
    // Comfort vignette amount for the eye images (0 none), set by the camera.
    std::atomic<float> vignette{0.0f};
};

Shared& shared();
// Controllers (vr_input.cpp), Present thread: actions created and attached with the
// session, synced each headset frame, destroyed before the session.
bool input_start(XrInstance instance, XrSession session, PFN_xrGetInstanceProcAddr get) noexcept;
void input_sync(XrSession session, bool focused) noexcept;
void input_stop() noexcept;
// left_hand_pose (below): the left controller's grip pose in `base` at `time`; false while untracked.
Settings current_settings();
double seconds_now() noexcept;
// A camera write within this many seconds keeps the VR camera active.
inline constexpr double camera_timeout = 0.25;
inline bool camera_active(double now) { return now - shared().camera_time.load(std::memory_order_acquire) < camera_timeout; }

inline math::Pose to_pose(const XrPosef& pose) {
    return {{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
        {pose.position.x, pose.position.y, pose.position.z}};
}
inline XrPosef to_xr(const math::Pose& pose) {
    XrPosef result{};
    result.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    result.position = {pose.position[0], pose.position[1], pose.position[2]};
    return result;
}
bool left_hand_pose(XrSpace base, XrTime time, math::Pose& out) noexcept;
}
