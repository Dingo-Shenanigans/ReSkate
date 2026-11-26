#include "replay_export.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/replay_export.h"
#include "Engine/Game/Build/supported_build.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/UI/Overlay/gpu_diagnostics.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include <Windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <mfidl.h>
#include <mftransform.h>

namespace dingosdk::replay_export {
namespace {
namespace native = addr::replay_export;
// The encoder passes its arguments straight on to the codec, so all four
// register arguments are forwarded as they came.
using Submit = void (*)(std::uintptr_t encoder, std::uintptr_t pixels, std::uintptr_t pitch, std::uintptr_t unused);
std::atomic<Submit> original{};
std::atomic<std::uintptr_t> image{};

// The export as it runs (running() in replay_export.h): its frames counted as the encoder takes them, on the encoder's
// thread. A pause of a few seconds between frames is the next export; it is timed from the end of the last frame's encode,
// so a slow encode (4K) is still the same export.
constexpr std::uint64_t new_export_ms = 3000, running_ms = 2000;
std::atomic<std::uint64_t> export_session{}, export_frames{}, last_frame_ms{};
std::atomic<std::uint64_t> export_began_ms{}, previous_export_ended_ms{};
// The MP4 setup's size and frame rate (a WebM export has none), and when it last ran.
std::atomic<int> setup_width{}, setup_height{};
std::atomic<float> setup_fps{};
std::atomic<std::uint64_t> setup_ms{};

void count_frame() noexcept {
    const auto now = GetTickCount64();
    const auto last = last_frame_ms.load(std::memory_order_relaxed);
    if (now - last > new_export_ms) {
        previous_export_ended_ms.store(last, std::memory_order_relaxed);
        export_began_ms.store(now, std::memory_order_relaxed);
        export_frames.store(0, std::memory_order_relaxed);
        export_session.fetch_add(1, std::memory_order_relaxed);
    }
    export_frames.fetch_add(1, std::memory_order_relaxed);
    last_frame_ms.store(now, std::memory_order_release);
}

void submit(std::uintptr_t encoder, std::uintptr_t pixels, std::uintptr_t pitch, std::uintptr_t unused) {
    // Encoding is done on this thread before the call returns, so letting every
    // frame through also holds the renderer to the pace the encoder keeps.
    std::uintptr_t vtable{};
    const bool replay = memory::peek(encoder, vtable) && vtable == image.load() + native::replay_encoder_vtable;
    if (replay) {
        *reinterpret_cast<std::uint8_t*>(encoder + native::frame_permit) = 1;
        count_frame();
    }
    original.load()(encoder, pixels, pitch, unused);
    if (replay) last_frame_ms.store(GetTickCount64(), std::memory_order_release);
}

// The export's quality: the H.264 setup reads the encoder's bitrate as it builds the output type, so
// it is set on the encoder just before. The setup's other arguments are passed on as they came.
using Configure = std::uintptr_t (*)(std::uintptr_t encoder, std::uintptr_t, std::uintptr_t, std::uintptr_t);
std::atomic<Configure> original_configure{};

void apply_quality(std::uintptr_t encoder) noexcept {
    int kilobits{}, mode{}, width{}, height{};
    float fps{};
    if (!memory::peek(encoder + native::h264_kilobits, kilobits) || !memory::peek(encoder + native::h264_rate_mode, mode) ||
        !memory::peek(encoder + native::h264_width, width) || !memory::peek(encoder + native::h264_height, height) ||
        !memory::peek(encoder + native::h264_fps, fps))
        return;
    const int quality = profile_runtime::local_replay_export_quality();
    const int wanted = export_kilobits(quality, width, height, fps);
    // What the game asked for goes to the log either way, so an export's own bitrate can be compared.
    logging::write(logging::Level::info, logging::Channel::graphics,
        std::format("Replay MP4 export: {}x{} at {:.2f} fps, the game's bitrate {} kbit/s (rate control {}); quality {}{}.", width,
            height, fps, kilobits, mode, quality_names[static_cast<std::size_t>(std::clamp(quality, 0, quality_count - 1))],
            wanted ? std::format(", encoded at {} kbit/s", wanted) : std::string()));
    if (!wanted) return;
    if (mode == 1 && kilobits > 0) {
        // Peak constrained: the peak keeps its share of the average, in whatever unit the game gave it.
        int peak{};
        if (memory::peek(encoder + native::h264_peak, peak) && peak > 0) {
            const auto scaled = static_cast<long long>(peak) * wanted / kilobits;
            *reinterpret_cast<int*>(encoder + native::h264_peak) = static_cast<int>(std::min<long long>(scaled, INT_MAX));
        }
    }
    if (mode == 3) *reinterpret_cast<int*>(encoder + native::h264_quality) = quality_percent[static_cast<std::size_t>(quality)];
    *reinterpret_cast<int*>(encoder + native::h264_kilobits) = wanted;
}

std::uintptr_t configure(std::uintptr_t encoder, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    if (encoder) {
        apply_quality(encoder);
        int width{}, height{};
        float fps{};
        if (memory::peek(encoder + native::h264_width, width) && memory::peek(encoder + native::h264_height, height) &&
            memory::peek(encoder + native::h264_fps, fps) && width > 0 && height > 0 && width <= 16384 && height <= 16384) {
            setup_width.store(width, std::memory_order_relaxed);
            setup_height.store(height, std::memory_order_relaxed);
            setup_fps.store(std::isfinite(fps) && fps > 0 && fps <= 1000 ? fps : 0.0f, std::memory_order_relaxed);
            setup_ms.store(GetTickCount64(), std::memory_order_release);
        }
    }
    return original_configure.load()(encoder, a, b, c);
}

// A hardware H.264 encoder only accepts frames from a device on its own GPU. On
// a PC with two GPUs (an AMD processor's integrated graphics beside a graphics
// card) the encoder the game gets first can belong to the GPU it does not render
// on, and the export fails with "Failed to save video".
using EnumTransforms = HRESULT(WINAPI*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*,
    IMFActivate***, UINT32*);
std::atomic<EnumTransforms> original_enum{};
constexpr GUID video_encoder_category{0xf79eac7d, 0xe545, 0x4387, {0xbd, 0xee, 0xd6, 0x47, 0xd7, 0xbd, 0xe4, 0x2a}};
constexpr GUID hardware_vendor_id{0x3aecb0cc, 0x035b, 0x4bcc, {0x81, 0x85, 0x2b, 0x8d, 0x55, 0x1e, 0xf3, 0xaf}};

// The encoders do not say which adapter they sit on, only their vendor ("VEN_1002").
UINT render_vendor() noexcept {
    LUID render{};
    if (!overlay::render_adapter_luid(render)) return 0;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(render, IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&description)))
        return 0;
    return description.VendorId;
}

