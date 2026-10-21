#include <bmx/pose_track.hpp>
#include <bmx/observed_trick_timing.hpp>
#include <cmath>
#include <iostream>
#include <limits>
namespace {
unsigned checks=0;
void check(bool value){++checks;if(!value){std::cerr<<"posture assertion "<<checks<<" failed\n";std::exit(1);}}
bool near(double a,double b,double epsilon=1e-5){return std::abs(a-b)<epsilon;}
}
int main(){
    using namespace bmx;
    PoseSample neutral{};
    const unsigned hips=unsigned(PoseBoneId::Hips),hand=unsigned(PoseBoneId::LeftHand);
    neutral[hips]={{0,1,0},{0,0,0,1},pose_position|pose_rotation};
    neutral[hand]={{.2f,1,.3f},{0,0,0,1},pose_position};
    std::array<PoseKey,2> keys{};keys[0].seconds=-.1f;keys[1].seconds=.9f;
    keys[0].delta[hips]={{0,0,0},{0,0,0,1},pose_position|pose_rotation};
    keys[1].delta[hips]={{0,.4f,.2f},{0,1,0,0},pose_position|pose_rotation};
    keys[0].delta[hand]={{0,0,0},{0,0,0,1},pose_position};
    // The later hand sample is missing: no interpolation across its gap.
    PoseTrack track;track.id=TrickId::superman;track.neutral=neutral;track.keys=keys;
    auto sample=sample_posture(track,.4);
    check(sample[hips].available==(pose_position|pose_rotation));
    check(near(sample[hips].position[1],1.2));check(near(sample[hips].position[2],.1));
    check(near(std::abs(sample[hips].rotation[1]),std::sqrt(.5)));
    check(near(std::abs(sample[hips].rotation[3]),std::sqrt(.5)));
    check(sample[hand].available==0);
    auto world=posture_to_world(sample,{{3,2,-1},{0,std::sqrt(.5),0,std::sqrt(.5)}});
    check(near(world[hips].position[0],3.1));check(near(world[hips].position[1],3.2));
    check(near(world[hips].position[2],-1));check(near(std::abs(world[hips].rotation[1]),1));
    auto half=sample_posture(track,.4,.5);
    check(near(half[hips].position[1],1.1));
    check(near(std::abs(half[hips].rotation[1]),std::sin(std::acos(-1.)/8)));
    auto first=sample_posture(track,-10),last=sample_posture(track,10);
    check(near(first[hips].position[1],1));check(near(last[hips].position[1],1.4));
    check(sample_posture(track,std::numeric_limits<double>::quiet_NaN())[hips].available==0);
    check(posture_to_world(sample,{{0,0,0},{0,0,0,0}})[hips].available==0);
    keys[1].seconds=keys[0].seconds;check(sample_posture(track,0)[hips].available==0);keys[1].seconds=.9;
    keys[1].delta[hips].position[1]=std::numeric_limits<float>::infinity();
    auto missing=sample_posture(track,.4);check(!(missing[hips].available&pose_position));check(missing[hips].available&pose_rotation);
    keys[1].delta[hips].rotation={0,0,0,0};check(sample_posture(track,.4)[hips].available==0);
    RiderPose fallback;fallback.hands[0]={4,5,6};auto rider=posture_rider(world,fallback);
    check(near(rider.hips[0],world[hips].position[0]));check(rider.hands[0]==fallback.hands[0]);
    check(pose_bone_count==55);check(pose_bone_names[unsigned(PoseBoneId::UpperChest)]=="UpperChest");
    PosturePlayer player;check(!player.begin(TrickId::none));check(!player.begin(TrickId::grand_slam_left,0));
    check(player.begin(TrickId::grand_slam_left,19));player.advance(.01,19);check(player.active());check(near(player.seconds(),.01));
    check(player.local()[hips].available&pose_position);player.advance(.01,20);check(!player.active());
    check(player.begin(TrickId::grand_slam_left));player.advance(.01,1,false);check(!player.active());
    check(player.begin(TrickId::grand_slam_left));player.advance(.11);check(!player.active());
    TrickSettings timing;apply_streets_observed_trick_timing(timing);
    check(near(timing.enter_seconds[0],2.5/4.8));check(near(timing.recover_seconds[0],2.041667/(4.8*.72)));
    check(timing.enter_seconds[30]==0);check(timing.enter_seconds[40]==0);
    TrickSettings twice;apply_streets_observed_trick_timing(twice,2);check(near(twice.enter_seconds[0],timing.enter_seconds[0]/2));
    TrickSettings rejected;apply_streets_observed_trick_timing(rejected,0);check(rejected.enter_seconds[0]==0);
    std::cout<<checks<<" posture interpolation assertions passed\n";
}
