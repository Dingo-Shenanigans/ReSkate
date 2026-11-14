#include "frame_generation_retain.h"

#include "Engine/Core/Hooks/patches.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/frame_generation.h"

#include <array>

namespace dingosdk {
bool start_frame_generation_retain(std::uintptr_t base, std::string& error) {
    namespace contract = addr::frame_generation;
    // The retain byte read from the asked-for mode instead: mov eax, dword ptr [rbx + 0x17c] / nop.
    // The neg/sbb/and after it test al, so the "retain resources when off" bit is set whenever the
    // mode is anything but off (its values, 0 to 3, all fit in al). A frame its checks turn off keeps the resources; turning
    // frame generation off in the settings frees them.
    constexpr auto mode = contract::mode_offset;
    constexpr std::array<std::uint8_t, contract::retain_flag_prefix.size()> from_mode{
        0x8b, 0x83, static_cast<std::uint8_t>(mode), static_cast<std::uint8_t>(mode >> 8), 0x00, 0x00, 0x90};
    const auto address = base + contract::retain_flag_rva;
    const auto status = patch_code("frame generation retains its resources while it is on", address, contract::retain_flag_prefix, from_mode);
    if (status == HookOk) return true;
    // A refused patch wrote nothing; a failed write may have, so the original goes back.
    if (status != HookBytesDiffer) (void)patch_code_restore(address, contract::retain_flag_prefix);
    error = status == HookBytesDiffer ? std::string("the frame generation update does not match the supported game")
                                      : std::string("the frame generation flags could not be written: ") + hook_status_string(status);
    return false;
}
} // namespace dingosdk
