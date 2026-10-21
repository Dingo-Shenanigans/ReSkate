// Headless empirical calibration bench. No window, controller, clock or game access.
// Compile against bmx_movement and feed the numeric fixture from native_replay.py.
#include <bmx/movement.hpp>
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

namespace {
std::vector<double> numbers(const std::string& line) {
    std::vector<double> values;std::istringstream stream(line);std::string part;
    while(std::getline(stream,part,',')) {
        std::size_t used=0;const auto value=std::stod(part,&used);
        if(used!=part.size()||!std::isfinite(value))throw std::runtime_error("Expected finite numeric fixture");
        values.push_back(value);
    }
    return values;
}
double bounded(const char* text,double low,double high) {
    std::size_t used=0;const std::string string=text;const double value=std::stod(string,&used);
    if(used!=string.size()||!std::isfinite(value)||value<low||value>high)throw std::runtime_error("Override outside bounds");
    return value;
}
}

int main(int argc,char** argv) {
    try {
        if(argc<3||argc>13||argc%2!=1)throw std::runtime_error("Usage: replay_native fixture.csv fresh-output.csv [--hop-speed 7.9] [--pedal-torque 75] [--brake-min-scale .69] [--brake-max-scale 1.9] [--brake-speed-gain .39]");
        auto profile=bmx::streets_observed_v1();profile.simulation.seconds=.01;
        for(int i=3;i<argc;i+=2) {
            const std::string flag=argv[i];
            if(flag=="--hop-speed")profile.tuning.hop_delta_speed=bounded(argv[i+1],.1,15);
            else if(flag=="--pedal-torque")profile.tuning.pedal_torque=bounded(argv[i+1],0,300);
            else if(flag=="--brake-min-scale")profile.tuning.brake_min_scale=bounded(argv[i+1],.01,4);
            else if(flag=="--brake-max-scale")profile.tuning.brake_max_scale=bounded(argv[i+1],.01,4);
            else if(flag=="--brake-speed-gain")profile.tuning.brake_speed_gain=bounded(argv[i+1],0,4);
            else throw std::runtime_error("Unknown override");
        }
        std::ifstream source(argv[1]);if(!source)throw std::runtime_error("Fixture unavailable");
        if(std::filesystem::exists(argv[2]))throw std::runtime_error("Output must be fresh");
        std::string line;std::getline(source,line);if(!line.empty()&&line.back()=='\r')line.pop_back();
        const std::string header="index,wall_seconds,simulation_seconds,fixed_dt,buttons,lx,ly,rx,ry,lt,rt,native_speed,native_chassis_y,native_vehicle_com_y,native_vy,native_grounded,report_index,report_age_seconds";
        if(line!=header)throw std::runtime_error("Unsupported fixture columns");
        std::vector<std::vector<double>> samples;
        while(std::getline(source,line)) {
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            if(line.empty())continue;
            auto row=numbers(line);
            if(row.size()!=18||row[0]!=static_cast<double>(samples.size())||std::abs(row[3]-.01)>1e-6)
                throw std::runtime_error("Invalid fixed-step fixture");
            if(!samples.empty()&&(row[1]<=samples.back()[1]||std::abs(row[2]-samples.back()[2]-.01)>2e-4))
                throw std::runtime_error("Fixture clock lost a fixed sample");
            for(unsigned axis=5;axis<=8;++axis)if(row[axis]<-32768||row[axis]>32767)throw std::runtime_error("Axis outside pad range");
            if(row[4]<0||row[4]>65535||row[9]<0||row[9]>255||row[10]<0||row[10]>255)throw std::runtime_error("Buttons/triggers outside pad range");
            samples.push_back(std::move(row));if(samples.size()>6000)throw std::runtime_error("Fixture exceeds60seconds");
        }
        if(samples.size()<20)throw std::runtime_error("Insufficient fixture samples");
        std::ofstream output(argv[2]);if(!output)throw std::runtime_error("Cannot create output");
        output<<"source_index,source_wall_seconds,source_simulation_seconds,replay_elapsed_seconds,valid,grounded,speed,x,y,z,vx,vy,vz,pedal,brake,quick_hop,hop_impulse_y\n"<<std::setprecision(17);
        bmx::FlatWorld world(profile.tuning,profile.simulation,profile.rider);unsigned invalid=0;
        auto axis=[](double value){return std::clamp(value/32767.,-1.,1.);};
        for(const auto& row:samples) {
            const auto buttons=static_cast<unsigned>(row[4]);bmx::RawInput raw;
            raw.available=true;raw.focused=true;raw.device=1;
            raw.left_x=axis(row[5]);raw.left_y=axis(row[6]);raw.right_x=axis(row[7]);raw.right_y=axis(row[8]);
            raw.left_trigger=row[9]/255;raw.right_trigger=row[10]/255;
            raw.pedal=(buttons&0x1000)?1:0;raw.brake=(buttons&0x2000)?1:0;raw.quick_hop=(buttons&0x0080)!=0;
            raw.left_shoulder=(buttons&0x0100)!=0;raw.right_shoulder=(buttons&0x0200)!=0;
            const auto frame=world.step(raw);const auto& p=frame.body.center_of_mass;const auto& v=frame.body.linear_velocity;
            invalid+=!frame.valid;
            output<<row[0]<<','<<row[1]<<','<<row[2]<<','<<(row[0]+1)*.01<<','<<frame.valid<<','<<frame.grounded<<','<<std::hypot(v[0],v[2])
                  <<','<<p[0]<<','<<p[1]<<','<<p[2]<<','<<v[0]<<','<<v[1]<<','<<v[2]<<','<<raw.pedal<<','<<raw.brake<<','<<raw.quick_hop<<','<<frame.hop_impulse[1]<<'\n';
        }
        output.flush();if(!output)throw std::runtime_error("Replay write failed");
        std::cerr<<"{\"event\":\"headless_native_sample_replay\",\"actual_controller_recording\":false,\"samples\":"<<samples.size()
                 <<",\"invalid\":"<<invalid<<",\"fixed_seconds\":0.01,\"hop_speed\":"<<profile.tuning.hop_delta_speed
                 <<",\"pedal_torque\":"<<profile.tuning.pedal_torque<<",\"brake_min_scale\":"<<profile.tuning.brake_min_scale
                 <<",\"brake_max_scale\":"<<profile.tuning.brake_max_scale<<",\"brake_speed_gain\":"<<profile.tuning.brake_speed_gain<<"}\n";
        return invalid?2:0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
