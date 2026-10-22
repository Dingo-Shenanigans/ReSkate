// Included inside bmx_runtime.cpp's private namespace after fresh_mount_seed.
// Exact profile, finite one-shot startup experiment; no console/input injection,
// avatar motion, rider retargeting, joint forces, or controller force adoption.
namespace coupled_lab {
namespace nc=native_coupled;
namespace cal=nc::calibration;
struct Lab {
    bool selected{},initialized{},started{},terminal{},exported{};std::atomic<bool> abort{};
    cal::Kind kind{};ULONGLONG created{},quiet_since{},deadline{},flush_after{};
    NativeFrame owner;std::uintptr_t base{};const char* issue="waiting_for_native_offboard_owner";
    std::array<mount_pose::Seed,2> support;
};
Lab& state(){static auto* value=new Lab;return *value;}
bool selected() noexcept {
    auto& s=state();if(s.initialized)return s.selected;s.initialized=true;
    if(!nc::enabled())return false;
    wchar_t mode[16]{};const auto size=GetEnvironmentVariableW(L"RESKATE_BMX_COUPLED_CALIBRATION",mode,16);
    if(size==4&&std::wstring_view(mode,size)==L"pair")s.kind=cal::Kind::pair_filter;
    else if(size==5&&std::wstring_view(mode,size)==L"world")s.kind=cal::Kind::world_contact;
    else if(size==7&&std::wstring_view(mode,size)==L"contact")s.kind=cal::Kind::world_contact_only;
    else if(size==13&&std::wstring_view(mode,size)==L"pair-positive")s.kind=cal::Kind::pair_positive;
    else return false;
    s.selected=true;s.created=GetTickCount64();s.deadline=s.created+60000;return true;
}
void finish(const char* issue) noexcept {auto& s=state();s.terminal=true;s.issue=issue;s.flush_after=GetTickCount64()+1000;}
void request_stop() noexcept {state().abort.store(true,std::memory_order_release);nc::cancel();}
void flush() noexcept {
    auto& s=state();if(!s.terminal||s.exported||GetTickCount64()<s.flush_after)return;s.exported=true;
    try {
        constexpr wchar_t exact_root[]=L"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-coupled-test";
        const auto root=user_data::override_root();
        if(!root||root->native()!=exact_root||!user_data::runtime_development_selected(L"bmx-user-data-coupled-test",exact_root))return;
        const auto report=nc::calibration_report();const auto status=nc::status();Json rows=Json::array();
        for(unsigned i=0;i<report.count;++i){const auto& row=report.samples[i];
            auto bodies=Json::array();for(unsigned role=0;role<2;++role)bodies.push_back(Json{
                {"world",row.epoch.bodies[role].world},{"index",row.epoch.bodies[role].index},{"generation",row.epoch.bodies[role].generation},
                {"position",row.position[role]},{"linear_velocity",row.linear[role]},{"angular_velocity",row.angular[role]},{"proxy_plane_clearance",row.clearance[role]}});
            for(unsigned role=0;role<2;++role){bodies[role]["mass"]=row.mass[role];bodies[role]["principal_inertia"]=row.inertia[role];bodies[role]["principal_orientation"]=row.principal_orientation[role];bodies[role]["maximum_angular_speed_property"]=row.maximum_angular_speed[role];bodies[role]["allocation_angular_limit_before_setup"]=row.allocated_maximum_angular_speed[role];bodies[role]["integrates_angular_accumulator"]=row.integrates_angular_accumulator[role];}
            rows.push_back(Json{{"serial",row.epoch.serial},{"simulation_time",row.epoch.time},{"seconds",row.epoch.seconds},
                {"token",row.epoch.token},{"owner_world",row.epoch.owner_world},{"roles",std::move(bodies)},
                {"shape_categories",row.categories},{"shape_collides_with",row.collides_with}});
        }
        auto seeds=Json::array();for(const auto& seed:s.support)seeds.push_back(Json{{"ready",seed.ready},{"root",seed.root},
            {"up",seed.up},{"orientation",seed.orientation},{"pitch",seed.pitch},{"roll",seed.roll},{"world",seed.request.world},{"context",seed.request.context},
            {"serial",seed.request.serial},{"simulation_time",seed.request.simulation_time}});
        Json rejected=nullptr;
        if(report.rejected_sample_available){const auto& row=report.rejected_sample;auto roles=Json::array();
            for(unsigned role=0;role<2;++role)roles.push_back(Json{{"world",row.epoch.bodies[role].world},{"index",row.epoch.bodies[role].index},
                {"generation",row.epoch.bodies[role].generation},{"position",row.position[role]},{"linear_velocity",row.linear[role]},
                {"angular_velocity",row.angular[role]},{"proxy_plane_clearance",row.clearance[role]},
                {"mass",row.mass[role]},{"principal_inertia",row.inertia[role]},{"principal_orientation",row.principal_orientation[role]},
                {"maximum_angular_speed_property",row.maximum_angular_speed[role]},{"integrates_angular_accumulator",row.integrates_angular_accumulator[role]}});
            rejected=Json{{"accepted",false},{"cap",report.rejected_cap},{"role",report.rejected_role},{"value",report.rejected_value},{"limit",report.rejected_limit},
                {"owner_world",row.epoch.owner_world},{"token",row.epoch.token},{"serial",row.epoch.serial},{"simulation_time",row.epoch.time},{"seconds",row.epoch.seconds},
                {"roles",std::move(roles)},{"shape_categories",row.categories},{"shape_collides_with",row.collides_with}};
        }
        auto placement=Json::array();
        if(cal::world_kind(s.kind))for(unsigned role=0;role<2;++role){const auto& seed=s.support[role];
            const auto plan=cal::world_placement(collision_envelope::capsules,role,seed.root,seed.orientation,seed.up);
            placement.push_back(Json{{"role",role},{"valid_plan",plan.valid},{"target_proxy_plane_clearance",cal::world_fixture_clearance},
                {"original_proxy_plane_clearance",plan.original_clearance},{"normal_translation",plan.translation},{"requested_root",plan.root},
                {"actual_initial_proxy_plane_clearance",report.count?Json(report.samples[0].clearance[role]):Json(nullptr)}});}
        const Json value{{"schema",1},{"process_id",GetCurrentProcessId()},{"mode",s.kind==cal::Kind::pair_filter?"pair":s.kind==cal::Kind::pair_positive?"pair-positive":s.kind==cal::Kind::world_contact_only?"contact":"world"},
            {"issue",s.issue},{"started",s.started},{"phase",static_cast<unsigned>(report.phase)},{"report_issue",report.issue},
            {"samples",std::move(rows)},{"rejected_sample",std::move(rejected)},{"fresh_support_seeds",std::move(seeds)},
            {"equal_clearance_world_fixture",std::move(placement)},
            {"falling",report.falling},{"contact_candidate",report.contact_candidate},{"settled_samples",report.settled},{"pair_response",report.pair_response},
            {"bounded_response_candidate",report.neutral_response_candidate},{"parked_proved",report.parked},
            {"allocation_attempts",status.allocation_attempts},{"owned_bodies",status.live_bodies},{"owned_shapes",status.live_shapes},
            {"controller_force_submissions",status.submissions},{"force_adoption_enabled",status.force_adoption_enabled},
            {"admission_issue",status.calibration_admission_issue},{"admission_rejections",status.calibration_admission_rejections},{"rejected_role",status.calibration_rejected_role},
            {"boundary","bounded neutral actual-body evidence; world/contact use equal08m lowest-proxy drop clearance; contact mode proves no settling, upright balance or riding stability; no automatic collision proof, handling adoption, rider movement or joint-force claim"}};
        const auto name="coupled-calibration-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(s.created);
        const auto temporary=*root/(name+".tmp"),path=*root/(name+".json");
        std::ofstream file(temporary,std::ios::binary);file.exceptions(std::ios::failbit|std::ios::badbit);file<<value.dump(2)<<'\n';file.close();
        std::filesystem::rename(temporary,path);
        logging::log(logging::Level::info,logging::Channel::skater,"Coupled neutral calibration exported: {}",path.string());
    }catch(...){logging::write(logging::Level::warning,logging::Channel::skater,"Coupled neutral calibration export failed.");}
}
bool clear_initial_region(const NativeFrame& frame,const std::array<mount_pose::Seed,2>& seeds,cal::Kind kind) {
    const auto shift=cal::overlap_shift(collision_envelope::capsules);
    for(unsigned role=0;role<2;++role){
        const auto& seed=seeds[role];auto root=seed.root;
        if(cal::world_kind(kind)){const auto plan=cal::world_placement(collision_envelope::capsules,role,root,seed.orientation,seed.up);
            if(!plan.valid)return false;root=plan.root;}
        else for(unsigned axis=0;axis<3;++axis)root[axis]+=seed.up[axis]*.5f;
        if(role&&!cal::world_kind(kind)){const auto offset=chassis::rotate_unit(seed.orientation,shift);for(unsigned axis=0;axis<3;++axis)root[axis]+=offset[axis];}
        const auto world_point=[&](std::array<float,3> point){point=chassis::rotate_unit(seed.orientation,point);for(unsigned axis=0;axis<3;++axis)point[axis]+=root[axis];return point;};
        const auto clear=[&](std::array<float,3> a,std::array<float,3> b){const auto hit=probe_local_surface(a,b,{},frame.entity);
            return hit.available&&!hit.hit&&hit.world==seed.request.world&&hit.context==seed.request.context;};
        for(unsigned index=role?5u:0u;index<(role?8u:5u);++index){const auto& shape=collision_envelope::capsules[index];
            if(!clear(world_point(shape.a),world_point(shape.b)))return false;
            for(unsigned axis=0;axis<3;++axis){std::array<float,3> a{},b{};
                for(unsigned i=0;i<3;++i)a[i]=b[i]=(shape.a[i]+shape.b[i])*.5f;
                a[axis]-=shape.radius;b[axis]+=shape.radius;if(!clear(world_point(a),world_point(b)))return false;
            }
        }
    }
    return true; //32 finite ray witnesses, not complete volume-clearance proof.
}
void tick(std::uintptr_t base,std::uintptr_t client,bool gameplay_ready) noexcept {
    tick_player_startup(base,client,gameplay_ready); // preserve ONLY the existing separately opted-in/requested bootstrap
    auto& s=state();auto& r=runtime();s.base=base;flush();if(s.terminal)return;
    try {
        const auto input=overlay::read_bmx_input();const auto ui=multiplayer::read_game_ui_state(base);const auto mp=multiplayer::model();
        const bool quiet=input.available&&input.focused&&input.overlay_ready&&!input.menu_blocked&&!input.capture_blocked&&!input.menu_requested&&!input.capturing&&
            !input.buttons&&!input.quick_hop&&!input.dismount_requested&&!input.left_shoulder&&!input.right_shoulder&&
            std::abs(input.left_x)<.05f&&std::abs(input.left_y)<.05f&&std::abs(input.right_x)<.05f&&std::abs(input.right_y)<.05f&&
            input.pedal<.05f&&input.brake<.05f&&input.left_trigger<.05f&&input.right_trigger<.05f;
        const bool idle=!r.active.load()&&!r.pending&&!r.stepping_off&&!r.bike_parked&&!r.bike_crashed&&!r.bike_carried&&!r.chassis_started&&!r.rehearsal&&
            !r.board_switch.active()&&!r.setdown_search.active()&&!overlay::detail::bmx_input_capture_active()&&!overlay::detail::bmx_interaction_capture_active()&&world::snapshot().live_parts==0;
        const bool gates=!s.abort&&gameplay_ready&&quiet&&idle&&ui.available&&!ui.in_menu&&!mp.active&&!mp.lobby_joining&&nc::enabled();
        if(s.started){const auto frame=read_native_frame(base,client);const auto report=nc::poll_calibration(base,frame,gates);
            if(report.phase!=cal::Phase::running)finish(report.issue);return;}
        if(GetTickCount64()>=s.deadline){finish("neutral_startup60second_deadline");return;}
        if(!gates){s.quiet_since=0;return;}
        if(!s.quiet_since){s.quiet_since=GetTickCount64();return;}if(GetTickCount64()-s.quiet_since<500)return;
        if(!enable_native_motion(base))return;
        const auto frame=read_native_frame(base,client);if(!frame.available||!frame.offboard||!frame.standable||!frame.timing_available)return;
        const auto lease=native_motion_lease_status();if(!lease.available||lease.active)return;
        const float yaw=std::atan2(frame.transform[8],frame.transform[10]);std::array<mount_pose::Seed,2> seeds;
        for(unsigned role=0;role<2;++role){const float side=!cal::world_kind(s.kind)?0:role?1.05f:-1.05f;
            const std::array<float,3> target{frame.position[0]+2*std::sin(yaw)+side*std::cos(yaw),frame.position[1],frame.position[2]+2*std::cos(yaw)-side*std::sin(yaw)};
            const auto floor=probe_local_surface({target[0],target[1]+1,target[2]},{target[0],target[1]-3,target[2]}, {},frame.entity);
            if(!floor.available||!floor.hit)return;
            bool changed{},rejected{};MountSearchSupport observed;
            if(!fresh_mount_seed(base,frame,{target[0],floor.position[1],target[2]},yaw,seeds[role],changed,rejected,{},observed))return;
            if(!cal::world_kind(s.kind)){seeds[1]=seeds[0];break;}
        }
        if(!clear_initial_region(frame,seeds,s.kind))return;
        s.support=seeds;s.owner=frame;
        if(nc::begin_calibration(base,frame,s.kind,seeds)){s.started=true;return;}
        const auto status=nc::status();if(status.failed||status.blocked||nc::calibration_report().phase==cal::Phase::failed)finish(status.issue);
    }catch(...){if(s.started){const auto frame=read_native_frame(base,client);(void)nc::poll_calibration(base,frame,false);}finish("neutral_runtime_exception");}
}
}
