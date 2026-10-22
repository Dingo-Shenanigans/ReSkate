#pragma once
#include <charconv>
#include <array>
#include <cstdint>
#include <string_view>

namespace dingosdk::runtime::recording_gate {
// Read-only identity from the authoritative shared gate, client thread only.
// Default/invalid/unreleased configurations return released=false.
struct CaptureIdentity {bool released{};std::uint32_t process_id{};std::uint64_t creation_filetime{},released_tick_ms{};std::array<char,32> nonce{};};
CaptureIdentity coupled_capture_identity() noexcept;
// Exclusive private native-board control is an automated recorder scenario,
// even though its launch mode intentionally excludes all BMX ride/switch work.
struct NativeBoardControlSelection {
    std::wstring_view active,trace,mode,chassis,launch,root,local;
    bool conflicts_clear{};
};
inline bool native_board_control_selected(const NativeBoardControlSelection& s) noexcept {
    return s.active==L"1"&&s.trace==L"1"&&s.mode==L"native-only"&&s.chassis==L"1"&&
        s.launch==L"human-ride"&&s.local==L"1"&&s.conflicts_clear&&
        s.root==LR"(C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-chassis-probe)";
}
// Separate exact coupled controller recording branch. It never borrows the
// legacy automated selector or schedules a legacy mount/startup command.
struct CoupledRideSelection {
    std::wstring_view native,ride,trial,force_probe,calibration,launch,root,local;
    bool both_features{},conflicts_clear{};
};
inline bool coupled_ride_requested(const CoupledRideSelection& s) noexcept {
    return s.native==L"1"&&s.ride==L"1"&&s.trial==L"1";
}
inline bool coupled_ride_selected(const CoupledRideSelection& s) noexcept {
    return s.both_features&&coupled_ride_requested(s)&&s.conflicts_clear&&s.local==L"1"&&s.launch==L"other"&&
        (s.force_probe.empty()||s.force_probe==L"0")&&(s.calibration.empty()||s.calibration==L"0")&&
        s.root==LR"(C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-coupled-test)";
}
inline bool valid_nonce(std::string_view nonce) {
    if (nonce.size() != 32) return false;
    for (const char c : nonce) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
// This receipt is a recorder acknowledgement, not independent proof of pixels.
// Exact identity and framing prevent a stale or partial file releasing a run.
inline bool matches_receipt(std::string_view text, std::string_view nonce,
                            std::uint32_t process, std::uint64_t creation) {
    if (!valid_nonce(nonce) || !process || !creation || text.size() > 256) return false;
    const auto line = [&text]() {
        const auto end = text.find('\n');
        if (end == std::string_view::npos) { text = {}; return std::string_view{}; }
        const auto result = text.substr(0, end); text.remove_prefix(end + 1); return result;
    };
    if (line() != "RESKATE_BMX_CAPTURE_READY_V1" || line() != nonce) return false;
    const auto number = [](std::string_view value, std::uint64_t wanted) {
        if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
        std::uint64_t parsed{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        return result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed == wanted;
    };
    return number(line(), process) && number(line(), creation) && text.empty();
}
enum class State { waiting, released, timed_out };
// Release must still permit the private tick after world/focus loss so that
// its existing cleanup runs. A failed status write never grants this latch.
inline bool released_for_coupled_cleanup(bool valid,bool failed,State state) noexcept {
    return valid&&!failed&&state==State::released;
}
struct Gate {
    std::uint64_t started_ms{}, timeout_ms{};
    State state{State::waiting};
    State observe(std::uint64_t now_ms, bool matching_receipt) {
        if (state != State::waiting) return state;
        // A rewound clock and the exact deadline both fail closed.
        if (now_ms < started_ms || now_ms - started_ms >= timeout_ms) state = State::timed_out;
        else if (matching_receipt) state = State::released;
        return state;
    }
};
}
