#pragma once
#include "Engine/Game/BMX/bmx_input.h"
#include <cstdint>
#include <string_view>
#ifdef _WIN32
#include "Engine/Core/Platform/user_data_paths.h"
#include <Windows.h>
#endif

namespace dingosdk::bmx::automated_switch {
inline constexpr std::uint64_t wall_limit_ms=200000;
inline constexpr std::uint32_t synthetic_device=0x424d5802;
inline bool configuration_selected(bool development,std::wstring_view profile,
    std::wstring_view value,std::wstring_view native_chassis) noexcept {
    return development&&profile==L"bmx-user-data-chassis-probe"&&value==L"1"&&native_chassis==L"1";
}
// One process-local request window. Neither repeated commands nor a clock
// reversal can restart an expired diagnostic and admit physical input.
class Lease {
public:
    void begin(std::uint64_t now) noexcept {if(!started_&&!expired_){started_=true;started_ms_=now;}}
    bool active(std::uint64_t now) noexcept {
        if(started_&&(now<started_ms_||now-started_ms_>=wall_limit_ms))expired_=true;
        return started_&&!expired_;
    }
    bool expired() const noexcept {return expired_;}
private:
    std::uint64_t started_ms_{};
    bool started_{},expired_{};
};
// ui_allowed is the existing unattended selftest UI policy: native/overlay
// menus and other capture owners still block. Focus/device are replaced only
// for this exact opted-in diagnostic; native ownership checks remain callers'.
inline bool lifecycle_allowed(bool lease_active,bool gameplay_ready,bool solo,
    bool ui_allowed,bool native_backend) noexcept {
    return lease_active&&gameplay_ready&&solo&&ui_allowed&&native_backend;
}
inline RawInput neutral_input() noexcept {
    RawInput raw;raw.available=raw.focused=true;raw.device=synthetic_device;return raw;
}
inline bool enabled() noexcept {
#ifdef _WIN32
    static const bool selected=[]() noexcept {
        try {
            if(!user_data::development_requested())return false;
            const auto root=user_data::override_root();if(!root)return false;
            wchar_t value[2]{},native[2]{};
            const auto count=GetEnvironmentVariableW(L"RESKATE_BMX_AUTOMATED_SWITCH",value,2);
            const auto native_count=GetEnvironmentVariableW(L"RESKATE_BMX_NATIVE_CHASSIS",native,2);
            return count==1&&native_count==1&&configuration_selected(true,root->filename().wstring(),
                {value,count},{native,native_count});
        }catch(...){return false;}
    }();
    return selected;
#else
    return false;
#endif
}
}
