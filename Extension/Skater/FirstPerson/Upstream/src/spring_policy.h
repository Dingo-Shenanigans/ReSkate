#pragma once
#include "Extension/Skater/FirstPerson/Upstream/src/mod.h"
#include <optional>
namespace jishsfirstpersonmod {
// Mode is the live ownership sample: 1 includes normal and special Offboard children.
inline Config camera_config_for_state(Config config,bool active,int movement_mode,bool locked=false) {
    if(active && (locked || (config.disable_spring_on_foot && movement_mode==1))) config.spring.enabled=false;
    return config;
}
// Automatic queue writes must never become the user's saved Spring preference.
// Keep protecting it through restoration and retry rejected/unacknowledged writes.
class RuntimeSpringSwitch {
    bool managed_{};
    std::optional<bool> desired_,pending_;
    std::uint64_t retry_at_{};
public:
    bool protects_preference(bool option) const { return option || managed_; }
    std::optional<bool> update(bool option,bool desired,bool observed,std::uint64_t now) {
        if(option) managed_=true;
        if(!managed_) return {};
        const bool reversed=desired_ && *desired_!=desired;
        const bool replace=reversed && pending_ && *pending_!=desired;
        if(reversed) { pending_.reset();retry_at_=now; }
        desired_=desired;
        if(observed==desired && !replace) {
            pending_.reset();
            if(!option) { managed_=false;desired_.reset(); }
            return {};
        }
        if(now<retry_at_) return {};
        retry_at_=now+1000;
        return desired;
    }
    void queued(bool desired,bool accepted) {
        if(accepted) pending_=desired;
        else pending_.reset();
    }
};
}
