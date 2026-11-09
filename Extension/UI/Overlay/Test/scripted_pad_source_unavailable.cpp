#include "Extension/Input/input_source_environment.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include <Windows.h>

// Constructed facts qualify only the real actor-free report pipeline. They do
// not establish any native menu, controller or gameplay state.
namespace dingosdk::native_input_source::environment {
Facts sample() noexcept {
    return {GetTickCount64(), GetCurrentThreadId(), true, true, true, true};
}
bool trusted_capture() noexcept { return false; }
bool preview_report_valid(std::uint64_t epoch, std::uint64_t now_ms) noexcept {
    return overlay::preview_report_valid(epoch, now_ms);
}
}
