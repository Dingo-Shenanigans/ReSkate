#pragma once
#include "Engine/Game/Settings/transient_count_lease.h"
#include "Engine/Game/Build/20260929/named_settings.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Core/Platform/memory.h"
#include <Windows.h>
#include <algorithm>
#include <array>

namespace dingosdk::effects {
// Native renderer truncates whole later material groups at these limits.
// Read-only live recon: Low environment count 120, total view count 256.
// The named typed setter notifies the engine to resize its render buffers.
// This is the in-memory SystemSettings manager, not the profile-option setter
// hooked by local_native_settings.cpp. The pinned setter copies the field,
// increments DdcDirtyVersion and invokes listeners; it has no profile write.
// named_settings.cpp persists only the list of names, never their values.
class DrawBudget {
public:
    bool update(std::uintptr_t base,std::uint32_t extra) noexcept {
        namespace contract=addr::named_settings;
        if (!checked_) {
            base_=base; checked_=true;
            valid_=std::all_of(contract::named_settings_sites.begin(),contract::named_settings_sites.end(),[&](const auto& site) {
                std::array<unsigned char,32> bytes{};
                return memory::read_bytes(base+site.rva,bytes.data(),bytes.size()) && bytes==site.bytes;
            });
        }
        if (!valid_ || base!=base_) return false;
        bool ready=true;
        for (std::size_t i=0;i<names.size();++i) {
            const auto read=[&] {return sample(base,names[i]);};
            const auto write=[&](const CountSettingSample& expected,std::uint32_t value) {
                return assign(base,names[i],expected,value);
            };
            ready=leases_[i].update(extra,read,write) && ready;
        }
        return ready;
    }
private:
    inline static constexpr std::array names={
        "WorldRender.MaxDecalVolumeCount",
        "WorldRender.EnvironmentDecalVolumeMaxCount.Low",
        "WorldRender.EnvironmentDecalVolumeMaxCount.Medium",
        "WorldRender.EnvironmentDecalVolumeMaxCount.High",
        "WorldRender.EnvironmentDecalVolumeMaxCount.Ultra"};
    static std::optional<CountSettingSample> sample(std::uintptr_t base,const char* name) noexcept {
        namespace contract=addr::named_settings;
        CountSettingSample result;
        if (!memory::read(base+contract::named_settings_manager,result.manager) || !result.manager) return {};
        using Getter=std::uintptr_t(*)(std::uintptr_t,const char*,std::uintptr_t*,bool);
        __try {result.address=reinterpret_cast<Getter>(base+contract::named_settings_get)(result.manager,name,&result.type,false);}
        __except(EXCEPTION_EXECUTE_HANDLER) {return {};}
        if (!result.address || (result.type!=base+contract::native_uint32 && result.type!=base+contract::native_int32) ||
            !memory::read(result.address,result.value) || result.value>65536) return {};
        return result;
    }
    static bool assign(std::uintptr_t base,const char* name,const CountSettingSample& expected,std::uint32_t value) noexcept {
        const auto current=sample(base,name);
        if (!current || *current!=expected) return false;
        return write_native(base,name,expected.manager,value);
    }
    static bool write_native(std::uintptr_t base,const char* name,std::uintptr_t manager,std::uint32_t value) noexcept {
        namespace contract=addr::named_settings;
        using Setter=bool(*)(std::uintptr_t,const char*,std::uintptr_t,const void*);
        __try {return reinterpret_cast<Setter>(base+contract::named_settings_set)(manager,name,0,&value);}
        __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    }
    std::array<TransientCountLease,names.size()> leases_;
    std::uintptr_t base_{};
    bool checked_{},valid_{};
};
}
