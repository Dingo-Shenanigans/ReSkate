#pragma once
#include <cstdint>
namespace jishsfirstpersonmod {
// A short teleport episode only. Never restore across world/device changes.
class MarkerGlanceMemory {
    int direction_=-1;
    unsigned device_{};
    std::uintptr_t context_{};
    double deadline_{};
public:
    void clear() { *this={}; }
    void observe(bool allowed,bool teleport,int direction,unsigned device,std::uintptr_t context,double time) {
        if(!allowed || !device || !context || (direction_>=0 &&
           (device!=device_ || context!=context_ || time>deadline_))) {clear();return;}
        if(teleport && direction_>=0 && direction_<=3) return;
        if(teleport && direction>=0 && direction<=3) {
            direction_=direction;device_=device;context_=context;deadline_=time+5;
        }
    }
    int take(bool ready) {if(!ready)return -1;const auto result=direction_;clear();return result;}
};
}
