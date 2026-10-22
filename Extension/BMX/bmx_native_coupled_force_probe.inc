// Inside native_coupled: verified owner/configuration helpers only. There is
// no caller-supplied wrench API and no new hook or recovered native address.
namespace force_probe {
namespace {
struct Probe {bool used{},active{};ULONGLONG until{};Monitor monitor;};
Probe& probe(){static auto* p=new Probe;return *p;}
bool queue(const Frame& frame) noexcept {
    auto& p=paired_pool();const auto& monitor=probe().monitor;const auto cmd=monitor.pending_command();
    if(!selected()||!probe().active||!p.active||!p.prepared||p.failed||p.blocked||!frame.available||!frame.active||
       !p.latest.available||frame.epoch!=p.latest.epoch||frame.epoch.token!=p.token||
       !frame.integrates_angular_accumulator[0]||!frame.integrates_angular_accumulator[1]||
       frame.maximum_angular_speed[0]!=15||frame.maximum_angular_speed[1]!=15||!policy::prepare(cmd,frame.epoch.seconds).valid)return false;
    NativeFrame f;if(!fresh(p,p.api.base,p.owner,f)||f.serial!=frame.epoch.serial||f.simulation_time!=frame.epoch.time||!own_all(p,f,true))return false;
    auto proof=proof_pool(p,0);PairLease work;if(!make_lease(proof,f,work.roles[0]))return false;
    work.roles[1]=work.roles[0];work.roles[1].body=p.roles[1].body;work.roles[1].configuration=p.roles[1].configuration;
    for(auto& role:work.roles){role.token=p.token;role.serial=frame.epoch.serial;role.simulation_time=frame.epoch.time;role.step_seconds=frame.epoch.seconds;}
    work.epoch=frame.epoch;work.command=cmd;work.until=std::min(GetTickCount64()+100,probe().until);work.active=work.force_probe=true;
    auto& shared=paired_shared();AcquireSRWLockExclusive(&shared.lock);
    if(!shared.force_probe_armed.load(std::memory_order_acquire)||shared.info.failed||shared.lease.active||frame.epoch.serial<=shared.info.receipt.serial){ReleaseSRWLockExclusive(&shared.lock);return false;}
    shared.lease=work;shared.info.receipt={policy::ReceiptState::queued,p.token,frame.epoch.serial,0,0,0};shared.info.receipt.epoch=frame.epoch;
    ReleaseSRWLockExclusive(&shared.lock);return true;
}
bool terminate(std::uintptr_t base,const NativeFrame& owner,const char* issue=nullptr) noexcept {
    auto& s=probe();if(issue)s.monitor.fail(issue);s.active=false;
    paired_shared().force_probe_armed.store(false,std::memory_order_release);cancel();s.monitor.parked(park(base,owner));return false;
}
}
bool selected() noexcept {wchar_t flag[2]{};return enabled()&&GetEnvironmentVariableW(L"RESKATE_BMX_COUPLED_FORCE_PROBE",flag,2)==1&&flag[0]==L'1';}
Report current_report() noexcept{return probe().monitor.report();}
bool begin(std::uintptr_t base,const NativeFrame& hint,const std::array<mount_pose::Seed,2>& support) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();auto& s=probe();try {
        if(!selected()||s.used||p.active||p.calibrating||p.calibration_used||p.failed||p.blocked)return false;
        const auto owner=read_native_frame(base,hint.client);Api api;WorldKey world;
        if(!same_owner(hint,owner)||!owner.offboard||!owner.standable||!owner.timing_available||!step_supported(owner.seconds)||
           !resolve(base,api)||!raw_world(api,owner,world))return false;
        for(unsigned role=0;role<2;++role){const auto& seed=support[role];
            if(!mount_pose::matches(seed,world.world,world.context,owner.serial,owner.simulation_time)||
               !calibration::fixture_supported(seed.pitch,seed.roll,seed.up)||
               calibration::horizontal(calibration::sub(seed.root,owner.position))<1.5||policy::length(calibration::sub(seed.root,owner.position))>4)return false;}
        const auto separation=calibration::sub(support[1].root,support[0].root);
        if(calibration::horizontal(separation)<2||calibration::horizontal(separation)>2.3||std::abs(separation[1])>.02f)return false;
        if(!acquire(base,owner,support[0]))return false;s.used=true;p.calibration_support=support;
        std::array<calibration::WorldPlacement,2> plans;
        for(unsigned role=0;role<2;++role){plans[role]=placement(collision_envelope::capsules,role,support[role].root,support[role].orientation,support[role].up);
            if(!plans[role].valid)return terminate(base,owner,"probe_private_air_placement_invalid");
            const auto& plan=plans[role];alignas(16) const std::array<float,8> transform{plan.native_orientation[0],plan.native_orientation[1],plan.native_orientation[2],plan.native_orientation[3],plan.com[0],plan.com[1],plan.com[2],1};
            std::uint64_t serial{};double time{};
            if(!read_native_clock(owner.entity,owner.core,serial,time)||serial!=owner.serial||time!=owner.simulation_time||!own_all(p,owner)||
               !vector(p.api.transform,&p.roles[role].body,transform.data())||!own_all(p,owner))return terminate(base,owner,"probe_air_transform_or_clock_proof_failed");
        }
        const auto initial=observe(base,owner);if(!initial.available||initial.epoch.serial!=owner.serial||initial.epoch.time!=owner.simulation_time)return terminate(base,owner,"probe_actual_initial_pair_not_current");
        for(unsigned role=0;role<2;++role)if(!policy::pose_matches(initial.roles[role].position,initial.roles[role].orientation,plans[role].com,plans[role].native_orientation))
            return terminate(base,owner,"probe_actual_pose_disagrees_with_air_plan");
        if(!s.monitor.begin(calibration_sample(p,initial)))return terminate(base,owner,"probe_initial_actual_config_or_clearance_not_qualified");
        auto hook=proof_pool(p,0);if(!install_hook(hook))return terminate(base,owner,"probe_existing_hook_unavailable");
        if(!admit_neutral_masks(p,owner))return terminate(base,owner,"probe_world_mask_admission_failed");
        for(unsigned role=0;role<2;++role)if(!pending_zero(p,role)||!own_role(p,owner,role)||!integer(p.api.motion,&p.roles[role].body,3)||
            !own_role(p,owner,role,true)||!touch(p.api.wake,&p.roles[role].body)||!own_role(p,owner,role,true))return terminate(base,owner,"probe_dynamic_admission_failed");
        p.active=true;p.parked=p.detached=false;p.latest={};p.issue="private_six_epoch_small_wrench_probe_not_ride_adoption";publish(p);
        s.active=true;s.until=GetTickCount64()+1000;paired_shared().force_probe_armed.store(true,std::memory_order_release);
        const auto fresh_actual=observe(base,owner);
        if(!fresh_actual.available||fresh_actual.epoch!=initial.epoch||!queue(fresh_actual))return terminate(base,owner,"probe_initial_zero_group_not_queued");return true;
    }catch(...){return terminate(base,hint,"probe_begin_exception");}
}
Report poll(std::uintptr_t base,const NativeFrame& hint,bool gates) noexcept {
    native_chassis::LastError error;auto& s=probe();auto& p=paired_pool();try {
        if(!s.active)return s.monitor.report();
        if(!gates||!selected()||GetTickCount64()>=s.until){(void)terminate(base,hint,"probe_gate_or_one_second_deadline");return s.monitor.report();}
        const auto frame=observe(base,hint);if(!frame.available){(void)terminate(base,hint,"probe_actual_pair_observation_unavailable");return s.monitor.report();}
        const auto report=s.monitor.report();const auto& before=report.actual[report.completed];const auto receipt=status().receipt;
        if(receipt.state!=policy::ReceiptState::queued&&receipt.state!=policy::ReceiptState::submitted){(void)terminate(base,hint,"probe_producer_terminal_cancelled_or_expired");return s.monitor.report();}
        if(frame.epoch==before.epoch)return s.monitor.report();
        if(!s.monitor.accept(receipt,calibration_sample(p,frame))){(void)terminate(base,hint);return s.monitor.report();}
        if(s.monitor.report().phase==Phase::complete){(void)terminate(base,hint);return s.monitor.report();}
        if(!queue(frame))(void)terminate(base,hint,"probe_next_deterministic_group_not_queued");return s.monitor.report();
    }catch(...){(void)terminate(base,hint,"probe_poll_exception");return s.monitor.report();}
}
}
