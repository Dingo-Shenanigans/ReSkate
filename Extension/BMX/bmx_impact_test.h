#pragma once
#include "Engine/Core/Platform/user_data_paths.h"
#include <Windows.h>
namespace dingosdk::bmx::impact_test {
// A diagnostic mount seed, never a riding correction. Only the private probe
// profile and explicit child-process environment can select this path.
inline unsigned variant() noexcept {
    try {
        wchar_t value[16]{};
        const auto size=GetEnvironmentVariableW(L"RESKATE_BMX_IMPACT_TEST",value,16);
        if(size==0||size>=16)return 0;
        const unsigned selected=_wcsicmp(value,L"inverted")==0?1:_wcsicmp(value,L"side")==0?2:0;
        return selected&&user_data::runtime_development_selected(L"bmx-user-data-chassis-probe")?selected:0;
    } catch(...) {return 0;}
}
inline bool recovery_enabled() noexcept {
    if(!variant())return false;
    wchar_t value[4]{};
    return GetEnvironmentVariableW(L"RESKATE_BMX_IMPACT_RECOVERY",value,4)==1&&value[0]==L'1';
}
// Separate single-action lab. Never enables a normal controller mapping.
inline bool getup_enabled() noexcept {
    if(!variant()||recovery_enabled())return false;
    wchar_t value[4]{};
    return GetEnvironmentVariableW(L"RESKATE_BMX_GETUP_PULSE",value,4)==1&&value[0]==L'1';
}
}
