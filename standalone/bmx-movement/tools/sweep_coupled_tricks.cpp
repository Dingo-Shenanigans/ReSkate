// Headless bounded physical exercise; no controller/window/native game access.
#include <bmx/suspension.hpp>
#include <bmx/joint_actuation.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv) {
    try {
        if(argc<2||argc>16||argc%2)throw std::runtime_error("Usage: sweep_coupled_tricks fresh-output-directory [--id 3] [--torque-limit 40000] [--force-limit 150000] [--hop-speed 7.2] [--hop-at 0.77] [--selection-delay 0.22] [--selection-hold 0.41]");
        int selected_id=-1;double torque_override=-1,force_override=-1,hop_override=-1,hop_at=1.5,selection_delay=.24,selection_hold=.40;
        for(int i=2;i<argc;i+=2) {
            const std::string flag=argv[i];const double value=std::stod(argv[i+1]);
            if(!std::isfinite(value))throw std::runtime_error("Nonfinite override");
            if(flag=="--id"&&value>=0&&value<=79&&value==int(value))selected_id=int(value);
            else if(flag=="--torque-limit"&&value>=4000&&value<=150000)torque_override=value;
            else if(flag=="--force-limit"&&value>=85000&&value<=150000)force_override=value;
            else if(flag=="--hop-speed"&&value>=4&&value<=12)hop_override=value;
            else if(flag=="--hop-at"&&value>=.5&&value<=2)hop_at=value;
            else if(flag=="--selection-delay"&&value>=.12&&value<=.4)selection_delay=value;
            else if(flag=="--selection-hold"&&value>=.1&&value<=1)selection_hold=value;
            else throw std::runtime_error("Unknown or out-of-range override");
        }
        const std::filesystem::path directory=argv[1];
        if(std::filesystem::exists(directory)||!std::filesystem::create_directory(directory))throw std::runtime_error("Fresh output directory required");
        std::ofstream samples(directory/"samples.csv"),summary(directory/"summary.json");
        if(!samples||!summary)throw std::runtime_error("Cannot create numeric sweep output");
        samples<<"id,seconds,valid,converged,coupling_iterations,coupling_residual,grounded,chassis_y,rider_y,relative_y,selected_id,actuation_id,actuation_seconds,suspension_issue,bike_issue,event_failed,event_canceled,event_combo_landed,failure,landed_barspins,landed_tailwhips,landed_combos,landing_pending,landing_support_seconds,front_gap,rear_gap,measured_body_up_y,measured_closing_normal_speed,spin_radians,flip_radians,roll_radians,pending_spin_half_turns,pending_front_flips,pending_back_flips,pending_barrel_rolls\n"<<std::setprecision(17);
        summary<<std::setprecision(17)<<"{\"schema\":1,\"actual_controller_recording\":false,\"event\":\"headless_coupled_all_slot_sweep\",\"fixed_seconds\":0.01,\"attempts\":[";
        auto profile=bmx::streets_observed_v2();profile.movement.simulation.seconds=.01;
        if(torque_override>0)profile.movement.tuning.torque_limit=torque_override;
        if(force_override>0)profile.movement.tuning.force_limit=force_override;
        if(hop_override>0)profile.movement.tuning.hop_delta_speed=hop_override;
        const double directions[8][2]={{0,1},{23170./32767,23170./32767},{1,0},{23170./32767,-23170./32767},
            {0,-1},{-23170./32767,-23170./32767},{-1,0},{-23170./32767,23170./32767}};
        unsigned all_invalid=0,all_nonconverged=0,all_fired=0,all_caught=0,all_landed=0,index=0;
        for(const auto& entry:bmx::catalog::entries) {
            const unsigned id=static_cast<unsigned>(entry.id),set=id/10,slot=id%10;
            if(selected_id>=0&&id!=static_cast<unsigned>(selected_id))continue;
            bmx::CoupledWorld world(profile);unsigned invalid=0,nonconverged=0;bool fired=false,caught=false,landed=false;
            double peak=-1e9,base=0,minimum_gap=1e9,maximum_gap=-1e9;int first_bike_issue=0,first_suspension_issue=0;double first_bad_time=-1,first_nonconverged_time=-1,maximum_residual=0;
            bmx::CoupledOutput frame;
            for(unsigned tick=0;tick<650;++tick) {
                const double time=tick*.01;bmx::RawInput raw;raw.available=raw.focused=true;raw.device=1;
                const double selection_at=hop_at+selection_delay,release_at=selection_at+selection_hold;
                raw.quick_hop=time>=hop_at&&time<hop_at+.1;
                if(time>=selection_at-.08&&time<release_at) {
                    if(set==0){raw.left_shoulder=true;raw.right_trigger=1;}
                    if(set==1){raw.right_shoulder=true;raw.left_trigger=1;}
                    if(set==2)raw.left_shoulder=raw.right_shoulder=true;
                    if(set==3)raw.left_shoulder=true;if(set==4)raw.right_shoulder=true;
                    if(set==5)raw.left_trigger=raw.right_trigger=1;
                    if(set==6)raw.left_trigger=1;if(set==7)raw.right_trigger=1;
                    if(time>=selection_at){raw.right_x=directions[slot][0];raw.right_y=directions[slot][1];}
                }
                auto measured=world.output().bike;measured.body=world.body();double gap[2]{},closing=0;
                const auto up=bmx::rotate(measured.body.orientation,{0,1,0});
                for(unsigned wheel=0;wheel<2;++wheel){
                    const auto hub=bmx::wheel_hub(measured,wheel==0);gap[wheel]=hub[1]-profile.movement.simulation.wheels[wheel].radius;
                    bmx::ContactSample query;query.separation=gap[wheel];query.contact.point={hub[0],0,hub[2]};query.contact.normal={0,1,0};
                    query.contact.status=gap[wheel]<=profile.movement.tuning.contact_gap?bmx::ContactStatus::hit:bmx::ContactStatus::clear;
                    if(bmx::detail::source::chassis_control::eligible_support(measured.body,query))
                        closing=std::max(closing,std::max(0.,-bmx::detail::source::tire::point_velocity(measured.body,query.contact.point)[1]));
                }
                frame=world.step(raw);invalid+=!frame.valid;nonconverged+=!frame.converged;
                maximum_residual=std::max(maximum_residual,frame.coupling_residual);
                if(!frame.converged&&first_nonconverged_time<0)first_nonconverged_time=time;
                if(!frame.valid&&first_bad_time<0){first_bad_time=time;first_bike_issue=static_cast<int>(frame.bike.issue);first_suspension_issue=static_cast<int>(frame.suspension.issue);}
                if(tick==static_cast<unsigned>(std::ceil(hop_at/.01))-1)base=frame.observed_chassis_com[1];
                if(frame.valid&&time>=hop_at){peak=std::max(peak,frame.observed_chassis_com[1]);minimum_gap=std::min(minimum_gap,frame.rider_relative_com[1]);maximum_gap=std::max(maximum_gap,frame.rider_relative_com[1]);}
                const bool legacy=(id==34||id==44)?frame.bike.tricks.bar_rotating:(id==52||id==56)?frame.bike.tricks.tail_rotating:false;
                if(static_cast<unsigned>(frame.bike.actions.movement)==id&&(static_cast<unsigned>(frame.bike.tricks.movement_id)==id||legacy))fired=true;
                caught=caught||frame.bike.events.movement_caught||frame.bike.events.bar_caught||frame.bike.events.tail_caught;
                landed=landed||frame.bike.tricks.tally.landed_by_id[id]>0;
                samples<<id<<','<<time<<','<<frame.valid<<','<<frame.converged<<','<<frame.coupling_iterations<<','<<frame.coupling_residual<<','<<frame.bike.grounded<<','<<frame.observed_chassis_com[1]<<','<<frame.rider.center_of_mass[1]<<','<<frame.rider_relative_com[1]<<','<<static_cast<int>(frame.bike.actions.movement)<<','<<(frame.joint_actuation_available?static_cast<int>(frame.joint_actuation_id):-1)<<','<<frame.joint_actuation_seconds<<','<<static_cast<int>(frame.suspension.issue)<<','<<static_cast<int>(frame.bike.issue)
                    <<','<<frame.bike.events.failed<<','<<frame.bike.events.canceled<<','<<frame.bike.events.combo_landed<<','<<static_cast<int>(frame.bike.tricks.failure)<<','<<frame.bike.tricks.tally.landed_barspins<<','<<frame.bike.tricks.tally.landed_tailwhips<<','<<frame.bike.tricks.tally.landed_combos<<','<<frame.bike.tricks.landing_pending<<','<<frame.bike.tricks.landing_support_seconds<<','<<gap[0]<<','<<gap[1]<<','<<up[1]<<','<<closing
                    <<','<<frame.bike.tricks.spin_radians<<','<<frame.bike.tricks.flip_radians<<','<<frame.bike.tricks.roll_radians<<','<<frame.bike.tricks.pending_spin_half_turns<<','<<frame.bike.tricks.pending_front_flips<<','<<frame.bike.tricks.pending_back_flips<<','<<frame.bike.tricks.pending_barrel_rolls<<'\n';
            }
            if(index++)summary<<',';
            summary<<"{\"id\":"<<id<<",\"valid_samples\":"<<(650-invalid)<<",\"invalid_samples\":"<<invalid<<",\"nonconverged_samples\":"<<nonconverged<<",\"adopted_selection\":"<<(fired?"true":"false")<<",\"caught_signal\":"<<(caught?"true":"false")<<",\"module_landed_by_id\":"<<(landed?"true":"false")<<",\"chassis_rise_m\":"<<peak-base<<",\"relative_y_range_m\":["<<minimum_gap<<','<<maximum_gap<<"],\"first_invalid_seconds\":"<<first_bad_time<<",\"first_nonconverged_seconds\":"<<first_nonconverged_time<<",\"maximum_coupling_residual\":"<<maximum_residual<<",\"first_bike_issue\":"<<first_bike_issue<<",\"first_suspension_issue\":"<<first_suspension_issue<<'}';
            all_invalid+=invalid;all_nonconverged+=nonconverged;all_fired+=fired;all_caught+=caught;all_landed+=landed;
        }
        summary<<"],\"counts\":{\"attempted\":"<<index<<",\"adopted\":"<<all_fired<<",\"caught\":"<<all_caught<<",\"module_landed_by_id\":"<<all_landed<<",\"invalid_samples\":"<<all_invalid<<",\"nonconverged_samples\":"<<all_nonconverged<<"},\"profile_parameters\":{\"hop_delta_speed\":"<<profile.movement.tuning.hop_delta_speed<<",\"hop_at_seconds\":"<<hop_at<<",\"selection_delay_seconds\":"<<selection_delay<<",\"selection_hold_seconds\":"<<selection_hold<<",\"force_limit\":"<<profile.movement.tuning.force_limit<<",\"torque_limit\":"<<profile.movement.tuning.torque_limit<<"},\"limits\":[\"Numeric headless exercise; actual controller/video validation is separate.\",\"Every slot starts from a fresh independent flat world with the same 100Hz quick-hop/selection sequence.\",\"Module landed-by-id signals are independent rules and do not establish native scoring parity.\"]}\n";
        samples.flush();summary.flush();if(!samples||!summary)throw std::runtime_error("Sweep write failed");
        std::cout<<"Attempted "<<index<<", adopted "<<all_fired<<", caught "<<all_caught<<", module landed "<<all_landed<<", invalid samples "<<all_invalid<<", nonconverged samples "<<all_nonconverged<<'\n';return (all_invalid||all_nonconverged)?2:0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
