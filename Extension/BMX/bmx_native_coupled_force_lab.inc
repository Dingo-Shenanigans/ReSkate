// Separate exact-profile one-shot private operator. No controller/avatar lease,
// input injection, source simulation or ordinary mount path is entered here.
namespace coupled_force_lab {
namespace fp=native_coupled::force_probe;
struct Lab {bool initialized{},selected{},started{},terminal{},exported{};std::atomic<bool> abort{};
    ULONGLONG created{},quiet_since{},flush_after{};const char* issue="waiting_for_quiet_fresh_offboard_owner";
    const char* last_stage="not_polled";std::array<unsigned,10> stage_counts{};NativeFrame last_native;
    std::array<mount_pose::Seed,2> last_support;bool last_gates{},last_neutral{},last_idle{},last_enable{},last_lease_available{},last_lease_active{};
    unsigned last_role=0xffffffffU,last_shape=0xffffffffU,last_axis=0xffffffffU;
    const char* last_floor_issue="not_queried";const char* last_clear_issue="not_queried";
    bool last_clear_available{},last_clear_hit{};std::uintptr_t last_clear_world{},last_clear_context{};
    fp::Vec last_clear_from{},last_clear_to{};
};
Lab& state(){static auto* p=new Lab;return *p;}
void stage(unsigned index,const char* value){auto& s=state();s.last_stage=value;if(index<s.stage_counts.size()&&s.stage_counts[index]<65535)++s.stage_counts[index];}
bool selected() noexcept {auto& s=state();if(!s.initialized){s.initialized=true;s.selected=fp::selected();s.created=GetTickCount64();}return s.selected;}
void request_stop() noexcept {state().abort.store(true,std::memory_order_release);native_coupled::cancel();}
void finish(const char* issue) noexcept {auto& s=state();s.terminal=true;s.issue=issue;s.flush_after=GetTickCount64()+1000;}
Json sample_json(const fp::Sample& row) {
    auto roles=Json::array();for(unsigned role=0;role<2;++role)roles.push_back(Json{{"world",row.epoch.bodies[role].world},{"index",row.epoch.bodies[role].index},
        {"generation",row.epoch.bodies[role].generation},{"COM",row.position[role]},{"principalQ",row.principal_orientation[role]},
        {"velocity",row.linear[role]},{"angular_velocity",row.angular[role]},{"mass",row.mass[role]},{"I",row.inertia[role]},
        {"actual998",row.maximum_angular_speed[role]},{"allocated998",row.allocated_maximum_angular_speed[role]},
        {"actualA68",row.integrates_angular_accumulator[role]},{"proxy_plane_clearance",row.clearance[role]}});
    return Json{{"owner_world",row.epoch.owner_world},{"token",row.epoch.token},{"serial",row.epoch.serial},{"time",row.epoch.time},{"h",row.epoch.seconds},
        {"roles",std::move(roles)},{"shape_categories",row.categories},{"shape_collides_with",row.collides_with}};
}
void flush() noexcept {auto& s=state();if(!s.terminal||s.exported||GetTickCount64()<s.flush_after)return;s.exported=true;
    try {
        constexpr wchar_t exact[]=L"C:\\Users\\Jeffr\\Desktop\\reskate\\build\\bmx-user-data-coupled-test";const auto root=user_data::override_root();
        if(!root||root->native()!=exact||!user_data::runtime_development_selected(L"bmx-user-data-coupled-test",exact))return;
        const auto report=fp::current_report();auto actual=Json::array(),commands=Json::array();
        for(unsigned i=0;i<report.count;++i)actual.push_back(sample_json(report.actual[i]));
        for(unsigned i=0;i<report.completed;++i){const auto& receipt=report.receipts[i];const auto& cmd=report.commands[i];
            auto receipt_bodies=Json::array();for(const auto& body:receipt.epoch.bodies)
                receipt_bodies.push_back(Json{{"world",body.world},{"index",body.index},{"generation",body.generation}});
            auto roles=Json::array();for(unsigned role=0;role<2;++role)roles.push_back(Json{{"force",cmd.roles[role].force},{"torque",cmd.roles[role].torque}});
            commands.push_back(Json{{"index",i},{"command_roles",std::move(roles)},{"receipt_state",static_cast<unsigned>(receipt.state)},
                {"receipt_owner_world",receipt.epoch.owner_world},{"receipt_bodies",std::move(receipt_bodies)},{"receipt_token",receipt.token},{"receipt_serial",receipt.serial},
                {"receipt_time",receipt.epoch.time},{"receipt_h",receipt.epoch.seconds},{"attempted",receipt.attempted},{"completed",receipt.completed},
                {"I_required",receipt.inertia_required},{"I_attempted",receipt.inertia_attempted},{"I_completed",receipt.inertia_completed},
                {"I_before_wrenches",receipt.inertia_before_wrenches},{"linear_response",report.linear_response[i]},{"expected_linear",report.expected_linear[i]},
                {"angular_response",report.angular_response[i]},{"expected_angular",report.expected_angular[i]},{"actual_quaternion_delta",report.quaternion_delta[i]}});
        }
        Json rejected=nullptr;if(report.rejected_available){rejected=sample_json(report.rejected);rejected["role"]=report.rejected_role;
            rejected["check"]=report.rejected_check;rejected["value"]=report.rejected_value;rejected["limit"]=report.rejected_limit;}
        const auto backend=native_coupled::status();auto support=Json::array();
        for(const auto& seed:s.last_support)support.push_back(Json{{"ready",seed.ready},{"root",seed.root},{"orientation",seed.orientation},{"up",seed.up},
            {"pitch",seed.pitch},{"roll",seed.roll},{"world",seed.request.world},{"context",seed.request.context},
            {"serial",seed.request.serial},{"time",seed.request.simulation_time}});
        const Json readiness{{"last_stage",s.last_stage},{"stage_counts",s.stage_counts},{"session_gates",s.last_gates},{"neutral",s.last_neutral},{"idle",s.last_idle},
            {"enable_succeeded",s.last_enable},{"native_available",s.last_native.available},{"native_issue",s.last_native.issue},
            {"native_offboard",s.last_native.offboard},{"native_standable",s.last_native.standable},{"native_timing",s.last_native.timing_available},
            {"native_serial",s.last_native.serial},{"native_time",s.last_native.simulation_time},{"native_position",s.last_native.position},
            {"lease_available",s.last_lease_available},{"lease_active",s.last_lease_active},{"last_role",s.last_role},{"last_shape",s.last_shape},{"last_axis",s.last_axis},
            {"last_floor_issue",s.last_floor_issue},{"fresh_support_seeds",std::move(support)},{"last_clear_issue",s.last_clear_issue},
            {"last_clear_available",s.last_clear_available},{"last_clear_hit",s.last_clear_hit},{"last_clear_world",s.last_clear_world},{"last_clear_context",s.last_clear_context},
            {"last_clear_from",s.last_clear_from},{"last_clear_to",s.last_clear_to},
            {"backend_attempted",backend.attempted},{"backend_prepared",backend.prepared},{"backend_failed",backend.failed},{"backend_blocked",backend.blocked},
            {"backend_allocations",backend.allocation_attempts},{"backend_bodies",backend.live_bodies},{"backend_shapes",backend.live_shapes},{"backend_issue",backend.issue},
            {"backend_admission_issue",backend.calibration_admission_issue},{"backend_admission_rejections",backend.calibration_admission_rejections}};
        const Json value{{"schema",1},{"process_id",GetCurrentProcessId()},{"operator_issue",s.issue},{"report_issue",report.issue},{"startup_readiness",readiness},
            {"phase",static_cast<unsigned>(report.phase)},{"parked",report.parked},{"completed_groups",report.completed},
            {"bounded_small_application_candidate",report.bounded_application_candidate},{"normal_force_adoption_enabled",native_coupled::status().force_adoption_enabled},
            {"actual_samples",std::move(actual)},{"commands_and_receipts",std::move(commands)},{"rejected_sample",std::move(rejected)},
            {"boundary","six-epoch small native force/torque application diagnostic; no avatar motion, joint controller, handling-parity, full torque/clamp or world-contact proof"}};
        const auto name="coupled-force-probe-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(s.created);
        const auto temp=*root/(name+".tmp"),path=*root/(name+".json");std::ofstream file(temp,std::ios::binary);
        file.exceptions(std::ios::failbit|std::ios::badbit);file<<value.dump(2)<<'\n';file.close();std::filesystem::rename(temp,path);
    }catch(...){logging::write(logging::Level::warning,logging::Channel::skater,"Coupled force probe export failed.");}
}
bool clear_region(const NativeFrame& owner,const std::array<mount_pose::Seed,2>& support) {
    auto& s=state();stage(7,"clear_region");
    for(unsigned role=0;role<2;++role){const auto& seed=support[role];
        s.last_role=role;const auto plan=fp::placement(collision_envelope::capsules,role,seed.root,seed.orientation,seed.up);
        if(!plan.valid){s.last_clear_issue="placement_invalid";return false;}
        const auto world_point=[&](std::array<float,3> authored){const auto offset=chassis::rotate_unit(seed.orientation,authored);
            return fp::Vec{plan.root[0]+offset[0],plan.root[1]+offset[1],plan.root[2]+offset[2]};};
        const auto clear=[&](auto a,auto b){const auto hit=probe_local_surface(a,b,{},owner.entity);
            s.last_clear_from=a;s.last_clear_to=b;s.last_clear_available=hit.available;s.last_clear_hit=hit.hit;
            s.last_clear_world=hit.world;s.last_clear_context=hit.context;
            s.last_clear_issue=!hit.available?"ray_unavailable":hit.world!=seed.request.world?"ray_world_changed":hit.context!=seed.request.context?"ray_context_changed":hit.hit?"ray_hit":"clear";
            return hit.available&&!hit.hit&&hit.world==seed.request.world&&hit.context==seed.request.context;};
        for(unsigned index=role?5u:0u;index<(role?8u:5u);++index){const auto& capsule=collision_envelope::capsules[index];
            s.last_shape=index;s.last_axis=0xffffffffU;
            if(!clear(world_point(capsule.a),world_point(capsule.b)))return false;
            for(unsigned axis=0;axis<3;++axis){s.last_axis=axis;fp::Vec a{},b{};for(unsigned i=0;i<3;++i)a[i]=b[i]=(capsule.a[i]+capsule.b[i])*.5f;
                a[axis]-=capsule.radius;b[axis]+=capsule.radius;if(!clear(world_point(a),world_point(b)))return false;}
        }
    }return true; //32 finite witnesses, not a volume sweep/collision guarantee.
}
void tick(std::uintptr_t base,std::uintptr_t client,bool gameplay_ready) noexcept {
    tick_player_startup(base,client,gameplay_ready);auto& s=state();flush();if(s.terminal)return;
    try {
        const auto input=overlay::read_bmx_input();const auto ui=multiplayer::read_game_ui_state(base);const auto mp=multiplayer::model();
        auto frame=read_native_frame(base,client);s.last_native=frame;const auto& r=runtime();
        const bool neutral=input.available&&input.focused&&input.overlay_ready&&!input.menu_blocked&&!input.capture_blocked&&!input.menu_requested&&!input.capturing&&
            !input.buttons&&!input.quick_hop&&!input.dismount_requested&&!input.left_shoulder&&!input.right_shoulder&&
            std::abs(input.left_x)<.05f&&std::abs(input.left_y)<.05f&&std::abs(input.right_x)<.05f&&std::abs(input.right_y)<.05f&&
            input.pedal<.05f&&input.brake<.05f&&input.left_trigger<.05f&&input.right_trigger<.05f;
        const bool idle=!r.active.load()&&!r.pending&&!r.stepping_off&&!r.bike_parked&&!r.bike_crashed&&!r.bike_carried&&!r.chassis_started&&!r.rehearsal&&
            !r.board_switch.active()&&!r.setdown_search.active()&&!overlay::detail::bmx_input_capture_active()&&!overlay::detail::bmx_interaction_capture_active()&&world::snapshot().live_parts==0;
        const bool gates=!s.abort&&gameplay_ready&&neutral&&idle&&ui.available&&!ui.in_menu&&!mp.active&&!mp.lobby_joining&&fp::selected();
        s.last_gates=gates;s.last_neutral=neutral;s.last_idle=idle;
        if(s.started){stage(9,"running_actual_probe");const auto report=fp::poll(base,frame,gates);if(report.phase!=fp::Phase::running)finish(report.issue);return;}
        if(GetTickCount64()-s.created>=60000){finish("force_probe_sixty_second_startup_deadline");return;}
        stage(0,"quiet_gameplay_gates");if(!gates){s.quiet_since=0;return;}
        stage(1,"quiet_dwell");if(!s.quiet_since){s.quiet_since=GetTickCount64();return;}if(GetTickCount64()-s.quiet_since<500)return;
        stage(2,"initialize_native_source");
        const auto initialized=startup_owner::initialize<NativeFrame>(true,[&](){return enable_native_motion(base);},[&](){return read_native_frame(base,client);});
        s.last_enable=initialized.enabled;if(!initialized.enabled||!initialized.observed)return;frame=initialized.frame;s.last_native=frame;
        stage(3,"fresh_native_owner");if(!frame.available||!frame.offboard||!frame.standable||!frame.timing_available)return;
        stage(4,"idle_native_lease");const auto lease=native_motion_lease_status();s.last_lease_available=lease.available;s.last_lease_active=lease.active;
        if(!lease.available||lease.active)return;
        const float yaw=std::atan2(frame.transform[8],frame.transform[10]);std::array<mount_pose::Seed,2> support;
        for(unsigned role=0;role<2;++role){const float side=role?1.05f:-1.05f;
            s.last_role=role;stage(5,"fresh_floor");
            const fp::Vec target{frame.position[0]+2*std::sin(yaw)+side*std::cos(yaw),frame.position[1],frame.position[2]+2*std::cos(yaw)-side*std::sin(yaw)};
            const auto floor=probe_local_surface({target[0],target[1]+1,target[2]},{target[0],target[1]-3,target[2]}, {},frame.entity);
            s.last_floor_issue=!floor.available?"floor_unavailable":!floor.hit?"floor_clear":"floor_hit";if(!floor.available||!floor.hit)return;
            bool changed{},rejected{};MountSearchSupport observed;
            stage(6,"fresh_support_seed");const bool ready=fresh_mount_seed(base,frame,{target[0],floor.position[1],target[2]},yaw,support[role],changed,rejected,{},observed);
            s.last_support[role]=support[role];if(!ready||changed)return;
        }
        if(!clear_region(frame,support))return;
        stage(8,"begin_verified_native_probe");if(fp::begin(base,frame,support)){s.started=true;return;}
        const auto report=fp::current_report();if(report.phase==fp::Phase::failed)finish(report.issue);
    }catch(...){const auto report=fp::poll(base,read_native_frame(base,client),false);(void)report;finish("force_probe_operator_exception");}
}
}