HRESULT WINAPI enum_transforms(GUID category, UINT32 flags, const MFT_REGISTER_TYPE_INFO* input,
    const MFT_REGISTER_TYPE_INFO* output, IMFActivate*** activates, UINT32* count) {
    const HRESULT result = original_enum.load()(category, flags, input, output, activates, count);
    if (FAILED(result) || !IsEqualGUID(category, video_encoder_category) || !activates || !*activates || !count)
        return result;
    const UINT wanted = render_vendor();
    if (!wanted) return result;

    std::vector<IMFActivate*> kept, dropped;
    for (UINT32 i = 0; i < *count; ++i) {
        IMFActivate* const activate = (*activates)[i];
        wchar_t vendor[64]{};
        UINT32 length{};
        bool foreign{};
        if (activate && SUCCEEDED(activate->GetString(hardware_vendor_id, vendor, 64, &length))) {
            const wchar_t* const id = wcsstr(vendor, L"VEN_");
            foreign = id && static_cast<UINT>(wcstoul(id + 4, nullptr, 16)) != wanted;
        }
        (foreign ? dropped : kept).push_back(activate);
    }
    if (dropped.empty() || kept.empty()) return result;
    for (std::size_t i = 0; i < kept.size(); ++i) (*activates)[i] = kept[i];
    *count = static_cast<UINT32>(kept.size());
    for (auto* const activate : dropped) activate->Release();
    logging::write(logging::Level::info, logging::Channel::graphics,
        "Replay video export skipped hardware encoders that are not on the graphics card the game renders on.");
    return result;
}
} // namespace

bool start(std::uintptr_t base) noexcept {
    try {
        if (const auto mfplat = LoadLibraryW(L"mfplat.dll")) {
            void* const target = reinterpret_cast<void*>(GetProcAddress(mfplat, "MFTEnumEx"));
            void* previous{};
            if (target && hook_prepare(target, reinterpret_cast<void*>(&enum_transforms), &previous) == HookOk) {
                original_enum = reinterpret_cast<EnumTransforms>(previous);
                if (hook_enable(target) != HookOk) hook_remove(target);
            }
        }

        // The export quality, on its own: without it MP4 exports keep the game's bitrate.
        std::array<unsigned char, native::configure_h264_prefix.size()> setup{};
        if (base && memory::read_bytes(base + native::configure_h264, setup.data(), setup.size()) && setup == native::configure_h264_prefix) {
            auto* target = reinterpret_cast<void*>(base + native::configure_h264);
            void* previous{};
            if (hook_prepare(target, reinterpret_cast<void*>(&configure), &previous) == HookOk) {
                original_configure = reinterpret_cast<Configure>(previous);
                if (hook_enable(target) != HookOk) hook_remove(target);
            }
        } else {
            logging::write(logging::Level::warning, logging::Channel::graphics,
                "Replay export quality hook skipped: game code did not match the supported build.");
        }

        std::array<unsigned char, native::submit_frame_prefix.size()> bytes{};
        if (!base || !memory::read_bytes(base + native::submit_frame, bytes.data(), bytes.size()) ||
            bytes != native::submit_frame_prefix) {
            logging::write(logging::Level::warning, logging::Channel::graphics,
                "Replay export frame pacing hook skipped: game code did not match the supported build.");
            return false;
        }
        image = base;
        auto* target = reinterpret_cast<void*>(base + native::submit_frame);
        void* previous{};
        if (hook_prepare(target, reinterpret_cast<void*>(&submit), &previous) != HookOk) return false;
        original = reinterpret_cast<Submit>(previous);
        if (hook_enable(target) != HookOk) {
            hook_remove(target);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

Running running() noexcept {
    Running r;
    const auto last = last_frame_ms.load(std::memory_order_acquire);
    const auto now = GetTickCount64();
    r.running = last && now - last <= running_ms;
    r.session = export_session.load(std::memory_order_relaxed);
    r.frames = export_frames.load(std::memory_order_relaxed);
    r.began_ms = export_began_ms.load(std::memory_order_relaxed);
    // The MP4 setup counts for this export when it ran after the export before it ended, and at most ten seconds before
    // this one's first frame (one called off before its first frame is not this one's); a WebM export has none.
    const auto at = setup_ms.load(std::memory_order_acquire);
    const auto began = export_began_ms.load(std::memory_order_relaxed), before = previous_export_ended_ms.load(std::memory_order_relaxed);
    if (r.running && at && began && at > before && !(began > at && began - at > 10000)) {
        r.width = setup_width.load(std::memory_order_relaxed);
        r.height = setup_height.load(std::memory_order_relaxed);
        r.fps = setup_fps.load(std::memory_order_relaxed);
    }
    return r;
}
} // namespace dingosdk::replay_export
