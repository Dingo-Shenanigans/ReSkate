#include "Runtime/startup_recording_gate.h"
#include <cstdlib>
#include <iostream>
#include <string>
namespace g = dingosdk::runtime::recording_gate;
unsigned checks{};
void require(bool value, const char* reason) { ++checks; if (!value) { std::cerr << reason << '\n'; std::exit(1); } }
int main() {
    const std::string nonce = "1234567890abcdef1234567890abcdef";
    const auto receipt = "RESKATE_BMX_CAPTURE_READY_V1\n" + nonce + "\n123\n987654321\n";
    require(g::matches_receipt(receipt, nonce, 123, 987654321), "Current complete recorder receipt releases its process.");
    require(!g::matches_receipt(receipt, nonce, 124, 987654321), "Other PID cannot release.");
    require(!g::matches_receipt(receipt, nonce, 123, 987654322), "Reused PID cannot release.");
    require(!g::matches_receipt(receipt, "0234567890abcdef1234567890abcdef", 123, 987654321), "Other run nonce cannot release.");
    require(!g::matches_receipt(receipt.substr(0, receipt.size()-1), nonce, 123, 987654321), "Partially written receipt cannot release.");
    require(!g::matches_receipt(receipt + "extra\n", nonce, 123, 987654321), "Trailing receipt data cannot release.");
    require(!g::matches_receipt("\xef\xbb\xbf" + receipt, nonce, 123, 987654321), "BOM cannot silently change protocol.");
    require(!g::valid_nonce("1234567890ABCDEF1234567890ABCDEF") && !g::valid_nonce(""), "Malformed nonce is rejected.");
    g::Gate timeout{100, 1000};
    require(timeout.observe(1099, false) == g::State::waiting, "No implicit timeout release.");
    require(timeout.observe(1100, true) == g::State::timed_out, "Receipt at deadline cannot release.");
    require(timeout.observe(1101, true) == g::State::timed_out, "Timed out gate never reopens.");
    g::Gate ready{100,1000};
    require(ready.observe(200,true) == g::State::released && ready.observe(9999,false) == g::State::released, "Acknowledgement latches once.");
    g::Gate rewind{100,1000}; require(rewind.observe(99,true) == g::State::timed_out, "Clock rewind fails closed.");
    const g::NativeBoardControlSelection native{L"1",L"1",L"native-only",L"1",L"human-ride",
        LR"(C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-chassis-probe)",L"1",true};
    require(g::native_board_control_selected(native), "Explicit active NativeOnly has its own recording gate admission.");
    std::wstring_view g::NativeBoardControlSelection::* const fields[]{
        &g::NativeBoardControlSelection::active,&g::NativeBoardControlSelection::trace,
        &g::NativeBoardControlSelection::mode,&g::NativeBoardControlSelection::chassis,
        &g::NativeBoardControlSelection::launch,&g::NativeBoardControlSelection::root,&g::NativeBoardControlSelection::local};
    for(const auto field:fields)for(const auto bad:{L"",L"0",L"true",L"1 ",L"other"}) {
        auto changed=native;changed.*field=bad;
        require(!g::native_board_control_selected(changed), "Every missing/malformed selector rejects active native-board gate.");
    }
    auto changed=native;changed.conflicts_clear=false;
    require(!g::native_board_control_selected(changed), "Existing automated/QuickSwitch conflicts reject the exclusive branch.");
    changed=native;changed.active=L"0";
    require(!g::native_board_control_selected(changed), "Manual NativeOnly/human Ride alone cannot admit the active gate.");
    changed=native;changed.root=LR"(C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-working-profile)";
    require(!g::native_board_control_selected(changed), "Human controller profile cannot admit this private control gate.");
    changed=native;changed.mode=L"quick-switch";
    require(!g::native_board_control_selected(changed), "QuickSwitch cannot masquerade as the private control.");
    const g::CoupledRideSelection coupled{L"1",L"1",L"1",L"",L"",L"other",
        LR"(C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-coupled-test)",L"1",true,true};
    require(g::coupled_ride_selected(coupled), "Exact no-Ride coupled request admits recorder gate.");
    const auto bad_values={L"",L"0",L"true",L"1 ",L"human-ride",L"unattended-test"};
    std::wstring_view g::CoupledRideSelection::* const exact_fields[]{
        &g::CoupledRideSelection::native,&g::CoupledRideSelection::ride,&g::CoupledRideSelection::trial,
        &g::CoupledRideSelection::launch,&g::CoupledRideSelection::root,&g::CoupledRideSelection::local};
    for(const auto field:exact_fields)for(const auto bad:bad_values){auto v=coupled;v.*field=bad;
        require(!g::coupled_ride_selected(v), "Every malformed required coupled selector rejects.");}
    auto v=coupled;v.both_features=false;require(!g::coupled_ride_selected(v), "Both compiled features required.");
    v=coupled;v.conflicts_clear=false;require(!g::coupled_ride_selected(v), "Legacy automated/board conflicts reject coupled admission.");
    for(const auto bad:{L"1",L"true",L"pair",L"world",L" "}){v=coupled;v.force_probe=bad;
        require(!g::coupled_ride_selected(v), "Force probe cannot borrow controller capture gate.");
        v=coupled;v.calibration=bad;require(!g::coupled_ride_selected(v), "Calibration cannot borrow controller capture gate.");}
    v=coupled;v.force_probe=v.calibration=L"0";require(g::coupled_ride_selected(v), "Explicit disabled optional selectors match absent ones.");
    v=coupled;v.launch=L"human-ride";require(!g::coupled_ride_selected(v), "Ride legacy mount command route cannot qualify.");
    for(bool valid:{false,true})for(bool failed:{false,true})for(auto state:{g::State::waiting,g::State::released,g::State::timed_out})
        require(g::released_for_coupled_cleanup(valid,failed,state)==(valid&&!failed&&state==g::State::released),
            "Only a successful release remains permitted through later world loss for cleanup.");
    std::cout << checks << " recording gate policy/identity/deadline/selection assertions PASS.\n";
}
