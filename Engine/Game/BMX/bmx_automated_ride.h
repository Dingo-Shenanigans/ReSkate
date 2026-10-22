#pragma once
#include <string_view>
#ifdef _WIN32
#include "Engine/Core/Platform/user_data_paths.h"
#include <Windows.h>
#endif

namespace dingosdk::bmx::automated_ride {
inline bool configuration_selected(bool development,std::wstring_view profile,std::wstring_view value) noexcept {
    return development&&profile==L"bmx-user-data-chassis-probe"&&value==L"1";
}
// Process-local rehearsal setting. It disables only physical BMX interaction
// request generation; ordinary native input and explicit console requests remain.
inline bool enabled() noexcept {
#ifdef _WIN32
    static const bool selected=[]() noexcept {
        try {
            if(!user_data::development_requested())return false;
            const auto root=user_data::override_root();if(!root)return false;
            wchar_t value[2]{};const auto count=GetEnvironmentVariableW(L"RESKATE_BMX_AUTOMATED_RIDE",value,2);
            return count==1&&configuration_selected(true,root->filename().wstring(),{value,count});
        } catch(...) {return false;}
    }();
    return selected;
#else
    return false;
#endif
}
}
