#include "Extension/BMX/bmx_rider_pose.h"
#include "Extension/BMX/bmx_board_visibility.h"
#include "Engine/Game/BMX/bmx_landing_presentation.h"
#include "Engine/Game/BMX/bmx_tricks.h"
#include "Engine/Game/BMX/bmx_grip_probe_policy.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
namespace p=dingosdk::bmx::rider::pose;
using dingosdk::bmx::visual::Frame;
constexpr float supplied_model_factor=.2630808f/.34f;
void check(bool value,const char* text) { if(!value){std::cerr<<text<<'\n';std::exit(1);} }
struct Fixture { p::Rig rig;std::vector<p::Bone> bones; };
Fixture fixture() {
    // Primary-chain transforms from the supported AnimBase_Default_RenderSkeleton
    // LocalPose, with only nonessential subnanometre roundoff removed. These axes
    // are intentionally not an identity/vertical synthetic rig: left is +X and
    // each bone's length runs along native local +X.
    constexpr float values[][7]{
        {0,0,0,0,0,0,1}, {0,0,0,0,0,0,1},
        {0,.928105f,.024836f,.5f,.5f,.5f,.5f},
        {.09116f,-.023888f,0,0,0,.1659156f,.98613996f},
        {.0697396f,0,0,0,0,-.19037838f,.98171079f},
        {.0935366f,0,0,0,0,-.10392933f,.99458468f},
        {.187545f,0,0,0,0,.22027384f,.97543807f},
        {.128881f,.0726601f,-.02593f,-.097978127f,.75556494f,-.20078539f,.61579796f},
        {.168454f,0,0,-.087031407f,.39259716f,.18090958f,.89753258f},
        {.295091f,0,0,0,0,.2336999f,.97230878f},
        {.273977f,0,0,-.020682992f,.18773654f,.042997995f,.98105982f},
        {.139632f,0,0,0,0,.11357232f,.99352973f},
        {.0780845f,0,0,0,0,-.19606917f,.98059007f},
        {.0637397f,0,0,0,0,-.0095862915f,.99995405f},
        {.128881f,.0726601f,.02593f,.097978127f,-.75556494f,-.20078539f,.61579796f},
        {.168454f,0,0,.087031407f,-.39259716f,.18090958f,.89753258f},
        {.295091f,0,0,0,0,.2336999f,.97230878f},
        {.273977f,0,0,.020682992f,-.18773654f,.042997995f,.98105982f},
        {-.000001f,-.001282f,-.087603f,-.053062952f,.090424695f,.99365267f,.040768365f},
        {.408567f,0,0,0,0,.090318643f,.99591292f},
        {.438437f,0,0,.006730955f,-.052158696f,-.55218877f,.83205873f},
        {-.000001f,-.001282f,.087603f,.053062952f,-.090424695f,.99365267f,.040768365f},
        {.408567f,0,0,0,0,.090318643f,.99591292f},
        {.438437f,0,0,-.006730955f,.052158696f,-.55218877f,.83205873f}
    };
    Fixture f;f.rig.parents={-1,0,1,2,3,4,5,6,7,8,9,6,11,12,6,14,15,16,2,18,19,2,21,22};
    f.rig.hips=2;f.rig.head=13;f.rig.spine={3,4,5,6};f.rig.neck={11,12};f.rig.shoulders={14,7};
    f.rig.right_arm={8,9,10};f.rig.left_arm={15,16,17};f.rig.right_leg={18,19,20};f.rig.left_leg={21,22,23};
    for(const auto& v:values){p::Bone bone;bone.position={v[0],v[1],v[2],17};bone.rotation={v[3],v[4],v[5],v[6]};bone.scale[3]=23;f.bones.push_back(bone);f.rig.bind.push_back(bone.rotation);}
    return f;
}
Fixture hands_fixture() {
    auto f=fixture();
    // Actual right finger LocalPose matrices from the shipped render skeleton.
    // The shipped left matrices are their reflection in native hand local Z.
    struct Row { int finger; bool metacarpal; std::array<float,12> matrix; };
    constexpr Row rows[]{
        {0,true, {0.962383f, 0.262448f, 0.0702909f, -0.263098f, 0.964769f, 0.f, -0.0678145f, -0.0184934f, 0.997527f, 0.0320206f, 0.0087322f, 0.00233873f}}, // RightInHandIndex
        {0,false, {0.945871f, -0.183305f, -0.267821f, 0.21234f, 0.973617f, 0.0835524f, 0.245439f, -0.135899f, 0.959839f, 0.0538277f, -1.42109e-16f, 0.f}}, // RightHandIndex1
        {0,false, {0.935635f, 0.f, -0.352968f, 1.04708e-17f, 1.f, 2.77556e-17f, 0.352968f, -2.9665e-17f, 0.935635f, 0.0499791f, 1.42109e-16f, -1.42109e-16f}}, // RightHandIndex2
        {0,false, {0.983663f, 0.f, -0.180022f, 0.f, 1.f, 0.f, 0.180022f, 0.f, 0.983663f, 0.0239749f, 0.f, -7.10543e-17f}}, // RightHandIndex3
        {1,true, {0.998234f, 0.0223807f, 0.0550188f, -0.0224146f, 0.999749f, -2.77556e-17f, -0.0550049f, -0.00123322f, 0.998485f, 0.0316469f, 0.00070953f, 0.00174425f}}, // RightInHandMiddle
        {1,false, {0.951104f, -0.0785054f, -0.298726f, 0.123137f, 0.983352f, 0.133625f, 0.283263f, -0.163875f, 0.944938f, 0.0512889f, 1.42109e-16f, 2.84217e-16f}}, // RightHandMiddle1
        {1,false, {0.951695f, 1.11022e-16f, -0.307046f, -1.16657e-16f, 1.f, 0.f, 0.307046f, 3.58193e-17f, 0.951695f, 0.0547859f, 1.42109e-16f, 0.f}}, // RightHandMiddle2
        {1,false, {0.985293f, 2.77556e-17f, -0.170875f, -3.29834e-17f, 1.f, -2.77556e-17f, 0.170875f, 3.29834e-17f, 0.985293f, 0.0306501f, -7.10543e-17f, 3.19744e-16f}}, // RightHandMiddle3
        {2,true, {0.975496f, -0.21988f, -0.00781909f, 0.219887f, 0.975525f, -5.55112e-17f, 0.00762772f, -0.00171931f, 0.999969f, 0.0299183f, -0.0067437f, -0.000239811f}}, // RightInHandRing
        {2,false, {0.946453f, 0.0393865f, -0.32043f, 0.0223015f, 0.982183f, 0.1866f, 0.32207f, -0.183754f, 0.928712f, 0.0496178f, 3.55271e-17f, 0.f}}, // RightHandRing1
        {2,false, {0.9625f, 2.77556e-17f, -0.27128f, -3.27484e-17f, 1.f, -1.38778e-17f, 0.27128f, 2.22414e-17f, 0.9625f, 0.0499779f, -7.10543e-17f, -1.42109e-16f}}, // RightHandRing2
        {2,false, {0.990148f, 0.f, -0.140024f, -3.92512e-18f, 1.f, -2.77556e-17f, 0.140024f, 2.80317e-17f, 0.990148f, 0.0265406f, 3.55271e-17f, -8.88178e-17f}}, // RightHandRing3
        {3,true, {0.893227f, -0.443028f, -0.0766253f, 0.444334f, 0.895861f, -2.77556e-17f, 0.0686456f, -0.0340472f, 0.99706f, 0.0266579f, -0.0132219f, -0.00228684f}}, // RightInHandPinky
        {3,false, {0.895162f, 0.130645f, -0.426166f, 0.120418f, 0.849654f, 0.513408f, 0.429167f, -0.510901f, 0.744846f, 0.0482824f, 5.32907e-17f, -1.42109e-16f}}, // RightHandPinky1
        {3,false, {0.970067f, 0.f, -0.242836f, 0.f, 1.f, 0.f, 0.242836f, 0.f, 0.970067f, 0.0441697f, 1.06581e-16f, -1.06581e-16f}}, // RightHandPinky2
        {3,false, {0.998235f, -2.77556e-17f, -0.0593852f, 2.78046e-17f, 1.f, 0.f, 0.0593852f, -1.65119e-18f, 0.998235f, 0.0214397f, -1.42109e-16f, 8.88178e-17f}}, // RightHandPinky3
        {4,false, {0.716214f, 0.597554f, -0.360509f, -0.296048f, -0.207645f, -0.93233f, -0.631976f, 0.774476f, 0.0281859f, 0.0212265f, 0.0177098f, -0.0106844f}}, // RightHandThumb1
        {4,false, {0.887307f, 0.f, -0.461179f, 0.f, 1.f, 0.f, 0.461179f, 0.f, 0.887307f, 0.047947f, -7.10543e-17f, 5.68434e-16f}}, // RightHandThumb2
        {4,false, {0.999903f, 0.f, -0.013935f, -7.73625e-19f, 1.f, -5.55112e-17f, 0.013935f, 5.55165e-17f, 0.999903f, 0.0324583f, 1.42109e-16f, -4.26326e-16f}}, // RightHandThumb3
    };
    for(bool left:{false,true}) {
        auto& fingers=left?f.rig.left_fingers:f.rig.right_fingers;
        const int hand=left?f.rig.left_arm[2]:f.rig.right_arm[2];
        int parent=hand;unsigned segment=0;
        for(const auto& row:rows) {
            auto m=row.matrix;
            if(left)for(unsigned i:{2u,5u,6u,7u,11u})m[i]=-m[i];
            const int index=static_cast<int>(f.bones.size());
            auto& finger=fingers[static_cast<std::size_t>(row.finger)];
            if(row.metacarpal){parent=hand;segment=0;finger.metacarpal=index;}
            else if(row.finger==4&&segment==3){parent=hand;segment=0;}
            if(row.metacarpal==false)finger.joints[segment++]=index;
            p::Bone bone;bone.position={m[9],m[10],m[11],17};
            bone.rotation=p::orientation({m[0],m[1],m[2]},{m[3],m[4],m[5]},{m[6],m[7],m[8]});bone.scale[3]=23;
            f.rig.parents.push_back(parent);f.rig.bind.push_back(bone.rotation);f.bones.push_back(bone);parent=index;
        }
    }
    return f;
}
void palms_and_fingers() {
    for(float yaw:{0.f,1.5707963f,-2.4f})for(float steer:{-.4f,0.f,.4f})for(float barspin:{-.7f,0.f,.7f}) {
        auto f=hands_fixture();Frame frame;frame.active=true;frame.position={627,199,1062};frame.yaw=yaw;frame.steer=steer;
        frame.barspin=barspin;frame.pitch=.12f;frame.roll=-.1f;
        f.bones[1].position={627,199,1062,17};f.bones[1].rotation=p::axis(1,yaw);
        // A different native gait may supply any valid wrist/finger rotations.
        // The grip solve must remove that variation on both animation callbacks.
        for(const auto* fingers:{&f.rig.left_fingers,&f.rig.right_fingers})for(const auto& finger:*fingers)
            for(int i:finger.joints)f.bones[static_cast<std::size_t>(i)].rotation=p::axis(2,.7f);
        check(p::solve(f.bones,f.rig,frame).ready,"complete native hand hierarchy can solve a stable grip");
        const auto g=p::compose(f.bones,f.rig);
        std::array<dingosdk::bmx::world::Matrix,dingosdk::bmx::world::part_count> matrices;
        check(dingosdk::bmx::world::transforms(frame,matrices),"native handlebars publish a render transform");
        const auto& matrix=matrices[5];
        for(bool left:{false,true}) {
            const int hand=left?f.rig.left_arm[2]:f.rig.right_arm[2];const auto& h=g[static_cast<std::size_t>(hand)];
            const auto center=p::add(h.position,p::turn(h.rotation,p::palm_bar_offset(left)));
            check(p::length(p::sub(center,p::grip(frame,left)))<.003f,"palm surrounds the rendered grip midpoint, not its outer tube end");
            // Independent supplied-FBX grip-fit literals and central-knurl axes,
            // verified against the actual articulated native handlebars matrix.
            const p::Vec authored=p::mul(left?p::Vec{.334575045f,1.231948059f,.340623945f}:
                p::Vec{-.334575042f,1.231948045f,.340623981f},supplied_model_factor);
            const p::Vec source_outward=left?p::Vec{.942691934f,.113109012f,-.313908057f}:
                p::Vec{-.942691915f,.113108999f,-.313908120f};
            const p::Vec rendered{matrix[0]*authored[0]+matrix[4]*authored[1]+matrix[8]*authored[2]+matrix[12],
                matrix[1]*authored[0]+matrix[5]*authored[1]+matrix[9]*authored[2]+matrix[13],
                matrix[2]*authored[0]+matrix[6]*authored[1]+matrix[10]*authored[2]+matrix[14]};
            check(p::length(p::sub(center,rendered))<.003f,"palm contact meets the actual articulated tube mesh midpoint");
            const p::Vec rendered_axis=p::unit({matrix[0]*source_outward[0]+matrix[4]*source_outward[1]+matrix[8]*source_outward[2],
                matrix[1]*source_outward[0]+matrix[5]*source_outward[1]+matrix[9]*source_outward[2],
                matrix[2]*source_outward[0]+matrix[6]*source_outward[1]+matrix[10]*source_outward[2]});
            check(p::dot(p::turn(h.rotation,{0,-1,0}),rendered_axis)>.99999f,
                "native palm width follows the actual swept tube axis through yaw, steer and barspin");
            check(p::length(p::sub(h.position,p::grip(frame,left)))>.05f,"wrist-on-tube false positive must no longer qualify as a grip");
            check(p::dot(p::turn(h.rotation,{0,0,left?1.f:-1.f}),p::unit({-matrix[4],-matrix[5],-matrix[6]}))>.95f,
                "both palms face down relative to the tilted articulated bars rather than inherit native gait rotation");
            const auto& fingers=left?f.rig.left_fingers:f.rig.right_fingers;
            for(std::size_t i=0;i<fingers.size();++i) {
                const auto tip=p::turn(p::inverse(h.rotation),p::sub(g[static_cast<std::size_t>(fingers[i].joints[2])].position,h.position));
                check(left?tip[2]>.015f:tip[2]<-.015f,"every finger closes onto the palmar side of the handlebar");
                const auto root=p::turn(p::inverse(h.rotation),p::sub(g[static_cast<std::size_t>(fingers[i].joints[0])].position,h.position));
                check(p::length(p::sub(tip,root))>.01f,"fingers retain their real native articulated lengths");
                for(unsigned segment=0;segment<2;++segment) {
                    auto a=p::sub(g[static_cast<std::size_t>(fingers[i].joints[segment])].position,rendered);
                    auto b=p::sub(g[static_cast<std::size_t>(fingers[i].joints[segment+1])].position,rendered);
                    a=p::sub(a,p::mul(rendered_axis,p::dot(a,rendered_axis)));
                    b=p::sub(b,p::mul(rendered_axis,p::dot(b,rendered_axis)));const auto delta=p::sub(b,a);
                    const float along=std::clamp(-p::dot(a,delta)/p::dot(delta,delta),0.f,1.f);
                    check(p::length(p::add(a,p::mul(delta,along)))>(.0164f*supplied_model_factor+.0015f),
                        "native-length finger segments wrap outside the scaled source knurl with unchanged1.5mm finger clearance instead of cutting through it");
                }
            }
        }
        const auto local_point=[&](int i){return p::turn(p::inverse(p::bike_rotation(frame)),p::sub(g[static_cast<std::size_t>(i)].position,frame.position));};
        if(barspin==0)for(const auto chain:{f.rig.left_arm,f.rig.right_arm}) {
            const auto shoulder=local_point(chain[0]),elbow=local_point(chain[1]),wrist=local_point(chain[2]);
            check(std::abs(elbow[0])<.35f&&elbow[1]<shoulder[1]-.15f&&std::abs(elbow[1]-wrist[1])<.10f,
                "resting elbows remain below the shoulders and near wrist height, without lateral flare or vertical forearms");
        }
        const auto before=f.bones;
        check(p::solve(f.bones,f.rig,frame).ready,"grip supports a second render publication");
        for(std::size_t i=0;i<f.bones.size();++i)for(unsigned a=0;a<3;++a)
            check(f.bones[i].position[a]==before[i].position[a]&&f.bones[i].scale[a]==before[i].scale[a],
                "hand solve never changes segment lengths or native scales");
    }
}
void contact_and_world_placement() {
    for(float yaw:{0.f,1.5707963f,-2.4f})for(float phase:{0.f,1.5707963f,3.1415927f,4.712389f}) {
        auto f=fixture();Frame frame;frame.active=true;frame.position={627,199,1062};frame.yaw=yaw;frame.crank_phase=phase;frame.steer=.25f;
        // The trajectory already carries world placement. An extra application
        // of the entity transform would displace these bones hundreds of metres.
        f.bones[1].position={627,199,1062,17};f.bones[1].rotation=p::axis(1,yaw);
        const auto root=f.bones[1];const auto before=p::compose(f.bones,f.rig);
        const float upper=p::length(p::sub(before[16].position,before[15].position));
        const auto solved=p::solve(f.bones,f.rig,frame);check(solved.ready,"shipped anatomical rig must produce a riding stance");
        check(solved.hand_error<.003f&&solved.foot_error<.003f,"both palm contacts and ankles must meet contacts within 3mm");
        auto g=p::compose(f.bones,f.rig);
        check(p::length(p::sub(g[2].position,p::world(frame,{0,.957898834f,-.287771106f})))<.015f,"small-bike hips remain near the authored neutral request after per-crank native-leg projection");
        check(std::abs(p::length(p::sub(g[16].position,g[15].position))-upper)<.001f,"IK must preserve live arm length");
        const auto local=[&](p::Vec v){return p::turn(p::inverse(p::bike_rotation(frame)),p::sub(v,frame.position));};
        const auto left=local(g[17].position),right=local(g[10].position);
        check(left[0]>.2f&&right[0]<-.2f,"native left/right wrists must not cross over the frame");
        const auto foot=local(g[23].position);
        const float dy=-.189633023f*supplied_model_factor,dz=.148017433f*supplied_model_factor;
        check(std::abs(foot[1]-(.220121164f*supplied_model_factor+.09f+dy*(std::cos(phase)-1)-dz*std::sin(phase)))<.003f&&
            std::abs(foot[2]-(.011273204f*supplied_model_factor+dy*std::sin(phase)+dz*(std::cos(phase)-1)))<.003f,
            "ankle follows source left-low spindle phase with a level platform and sole clearance");
        std::array<dingosdk::bmx::world::Matrix,dingosdk::bmx::world::part_count> matrices;
        check(dingosdk::bmx::world::transforms(frame,matrices),"native crank transform accepts rider frame");
        const auto& m=matrices[6];const p::Vec sole=p::mul(p::Vec{.139242194f,.220121164f,.011273204f},supplied_model_factor);
        const p::Vec rendered{m[0]*sole[0]+m[4]*sole[1]+m[8]*sole[2]+m[12],
            m[1]*sole[0]+m[5]*sole[1]+m[9]*sole[2]+m[13],m[2]*sole[0]+m[6]*sole[1]+m[10]*sole[2]+m[14]};
        check(p::length(p::sub(g[23].position,p::add(rendered,p::turn(p::bike_rotation(frame),{0,.09f,0}))))<.003f,
            "solved ankle stays nine centimetres above the native rendered pedal sole");
        const auto toe=p::turn(g[23].rotation,{1,0,0});
        check(p::dot(toe,p::turn(p::bike_rotation(frame),{0,0,1}))>.999f,"shoe toes face forward rather than inherit walk tilt");
        check(std::memcmp(&root,&f.bones[1],sizeof(root))==0,"trajectory/root ownership must remain native");
        for(const auto& b:f.bones)check(b.position[3]==17&&b.scale[3]==23,"pose metadata W lanes must be preserved");
        const auto old=g;check(p::solve(f.bones,f.rig,frame).ready,"render handoff can safely repeat animation solve");g=p::compose(f.bones,f.rig);
        for(std::size_t i=0;i<g.size();++i)check(p::length(p::sub(old[i].position,g[i].position))<.001f,"animation plus render application must not accumulate transform drift");
    }
}
void rejection() {
    auto f=fixture();Frame frame;const auto original=f.bones;
    f.rig.parents[5]=5;check(!p::solve(f.bones,f.rig,frame).ready,"cyclic hierarchy must reject");
    check(std::memcmp(original.data(),f.bones.data(),f.bones.size()*sizeof(p::Bone))==0,"bad hierarchy must not modify a pose");
    f=fixture();frame.yaw=std::numeric_limits<float>::quiet_NaN();check(!p::solve(f.bones,f.rig,frame).ready,"nonfinite placement must reject");
    f=fixture();frame.yaw=0;f.bones[3].rotation={0,0,0,0};check(!p::solve(f.bones,f.rig,frame).ready,"invalid native quaternion must reject");
    f=fixture();f.rig.left_arm[2]=400;check(!p::solve(f.bones,f.rig,frame).ready,"invalid chain must reject before indexing");
    f=fixture();frame.preload=std::numeric_limits<float>::quiet_NaN();check(!p::solve(f.bones,f.rig,frame).ready,"nonfinite posture cue must reject");
    for(bool hands:{false,true}) {
        f=fixture();frame={};const auto before=f.bones;
        (hands?frame.barspin_release:frame.tailwhip_release)=std::numeric_limits<float>::quiet_NaN();
        check(!p::solve(f.bones,f.rig,frame).ready,"nonfinite trick release rejects before pose mutation");
        check(std::memcmp(before.data(),f.bones.data(),before.size()*sizeof(p::Bone))==0,"invalid release cannot partially modify a native pose");
    }
}
void trick_release_and_catch() {
    constexpr float pi=3.14159265359f;
    struct Phase {float angle,release;};
    constexpr Phase phases[]{{0,0},{.2f,.5f},{.4f,1},{pi*.5f,1},{pi,1},{pi*1.5f,1},{pi*2,1},{pi*2,.5f},{pi*2,0}};
    for(bool barspin:{false,true})for(float yaw:{0.f,1.5707963f,-2.4f}) {
        p::Vec parked_left{},parked_right{};bool captured=false;
        for(const auto& phase:phases) {
            auto f=hands_fixture();Frame frame;frame.active=true;frame.airborne=true;frame.position={627,199,1062};
            frame.yaw=yaw;frame.pitch=.12f;frame.roll=-.1f;frame.steer=.2f;frame.crank_phase=.6f;
            if(barspin){frame.barspin=phase.angle;frame.barspin_release=phase.release;}
            else {frame.tailwhip=phase.angle;frame.tailwhip_release=phase.release;}
            f.bones[1].position={627,199,1062,17};f.bones[1].rotation=p::axis(1,yaw);const auto before=f.bones;
            const auto solved=p::solve(f.bones,f.rig,frame);
            check(solved.ready&&solved.hand_error<.003f&&solved.foot_error<.003f,"release, free rotation and catch must solve anatomical limbs to their intended targets");
            check(std::abs((barspin?solved.hand_contact:solved.foot_contact)-(1-phase.release))<.00001f,"pose exposes released contact separately from target error");
            check((barspin?solved.foot_contact:solved.hand_contact)==1,"barspin retains feet and tailwhip retains hands");
            const auto g=p::compose(f.bones,f.rig);
            const auto palm=[&](bool left){const auto& h=g[static_cast<std::size_t>(left?f.rig.left_arm[2]:f.rig.right_arm[2])];return p::add(h.position,p::turn(h.rotation,p::palm_bar_offset(left)));};
            const auto left=barspin?palm(true):g[static_cast<std::size_t>(f.rig.left_leg[2])].position;
            const auto right=barspin?palm(false):g[static_cast<std::size_t>(f.rig.right_leg[2])].position;
            if(phase.release==1) {
                if(captured)check(p::length(p::sub(left,parked_left))<.003f&&p::length(p::sub(right,parked_right))<.003f,"released limbs must not chase rotating bars or a whipping frame");
                else {parked_left=left;parked_right=right;captured=true;}
                if(barspin)for(const auto* fingers:{&f.rig.left_fingers,&f.rig.right_fingers})for(const auto& finger:*fingers)for(int index:finger.joints)
                    check(f.bones[static_cast<std::size_t>(index)].rotation==f.rig.bind[static_cast<std::size_t>(index)],"released fingers relax instead of gripping an imaginary moving tube");
                if(!barspin) {
                    const auto local=p::turn(p::inverse(p::bike_rotation(frame)),p::sub(left,frame.position));
                    check(local[1]>(.4009181148f*supplied_model_factor+.09f+.409f)&&local[0]>.25f,"tailwhip raises and spreads feet clear of the upper frame");
                    if(phase.angle==pi*2)check(p::length(p::sub(left,p::pedal(frame,true)))>.3f,"full rotation alone cannot claim a foot catch before the controller permits contact");
                }
            }
            if(phase.release==0) {
                check(p::length(p::sub(palm(true),p::grip(frame,true)))<.003f&&p::length(p::sub(g[static_cast<std::size_t>(f.rig.left_leg[2])].position,p::pedal(frame,true)))<.003f,"neutral catch returns palms and shoes to actual native parts");
            }
            check(std::memcmp(&before[1],&f.bones[1],sizeof(p::Bone))==0,"trick pose never changes native trajectory ownership");
            for(std::size_t i=0;i<f.bones.size();++i) {
                check(f.bones[i].scale==before[i].scale&&f.bones[i].position[3]==17,"trick pose preserves scales and metadata");
                if(static_cast<int>(i)!=f.rig.hips)check(f.bones[i].position==before[i].position,"trick IK preserves native segment lengths");
                float norm=0;for(float v:f.bones[i].rotation)norm+=v*v;check(std::isfinite(norm)&&std::abs(norm-1)<.001f,"all trick bone rotations remain finite unit quaternions");
            }
        }
    }
    // A freed foot is independent of continued crank rotation, and a catch
    // ramp has bounded continuous endpoints after the controller finds neutral.
    Frame frame;frame.airborne=true;frame.barspin_release=frame.tailwhip_release=1;
    const auto foot=p::foot_target(frame,true),hand=p::palm_target(frame,true);
    for(float angle:{0.f,pi*.5f,pi,pi*1.5f,pi*2}) {
        frame.crank_phase=frame.tailwhip=frame.barspin=angle;
        check(p::length(p::sub(foot,p::foot_target(frame,true)))<.00001f&&p::length(p::sub(hand,p::palm_target(frame,true)))<.00001f,"fully released targets stay independent of all trick/crank angles");
    }
    frame.crank_phase=frame.tailwhip=frame.barspin=0;auto old_hand=p::palm_target(frame,true),old_foot=p::foot_target(frame,true);
    for(float release:{.9f,.8f,.7f,.6f,.5f,.4f,.3f,.2f,.1f,0.f}) {
        frame.barspin_release=frame.tailwhip_release=release;const auto next_hand=p::palm_target(frame,true),next_foot=p::foot_target(frame,true);
        check(p::length(p::sub(next_hand,old_hand))<.035f&&p::length(p::sub(next_foot,old_foot))<.1f,"controller catch fades contact without a discontinuous target snap");old_hand=next_hand;old_foot=next_foot;
    }
}
void board_visibility_native_refresh() {
    using dingosdk::bmx::rider::apply_mounted_board_hide;
    std::array<p::Bone,16> bones;
    for(std::size_t i=0;i<bones.size();++i) {
        bones[i].scale={1,1,1,23};bones[i].position={static_cast<float>(i),2,3,17};
    }
    // Non-unit trajectory scales are native data too, not values to normalize.
    bones[0].scale={.98f,1.02f,1,23};bones[1].scale={1.1f,.9f,1,23};
    const auto native=bones;
    check(apply_mounted_board_hide<p::Bone>(bones,true),"exact board rig permits mounted render hide");
    auto expected=native;
    for(std::size_t i=2;i<expected.size();++i)for(unsigned a=0;a<3;++a)expected[i].scale[a]=0;
    check(std::memcmp(bones.data(),expected.data(),sizeof(bones))==0,
        "only render descendants scaleXYZ may change; roots and all W/pose metadata stay native");
    // Reproduce the native boundary seen in the live probe: descendants refresh
    // but the root channels can survive unchanged. A root hide would persist.
    for(std::size_t i=2;i<bones.size();++i)bones[i]=native[i];
    check(!apply_mounted_board_hide<p::Bone>(bones,false),"released BMX has no visibility operation");
    check(std::memcmp(bones.data(),native.data(),sizeof(bones))==0,
        "native descendant refresh restores the whole board without a cached root/pose write");
    check(!apply_mounted_board_hide<p::Bone>(std::span<p::Bone>{bones.data(),15},true),"other rig sizes reject before mutation");
    check(std::memcmp(bones.data(),native.data(),sizeof(bones))==0,"rejected rig is untouched");
}
void preload_weight_and_airborne() {
    Frame frame;frame.active=true;frame.position={627,199,1062};frame.yaw=-1.2f;frame.pitch=.12f;frame.roll=-.1f;
    for(float load:{0.f,.5f,1.f})for(float weight:{-1.f,0.f,1.f}){
        auto f=fixture();f.bones[1].position={627,199,1062,17};f.bones[1].rotation=p::axis(1,frame.yaw);
        frame.preload=load;frame.front_weight=weight;frame.rear_weight=weight;
        const auto solved=p::solve(f.bones,f.rig,frame);check(solved.ready&&solved.hand_error<.003f&&solved.foot_error<.003f,
            "crouch and weight transfer retain exact wrists and ankles on pitched/rolled bike");
        const auto g=p::compose(f.bones,f.rig);const auto hip=p::turn(p::inverse(p::bike_rotation(frame)),p::sub(g[2].position,frame.position));
        check(hip[1]>=.8200f&&hip[1]<.99f,"supported small-bike loading stays above measured seat plus native pelvis proxy");
        check(std::abs(hip[2]-(-.287771106f-.035f*load+.09f*weight))<.001f,"front/rear weights move the torso fore and aft within a bounded range");
    }
    auto f=fixture();f.bones[1].position={627,199,1062,17};frame.airborne=true;frame.preload=1;
    check(p::solve(f.bones,f.rig,frame).ready,"airborne extension retains a valid contact pose");
    const auto g=p::compose(f.bones,f.rig);const auto hip=p::turn(p::inverse(p::bike_rotation(frame)),p::sub(g[2].position,frame.position));
    check(std::abs(hip[1]-.997898834f)<.001f,"takeoff releases crouch and extends instead of inheriting held preload");
}
void native_landing_posture_continuity_run155() {
    namespace presentation=dingosdk::bmx::landing_presentation;
    constexpr double h=.01666666567325592;
    struct Sample {std::uint64_t serial;bool grounded;float compression;bool first_contact;};
    // Exact measured support/compression from43FB prepared ramp155. Hold the
    // bike root/rotation fixed to isolate rider posture from physical motion.
    // The real bike was already airborne for29 native samples before940.
    constexpr Sample samples[]{
        {940,false,0.0f,false},
        {941,true,0.7718937993049622f,true},
        {942,false,0.7052271366119385f,false},
        {943,false,0.6385604739189148f,false},
        {944,false,0.5718938112258911f,false},
        {945,true,0.74090975522995f,true},
        {946,true,0.6742430925369263f,false},
        {947,false,0.6075764298439026f,false},
        {948,false,0.5409097671508789f,false},
        {949,false,0.4742431044578552f,false},
        {950,false,0.40757644176483154f,false},
        {951,false,0.34090977907180786f,false},
        {952,false,0.2742431163787842f,false},
        {953,false,0.2075764536857605f,false},
        {954,true,0.14090979099273682f,true},
        {955,true,0.07424312829971313f,false},
        {956,true,0.007576465606689453f,false},
        {957,true,0.0f,false},
        {958,true,0.0f,false},
        {959,true,0.0f,false},
        {960,true,0.0f,false},
        {961,true,0.0f,false},
        {962,true,0.0f,false},
        {963,true,0.0f,false},
        {964,true,0.0f,false},
    };
    float extension=0;
    for(unsigned tick=0;tick<30;++tick)extension=presentation::step_air_extension(extension,h,true).blend;
    float previous_hip=0,previous_target=0,previous_legacy=0,worst_legacy_rebound=0;
    bool observed=false;
    for(const auto& sample:samples) {
        extension=presentation::step_air_extension(extension,h,!sample.grounded).blend;
        Frame frame;frame.airborne=!sample.grounded;frame.landing_compression=sample.compression;
        frame.rider_posture_available=true; // exercise new fore/aft compression while retaining the155 vertical oracle
        frame.air_extension_available=true;frame.air_extension_blend=extension;
        auto f=fixture();const auto solved=p::solve(f.bones,f.rig,frame);
        check(solved.ready&&solved.hand_error<.003f&&solved.foot_error<.003f,
            "measured landing/support toggles keep actual native-proportion palms and feet attached");
        const float hip=p::compose(f.bones,f.rig)[static_cast<std::size_t>(f.rig.hips)].position[1];
        const float target=.957898834f+.04f*extension-.08f*sample.compression;
        const float legacy=.917898834f+(sample.grounded?0.f:.04f)-(sample.grounded?.08f*sample.compression:0.f);
        if(observed) {
            worst_legacy_rebound=std::max(worst_legacy_rebound,legacy-previous_legacy);
            check(target-previous_target<.009f&&hip-previous_hip<.015f,
                "real155 support loss cannot create the former9-10cm upward pelvis rebound");
            if(!sample.first_contact) {
                check(std::abs(target-previous_target)<.009f&&std::abs(hip-previous_hip)<.015f,
                    "non-impact native samples keep both posture target and solved pelvis continuous");
            }
            // Fresh impact compression remains deliberately immediate; it may
            // absorb downward faster than ordinary recovery/extension movement.
        }
        previous_hip=hip;previous_target=target;previous_legacy=legacy;observed=true;
    }
    check(worst_legacy_rebound>.10f,
        "recorded regression includes the old10cm binary support rebound, not only a synthetic smooth case");
    // No extension or recovery from an old ride may leak into a new mount.
    Frame remount;remount.air_extension_available=true;
    auto f=fixture();check(p::solve(f.bones,f.rig,remount).ready,"new mount with reset presentation remains valid");
    check(std::abs(p::compose(f.bones,f.rig)[2].position[1]-.957898834f)<.001f,
        "new mount starts at neutral hip height after both bridge presentation scalars reset");
    for(float bad:{-.01f,1.01f,std::numeric_limits<float>::quiet_NaN()}) {
        auto invalid=fixture();const auto before=invalid.bones;Frame frame;
        frame.air_extension_available=true;frame.air_extension_blend=bad;
        check(!p::solve(invalid.bones,invalid.rig,frame).ready&&
              std::memcmp(before.data(),invalid.bones.data(),before.size()*sizeof(p::Bone))==0,
            "malformed native extension fails before mutating any pose lane");
    }
}
void full_chassis_rotation() {
    using namespace dingosdk::bmx;
    constexpr float pi=3.14159265359f;
    const auto matrix_point=[](const world::Matrix& m,p::Vec v){return p::Vec{
        m[0]*v[0]+m[4]*v[1]+m[8]*v[2]+m[12],m[1]*v[0]+m[5]*v[1]+m[9]*v[2]+m[13],
        m[2]*v[0]+m[6]*v[1]+m[10]*v[2]+m[14]};};
    for(const auto q:{p::Quat{.5f,.5f,.5f,.5f},p::axis(0,pi),
            p::multiply(p::axis(1,.71f),p::multiply(p::axis(0,pi*.5f-.000001f),p::axis(2,-1.3f))),
            p::multiply(p::axis(0,pi),p::multiply(p::axis(2,1.43f),p::axis(1,-.82f)))}) {
        Frame frame;frame.active=true;frame.chassis_quaternion_available=true;frame.chassis_quaternion=q;
        check(chassis::root_from_com(std::array<float,4>{627,201,1062,std::numeric_limits<float>::infinity()},
            q,{.13f,.85f,-.21f},frame.position),"measured COM becomes bike-root position before pose publication");
        frame.yaw=frame.pitch=frame.roll=std::numeric_limits<float>::quiet_NaN();
        frame.steer=.3f;frame.barspin=-.2f;frame.tailwhip=.35f;frame.crank_phase=.8f;
        std::array<world::Matrix,dingosdk::bmx::world::part_count> matrices;
        check(world::transforms(frame,matrices),"full quaternion native part transforms accepted");
        auto f=hands_fixture();
        // The native trajectory has its own valid orientation. Solving a chassis
        // flip must compensate relative to it, without rewriting that trajectory.
        f.bones[1].position={627,199,1062,17};f.bones[1].rotation=p::axis(1,-.45f);
        const auto before=f.bones;
        const auto solved=p::solve(f.bones,f.rig,frame);
        check(solved.ready&&solved.hand_error<.003f&&solved.foot_error<.003f,
            "full chassis flips and near-vertical combined rotations preserve anatomical hand and foot contacts");
        const auto composed=p::compose(f.bones,f.rig);
        for(bool left:{false,true}) {
            const auto& hand=composed[static_cast<std::size_t>(left?f.rig.left_arm[2]:f.rig.right_arm[2])];
            // Authored mesh endpoints, independent of pose target functions.
            const p::Vec authored=p::mul(left?p::Vec{.334575045f,1.231948059f,.340623945f}:
                p::Vec{-.334575042f,1.231948045f,.340623981f},supplied_model_factor);
            const p::Vec source_outward=left?p::Vec{.942691934f,.113109012f,-.313908057f}:
                p::Vec{-.942691915f,.113108999f,-.313908120f};
            const auto mesh_center=matrix_point(matrices[5],authored);
            const auto palm=p::add(hand.position,p::turn(hand.rotation,p::palm_bar_offset(left)));
            check(p::length(p::sub(palm,mesh_center))<.003f,"flipped rider palm meets the actual six-part handlebars matrix");
            const auto mesh_outer=matrix_point(matrices[5],p::add(authored,source_outward));
            check(p::dot(p::turn(hand.rotation,{0,-1,0}),p::unit(p::sub(mesh_outer,mesh_center)))>.99999f,
                "full quaternion preserves left/right palm orientation along the actual swept grip");
            const auto& platform=matrices[left?6:7];
            const auto mesh_sole=matrix_point(platform,p::mul(left?p::Vec{.139242194f,.220121164f,.011273204f}:
                p::Vec{-.139242213f,.599275463f,-.284814701f},supplied_model_factor));
            const auto rotated_up=p::mul(p::Vec{platform[4],platform[5],platform[6]},.09f);
            const auto& ankle=composed[static_cast<std::size_t>(left?f.rig.left_leg[2]:f.rig.right_leg[2])];
            check(p::length(p::sub(ankle.position,p::add(mesh_sole,rotated_up)))<.003f,
                "flipped ankles share crank part placement with a rotated nine-centimetre sole clearance");
        }
        check(std::memcmp(&before[1],&f.bones[1],sizeof(p::Bone))==0,"full chassis quaternion cannot rewrite native trajectory ownership");
        for(std::size_t i=0;i<f.bones.size();++i) {
            check(f.bones[i].scale==before[i].scale&&f.bones[i].position[3]==before[i].position[3],
                "flipped pose preserves native scale and W metadata");
            if(static_cast<int>(i)!=f.rig.hips)check(f.bones[i].position==before[i].position,"flipped IK preserves native segment lengths");
        }
        for(auto& component:frame.chassis_quaternion)component=-component;
        auto opposite=hands_fixture();opposite.bones[1]=before[1];
        check(p::solve(opposite.bones,opposite.rig,frame).ready,"antipodal full chassis quaternion solves");
        const auto antipodal=p::compose(opposite.bones,opposite.rig);
        for(std::size_t i=0;i<composed.size();++i)
            check(p::length(p::sub(composed[i].position,antipodal[i].position))<.001f,"q and -q yield identical rider bone world placement");
    }
    // Available but invalid measured orientation fails before any bind/IK write.
    for(const auto bad:{p::Quat{},p::Quat{0,0,0,2},p::Quat{0,0,0,1.01f},
            p::Quat{std::numeric_limits<float>::quiet_NaN(),0,0,1},p::Quat{0,0,std::numeric_limits<float>::infinity(),1}}) {
        auto f=hands_fixture();const auto before=f.bones;Frame frame;
        frame.chassis_quaternion_available=true;frame.chassis_quaternion=bad;
        check(!p::solve(f.bones,f.rig,frame).ready,"nonunit or nonfinite chassis quaternion rejected");
        check(std::memcmp(before.data(),f.bones.data(),before.size()*sizeof(p::Bone))==0,"invalid chassis quaternion never partially modifies native pose");
    }
    auto a=fixture(),b=a;Frame frame;frame.yaw=.4f;frame.pitch=.2f;frame.roll=-.3f;
    check(p::solve(a.bones,a.rig,frame).ready,"legacy Euler pose solves before optional quaternion");
    frame.chassis_quaternion={std::numeric_limits<float>::quiet_NaN(),2,3,4};
    check(p::solve(b.bones,b.rig,frame).ready&&std::memcmp(a.bones.data(),b.bones.data(),a.bones.size()*sizeof(p::Bone))==0,
        "unavailable chassis quaternion leaves the legacy Euler pose byte-for-byte unchanged");
}
void catalog_movements() {
    using namespace dingosdk::bmx;
    constexpr float pi=3.14159265359f;
    for(const auto& entry:catalog::entries)for(float phase:{0.f,.1f,.15f,.25f,.5f,.75f,.9f,1.f}) {
        auto f=hands_fixture();Frame frame;frame.airborne=true;frame.movement_id=entry.id;
        const float smooth=phase*phase*(3-2*phase),release=std::min(1.f,phase/.15f);
        frame.movement_phase=entry.motion==catalog::Motion::pose?smooth:std::max(0.f,std::sin(pi*phase));frame.movement_progress=phase;
        frame.release_left_hand=(entry.release_mask&catalog::left_hand)?release:0;
        frame.release_right_hand=(entry.release_mask&catalog::right_hand)?release:0;
        frame.release_left_foot=(entry.release_mask&catalog::left_foot)?release:0;
        frame.release_right_foot=(entry.release_mask&catalog::right_foot)?release:0;
        frame.barspin=static_cast<float>(entry.bar_turns)*2*pi*smooth;
        frame.barspin+=static_cast<float>(entry.bar_pose_radians)*frame.movement_phase;
        frame.tailwhip=static_cast<float>(entry.tail_turns)*2*pi*smooth;
        frame.crank_phase=static_cast<float>(entry.crank_turns)*2*pi*smooth;
        frame.movement_bike_pitch=static_cast<float>(entry.bike_pitch)*smooth;
        frame.movement_bike_roll=static_cast<float>(entry.bike_roll)*smooth;
        if(entry.family==catalog::Family::grand_slam)frame.movement_bike_roll=static_cast<float>(entry.bike_roll)*std::sin(2*pi*smooth);
        if(entry.family==catalog::Family::bri_flip)frame.movement_bike_pitch=static_cast<float>(entry.bike_pitch)*std::sin(2*pi*smooth);
        frame.position={627,199,1062};frame.chassis_quaternion_available=true;
        frame.chassis_quaternion=p::multiply(p::axis(1,.7f),p::multiply(p::axis(0,.3f),p::axis(2,-.25f)));
        f.bones[1].position={627,199,1062,17};const auto before=f.bones;
        const auto result=p::solve(f.bones,f.rig,frame);
        if(!result.ready||result.hand_error>.03f||result.foot_error>.03f) {
            std::cerr<<"movement "<<entry.name<<" phase="<<phase<<" hand="<<result.hand_error<<" foot="<<result.foot_error<<'\n';
            const auto actual=p::compose(f.bones,f.rig);
            const auto hip=p::turn(p::inverse(p::bike_rotation(frame)),p::sub(actual[static_cast<std::size_t>(f.rig.hips)].position,frame.position));
            const auto target=p::turn(p::inverse(p::bike_rotation(frame)),p::sub(p::foot_target(frame,false),frame.position));
            std::cerr<<"hip "<<hip[0]<<','<<hip[1]<<','<<hip[2]<<" targetRF "<<target[0]<<','<<target[1]<<','<<target[2]<<'\n';
            for(bool left:{false,true})std::cerr<<"foot "<<left<<" error="<<p::length(p::sub(actual[static_cast<std::size_t>(left?f.rig.left_leg[2]:f.rig.right_leg[2])].position,p::foot_target(frame,left)))<<'\n';
        }
        check(result.ready&&result.hand_error<.03f&&result.foot_error<.03f,"every authored selection has a reachable bounded family pose within three centimetres");
        std::array<world::Matrix,dingosdk::bmx::world::part_count> matrices;check(world::transforms(frame,matrices),"catalog visual world matrices are valid");
        const auto composed=p::compose(f.bones,f.rig);
        for(bool left:{false,true}) {
            const auto& hand=composed[static_cast<std::size_t>(left?f.rig.left_arm[2]:f.rig.right_arm[2])];
            const auto palm=p::add(hand.position,p::turn(hand.rotation,p::palm_bar_offset(left)));
            const auto ankle=composed[static_cast<std::size_t>(left?f.rig.left_leg[2]:f.rig.right_leg[2])].position;
            if(p::hand_release(frame,left)==0) {
                if(p::length(p::sub(palm,p::grip(frame,left)))>=.003f)std::cerr<<entry.name<<" progress="<<phase<<" retained hand "<<left<<" error="<<p::length(p::sub(palm,p::grip(frame,left)))<<'\n';
                check(p::length(p::sub(palm,p::grip(frame,left)))<.003f,"catalog motions retain each unreleased palm on its actual moving grip");
            }
            if(p::foot_release(frame,left)==0) {
                if(p::length(p::sub(ankle,p::pedal(frame,left)))>=.003f)std::cerr<<entry.name<<" progress="<<phase<<" retained foot "<<left<<" error="<<p::length(p::sub(ankle,p::pedal(frame,left)))<<'\n';
                check(p::length(p::sub(ankle,p::pedal(frame,left)))<.003f,"catalog motions retain each unreleased foot on its actual moving pedal");
                const auto& foot=composed[static_cast<std::size_t>(left?f.rig.left_leg[2]:f.rig.right_leg[2])];
                check(p::dot(p::turn(foot.rotation,{0,-1,0}),p::Vec{matrices[left?6:7][4],matrices[left?6:7][5],matrices[left?6:7][6]})>.99999f,
                    "a retained shoe sole follows the actual articulated pedal plane");
            }
        }
        for(std::size_t i=0;i<f.bones.size();++i) {
            check(f.bones[i].scale==before[i].scale&&f.bones[i].position[3]==before[i].position[3],"catalog poses preserve native scale/W metadata");
            if(static_cast<int>(i)!=f.rig.hips)check(f.bones[i].position==before[i].position,"catalog poses preserve native limb lengths and root ownership");
            float norm=0;for(float v:f.bones[i].rotation)norm+=v*v;
            check(std::isfinite(norm)&&std::abs(norm-1)<.001f,"all catalog limb/finger rotations remain finite and normalized");
        }
    }
    // Distinct family geometry rather than a name-only static rider. These
    // expected directions are independent intent checks, not solver snapshots.
    Frame frame;frame.airborne=true;frame.movement_phase=1;
    frame.movement_id=catalog::TrickId::superman;frame.release_left_foot=frame.release_right_foot=1;
    check(p::foot_target(frame,true)[2]<-.85f,"Superman visibly extends released feet behind the bike");
    frame={};frame.airborne=true;frame.movement_phase=1;frame.movement_id=catalog::TrickId::christ_air;
    frame.release_left_hand=frame.release_right_hand=1;
    check(p::palm_target(frame,true)[0]>.65f&&p::palm_target(frame,false)[0]<-.65f,"Christ Air spreads both hands outward");
    frame.movement_id=catalog::TrickId::tuck_no;
    check(p::palm_target(frame,true)[1]>1.45f,"Tuck No raises the freed hands above the bars");
    frame.movement_id=catalog::TrickId::cannon_ball;frame.release_left_foot=1;
    check(p::foot_target(frame,true)[1]>.95f&&p::palm_target(frame,true)[1]<1.1f,"Cannon Ball draws feet and hands together in a tuck");
    // Every directional pair mirrors its free targets without swapping release
    // ownership. Actual left/right grip backsweep remains deliberately asymmetric.
    for(const auto pair:{std::array{catalog::TrickId::one_hander_left,catalog::TrickId::one_hander_right},
            std::array{catalog::TrickId::one_footer_left,catalog::TrickId::one_footer_right},
            std::array{catalog::TrickId::can_left,catalog::TrickId::can_right},
            std::array{catalog::TrickId::nac_left,catalog::TrickId::nac_right}}) {
        Frame a;a.movement_id=pair[0];a.movement_phase=1;Frame b=a;b.movement_id=pair[1];
        const auto x=rider::motion::sample(a),y=rider::motion::sample(b);
        for(const auto* targets:{&x.palms,&x.ankles}) {
            const auto& rhs=targets==&x.palms?y.palms:y.ankles;
            check(std::abs((*targets)[0][0]+rhs[1][0])<.00001f&&std::abs((*targets)[0][1]-rhs[1][1])<.00001f&&
                std::abs((*targets)[0][2]-rhs[1][2])<.00001f,"left/right adapted free targets reflect anatomically");
        }
    }
    // A zero extension/release returns exactly to the old neutral pose, and
    // invalid new input rejects before any native pose-lane changes.
    auto neutral=hands_fixture(),zero=neutral;Frame base;base.airborne=true;
    check(p::solve(neutral.bones,neutral.rig,base).ready,"baseline neutral pose");
    base.movement_id=catalog::TrickId::nothing;
    check(p::solve(zero.bones,zero.rig,base).ready&&std::memcmp(neutral.bones.data(),zero.bones.data(),zero.bones.size()*sizeof(p::Bone))==0,
        "zero movement phase/release returns byte-identically to neutral");
    for(float bad:{-1.f,1.01f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        auto f=hands_fixture();const auto before=f.bones;Frame invalid;invalid.movement_phase=bad;
        check(!p::solve(f.bones,f.rig,invalid).ready&&std::memcmp(before.data(),f.bones.data(),before.size()*sizeof(p::Bone))==0,
            "invalid movement envelope cannot partially mutate pose");
    }
    auto f=hands_fixture();const auto before=f.bones;Frame invalid;invalid.movement_id=static_cast<catalog::TrickId>(9);
    check(!p::solve(f.bones,f.rig,invalid).ready&&std::memcmp(before.data(),f.bones.data(),before.size()*sizeof(p::Bone))==0,"unknown authored movement ID fails before mutation");
    // Landing recovery compresses without leaving contacted palms/pedals.
    f=hands_fixture();Frame land;land.landing_compression=1;land.pedal_effort=1;land.crank_phase=pi*.25f;
    const auto settled=p::solve(f.bones,f.rig,land);check(settled.ready&&settled.hand_error<.003f&&settled.foot_error<.003f,"land compression and crank-driven pedal bob preserve contacts");
    const auto g=p::compose(f.bones,f.rig);check(std::abs(g[2].position[1]-(.957898834f-.08f+.012f))<.001f,"landing/pedal posture uses bounded visible displacement");
    Frame free;free.movement_id=catalog::TrickId::grand_slam_left;free.movement_phase=1;
    free.release_left_hand=free.release_right_hand=1;const auto relaxed=p::hand_rotation(free,true);
    free.movement_bike_pitch=2.1f;free.movement_bike_roll=-.8f;
    check(p::length(p::sub(p::turn(relaxed,{1,0,0}),p::turn(p::hand_rotation(free,true),{1,0,0})))<.00001f,
        "a fully released wrist cannot inherit the freely rotating bike orientation");
}
void carrying_and_transitions() {
    using namespace dingosdk::bmx;
    for(float yaw:{0.f,.7f,-2.2f})for(float walking:{-.15f,0.f,.15f}) {
        auto f=hands_fixture();f.bones[1].rotation=p::axis(1,yaw);
        f.bones[static_cast<std::size_t>(f.rig.left_leg[0])].rotation=p::multiply(p::axis(0,walking),f.rig.bind[static_cast<std::size_t>(f.rig.left_leg[0])]);
        f.bones[static_cast<std::size_t>(f.rig.right_leg[0])].rotation=p::multiply(p::axis(0,-walking),f.rig.bind[static_cast<std::size_t>(f.rig.right_leg[0])]);
        f.bones[static_cast<std::size_t>(f.rig.spine[0])].rotation=p::multiply(p::axis(1,walking*.2f),f.rig.bind[static_cast<std::size_t>(f.rig.spine[0])]);
        const auto before=f.bones;Frame frame;frame.rider_mode=visual::RiderMode::carrying;frame.rider_blend=0;
        frame.position=p::turn(p::axis(1,yaw),rider::motion::carry_bike_offset);frame.yaw=yaw;frame.roll=.25f;
        const auto result=p::solve(f.bones,f.rig,frame);
        if(!result.ready)std::cerr<<"carry error="<<result.hand_error<<'\n';
        check(result.ready&&result.hand_error<.003f,"carry hand reaches the proposed top-tube proxy while native walking continues");
        std::vector<int> allowed(f.rig.right_arm.begin(),f.rig.right_arm.end());
        for(const auto& finger:f.rig.right_fingers) {
            if(finger.metacarpal>=0)allowed.push_back(finger.metacarpal);
            for(int i:finger.joints)if(i>=0)allowed.push_back(i);
        }
        for(std::size_t i=0;i<f.bones.size();++i) {
            if(std::find(allowed.begin(),allowed.end(),static_cast<int>(i))==allowed.end())
                check(std::memcmp(&f.bones[i],&before[i],sizeof(p::Bone))==0,"carry cannot override native hips, spine, head, left arm or walking legs");
            check(f.bones[i].position==before[i].position&&f.bones[i].scale==before[i].scale,"carry preserves every native translation, scale and W metadata lane");
        }
        check(result.foot_contact==0,"carry makes no pedal-contact claim");
    }
    auto source=hands_fixture();source.bones[2].position[1]+=.04f;
    source.bones[9].rotation=p::multiply(p::axis(0,.2f),source.bones[9].rotation);
    Frame frame;auto full=source;check(p::solve(full.bones,full.rig,frame).ready,"full riding target for transition");
    for(float amount:{0.f,.25f,.5f,.75f,1.f}) {
        auto next=source;frame.rider_blend=amount;const auto result=p::solve(next.bones,next.rig,frame);
        check(result.ready,"native-to-riding transition is valid");
        if(amount==0)check(std::memcmp(next.bones.data(),source.bones.data(),source.bones.size()*sizeof(p::Bone))==0,"zero blend is exactly the current native pose");
        if(amount==1)check(std::memcmp(next.bones.data(),full.bones.data(),full.bones.size()*sizeof(p::Bone))==0,"full blend is exactly the contact solution");
        const auto indices=p::changed(next.rig);
        for(std::size_t i=0;i<next.bones.size();++i) {
            check(next.bones[i].scale==source.bones[i].scale&&next.bones[i].position[3]==source.bones[i].position[3],"transition preserves metadata/scale");
            if(static_cast<int>(i)!=next.rig.hips)check(next.bones[i].position==source.bones[i].position,"transition preserves every segment/root translation");
            if(std::find(indices.begin(),indices.end(),static_cast<int>(i))==indices.end())check(next.bones[i].rotation==source.bones[i].rotation,"transition cannot touch unowned joints");
        }
        check(std::abs(next.bones[2].position[1]-(source.bones[2].position[1]+(full.bones[2].position[1]-source.bones[2].position[1])*amount))<.00001f,
            "mount/exit hip translation blends from fresh native input rather than a stale saved pose");
    }
    auto none=source;frame.rider_mode=visual::RiderMode::none;
    check(!p::solve(none.bones,none.rig,frame).ready&&std::memcmp(none.bones.data(),source.bones.data(),source.bones.size()*sizeof(p::Bone))==0,"parked/walking mode has no pose operation");
}
void native_mount_contacts() {
    float worst_support=0,worst_hand=0,worst_intermediate=0;std::array<float,3> supports{},errors{};unsigned crossed_tube=0;
    for(bool from_left:{false,true})for(float yaw:{0.f,.8f,-2.2f})for(float distance:{.12f,.65f,.7f})for(float turn:{0.f,1.57f,3.14f}) {
        const auto original=hands_fixture();p::EntryContacts contacts;
        Frame frame;frame.active=true;frame.entity=17;frame.mount_token=31;frame.mount_progress=0;
        frame.mount_from_left=from_left;frame.yaw=yaw;frame.position={627,199,1062};
        const auto q=p::axis(1,yaw);const float side=from_left?1.f:-1.f;
        auto standing=original;standing.bones[1].position={627,199,1062,17};
        const auto stance=p::add(frame.position,p::turn(q,{side*distance,0,0}));
        for(unsigned a=0;a<3;++a)standing.bones[1].position[a]=stance[a];
        standing.bones[1].rotation=p::multiply(q,p::axis(1,turn));const auto before=standing.bones;
        check(p::solve(standing.bones,standing.rig,frame,&contacts).ready,"mount captures the fresh supported native pose");
        check(std::memcmp(standing.bones.data(),before.data(),before.size()*sizeof(p::Bone))==0,"entry starts byte-exactly at current native pose");
        const auto anchors=contacts;
        float previous_crossing=p::turn(p::inverse(q),p::sub(anchors.ankles[from_left?1:0],frame.position))[0];
        for(unsigned step=1;step<100;++step) {
            const float t=step/100.f;auto current=original;
            const float root_progress=dingosdk::bmx::mount_transition::phase(t,0,dingosdk::bmx::mount_transition::root_phase_end);
            const auto native_root=p::blend(stance,frame.position,root_progress);
            current.bones[1].position={native_root[0],native_root[1],native_root[2],17};
            current.bones[1].rotation=dingosdk::bmx::mount_transition::shortest_arc(standing.bones[1].rotation,q,
                root_progress);
            const auto native=current.bones;frame.mount_progress=t;
            const auto result=p::solve(current.bones,current.rig,frame,&contacts);
            check(result.ready,"phased mount remains finite over the supported native rig");
            const auto g=p::compose(current.bones,current.rig);
            const auto support=static_cast<std::size_t>(from_left?current.rig.left_leg[2]:current.rig.right_leg[2]);
            const unsigned range=distance<.2f?0:distance<.68f?1:2;
            if(t<.45f){const float error=p::length(p::sub(g[support].position,anchors.ankles[from_left?0:1]));
                worst_support=std::max(worst_support,error);supports[range]=std::max(supports[range],error);}
            worst_intermediate=std::max({worst_intermediate,result.hand_error,result.foot_error});
            errors[range]=std::max({errors[range],result.hand_error,result.foot_error});
            if(t>.9f)worst_hand=std::max(worst_hand,result.hand_error);
            check(result.hand_error<.003f&&result.foot_error<.003f,"all intermediate native chain targets stay reachable during actual root/turn timing");
            const auto crossing=static_cast<std::size_t>(from_left?current.rig.right_leg[2]:current.rig.left_leg[2]);
            const auto local_crossing=p::turn(p::inverse(q),p::sub(g[crossing].position,frame.position));
            if(previous_crossing*local_crossing[0]<0) {
                ++crossed_tube;check(local_crossing[1]>
                    dingosdk::bmx::geometry::carry_tube_center[1]+dingosdk::bmx::geometry::carry_tube_radius+
                    dingosdk::bmx::geometry::ankle_from_sole,"crossing ankle clears the supplied top tube while passing its plane");
            }
            previous_crossing=local_crossing[0];
            check(contacts.ankles==anchors.ankles&&contacts.palms==anchors.palms,"root following never rebases planted contacts to moving native input");
            const auto owned=p::changed(current.rig);
            for(std::size_t i=0;i<current.bones.size();++i) {
                check(current.bones[i].scale==native[i].scale&&current.bones[i].position[3]==native[i].position[3],"entry preserves native metadata and scale");
                if(static_cast<int>(i)!=current.rig.hips)check(current.bones[i].position==native[i].position,"entry cannot stretch any native limb or rewrite root translation");
                if(std::find(owned.begin(),owned.end(),static_cast<int>(i))==owned.end())check(current.bones[i].rotation==native[i].rotation,"entry leaves unowned native joints untouched");
            }
            if(t<.45f)check(result.foot_contact==0,"planted support cannot be reported as both pedal contacts");
        }
        auto completed=original;completed.bones[1].position={627,199,1062,17};completed.bones[1].rotation=q;
        frame.mount_progress=1;auto expected=completed;auto full=frame;full.mount_token=0;
        check(p::solve(completed.bones,completed.rig,frame,&contacts).ready&&p::solve(expected.bones,expected.rig,full).ready,
            "entry completion accepts the measured riding solution");
        check(std::memcmp(completed.bones.data(),expected.bones.data(),completed.bones.size()*sizeof(p::Bone))==0,"completion exactly converges to existing full contact solve");
        frame.mount_progress=0;frame.mount_token=32;frame.entity=18;
        check(p::solve(completed.bones,completed.rig,frame,&contacts).ready&&contacts.entity==18&&contacts.token==32,"new native body/rider replaces previous entry anchors");
        auto rejected=completed;frame.mount_progress=std::numeric_limits<float>::quiet_NaN();
        check(!p::solve(rejected.bones,rejected.rig,frame,&contacts).ready&&std::memcmp(rejected.bones.data(),completed.bones.data(),completed.bones.size()*sizeof(p::Bone))==0,"invalid entry phase fails before a native pose mutation");
        frame.mount_progress=.6f;contacts.ankles[0][0]+=5;rejected=completed;
        check(!p::solve(rejected.bones,rejected.rig,frame,&contacts).ready&&std::memcmp(rejected.bones.data(),completed.bones.data(),completed.bones.size()*sizeof(p::Bone))==0,
            "unreachable entry contact rejects rather than stretch a segment or partially overwrite native pose");
    }
    std::cout<<"Mount world anchor errors: support="<<worst_support<<"m, settled palms="<<worst_hand<<"m, intermediate max="<<worst_intermediate<<"m.\n";
    for(unsigned i=0;i<3;++i)std::cout<<"Mount range "<<i<<": support="<<supports[i]<<" intermediate="<<errors[i]<<'\n';
    check(worst_support<.003f,"reachable planted support remains fixed in world space during rider-root movement");
    check(worst_hand<.003f,"settled grips stay attached while the support foot steps onto the pedal");
    check(crossed_tube>0,"native-rig fixtures actually exercise a step across the measured frame plane");
}
void walking_carry_envelope() {
    using namespace dingosdk::bmx;
    float maximum_error=0;unsigned rejected_old=0;
    for(float yaw:{0.f,.8f,-2.2f})for(float hip_height:{-.03f,0.f,.06f,.10f})
        for(float hip_pitch:{-.08f,0.f,.08f})for(float spine_lean:{-.15f,0.f,.15f})for(float shoulder_swing:{-.2f,.2f}) {
        auto f=hands_fixture();f.bones[1].position={627,199,1062,17};f.bones[1].rotation=p::axis(1,yaw);
        f.bones[2].position[1]+=hip_height;f.bones[2].rotation=p::multiply(p::axis(0,hip_pitch),f.bones[2].rotation);
        f.bones[3].rotation=p::multiply(f.bones[3].rotation,p::axis(2,spine_lean));
        f.bones[static_cast<std::size_t>(f.rig.shoulders[1])].rotation=p::multiply(
            f.bones[static_cast<std::size_t>(f.rig.shoulders[1])].rotation,p::axis(0,shoulder_swing));
        // Native walking arms/legs vary; their segment translations and all
        // non-right-arm rotations must remain owned by the animation graph.
        f.bones[static_cast<std::size_t>(f.rig.left_arm[0])].rotation=p::multiply(p::axis(0,shoulder_swing),f.bones[static_cast<std::size_t>(f.rig.left_arm[0])].rotation);
        f.bones[static_cast<std::size_t>(f.rig.left_leg[0])].rotation=p::multiply(p::axis(0,shoulder_swing),f.bones[static_cast<std::size_t>(f.rig.left_leg[0])].rotation);
        f.bones[static_cast<std::size_t>(f.rig.right_leg[0])].rotation=p::multiply(p::axis(0,-shoulder_swing),f.bones[static_cast<std::size_t>(f.rig.right_leg[0])].rotation);
        const auto original=f.bones;Frame carry;carry.rider_mode=visual::RiderMode::carrying;carry.yaw=yaw;carry.roll=.25f;
        carry.position=p::add({627,199,1062},p::turn(p::axis(1,yaw),rider::motion::carry_bike_offset));
        const auto solved=p::solve(f.bones,f.rig,carry);maximum_error=std::max(maximum_error,solved.hand_error);
        if(!solved.ready)std::cerr<<"walking carry reach: hip="<<hip_height<<" pitch="<<hip_pitch<<" spine="<<spine_lean<<" error="<<solved.hand_error<<'\n';
        check(solved.ready&&solved.hand_error<.003f,"raised/inward carry placement retains the tube grip throughout a varied native walking envelope");
        for(std::size_t i=0;i<f.bones.size();++i) {
            bool right=std::find(f.rig.right_arm.begin(),f.rig.right_arm.end(),static_cast<int>(i))!=f.rig.right_arm.end();
            for(const auto& finger:f.rig.right_fingers)right=right||finger.metacarpal==static_cast<int>(i)||std::find(finger.joints.begin(),finger.joints.end(),static_cast<int>(i))!=finger.joints.end();
            if(!right)check(std::memcmp(&f.bones[i],&original[i],sizeof(p::Bone))==0,"carry walking envelope leaves all native body/left-arm/leg lanes byte-identical");
            check(f.bones[i].position==original[i].position&&f.bones[i].scale==original[i].scale,"carry keeps every segment translation, scale and W lane");
        }
        auto old=f;old.bones=original;carry.position=p::add({627,199,1062},p::turn(p::axis(1,yaw),{-.40f,.35f,0}));
        if(!p::solve(old.bones,old.rig,carry).ready)++rejected_old;
    }
    check(rejected_old>0,"walking fixture exposes the old carry placement overreach rather than only testing the new implementation");
    std::cout<<"Carry walking envelope max error "<<maximum_error<<" m; old placement rejects "<<rejected_old<<" poses.\n";
}
void supplied_geometry_reach() {
    using namespace dingosdk::bmx;
    float hand_max=0,foot_max=0,pelvis_adjustment=0;
    for(unsigned step=0;step<32;++step)for(float preload:{0.f,.5f,1.f})
        for(float steer:{-.4f,0.f,.4f})for(bool air:{false,true}) {
        auto f=hands_fixture();Frame frame;frame.crank_phase=step*6.28318530718f/32;
        frame.preload=preload;frame.steer=steer;frame.airborne=air;
        const auto result=p::solve(f.bones,f.rig,frame);
        check(result.ready&&result.hand_error<.003f&&result.foot_error<.003f,
            "full source crank cycle, preload and steering preserve strict native-proportion contacts");
        hand_max=std::max(hand_max,result.hand_error);foot_max=std::max(foot_max,result.foot_error);
        const auto g=p::compose(f.bones,f.rig);
        const p::Vec desired{0,std::max(.820228576f,.957898834f-(air?0:.20f*preload)+(air?.04f:0)),-.287771106f-(air?0:.035f*preload)};
        pelvis_adjustment=std::max(pelvis_adjustment,p::length(p::sub(g[2].position,desired)));
        check(p::length(p::sub(g[2].position,desired))<=.1801f,
            "geometry reach adjustment remains a bounded pelvis cue, never a stretched segment");
        std::array<world::Matrix,world::part_count> matrices;check(world::transforms(frame,matrices),"source pedal transforms valid");
        for(bool left:{false,true}) {
            const auto& foot=g[static_cast<std::size_t>(left?f.rig.left_leg[2]:f.rig.right_leg[2])];
            const auto& platform=matrices[left?6:7];
            check(p::dot(p::turn(foot.rotation,{0,-1,0}),p::Vec{platform[4],platform[5],platform[6]})>.99999f,
                "actual platform and shoe normal stay level for every crank phase, including half rotation");
        }
    }
    std::cout<<"Source geometry reach max hand="<<hand_max<<" foot="<<foot_max<<" pelvis adjustment="<<pelvis_adjustment<<" m.\n";
}
Fixture with_native_toes(Fixture f) {
    // Exact ToeBase LocalPose from the supported render skeleton. Foot+X is
    // ankle-to-ball; the ToeBase bind frame is tilted28.106degrees relative
    // to it. A foot-axis-only fixture cannot detect this convention error.
    for(bool left:{false,true}) {
        const int index=static_cast<int>(f.bones.size());
        p::Bone bone;bone.position={.112405f,0,0,17};bone.scale[3]=23;
        bone.rotation=p::orientation({.882076f,-.471108f,0},{.471108f,.882076f,0},{0,0,1});
        f.rig.toe_base[left?0:1]=index;
        f.rig.parents.push_back(left?f.rig.left_leg[2]:f.rig.right_leg[2]);
        f.rig.bind.push_back(bone.rotation);f.bones.push_back(bone);
    }
    return f;
}
void native_foot_bind_plane() {
    using namespace dingosdk::bmx;
    const auto plane=p::orientation({0,0,1},{0,-1,0},{1,0,0});
    const auto toe_bind=p::orientation({.882076f,-.471108f,0},{.471108f,.882076f,0},{0,0,1});
    const auto old_toe=p::turn(p::multiply(plane,toe_bind),{1,0,0});
    check(old_toe[1]>.47f&&old_toe[1]<.472f,
        "old Foot-axis plane actually raises shipped bind forefoot by28.106degrees");
    for(float yaw:{0.f,1.5707963f,-2.4f})for(unsigned phase=0;phase<32;++phase)for(bool tilt:{false,true}) {
        Frame frame;frame.active=true;frame.position={627,199,1062};frame.crank_phase=phase*6.28318530718f/32;
        frame.chassis_quaternion_available=true;
        frame.chassis_quaternion=p::multiply(p::axis(1,yaw),p::multiply(p::axis(0,tilt?.3f:0),p::axis(2,tilt?-.15f:0)));
        frame.tailwhip=.35f;
        auto f=with_native_toes(fixture());f.bones[1].position={627,199,1062,17};const auto before=f.bones;
        auto legacy=fixture();legacy.bones[1]=f.bones[1];
        const auto result=p::solve(f.bones,f.rig,frame),legacy_result=p::solve(legacy.bones,legacy.rig,frame);
        check(result.ready&&legacy_result.ready&&result.hand_error<.003f&&result.foot_error<.003f,
            "native forefoot calibration retains measured ankle and grip targets through the full crank cycle");
        const auto g=p::compose(f.bones,f.rig),old=p::compose(legacy.bones,legacy.rig);
        std::array<world::Matrix,world::part_count> matrices;check(world::transforms(frame,matrices),"native pedal matrices ready");
        for(bool left:{false,true}) {
            const int foot=left?f.rig.left_leg[2]:f.rig.right_leg[2],toe=f.rig.toe_base[left?0:1];
            const auto& platform=matrices[left?6:7];const p::Vec up{platform[4],platform[5],platform[6]};
            check(p::dot(p::turn(g[static_cast<std::size_t>(toe)].rotation,{0,-1,0}),up)>.99999f,
                "actual shipped bind ToeBase plane follows articulated pedal, not the tilted Foot proxy plane");
            const auto delta=p::sub(g[static_cast<std::size_t>(toe)].position,g[static_cast<std::size_t>(foot)].position);
            check(std::abs(p::dot(delta,up)-(-.0529549f))<.0001f,
                "shipped ankle-to-ball slope lowers the toe joint by52.955mm from the retained ankle target");
            check(p::length(p::sub(g[static_cast<std::size_t>(foot)].position,old[static_cast<std::size_t>(foot)].position))<.0001f,
                "bind calibration changes orientation only; it cannot move an ankle or alter the retained nine-centimetre proxy");
            check(std::memcmp(&f.bones[static_cast<std::size_t>(toe)],&before[static_cast<std::size_t>(toe)],sizeof(p::Bone))==0,
                "native ToeBase translation, rotation, scale and metadata stay untouched");
        }
        check(p::solve(f.bones,f.rig,frame).ready,"repeat render solve accepted");
        const auto repeated=p::compose(f.bones,f.rig);
        for(std::size_t i=0;i<g.size();++i)check(p::length(p::sub(repeated[i].position,g[i].position))<.001f,
            "animation and render application cannot accumulate calibration drift");
        for(std::size_t i=0;i<f.bones.size();++i) {
            check(f.bones[i].scale==before[i].scale&&f.bones[i].position[3]==before[i].position[3],"calibration preserves native scale/W metadata");
            if(static_cast<int>(i)!=f.rig.hips)check(f.bones[i].position==before[i].position,"calibration never writes nonpelvis translations");
        }
    }
    for(float release:{0.f,.5f,1.f}) {
        auto f=with_native_toes(fixture());Frame frame;frame.position={627,199,1062};frame.tailwhip_release=release;
        for(int toe:f.rig.toe_base)f.bones[static_cast<std::size_t>(toe)].rotation=p::multiply(toe_bind,p::axis(2,.17f));
        const auto native=f.bones;check(p::solve(f.bones,f.rig,frame).ready,"native toe-flex riding fixture accepted");
        for(int toe:f.rig.toe_base)check(std::memcmp(&f.bones[static_cast<std::size_t>(toe)],&native[static_cast<std::size_t>(toe)],sizeof(p::Bone))==0,
            "caught, releasing and released feet retain native toe-flex rather than resetting it");
    }
    auto carrying=with_native_toes(hands_fixture());Frame frame;frame.rider_mode=visual::RiderMode::carrying;
    frame.position=rider::motion::carry_bike_offset;
    const auto native=carrying.bones;check(p::solve(carrying.bones,carrying.rig,frame).ready,"native walking carry fixture accepted");
    for(bool left:{false,true})for(int joint:{left?carrying.rig.left_leg[2]:carrying.rig.right_leg[2],carrying.rig.toe_base[left?0:1]})
        check(std::memcmp(&carrying.bones[static_cast<std::size_t>(joint)],&native[static_cast<std::size_t>(joint)],sizeof(p::Bone))==0,
            "walking/carrying feet and toes remain entirely native-owned");
    auto invalid=with_native_toes(fixture());invalid.rig.parents[static_cast<std::size_t>(invalid.rig.toe_base[0])]=invalid.rig.hips;
    const auto untouched=invalid.bones;frame.rider_mode=visual::RiderMode::riding;
    check(!p::solve(invalid.bones,invalid.rig,frame).ready&&std::memcmp(invalid.bones.data(),untouched.data(),untouched.size()*sizeof(p::Bone))==0,
        "mismatched native toe parent rejects before modifying any bone");
}
void natural_riding_anatomy() {
    float torso_min=180,torso_max=0,elbow_min=180,elbow_max=0,wrist_max=0,forearm_max=0,knee_min=180,knee_max=0;
    const auto angle=[](p::Vec a,p::Vec b){return std::acos(std::clamp(p::dot(p::unit(a),p::unit(b)),-1.f,1.f))*57.2957795f;};
    // Actual shipped primary/finger chains, complete source crank rotation,
    // source steerer travel and nontrivial native world/bike orientations.
    // Endpoint-only acceptance previously allowed 40-degree folded elbows,
    // 57-degree wrist extension and a 56-degree forward torso lean.
    for(float yaw:{0.f,1.5707963f,-2.4f})for(float steer:{-.4f,0.f,.4f})
        for(unsigned phase=0;phase<32;++phase)for(bool tilted:{false,true}) {
        auto f=hands_fixture();Frame frame;frame.position={627,199,1062};frame.steer=steer;
        frame.crank_phase=phase*6.28318530718f/32;frame.chassis_quaternion_available=true;
        frame.chassis_quaternion=p::multiply(p::axis(1,yaw),p::multiply(p::axis(0,tilted?.18f:0),p::axis(2,tilted?-.15f:0)));
        f.bones[1].position={627,199,1062,17};f.bones[1].rotation=p::axis(1,yaw);const auto original=f.bones;
        const auto result=p::solve(f.bones,f.rig,frame);
        check(result.ready&&result.hand_error<.003f&&result.foot_error<.003f,"natural riding anatomy retains exact measured palm and pedal contacts");
        const auto g=p::compose(f.bones,f.rig);const auto inverse=p::inverse(p::bike_rotation(frame));
        const auto local=[&](int joint){return p::turn(inverse,p::sub(g[static_cast<std::size_t>(joint)].position,frame.position));};
        const float torso=angle(p::sub(local(f.rig.spine[3]),local(f.rig.spine[0])),{0,1,0});
        torso_min=std::min(torso_min,torso);torso_max=std::max(torso_max,torso);
        check(torso>25&&torso<40,"neutral torso has moderate forward riding lean, not the former deep hunch");
        check(local(f.rig.hips)[1]<1.09f&&!p::retained_neutral_anatomy(f.bones,f.rig,frame),"small-bike stance explicitly fails old prototype1.09m floor comparison");
        if(!p::proposed_small_bike_anatomy(f.bones,f.rig,frame)) {const auto band=p::proposed_hip_band(f.bones,f.rig,frame,local(f.rig.hips)[2]);std::cerr<<"Small-bike neutral mismatch yaw="<<yaw<<" steer="<<steer<<" phase="<<phase<<" tilt="<<tilted<<" hip="<<local(f.rig.hips)[1]<<" band="<<band.low<<","<<band.high<<" torso="<<torso<<'\n';for(auto chain:{f.rig.left_arm,f.rig.right_arm}){const auto shoulder=local(chain[0]),elbow=local(chain[1]),wrist=local(chain[2]);std::cerr<<"elbow="<<angle(p::sub(shoulder,elbow),p::sub(wrist,elbow))<<" fore="<<angle(p::sub(wrist,elbow),{0,0,1})<<" wrist="<<angle(p::sub(wrist,elbow),p::turn(inverse,p::turn(g[static_cast<std::size_t>(chain[2])].rotation,{1,0,0})))<<" elbowheight="<<elbow[1]<<" shoulder="<<shoulder[1]<<" wristheight="<<wrist[1]<<'\n';}}
        check(p::proposed_small_bike_anatomy(f.bones,f.rig,frame),"small-bike neutral anatomy retains original angles and new per-crank native-leg height band");
        for(const auto chain:{f.rig.left_arm,f.rig.right_arm}) {
            const auto shoulder=local(chain[0]),elbow=local(chain[1]),wrist=local(chain[2]);
            const auto forearm=p::sub(wrist,elbow);
            const float bend=angle(p::sub(shoulder,elbow),forearm),forward=angle(forearm,{0,0,1});
            const float extension=angle(forearm,p::turn(inverse,p::turn(g[static_cast<std::size_t>(chain[2])].rotation,{1,0,0})));
            elbow_min=std::min(elbow_min,bend);elbow_max=std::max(elbow_max,bend);
            wrist_max=std::max(wrist_max,extension);forearm_max=std::max(forearm_max,forward);
            check(bend>65&&bend<120,"riding elbow remains comfortably flexed, neither folded shut nor straight locked");
            check(forward<35&&extension<35,"forearm reaches forward and aligns with the fixed gripping hand without extreme wrist extension");
            check(elbow[1]<shoulder[1]-.15f&&std::abs(elbow[1]-wrist[1])<.10f&&std::abs(elbow[0])<.35f,
                "front and side anatomy keep elbows below shoulders near wrist height without chicken-wing flare");
        }
        for(const auto chain:{f.rig.left_leg,f.rig.right_leg}) {
            const float knee=angle(p::sub(local(chain[0]),local(chain[1])),p::sub(local(chain[2]),local(chain[1])));
            knee_min=std::min(knee_min,knee);knee_max=std::max(knee_max,knee);
            check(knee>50&&knee<176,"native knees avoid the former deep squat while retaining flexion at the lowest source pedal");
        }
        for(std::size_t i=0;i<f.bones.size();++i) {
            check(f.bones[i].scale==original[i].scale&&f.bones[i].position[3]==original[i].position[3],"anatomical stance never changes native scale or metadata");
            if(static_cast<int>(i)!=f.rig.hips)check(f.bones[i].position==original[i].position,"anatomical stance preserves every nonpelvis local translation");
        }
    }
    std::cout<<"Natural stance degrees: torso="<<torso_min<<".."<<torso_max<<" elbow="<<elbow_min<<".."<<elbow_max
        <<" wrist max="<<wrist_max<<" forearm from forward max="<<forearm_max<<" knee="<<knee_min<<".."<<knee_max<<".\n";
}
void measured_uphill_policy() {
    namespace r=dingosdk::bmx::rider_posture;constexpr double h=1.0/60;
    const auto sample=[](float grade,double seconds,std::uint64_t serial) {
        r::Observation out;out.seconds=seconds;out.serial=serial;out.supported={true,true};out.normal_available={true,true};
        out.normal={r::Vec{0,std::cos(grade),-std::sin(grade)},r::Vec{0,std::cos(grade),-std::sin(grade)}};
        out.forward={0,std::sin(grade),std::cos(grade)};return out;
    };
    r::State state;for(unsigned tick=1;tick<=30;++tick) {
        const auto out=r::step(state,sample(20*r::radians_per_degree,h,tick));
        check(out.valid&&out.advanced&&out.measured_plane,"two actual measured wheel normals admit native-time incline cue");
        check(out.state.uphill-state.uphill<=4*h+1e-6&&
            out.state.counterpitch-state.counterpitch<=r::counterpitch_rate*h+1e-6,"native-time cue respects spatial/angular slew limits");state=out.state;
    }
    check(state.uphill>.999f&&std::abs(state.counterpitch-20*r::radians_per_degree)<.001f*r::radians_per_degree,"settled20deg incline reaches bounded horizon counterpitch");
    auto duplicate=sample(0,h,state.serial);duplicate.supported={false,false};
    const auto dupe=r::step(state,duplicate);check(dupe.valid&&!dupe.advanced&&dupe.state.uphill==state.uphill&&dupe.state.counterpitch==state.counterpitch,"duplicate native serial cannot spend incline recovery");
    auto paused=sample(0,0,state.serial+1);const auto pause=r::step(state,paused);
    check(pause.valid&&!pause.advanced&&pause.state.serial==state.serial&&pause.state.uphill==state.uphill,"paused native time neither advances serial nor pose cue");
    auto missing=sample(20*r::radians_per_degree,h,state.serial+1);missing.normal_available[1]=false;
    const auto lost=r::step(state,missing);check(lost.valid&&!lost.measured_plane&&lost.state.uphill<state.uphill&&lost.state.counterpitch<state.counterpitch,"missing normal fades both previous cues without manufacturing support");
    r::State partial{.8f,3*r::radians_per_degree,10};auto departing=sample(30*r::radians_per_degree,h,11);departing.supported={false,false};
    check(r::step(partial,departing).state.counterpitch<=partial.counterpitch,"a nose-up takeoff without fresh measured wheel support cannot grow an earlier gaze cue");
    auto one=sample(20*r::radians_per_degree,h,1);one.supported[1]=false;
    check(r::step({},one).state.uphill==0,"one-wheel manual cannot create an incline pose");
    auto disagreement=sample(20*r::radians_per_degree,h,1);disagreement.normal[1]={0,1,0};
    check(!r::step({},disagreement).measured_plane,"inconsistent measured wheel planes do not create incline");
    auto nose_up=sample(20*r::radians_per_degree,h,1);nose_up.normal={r::Vec{0,1,0},r::Vec{0,1,0}};
    const auto flat=r::step({},nose_up);check(flat.valid&&flat.state.uphill==0&&flat.state.counterpitch==0,"nose-up chassis or requested pitch alone cannot manufacture a ground grade");
    check(r::step({},sample(-20*r::radians_per_degree,h,1)).state.uphill==0,"signed downhill normal does not run uphill posture");
    auto malformed=sample(20*r::radians_per_degree,h,1);malformed.normal[0][0]=std::numeric_limits<float>::quiet_NaN();
    const auto no_normal=r::step({},malformed);check(no_normal.valid&&!no_normal.measured_plane&&no_normal.state.uphill==0,"nonfinite missing surface cannot create an incline target");
    auto rewound=sample(0,h,state.serial-1);check(!r::step(state,rewound).valid,"rewound native serial needs bridge reset");
    auto invalid=state;invalid.counterpitch=std::numeric_limits<float>::quiet_NaN();check(!r::step(invalid,sample(0,h,state.serial+1)).valid,"malformed previous cue rejects");
    // Explicit bridge reset is a fresh State, never old mount/slope history.
    const auto reset=r::step({},sample(0,h,1));check(reset.valid&&reset.state.uphill==0&&reset.state.counterpitch==0,"reset cannot retain an old incline on a new body");
    r::State sixty,one_twenty;for(unsigned i=1;i<=12;++i)sixty=r::step(sixty,sample(20*r::radians_per_degree,1.0/60,i)).state;
    for(unsigned i=1;i<=24;++i)one_twenty=r::step(one_twenty,sample(20*r::radians_per_degree,1.0/120,i)).state;
    check(std::abs(sixty.uphill-one_twenty.uphill)<.00001f&&std::abs(sixty.counterpitch-one_twenty.counterpitch)<.00001f,"pure policy follows native seconds rather than callback count; this does not admit a new physical native step");
}
void measured_uphill_native_pose() {
    namespace r=dingosdk::bmx::rider_posture;
    const auto elevation=[](p::Vec forward){return std::atan2(forward[1],std::hypot(forward[0],forward[2]));};
    // Signed uphill is negative X quaternion rotation for +Z-forward bike space.
    Frame frame;frame.chassis_quaternion_available=true;frame.chassis_quaternion=p::axis(0,-20*r::radians_per_degree);
    auto original=with_native_toes(hands_fixture()),legacy=original,corrected=original;
    check(p::solve(legacy.bones,legacy.rig,frame).ready,"legacy uphill native fixture accepted");
    frame.rider_posture_available=true;frame.rider_uphill_blend=1;frame.rider_gaze_counterpitch=r::maximum_counterpitch;
    const auto result=p::solve(corrected.bones,corrected.rig,frame);check(result.ready&&result.posture_requested&&result.posture_scale>0,"actual native-proportion uphill fixture accepts a bounded posture candidate");
    const auto before=p::compose(legacy.bones,legacy.rig),after=p::compose(corrected.bones,corrected.rig);
    const float old_gaze=elevation(p::turn(before[static_cast<std::size_t>(legacy.rig.head)].rotation,{0,1,0}));
    const float new_gaze=elevation(p::turn(after[static_cast<std::size_t>(corrected.rig.head)].rotation,{0,1,0}));
    check(old_gaze>19.9f*r::radians_per_degree&&new_gaze>=-.000001f&&new_gaze<.25f*r::radians_per_degree,
        "world-relative gaze improves on a signed uphill pose; bike-relative yaw-only acceptance cannot satisfy this");
    const auto tilt=[](const auto& transforms,const p::Rig& rig) {
        const auto torso=p::sub(transforms[static_cast<std::size_t>(rig.spine[3])].position,transforms[static_cast<std::size_t>(rig.spine[0])].position);
        return std::atan2(torso[2],torso[1]);
    };
    check(tilt(after,corrected.rig)>tilt(before,legacy.rig)+.5f*r::radians_per_degree,"solved spine gains actual world-forward bracing rather than only moving the gaze");
    // Both neck joints must share the correction; a head-only shortcut fails.
    for(int neck:corrected.rig.neck) {
        float d=0;for(unsigned i=0;i<4;++i)d+=corrected.bones[static_cast<std::size_t>(neck)].rotation[i]*corrected.rig.bind[static_cast<std::size_t>(neck)][i];
        const float angle=2*std::acos(std::clamp(std::abs(d),0.f,1.f));
        check(angle>.2f*r::radians_per_degree&&angle<.35f*r::maximum_counterpitch+.001f,"native cervical correction is distributed within the25deg envelope");
    }
    // Disabled capability ignores even nonfinite optional lanes, preserving the
    // exact legacy source result. Enabled malformed lanes reject before mutation.
    Frame disabled=frame;disabled.rider_posture_available=false;disabled.rider_uphill_blend=std::numeric_limits<float>::quiet_NaN();
    disabled.rider_gaze_counterpitch=std::numeric_limits<float>::quiet_NaN();auto old=original;
    check(p::solve(old.bones,old.rig,disabled).ready&&std::memcmp(old.bones.data(),legacy.bones.data(),old.bones.size()*sizeof(p::Bone))==0,"false capability preserves legacy bones exactly");
    auto rejected=original;disabled.rider_posture_available=true;const auto untouched=rejected.bones;
    check(!p::solve(rejected.bones,rejected.rig,disabled).ready&&std::memcmp(rejected.bones.data(),untouched.data(),untouched.size()*sizeof(p::Bone))==0,"enabled nonfinite cue rejects before any pose mutation");
    auto released=original,released_legacy=original;Frame release=frame;release.barspin_release=1;
    const auto released_result=p::solve(released.bones,released.rig,release);release.rider_posture_available=false;
    check(p::solve(released_legacy.bones,released_legacy.rig,release).ready&&released_result.ready&&!released_result.posture_requested&&
        std::memcmp(released.bones.data(),released_legacy.bones.data(),released.bones.size()*sizeof(p::Bone))==0,"full hand release fades neutral correction and retains exact authored contact solution");
    auto rolled=original,rolled_legacy=original;Frame invert=frame;invert.chassis_quaternion=p::multiply(invert.chassis_quaternion,p::axis(2,1.5707963f));
    const auto rolled_result=p::solve(rolled.bones,rolled.rig,invert);invert.rider_posture_available=false;
    check(p::solve(rolled_legacy.bones,rolled_legacy.rig,invert).ready&&rolled_result.ready&&!rolled_result.posture_requested&&
        std::memcmp(rolled.bones.data(),rolled_legacy.bones.data(),rolled.bones.size()*sizeof(p::Bone))==0,"deep roll fades correction rather than forcing an upright head through an inverted bike");
    // Full native crank, yaw, mild roll, air, preload and compression matrix.
    unsigned candidates=0;
    for(float yaw:{0.f,1.5707963f,-2.4f})for(float roll:{-.15f,0.f,.15f})for(unsigned phase=0;phase<32;++phase)
        for(float air:{0.f,1.f})for(float compression:{0.f,1.f})for(float preload:{0.f,1.f}) {
        auto f=with_native_toes(hands_fixture());Frame active=frame;active.position={627,199,1062};
        active.chassis_quaternion=p::multiply(p::axis(1,yaw),p::multiply(p::axis(0,-20*r::radians_per_degree),p::axis(2,roll)));
        active.crank_phase=phase*6.28318530718f/32;active.air_extension_available=true;active.air_extension_blend=air;
        active.airborne=air==1;active.landing_compression=compression;active.preload=preload;
        f.bones[1].position={627,199,1062,17};f.bones[1].rotation=p::axis(1,yaw);const auto native=f.bones;
        const auto solved=p::solve(f.bones,f.rig,active);check(solved.ready&&solved.hand_error<.003f&&solved.foot_error<.003f,"uphill crank/air/load matrix keeps both actual palms and ankles within3mm");
        check(solved.posture_attempts>=1&&solved.posture_attempts<=3&&solved.posture_fit_adjustment<=.1801f&&
            solved.posture_gaze>=0&&solved.posture_gaze<=r::maximum_counterpitch,"all accepted correction attempts and reach fits remain bounded and observable");
        const auto pose=p::compose(f.bones,f.rig);const auto q=p::bike_rotation(active);
        const float original_elevation=elevation(p::turn(q,{0,0,1})),actual_elevation=elevation(p::turn(pose[static_cast<std::size_t>(f.rig.head)].rotation,{0,1,0}));
        check(actual_elevation>=-.0001f&&actual_elevation<=original_elevation+.0001f,"world gaze never rises further or looks below horizon on the20deg incline");
        for(std::size_t i=0;i<f.bones.size();++i) {
            check(f.bones[i].scale==native[i].scale&&f.bones[i].position[3]==native[i].position[3],"uphill solve retains all native scale and W metadata");
            if(static_cast<int>(i)!=f.rig.hips)check(f.bones[i].position==native[i].position,"uphill correction never changes native segment lengths or toe translations");
        }
        for(int toe:f.rig.toe_base)check(std::memcmp(&f.bones[static_cast<std::size_t>(toe)],&native[static_cast<std::size_t>(toe)],sizeof(p::Bone))==0,"new neck/torso cues leave native toe flex untouched");
        ++candidates;
    }
    check(candidates==2304,"uphill matrix includes32cranks,3yaws,3rolls and both air/compression/preload states");
    const auto ground=p::posture_targets(frame,1);Frame air=frame;air.air_extension_available=true;air.air_extension_blend=1;
    const auto floating=p::posture_targets(air,1);check(std::abs(floating.hip[1]-.5f*ground.hip[1])<.000001f&&std::abs(floating.gaze-.5f*ground.gaze)<.000001f,"measured native-time air extension fades torso and neck cues together");
    Frame compressed=frame;compressed.landing_compression=1;const auto bounds=p::posture_targets(compressed,1);
    check(bounds.torso[2]<=.075001f&&bounds.hip[1]>=-.018001f,"combined slope and impact cue cannot exceed75mm chest or18mm hip drop");
    // An anatomically impossible arm reach must exhaust the fixed budget and
    // leave the caller's source pose untouched, never relax the3mm requirement.
    auto unreachable=hands_fixture();for(const auto chain:{unreachable.rig.left_arm,unreachable.rig.right_arm})
        for(int joint:{chain[1],chain[2]})for(unsigned axis=0;axis<3;++axis)unreachable.bones[static_cast<std::size_t>(joint)].position[axis]*=.1f;
    const auto source=unreachable.bones;const auto failed=p::solve(unreachable.bones,unreachable.rig,frame);
    check(!failed.ready&&failed.posture_requested&&failed.posture_fallback&&failed.posture_attempts==3&&failed.posture_scale==0&&
        std::memcmp(source.data(),unreachable.bones.data(),source.size()*sizeof(p::Bone))==0,"unreachable native chain tries1/.5/0 exactly and never commits a failed contact candidate");
}

void measured_uphill_authored_trick_lifecycle() {
    using namespace dingosdk::bmx;
    namespace r=dingosdk::bmx::rider_posture;
    constexpr double h=1.0/120;constexpr float pi=3.14159265359f;
    unsigned selections=0,samples=0,corrected_samples=0,faded_samples=0,baseline_rejections=0,guarded_rejections=0;
    // Seed from actual supported20deg normals. Saturated residual cues below
    // stress the Frame consumer even at late catch; the native policy normally
    // fades these sooner without support. This is not a native air-time claim.
    r::State seeded;r::Observation measured;measured.seconds=h;
    measured.supported={true,true};measured.normal_available={true,true};
    measured.normal={r::Vec{0,std::cos(20*r::radians_per_degree),-std::sin(20*r::radians_per_degree)},
        r::Vec{0,std::cos(20*r::radians_per_degree),-std::sin(20*r::radians_per_degree)}};
    measured.forward={0,std::sin(20*r::radians_per_degree),std::cos(20*r::radians_per_degree)};
    for(unsigned serial=1;serial<=40;++serial){measured.serial=serial;seeded=r::step(seeded,measured).state;}
    check(seeded.uphill>.999f&&seeded.counterpitch>14.99f*r::radians_per_degree,"authored trick residual cues originate in measured uphill policy");
    for(const auto& entry:catalog::entries) {
        TrickController controller;TrickInput input;input.enabled=true;input.owner=1;
        input.bar_neutral=input.rear_neutral=true;controller.update(h,input);
        input.grounded=false;check(controller.update(h,input).took_off,"trick lifecycle requires observed ground and neutral takeoff");
        const bool bars=entry.id==catalog::TrickId::barspin_left||entry.id==catalog::TrickId::barspin_right;
        const bool tails=entry.id==catalog::TrickId::tailwhip_left||entry.id==catalog::TrickId::tailwhip_right;
        const auto direction=entry.direction<0?TrickDirection::left:TrickDirection::right;
        if(bars){input.barspin=direction;input.bar_neutral=false;}
        else if(tails){input.tailwhip=direction;input.rear_neutral=false;}
        else {input.movement=entry.id;input.rear_neutral=false;}
        controller.update(h,input);
        bool entered_partial=false,released=false,recovered_partial=false,completed=false;
        unsigned tick=0;
        const auto evaluate=[&](const TrickState& state,bool recovery) {
            auto original=with_native_toes(hands_fixture());Frame frame;
            frame.position={627,199,1062};frame.chassis_quaternion_available=true;
            frame.chassis_quaternion=p::multiply(p::axis(1,.7f),p::multiply(p::axis(0,-20*r::radians_per_degree),p::axis(2,-.2f)));
            original.bones[1].position={627,199,1062,17};original.bones[1].rotation=p::axis(1,.7f);
            frame.airborne=true;frame.air_extension_available=true;frame.air_extension_blend=1;
            frame.barspin=static_cast<float>(state.bar_angle+state.movement_bar_angle);
            frame.tailwhip=static_cast<float>(state.tail_angle+state.movement_tail_angle);
            frame.crank_phase=static_cast<float>(tick)*pi/16+static_cast<float>(state.movement_crank_angle);
            frame.barspin_release=static_cast<float>(state.bar_release);frame.tailwhip_release=static_cast<float>(state.tail_release);
            frame.movement_id=state.movement_id;frame.movement_phase=static_cast<float>(state.movement_phase);
            frame.movement_progress=static_cast<float>(state.movement_progress);
            frame.movement_bike_pitch=static_cast<float>(state.movement_bike_pitch);frame.movement_bike_roll=static_cast<float>(state.movement_bike_roll);
            frame.release_left_hand=static_cast<float>(state.release_left_hand);frame.release_right_hand=static_cast<float>(state.release_right_hand);
            frame.release_left_foot=static_cast<float>(state.release_left_foot);frame.release_right_foot=static_cast<float>(state.release_right_foot);
            const float envelope=std::max({frame.movement_phase,p::hand_release(frame,true),p::hand_release(frame,false),p::foot_release(frame,true),p::foot_release(frame,false)});
            if(!recovery&&envelope>0&&envelope<1)entered_partial=true;
            if(envelope==1)released=true;
            if(recovery&&envelope>0&&envelope<1)recovered_partial=true;
            auto legacy=original,disabled=original,enabled=original;
            const auto old=p::solve(legacy.bones,legacy.rig,frame);
            frame.rider_uphill_blend=frame.rider_gaze_counterpitch=std::numeric_limits<float>::quiet_NaN();
            const auto unavailable=p::solve(disabled.bones,disabled.rig,frame);
            if(old.ready!=unavailable.ready||std::memcmp(legacy.bones.data(),disabled.bones.data(),legacy.bones.size()*sizeof(p::Bone))!=0)
                std::cerr<<"legacy trick "<<entry.name<<" tick="<<tick<<" recovery="<<recovery<<" phase="<<frame.movement_phase<<" release="<<envelope<<" ready="<<old.ready<<','<<unavailable.ready<<" errors="<<old.hand_error<<','<<old.foot_error<<'\n';
            check(old.ready==unavailable.ready&&std::memcmp(legacy.bones.data(),disabled.bones.data(),legacy.bones.size()*sizeof(p::Bone))==0,
                "disabled posture remains byte-exact throughout every controller trick stage");
            frame.rider_posture_available=true;frame.rider_uphill_blend=seeded.uphill;frame.rider_gaze_counterpitch=seeded.counterpitch;
            const auto result=p::solve(enabled.bones,enabled.rig,frame);
            if(!result.ready&&old.ready)std::cerr<<"uphill trick "<<entry.name<<" tick="<<tick<<" recovery="<<recovery<<" phase="<<frame.movement_phase<<" release="<<envelope<<" errors="<<result.hand_error<<','<<result.foot_error<<'\n';
            check(result.ready||!old.ready,"carried uphill cues cannot reject an accepted legacy trick stage");
            if(!old.ready) {
                ++baseline_rejections;
                std::cout<<"Existing catalog reach rejection: "<<entry.name<<" tick="<<tick<<" recovery="<<recovery
                    <<" phase="<<frame.movement_phase<<" progress="<<frame.movement_progress<<" envelope="<<envelope<<'\n';
            }
            if(!result.ready) {
                // Existing reach rejection is not a new posture regression.
                // A requested strict candidate must fail transactionally; a
                // fully faded legacy path retains exactly its original result.
                if(result.posture_requested) {
                    ++guarded_rejections;check(result.posture_attempts==3&&result.posture_scale==0&&result.posture_fallback&&
                        std::memcmp(enabled.bones.data(),original.bones.data(),enabled.bones.size()*sizeof(p::Bone))==0,
                        "unreachable authored correction exhausts1/.5/0 without mutating native pose");
                } else check(std::memcmp(enabled.bones.data(),legacy.bones.data(),enabled.bones.size()*sizeof(p::Bone))==0,
                    "fully faded unreachable authored stage preserves exact legacy rejection");
                ++samples;return;
            }
            if(result.posture_requested) {
                ++corrected_samples;check(result.posture_attempts>=1&&result.posture_attempts<=3&&result.hand_error<.003f&&result.foot_error<.003f,
                    "enabled trick correction retains the strict3mm candidate guard and bounded attempts");
            }
            if(envelope==1) {
                ++faded_samples;check(!result.posture_requested&&std::memcmp(legacy.bones.data(),enabled.bones.data(),legacy.bones.size()*sizeof(p::Bone))==0,
                    "full authored movement or limb release exactly fades to legacy native pose");
            }
            const auto composed=p::compose(enabled.bones,enabled.rig);
            for(bool left:{false,true}) {
                const auto& hand=composed[static_cast<std::size_t>(left?enabled.rig.left_arm[2]:enabled.rig.right_arm[2])];
                const auto palm=p::add(hand.position,p::turn(hand.rotation,p::palm_bar_offset(left)));
                const auto ankle=composed[static_cast<std::size_t>(left?enabled.rig.left_leg[2]:enabled.rig.right_leg[2])].position;
                if(p::hand_release(frame,left)==0)check(p::length(p::sub(palm,p::grip(frame,left)))<.003f,"retained/caught authored palm stays within3mm of actual grip");
                if(p::foot_release(frame,left)==0)check(p::length(p::sub(ankle,p::pedal(frame,left)))<.003f,"retained/caught authored ankle stays within3mm of actual pedal");
            }
            check(std::abs(result.hand_contact-(1-std::max(p::hand_release(frame,true),p::hand_release(frame,false))))<.00001f&&
                std::abs(result.foot_contact-(1-std::max(p::foot_release(frame,true),p::foot_release(frame,false))))<.00001f,
                "posture correction never manufactures release/catch contact credit");
            for(std::size_t i=0;i<enabled.bones.size();++i) {
                check(enabled.bones[i].scale==original.bones[i].scale&&enabled.bones[i].position[3]==original.bones[i].position[3],"trick posture retains native scale and metadata");
                if(static_cast<int>(i)!=enabled.rig.hips)check(enabled.bones[i].position==original.bones[i].position,"trick posture retains native root and segment translations");
            }
            for(int toe:enabled.rig.toe_base)check(std::memcmp(&enabled.bones[static_cast<std::size_t>(toe)],&original.bones[static_cast<std::size_t>(toe)],sizeof(p::Bone))==0,"trick posture preserves native toe flex");
            ++samples;
        };
        evaluate(controller.state(),false);
        for(;tick<100;++tick) {
            controller.update(h,input);const auto& state=controller.state();
            const bool reached=bars?state.bar_waiting_catch:(tails?state.tail_waiting_catch:state.movement_progress>=1);
            if(tick%3==0||reached)evaluate(state,false);
            if(reached){completed=true;break;}
        }
        check(completed,"every named controller trick reaches hold or catch waiting within bounded native time");
        input.barspin=input.tailwhip=TrickDirection::none;input.movement=catalog::TrickId::none;input.bar_neutral=input.rear_neutral=true;
        for(unsigned recovery=0;recovery<20;++recovery) {
            ++tick;controller.update(h,input);if(recovery%2==0||recovery==19)evaluate(controller.state(),true);
        }
        const auto& caught=controller.state();
        check((bars?caught.bar_caught:(tails?caught.tail_caught:caught.movement_caught))&&entered_partial&&recovered_partial,
            "every named trick covers partial entry and partial measured recovery through actual catch");
        check(released||entry.release_mask==0,"each released catalog selection reaches a fully released legacy-equivalent pose");
        ++selections;
    }
    check(selections==58&&corrected_samples>58&&faded_samples>58,"all58named selections include active and fully faded posture observations");
    std::cout<<"Uphill authored trick lifecycle: "<<selections<<" selections, "<<samples<<" snapshots, "<<corrected_samples<<" correction requests, "<<faded_samples<<" fully faded, "<<baseline_rejections<<" baseline-rejected, "<<guarded_rejections<<" transactional rejections.\n";
}

void stronger_supported_horizon_and_hinge() {
    using namespace dingosdk::bmx;namespace r=rider_posture;
    const auto elevation=[](p::Vec v){return std::atan2(v[1],std::hypot(v[0],v[2]));};
    for(float pitch:{5.f,15.f,20.f,25.f,30.f})for(float yaw:{0.f,.7f})for(float roll:{0.f,.2f}) {
        auto f=with_native_toes(hands_fixture()),legacy=f;Frame frame;
        frame.chassis_quaternion_available=true;
        frame.chassis_quaternion=p::multiply(p::axis(1,yaw),p::multiply(p::axis(0,-pitch*r::radians_per_degree),p::axis(2,roll)));
        check(p::solve(legacy.bones,legacy.rig,frame).ready,"stronger incline legacy reference accepted");
        frame.rider_posture_available=true;frame.rider_uphill_blend=1;frame.rider_gaze_counterpitch=r::maximum_counterpitch;
        const auto result=p::solve(f.bones,f.rig,frame);check(result.ready&&result.hand_error<.003f&&result.foot_error<.003f,"stronger supported slope retains strict contacts");
        const auto g=p::compose(f.bones,f.rig),old=p::compose(legacy.bones,legacy.rig);
        const float gaze=elevation(p::turn(g[static_cast<std::size_t>(f.rig.head)].rotation,{0,1,0}));
        const float reference=elevation(p::turn(old[static_cast<std::size_t>(legacy.rig.head)].rotation,{0,1,0}));
        check(gaze>=-.000001f&&gaze<=reference+.000001f,"positive world gaze never crosses below horizon");
        if(pitch<=25)check(result.posture_scale==1&&gaze<.25f*r::radians_per_degree,"supported native fixture reaches horizon without attenuated fallback");
        else check(gaze<5.25f*r::radians_per_degree,"25deg cervical cap leaves only the bounded residual on30deg incline");
        if(pitch==20&&yaw==0&&roll==0) {
            const auto inv=p::inverse(p::bike_rotation(frame));
            const auto spine=[&](const auto& x){return p::turn(inv,p::sub(x[static_cast<std::size_t>(f.rig.spine[3])].position,x[static_cast<std::size_t>(f.rig.spine[0])].position));};
            const auto a=spine(g),b=spine(old);
            check(std::atan2(a[2],a[1])>std::atan2(b[2],b[1])+.5f*r::radians_per_degree,"small-bike bounded supported cue adds actual anatomical hip hinge");
            const auto hip=p::turn(inv,p::sub(g[static_cast<std::size_t>(f.rig.hips)].position,frame.position));
            const auto oldhip=p::turn(inv,p::sub(old[static_cast<std::size_t>(f.rig.hips)].position,frame.position));
            check(hip[1]<=oldhip[1]+.0001f&&hip[1]>=oldhip[1]-.0046f&&hip[1]<1.09f,"small-bike incline drop stays within4.5mm while old absolute hip floor remains a failed comparison");
        }
    }
    for(unsigned crank=0;crank<32;++crank) {
        auto legacy=with_native_toes(hands_fixture()),enabled=legacy;Frame frame;frame.crank_phase=crank*6.28318530718f/32;
        check(p::solve(legacy.bones,legacy.rig,frame).ready,"unchanged neutral crank reference");frame.rider_posture_available=true;
        const auto result=p::solve(enabled.bones,enabled.rig,frame);
        check(result.ready&&!result.posture_requested&&std::memcmp(enabled.bones.data(),legacy.bones.data(),enabled.bones.size()*sizeof(p::Bone))==0,
            "zero measured cue preserves existing neutral base byte-for-byte");
    }
    unsigned full_supported=0;
    for(unsigned crank=0;crank<32;++crank) {
        auto f=with_native_toes(hands_fixture());Frame frame;frame.chassis_quaternion_available=true;
        frame.chassis_quaternion=p::axis(0,-20*r::radians_per_degree);frame.crank_phase=crank*6.28318530718f/32;
        frame.pedal_effort=1;frame.rider_posture_available=true;frame.rider_uphill_blend=1;frame.rider_gaze_counterpitch=r::maximum_counterpitch;
        const auto result=p::solve(f.bones,f.rig,frame);
        check(result.ready&&result.hand_error<.003f&&result.foot_error<.003f,"anatomical incline fit keeps all32pedal-bob crank poses in strict contact");
        check(p::proposed_small_bike_anatomy(f.bones,f.rig,frame),"small-bike incline fit preserves original angle bounds and independently specified native-leg band");
        if(result.posture_scale==1)++full_supported;
    }
    check(full_supported==32,"pedal-bob anatomy cannot unnecessarily halve supported horizon correction");
    // The fit remains continuous at the old guard's exact-zero boundaries.
    for(unsigned boundary=0;boundary<5;++boundary) {
        auto zero=with_native_toes(hands_fixture()),epsilon=zero;Frame frame;
        frame.chassis_quaternion_available=true;frame.chassis_quaternion=p::axis(0,-20*r::radians_per_degree);
        frame.crank_phase=3*3.14159265359f/4;frame.pedal_effort=1;
        frame.rider_posture_available=true;frame.rider_uphill_blend=1;frame.rider_gaze_counterpitch=r::maximum_counterpitch;
        check(p::solve(zero.bones,zero.rig,frame).ready,"incline fit zero-boundary reference");
        if(boundary==0)frame.landing_compression=.00001f;
        if(boundary==1){frame.air_extension_available=true;frame.air_extension_blend=.00001f;}
        if(boundary==2)frame.preload=.00001f;
        if(boundary==3)frame.movement_phase=.00001f;
        if(boundary==4)frame.barspin_release=.00001f;
        const auto result=p::solve(epsilon.bones,epsilon.rig,frame);check(result.ready&&result.hand_error<.003f&&result.foot_error<.003f,"incline fit remains valid across tiny loading/air/release cues");
        const auto a=p::compose(zero.bones,zero.rig),b=p::compose(epsilon.bones,epsilon.rig);
        check(p::length(p::sub(a[static_cast<std::size_t>(zero.rig.hips)].position,b[static_cast<std::size_t>(epsilon.rig.hips)].position))<.0001f,
            "vanishing load/air/release cannot produce a discrete incline hip target jump");
    }
    // A retained cue cannot push an already downward native head farther down.
    auto f=hands_fixture(),legacy=f;Frame descending;descending.chassis_quaternion_available=true;
    descending.chassis_quaternion=p::axis(0,10*r::radians_per_degree);check(p::solve(legacy.bones,legacy.rig,descending).ready,"downhill reference");
    descending.rider_posture_available=true;descending.rider_uphill_blend=1;descending.rider_gaze_counterpitch=r::maximum_counterpitch;
    const auto result=p::solve(f.bones,f.rig,descending);check(result.ready&&result.posture_gaze==0,"carried uphill cue adds no downward cervical pitch during descent");
    const auto a=p::compose(f.bones,f.rig),b=p::compose(legacy.bones,legacy.rig);
    check(p::length(p::sub(p::turn(a[static_cast<std::size_t>(f.rig.head)].rotation,{0,1,0}),p::turn(b[static_cast<std::size_t>(legacy.rig.head)].rotation,{0,1,0})))<.000001f,
        "downward measured gaze retains exact world-forward direction");
}


void bounded_grip_probe_window() {
    namespace g=dingosdk::bmx::grip_probe_policy;
    g::Window w;double time=0;std::uint64_t serial=100;
    const auto step=[&](bool air){time+=1./60;return w.observe(1,2,true,++serial,time,air);};
    check(!step(false)&&!step(true)&&!step(true)&&!step(false),"two-step lip gap cannot arm grip evidence");
    check(!step(true)&&!step(true)&&!step(true)&&step(true)&&w.armed&&w.count==1,"four distinct consecutive observed airborne serials arm grip evidence");
    check(!w.observe(1,2,true,serial,time,true)&&w.duplicates==1&&w.count==1,"paused/duplicate listener clock never grows evidence window");
    for(unsigned i=1;i<128;++i)check(step(i<20),"bounded evidence continues through landing");
    check(w.count==128&&w.sealed&&!step(false),"128-record hard cap seals evidence without overwriting earliest descent");
    g::Window missing;check(!missing.observe(1,2,false,1,0,true)&&missing.missing_clock==1&&!missing.owner,"missing listener clock cannot claim armed evidence");
    g::Window gap;for(unsigned i=1;i<=3;++i)check(!gap.observe(1,2,true,i,i/60.,true),"three serials cannot arm");
    check(!gap.observe(1,2,true,5,5/60.,true)&&gap.air_steps==1&&gap.serial_gaps==1,"unobserved serial gap resets sustained-flight arming");
    check(!gap.observe(2,2,true,6,6/60.,true)&&gap.owner_changed&&gap.sealed,"changed owner seals rather than reusing source pose");
    g::Window rewind;rewind.observe(1,2,true,10,1,true);
    check(!rewind.observe(1,2,true,9,.9,true)&&rewind.clock_rewound&&rewind.sealed,"clock reset cannot associate old source with new readback");
    g::Edge source{1,2,3,4,5,6,6,60,1,true,true,true,true};
    source.actual_owner={5,1,2,3,4};source.actual_available=true;
    check(g::same_admission(source,source,source),"matching copied owner epoch and listener clock permits readback qualification");
    for(unsigned mutation=0;mutation<13;++mutation){auto after=source;
        if(mutation==0)++after.entity;if(mutation==1)++after.core;if(mutation==2)++after.context;if(mutation==3)++after.rig;
        if(mutation==4)++after.epoch;if(mutation==5)++after.lease_revision;if(mutation==6)++after.serial;if(mutation==7)after.time+=.001;
        if(mutation==8)after.clock=false;if(mutation==9)after.lease=false;if(mutation==10)after.active=false;if(mutation==11)after.epoch_valid=false;if(mutation==12)++after.client;
        check(!g::same_admission(source,source,after)&&!g::same_admission(source,after,source),"every owner/epoch/clock/unavailable mutation disqualifies readback");
    }
    for(unsigned lane=0;lane<5;++lane){auto stale=source;
        if(lane==0)++stale.actual_owner.client;if(lane==1)++stale.actual_owner.entity;if(lane==2)++stale.actual_owner.core;
        if(lane==3)++stale.actual_owner.context;if(lane==4)++stale.actual_owner.rig;
        check(!g::owner_joined(stale)&&!g::same_admission(source,stale,source)&&!g::same_admission(stale,stale,stale),
            "stable stale lease cannot qualify any mismatching actual source/client/context/rig lane");
    }
    auto unknown_actual=source;unknown_actual.actual_available=false;
    check(!g::same_admission(source,unknown_actual,source),"unavailable actual owner is unknown even with stable copied lease edges");
}
void small_bike_native_leg_band_and_epsilon() {
    using namespace dingosdk::bmx;
    // Independent measurement from solved native segment positions. The old
    // prototype floor remains a failed comparison, never a universal human rule.
    for(unsigned phase=0;phase<32;++phase)for(float steer:{-.4f,0.f,.4f}) {
        auto f=with_native_toes(hands_fixture());Frame frame;frame.crank_phase=phase*6.28318530718f/32;frame.steer=steer;
        const auto result=p::solve(f.bones,f.rig,frame);check(result.ready&&result.hand_error<.003f&&result.foot_error<.003f,"new-small-bike native leg-band fixture retains strict endpoints");
        const auto g=p::compose(f.bones,f.rig);const auto hip=g[static_cast<std::size_t>(f.rig.hips)].position;
        const auto left=g[static_cast<std::size_t>(f.rig.left_leg[0])].position,right=g[static_cast<std::size_t>(f.rig.right_leg[0])].position;
        double lower=.702625573+.5*p::length(p::sub(left,right))+.03,upper=10;
        for(auto chain:{f.rig.left_leg,f.rig.right_leg}) {
            const auto root=g[static_cast<std::size_t>(chain[0])].position,knee=g[static_cast<std::size_t>(chain[1])].position,ankle=g[static_cast<std::size_t>(chain[2])].position;
            const double a=p::length(p::sub(knee,root)),b=p::length(p::sub(ankle,knee));
            const double horizontal=std::pow(double(ankle[0])-root[0],2)+std::pow(double(ankle[2])-root[2],2);
            const double near2=a*a+b*b-2*a*b*std::cos(50*3.141592653589793/180),far2=a*a+b*b-2*a*b*std::cos(176*3.141592653589793/180);
            check(horizontal<far2,"new-small-bike horizontal leg reach fits native segments without stretching");
            lower=std::max(lower,ankle[1]+std::sqrt(std::max(0.,near2-horizontal))-(root[1]-hip[1]));
            upper=std::min(upper,ankle[1]+std::sqrt(far2-horizontal)-(root[1]-hip[1]));
        }
        check(hip[1]>lower&&hip[1]<upper,"new-small-bike pelvis lies inside independently derived50..176deg native leg and saddle interval");
        check(hip[1]<1.09f&&!p::retained_neutral_anatomy(f.bones,f.rig,frame),"old1.09m floor is retained as an explicit failed comparison");
    }
    for(unsigned phase=0;phase<32;++phase)for(float pitch:{-.5235988f,0.f,.5235988f})for(unsigned cue=0;cue<10;++cue) {
        auto source=with_native_toes(hands_fixture()),next=source;Frame frame;frame.crank_phase=phase*6.28318530718f/32;
        frame.chassis_quaternion_available=true;frame.chassis_quaternion=p::axis(0,pitch);frame.air_extension_available=true;
        frame.rider_posture_available=true;frame.rider_uphill_blend=pitch<0?1.f:0.f;frame.rider_gaze_counterpitch=pitch<0?rider_posture::maximum_counterpitch:0;
        const auto a=p::solve(source.bones,source.rig,frame);constexpr float epsilon=1e-7f;
        if(cue==0)frame.preload=epsilon;if(cue==1)frame.front_weight=epsilon;if(cue==2)frame.rear_weight=epsilon;
        if(cue==3)frame.landing_compression=epsilon;if(cue==4)frame.air_extension_blend=epsilon;
        if(cue>=5){frame.movement_id=catalog::TrickId::tuck_no;if(cue==5)frame.movement_phase=epsilon;
            if(cue==6)frame.release_left_hand=epsilon;if(cue==7)frame.release_right_hand=epsilon;if(cue==8)frame.release_left_foot=epsilon;if(cue==9)frame.release_right_foot=epsilon;}
        const auto b=p::solve(next.bones,next.rig,frame);
        check(a.ready&&b.ready&&a.hand_error<.003f&&a.foot_error<.003f&&b.hand_error<.003f&&b.foot_error<.003f,"zero-to-epsilon small-bike transitions preserve strict contacts");
        check(a.posture_attempts<=1&&b.posture_attempts<=1,"small-bike neutral and epsilon candidates never switch between coarse fallback amounts");
        const auto ga=p::compose(source.bones,source.rig),gb=p::compose(next.bones,next.rig);
        for(std::size_t i=0;i<ga.size();++i)check(p::length(p::sub(ga[i].position,gb[i].position))<.00001f,"tiny native input cannot cause a finite hip/head/limb jump");
    }
}


// Authored revision1 contrast: modest leg extension, with existing source
// preload/air/landing amplitudes, unchanged contacts and native ownership.
void modest_standing_rider_contrast() {
    constexpr float tau=6.28318530718f;
    for(unsigned phase=0;phase<24;++phase) {
        Frame neutral;neutral.crank_phase=tau*phase/24;neutral.air_extension_available=true;
        auto standing=hands_fixture();const auto root=standing.bones[1];
        const auto sr=p::solve(standing.bones,standing.rig,neutral);
        check(sr.ready&&sr.hand_error<.003f&&sr.foot_error<.003f,"extended standing keeps strict native-proportion contact targets");
        const auto gs=p::compose(standing.bones,standing.rig);const auto sh=gs[2].position[1];
        check(sh>.95f&&sh<.98f&&p::proposed_small_bike_anatomy(standing.bones,standing.rig,neutral),
            "standing opens native legs within the unchanged elbow/torso/knee anatomy bounds");
        for(auto chain:{standing.rig.left_leg,standing.rig.right_leg}) {
            const auto a=p::sub(gs[static_cast<std::size_t>(chain[0])].position,gs[static_cast<std::size_t>(chain[1])].position);
            const auto b=p::sub(gs[static_cast<std::size_t>(chain[2])].position,gs[static_cast<std::size_t>(chain[1])].position);
            const float knee=std::acos(std::clamp(p::dot(p::unit(a),p::unit(b)),-1.f,1.f))*57.2957795f;
            check(knee>54&&knee<176,"revised standing request measurably opens both native knees over the crank cycle");
        }
        for(unsigned cue=0;cue<3;++cue) {
            auto f=hands_fixture();auto frame=neutral;
            if(cue==0)frame.preload=1;
            if(cue==1){frame.airborne=true;frame.preload=1;frame.air_extension_blend=1;}
            if(cue==2)frame.landing_compression=1;
            const auto r=p::solve(f.bones,f.rig,frame);
            check(r.ready&&r.hand_error<.003f&&r.foot_error<.003f,"distinct loaded/air/landing poses retain grip and pedal anchors");
            const auto g=p::compose(f.bones,f.rig);const auto h=g[2].position[1];
            if(cue==0)check(sh-h>.12f&&h>=.8200f,"full preload retains saddle clearance and visible travel below standing");
            if(cue==1)check(h-sh>.025f&&h-sh<.045f,"takeoff releases held preload and retains bounded4cm extension");
            if(cue==2)check(sh-h>.06f&&sh-h<.09f,"full landing remains visibly compressed without increasing source recovery speed");
            check(std::memcmp(&root,&f.bones[1],sizeof(p::Bone))==0,"rider contrast never shifts the native root");
            const auto original=fixture();
            for(std::size_t i=0;i<f.bones.size();++i)if(static_cast<int>(i)!=f.rig.hips) {
                // Finger fixture adds descendants; the full existing matrix
                // already checks every local translation and scale. Here the
                // independently specified primary fixture covers native limbs.
                if(i<original.bones.size())check(f.bones[i].position==original.bones[i].position&&f.bones[i].scale==original.bones[i].scale,
                    "primary local segment translations and scales remain native-length");
            }
        }
    }
}


void articulated_clavicle_standing_constraints() {
    for(float yaw:{0.f,1.5707963f,-2.4f})for(float steer:{-.4f,0.f,.4f})for(unsigned phase=0;phase<32;++phase)for(bool tilt:{false,true}) {
        auto f=with_native_toes(hands_fixture());Frame frame;frame.position={627,199,1062};frame.steer=steer;
        frame.crank_phase=phase*6.28318530718f/32;frame.chassis_quaternion_available=true;
        frame.chassis_quaternion=p::multiply(p::axis(1,yaw),p::multiply(p::axis(0,tilt?.18f:0),p::axis(2,tilt?-.15f:0)));
        f.bones[1].position={627,199,1062,17};f.bones[1].rotation=p::axis(1,yaw);const auto native=f.bones;
        const auto result=p::solve(f.bones,f.rig,frame);const auto g=p::compose(f.bones,f.rig);const auto inv=p::inverse(p::bike_rotation(frame));
        check(result.ready&&result.hand_error<.003f&&result.foot_error<.003f&&!result.posture_fallback,
            "articulated standing keeps strict grips/pedals without a fallback hiding the stance");
        const auto hip=p::turn(inv,p::sub(g[static_cast<std::size_t>(f.rig.hips)].position,frame.position));
        check(hip[1]>.953f&&hip[1]<.963f,"composed standing hips retain the authored4cm opening after all fitting");
        auto unarticulated=f.bones;for(int joint:f.rig.shoulders)unarticulated[static_cast<std::size_t>(joint)].rotation=f.rig.bind[static_cast<std::size_t>(joint)];
        const auto reference=p::compose(unarticulated,f.rig);
        for(unsigned side=0;side<2;++side) {
            const int joint=f.rig.shoulders[side],arm=side==0?f.rig.left_arm[0]:f.rig.right_arm[0];
            const auto shift=p::turn(inv,p::sub(g[static_cast<std::size_t>(arm)].position,reference[static_cast<std::size_t>(arm)].position));
            const auto delta=p::normal(p::multiply(p::inverse(f.rig.bind[static_cast<std::size_t>(joint)]),f.bones[static_cast<std::size_t>(joint)].rotation));
            const float degrees=2*std::acos(std::clamp(std::abs(delta[3]),0.f,1.f))*57.2957795f;
            check(shift[1]<-.035f&&shift[1]>-.045f&&degrees>10&&degrees<15,
                "existing clavicle rotations lower upper-arm origins within measured rigid swing envelope");
        }
        check(std::memcmp(&f.bones[1],&native[1],sizeof(p::Bone))==0,"articulated stance never moves the native trajectory root");
        for(std::size_t i=0;i<f.bones.size();++i) {
            check(f.bones[i].scale==native[i].scale,"articulated clavicles never stretch a native segment");
            if(static_cast<int>(i)!=f.rig.hips)check(f.bones[i].position==native[i].position,"all nonpelvis native translations remain byte exact");
        }
        check(p::length(p::sub(g[static_cast<std::size_t>(f.rig.head)].position,reference[static_cast<std::size_t>(f.rig.head)].position))<.000001f,
            "clavicle articulation is independent of the head/gaze branch");
    }
    auto unsupported=fixture();const auto original=unsupported.bones;
    unsupported.rig.parents[static_cast<std::size_t>(unsupported.rig.right_arm[0])]=unsupported.rig.spine[3];
    check(!p::articulate_clavicles(unsupported.bones,unsupported.rig,Frame{})&&
        std::memcmp(original.data(),unsupported.bones.data(),original.size()*sizeof(p::Bone))==0,
        "unexpected shoulder topology rejects both articulations before either joint changes");
}

}
int main(){articulated_clavicle_standing_constraints();modest_standing_rider_contrast();small_bike_native_leg_band_and_epsilon();bounded_grip_probe_window();stronger_supported_horizon_and_hinge();measured_uphill_authored_trick_lifecycle();measured_uphill_policy();measured_uphill_native_pose();palms_and_fingers();contact_and_world_placement();preload_weight_and_airborne();native_landing_posture_continuity_run155();trick_release_and_catch();board_visibility_native_refresh();rejection();full_chassis_rotation();catalog_movements();carrying_and_transitions();native_mount_contacts();walking_carry_envelope();supplied_geometry_reach();native_foot_bind_plane();natural_riding_anatomy();std::cout<<"BMX rider native-axis contacts, catalog motion/recovery, carry, transitions and ownership tests passed.\n";return 0;}
