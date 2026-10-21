#include <bmx/vehicle_pose.hpp>
#include <bmx/ik_track.hpp>
#include <cmath>
#include <cstdlib>
#include <iostream>
namespace {
unsigned checks{};
void check(bool value){++checks;if(!value){std::cerr<<"vehicle/IK assertion "<<checks<<" failed\n";std::exit(1);}}
bool near(double a,double b){return std::abs(a-b)<1e-5;}
}
int main(){
    using namespace bmx;
    std::array<VehiclePoseKey,2> keys{{{0,{{1,0,0},{0,0,0,1},3}},{1,{{2,0,0},{0,1,0,0},3}}}};
    VehiclePoseTrack track;track.neutral=keys[0].transform;track.keys=keys;
    const auto sample=sample_vehicle_node(track,.5);check(sample.available==3);check(near(sample.position[0],1.5));
    check(near(std::abs(sample.rotation[1]),std::sqrt(.5)));
    const auto world=articulate_vehicle_point({1,0,1},track,sample,{{3,2,-1},{0,0,0,1}});
    check(near(world[0],5.5));check(near(world[1],2));check(near(world[2],-1));
    const auto moved=articulate_vehicle_point({1,0,1},track,sample,{{3,2,-1},{0,std::sqrt(.5),0,std::sqrt(.5)}});
    check(near(moved[0],3));check(near(moved[1],2));check(near(moved[2],-3.5));
    check(near(sample_vehicle_node(track,-1).position[0],1));check(near(sample_vehicle_node(track,2).position[0],2));
    keys[1].transform.available=pose_position;check(sample_vehicle_node(track,.5).available==pose_position);
    const auto fallback=articulate_vehicle_point({1,0,1},track,{},{{3,2,-1},{0,0,0,1}});
    check(near(fallback[0],4));check(near(fallback[2],0));
    keys[1].seconds=0;check(sample_vehicle_node(track,.5).available==0);
    std::array<IKKey,2> weights{{{0,{0,1,.2f,4,15,IKContactBinding::unknown}},
                                {1,{1,0,.8f,9,15,IKContactBinding::left_grip}}}};
    IKTrack ik;ik.keys=weights;auto half=sample_ik(ik,.5);
    check(near(half.position_weight,.5));check(near(half.rotation_weight,.5));check(near(half.reference_weight,.5));
    check(half.target_selector==4);check(half.binding==IKContactBinding::unknown);
    auto end=sample_ik(ik,1);check(end.target_selector==9);check(end.binding==IKContactBinding::left_grip);
    weights[1].command.available&=~ik_reference_weight;check(!(sample_ik(ik,.5).available&ik_reference_weight));
    weights[1].seconds=0;check(sample_ik(ik,.5).available==0);
    check(vehicle_pose_node_count==13);check(unsigned(RiderLimb::count)==4);
    std::cout<<checks<<" vehicle transform and IK interpolation assertions passed\n";
}
