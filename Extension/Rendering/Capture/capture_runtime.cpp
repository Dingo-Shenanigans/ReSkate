#include "capture_runtime.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>

namespace dingosdk::capture {
namespace {
std::span<const std::uint8_t> built_in(const wchar_t* name) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&built_in), &module))
        return {};
    const auto resource = FindResourceW(module, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto handle = resource ? LoadResource(module, resource) : nullptr;
    const auto* data = handle ? static_cast<const std::uint8_t*>(LockResource(handle)) : nullptr;
    return data ? std::span<const std::uint8_t>(data, SizeofResource(module, resource)) : std::span<const std::uint8_t>{};
}
bool grab_active() noexcept { return recorder().active(); }
bool grab_wants() noexcept { return recorder().wants_frame(); }
void grab_take(const overlay::GrabbedFrame& frame) noexcept {
    recorder().take_frame({frame.data, frame.width, frame.height, frame.pitch, frame.bgra, frame.depth, frame.depth_pitch});
}
constexpr overlay::FrameGrabHost frame_grab_host{&grab_active, &grab_wants, &grab_take};
} // namespace

Recorder& recorder() noexcept {
    static auto* value = new Recorder; // process lifetime: its saving thread may outlive statics
    return *value;
}
const overlay::FrameGrabHost* frame_grab() noexcept { return &frame_grab_host; }
PanelFonts menu_fonts() noexcept {
    static const PanelFonts fonts{built_in(L"FONT_HEADING"), built_in(L"FONT_BODY"), built_in(L"FONT_BRUSH")};
    return fonts;
}
} // namespace dingosdk::capture
