#include "Engine/Game/BMX/bmx_skeleton_phase_policy.h"
#include "Engine/Game/BMX/bmx_pose_comparison_policy.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace p=dingosdk::bmx::skeleton_phase_probe::detail;
void require(bool x,const char* text){if(!x){std::cerr<<text<<'\n';std::exit(1);}}
int main(){
    {
        namespace c=dingosdk::bmx::pose_comparison;
        c::Scope scope{true,true,true,true,true,true,true,true,true};
        const auto dual=c::select(L"Default",scope),render=c::select(L"RenderOnly",scope),none=c::select(L"None",scope);
        require(dual.qualified_scope&&!dual.refused&&dual.mode==c::Mode::Default,"qualified Default is the unchanged paired control");
        require(render.qualified_scope&&!render.refused&&render.mode==c::Mode::RenderOnly,"explicit fully scoped RenderOnly accepted");
        require(none.qualified_scope&&!none.refused&&none.mode==c::Mode::None,"explicit fully scoped None accepted");
        unsigned calls{};
        require(c::dispatch(dual,false,[&]{++calls;})&&c::dispatch(dual,true,[&]{++calls;})&&calls==2,"Default preserves both custom callbacks exactly once");
        calls=0;
        require(!c::dispatch(render,false,[&]{++calls;})&&c::dispatch(render,true,[&]{++calls;})&&calls==1,"RenderOnly suppresses animation apply before invocation, preserves render apply");
        calls=0;
        require(!c::dispatch(none,false,[&]{++calls;})&&!c::dispatch(none,true,[&]{++calls;})&&calls==0,"None invokes zero custom rider applies");
        constexpr std::array<bool c::Scope::*,9> fields{&c::Scope::development,&c::Scope::exact_profile,&c::Scope::postfall,
            &c::Scope::phase,&c::Scope::producer,&c::Scope::side,&c::Scope::getup,&c::Scope::unattended,&c::Scope::native_chassis};
        for(const auto field:fields){
            auto missing=scope;missing.*field=false;
            for(const auto request:{L"RenderOnly",L"None"}){
                const auto refused=c::select(request,missing);
                require(refused.refused&&!refused.qualified_scope&&refused.mode==c::Mode::Default,"each missing private scope proof independently refuses suppression");
                require(c::allows(refused,false)&&c::allows(refused,true),"refusal never changes ordinary guarded rider behavior");
            }
        }
        for(const auto request:{L"renderonly",L"None ",L"unknown"}){
            const auto refused=c::select(request,scope);
            require(refused.refused&&refused.mode==c::Mode::Default,"unknown/noncanonical selectors are refused, not coerced");
        }
        require(!c::select(L"",{}).refused&&!c::select(L"Default",{}).refused&&
                c::allows(c::select(L"Default",{}),false)&&c::allows(c::select(L"Default",{}),true),"absent and explicit Default preserve production behavior without private scope");
    }
    {
        // Exact pure dispatcher used by proposed live wrapper; no native calls.
        std::array<unsigned,3> order{};unsigned at{},calls{},posts{},error=42;
        std::uintptr_t actual_rig{};float actual_seconds{};bool actual_effective{};
        const auto original=[&](std::uintptr_t rig,float seconds,bool effective) {
            order[at++]=2;++calls;require(error==42,"pre observer cannot alter error seen by original");
            actual_rig=rig;actual_seconds=seconds;actual_effective=effective;error=73;
        };
        p::invoke_observed(123,.016f,false,original,
            [&]() noexcept {order[at++]=1;error=999;return std::uint64_t{81};},
            [&](std::uint64_t cookie) noexcept {order[at++]=3;++posts;require(cookie==81,"cookie paired exactly");
                require(error==73,"post observes error produced by original");error=1000;},
            [&]() noexcept{return error;},[&](unsigned e) noexcept{error=e;});
        require(calls==1&&posts==1&&order==std::array<unsigned,3>{1,2,3},"pre original post ordering and exactly once dispatch");
        require(actual_rig==123&&actual_seconds==.016f&&!actual_effective,"effective arguments unchanged");
        require(error==73,"post observer cannot alter native returned error");
        calls=posts=at=0;error=42;
        p::invoke_observed(7,.02f,true,original,[]() noexcept{return std::uint64_t{0};},
            [&](std::uint64_t) noexcept{++posts;},[&]() noexcept{return error;},[&](unsigned e) noexcept{error=e;});
        require(calls==1&&posts==0&&actual_effective,"zero cookie does not skip original or invent post");
        calls=posts=0;bool propagated{};error=42;volatile bool throw_requested=true;
        try {
            p::invoke_observed(7,.02f,true,[&](std::uintptr_t,float,bool){++calls;error=91;if(throw_requested)throw 5;},
                []() noexcept{return std::uint64_t{1};},[&](std::uint64_t) noexcept{++posts;},
                [&]() noexcept{return error;},[&](unsigned e) noexcept{error=e;});
        }catch(int e){propagated=e==5;}
        require(propagated&&calls==1&&posts==0&&error==91,"native exception path propagates with no post or synthesized outcome");
    }
    {
        constexpr std::uintptr_t base=0x7ff600000000ULL;
        require(p::image_range(base,base+0x6000000,0xe0,p::ImageUse::vtable_data),"source rodata vtable prefix and+D8 slot are bounded");
        require(p::image_range(base,base+0x47770c0,1,p::ImageUse::code_target),"source retained tail lies in executable xtls");
        require(p::image_range(base,base+0x865c000,1,p::ImageUse::code_target),"source writable executable cfg section is retained, not invented as unreadable");
        require(!p::image_range(base,base+0x47770c0,0xe0,p::ImageUse::vtable_data),"code bytes are not a vtable data range");
        require(!p::image_range(base,base+0x6000000,1,p::ImageUse::code_target),"rodata is not promoted to an executable target");
        require(!p::image_range(base,base+0x6f7a000-0xd8,0xe0,p::ImageUse::vtable_data),"vtable+D8 read cannot cross a source section boundary");
        require(!p::image_range(base,base+0x6f7a000-0xd8,0,p::ImageUse::vtable_data),"zero-byte range never qualifies");
        require(!p::image_range(base,base-1,1,p::ImageUse::code_target),"below-image target rejected without subtraction wrap");
        require(!p::image_range(base,base+0x9144000,1,p::ImageUse::vtable_data),"image end is excluded");
        require(!p::image_range(base,base+0x8bb60a8,1,p::ImageUse::vtable_data),"unmapped virtual gap cannot borrow a neighboring source section");
        require(!p::image_range(0xfffffffffffffff0ULL,0xfffffffffffffff1ULL,1,p::ImageUse::code_target),"wrapped image base rejected");
        require(!p::image_range(base,base+0x6000000,std::numeric_limits<std::size_t>::max(),p::ImageUse::vtable_data),"oversized read rejected without addition overflow");
        for(const auto& section:p::image_sections) {
            const auto at=base+section.rva;
            const bool code=(section.flags&0x20000000u)!=0,read=(section.flags&0x40000000u)!=0;
            require(p::image_range(base,at,section.size,p::ImageUse::code_target)==(read&&code),"entire exact source section uses its recorded execute permissions");
            require(p::image_range(base,at,section.size,p::ImageUse::vtable_data)==(read&&!code),"entire exact source section uses its recorded data permissions");
            require(!p::image_range(base,at,static_cast<std::size_t>(section.size)+1,p::ImageUse::vtable_data)&&
                    !p::image_range(base,at,static_cast<std::size_t>(section.size)+1,p::ImageUse::code_target),
                "range cannot overrun any exact source virtual section");
        }
    }
    {
        namespace c=dingosdk::bmx::pose_comparison;
        constexpr std::array<bool c::Scope::*,9> fields{&c::Scope::development,&c::Scope::exact_profile,&c::Scope::postfall,
            &c::Scope::phase,&c::Scope::producer,&c::Scope::side,&c::Scope::getup,&c::Scope::unattended,&c::Scope::native_chassis};
        c::MotionBasisDecision experiment;
        for(unsigned bits=0;bits<512;++bits){c::Scope scope{};
            for(unsigned field=0;field<9;++field)scope.*fields[field]=bool(bits&(1u<<field));
            const auto pose=c::select(L"None",scope);
            const auto chosen=c::select_motion_basis(L"PreserveSuppliedBasis",pose,scope);
            require((chosen.mode==c::MotionBasis::PreserveSuppliedBasis)==(bits==511),"basis contrast requires every private scope gate AND actual None selection");
            require(chosen.refused==(bits!=511),"missing scope refuses diagnostic but preserves ordinary motion basis");
            for(const auto request:{L"",L"Default"}){
                const auto unchanged=c::select_motion_basis(request,pose,scope);
                require(unchanged.mode==c::MotionBasis::Default&&!unchanged.refused&&!unchanged.qualified_scope,"default selection does not depend on private flags");
            }
            if(bits==511)experiment=chosen;
        }
        const c::Scope scope{true,true,true,true,true,true,true,true,true};
        for(const auto pose_name:{L"Default",L"RenderOnly"})
            require(c::select_motion_basis(L"PreserveSuppliedBasis",c::select(pose_name,scope),scope).refused,"custom rider writes must be excluded by None");
        for(const auto request:{L"preservesuppliedbasis",L"PreserveSuppliedBasis ",L"unknown"})
            require(c::select_motion_basis(request,c::select(L"None",scope),scope).refused,"noncanonical basis selector refused");
        const std::array<float,16> supplied{0,-0.f,-1,0, 0,1,-0.f,0, 1,0,0,0, 100,200,300,1};
        const std::array<float,16> selected{1,0,0,0, 0,0,1,0, 0,-1,0,0, 7,8,9,1};
        auto target=selected;
        require(!c::preserve_motion_basis({},true,supplied,target)&&std::bit_cast<std::array<std::uint32_t,16>>(target)==std::bit_cast<std::array<std::uint32_t,16>>(selected),"Default target remains bit exact in all16 words");
        require(!c::preserve_motion_basis(experiment,false,supplied,target)&&target==selected,"legacy yaw target is untouched by full-rotation-only experiment");
        require(c::preserve_motion_basis(experiment,true,supplied,target),"explicit full rotation contrast selected");
        const auto words=std::bit_cast<std::array<std::uint32_t,16>>(target);
        const auto incoming=std::bit_cast<std::array<std::uint32_t,16>>(supplied),normal=std::bit_cast<std::array<std::uint32_t,16>>(selected);
        for(unsigned i=0;i<16;++i)require(words[i]==(i<12&&i%4<3?incoming[i]:normal[i]),"exact supplied3x3 including signed zero, fixed XYZ and homogeneous remainder");
        auto refused=experiment;refused.refused=true;target=selected;
        require(!c::preserve_motion_basis(refused,true,supplied,target)&&target==selected,"refused selector cannot change target");
    }
    const p::Owner owner{1,2,3,4,5};const p::Clock clock{true,99,1.25};
    require(p::snapshot_qualified(owner,owner,clock,clock),"same owner and exact listener serial qualify measurement association");
    for(unsigned field=0;field<5;++field){
        auto changed=owner;
        if(field==0)++changed.base;else if(field==1)++changed.entity;else if(field==2)++changed.core;else if(field==3)++changed.context;else ++changed.rig;
        require(!p::snapshot_qualified(owner,changed,clock,clock),"every identity field is independently checked");
    }
    require(!p::snapshot_qualified({},owner,clock,clock),"absent owner never qualifies");
    auto changed_clock=clock;++changed_clock.serial;
    require(!p::same_clock(clock,changed_clock),"serial crossing cannot form a coherent measurement half");
    changed_clock=clock;changed_clock.time+=1.0/60.0;
    require(!p::same_clock(clock,changed_clock),"equal serial with differing clock is rejected");
    changed_clock=clock;changed_clock.available=false;
    require(!p::same_clock(clock,changed_clock),"unavailable listener clock stays unknown at startup");
    changed_clock=clock;changed_clock.time=std::numeric_limits<double>::quiet_NaN();
    require(!p::same_clock(clock,changed_clock),"nonfinite clock never qualifies");
    require(p::pair_qualified(true,true,owner,owner,clock,clock),"known pair may be associated");
    require(!p::pair_qualified(true,false,owner,owner,clock,clock),"unknown post cannot borrow a good pre");
    require(!p::pair_qualified(false,true,owner,owner,clock,clock),"good post cannot repair unknown pre");
    require(p::cookie_matches(512,10,10,9,9),"last fixed slot cookie is valid");
    require(!p::cookie_matches(0,10,10,9,9),"zero cookie means no accepted pre");
    require(!p::cookie_matches(513,10,10,9,9),"out of range cookie rejected");
    require(!p::cookie_matches(1,11,10,9,9),"nested or mismatched invocation rejected");
    require(!p::cookie_matches(1,10,10,8,9),"cross-thread post rejected");
    require(p::offsets[2]+p::limits[2]==p::capacity,"all fixed window partitions are bounded");
    std::array<std::uint32_t,4> zero{};bool known{};
    for(unsigned bits=0;bits<8;++bits){
        const bool effective=bits&1,flags_available=bits&2,offset_available=bits&4;
        const bool predicts=p::copy_guard_prediction(effective,flags_available,0x8000,offset_available,zero,known);
        require(known==(flags_available&&offset_available),"missing raw guards remain unknown");
        require(predicts==(effective&&flags_available&&offset_available),"all three observed guards required");
    }
    require(!p::copy_guard_prediction(true,true,0,true,zero,known)&&known,"unset context bit predicts branch closed");
    auto one=zero;one[0]=std::bit_cast<std::uint32_t>(1.0f);
    require(!p::copy_guard_prediction(true,true,0x8000,true,one,known)&&known,"exact squared length one fails strict guard");
    one[0]=std::bit_cast<std::uint32_t>(.5f);
    require(p::copy_guard_prediction(true,true,0x8000,true,one,known)&&known,"finite raw half offset predicts guard open");
    one[3]=0x7fc00000;
    require(p::copy_guard_prediction(true,true,0x8000,true,one,known)&&known,"padding lane NaN does not poison independent XYZ guard");
    require(p::finite_words(one,0,3)&&!p::finite_words(one,0,4),"raw fourth-lane NaN is retained and separately qualified");
    one[0]=0x7fc00000;
    require(!p::copy_guard_prediction(true,true,0x8000,true,one,known)&&!known,"XYZ NaN cannot prove copy guard prediction");
    volatile float largest_finite=std::numeric_limits<float>::max();
    one[0]=std::bit_cast<std::uint32_t>(static_cast<float>(largest_finite));one[3]=0;
    require(!p::copy_guard_prediction(true,true,0x8000,true,one,known)&&!known,"overflowed squared norm remains unknown");
    require(!p::finite_words(zero,3,2),"finite accessor is bounded");
    {
        // Same generic dispatcher used at both new existing-original seams.
        unsigned calls{},before{},after{},error=17;std::array<float,16> selected{};
        selected[12]=8;std::uint8_t flags=0xa5;std::uintptr_t rig=123,context=456;
        const auto original=[&](std::uintptr_t actual_rig,std::uintptr_t actual_context,
            const std::array<float,16>* target,std::uint8_t actual_flags) {
            ++calls;require(error==17&&actual_rig==rig&&actual_context==context&&target==&selected&&actual_flags==0xa5,
                "passive motion dispatch preserves exact target address flags and original error");error=91;
        };
        p::invoke_call_observed([&]{original(rig,context,&selected,flags);},
            [&]() noexcept {++before;error=777;return std::uint64_t{1};},
            [&](std::uint64_t cookie) noexcept {++after;require(cookie==1&&error==91,"post occurs immediately after original");error=888;},
            [&]() noexcept{return error;},[&](unsigned value) noexcept{error=value;});
        require(calls==1&&before==1&&after==1&&error==91,"motion original exactly once and returned error preserved");
        calls=after=0;error=17;
        p::invoke_call_observed([&]{original(rig,context,&selected,flags);},[]() noexcept{return std::uint64_t{0};},
            [&](std::uint64_t) noexcept{++after;},[&]() noexcept{return error;},[&](unsigned value) noexcept{error=value;});
        require(calls==1&&after==0&&error==91,"missing pre never suppresses native motion or invents post");
        calls=after=0;bool propagated=false;error=17;volatile bool throw_requested=true;
        try {
            p::invoke_call_observed([&]{++calls;error=93;if(throw_requested)throw 6;},
                []() noexcept{return std::uint64_t{1};},[&](std::uint64_t) noexcept{++after;},
                [&]() noexcept{return error;},[&](unsigned value) noexcept{error=value;});
        }catch(int value){propagated=value==6;}
        require(propagated&&calls==1&&after==0&&error==93,"motion or animation throw propagates with no returned post");
        require(p::producer_cookie_matches(32,18,18,9,9),"last paired producer slot admitted");
        require(!p::producer_cookie_matches(33,18,18,9,9),"writer prefix cannot acquire a post");
        require(!p::producer_cookie_matches(0,18,18,9,9)&&!p::producer_cookie_matches(1,19,18,9,9)&&
            !p::producer_cookie_matches(1,18,18,8,9),"producer zero nested stale and cross-thread cookies rejected");
    }
    {
        p::ProducerBudget budget;std::array<bool,p::producer_snapshots> used{};std::size_t records{},snapshots{};
        for(std::uint64_t epoch=100;epoch<108;++epoch)for(unsigned k=0;k<3;++k) {
            const p::Clock at{true,epoch,static_cast<double>(epoch)/60};
            for(unsigned duplicate=0;duplicate<2;++duplicate) {
                const auto r=budget.reserve(static_cast<p::ProducerKind>(k),owner,at);
                require(r.accepted&&r.record/16==k,"each captured duplicate consumes its own kind slot");++records;
                const auto pre=p::producer_pre_index(r.record);require(pre<80&&!used[pre],"unique bounded pre cell");used[pre]=true;++snapshots;
                const auto post=p::producer_post_index(r.record);
                if(k<2){require(post<80&&!used[post],"paired original gets unique bounded post cell");used[post]=true;++snapshots;}
                else require(post==80,"writer has no post cell");
            }
            require(!budget.reserve(static_cast<p::ProducerKind>(k),owner,at).accepted,"third duplicate is dropped without native dispatch changes");
        }
        require(records==48&&snapshots==80,"all three quotas fit exactly48records and80snapshots");
        for(bool present:used)require(present,"no alias or unused hole in full producer snapshot layout");
        require(!budget.reserve(p::ProducerKind::motion,owner,{true,108,1.8}).accepted,"capacity never resets when epoch advances");
    }
    {
        p::ProducerBudget budget;
        require(!budget.reserve(p::ProducerKind::motion,{},{}).accepted&&!budget.pinned.present(),"unavailable owner never pins or borrows remembered owner");
        require(budget.reserve(p::ProducerKind::motion,owner,{}).accepted&&budget.reserve(p::ProducerKind::motion,owner,{}).accepted,
            "at most two unknown-clock prefixes may be retained");
        require(!budget.reserve(p::ProducerKind::motion,owner,{}).accepted,"stopped unknown clock cannot saturate snapshots");
        for(std::uint64_t epoch=1;epoch<=8;++epoch)for(unsigned n=0;n<2;++n)
            (void)budget.reserve(p::ProducerKind::motion,owner,{true,epoch,static_cast<double>(epoch)/60});
        require(budget.taken[0]==16&&budget.unknown[0]==2,"unknown-clock records consume same16record total");
        p::ProducerBudget span;
        require(span.reserve(p::ProducerKind::animation,owner,{true,90,1.5}).accepted,"first qualified clock anchors bounded span");
        require(!span.reserve(p::ProducerKind::animation,{},{}).accepted&&!span.sealed,"temporary owner unknown does not create lifetime death");
        require(span.reserve(p::ProducerKind::animation,owner,{true,97,1.6}).accepted,"eighth arithmetic epoch admitted even with missing intermediate measurements");
        require(!span.reserve(p::ProducerKind::writer,owner,{true,98,1.7}).accepted&&span.sealed,"ninth arithmetic epoch seals instead of counting arbitrary distinct clocks");
        p::ProducerBudget rollback;(void)rollback.reserve(p::ProducerKind::motion,owner,{true,12,.2});
        require(!rollback.reserve(p::ProducerKind::animation,owner,{true,11,.18}).accepted&&rollback.sealed,"clock rollback never resets window");
        p::ProducerBudget within_window;
        require(within_window.reserve(p::ProducerKind::motion,owner,{true,100,1.6}).accepted&&
            within_window.reserve(p::ProducerKind::animation,owner,{true,107,1.7}).accepted,
            "high-water serial is shared across motion and animation kinds");
        const auto prior_taken=within_window.taken;
        require(!within_window.reserve(p::ProducerKind::writer,owner,{true,103,1.65}).accepted&&within_window.sealed&&
            within_window.highest_serial==107&&within_window.taken==prior_taken,
            "100 to107 to103 rollback inside original window seals without reserving or rearming");
        require(!within_window.reserve(p::ProducerKind::motion,owner,{true,107,1.7}).accepted,
            "return to prior high-water cannot unseal or consume a new quota");
        p::ProducerBudget dropped_highwater;
        (void)dropped_highwater.reserve(p::ProducerKind::motion,owner,{});
        (void)dropped_highwater.reserve(p::ProducerKind::motion,owner,{});
        for(std::uint64_t serial=100;serial<107;++serial)for(unsigned n=0;n<2;++n)
            (void)dropped_highwater.reserve(p::ProducerKind::motion,owner,{true,serial,static_cast<double>(serial)/60});
        require(!dropped_highwater.reserve(p::ProducerKind::motion,owner,{true,107,1.8}).accepted&&
            dropped_highwater.highest_serial==107&&!dropped_highwater.sealed,
            "full-kind admission attempt still advances shared serial high-water");
        require(!dropped_highwater.reserve(p::ProducerKind::writer,owner,{true,105,1.7}).accepted&&dropped_highwater.sealed,
            "rollback cannot hide behind another kind's full quota");
        for(unsigned field=0;field<5;++field) {
            p::ProducerBudget identity;(void)identity.reserve(p::ProducerKind::motion,owner,clock);auto other=owner;
            if(field==0)++other.base;else if(field==1)++other.entity;else if(field==2)++other.core;else if(field==3)++other.context;else ++other.rig;
            require(!identity.reserve(p::ProducerKind::animation,other,clock).accepted&&identity.sealed&&identity.pinned==owner,
                "any confirmed owner replacement is terminal and cannot rebind");
            require(!identity.reserve(p::ProducerKind::motion,owner,clock).accepted,"original owner return cannot unseal replacement");
        }
    }
    std::cout<<"SkeletonResponse and bounded first-mount producer policy tests passed.\n";
}
