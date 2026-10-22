#include "bmx_bail.h"
#include "bmx_intent_observer.h"
#include "bmx_intent_pulse.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/offboard_flight.h"
#include "Engine/Core/Json/json.h"
#include <atomic>
#include <algorithm>
#include <fstream>
#include <mutex>

namespace dingosdk::bmx::bail {
namespace {
namespace contract=game::build::v20260929::offboard_flight;
constexpr std::uintptr_t request_rva=0x481eb50;
constexpr std::uintptr_t update_rva=0x4822b80;
// Exact complete native leaf, supported FBCE74D5... image. REA on source-exact
// analysis carrier: ev_123b66bc8996d266cc7041e5273e879a3a74ef611367a7100fddea865b1626e4.
// void(OffboardParent*): selects the native ragdoll variant from context flags;
// respects transition lock+cf0, and changes pending+50 only (never active+48).
constexpr std::array<unsigned char,64> request_code{
    0x48,0x8b,0x41,0x08,0x8b,0x90,0xc0,0x09,0x00,0x00,0x0f,0xba,0xe2,0x1c,0x73,0x13,
    0x0f,0xba,0xe2,0x13,0xb8,0xe0,0x08,0x00,0x00,0xba,0xa0,0x0a,0x00,0x00,0x0f,0x42,
    0xc2,0xeb,0x05,0xb8,0x20,0x07,0x00,0x00,0x48,0x8d,0x14,0x08,0x48,0x39,0x51,0x48,
    0x74,0x0d,0x80,0xb9,0xf0,0x0c,0x00,0x00,0x00,0x75,0x04,0x48,0x89,0x51,0x50,0xc3};
constexpr std::array<unsigned char,32> update_code{
    0x40,0x57,0x48,0x81,0xec,0xd0,0x00,0x00,0x00,0x48,0x8b,0x05,0x30,0x18,0x9a,0x02,
    0x48,0x33,0xc4,0x48,0x89,0x84,0x24,0xc0,0x00,0x00,0x00,0x48,0x8b,0xf9,0x48,0x8b};
struct ErrorGuard {DWORD value=GetLastError();~ErrorGuard(){SetLastError(value);}};
struct Identity {std::uintptr_t client{};NoBailSkater skater;};
struct Shared {
    std::mutex mutex;
    std::atomic<bool> work{};
    std::uintptr_t base{};
    bool listener{};
    bool arming{};
    Identity owner;
    std::uint64_t expires{},observe_until{};
    bool intent_reset_seen{};
    Status state;
};
Shared& shared(){static auto* value=new Shared;return *value;}
struct ObservationSample {
    std::uint64_t elapsed_ms{};
    bool coherent{},wipeout{},offboard{},ragdoll_available{},ragdoll{};
    std::uint32_t physics{},flags{},impact_requests{},animation_requests{};
    std::uintptr_t parent_vtable_rva{},active_offset{},active_vtable_rva{},pending_offset{};
    std::uint8_t transition_locked{};
};
struct Observation {
    std::mutex mutex;
    std::atomic<bool> active{};
    bool requested{},writing{};
    Identity owner;
    std::filesystem::path root;
    std::array<ObservationSample,512> samples{};
    ObservationStatus state;
};
Observation& observation(){static auto* value=new Observation;return *value;}
bool pointer(std::uintptr_t value) noexcept {return value>=0x10000&&value<=memory::highest_user_address-0x10000;}
template<class T> bool read(std::uintptr_t object,std::uintptr_t offset,T& value) noexcept {
    return pointer(object)&&offset<=0x10000&&object<=memory::highest_user_address-offset-sizeof(T)&&memory::peek(object+offset,value);
}
std::uintptr_t link(std::uintptr_t object,std::uintptr_t offset=0) noexcept {
    std::uintptr_t result{};return read(object,offset,result)&&pointer(result)?result:0;
}
bool same(const NoBailSkater& a,const NoBailSkater& b) noexcept {
    return a.base==b.base&&a.entity==b.entity&&a.core==b.core&&a.context==b.context&&a.rig==b.rig;
}
bool compatible(std::uintptr_t base,const char*& issue) noexcept {
    if(!pointer(base)||base>memory::highest_user_address-0x09144000){issue="Native bail image bounds rejected.";return false;}
    std::array<unsigned char,64> request{};std::array<unsigned char,32> update{};
    if(!memory::peek(base+request_rva,request)){issue="Native ragdoll request bytes unreadable.";return false;}
    if(request!=request_code){issue="Native ragdoll request bytes differ.";return false;}
    if(!memory::peek(base+update_rva,update)){issue="Native Offboard update bytes unreadable.";return false;}
    if(update!=update_code){issue="Native Offboard update bytes differ.";return false;}
    if(link(base+contract::offboard_flight_vtable,0x10)!=base+update_rva){issue="Native Offboard update vtable differs.";return false;}
    if(!offboard_flight_compatible(base)){issue="Native Offboard owner/flight family contract differs.";return false;}
    return true;
}
bool ragdoll(std::uintptr_t base,std::uintptr_t parent,std::uintptr_t state) noexcept {
    for(const auto& entry:contract::offboard_ragdoll_states)
        if(state==parent+entry[0]&&link(state)==base+entry[1])return true;
    return false;
}
bool flight(std::uintptr_t base,std::uintptr_t parent,std::uintptr_t state) noexcept {
    for(const auto& entry:contract::offboard_flight_states)
        if(state==parent+entry.offset&&link(state)==base+entry.vtable_rva)return true;
    return false;
}
struct Parent {std::uintptr_t object{},active{},pending{};std::uint8_t locked{};std::uint32_t flags{};};
bool parent_state(const NoBailSkater& owner,Parent& out) noexcept {
    out.object=link(owner.core,0x3b0);
    std::uint32_t physics{};
    return link(out.object)==owner.base+contract::offboard_flight_vtable&&
        link(out.object,8)==owner.context&&link(out.object,0x18)==owner.rig&&
        link(out.object,0x10)==link(owner.core,0x430)&&
        read(owner.context,game::build::v20260929::skater_state::physics_state_offset,physics)&&physics==504&&
        read(out.object,0x48,out.active)&&read(out.object,0x50,out.pending)&&
        read(out.object,0xcf0,out.locked)&&out.locked<=1&&read(owner.context,0x9c0,out.flags);
}
bool invoke(std::uintptr_t target,std::uintptr_t parent) noexcept {
    __try {reinterpret_cast<void(*)(std::uintptr_t)>(target)(parent);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool observation_opt_in(std::filesystem::path& root) {
    wchar_t value[2]{};
    if(GetEnvironmentVariableW(L"RESKATE_BMX_NATIVE_FALL_OBSERVER",value,2)!=1||value[0]!=L'1'||
       !user_data::development_requested())return false;
    const auto requested=user_data::override_root();
    if(!requested||_wcsicmp(requested->filename().c_str(),L"bmx-user-data-chassis-probe")!=0)return false;
    root=*requested;return true;
}
void stop_observation(Observation& o,const char* issue,bool changed=false) noexcept {
    o.active.store(false,std::memory_order_release);o.state.active=false;o.state.completed=true;
    o.state.completed_ms=GetTickCount64();o.state.owner_changed=changed;o.state.issue=issue;
}
void observe(const NoBailSkater& callback,bool wipeout) noexcept {
    auto& o=observation();if(!o.active.load(std::memory_order_acquire))return;
    try {
        std::unique_lock lock(o.mutex,std::try_to_lock);if(!lock.owns_lock())return;
        if(!o.state.active)return;
        const auto now=GetTickCount64();
        if(now-o.state.started_ms>=6000){stop_observation(o,"Observation window completed.");return;}
        if(!same(callback,o.owner.skater)){stop_observation(o,"Observed native owner changed.",true);return;}
        const auto& mp=multiplayer::model();
        if(mp.active||mp.lobby_joining){stop_observation(o,"Solo observation eligibility changed.");return;}
        NoBailSkater current;bool held{};
        if(!inspect_no_bail_owner(o.owner.client,callback.entity,current,held)||!same(current,callback)) {
            stop_observation(o,"Observed native owner unavailable.",true);return;
        }
        if(held){stop_observation(o,"Independent protection became active.");return;}
        if(o.state.samples>=o.samples.size()){stop_observation(o,"Bounded sample capacity reached.");return;}
        ObservationSample sample;sample.elapsed_ms=now-o.state.started_ms;sample.wipeout=wipeout;
        const auto parent=link(current.core,0x3b0),parent_vtable=link(parent);
        sample.parent_vtable_rva=parent_vtable>=current.base&&parent_vtable-current.base<0x09144000?
            parent_vtable-current.base:0;
        sample.coherent=read(current.context,0x1414,sample.physics)&&read(current.context,0x9c0,sample.flags)&&
            read(current.context,0x13c4,sample.impact_requests)&&read(current.context,0x13d4,sample.animation_requests);
        sample.offboard=parent_vtable==current.base+contract::offboard_flight_vtable;
        std::uintptr_t active{},pending{};
        if(sample.offboard) {
            sample.coherent=sample.coherent&&read(parent,0x48,active)&&read(parent,0x50,pending)&&
                read(parent,0xcf0,sample.transition_locked)&&sample.transition_locked<=1;
            if(active>=parent&&active-parent<=0xd50) {
                sample.active_offset=active-parent;const auto vt=link(active);
                sample.active_vtable_rva=vt>=current.base&&vt-current.base<0x09144000?vt-current.base:0;
            }
            if(pending>=parent&&pending-parent<=0xd50)sample.pending_offset=pending-parent;
        }
        const auto trick=link(current.core,0x3b8),offboard=link(trick,0x78);
        std::uint8_t rag{};
        sample.ragdoll_available=read(offboard,0x18c,rag)&&rag<=1;sample.ragdoll=sample.ragdoll_available&&rag!=0;
        NoBailSkater final_owner;bool final_held{};std::uint32_t physics{},flags{},impact{},animation{};
        sample.coherent=sample.coherent&&inspect_no_bail_owner(o.owner.client,callback.entity,final_owner,final_held)&&
            same(current,final_owner)&&!final_held&&read(current.context,0x1414,physics)&&physics==sample.physics&&
            read(current.context,0x9c0,flags)&&flags==sample.flags&&
            read(current.context,0x13c4,impact)&&impact==sample.impact_requests&&
            read(current.context,0x13d4,animation)&&animation==sample.animation_requests&&
            link(current.core,0x3b0)==parent&&link(parent)==parent_vtable;
        if(sample.offboard) {
            std::uint8_t locked{};
            sample.coherent=sample.coherent&&link(parent,0x48)==active&&link(parent,0x50)==pending&&
                read(parent,0xcf0,locked)&&locked==sample.transition_locked;
        }
        if(sample.ragdoll_available) {
            std::uint8_t final_rag{};
            sample.coherent=sample.coherent&&link(current.core,0x3b8)==trick&&link(trick,0x78)==offboard&&
                read(offboard,0x18c,final_rag)&&final_rag==rag;
        }
        o.samples[static_cast<std::size_t>(o.state.samples++)]=sample;
        if(sample.coherent) {
            o.state.native_ragdoll_mode_seen=o.state.native_ragdoll_mode_seen||(sample.flags&0x2000)==0;
            o.state.ragdoll_seen=o.state.ragdoll_seen||sample.ragdoll;
        }
    }catch(...){try{std::unique_lock lock(o.mutex,std::try_to_lock);if(lock.owns_lock())stop_observation(o,"Passive native observation failed.");}catch(...){}}
}
void stop_locked(Shared& s,const char* issue,bool expired=false,bool changed=false) noexcept {
    s.state.pending=false;s.state.expired=expired;s.state.owner_changed=changed;s.state.issue=issue;
    s.expires=s.observe_until=0;s.work.store(false,std::memory_order_release);
}
void merge_intent(Status& state,const intent_observer::BailProbeStatus& probe) noexcept {
    // A native callback may currently own the nonblocking lifecycle lock. An
    // unavailable snapshot must not erase a previously observed reset duty.
    state.intent_status_available=probe.available;
    (void)intent_observer::detail::apply_available(probe.available,[&]() noexcept {
    state.intent_generation=probe.generation;
    state.context_verified=probe.context_verified;state.intent_callbacks=probe.callbacks;
    state.intent_native_calls=probe.true_calls+probe.reset_calls;
    state.intent_written=probe.intent_written;state.intent_reset=probe.intent_reset;
    state.activation_enabled=probe.activation_enabled;state.intent_reset_pending=probe.reset_pending;
    state.intent_prior_false=probe.prior_false;state.intent_true_returned=probe.true_returned;
    state.intent_true_readback=probe.true_readback;state.intent_reset_readback=probe.reset_readback;
    state.intent_native_cleared=probe.native_cleared;state.intent_owner_expired=probe.owner_expired;
    state.intent_true_serial=probe.true_serial;state.intent_reset_serial=probe.reset_serial;
    state.intent_true_calls=probe.true_calls;state.intent_reset_calls=probe.reset_calls;
    state.consumed=state.consumed||probe.intent_written;
    });
}
void physics_step(const NoBailSkater& callback,float,bool wipeout) noexcept {
    auto& s=shared();
    if(!s.work.load(std::memory_order_acquire)&&!observation().active.load(std::memory_order_acquire))return;
    ErrorGuard error;
    observe(callback,wipeout);
    if(!s.work.load(std::memory_order_acquire))return;
    try {
        Identity owner;std::uint64_t id{},expires{},observe_until{};bool pending{};
        {
            std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock()||s.arming)return;
            owner=s.owner;id=s.state.request_id;expires=s.expires;observe_until=s.observe_until;pending=s.state.pending;
        }
        // The existing dispatcher publishes only its freshly canonical local
        // owner. A replacement cannot consume the old owner's native request.
        if(!same(callback,owner.skater)) {
            std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock())return;
            if(id==s.state.request_id)stop_locked(s,"Native bail owner changed before handoff.",false,true);
            return;
        }
        const auto now=GetTickCount64();
        NoBailSkater current;bool protected_now{};
        const bool coherent=inspect_no_bail_owner(owner.client,owner.skater.entity,current,protected_now)&&same(current,owner.skater);
        Parent parent;
        const bool valid=coherent&&parent_state(current,parent);
        std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock()||s.arming)return;
        if(id!=s.state.request_id||!s.work.load(std::memory_order_acquire))return;
        if(s.state.intent_armed){
            const auto probe=intent_observer::bail_probe_status();merge_intent(s.state,probe);
            if(!probe.available)return;
            if(!coherent){stop_locked(s,"Native intent fall owner changed.",false,true);return;}
            if(valid){
                s.state.native_offboard_flags=parent.flags;s.state.native_ragdoll_mode=(parent.flags&0x2000)==0;
                if(ragdoll(current.base,parent.object,parent.active)){
                    s.state.ragdoll_observed=true;if(!s.state.observed_ms)s.state.observed_ms=now;
                }
            }
            if(probe.intent_reset&&!s.intent_reset_seen){
                s.intent_reset_seen=true;s.observe_until=(std::max)(s.observe_until,now+2000);
            }
            s.state.pending=!probe.completed||probe.reset_pending;
            if(s.state.ragdoll_observed&&probe.intent_reset){
                stop_locked(s,"Native RequestWipeout pulse reset and typed ragdoll observed.");return;
            }
            if(now>=s.observe_until&&!probe.reset_pending){
                stop_locked(s,s.state.consumed?"Native intent reset verified; typed ragdoll was not observed.":
                    "Native intent pulse was not verified before the observation deadline.",true);return;
            }
            s.state.issue=probe.issue;
            return; // intent activation never bypasses the old bit13 leaf guard
        }
        if(!valid){stop_locked(s,"Native bail owner/state changed.",false,true);return;}
        if(ragdoll(current.base,parent.object,parent.active)) {
            s.state.ragdoll_observed=true;s.state.observed_ms=now;
            stop_locked(s,"Native offboard ragdoll observed.");return;
        }
        if(!pending) {
            if(now>=observe_until)stop_locked(s,"Native ragdoll observation expired.",true);
            return;
        }
        if(now>=expires){stop_locked(s,"Native bail request expired.",true);return;}
        const auto& mp=multiplayer::model();
        if(!user_data::development_requested()||!user_data::override_root()||mp.active||mp.lobby_joining) {
            stop_locked(s,"Native bail eligibility changed before handoff.");return;
        }
        s.state.native_offboard_flags=parent.flags;s.state.native_ragdoll_mode=(parent.flags&0x2000)==0;
        if(!s.state.native_ragdoll_mode) {
            stop_locked(s,"Native rig remains animated; activation contract unavailable.");return;
        }
        s.state.protected_owner=protected_now;
        if(protected_now){stop_locked(s,"Independent No Bail protection is active.");return;}
        if(!flight(current.base,parent.object,parent.active)||
           !(flight(current.base,parent.object,parent.pending)||ragdoll(current.base,parent.object,parent.pending))) {
            stop_locked(s,"Native animation owns the offboard transition.");return;
        }
        if(parent.locked)return; // native transition retains priority until the short request expires
        // The function is a complete call-free leaf. Recheck its exact bytes,
        // parent identity and full current ownership immediately before use.
        std::array<unsigned char,64> bytes{};NoBailSkater final_owner;bool final_protected{};Parent final_parent;
        if(!user_data::development_requested()||!user_data::override_root()||
           multiplayer::model().active||multiplayer::model().lobby_joining||
           !memory::peek(current.base+request_rva,bytes)||bytes!=request_code||
           !inspect_no_bail_owner(owner.client,owner.skater.entity,final_owner,final_protected)||
           !same(final_owner,current)||final_protected||!parent_state(final_owner,final_parent)||
           final_parent.object!=parent.object||final_parent.active!=parent.active||
           final_parent.pending!=parent.pending||final_parent.locked||(final_parent.flags&0x2000)!=0) {
            stop_locked(s,"Native bail handoff changed before use.",false,true);return;
        }
        // Consume before native entry: a failed/uncertain call is never retried.
        s.state.pending=false;s.state.consumed=true;s.state.consumed_ms=now;
        s.expires=0;s.observe_until=now+2000;++s.state.native_calls;
        lock.unlock(); // no SDK mutex across the native call
        const bool called=invoke(current.base+request_rva,parent.object);
        Parent after;
        const bool retained=called&&parent_state(current,after)&&after.object==parent.object&&
            after.active==parent.active&&ragdoll(current.base,after.object,after.pending);
        if(!lock.try_lock())return;
        if(id!=s.state.request_id||!s.work.load(std::memory_order_acquire))return;
        if(!called) {
            stop_locked(s,"Native ragdoll request faulted.");return;
        }
        if(!retained) {
            stop_locked(s,"Native ragdoll request was not retained.");return;
        }
        s.state.issue="Native ragdoll transition queued; awaiting native entry.";
    } catch (...) {
        try {std::unique_lock lock(s.mutex,std::try_to_lock);if(lock.owns_lock())stop_locked(s,"Native bail observation failed.");}catch(...){}
    }
}
}

