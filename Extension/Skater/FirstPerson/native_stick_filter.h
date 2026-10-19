#pragma once
#include "right_stick_runtime.h"
#include "Engine/Game/Input/playstation_report.h"
#include <span>
namespace dingosdk::first_person_integration {
inline bool filter_sony_stick(const StickCapture& capture,PlayStationPad kind,
                              std::span<std::uint8_t> report,std::uint64_t now) {
    if(!capture.sony || report.size()<10 || report.size()>78) return false;
    const auto parsed=parse_playstation_report(kind,playstation_bluetooth(report.size()),report.data(),report.size());
    if(!parsed) return false;
    std::uint32_t buttons=parsed->buttons;
    if(parsed->left_trigger>30) buttons|=0x10000;
    if(parsed->right_trigger>30) buttons|=0x20000;
    if(!capture.matches(buttons,now)) return false;
    const unsigned start=report[0]==0x31 ? 2:report[0]==0x11 ? 3:1;
    report[start+2]=static_cast<std::uint8_t>(std::clamp(std::lround(128.f+capture.x/258.f),0l,255l));
    report[start+3]=capture.y==0 ? 128:static_cast<std::uint8_t>(std::clamp(std::lround(127.f-capture.y/258.f),0l,255l));
    update_playstation_report_crc(report);
    return true;
}
// Called only by the existing native game-facing input hooks, never raw readers.
bool stick_filter_active() noexcept;
void filter_stick(unsigned slot,overlay::GamepadValues& pad) noexcept;
void filter_stick_report(PlayStationPad kind,std::span<std::uint8_t> report) noexcept;
}
