namespace controller_trial {
namespace {
trial_policy::Clock& clock(){static auto* value=new trial_policy::Clock;return *value;}
bool stop_failed(std::uintptr_t base,const NativeFrame& owner,const char* issue) noexcept {
    clock().cancel();paired_shared().controller_trial_armed.store(false,std::memory_order_release);cancel();
    (void)park(base,owner);auto& p=paired_pool();p.issue=issue;publish(p);return false;
}
}
bool selected() noexcept {
#if defined(DINGOSDK_BMX_STANDALONE_MOVEMENT)
    wchar_t ride[2]{},trial[2]{};return enabled()&&
        GetEnvironmentVariableW(L"RESKATE_BMX_COUPLED_RIDE",ride,2)==1&&ride[0]==L'1'&&
        GetEnvironmentVariableW(L"RESKATE_BMX_COUPLED_CONTROLLER_TRIAL",trial,2)==1&&trial[0]==L'1'&&
        !force_probe::selected();
#else
    return false;
#endif
}
bool activate(std::uintptr_t base,const NativeFrame& hint) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();try {
        if(!selected()||clock().used()||!p.prepared||p.active||!p.parked||p.calibrating||p.calibration_used||p.failed||p.blocked)return false;
        NativeFrame owner;if(!fresh(p,base,hint,owner)||!owner.offboard||!own_all(p,owner))return false;
        const auto actual=observe(base,owner);if(!trial_policy::qualified(actual,false))return false;
        if(!clock().begin(actual.epoch,GetTickCount64(),enabled(),true,true))return false;
        auto hook=proof_pool(p,0);if(!install_hook(hook)||!admit_neutral_masks(p,owner))return stop_failed(base,owner,"private_trial_hook_or_mask_admission_failed");
        for(unsigned role=0;role<2;++role)if(!pending_zero(p,role)||!own_role(p,owner,role)||!integer(p.api.motion,&p.roles[role].body,3)||
            !own_role(p,owner,role,true)||!touch(p.api.wake,&p.roles[role].body)||!own_role(p,owner,role,true))return stop_failed(base,owner,"private_trial_partial_dynamic_admission_failed");
        p.active=true;p.parked=p.detached=false;p.latest={};p.issue="private_ten_second_coupled_controller_trial_normal_adoption_disabled";publish(p);
        const auto admitted=observe(base,owner);
        if(admitted.epoch!=actual.epoch||!trial_policy::qualified(admitted,true)||!clock().accepts(admitted.epoch,GetTickCount64()))
            return stop_failed(base,owner,"private_trial_actual_admission_or_epoch_not_qualified");
        paired_shared().controller_trial_armed.store(true,std::memory_order_release);return true;
    }catch(...){return stop_failed(base,hint,"private_trial_activation_exception");}
}
bool submit(const Frame& frame,const Command& command) noexcept {
    native_chassis::LastError error;auto& p=paired_pool();try {
        if(!selected()||!p.active||!p.prepared||p.failed||p.blocked||!p.latest.available||
            !trial_policy::qualified(frame,true)||!trial_policy::qualified(p.latest,true)||!trial_policy::same_actual(frame,p.latest)||!policy::prepare(command,frame.epoch.seconds).valid)
            return stop_failed(p.api.base,p.owner,"private_trial_current_pair_or_command_not_qualified");
        const auto until=clock().deadline(frame.epoch,GetTickCount64());
        if(!until)return stop_failed(p.api.base,p.owner,"private_trial_ten_second_backend_deadline");
        NativeFrame owner;if(!fresh(p,p.api.base,p.owner,owner)||owner.serial!=frame.epoch.serial||owner.simulation_time!=frame.epoch.time||!own_all(p,owner,true))
            return stop_failed(p.api.base,p.owner,"private_trial_owner_or_epoch_changed_before_queue");
        auto proof=proof_pool(p,0);PairLease work;if(!make_lease(proof,owner,work.roles[0]))return stop_failed(p.api.base,owner,"private_trial_role_lease_unavailable");
        work.roles[1]=work.roles[0];work.roles[1].body=p.roles[1].body;work.roles[1].configuration=p.roles[1].configuration;
        for(auto& role:work.roles){role.token=p.token;role.serial=frame.epoch.serial;role.simulation_time=frame.epoch.time;role.step_seconds=frame.epoch.seconds;}
        work.epoch=frame.epoch;work.command=command;work.until=until;work.active=work.controller_trial=true;
        auto& shared=paired_shared();AcquireSRWLockExclusive(&shared.lock);
        if(!shared.controller_trial_armed.load(std::memory_order_acquire)||shared.info.failed||shared.lease.active||frame.epoch.serial<=shared.info.receipt.serial){
            ReleaseSRWLockExclusive(&shared.lock);return stop_failed(p.api.base,owner,"private_trial_duplicate_or_disarmed_queue");}
        shared.lease=work;shared.info.receipt={policy::ReceiptState::queued,p.token,frame.epoch.serial,command.hop_part,0,0};shared.info.receipt.epoch=frame.epoch;
        ReleaseSRWLockExclusive(&shared.lock);return true;
    }catch(...){return stop_failed(p.api.base,p.owner,"private_trial_submit_exception");}
}
}
