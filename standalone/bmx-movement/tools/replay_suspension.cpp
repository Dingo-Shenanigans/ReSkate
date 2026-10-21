// Numeric read-only fixture replay; no game, controller, UI or wall-clock access.
#include <bmx/suspension.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

int main(int argc,char** argv) {
    try {
        if(argc<3||argc>8)throw std::runtime_error("Usage: replay_suspension fixture.csv fresh-output.csv [--static-target] [--hop-delay .12] [--torque-limit 20000]");
        bool measured=true;double hop_delay=-1,torque_limit=-1;
        for(int i=3;i<argc;++i) {
            const std::string flag=argv[i];
            if(flag=="--static-target")measured=false;
            else if(flag=="--hop-delay"&&i+1<argc) {hop_delay=std::stod(argv[++i]);if(!std::isfinite(hop_delay)||hop_delay<0||hop_delay>.3)throw std::runtime_error("Hop delay outside 0..0.3");}
            else if(flag=="--torque-limit"&&i+1<argc) {torque_limit=std::stod(argv[++i]);if(!std::isfinite(torque_limit)||torque_limit<4000||torque_limit>150000)throw std::runtime_error("Torque limit outside 4000..150000");}
            else throw std::runtime_error("Unknown option");
        }
        if(std::filesystem::exists(argv[2]))throw std::runtime_error("Fresh output required");
        std::ifstream input(argv[1]);if(!input)throw std::runtime_error("Fixture unavailable");
        auto split=[](std::string line){if(!line.empty()&&line.back()=='\r')line.pop_back();std::vector<std::string> fields;std::istringstream s(line);std::string v;while(std::getline(s,v,','))fields.push_back(v);return fields;};
        std::string line;std::getline(input,line);const auto names=split(line);std::unordered_map<std::string,unsigned> columns;
        for(unsigned i=0;i<names.size();++i)if(!columns.emplace(names[i],i).second)throw std::runtime_error("Duplicate fixture column");
        std::ofstream output(argv[2]);if(!output)throw std::runtime_error("Cannot write output");
        output<<"source_index,simulation_seconds,valid,converged,grounded,chassis_y,spring_y,chassis_vy,spring_vy,relative_x,relative_y,relative_z,hop_impulse_y,rider_hop_impulse_y,coupling_residual,suspension_issue,bike_issue,chassis_up_y,chassis_qx,chassis_qy,chassis_qz,chassis_qw\n"<<std::setprecision(17);
        auto profile=bmx::streets_observed_v2();profile.movement.simulation.seconds=.01;
        if(hop_delay>=0)profile.movement.simulation.hop_delay_seconds=hop_delay;
        if(torque_limit>=0)profile.movement.tuning.torque_limit=torque_limit;
        if(!bmx::valid_suspension(profile.coupled.suspension))throw std::runtime_error("Factory suspension settings invalid; verify library/header ABI");
        bmx::CoupledWorld world(profile);
        unsigned index=0,invalid=0;double previous=0;auto axis=[](double value){return std::clamp(value/32767.,-1.,1.);};
        while(std::getline(input,line)) {
            if(line.empty())continue;const auto values=split(line);if(values.size()!=names.size())throw std::runtime_error("Fixture width changed");
            auto value=[&](const std::string& name){std::size_t used=0;const auto& text=values.at(columns.at(name));const auto n=std::stod(text,&used);if(used!=text.size()||!std::isfinite(n))throw std::runtime_error("Nonfinite fixture");return n;};
            const auto sim=value("simulation_seconds");if(value("index")!=index||std::abs(value("fixed_dt")-.01)>1e-8||(index&&std::abs(sim-previous-.01)>2e-4))throw std::runtime_error("Discontinuous fixed sample");
            previous=sim;const auto buttons=static_cast<unsigned>(value("buttons"));bmx::RawInput raw;
            raw.available=true;raw.focused=true;raw.device=1;raw.left_x=axis(value("lx"));raw.left_y=axis(value("ly"));raw.right_x=axis(value("rx"));raw.right_y=axis(value("ry"));
            raw.left_trigger=value("lt")/255;raw.right_trigger=value("rt")/255;raw.pedal=(buttons&0x1000)?1:0;raw.brake=(buttons&0x2000)?1:0;
            raw.quick_hop=(buttons&0x0080)!=0;raw.left_shoulder=(buttons&0x0100)!=0;raw.right_shoulder=(buttons&0x0200)!=0;
            bmx::CoupledTargets targets;
            if(measured) {
                targets.suspension_available=targets.rider_inertia_available=true;auto& joint=targets.suspension=profile.coupled.suspension;
                joint.target_position={value("target_x"),value("target_y"),value("target_z")};joint.target_rotation={value("target_qx"),value("target_qy"),value("target_qz"),value("target_qw")};
                for(unsigned i=0;i<4;++i) {
                    const std::string key=i==0?"x":i==1?"y":i==2?"z":"angular";
                    // The finite native Float.MaxValue is effectively uncapped.
                    // This independent solver retains its declared 1 MN bound.
                    bmx::SuspensionDrive drive{value(key+"_spring"),value(key+"_damper"),std::min(1000000.,value(key+"_maximum_effort")),value(key+"_acceleration")?bmx::SuspensionDriveMode::acceleration:bmx::SuspensionDriveMode::force};
                    if(i<3)joint.linear[i]=drive;else for(auto& angular:joint.angular)angular=drive;
                }
                for(auto& limit:joint.linear_limits)limit.enabled=false;
                joint.linear_radial_limit={true,0,value("linear_limit"),{1000000,5000,150000}};
                targets.rider_inertia={value("inertia_x"),value("inertia_y"),value("inertia_z")};targets.rider_inertia_orientation={value("inertia_qx"),value("inertia_qy"),value("inertia_qz"),value("inertia_qw")};
                if(!bmx::valid_suspension(joint))throw std::runtime_error("Invalid converted suspension settings at sample "+std::to_string(index));
            }
            const auto result=world.step(raw,targets);invalid+=!result.valid;
            if(!result.valid&&invalid==1)std::cerr<<"First invalid sample: "<<index<<", suspension issue: "<<bmx::suspension_issue_name(result.suspension.issue)<<", bike issue: "<<static_cast<int>(result.bike.issue)<<", residual: "<<result.coupling_residual<<'\n';
            output<<index<<','<<sim<<','<<result.valid<<','<<result.converged<<','<<result.bike.grounded<<','<<result.observed_chassis_com[1]<<','<<result.rider.center_of_mass[1]<<','<<result.bike.body.linear_velocity[1]<<','<<result.rider.linear_velocity[1]
                  <<','<<result.rider_relative_com[0]<<','<<result.rider_relative_com[1]<<','<<result.rider_relative_com[2]<<','<<result.bike.hop_impulse[1]<<','<<result.rider_hop_impulse[1]<<','<<result.coupling_residual<<','<<static_cast<int>(result.suspension.issue)<<','<<static_cast<int>(result.bike.issue)<<','<<bmx::rotate(result.bike.body.orientation,{0,1,0})[1];
            for(double q:result.bike.body.orientation)output<<','<<q;output<<'\n';
            if(++index>6000)throw std::runtime_error("Fixture exceeds 60 seconds");
        }
        if(index<20)throw std::runtime_error("Insufficient fixture");output.flush();if(!output)throw std::runtime_error("Replay write failed");
        std::cerr<<"{\"event\":\"headless_suspension_replay\",\"actual_controller_recording\":false,\"samples\":"<<index<<",\"invalid\":"<<invalid<<",\"measured_targets\":"<<(measured?"true":"false")<<",\"hop_delay_seconds\":"<<profile.movement.simulation.hop_delay_seconds<<",\"chassis_torque_limit\":"<<profile.movement.tuning.torque_limit<<",\"maximum_drive_effort\":1000000,\"maximum_net_force\":150000}\n";
        return invalid?2:0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
