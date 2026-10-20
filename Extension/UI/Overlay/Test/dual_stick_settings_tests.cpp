#include "../dual_stick_settings.h"
// Keep assertions (including file operations) enabled in Release test builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

int main() {
    using namespace dingosdk::dual_stick;
    std::array<wchar_t, MAX_PATH> temp{}, name{};
    assert(GetTempPathW(static_cast<DWORD>(temp.size()),temp.data()));
    assert(GetTempFileNameW(temp.data(),L"dsc",0,name.data()));
    Settings settings(name.data());
    Section initial{{L"onefootflips",L"1"},{L"UnknownFutureTuning",L"123"},{L"ShowMenuOnStartup",L"0"},
                    {L"MenuKey",L"F8"},{L"RampDiagnostics",L"2"},{L"AirControls",L"1"}};
    assert(settings.write(initial));
    assert(WritePrivateProfileStringW(L"Unrelated", L"Keep", L"unchanged", name.data()));
    for (const auto& c : choices) {
        if (std::wcscmp(c.key,L"ManualCatch") == 0) assert(c.fallback == 0);
        if (std::wcscmp(c.key,L"DpadCameraSide") == 0 ||
            std::wcscmp(c.key,L"SplitGrindControls") == 0) assert(c.fallback == 1);
    }
    assert(settings.set(L"ManualCatch",1));
    assert(settings.set(L"GestureSpeed",0));
    assert(settings.set(L"DpadCameraSide",0));
    assert(settings.set(L"SplitGrindControls",1));
    assert(settings.set(L"StickSteeringAngleDegrees",23));
    assert(settings.preset(false));
    assert(settings.active(false));
    assert(settings.number(L"OneFootFlips",-1) == 0);
    assert(settings.number(L"UnknownFutureTuning",-1) == 123);
    assert(settings.text(L"MenuKey",L"") == L"F8");
    assert(settings.preset(true));
    assert(settings.active(true));
    assert(settings.number(L"SwitchStance",-1) == 3);
    assert(settings.number(L"OneFootFlips",-1) == 1);
    assert(settings.layout() == 2);
    assert(settings.number(L"XLFootDirections",-1) == 1);
    assert(settings.number(L"StickSteering",-1) == 2);
    assert(settings.number(L"ManualCatch",-1) == 1);
    assert(settings.number(L"GestureSpeed",-1) == 0);
    assert(settings.number(L"DpadCameraSide",-1) == 0);
    assert(settings.number(L"SplitGrindControls",-1) == 1);
    assert(settings.number(L"StickSteeringAngleDegrees",-1) == 23);
    assert(settings.set_layout(1) && settings.layout() == 1);
    assert(settings.number(L"XLFootDirections",-1) == 0);
    assert(!settings.active(true));
    assert(settings.set_layout(0) && settings.layout() == 0);
    assert(!settings.set_layout(3));
    assert(settings.preset(true));
    assert(settings.set(L"SwitchInvertX",1));
    assert(!settings.active(true));
    assert(settings.preset(true));
    assert(settings.number(L"SwitchInvertX",-1) == 0);
    assert(settings.set(L"DisableHoldFakie",1));
    assert(settings.set(L"OneFootFlips",0));
    assert(!settings.active(true));
    assert(settings.save(1));
    assert(settings.filled(1));
    assert(settings.set(L"StockGrabAir",1));
    assert(settings.set(L"StockTransitionGround",1));
    assert(settings.save(5));
    assert(settings.set(L"ManualCatch",0));
    assert(settings.set(L"GestureSpeed",1));
    assert(settings.set(L"DpadCameraSide",1));
    assert(settings.set(L"SplitGrindControls",0));
    assert(settings.set(L"StickSteeringAngleDegrees",55));
    assert(settings.set(L"AirControls",0));
    assert(settings.set(L"RampDiagnostics",0));
    assert(settings.set(L"UnknownFutureTuning",456));
    assert(settings.set_layout(0));
    assert(settings.set(L"DisableHoldFakie",0));
    assert(settings.set(L"AutoStance",0));
    assert(settings.set(L"PlayStationPads",2));
    assert(settings.set(L"EnabledAtStart",0));
    assert(settings.load(1));
    assert(settings.layout() == 2);
    assert(settings.number(L"DisableHoldFakie",-1) == 1);
    assert(settings.number(L"AutoStance",-1) == 0);
    assert(settings.number(L"PlayStationPads",-1) == 2);
    assert(settings.number(L"EnabledAtStart",-1) == 0);
    assert(settings.number(L"AirControls",-1) == 1);
    assert(settings.number(L"ManualCatch",-1) == 1);
    assert(settings.number(L"GestureSpeed",-1) == 1); // Removed option stays an unknown global key.
    assert(settings.number(L"DpadCameraSide",-1) == 0);
    assert(settings.number(L"SplitGrindControls",-1) == 1);
    assert(settings.number(L"StickSteeringAngleDegrees",-1) == 23);
    assert(settings.number(L"RampDiagnostics",-1) == 0);
    assert(settings.number(L"UnknownFutureTuning",-1) == 456);
    assert(settings.number(L"StockGrabAir",-1) == 0);
    assert(settings.load(5));
    assert(settings.number(L"StockTransitionGround",-1) == 1);
    assert(settings.text(L"Keep",L"missing",L"Unrelated") == L"unchanged");
    // Older slots must preserve newer options they did not save.
    assert(settings.write(Section{{L"PresetSaved",L"1"},{L"PopOnRelease",L"0"}},L"CustomPreset2"));
    assert(settings.set(L"ManualCatch",0) && settings.set(L"GestureSpeed",1));
    assert(settings.load(2));
    assert(settings.number(L"ManualCatch",-1) == 0 && settings.number(L"GestureSpeed",-1) == 1);
    assert(!settings.load(3));
    assert(!settings.save(0) && !settings.save(6));
    Section readback;
    assert(settings.read(readback));
    assert(readback.count(L"OneFootFlips") == 1);
    assert(SetFileAttributesW(name.data(),FILE_ATTRIBUTE_READONLY));
    assert(!settings.set(L"OneFootFlips",1));
    assert(!settings.preset(false));
    assert(SetFileAttributesW(name.data(),FILE_ATTRIBUTE_NORMAL));
    assert(DeleteFileW(name.data()));
    assert(!settings.available());
    assert(!settings.set(L"OneFootFlips",1) && !settings.preset(false) && !settings.save(1));
    assert(GetFileAttributesW(name.data()) == INVALID_FILE_ATTRIBUTES);
    std::cout << "Dual Stick settings integration checks passed\n";
}
