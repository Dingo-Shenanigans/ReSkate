#include "Engine/Game/BMX/bmx_pool_admission.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace p = dingosdk::bmx::pool_admission;
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
int main() {
    const p::WorldKey current{10,20,30};
    const p::Owner owner{1,2,3,4,5};
    const std::array history{current,p::WorldKey{11,21,31}};
    p::PoolView pool{current,owner,true,true,false,false,false,false,false,history};
    require(p::evaluate({},current,owner).decision == p::Decision::proceed,
        "first-ever full world key can continue to real native acquisition");
    require(p::evaluate(pool,current,owner).decision == p::Decision::proceed,
        "same-owner ready parked pool remains reusable");
    for (unsigned lane=0; lane<5; ++lane) {
        auto changed=owner;
        if(lane==0)++changed.client; else if(lane==1)++changed.entity;
        else if(lane==2)++changed.core; else if(lane==3)++changed.context; else ++changed.rig;
        const auto result=p::evaluate(pool,current,changed);
        require(result.decision == p::Decision::refused,
            "every rider-owner component independently fences the retained lifetime");
        require(std::string_view(result.issue).find("Restart this isolated game") != std::string_view::npos,
            "owner replacement gives honest isolated-process restart guidance");
    }
    for(unsigned failure=0; failure<4; ++failure) {
        auto failed=pool;
        if(failure==0)failed.blocked=true; else if(failure==1)failed.failed=true;
        else if(failure==2)failed.producer_failed=true; else failed.ready=false;
        require(p::evaluate(failed,current,owner).decision == p::Decision::refused,
            "blocked, local failure, producer failure and partial allocation all refuse current key");
        for(unsigned lane=0; lane<3; ++lane) {
            auto fresh=current;
            if(lane==0)fresh.world+=100; else if(lane==1)fresh.state+=100; else fresh.context+=100;
            require(p::evaluate(failed,fresh,owner).decision == p::Decision::proceed,
                "each changed full-key component escapes only the older key's failure");
        }
        require(p::evaluate(failed,history[1],owner).decision == p::Decision::refused,
            "a different but previously attempted key cannot allocate again");
    }
    auto busy=pool;busy.active=true;
    require(p::evaluate(busy,current,owner).decision == p::Decision::wait,
        "active pool is busy rather than terminally requiring a restart");
    busy.active=false;busy.detached=true;
    require(p::evaluate(busy,current,owner).decision == p::Decision::wait,
        "detached bike must finish its explicit recovery before mounting");
    for(unsigned lane=0; lane<3; ++lane) {
        auto missing=current;
        if(lane==0)missing.world=0;else if(lane==1)missing.state=0;else missing.context=0;
        require(p::evaluate(pool,missing,owner).decision == p::Decision::wait,
            "missing full-world observation cannot invent eligibility or terminal failure");
    }
    for(unsigned lane=0; lane<5; ++lane) {
        auto missing=owner;
        if(lane==0)missing.client=0;else if(lane==1)missing.entity=0;
        else if(lane==2)missing.core=0;else if(lane==3)missing.context=0;else missing.rig=0;
        require(p::evaluate(pool,current,missing).decision == p::Decision::wait,
            "incomplete canonical owner remains unavailable");
    }
    auto unknown=pool;unknown.owner={};
    require(p::evaluate(unknown,current,owner).decision == p::Decision::wait,
        "missing retained owner never authorizes adoption");
    const auto before=pool;
    for(unsigned i=0;i<100;++i)(void)p::evaluate(pool,current,p::Owner{1,2,3,4,6});
    require(pool.current==before.current && pool.owner==before.owner && pool.attempted==before.attempted &&
        pool.ready==before.ready && pool.failed==before.failed && pool.blocked==before.blocked &&
        pool.attempted_worlds.data()==before.attempted_worlds.data() && history[0]==current,
        "repeated refused admissions cannot mutate lifetime, counters or history");
    std::cout << "BMX full-world mount admission lifetime and failure isolation tests passed.\n";
}
