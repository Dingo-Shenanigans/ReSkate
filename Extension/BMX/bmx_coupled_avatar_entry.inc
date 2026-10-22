// Include after fresh_mount_seed inside bmx_runtime.cpp's private namespace,
// with bmx_coupled_avatar_frame.h included at translation-unit top level.
// Caller retains the exact private profile, gameplay/input/protection gates.
// No old chassis_ride controller, physical body allocation or force adoption.
namespace coupled_avatar_entry {
namespace ab=avatar_binding;namespace af=coupled_avatar_frame;namespace np=native_coupled::policy;
struct Result {
    bool ready{},failed{},published{},motion_submitted{},placement_completed{},calibrated{},actual_spring_render_verified{};
    np::Epoch published_epoch,render_epoch;const char* issue="not_started";
};
struct State {
    bool started{},terminal{},last_was_bound{};std::uintptr_t base{};
    np::Epoch identity,last_epoch,published_epoch;
    mount_transition::Transition mount;ab::Observation published_actual;
    double started_time{};float root_height{};Result last;
};
State& state(){static State value;return value;}
void withdraw() noexcept {release_native_motion();visual::publish({});}
void reset() noexcept {withdraw();state()={};}
Result fail(const char* issue) noexcept {
    auto& s=state();withdraw();s.terminal=true;s.last={};s.last.failed=true;s.last.issue=issue;return s.last;
}
Result wait(const char* issue) noexcept {
    auto& s=state();withdraw();s.last={};s.last.issue=issue;s.last_was_bound=false;return s.last;
}
bool coherent(std::uintptr_t base,const NativeFrame& native,const native_coupled::Frame& pair) noexcept {
    if(!af::owner_epoch(native,pair.epoch))return false;
    const auto before=read_native_frame(base,native.client);if(!af::same_native(native,before,pair.epoch))return false;
    const auto again=native_coupled::observe(base,before);if(!af::same_pair(pair,again))return false;
    const auto after=read_native_frame(base,native.client);return af::same_native(before,after,pair.epoch);
}
bool unchanged_parked_pose(const ab::Observation& before,const ab::Observation& now) noexcept {
    return ab::same_identity(before.epoch,now.epoch)&&before.spring_com==now.spring_com&&before.spring_orientation==now.spring_orientation&&
        before.bike_root==now.bike_root&&before.bike_orientation==now.bike_orientation&&before.spring_velocity==now.spring_velocity&&
        before.spring_angular_velocity==now.spring_angular_velocity;
}
bool neutral_pair(const native_coupled::Frame& pair) noexcept {
    if(!pair.available||pair.active||!pair.parked||pair.detached)return false;
    for(auto mask:pair.categories)if(mask)return false;for(auto mask:pair.collides_with)if(mask)return false;
    for(const auto& body:pair.roles)if(np::length(body.linear_velocity)>.01||np::length(body.angular_velocity)>.01)return false;
    return true;
}
bool place(std::uintptr_t base,const NativeFrame& native,const visual::Frame& frame) noexcept {
    visual::begin_client_tick();visual::publish(frame);const auto pending=visual::pending_placement();
    if(!pending.available||pending.frame.entity!=native.entity)return false;
    const auto placed=world::tick(base,native.client,pending.frame,pending.revision);
    return placed.complete&&visual::complete_placement(placed.revision,placed.entity,placed.complete);
}
// Caller measured rootHeight from a fresh native root/floor before prepare.
// Each passed seed is refreshed in this exact parked-pair/native epoch.
Result tick(std::uintptr_t base,const NativeFrame& native,const native_coupled::Frame& pair,
            const mount_pose::Seed& seed,float verified_root_height,bool input_neutral) noexcept {
    auto& s=state();try {
        if(s.terminal)return s.last;
        if(!input_neutral)return fail("neutral_entry_requires_current_quiet_input");
        const auto actual=af::observe(pair);
        if(!ab::valid(actual)||!neutral_pair(pair)||!af::owner_epoch(native,pair.epoch)||
           !std::isfinite(verified_root_height)||verified_root_height<0||verified_root_height>2)return fail("neutral_entry_actual_pair_or_owner_invalid");
        if(!mount_pose::matches(seed,pair.epoch.owner_world[5],pair.epoch.owner_world[7],pair.epoch.serial,pair.epoch.time)||
           !ab::close(seed.root,actual.bike_root,.003f)||!ab::close_rotation(seed.orientation,actual.bike_orientation))return wait("neutral_entry_needs_current_verified_seed");
        if(s.started&&(!ab::same_identity(s.identity,pair.epoch)||s.base!=base||s.root_height!=verified_root_height))return fail("neutral_entry_identity_changed");
        if(!coherent(base,native,pair))return wait("neutral_entry_needs_coherent_native_epoch");
        if(!s.started){np::Quat basis;if(!mount_transition::native_rotation(native.transform,basis)||!s.mount.capture(native.position,basis))return fail("neutral_entry_native_basis_invalid");
            s.started=true;s.base=base;s.identity=pair.epoch;s.started_time=pair.epoch.time;s.root_height=verified_root_height;}
        if(pair.epoch.time-s.started_time>8)return fail("neutral_entry_eight_native_second_limit");
        if(s.last_epoch.serial&&!af::forward_epoch(s.last_epoch,pair.epoch))return fail("neutral_entry_clock_rewound");
        if(s.last_epoch.serial==pair.epoch.serial){if(s.last_epoch!=pair.epoch)return fail("neutral_entry_duplicate_epoch_changed");return s.last;}
        const auto binding=rider::coupled_avatar_binding_status();
        const bool calibrated=binding.calibration.ready&&binding.proximal_ready&&binding.completed_pose_write&&
            ab::same_identity(binding.calibration.bind_epoch,pair.epoch);
        const auto mounted_root=ab::add(seed.root,np::rotate(seed.orientation,{0,verified_root_height,0}));
        const auto entry=s.mount.sample(mounted_root,actual.bike_orientation,pair.epoch.seconds);
        if(!entry.ready)return fail("neutral_entry_transition_refused");
        auto target=entry.root,velocity=entry.velocity;auto rotation=entry.rotation;
        if(calibrated){const auto mapped=ab::sample(binding.calibration,actual);if(!mapped.ready)return fail("neutral_entry_calibrated_target_invalid");
            target=mapped.root;velocity=mapped.point_velocity;rotation=mapped.rotation;}
        visual::Frame frame;frame.active=true;frame.entity=native.entity;frame.position=actual.bike_root;
        frame.chassis_quaternion=actual.bike_orientation;frame.chassis_quaternion_available=true;
        frame.rider_mode=visual::RiderMode::riding;frame.rider_blend=entry.blend;frame.mount_progress=entry.progress;
        frame.mount_from_left=entry.from_left;frame.native_body_token=pair.epoch.token;
        if(entry.progress<1)frame.mount_token=pair.epoch.token;
        // Standing/partial entry never captures a body offset. At the endpoint
        // wait for the genuine native root to converge through its motion API.
        const bool capture_now=entry.progress==1&&entry.blend==1&&ab::close(native.position,mounted_root,.003f);
        frame.spring_avatar.actual=actual;frame.spring_avatar.requested=calibrated||capture_now;
        if(capture_now&&!calibrated){auto& capture=frame.spring_avatar.capture;capture.requested=true;capture.neutral=input_neutral;
            capture.native=af::root_witness(actual,native,true);capture.seed=seed;capture.root_height=verified_root_height;
            capture.root_height_verified=true;capture.mount_progress=capture.rider_blend=1;}
        np::Quat current_native_basis;const bool basis_matches=mount_transition::native_rotation(native.transform,current_native_basis)&&ab::close_rotation(current_native_basis,rotation);
        const bool prior_render=calibrated&&basis_matches&&s.last_was_bound&&binding.actual_spring_applied&&binding.completed_pose_epoch==s.published_epoch&&
            binding.hand_error<.003f&&binding.foot_error<.003f&&unchanged_parked_pose(s.published_actual,actual)&&
            ab::close(native.position,target,.003f);
        if(!coherent(base,native,pair))return wait("neutral_entry_epoch_changed_before_motion");
        if(!submit_native_chassis_motion(native,velocity,target,rotation))return fail("neutral_entry_motion_lease_refused");
        const auto lease=native_motion_lease_status(true);frame.spring_avatar.native_motion_lease_revision=lease.lease_revision;
        auto lease_frame=frame.spring_avatar;lease_frame.requested=true;ab::Target requested;requested.ready=true;requested.epoch=pair.epoch;
        requested.root=target;requested.point_velocity=velocity;requested.rotation=rotation;
        if(!ab::motion_lease_matches(lease_frame,requested,lease))return fail("neutral_entry_motion_lease_receipt_mismatch");
        if(calibrated&&!ab::motion_lease_matches(frame.spring_avatar,ab::sample(binding.calibration,actual),lease))return fail("neutral_entry_calibrated_lease_mismatch");
        if(!coherent(base,native,pair))return wait("neutral_entry_epoch_changed_before_publication");
        if(!place(base,native,frame))return wait("neutral_entry_waiting_for_all_part_placement");
        s.last_epoch=pair.epoch;s.published_epoch=pair.epoch;s.published_actual=actual;s.last_was_bound=calibrated;
        s.last={};s.last.ready=prior_render;s.last.published=s.last.motion_submitted=s.last.placement_completed=true;
        s.last.calibrated=calibrated;s.last.actual_spring_render_verified=prior_render;s.last.published_epoch=pair.epoch;
        s.last.render_epoch=binding.completed_pose_epoch;s.last.issue=prior_render?"parked_neutral_entry_and_bound_render_verified":
            calibrated?"awaiting_calibrated_spring_render":capture_now?"awaiting_completed_mounted_geometry_capture":"mount_entry_in_progress";
        return s.last;
    }catch(...){return fail("neutral_entry_exception");}
}
// Active physics stays owned by coupled_ride. This publishes only the genuine
// actual-pair mapping and leaves force/adoption/fall qualification unchanged.
Result bind_active(std::uintptr_t base,const NativeFrame& native,const native_coupled::Frame& pair,visual::Frame frame) noexcept {
    auto& s=state();try {
        const auto actual=af::observe(pair);const auto binding=rider::coupled_avatar_binding_status();
        if(!s.started||s.terminal||!pair.active||pair.parked||pair.detached||!ab::valid(actual)||
           !ab::same_identity(s.identity,pair.epoch)||!binding.calibration.ready||!binding.proximal_ready||!binding.completed_pose_write||
           !af::owner_epoch(native,pair.epoch)||!af::forward_epoch(s.last_epoch,pair.epoch)||!coherent(base,native,pair))return fail("active_avatar_binding_current_pair_invalid");
        if(s.last_was_bound&&(!binding.actual_spring_applied||binding.completed_pose_epoch!=s.published_epoch||
           binding.hand_error>=.003f||binding.foot_error>=.003f))return fail("active_avatar_binding_previous_render_missing");
        const auto mapped=ab::sample(binding.calibration,actual);if(!mapped.ready)return fail("active_avatar_binding_target_refused");
        if(!frame.active||!frame.chassis_quaternion_available||frame.entity!=native.entity||!ab::close(frame.position,actual.bike_root)||
           !ab::close_rotation(frame.chassis_quaternion,actual.bike_orientation))return fail("active_avatar_binding_bike_frame_mismatch");
        frame.rider_mode=visual::RiderMode::riding;frame.rider_blend=frame.mount_progress=1;frame.mount_token=0;
        frame.native_body_token=pair.epoch.token;frame.spring_avatar={};frame.spring_avatar.requested=true;frame.spring_avatar.actual=actual;
        if(!submit_native_chassis_motion(native,mapped.point_velocity,mapped.root,mapped.rotation))return fail("active_avatar_binding_motion_refused");
        const auto lease=native_motion_lease_status(true);frame.spring_avatar.native_motion_lease_revision=lease.lease_revision;
        if(!ab::motion_lease_matches(frame.spring_avatar,mapped,lease)||!coherent(base,native,pair))return fail("active_avatar_binding_lease_or_epoch_changed");
        if(!place(base,native,frame))return fail("active_avatar_binding_placement_refused");
        s.last_epoch=s.published_epoch=pair.epoch;s.published_actual=actual;s.last_was_bound=true;
        s.last={};s.last.published=s.last.motion_submitted=s.last.placement_completed=s.last.calibrated=true;s.last.published_epoch=pair.epoch;
        s.last.render_epoch=binding.completed_pose_epoch;s.last.issue="actual_spring_binding_published_render_pending";return s.last;
    }catch(...){return fail("active_avatar_binding_exception");}
}
}
