// Headless shared-request fixture; no controller, window or native game access.
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
#include <vector>

int main(int argc,char** argv) {
    try {
        if(argc!=3)throw std::runtime_error("Usage: replay_requests fixed-request.csv fresh-output.csv");
        const std::filesystem::path output_path=argv[2];
        if(std::filesystem::exists(output_path))throw std::runtime_error("Fresh output required");
        std::ifstream input(argv[1]);if(!input)throw std::runtime_error("Request fixture unavailable");
        std::string line;std::getline(input,line);
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line!="seconds,buttons,lx,ly,rx,ry,lt,rt")throw std::runtime_error("Unsupported request fixture header");
        std::ofstream output(output_path);if(!output)throw std::runtime_error("Cannot create output");
        output<<"seconds,reset,valid,converged,coupling_residual,grounded,chassis_y,rider_y,relative_y,chassis_vy,rider_vy,hop_pressed,hop_accepted,hop_kind,flick_preloading,hop_strength,preload,manual,nose_manual,hop_impulse_y,rider_hop_impulse_y,actuation_id,actuation_seconds,suspension_issue,bike_issue\n"<<std::setprecision(17);
        auto profile=bmx::streets_observed_v2();profile.movement.simulation.seconds=.01;
        bmx::CoupledWorld world(profile);unsigned index=0,previous_buttons=0,invalid=0,nonconverged=0;
        auto axis=[](double v){if(v< -32768||v>32767||v!=std::round(v))throw std::runtime_error("Axis outside raw HID domain");return std::clamp(v/32767.,-1.,1.);};
        while(std::getline(input,line)) {
            if(line.empty())continue;std::istringstream fields(line);std::string text;std::vector<double> row;
            while(std::getline(fields,text,',')){std::size_t consumed=0;double value=std::stod(text,&consumed);if(consumed!=text.size()&&text.substr(consumed)!="\r")throw std::runtime_error("Non-numeric request");if(!std::isfinite(value))throw std::runtime_error("Nonfinite request");row.push_back(value);}
            if(row.size()!=8||std::abs(row[0]-index*.01)>1e-8||index>4000)throw std::runtime_error("Discontinuous or overlong request fixture");
            if(row[1]<0||row[1]>65535||row[1]!=std::round(row[1])||row[6]<0||row[6]>255||row[7]<0||row[7]>255)throw std::runtime_error("Raw button/trigger outside range");
            const auto buttons=static_cast<unsigned>(row[1]);const bool reset=(buttons&2)&&!(previous_buttons&2);previous_buttons=buttons;
            if(reset)world.reset();
            bmx::RawInput raw;raw.available=raw.focused=true;raw.device=1;
            raw.left_x=axis(row[2]);raw.left_y=axis(row[3]);raw.right_x=axis(row[4]);raw.right_y=axis(row[5]);
            raw.left_trigger=row[6]/255;raw.right_trigger=row[7]/255;
            raw.pedal=(buttons&0x1000)?1:0;raw.brake=(buttons&0x2000)?1:0;raw.quick_hop=buttons&0x80;
            raw.left_shoulder=buttons&0x100;raw.right_shoulder=buttons&0x200;
            const auto frame=world.step(raw);invalid+=!frame.valid;nonconverged+=!frame.converged;
            output<<row[0]<<','<<reset<<','<<frame.valid<<','<<frame.converged<<','<<frame.coupling_residual<<','<<frame.bike.grounded<<','
                <<frame.observed_chassis_com[1]<<','<<frame.rider.center_of_mass[1]<<','<<frame.rider_relative_com[1]<<','
                <<frame.bike.body.linear_velocity[1]<<','<<frame.rider.linear_velocity[1]<<','<<frame.bike.actions.hop_pressed<<','
                <<frame.bike.hop_request_accepted<<','<<static_cast<int>(frame.bike.actions.hop_kind)<<','<<frame.bike.actions.flick_preloading<<','
                <<frame.bike.actions.hop_strength<<','<<frame.bike.actions.preload<<','<<frame.bike.actions.manual<<','<<frame.bike.actions.nose_manual<<','
                <<frame.bike.hop_impulse[1]<<','<<frame.rider_hop_impulse[1]<<','<<(frame.joint_actuation_available?static_cast<int>(frame.joint_actuation_id):-1)<<','
                <<frame.joint_actuation_seconds<<','<<static_cast<int>(frame.suspension.issue)<<','<<static_cast<int>(frame.bike.issue)<<'\n';
            ++index;
        }
        if(index<20)throw std::runtime_error("Insufficient request fixture");output.flush();if(!output)throw std::runtime_error("Output write failed");
        std::cout<<"{\"event\":\"headless_shared_request_replay\",\"actual_controller_recording\":false,\"samples\":"<<index<<",\"invalid\":"<<invalid<<",\"nonconverged\":"<<nonconverged<<"}\n";
        return invalid||nonconverged?2:0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
