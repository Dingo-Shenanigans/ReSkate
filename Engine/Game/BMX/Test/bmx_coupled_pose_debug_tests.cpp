#include "Extension/BMX/bmx_coupled_pose_debug.h"
#include <cstdio>
#include <cstdlib>
namespace d=dingosdk::bmx::rider::coupled_debug;
namespace p=dingosdk::bmx::rider::pose;
using dingosdk::Json;
unsigned checks{};
void check(bool v,const char* what){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",what);std::exit(1);}}
int main(){
    std::vector<p::Bone> bones(395);p::Rig rig;rig.parents.resize(386);rig.bind.resize(386,{0,0,0,1});
    dingosdk::bmx::visual::Frame frame;frame.active=true;frame.entity=123;frame.spring_avatar.requested=frame.spring_avatar.capture.requested=true;
    frame.spring_avatar.capture.neutral=true;frame.spring_avatar.actual.epoch.serial=100;frame.spring_avatar.actual.epoch.time=10;
    frame.spring_avatar.actual.epoch.bodies[1]={456,7,8};frame.spring_avatar.capture.seed.request.yaw=.25f;
    frame.spring_avatar.capture.seed.cross_plane_residual={.001,.002};frame.spring_avatar.capture.seed.cross_plane_allowance=.003;
    p::EntryContacts contacts;p::SpringBinding binding;dingosdk::bmx::NativeMotionLeaseStatus lease;lease.serial=101;lease.simulation_time=10.1;
    bones[0].position[3]=std::bit_cast<float>(0x7fc12345u);bones[0].scale[3]=std::bit_cast<float>(0xffc12345u);
    d::Store denied;{d::Trace trace(denied,false);trace.capture(bones,rig,frame,contacts,binding,false);}
    check(!denied.claimed&&!denied.fixture.load(),"default disabled instrumentation retains nothing");
    d::Store bounds;
    for(unsigned fault=0;fault<6;++fault){auto bad=frame;auto badbones=bones;auto badrig=rig;
        if(fault==0)bad.spring_avatar.requested=false;if(fault==1)bad.spring_avatar.capture.requested=false;
        if(fault==2)bad.mount_progress=.999f;if(fault==3)bad.rider_blend=.999f;if(fault==4)badbones.pop_back();if(fault==5)badrig.parents.pop_back();
        {d::Trace trace(bounds,true);trace.capture(badbones,badrig,bad,contacts,binding,false);}
        check(!bounds.claimed&&!bounds.fixture.load(),"only exact supported endpoint shape can claim one fixture");}
    d::Store once;{d::Trace trace(once,true);trace.at(d::Stage::lease_before);trace.capture(bones,rig,frame,contacts,binding,false);
        trace.clock(true,101,10.1,lease,frame.spring_avatar.actual.epoch);check(!once.fixture.load(),"partial fixture unpublished until callback exits");}
    const auto first=once.fixture.load();check(first&&first->stage==d::Stage::lease_before&&!first->solved,"prelease rejection retained distinctly from solve failure");
    check(once.exits[static_cast<unsigned>(d::Stage::lease_before)]==1&&once.actual_serial==101&&once.published_serial==100,"actual-versus-published clock mismatch retained without admission");
    auto changed=frame;changed.entity=999;{d::Trace trace(once,true);trace.at(d::Stage::completed);trace.capture(bones,rig,changed,contacts,binding,true);}
    check(once.fixture.load()==first&&first->frame.entity==123,"later endpoint cannot overwrite the single actual reproducer");
    check(once.exits[static_cast<unsigned>(d::Stage::completed)]==1,"later callback exits still counted");
    const auto parsed=Json::parse(d::fixture_json(*first).dump());
    check(parsed.at("original_bones").size()==395&&parsed.at("rig").at("parents").size()==386&&parsed.at("rig").at("bind").size()==386,"complete bounded rig and original bones survive deferred JSON");
    check(parsed.at("original_bones").at(0).at("position_bits").at(3).get<std::uint32_t>()==0x7fc12345u&&
          parsed.at("original_bones").at(0).at("scale_bits").at(3).get<std::uint32_t>()==0xffc12345u,"native nonfinite metadata W lanes retain exact raw bits");
    check(parsed.at("actual_serial").get<std::uint64_t>()==101&&parsed.at("frame").at("spring_avatar").at("actual").at("epoch").at("serial").get<std::uint64_t>()==100,"serialized endpoint is bound to actual and published clocks");
    check(parsed.at("lease").at("serial").get<std::uint64_t>()==101&&parsed.at("frame").at("entity").get<unsigned>()==123,"actual lease and independent publication survive serialization");
    check(parsed.at("frame").at("spring_avatar").at("actual").at("epoch").at("bodies").at(1).at("generation").get<unsigned>()==8,"full body reference retained");
    check(parsed.at("frame").at("spring_avatar").at("capture").at("seed").at("request_yaw").get<float>()==.25f,"seed request orientation retained");
    d::Store solve;{d::Trace trace(solve,true);trace.capture(bones,rig,frame,contacts,binding,true);trace.at(d::Stage::solved);p::Result result;result.hand_error=.004f;trace.solved(result);}
    check(solve.fixture.load()->solved&&!solve.fixture.load()->result.ready&&solve.fixture.load()->result.hand_error==.004f,"failed solve is distinguishable and retains actual solver result");
    check(!binding.root.ready&&!binding.proximal_ready&&!binding.completed_pose_write,"diagnostics cannot mutate source binding or manufacture completion");
    std::printf("bounded coupled native pose diagnostic: %u checks PASS (explicit host records)\n",checks);return 0;
}
