// Included once, after bmx_native_chassis.cpp closes its namespace. This shares
// that translation unit's VERIFIED private native helpers and existing hook.
// No new native address, guessed field, direct store or second hook lives here.
namespace dingosdk::bmx::native_coupled {
namespace {
using namespace native_chassis;
struct Role {Handle body;Configuration configuration;bool observed{};float allocated_angular_limit{};};
struct PairPool {
    Api api;SetFloat angular_limit{};NativeFrame owner;WorldKey world;DWORD thread{};
    std::array<Role,2> roles;
    std::array<Handle,8> shapes{};std::array<bool,8> shape_observed{};
    bool attempted{},prepared{},active{},parked{},detached{},failed{},blocked{},calibrating{},calibration_used{};
    unsigned attempts{};std::uint64_t token{};Frame latest;
    calibration::Monitor calibration;std::array<mount_pose::Seed,2> calibration_support;
    ULONGLONG calibration_until{};
    unsigned admission_rejections{},rejected_role=0xffffffffU;const char* admission_issue="not_attempted";
    const char* issue="not_prepared";
};
struct PairLease {std::array<Lease,2> roles;policy::Epoch epoch;Command command;ULONGLONG until{};bool active{},force_probe{},controller_trial{};};
struct PairShared {
    SRWLOCK lock=SRWLOCK_INIT;PairLease lease;policy::Once once;Status info;
    std::array<Configuration,2> native_configuration;std::uint64_t configuration_token{};
    std::atomic<bool> claimed{};
    std::atomic<bool> force_probe_armed{}; // exact private finite protocol only
    std::atomic<bool> controller_trial_armed{}; // two flags +finite backend deadline
};
PairPool& paired_pool(){static auto* p=new PairPool;return *p;}
PairShared& paired_shared(){static auto* p=new PairShared;return *p;}
// REA static 25DAA30..52: Handle*RCX +floatXMM1; state998[index].
// This binds only the verified owned keyframed property write/readback. Native
// integration may add angular velocity after its clamp: no absolute15rad/s claim.
constexpr game::build::Fingerprint set_angular_limit{0x25daa30,{
    0x48,0x8b,0x01,0x8b,0x51,0x08,0xc5,0xfa,0x10,0x05,0x0a,0x59,0xd6,0x03,0xc5,0xfa,
    0x5d,0xd1,0x48,0x8b,0x48,0x28,0x48,0x8b,0x81,0x98,0x09,0x00,0x00,0xc5,0xfa,0x11}};
bool resolve_pair(std::uintptr_t base,Api& api,SetFloat& angular_limit) noexcept {
    if(!resolve(base,api)||!matches(base,set_angular_limit))return false;
    angular_limit=reinterpret_cast<SetFloat>(base+set_angular_limit.rva);return true;
}
// A verified native mask setter/readback is not proof of the engine's pair
// filtering behavior. Only a separately reviewed evidence-backed change opens
// this gate. Neutral calibration is a separate future API, never this switch.
bool collision_adoption_ready() noexcept{return false;}
bool requested() noexcept {
    wchar_t value[2]{};
    constexpr wchar_t root[]=L"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-coupled-test";
    return GetEnvironmentVariableW(L"RESKATE_BMX_NATIVE_COUPLED",value,2)==1&&value[0]==L'1'&&
        user_data::runtime_development_selected(L"bmx-user-data-coupled-test",root)&&allowed();
}
void publish(PairPool& p) noexcept {
    auto& s=paired_shared();AcquireSRWLockExclusive(&s.lock);
    s.info.attempted=p.attempted;s.info.prepared=p.prepared;s.info.active=p.active;
    s.info.parked=p.parked;s.info.detached=p.detached;s.info.failed=p.failed||s.info.failed;s.info.blocked=p.blocked;
    s.info.force_adoption_enabled=collision_adoption_ready();s.info.token=p.token;s.info.allocation_attempts=p.attempts;
    s.info.calibration_admission_rejections=p.admission_rejections;s.info.calibration_rejected_role=p.rejected_role;s.info.calibration_admission_issue=p.admission_issue;
    s.info.live_bodies=0;s.info.live_shapes=0;
    for(const auto& r:p.roles)if(r.observed)++s.info.live_bodies;
    for(bool observed:p.shape_observed)if(observed)++s.info.live_shapes;
    s.info.issue=p.issue;ReleaseSRWLockExclusive(&s.lock);
}
void revoke() noexcept {auto& s=paired_shared();s.force_probe_armed.store(false,std::memory_order_release);s.controller_trial_armed.store(false,std::memory_order_release);AcquireSRWLockExclusive(&s.lock);s.lease.active=false;ReleaseSRWLockExclusive(&s.lock);}
bool admission_reject(PairPool& p,const char* reason,unsigned role=0xffffffffU) noexcept {
    if(p.admission_rejections<65535)++p.admission_rejections;p.admission_issue=reason;p.rejected_role=role;publish(p);return false;
}
void fail(PairPool& p,const char* issue,bool forget_handles=false) noexcept {
    revoke();p.active=p.parked=p.detached=p.calibrating=false;p.failed=p.blocked=true;p.issue=issue;p.latest={};
    if(forget_handles){p.prepared=false;p.roles={};p.shapes={};p.shape_observed={};
        if(p.token!=std::numeric_limits<std::uint64_t>::max())++p.token;}
    publish(p);
}
Pool proof_pool(const PairPool& p,unsigned role) {
    Pool proof;proof.api=p.api;proof.owner=p.owner;proof.world=p.world.world;proof.native_state=p.world.state;
    proof.client_context=p.world.context;proof.client_thread=p.thread;proof.body=p.roles[role].body;
    proof.body_observed=p.roles[role].observed;proof.configuration=p.roles[role].configuration;
    proof.shapes=p.shapes;proof.shape_observed=p.shape_observed;proof.ready=p.prepared;proof.token=p.token;
    return proof;
}
bool current(PairPool& p,const NativeFrame& f) noexcept {
    return GetCurrentThreadId()==p.thread&&same_owner(p.owner,f)&&[&]{WorldKey key;return raw_world(p.api,f,key)&&key==p.world;}();
}
bool own_role(PairPool& p,const NativeFrame& f,unsigned role,bool dynamic=false) noexcept {
    if(role>=2||!p.roles[role].observed||!current(p,f))return false;
    const auto& body=p.roles[role].body;
    return body.world==p.world.world&&allocated_body(p.world.state,body.index,dynamic)&&valid(p.api.body_valid,&body)&&current(p,f);
}
bool shape_identity(PairPool& p,const NativeFrame& f,unsigned index) noexcept {
    const unsigned role=index<5?0:1;
    if(index>=8||!p.shape_observed[index]||!own_role(p,f,role))return false;
    return p.shapes[index].world==p.world.world&&valid(p.api.shape_valid,&p.shapes[index])&&own_role(p,f,role);
}
bool own_shape(PairPool& p,const NativeFrame& f,unsigned index) noexcept {
    if(!shape_identity(p,f,index))return false;
    const unsigned role=index<5?0:1;
    const auto& shape=p.shapes[index];std::uintptr_t owners{},categories{},with{};std::uint32_t owner{},category{},collides{};
    const auto offset=std::uintptr_t{shape.index}*4;
    const bool positive=p.calibrating&&p.calibration.report().kind==calibration::Kind::pair_positive;
    const bool active=p.active||p.calibrating||(p.detached&&index<5);
    const bool selected=!positive||index==3||index==5;
    const std::uint32_t expected_category=positive&&selected?1U:0U,expected_mask=active&&selected?0x101U:0U;
    return shape.world==p.world.world&&valid(p.api.shape_valid,&shape)&&
        read(p.world.state+0x1008,owners)&&pointer(owners)&&read(owners+offset,owner)&&owner==0xffffffffU&&
        read(p.world.state+0xfe8,categories)&&pointer(categories)&&read(categories+offset,category)&&category==expected_category&&
        read(p.world.state+0xff0,with)&&pointer(with)&&read(with+offset,collides)&&
        collides==expected_mask&&
        own_role(p,f,role);
}
bool own_all(PairPool& p,const NativeFrame& f,bool dynamic=false) noexcept {
    if(!own_role(p,f,0,dynamic)||!own_role(p,f,1,dynamic)||p.roles[0].body.index==p.roles[1].body.index)return false;
    for(unsigned i=0;i<8;++i)if(!own_shape(p,f,i))return false;
    return current(p,f);
}
bool fresh(PairPool& p,std::uintptr_t base,const NativeFrame& hint,NativeFrame& out,bool calibrated=true) {
    auto proof=proof_pool(p,0);
    return same_owner(p.owner,hint)&&fresh_owner(proof,base,hint,out,calibrated);
}
bool pending_zero(PairPool& p,unsigned role) noexcept {
    V4 force{},torque{};std::uintptr_t a{},b{};
    return array4(p.world.state,0x970,p.roles[role].body.index,force,a)&&force==V4{}&&
        array4(p.world.state,0x978,p.roles[role].body.index,torque,b)&&torque==V4{};
}
bool angular_limit_value(PairPool& p,const NativeFrame& frame,unsigned role,float& value) noexcept {
    std::uintptr_t source{},again{};float repeated{};const auto offset=std::uintptr_t{p.roles[role].body.index}*4;
    return own_role(p,frame,role)&&read(p.world.state+0x998,source)&&pointer(source)&&read(source+offset,value)&&
        std::isfinite(value)&&value>0&&read(p.world.state+0x998,again)&&again==source&&read(again+offset,repeated)&&repeated==value&&own_role(p,frame,role);
}
bool angular_limit_readback(PairPool& p,const NativeFrame& frame,unsigned role,float& value) noexcept {
    return angular_limit_value(p,frame,role,value)&&value==15;
}
bool not_dynamic(PairPool& p,const NativeFrame& frame,unsigned role) noexcept {
    if(!own_role(p,frame,role))return false;
    const auto index=p.roles[role].body.index,bit=std::uint32_t{1}<<(index&31U);const auto offset=std::uintptr_t{index>>5}*4;
    std::uintptr_t source{},again{};std::uint32_t value{},repeated{};
    return read(p.world.state+0xa08,source)&&pointer(source)&&read(source+offset,value)&&!(value&bit)&&
        read(p.world.state+0xa08,again)&&again==source&&read(again+offset,repeated)&&!(repeated&bit)&&own_role(p,frame,role);
}
bool angular_integration_bit(std::uintptr_t state,const Handle& body,bool& set) noexcept {
    std::uintptr_t source{},again{};std::uint32_t value{},repeated{};const auto offset=std::uintptr_t{body.index>>5}*4;
    const auto bit=std::uint32_t{1}<<(body.index&31U);
    if(!read(state+0xa68,source)||!pointer(source)||!read(source+offset,value)||!read(state+0xa68,again)||again!=source||
        !read(again+offset,repeated)||(value&bit)!=(repeated&bit))return false;
    set=(value&bit)!=0;return true;
}
bool quiesce_role(PairPool& p,const NativeFrame& f,unsigned role) noexcept {
    if(!p.roles[role].observed)return true;
    for(unsigned i=role?5u:0u;i<(role?8u:5u);++i)if(p.shape_observed[i]){
        // A confirmed constructor result remains ours even if its first mask
        // configuration was partial. Cleanup does not require it to ALREADY
        // have the final masks it is about to disable.
        if(!shape_identity(p,f,i)||!mask(p.api.categories,&p.shapes[i],0)||!shape_identity(p,f,i)||
           !mask(p.api.collides,&p.shapes[i],0)||!shape_identity(p,f,i))return false;
        std::uintptr_t categories{},with{};std::uint32_t category{},collides{};const auto offset=std::uintptr_t{p.shapes[i].index}*4;
        if(!read(p.world.state+0xfe8,categories)||!pointer(categories)||!read(categories+offset,category)||category!=0||
           !read(p.world.state+0xff0,with)||!pointer(with)||!read(with+offset,collides)||collides!=0)return false;
    }
    alignas(16) const V4 zero{};auto& body=p.roles[role].body;
    if(!own_role(p,f,role))return false;
    if(allocated_body(p.world.state,body.index,true)&&
       (!vector(p.api.overwrite_force,&body,zero.data())||!own_role(p,f,role)||
        !vector(p.api.overwrite_torque,&body,zero.data())||!own_role(p,f,role)))return false;
    return pending_zero(p,role)&&vector(p.api.linear,&body,zero.data())&&own_role(p,f,role)&&
        vector(p.api.angular,&body,zero.data())&&own_role(p,f,role)&&
        integer(p.api.motion,&body,2)&&own_role(p,f,role)&&pending_zero(p,role);
}
bool same_snapshot(const Snapshot& a,const Snapshot& b) noexcept {
    return a.rings==b.rings&&a.direct_arrays==b.direct_arrays&&a.position==b.position&&a.quaternion_xyzw==b.quaternion_xyzw&&
        a.linear==b.linear&&a.angular==b.angular&&a.local_com==b.local_com&&a.local_quaternion_wxyz==b.local_quaternion_wxyz&&
        a.inertia_ratio==b.inertia_ratio&&a.inverse_mass==b.inverse_mass&&a.native_state==b.native_state;
}
bool role_snapshot(PairPool& p,const NativeFrame& f,unsigned role,Snapshot& result,bool configured=true) {
    auto proof=proof_pool(p,role);
    return snapshot_guarded(proof,result,configured?&p.roles[role].configuration:nullptr,nullptr,0,
        [&]{return own_role(p,f,role);})==SnapshotIssue::none;
}
bool consume_native_configuration(PairPool& p) noexcept {
    auto& s=paired_shared();AcquireSRWLockShared(&s.lock);
    const bool same=p.token&&s.configuration_token==p.token;
    if(same)for(unsigned role=0;role<2;++role)p.roles[role].configuration=s.native_configuration[role];
    ReleaseSRWLockShared(&s.lock);return same;
}
bool create_role(PairPool& p,const NativeFrame& f,unsigned role) {
    auto& r=p.roles[role];const auto& config=policy::v2_roles[role];
    r.configuration.mass=config.mass;r.configuration.inverse_mass=1.f/config.mass;
    r.configuration.inertia_ratio={config.mass/config.principal_moments[0],config.mass/config.principal_moments[1],config.mass/config.principal_moments[2],0};
    if(!allocate(&p.api,p.world.world,&r.body))return false;r.observed=true;
    if(role&&p.roles[0].body.index==r.body.index)return false;
    Snapshot defaults;
    if(!role_snapshot(p,f,role,defaults,false)||defaults.inverse_mass!=1||defaults.inertia_ratio!=V4{1,1,1,0})return false;
    alignas(16) const V4 zero{},ratio=r.configuration.inertia_ratio;
    if(!integer(p.api.motion,&r.body,2)||!own_role(p,f,role)||
       !scalar(p.api.inverse_mass,&r.body,r.configuration.inverse_mass)||!own_role(p,f,role)||
       !vector(p.api.inertia_ratio,&r.body,ratio.data())||!own_role(p,f,role)||
       !configured_body(p.world.state,r.body.index,r.configuration)||
       !touch(p.api.clear_aa0,&r.body)||!own_role(p,f,role)||!vector(p.api.com,&r.body,zero.data())||!own_role(p,f,role))return false;
    float angular_limit{};
    if(!not_dynamic(p,f,role)||!angular_limit_value(p,f,role,r.allocated_angular_limit)||
        !scalar(p.angular_limit,&r.body,15)||!not_dynamic(p,f,role)||!angular_limit_readback(p,f,role,angular_limit))return false;
    Quat principal,relative;if(!policy::unit(config.principal_to_model,principal)||!policy::unit(policy::rider_from_chassis,relative))return false;
    for(unsigned i=role?5u:0u;i<(role?8u:5u);++i){
        const auto& authored=collision_envelope::capsules[i];Vec endpoints[2]{authored.a,authored.b};
        alignas(16) V4 bottom{},top{};V4* target[2]{&bottom,&top};
        for(unsigned endpoint=0;endpoint<2;++endpoint){
            for(unsigned axis=0;axis<3;++axis)endpoints[endpoint][axis]-=config.com_from_bike_root[axis];
            if(role)endpoints[endpoint]=policy::rotate(policy::conjugate(relative),endpoints[endpoint]);
            endpoints[endpoint]=policy::native_local_point(endpoints[endpoint],principal);
            for(unsigned axis=0;axis<3;++axis)(*target[endpoint])[axis]=endpoints[endpoint][axis];
        }
        if(!own_role(p,f,role)||!capsule(&p.api,p.world.world,&r.body,&p.shapes[i],bottom.data(),top.data(),authored.radius)||
           !valid(p.api.shape_valid,&p.shapes[i])||p.shapes[i].world!=p.world.world)return false;
        for(unsigned j=0;j<i;++j)if(p.shape_observed[j]&&p.shapes[j]==p.shapes[i])return false;
        p.shape_observed[i]=true;
        if(!shape_identity(p,f,i)||!clear_owner(p.api.owner,&p.shapes[i])||!shape_identity(p,f,i)||
           !mask(p.api.categories,&p.shapes[i],0)||!shape_identity(p,f,i)||
           !mask(p.api.collides,&p.shapes[i],0)||!own_shape(p,f,i))return false;
    }
    return pending_zero(p,role);
}
bool seed_roles(PairPool& p,const NativeFrame& f,const mount_pose::Seed& seed) noexcept {
    Quat chassis_model;if(!policy::unit(seed.orientation,chassis_model))return false;
    const auto current_seed=[&]{std::uint64_t serial{};double time{};return read_native_clock(f.entity,f.core,serial,time)&&
        serial==seed.request.serial&&time==seed.request.simulation_time;};
    for(unsigned role=0;role<2;++role){
        const auto& config=policy::v2_roles[role];Quat principal,relative;
        if(!policy::unit(config.principal_to_model,principal)||!policy::unit(policy::rider_from_chassis,relative))return false;
        const auto model=role?policy::multiply(chassis_model,relative):chassis_model;
        const auto native=policy::native_orientation(model,principal);
        const auto offset=policy::rotate(chassis_model,config.com_from_bike_root);
        alignas(16) const std::array<float,8> transform{native[0],native[1],native[2],native[3],
            seed.root[0]+offset[0],seed.root[1]+offset[1],seed.root[2]+offset[2],1};
        if(!current_seed()||!own_all(p,f)||!vector(p.api.transform,&p.roles[role].body,transform.data())||!own_all(p,f)||!current_seed())return false;
    }
    return true;
}
bool calibration_requested(calibration::Kind kind) noexcept {
    wchar_t value[16]{};const auto size=GetEnvironmentVariableW(L"RESKATE_BMX_COUPLED_CALIBRATION",value,16);
    return requested()&&((kind==calibration::Kind::pair_filter&&size==4&&std::wstring_view(value,size)==L"pair")||
        (kind==calibration::Kind::world_contact&&size==5&&std::wstring_view(value,size)==L"world")||
        (kind==calibration::Kind::world_contact_only&&size==7&&std::wstring_view(value,size)==L"contact")||
        (kind==calibration::Kind::pair_positive&&size==13&&std::wstring_view(value,size)==L"pair-positive"));
}
bool exact_mask(PairPool& p,const NativeFrame& f,unsigned index,std::uint32_t mask_value,std::uint32_t category_value=0) noexcept {
    if(!shape_identity(p,f,index))return false;
    std::uintptr_t categories{},with{};std::uint32_t category{},collides{};const auto offset=std::uintptr_t{p.shapes[index].index}*4;
    return read(p.world.state+0xfe8,categories)&&pointer(categories)&&read(categories+offset,category)&&category==category_value&&
        read(p.world.state+0xff0,with)&&pointer(with)&&read(with+offset,collides)&&collides==mask_value&&shape_identity(p,f,index);
}
bool admit_neutral_masks(PairPool& p,const NativeFrame& f) noexcept {
    const bool positive=p.calibrating&&p.calibration.report().kind==calibration::Kind::pair_positive;
    for(unsigned i=0;i<8;++i){const bool selected=!positive||i==3||i==5;const std::uint32_t categories=positive&&selected?1U:0U,with=selected?0x101U:0U;
        if(!shape_identity(p,f,i)||!mask(p.api.categories,&p.shapes[i],categories)||!shape_identity(p,f,i)||
            !mask(p.api.collides,&p.shapes[i],with)||!exact_mask(p,f,i,with,categories))return false;
    }
    return true;
}
calibration::Sample calibration_sample(PairPool& p,const Frame& frame) noexcept {
    calibration::Sample out;out.epoch=frame.epoch;out.categories=frame.categories;out.collides_with=frame.collides_with;
    out.integrates_angular_accumulator=frame.integrates_angular_accumulator;
    for(unsigned role=0;role<2;++role){
        const auto& body=frame.roles[role];out.position[role]=body.position;out.linear[role]=body.linear_velocity;out.angular[role]=body.angular_velocity;
        out.mass[role]=body.mass;out.inertia[role]=body.inertia;out.principal_orientation[role]=body.orientation;
        out.maximum_angular_speed[role]=frame.maximum_angular_speed[role];
        out.allocated_maximum_angular_speed[role]=frame.allocated_maximum_angular_speed[role];
        const auto& support=p.calibration_support[role];
        out.clearance[role]=calibration::proxy_plane_clearance(collision_envelope::capsules,role,body.position,body.orientation,support.root,support.up)
            .value_or(std::numeric_limits<float>::quiet_NaN());
    }
    return out;
}
}
bool enabled() noexcept {native_chassis::LastError error;try{return requested();}catch(...){return false;}}
Status status() noexcept {
    auto& s=paired_shared();AcquireSRWLockShared(&s.lock);const auto out=s.info;ReleaseSRWLockShared(&s.lock);return out;
}
void cancel() noexcept {
    auto& s=paired_shared();s.force_probe_armed.store(false,std::memory_order_release);s.controller_trial_armed.store(false,std::memory_order_release);AcquireSRWLockExclusive(&s.lock);
    s.lease.active=false;if(s.info.receipt.state==policy::ReceiptState::queued)s.info.receipt.state=policy::ReceiptState::cancelled;
    ReleaseSRWLockExclusive(&s.lock);
}
bool acquire(std::uintptr_t base,const NativeFrame& hint,const mount_pose::Seed& seed) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();
    try {
        if(!requested()||!hint.available||!hint.offboard||!seed.ready||!policy::finite(seed.root))return false;
        // This private profile is selected before ANY one-body mount. No
        // attempt is made to reparent or destroy the existing retained pool.
        if(native_chassis::pool().attempted){p.issue="one_body_pool_already_attempted_restart_private_profile";publish(p);return false;}
        const auto fresh_frame=read_native_frame(base,hint.client);
        if(!same_owner(hint,fresh_frame)||!fresh_frame.offboard||!fresh_frame.timing_available||!step_supported(fresh_frame.seconds))return false;
        Api validated_api;SetFloat validated_limit{};WorldKey validated_world;
        if(!resolve_pair(base,validated_api,validated_limit)||!raw_world(validated_api,fresh_frame,validated_world)||
           !mount_pose::matches(seed,validated_world.world,validated_world.context,fresh_frame.serial,fresh_frame.simulation_time))return false;
        if(p.failed||p.blocked||status().failed||(p.attempted&&!p.prepared)||p.active||p.detached||p.calibrating)return false;
        if(!p.attempted){
            std::uint64_t serial{};double time{};
            if(!read_native_clock(fresh_frame.entity,fresh_frame.core,serial,time)||serial!=seed.request.serial||time!=seed.request.simulation_time)return false;
            p.api=validated_api;p.angular_limit=validated_limit;p.owner=fresh_frame;p.world=validated_world;p.thread=GetCurrentThreadId();
            p.attempted=true;++p.attempts;paired_shared().claimed.store(true,std::memory_order_release);
            if(!create_role(p,fresh_frame,0)||!create_role(p,fresh_frame,1)){
                (void)quiesce_role(p,fresh_frame,0);(void)quiesce_role(p,fresh_frame,1);
                fail(p,"partial_pair_allocation_same_world_retry_prohibited");return false;
            }
            p.prepared=p.parked=true;
        }
        if(base!=p.api.base||!current(p,fresh_frame)||!own_all(p,fresh_frame))return false;
        if(!mount_pose::matches(seed,p.world.world,p.world.context,fresh_frame.serial,fresh_frame.simulation_time))return false;
        cancel();
        if(!quiesce_role(p,fresh_frame,0)||!quiesce_role(p,fresh_frame,1)){
            fail(p,"pair_quiesce_before_seed_failed");return false;
        }
        for(unsigned role=0;role<2;++role){auto& configuration=p.roles[role].configuration;const auto& moments=policy::v2_roles[role].principal_moments;
            alignas(16) const V4 ratio{configuration.mass/moments[0],configuration.mass/moments[1],configuration.mass/moments[2],0};
            if(!not_dynamic(p,fresh_frame,role)||!vector(p.api.inertia_ratio,&p.roles[role].body,ratio.data())||!not_dynamic(p,fresh_frame,role)){
                fail(p,"keyframed_source_inertia_reset_failed");return false;}
            configuration.inertia_ratio=ratio;
            if(!configured_body(p.world.state,p.roles[role].body.index,configuration)){fail(p,"keyframed_source_inertia_reset_readback_failed");return false;}
        }
        if(!seed_roles(p,fresh_frame,seed)){
            p.issue="pair_prepared_parked_seed_not_current";publish(p);return false;
        }
        if(p.token==std::numeric_limits<std::uint64_t>::max()){fail(p,"pair_token_exhausted");return false;}
        ++p.token;p.latest={};p.active=p.detached=false;p.parked=true;
        auto& s=paired_shared();AcquireSRWLockExclusive(&s.lock);s.once.reset(p.token);s.lease={};s.info.receipt={};
        for(unsigned role=0;role<2;++role)s.native_configuration[role]=p.roles[role].configuration;s.configuration_token=p.token;
        ReleaseSRWLockExclusive(&s.lock);
        p.issue="pair_prepared_keyframed_collision_adoption_unverified";publish(p);return true;
    }catch(...){fail(p,"pair_acquisition_failed_closed");return false;}
}
Frame observe(std::uintptr_t base,const NativeFrame& hint) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();Frame out;
    try {
        if(!requested()||!p.prepared||p.failed||p.blocked||status().failed){out.issue=p.issue;return out;}
        for(unsigned attempt=0;attempt<2;++attempt){
            if(!consume_native_configuration(p)){out.issue="native_configuration_lease_changed";return out;}
            NativeFrame before,after;
            if(!fresh(p,base,hint,before)||!own_all(p,before)){fail(p,"paired_owner_or_world_unavailable",true);out.issue=p.issue;return out;}
            std::array<Snapshot,2> first,second;
            std::array<float,2> limits{};
            bool captured=true;
            for(unsigned role=0;role<2;++role)captured=captured&&role_snapshot(p,before,role,first[role]);
            for(unsigned role=0;role<2;++role)captured=captured&&angular_limit_readback(p,before,role,limits[role]);
            for(unsigned role=0;role<2;++role)captured=captured&&angular_integration_bit(p.world.state,p.roles[role].body,out.integrates_angular_accumulator[role]);
            std::uintptr_t categories{},with{};
            captured=captured&&read(p.world.state+0xfe8,categories)&&pointer(categories)&&read(p.world.state+0xff0,with)&&pointer(with);
            for(unsigned i=0;i<8;++i)captured=captured&&shape_identity(p,before,i)&&
                read(categories+std::uintptr_t{p.shapes[i].index}*4,out.categories[i])&&read(with+std::uintptr_t{p.shapes[i].index}*4,out.collides_with[i]);
            for(unsigned role=0;role<2;++role)captured=captured&&role_snapshot(p,before,role,second[role]);
            if(!fresh(p,base,hint,after)||!same_owner(before,after)||before.serial!=after.serial||
               before.simulation_time!=after.simulation_time||before.seconds!=after.seconds||!own_all(p,after)){
                out.issue="paired_epoch_changed";continue;
            }
            if(!captured||!same_snapshot(first[0],second[0])||!same_snapshot(first[1],second[1])||
               first[0].rings!=first[1].rings||second[0].rings!=second[1].rings){out.issue="paired_snapshot_not_jointly_stable";continue;}
            out.epoch.owner_world={before.client,before.entity,before.core,before.context,before.rig,
                p.world.world,p.world.state,p.world.context};
            out.epoch.token=p.token;out.epoch.serial=before.serial;out.epoch.time=before.simulation_time;out.epoch.seconds=before.seconds;
            for(unsigned role=0;role<2;++role){
                const auto& body=first[role];const auto& handle=p.roles[role].body;auto& r=out.roles[role];
                out.epoch.bodies[role]={handle.world,handle.index,handle.generation};
                r.available=true;r.active=p.active;r.detached=p.detached&&role==0;
                for(unsigned axis=0;axis<3;++axis){r.position[axis]=body.position[axis];r.linear_velocity[axis]=body.linear[axis];
                    r.angular_velocity[axis]=body.angular[axis];r.inertia[axis]=1.f/(body.inverse_mass*body.inertia_ratio[axis]);}
                r.orientation=body.quaternion_xyzw;r.mass=1.f/body.inverse_mass;r.step_seconds=before.seconds;
                r.world=p.world.world;r.native_state=p.world.state;r.body_index=handle.index;r.body_generation=handle.generation;
                r.token=p.token;r.serial=before.serial;r.simulation_time=before.simulation_time;r.issue="actual_paired_native_role";
                out.maximum_angular_speed[role]=limits[role];
                out.allocated_maximum_angular_speed[role]=p.roles[role].allocated_angular_limit;
                Quat principal;if(!policy::unit(policy::v2_roles[role].principal_to_model,principal)){out={};out.issue="principal_frame_invalid";return out;}
                out.model_orientation[role]=policy::model_orientation(r.orientation,principal);
            }
            if(!policy::valid_epoch(out.epoch)){out={};out.issue="paired_epoch_invalid";return out;}
            out.available=true;out.active=p.active;out.parked=p.parked;out.detached=p.detached;
            out.issue="paired_actual_observation_no_atomic_world_snapshot_claim";p.latest=out;
            auto& s=paired_shared();AcquireSRWLockExclusive(&s.lock);++s.info.observations;ReleaseSRWLockExclusive(&s.lock);return out;
        }
        cancel();return out;
    }catch(...){out={};out.issue="paired_observation_failed_closed";cancel();return out;}
}
bool activate(std::uintptr_t base,const NativeFrame& hint) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();
    try {
        if(!collision_adoption_ready()){p.issue="pair_filter_and_world_collision_not_proved_force_adoption_disabled";publish(p);return false;}
        NativeFrame f;
        if(!requested()||!p.prepared||p.active||!p.parked||p.failed||p.blocked||!fresh(p,base,hint,f)||!own_all(p,f))return false;
        auto hook_proof=proof_pool(p,0);if(!install_hook(hook_proof))return false;
        if(!admit_neutral_masks(p,f))return false;
        for(unsigned role=0;role<2;++role)if(!pending_zero(p,role)||!own_role(p,f,role)||
            !integer(p.api.motion,&p.roles[role].body,3)||!own_role(p,f,role,true)||!touch(p.api.wake,&p.roles[role].body)||!own_role(p,f,role,true)){
                (void)quiesce_role(p,f,0);(void)quiesce_role(p,f,1);fail(p,"partial_pair_activation_terminal");return false;}
        p.active=true;p.parked=p.detached=false;p.latest={};p.issue="paired_native_roles_active";publish(p);return own_all(p,f,true);
    }catch(...){fail(p,"pair_activation_failed_closed");return false;}
}
bool submit(const Frame& frame,const Command& command) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();
    try {
        if(!collision_adoption_ready()||!requested()||!p.active||!p.prepared||p.failed||p.blocked||
           !frame.available||!frame.active||!p.latest.available||frame.epoch!=p.latest.epoch||
           frame.epoch.token!=p.token||!frame.integrates_angular_accumulator[0]||!frame.integrates_angular_accumulator[1]||
           !policy::prepare(command,frame.epoch.seconds).valid)return false;
        NativeFrame f;if(!fresh(p,p.api.base,p.owner,f)||f.serial!=frame.epoch.serial||f.simulation_time!=frame.epoch.time||!own_all(p,f,true))return false;
        auto proof=proof_pool(p,0);PairLease work;
        if(!make_lease(proof,f,work.roles[0]))return false;
        work.roles[1]=work.roles[0];work.roles[1].body=p.roles[1].body;work.roles[1].configuration=p.roles[1].configuration;
        for(auto& role:work.roles){role.token=p.token;role.serial=frame.epoch.serial;role.simulation_time=frame.epoch.time;role.step_seconds=frame.epoch.seconds;}
        work.epoch=frame.epoch;work.command=command;work.until=GetTickCount64()+100;work.active=true;
        auto& s=paired_shared();AcquireSRWLockExclusive(&s.lock);
        if(s.info.failed||s.lease.active||frame.epoch.serial<=s.info.receipt.serial){ReleaseSRWLockExclusive(&s.lock);return false;}
        s.lease=work;s.info.receipt={policy::ReceiptState::queued,p.token,frame.epoch.serial,command.hop_part,0,0};s.info.receipt.epoch=frame.epoch;
        ReleaseSRWLockExclusive(&s.lock);return true;
    }catch(...){cancel();return false;}
}
bool park(std::uintptr_t base,const NativeFrame& hint) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();paired_shared().force_probe_armed.store(false,std::memory_order_release);paired_shared().controller_trial_armed.store(false,std::memory_order_release);cancel();
    try {
        if(!p.attempted)return true;
        NativeFrame f;if(!fresh(p,base,hint,f,false)){fail(p,"pair_park_owner_unavailable_handles_abandoned",true);return false;}
        if(!quiesce_role(p,f,0)||!quiesce_role(p,f,1)){fail(p,"pair_park_failed_handles_abandoned",true);return false;}
        p.active=p.detached=p.calibrating=false;p.parked=true;p.latest={};p.issue="paired_roles_parked_all_eight_proxies_disabled";publish(p);return own_all(p,f);
    }catch(...){fail(p,"pair_park_exception_handles_abandoned",true);return false;}
}
bool detach(std::uintptr_t base,const NativeFrame& hint) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();cancel();
    try {
        NativeFrame f;if(!p.active||!p.prepared||p.failed||p.blocked||!fresh(p,base,hint,f)||!own_all(p,f,true))return false;
        alignas(16) const V4 zero{};
        if(!vector(p.api.overwrite_force,&p.roles[0].body,zero.data())||!own_role(p,f,0,true)||
           !vector(p.api.overwrite_torque,&p.roles[0].body,zero.data())||!own_role(p,f,0,true)||
           !pending_zero(p,0)||!quiesce_role(p,f,1))return false;
        p.active=p.parked=false;p.detached=true;p.latest={};p.issue="bike_retains_momentum_spring_parked_no_ragdoll_transfer_claim";publish(p);return true;
    }catch(...){fail(p,"pair_detach_failed_closed");return false;}
}
void forget() noexcept {fail(paired_pool(),"paired_roles_forgotten_no_stale_world_cleanup",true);}
calibration::Report calibration_report() noexcept{return paired_pool().calibration.report();}
bool begin_calibration(std::uintptr_t base,const NativeFrame& hint,calibration::Kind kind,const std::array<mount_pose::Seed,2>& support) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();
    try {
        if(!calibration_requested(kind)||p.calibration_used||p.calibrating||p.failed||p.blocked)return admission_reject(p,"neutral_disabled_used_or_terminal");
        const auto owner=read_native_frame(base,hint.client);
        if(!same_owner(hint,owner)||!owner.offboard||!owner.standable||!owner.timing_available||!step_supported(owner.seconds))return admission_reject(p,"neutral_current_native_owner_not_standing60Hz");
        Api api;WorldKey world;
        if(!resolve(base,api)||!raw_world(api,owner,world))return admission_reject(p,"neutral_supported_api_or_current_world_unavailable");
        for(unsigned role=0;role<2;++role){const auto& seed=support[role];
            if(!mount_pose::matches(seed,world.world,world.context,owner.serial,owner.simulation_time))return admission_reject(p,"neutral_seed_not_fully_current_and_qualified",role);
            if(!calibration::fixture_supported(seed.pitch,seed.roll,seed.up))return admission_reject(p,"neutral_measured_support_exceeds_reviewed02rad_fixture",role);
        }
        const auto separation=calibration::sub(support[1].root,support[0].root);
        if(!calibration::world_kind(kind)){if(policy::length(separation)>.0001||support[0].orientation!=support[1].orientation)return admission_reject(p,"neutral_pair_requires_same_qualified_seed");}
        else if(calibration::horizontal(separation)<2||calibration::horizontal(separation)>2.3||std::abs(separation[1])>.02f)return admission_reject(p,"neutral_world_seed_separation_outside_reviewed_bounds");
        for(const auto& seed:support)if(calibration::horizontal(calibration::sub(seed.root,owner.position))<1.5||
            policy::length(calibration::sub(seed.root,owner.position))>4)return admission_reject(p,"neutral_seed_distance_from_real_rider_outside_reviewed_bounds");
        if(!acquire(base,owner,support[0]))return admission_reject(p,"neutral_paired_acquisition_not_completed_this_epoch");
        NativeFrame fresh_frame;if(!fresh(p,base,owner,fresh_frame)||fresh_frame.serial!=owner.serial||fresh_frame.simulation_time!=owner.simulation_time)return admission_reject(p,"neutral_native_clock_changed_after_pair_preparation");
        p.calibration_support=support;
        // Analytic nearest endpoint pair: handlebar positive-X endpoint and
        // pelvis negative-X endpoint; only2mm penetration, with parallel axes.
        if(!calibration::world_kind(kind)&&!calibration::isolated_overlap(collision_envelope::capsules))return admission_reject(p,"neutral_isolated2mm_overlap_geometry_no_longer_matches");
        const Vec overlap_shift=calibration::overlap_shift(collision_envelope::capsules);
        std::array<Vec,2> requested_com;std::array<Quat,2> requested_orientation;
        for(unsigned role=0;role<2;++role){
            Quat q,principal,relative;
            if(!policy::unit(support[role].orientation,q)||!policy::unit(policy::v2_roles[role].principal_to_model,principal)||!policy::unit(policy::rider_from_chassis,relative))return admission_reject(p,"neutral_principal_or_support_frame_invalid",role);
            auto root=support[role].root;
            if(calibration::world_kind(kind)){
                const auto plan=calibration::world_placement(collision_envelope::capsules,role,root,q,support[role].up);
                if(!plan.valid)return admission_reject(p,"neutral_equal_clearance_world_plan_invalid",role);root=plan.root;
            }else for(unsigned axis=0;axis<3;++axis)root[axis]+=support[role].up[axis]*.5f;
            if(role&&!calibration::world_kind(kind)){const auto shift=policy::rotate(q,overlap_shift);for(unsigned axis=0;axis<3;++axis)root[axis]+=shift[axis];}
            const auto offset=policy::rotate(q,policy::v2_roles[role].com_from_bike_root);
            const auto native=policy::native_orientation(role?policy::multiply(q,relative):q,principal);
            requested_com[role]={root[0]+offset[0],root[1]+offset[1],root[2]+offset[2]};requested_orientation[role]=native;
            alignas(16) const std::array<float,8> transform{native[0],native[1],native[2],native[3],root[0]+offset[0],root[1]+offset[1],root[2]+offset[2],1};
            std::uint64_t serial{};double time{};
            if(!read_native_clock(owner.entity,owner.core,serial,time)||serial!=owner.serial||time!=owner.simulation_time||
                !own_all(p,owner)||!vector(p.api.transform,&p.roles[role].body,transform.data())||!own_all(p,owner))return admission_reject(p,"neutral_transform_or_clock_proof_failed",role);
        }
        const auto initial=observe(base,owner);if(!initial.available||initial.epoch.serial!=owner.serial||initial.epoch.time!=owner.simulation_time)return admission_reject(p,"neutral_actual_initial_pair_not_current");
        for(unsigned role=0;role<2;++role)if(!policy::pose_matches(initial.roles[role].position,initial.roles[role].orientation,
            requested_com[role],requested_orientation[role]))return admission_reject(p,"neutral_actual_pose_disagrees_with_clearance_and_overlap_plan",role);
        const auto initial_sample=calibration_sample(p,initial);
        if(calibration::world_kind(kind))for(unsigned role=0;role<2;++role)
            if(!std::isfinite(initial_sample.clearance[role])||std::abs(initial_sample.clearance[role]-calibration::world_fixture_clearance)>.0005f)
                return admission_reject(p,"neutral_actual_world_proxy_clearance_disagrees_with08m_plan",role);
        if(!p.calibration.begin(kind,initial_sample))return admission_reject(p,"neutral_monitor_refused_actual_initial_state");
        p.calibration_used=true;p.calibrating=true;p.parked=false;p.calibration_until=GetTickCount64()+1500;
        if(!admit_neutral_masks(p,owner)){
            p.calibration.fail("neutral_mask_admission_failed");p.calibration.parked(park(base,owner));return false;
        }
        for(unsigned role=0;role<2;++role)if(!pending_zero(p,role)||!own_role(p,owner,role)||
            !integer(p.api.motion,&p.roles[role].body,3)||!own_role(p,owner,role,true)||!touch(p.api.wake,&p.roles[role].body)){
                p.calibration.fail("neutral_dynamic_admission_failed");p.calibration.parked(park(base,owner));return false;
            }
        if(!own_all(p,owner,true)){p.calibration.fail("neutral_final_admission_proof_failed");p.calibration.parked(park(base,owner));return false;}
        p.latest={};p.issue="neutral_pair_calibration_no_controller_forces";p.admission_issue="admitted_actual_pose_and_configuration_verified";p.rejected_role=0xffffffffU;publish(p);return true;
    }catch(...){p.calibration.fail("neutral_begin_exception");p.calibration.parked(park(base,hint));return false;}
}
calibration::Report poll_calibration(std::uintptr_t base,const NativeFrame& hint,bool gates) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();
    try {
        if(!p.calibrating)return p.calibration.report();
        if(!gates||!calibration_requested(p.calibration.report().kind)||GetTickCount64()>=p.calibration_until)p.calibration.fail("neutral_gate_or_wall_limit");
        else {
            const auto frame=observe(base,hint);
            if(!frame.available)p.calibration.fail("neutral_actual_pair_observation_unavailable");
            else (void)p.calibration.observe(calibration_sample(p,frame));
        }
        if(p.calibration.report().phase!=calibration::Phase::running)p.calibration.parked(park(base,hint));
        return p.calibration.report();
    }catch(...){p.calibration.fail("neutral_poll_exception");p.calibration.parked(park(base,hint));return p.calibration.report();}
}
#include "bmx_native_coupled_force_probe.inc"
#include "bmx_native_coupled_trial.inc"
namespace detail {
bool allocation_claimed() noexcept{return paired_shared().claimed.load(std::memory_order_acquire);}
void produce(void* incoming_pointer,std::uintptr_t wrapper) noexcept {
    auto& s=paired_shared();
    if(!collision_adoption_ready()&&!s.force_probe_armed.load(std::memory_order_acquire)&&!s.controller_trial_armed.load(std::memory_order_acquire))return;
    if(!TryAcquireSRWLockExclusive(&s.lock))return;
    auto& work=s.lease;
    if(!collision_adoption_ready()&&!((work.force_probe&&s.force_probe_armed.load(std::memory_order_acquire))||
        (work.controller_trial&&s.controller_trial_armed.load(std::memory_order_acquire)))){ReleaseSRWLockExclusive(&s.lock);return;}
    if(!work.active||s.info.failed){ReleaseSRWLockExclusive(&s.lock);return;}
    if(GetTickCount64()>=work.until){work.active=false;s.info.receipt.state=policy::ReceiptState::expired;ReleaseSRWLockExclusive(&s.lock);return;}
    bool matches_wrapper=false;for(auto expected:work.roles[0].wrappers)matches_wrapper|=wrapper==expected;
    if(!matches_wrapper){ReleaseSRWLockExclusive(&s.lock);return;}++s.info.producer_matches;
    Handle incoming;
    const bool read_incoming=read(reinterpret_cast<std::uintptr_t>(incoming_pointer),incoming);
    const auto integrates=[&]() noexcept {bool a{},b{};return angular_integration_bit(work.roles[0].native_state,work.roles[0].body,a)&&a&&
        angular_integration_bit(work.roles[1].native_state,work.roles[1].body,b)&&b;};
    const auto angular_limits=[&]() noexcept {
        for(const auto& role:work.roles){std::uintptr_t source{},again{};float value{},repeated{};const auto offset=std::uintptr_t{role.body.index}*4;
            if(!read(role.native_state+0x998,source)||!pointer(source)||!read(source+offset,value)||value!=15||
               !read(role.native_state+0x998,again)||again!=source||!read(again+offset,repeated)||repeated!=value)return false;}
        return true;
    };
    const auto proof=[&]() noexcept {return read_incoming&&(!work.force_probe||s.force_probe_armed.load(std::memory_order_acquire))&&
        (!work.controller_trial||s.controller_trial_armed.load(std::memory_order_acquire))&&GetTickCount64()<work.until&&
        producer_owner(work.roles[0],incoming)&&producer_owner(work.roles[1],incoming)&&
        producer_clock(work.roles[0])&&producer_clock(work.roles[1])&&integrates()&&angular_limits();};
    // The whole group is consumed before the first of the four calls. A
    // partial result records attempted/completed masks and is never replayed.
    work.active=false;
    s.info.receipt=s.once.run(work.epoch,work.command,proof,[&](unsigned role,const Vec& target,Vec& verified) noexcept {
        auto& lease=work.roles[role];auto updated=lease.configuration;
        alignas(16) const V4 ratio{updated.mass/target[0],updated.mass/target[1],updated.mass/target[2],0};
        updated.inertia_ratio=ratio;
        if(!vector(lease.api.inertia_ratio,&lease.body,ratio.data()))return false;
        // No body-pose rebase: only principal diagonal values change. Owner,
        // clock and exact ratio readback must pass before ANY wrench call.
        if(!configured_body(lease.native_state,lease.body.index,updated))return false;
        lease.configuration=updated;
        if(!proof())return false;
        V4 actual_ratio{};std::uintptr_t source{};
        if(!array4(lease.native_state,0x988,lease.body.index,actual_ratio,source)||actual_ratio!=ratio)return false;
        for(unsigned axis=0;axis<3;++axis)verified[axis]=1.f/(updated.inverse_mass*actual_ratio[axis]);
        s.native_configuration[role]=updated;return proof();
    },[&](unsigned role,bool torque,const Vec& value) noexcept {
        alignas(16) const V4 v{value[0],value[1],value[2],0};auto& lease=work.roles[role];
        return vector(torque?lease.api.torque:lease.api.force,&lease.body,v.data());
    });
    if(s.info.receipt.state==policy::ReceiptState::submitted)++s.info.submissions;
    else if(s.info.receipt.state==policy::ReceiptState::terminal){s.info.failed=true;s.info.issue="partial_paired_native_submission_terminal_no_replay";++s.info.rejected;}
    ReleaseSRWLockExclusive(&s.lock);
}
}
} // namespace dingosdk::bmx::native_coupled
