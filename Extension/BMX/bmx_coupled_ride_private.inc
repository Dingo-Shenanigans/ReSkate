// Included only with BOTH developer features after fresh_mount_seed and the
// coupled avatar entry helper. The exact private profile owns this operator.
namespace coupled_ride_private {
namespace ca=coupled_avatar_entry;namespace af=coupled_avatar_frame;namespace ab=avatar_binding;
#include "bmx_coupled_trial_markers.inc"
struct Row {
    native_coupled::Frame actual;RawInput input;unsigned buttons{};
    ca::Result avatar;bool committed{},queued{},evaluated{};const char* phase="none";const char* issue="none";
    native_coupled::Status native;coupled_ride::Result control;bool have_control{};
};
struct Private {
    bool initialized{},selected{},attempted{},activated{},activation_attempted{},terminal{},exported{};std::atomic<bool> abort{};
    bool neutral_avatar_binding_verified{},active_avatar_binding_verified{};float verified_root_height{};
    ULONGLONG created{},quiet_since{},entry_finish_after{},finish_after{},flush_after{};
    std::uint64_t rows{},last_serial{},entry_ticks{},entry_publications{},entry_placements{},active_publications{};
    std::array<Row,48> history;trial_marker::Trace<Row> trace;NativeFrame owner;native_coupled::Status backend,backend_before_stop;
    coupled_ride::Result bridge_before_stop;ULONGLONG activation_begun{},activation_succeeded{};
    ca::Result entry_receipt,active_receipt;rider::CoupledAvatarBindingStatus binding_receipt;
    const char* issue="waiting_for_quiet_offboard_owner";
};
Private& state(){static auto* p=new Private;return *p;}
bool selected() noexcept {auto& s=state();if(!s.initialized){s.initialized=true;s.selected=coupled_ride::selected();s.created=GetTickCount64();if(s.selected)markers::initialize(s.created);}return s.selected;}
void request_stop() noexcept {state().abort.store(true,std::memory_order_release);native_coupled::cancel();}
bool neutral(const overlay::BmxInputSample& input) noexcept {
    return !input.buttons&&!input.quick_hop&&!input.dismount_requested&&!input.left_shoulder&&!input.right_shoulder&&
        std::abs(input.left_x)<.05f&&std::abs(input.left_y)<.05f&&std::abs(input.right_x)<.05f&&std::abs(input.right_y)<.05f&&
        input.pedal<.05f&&input.brake<.05f&&input.left_trigger<.05f&&input.right_trigger<.05f;
}
RawInput raw_input(const overlay::BmxInputSample& input) noexcept {
    RawInput raw;raw.available=input.available;raw.focused=input.focused;raw.device=input.device;
    raw.menu_open=input.menu_blocked||input.capture_blocked||input.menu_requested;
    raw.left_x=input.left_x;raw.left_y=input.left_y;raw.right_x=input.right_x;raw.right_y=input.right_y;raw.pedal=input.pedal;raw.brake=input.brake;
    raw.quick_hop=input.quick_hop;raw.left_trigger=input.left_trigger;raw.right_trigger=input.right_trigger;
    raw.left_shoulder=input.left_shoulder;raw.right_shoulder=input.right_shoulder;return raw;
}
void remember(const native_coupled::Frame& actual,const overlay::BmxInputSample& input,const ca::Result& avatar,
              const char* phase,const char* issue,bool committed=false,bool queued=false,bool evaluated=false,const coupled_ride::Result* control=nullptr) noexcept {
    auto& s=state();if(!actual.available)return;Row row{actual,raw_input(input),input.buttons,avatar,committed,queued,evaluated,phase,issue};
    row.native=native_coupled::status();if(control){row.control=*control;row.have_control=true;}
    s.trace.append(row,std::string_view(phase).starts_with("active_"));
    if(actual.epoch.serial==s.last_serial)return;s.last_serial=actual.epoch.serial;s.history[s.rows++%48]=row;
}
void finish(std::uintptr_t base,const NativeFrame& owner,const char* issue) noexcept {
    auto& s=state();s.binding_receipt=rider::coupled_avatar_binding_status();s.bridge_before_stop=coupled_ride::status();s.backend_before_stop=native_coupled::status();
    native_coupled::Frame terminal_pair;bool terminal_epoch_available=false;
    if(owner.client){const auto current=read_native_frame(base,owner.client);if(current.available&&current.timing_available&&current.offboard){
        terminal_pair=native_coupled::observe(base,current);terminal_epoch_available=terminal_pair.available&&af::owner_epoch(current,terminal_pair.epoch);}}
    Row event;event.actual=terminal_pair;event.phase="terminal";event.issue=issue;event.native=s.backend_before_stop;event.control=s.bridge_before_stop;event.have_control=true;s.trace.event(event);
    coupled_ride::stop(base,owner);s.backend=native_coupled::status();ca::reset();
    markers::terminal(terminal_epoch_available?&terminal_pair:nullptr,issue);
    s.terminal=true;s.issue=issue;s.flush_after=GetTickCount64()+1000;DingoSDKOverlaySetBmxInputCapture(false);
}
Json epoch_json(const native_coupled::policy::Epoch& e) {
    auto bodies=Json::array();for(const auto& key:e.bodies)bodies.push_back(Json{{"world",key.world},{"index",key.index},{"generation",key.generation}});
    return Json{{"owner_world",e.owner_world},{"bodies",std::move(bodies)},{"token",e.token},{"serial",e.serial},{"time",e.time},{"h",e.seconds}};
}
Json avatar_json(const ca::Result& r) {
    return Json{{"ready",r.ready},{"failed",r.failed},{"published",r.published},{"motion_submitted",r.motion_submitted},
        {"placement_completed",r.placement_completed},{"calibrated",r.calibrated},{"actual_spring_render_verified",r.actual_spring_render_verified},
        {"published_epoch",epoch_json(r.published_epoch)},{"render_epoch",epoch_json(r.render_epoch)},{"issue",r.issue}};
}
Json native_json(const native_coupled::Status& n) {
    const auto& r=n.receipt;return Json{{"issue",n.issue},{"failed",n.failed},{"blocked",n.blocked},{"active",n.active},{"parked",n.parked},
        {"token",n.token},{"submissions",n.submissions},{"rejected",n.rejected},{"normal_force_adoption_enabled",n.force_adoption_enabled},
        {"receipt",Json{{"state",static_cast<unsigned>(r.state)},{"epoch",epoch_json(r.epoch)},{"token",r.token},{"serial",r.serial},{"hop_part",r.hop_part},
            {"attempted",r.attempted},{"completed",r.completed},{"inertia_required",r.inertia_required},{"inertia_attempted",r.inertia_attempted},{"inertia_completed",r.inertia_completed},
            {"verified_principal_inertia",r.verified_principal_inertia},{"inertia_before_wrenches",r.inertia_before_wrenches}}}};
}
Json row_json(const Row& r) {
const auto& p=r.actual;auto bodies=Json::array();
        for(unsigned role=0;role<2;++role){const auto& body=p.roles[role];bodies.push_back(Json{{"world",body.world},{"index",body.body_index},
            {"generation",body.body_generation},{"COM",body.position},{"principalQ",body.orientation},{"native_state",body.native_state},{"modelQ",p.model_orientation[role]},
            {"velocity",body.linear_velocity},{"angular_velocity",body.angular_velocity},{"mass",body.mass},{"I",body.inertia},
            {"actual998",p.maximum_angular_speed[role]},{"actualA68",p.integrates_angular_accumulator[role]}});}
        const auto& input=r.input;Json value{{"epoch",epoch_json(p.epoch)},{"bodies",std::move(bodies)},
            {"phase",r.phase},{"buttons",r.buttons},{"input",Json{{"available",input.available},{"focused",input.focused},{"device",input.device},
                {"left_x",input.left_x},{"left_y",input.left_y},{"right_x",input.right_x},{"right_y",input.right_y},{"pedal",input.pedal},{"brake",input.brake},
                {"left_trigger",input.left_trigger},{"right_trigger",input.right_trigger},{"left_shoulder",input.left_shoulder},{"right_shoulder",input.right_shoulder},
                {"quick_hop",input.quick_hop},{"menu_open",input.menu_open}}},
            {"avatar",avatar_json(r.avatar)},{"controller_committed",r.committed},{"queued",r.queued},{"evaluated",r.evaluated},{"issue",r.issue}};
    value["shape_categories"]=p.categories;value["shape_collides_with"]=p.collides_with;
    value["native_status"]=native_json(r.native);auto commands=Json::array();
    if(r.have_control){for(const auto& c:r.control.command.roles)commands.push_back(Json{{"force",c.force},{"torque",c.torque},{"hop_impulse",c.hop_impulse},{"angular_impulse",c.angular_impulse},{"target_principal_inertia",c.target_principal_inertia}});
        const auto& step=r.control.step;const auto& f=step.frame;value["control"]=Json{{"issue",r.control.issue},{"actual_pair_observed_this_update",r.control.actual_pair_observed_this_update},{"command_available",r.control.command_available},{"evaluated_commands",std::move(commands)},
            {"sample_serial",step.serial},{"sample_time",step.native_simulation_seconds},{"h",step.native_step_seconds},{"grounded",step.grounded},{"actual_support",step.actual_support},
            {"wheel_contacts_sampled",step.wheel_contacts_sampled},{"contact_hit",step.contact_hit},{"contact_points",step.contact_points},{"contact_normals",step.contact_normals},{"wheel_separation",step.wheel_separation},
            {"movement_id",static_cast<unsigned>(f.movement_id)},{"movement_phase",f.movement_phase},{"movement_progress",f.movement_progress},{"steer",f.steer},{"crank_phase",f.crank_phase},
            {"native_before_cleanup_available",r.control.native_before_cleanup_available},{"native_before_cleanup",native_json(r.control.native_before_cleanup)}};
    }else value["control"]=nullptr;return value;
}
Json report() {
    const auto& s=state();auto rows=Json::array();const auto first=s.rows>48?s.rows-48:0;
    for(auto i=first;i<s.rows;++i)rows.push_back(row_json(s.history[i%48]));
    auto entry=Json::array(),active=Json::array(),events=Json::array();const auto entry_first=s.trace.entry_total>48?s.trace.entry_total-48:0;
    for(auto i=entry_first;i<s.trace.entry_total;++i)entry.push_back(row_json(s.trace.entry[i%48]));
    for(std::size_t i=0;i<std::min<std::uint64_t>(s.trace.active_total,640);++i)active.push_back(row_json(s.trace.active[i]));
    for(std::size_t i=0;i<std::min<std::uint64_t>(s.trace.event_total,16);++i)events.push_back(row_json(s.trace.events[i]));
    const auto& b=s.binding_receipt;
    return Json{{"schema",2},{"process_id",GetCurrentProcessId()},{"issue",s.issue},{"attempted",s.attempted},{"observations",s.rows},
        {"activated",s.activated},{"activation_attempted",s.activation_attempted},{"verified_root_height",s.verified_root_height},
        {"entry_ticks",s.entry_ticks},{"entry_publications",s.entry_publications},{"entry_placements",s.entry_placements},{"active_publications",s.active_publications},
        {"force_adoption_enabled",s.backend.force_adoption_enabled},{"native_submissions",s.backend.submissions},{"backend_parked",s.backend.parked},
        {"backend_active",s.backend.active},{"backend_issue",s.backend.issue},{"actual_samples",std::move(rows)},
        {"entry_samples",std::move(entry)},{"active_samples",std::move(active)},{"events",std::move(events)},
        {"entry_samples_total",s.trace.entry_total},{"active_samples_total",s.trace.active_total},{"events_total",s.trace.event_total},
        {"entry_overflow",s.trace.entry_overflow},{"active_overflow",s.trace.active_overflow},{"event_overflow",s.trace.event_overflow},
        {"markers",markers::status()},{"activation_begun_tick_ms",s.activation_begun},{"activation_succeeded_tick_ms",s.activation_succeeded},
        {"backend_before_stop",native_json(s.backend_before_stop)},{"native_rejection_before_cleanup_available",s.bridge_before_stop.native_before_cleanup_available},
        {"native_rejection_before_cleanup",native_json(s.bridge_before_stop.native_before_cleanup)},
        {"avatar_binding_verified",s.neutral_avatar_binding_verified||s.active_avatar_binding_verified},
        {"neutral_avatar_binding_verified",s.neutral_avatar_binding_verified},{"active_avatar_binding_verified",s.active_avatar_binding_verified},
        {"entry_receipt",avatar_json(s.entry_receipt)},{"active_receipt",avatar_json(s.active_receipt)},
        {"rider_pose_diagnostic",rider::coupled_pose_diagnostic()},
        {"completed_pose_epoch",epoch_json(b.completed_pose_epoch)},{"completed_pose_write",b.completed_pose_write},
        {"completed_presentation",Json{{"accepted",b.presentation.accepted},{"one_step_latency",b.presentation.one_step_latency},
            {"sampled_epoch",epoch_json(b.presentation.sampled_epoch)},{"source_lease_revision",b.presentation.source_lease_revision},
            {"render_clock_available",b.presentation.render_clock.available},{"render_serial",b.presentation.render_clock.serial},{"render_time",b.presentation.render_clock.time}}},
        {"actual_spring_applied",b.actual_spring_applied},{"proximal_ready",b.proximal_ready},{"proximal_fit_adjustment",b.proximal_fit_adjustment},
        {"hand_error",b.hand_error},{"foot_error",b.foot_error},{"fall_momentum_transfer_verified",false},
        {"boundary","avatar flags represent completed SDK pose writes and all-part placement receipts; native GPU appearance, force adoption and fall momentum need separate actual qualification"}};
}
void flush() noexcept {
    auto& s=state();if(!s.terminal||s.exported||GetTickCount64()<s.flush_after)return;s.exported=true;
    try {
        constexpr wchar_t exact[]=L"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-coupled-test";
        const auto root=user_data::override_root();if(!root||root->native()!=exact||!user_data::runtime_development_selected(L"bmx-user-data-coupled-test",exact))return;
        const auto name="coupled-ride-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(s.created);
        const auto temp=*root/(name+".tmp"),path=*root/(name+".json");std::ofstream f(temp,std::ios::binary);
        f.exceptions(std::ios::failbit|std::ios::badbit);f<<report().dump(2)<<'\n';f.close();std::filesystem::rename(temp,path);
    }catch(...){logging::write(logging::Level::warning,logging::Channel::skater,"Coupled ride deferred diagnostic export failed.");}
}
void tick(std::uintptr_t base,std::uintptr_t client,bool gameplay_ready) noexcept {
    tick_player_startup(base,client,gameplay_ready);auto& s=state();flush();
    if(s.terminal){const auto placed=world::tick(base,client,{},0);(void)placed;return;}
    try {
        // Reuse the existing sampler once per tick, including its unchanged
        // raw sticks/buttons and Dpad handling. No injected controller input.
        const auto input=overlay::read_bmx_input();const auto ui=multiplayer::read_game_ui_state(base);const auto mp=multiplayer::model();
        auto frame=read_native_frame(base,client);const bool session_gates=!s.abort&&gameplay_ready&&input.available&&input.focused&&input.overlay_ready&&
            !input.menu_blocked&&!input.capture_blocked&&!input.menu_requested&&!ui.in_menu&&ui.available&&!mp.active&&!mp.lobby_joining;
        const bool gates=session_gates&&frame.available&&frame.offboard&&frame.timing_available;
        if(s.attempted){
            if(!gates||input.dismount_requested){finish(base,frame,!gates?"private_coupled_ride_gate_lost":"private_coupled_dismount");return;}
            DingoSDKOverlaySetBmxInputCapture(true);
            if(!s.activated){
                if(GetTickCount64()>=s.entry_finish_after){finish(base,frame,"private_coupled_fifteen_second_entry_limit");return;}
                const auto pair=native_coupled::observe(base,frame);const auto actual=af::observe(pair);
                if(!ab::valid(actual)){finish(base,frame,"private_coupled_parked_observation_invalid");return;}
                mount_pose::Seed seed;bool changed{},rejected{};MountSearchSupport observed;
                const auto forward=native_coupled::policy::rotate(actual.bike_orientation,{0,0,1});const float yaw=std::atan2(forward[0],forward[2]);
                // The retained-placement flag belongs to the OLD one-body
                // parked lease. This new pair supplies its own world/wrapper.
                const MountSearchSupport expected{pair.epoch.owner_world[5],pair.epoch.owner_world[7]};
                const bool refreshed=fresh_mount_seed(base,frame,actual.bike_root,yaw,seed,changed,rejected,expected,observed);
                if(changed){finish(base,frame,"private_coupled_entry_support_owner_changed");return;}
                if(!refreshed)seed={};
                ++s.entry_ticks;s.entry_receipt=ca::tick(base,frame,pair,seed,s.verified_root_height,neutral(input));
                s.entry_publications+=s.entry_receipt.published;s.entry_placements+=s.entry_receipt.placement_completed;
                s.binding_receipt=rider::coupled_avatar_binding_status();
                remember(pair,input,s.entry_receipt,"parked_neutral_entry",s.entry_receipt.issue);
                if(s.entry_receipt.failed){finish(base,frame,s.entry_receipt.issue);return;}
                if(!s.entry_receipt.ready)return;
                s.neutral_avatar_binding_verified=s.entry_receipt.actual_spring_render_verified;
                s.activation_attempted=true;
                // This remains an independent backend hard gate. A rejected
                // activation retains the real completed entry receipt.
                s.activation_begun=GetTickCount64();const bool activated=coupled_ride::activate_prepared(base,frame);s.activation_succeeded=GetTickCount64();
                if(!activated){finish(base,frame,coupled_ride::status().issue);return;}
                const auto activated_owner=read_native_frame(base,client);const auto activated_pair=native_coupled::observe(base,activated_owner);
                const bool activation_qualified=af::owner_epoch(activated_owner,activated_pair.epoch)&&native_coupled::trial_policy::qualified(activated_pair,true);
                markers::activation(activated_pair,activation_qualified,s.activation_begun,s.activation_succeeded,s.entry_receipt.ready,s.entry_receipt.actual_spring_render_verified);
                Row activation_event;activation_event.actual=activated_pair;activation_event.input=raw_input(input);activation_event.buttons=input.buttons;activation_event.phase="activation_success";activation_event.issue=markers::state().activation_issue;activation_event.native=native_coupled::status();s.trace.event(activation_event);
                s.activated=true;s.finish_after=s.activation_succeeded+10000;s.issue="active_actual_pair_and_spring_binding";return;
            }
            if(GetTickCount64()>=s.finish_after){finish(base,frame,"private_coupled_ten_second_active_limit");return;}
            const auto previous=rider::coupled_avatar_binding_status();
            const auto result=coupled_ride::update(base,frame,raw_input(input));
            if(!result.physics_active){remember(result.actual_pair,input,{},"active_controller",result.issue,result.controller_committed,result.queued,result.evaluated,&result);finish(base,frame,result.issue);return;}
            s.active_receipt=ca::bind_active(base,frame,result.actual_pair,result.step.frame);
            s.binding_receipt=rider::coupled_avatar_binding_status();
            remember(result.actual_pair,input,s.active_receipt,"active_actual_spring",s.active_receipt.issue,result.controller_committed,result.queued,result.evaluated,&result);
            if(s.active_receipt.failed||!s.active_receipt.published){finish(base,frame,s.active_receipt.issue);return;}
            if(s.active_publications&&previous.actual_spring_applied&&previous.completed_pose_write&&previous.proximal_ready&&
               previous.completed_pose_epoch==s.active_receipt.render_epoch&&previous.hand_error<.003f&&previous.foot_error<.003f)
                s.active_avatar_binding_verified=true;
            ++s.active_publications;return;
        }
        if(GetTickCount64()-s.created>=60000){finish(base,frame,"private_coupled_sixty_second_startup_limit");return;}
        const auto& r=runtime();const bool idle=!r.active.load()&&!r.pending&&!r.stepping_off&&!r.bike_parked&&!r.bike_crashed&&!r.bike_carried&&!r.chassis_started&&
            !r.rehearsal&&!r.board_switch.active()&&!r.setdown_search.active()&&!overlay::detail::bmx_input_capture_active()&&!overlay::detail::bmx_interaction_capture_active()&&
            world::snapshot().live_parts==0;
        if(!session_gates||!idle||!neutral(input)){s.quiet_since=0;return;}
        if(!s.quiet_since){s.quiet_since=GetTickCount64();return;}if(GetTickCount64()-s.quiet_since<500)return;
        const auto initialized=startup_owner::initialize<NativeFrame>(true,[&](){return enable_native_motion(base);},[&](){return read_native_frame(base,client);});
        if(!initialized.enabled||!initialized.observed)return;frame=initialized.frame;
        if(!frame.available||!frame.offboard||!frame.standable||!frame.timing_available)return;
        const auto lease=native_motion_lease_status();if(!lease.available||lease.active)return;
        const float yaw=std::atan2(frame.transform[8],frame.transform[10]);
        const auto floor=probe_local_surface({frame.position[0],frame.position[1]+1,frame.position[2]},
            {frame.position[0],frame.position[1]-3,frame.position[2]}, {},frame.entity);if(!floor.available||!floor.hit)return;
        const float height=frame.position[1]-floor.position[1];if(!std::isfinite(height)||height<0||height>2)return;
        mount_pose::Seed seed;bool changed{},rejected{};MountSearchSupport observed;
        if(!fresh_mount_seed(base,frame,{frame.position[0],floor.position[1],frame.position[2]},yaw,seed,changed,rejected,{},observed)||changed)return;
        s.owner=frame;s.attempted=true;s.verified_root_height=height;s.entry_finish_after=GetTickCount64()+15000;
        ca::reset();
        if(!coupled_ride::prepare(base,frame,seed,height)){finish(base,frame,coupled_ride::status().issue);return;}
        DingoSDKOverlaySetBmxInputCapture(true);s.issue="parked_neutral_entry_pending";
    }catch(...){const auto frame=read_native_frame(base,client);finish(base,frame,"private_coupled_ride_exception");}
}
}