bool initialize(std::uintptr_t base) noexcept {
    ErrorGuard error;
    try {
        auto& s=shared();std::lock_guard lock(s.mutex);
        if(s.state.ready)return s.base==base;
        if(!no_bail_available()){s.state.issue="Native No Bail observation hooks unavailable.";return false;}
        if(!compatible(base,s.state.issue))return false;
        if(!s.listener) {
            if(!add_no_bail_step_listener(&physics_step)){s.state.issue="Native bail listener unavailable.";return false;}
            s.listener=true;
        }
        s.base=base;s.state.ready=true;s.state.issue="Native bail handoff ready.";return true;
    } catch(...){return false;}
}
bool request(std::uintptr_t base,const NativeFrame& frame) noexcept {
    ErrorGuard error;
    try {
        if(!frame.available||!frame.offboard||!user_data::development_requested()||!user_data::override_root())return false;
        const auto& mp=multiplayer::model();if(mp.active||mp.lobby_joining)return false;
        if(!initialize(base))return false;
        NoBailSkater owner;bool protected_now{};Parent parent;
        if(!inspect_no_bail_owner(frame.client,frame.entity,owner,protected_now)||owner.base!=base||
           owner.core!=frame.core||owner.context!=frame.context||owner.rig!=frame.rig||!parent_state(owner,parent))return false;
        auto& s=shared();std::unique_lock lock(s.mutex);
        if(s.work.load(std::memory_order_acquire)||s.arming)return false;
        const auto previous=intent_observer::bail_probe_status();
        if(!previous.available){s.state.intent_status_available=false;return false;}
        if(intent_observer::detail::blocks_rearm(previous.armed,previous.completed,previous.reset_pending,
           previous.true_calls,previous.intent_reset)){
            // A cancelled monitor may have stopped while its native setter was
            // entering. Never replace that status with a rejected new request:
            // the observer still owns cleanup of the previous generation.
            s.state.intent_armed=true;merge_intent(s.state,previous);
            s.state.pending=previous.reset_pending||!previous.completed;s.state.issue=previous.issue;
            return false;
        }
        const auto requests=s.state.requests+1,calls=s.state.native_calls,id=s.state.request_id+1;
        s.state={};s.state.ready=true;s.state.requests=requests;s.state.native_calls=calls;s.state.request_id=id;
        s.state.requested_ms=GetTickCount64();s.state.entity=owner.entity;s.state.protected_owner=protected_now;
        s.state.native_offboard_flags=parent.flags;s.state.native_ragdoll_mode=(parent.flags&0x2000)==0;
        if(protected_now){s.state.issue="Release BMX/independent No Bail protection before bail.";return false;}
        if(!s.state.native_ragdoll_mode){
            // Never hold the bail mutex across hook installation or native
            // pulse lifecycle work. Callbacks use try-lock and keep forwarding.
            s.arming=true;lock.unlock();
            const bool armed=intent_observer::arm_bail_probe(base,frame);
            lock.lock();
            if(id!=s.state.request_id||!s.arming){lock.unlock();if(armed)intent_observer::cancel_bail_probe();return false;}
            s.arming=false;
            if(armed){
                s.state.intent_armed=true;s.state.pending=true;
                s.owner={frame.client,owner};s.expires=0;s.observe_until=s.state.requested_ms+6000;s.intent_reset_seen=false;
                s.state.issue="Awaiting fresh canonical Bool callback.";
                s.work.store(true,std::memory_order_release);return true;
            }
            s.state.issue="Native rig remains animated; activation contract unavailable.";return false;
        }
        s.owner={frame.client,owner};s.expires=s.state.requested_ms+250;s.observe_until=0;
        s.state.pending=true;s.state.issue="Native bail request pending matching physics handoff.";
        s.work.store(true,std::memory_order_release);return true;
    } catch(...){return false;}
}
void cancel() noexcept {
    ErrorGuard error;
    intent_observer::cancel_bail_probe();
    try {
        const auto probe=intent_observer::bail_probe_status();
        auto& s=shared();std::lock_guard lock(s.mutex);
        const bool cleanup=s.state.intent_armed&&(!probe.available||probe.reset_pending);
        if(s.state.intent_armed)merge_intent(s.state,probe);
        s.arming=false;stop_locked(s,"Native bail request cancelled.");
        // Cancellation revokes entry; an in-flight/uncertain true write still
        // owns its cleanup obligation and prevents a subsequent crash rearm.
        s.state.pending=cleanup;if(cleanup)s.state.intent_reset_pending=true;
    }catch(...){}
}
Status status() noexcept {
    ErrorGuard error;
    try {
        auto& s=shared();std::lock_guard lock(s.mutex);
        if(s.state.intent_armed){
            const auto probe=intent_observer::bail_probe_status();
            if(!probe.available){s.state.intent_status_available=false;return s.state;}
            merge_intent(s.state,probe);
            s.state.pending=probe.reset_pending||s.work.load(std::memory_order_acquire);
            s.state.owner_changed=s.state.owner_changed||probe.owner_changed;
            if(s.work.load(std::memory_order_acquire))s.state.issue=probe.issue;
        }
        if(!s.state.intent_armed&&s.work.load(std::memory_order_acquire)&&GetTickCount64()>=(s.state.pending?s.expires:s.observe_until))
            stop_locked(s,s.state.pending?"Native bail request expired.":"Native ragdoll observation expired.",true);
        return s.state;
    }catch(...){return {};}
}
bool begin_observation(std::uintptr_t base,const NativeFrame& frame) noexcept {
    ErrorGuard error;
    try {
        const auto fail=[](const char* issue) {
            auto& o=observation();std::lock_guard lock(o.mutex);
            if(!o.requested)o.state.issue=issue;
            return false;
        };
        std::filesystem::path root;
        if(!frame.available)return fail("Observation NativeFrame unavailable.");
        if(!observation_opt_in(root))return fail("Observation exact profile/environment eligibility rejected.");
        if(!initialize(base))return fail("Observation native contract initialization rejected; inspect bail status.");
        if(multiplayer::model().active||multiplayer::model().lobby_joining)return fail("Observation solo eligibility rejected.");
        if(shared().work.load(std::memory_order_acquire))return fail("Another native bail request is pending.");
        NoBailSkater owner;bool held{};std::uint32_t physics{};
        if(!inspect_no_bail_owner(frame.client,frame.entity,owner,held))return fail("Observation canonical owner unavailable.");
        if(held)return fail("Observation native owner remains protected.");
        if(owner.base!=base||owner.core!=frame.core||owner.context!=frame.context||owner.rig!=frame.rig)
            return fail("Observation canonical owner differs from NativeFrame.");
        if(!read(owner.context,0x1414,physics))return fail("Observation native physics state unreadable.");
        if(physics!=100)return fail("Observation native physics state is not board100.");
        auto& o=observation();
        {
            std::lock_guard lock(o.mutex);
            if(o.requested){o.state.issue="Passive observation already requested in this process.";return false;}
            o.requested=true;o.owner={frame.client,owner};o.root=root;
            o.state={};o.state.active=true;o.state.started_ms=GetTickCount64();
            o.state.issue="Passive native fall observation active.";
            o.active.store(true,std::memory_order_release);
        }
        // Capture a canonical board100 baseline BEFORE the caller queues the
        // already existing BoardBreak wipeout request. This performs reads only.
        observe(owner,false);
        return observation().active.load(std::memory_order_acquire);
    }catch(...){return false;}
}
ObservationStatus poll_observation() noexcept {
    ErrorGuard error;
    try {
        auto& o=observation();std::unique_lock lock(o.mutex);
        if(!o.requested)return o.state;
        if(o.state.active&&GetTickCount64()-o.state.started_ms>=6000)
            stop_observation(o,"Observation window completed.");
        if(!o.state.completed||o.state.written||o.writing)return o.state;
        o.writing=true;
        const auto snapshot=o.state;const auto owner=o.owner;const auto root=o.root;const auto samples=o.samples;
        lock.unlock();
        bool written{};
        try {
            std::filesystem::path current_root;
            if(!observation_opt_in(current_root)||current_root!=root)
                throw std::runtime_error("Native observation root changed.");
            Json rows=Json::array();
            for(std::size_t i=0;i<snapshot.samples;++i) {
                const auto& x=samples[i];
                rows.push_back(Json{{"elapsed_ms",x.elapsed_ms},{"coherent",x.coherent},{"wipeout_argument",x.wipeout},
                    {"physics_state",x.physics},{"offboard_flags",x.flags},
                    {"animated_bit13",(x.flags&0x2000)!=0},{"impact_requests",x.impact_requests},
                    {"animation_requests",x.animation_requests},{"parent_vtable_rva",x.parent_vtable_rva},
                    {"offboard_parent",x.offboard},{"active_offset",x.active_offset},
                    {"active_vtable_rva",x.active_vtable_rva},{"pending_offset",x.pending_offset},
                    {"transition_locked",x.transition_locked},{"ragdoll_available",x.ragdoll_available},
                    {"ragdoll",x.ragdoll}});
            }
            Json report{{"process_id",GetCurrentProcessId()},{"completed",true},{"passive_only",true},
                {"source_image_sha256","fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9"},
                {"sample_limit",512},{"window_ms",6000},{"started_ms",snapshot.started_ms},
                {"completed_ms",snapshot.completed_ms},{"issue",snapshot.issue},
                {"owner_changed",snapshot.owner_changed},{"native_ragdoll_mode_seen",snapshot.native_ragdoll_mode_seen},
                {"ragdoll_seen",snapshot.ragdoll_seen},{"entity",owner.skater.entity},
                {"sample_count",snapshot.samples},{"samples",std::move(rows)},
                {"limits","Read-only pre-skeleton-response observations; no activation, rig-mode, input or pose writes. Bit13 is an animation-derived context input, not alone proof of a physical ragdoll."}};
            std::ofstream file(root/L"bmx-native-fall-observation.json",std::ios::binary|std::ios::trunc);
            if(!file)throw std::runtime_error("Cannot open native fall observation report.");
            file<<report.dump(2)<<'\n';file.close();
            if(!file)throw std::runtime_error("Cannot finish native fall observation report.");
            written=true;
        }catch(...){written=false;}
        lock.lock();o.state.written=written;
        if(!written)o.state.issue="Native fall observation report could not be written.";
        return o.state;
    }catch(...){return {};}
}
}
