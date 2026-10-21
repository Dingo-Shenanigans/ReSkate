#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#include <GL/gl.h>
#include <bmx/movement.hpp>
#include <bmx/suspension.hpp>
#include <bmx/pose_track.hpp>
#include <bmx/vehicle_pose.hpp>
#include <bmx/ik_track.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
using V = bmx::Vec3;
constexpr double pi = 3.14159265358979323846;
V add(V a,V b){for(int i=0;i<3;++i)a[i]+=b[i];return a;}
V sub(V a,V b){for(int i=0;i<3;++i)a[i]-=b[i];return a;}
V mul(V a,double s){for(auto&x:a)x*=s;return a;}
double dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
V cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
V unit(V a){double n=std::sqrt(dot(a,a));return n>1e-10?mul(a,1/n):V{0,1,0};}
V rotate(bmx::Quat q,V v){V u{q[0],q[1],q[2]};return add(v,add(mul(cross(u,v),2*q[3]),mul(cross(u,cross(u,v)),2)));}
V local(const bmx::Output&o,V p){return add(o.bike_root,rotate(o.body.orientation,p));}

struct App {
    HWND window{};HDC dc{};HGLRC gl{};GLuint font{};
    int width=1280,height=720; bool focused=true,running=true,scripted=false,observed=false,observed_v2=false,camera_behind=false;
    double seconds=0,limit=0,accumulator=0;unsigned ticks=0,bad=0,pad_ticks=0,hop_count=0;
    bmx::Tuning tuning; bmx::SimulationSettings simulation; bmx::RiderSettings rider_settings;
    bmx::FlatWorld world; bmx::Output out{}; bmx::RawInput raw{};
    std::unique_ptr<bmx::CoupledWorld> coupled; bmx::CoupledOutput coupled_out{};
    bmx::PoseSample posture{}; bmx::PoseFrame posture_reference{};
    bmx::VehiclePose vehicle_posture{};bmx::IKCommands ik_commands{};
    std::array<const bmx::VehiclePoseTrack*,bmx::vehicle_pose_node_count> vehicle_tracks{};
    double pose_sample_seconds{};
    const bmx::PoseTrack* pose_track{}; double pose_elapsed{},pose_recovery_elapsed{};bool pose_recovering{},pose_finished{};
    bmx::TrickId last_selection{bmx::TrickId::none};
    std::uint32_t pose_device{};
    bool reset_down{},reset_held{};unsigned resets{};
    HMODULE xinput{};DWORD(WINAPI*get_state)(DWORD,XINPUT_STATE*){};
    std::string source="NO CONTROLLER",last_event="Release controls to arm";
    std::ofstream csv,frame_csv,rider_json,trick_events,coupled_json;std::string csv_path,frame_path;
    unsigned frames=0,bone_frames=0;double last_frame=-1,last_bone=-1,bone_interval=1./30;
    std::chrono::steady_clock::time_point start;double start_utc_seconds{};
} app;
double step_seconds(){return app.coupled?app.coupled->fixed_seconds():app.world.fixed_seconds();}
void reset_world(){
    if(app.coupled)app.coupled->reset();else app.world.reset();
    app.pose_track=nullptr;app.posture={};app.last_selection=bmx::TrickId::none;++app.resets;
    app.last_event="RESET  /  release controls to arm";
}
void update_posture(){
    app.posture={};app.vehicle_posture={};app.vehicle_tracks={};app.ik_commands={};
    if(!app.out.valid||!app.out.actions.active||!app.raw.available||!app.raw.focused||app.raw.menu_open||
       (app.pose_track&&app.raw.device!=app.pose_device)){app.pose_track=nullptr;app.last_selection=bmx::TrickId::none;}
    auto neutral_output=app.out;neutral_output.tricks={};neutral_output.actions.preload=0;
    const auto neutral_rider=bmx::rider_pose(neutral_output,app.rider_settings,app.coupled?&app.coupled_out.posture_frame:nullptr);
    const auto all_tracks=bmx::streets_observed_pose_tracks();
    const auto q=app.coupled?app.coupled_out.posture_frame.orientation:app.out.body.orientation;
    const auto request=app.out.actions.movement;
    const bool accepted=!app.out.grounded&&request!=bmx::TrickId::none&&request!=app.last_selection&&
        (app.out.tricks.movement_id==request||
         (bmx::catalog::legacy_rotation(request)&&(app.out.tricks.bar_rotating||app.out.tricks.tail_rotating)));
    if(app.observed_v2&&accepted){app.pose_track=bmx::find_pose_track(request);app.pose_elapsed=app.pose_recovery_elapsed=0;app.pose_recovering=app.pose_finished=false;app.pose_device=app.raw.device;}
    else app.pose_elapsed+=step_seconds();
    app.last_selection=request;
    V neutral_hips{};const auto*neutral_track=app.pose_track?app.pose_track:all_tracks.empty()?nullptr:&all_tracks.front();
    if(neutral_track){const auto&h=neutral_track->neutral[unsigned(bmx::PoseBoneId::Hips)];neutral_hips={h.position[0],h.position[1],h.position[2]};}
    app.posture_reference={sub(neutral_rider.hips,rotate(q,neutral_hips)),q};
    if(!app.pose_track)return;
    const double end=app.pose_track->keys.empty()?0:app.pose_track->keys.back().seconds;
    if(!app.pose_finished&&app.out.tricks.movement_id==app.pose_track->id&&app.out.tricks.movement_caught){app.pose_finished=true;app.pose_elapsed=end;}
    const bool controlled=app.pose_track->hold_available&&app.out.tricks.movement_id==app.pose_track->id&&!app.pose_finished;
    if(!controlled&&app.pose_elapsed>end+.15){app.pose_track=nullptr;return;}
    // This is an explicit reduction: the physical rider drives our presentation
    // holder. It is not claimed to be the native Animated Skeleton parent.
    bmx::PoseSample local_posture{};
    if(controlled){
        if(app.out.tricks.movement_recovering){if(app.pose_recovering)app.pose_recovery_elapsed+=step_seconds();else app.pose_recovery_elapsed=0;}
        app.pose_recovering=app.out.tricks.movement_recovering;
        const auto i=unsigned(app.pose_track->id);double duration=app.simulation.trick_settings.recover_seconds[i];
        if(duration<=0)duration=.12;
        app.pose_sample_seconds=app.pose_recovering?
            app.pose_track->exit_start_seconds+(app.pose_track->exit_end_seconds-app.pose_track->exit_start_seconds)*std::clamp(app.pose_recovery_elapsed/duration,0.,1.):
            app.pose_track->enter_end_seconds*std::clamp(app.out.tricks.movement_progress,0.,1.);
        local_posture=bmx::sample_posture_phase(*app.pose_track,app.out.tricks.movement_progress,app.out.tricks.movement_holding,
            app.pose_recovering,app.pose_recovery_elapsed/duration);
    }else{
        app.pose_sample_seconds=app.pose_elapsed;
        const double recovery=std::clamp((end+.15-app.pose_elapsed)/.15,0.,1.);
        local_posture=bmx::sample_posture(*app.pose_track,app.pose_elapsed,recovery);
    }
    app.posture=bmx::posture_to_world(local_posture,app.posture_reference);
    app.out.rider=bmx::posture_rider(app.posture,app.out.rider);
    app.vehicle_posture=bmx::sample_vehicle_pose(app.pose_track->id,app.pose_sample_seconds);
    for(const auto&track:bmx::streets_observed_vehicle_pose_tracks())if(track.id==app.pose_track->id&&unsigned(track.node)<app.vehicle_tracks.size())app.vehicle_tracks[unsigned(track.node)]=&track;
    app.ik_commands=bmx::sample_ik(app.pose_track->id,app.pose_sample_seconds);
}
V render_part_point(const bmx::Output&o,V point,bmx::VehiclePoseNode node,bool tail=true){
    const auto index=unsigned(node);const auto*track=app.vehicle_tracks[index];
    if(app.observed_v2&&track&&(app.vehicle_posture[index].available&3)==3)
        return bmx::articulate_vehicle_point(sub(point,o.bike_com_from_root),*track,app.vehicle_posture[index],{o.body.center_of_mass,o.body.orientation});
    return bmx::bike_point(o,point,tail);
}
void capture_frame(){
    if(app.frame_path.empty()||app.width<1||app.height<1)return;
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-app.start).count();
    if(app.last_frame>=0&&elapsed-app.last_frame<.1)return;
    app.last_frame=elapsed;
    const int stride=(app.width*3+3)&~3;std::vector<unsigned char> pixels(static_cast<std::size_t>(stride)*app.height);
    glPixelStorei(GL_PACK_ALIGNMENT,4);glReadBuffer(GL_BACK);glReadPixels(0,0,app.width,app.height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
    for(int y=0;y<app.height;++y)for(int x=0;x<app.width;++x)std::swap(pixels[static_cast<std::size_t>(y)*stride+x*3],pixels[static_cast<std::size_t>(y)*stride+x*3+2]);
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size());
    info.biSize=sizeof(info);info.biWidth=app.width;info.biHeight=app.height;info.biPlanes=1;info.biBitCount=24;info.biSizeImage=static_cast<DWORD>(pixels.size());
    std::ostringstream name;name<<"frame-"<<std::setw(5)<<std::setfill('0')<<app.frames<<".bmp";
    std::ofstream image(std::filesystem::path(app.frame_path)/name.str(),std::ios::binary);
    image.write(reinterpret_cast<const char*>(&file),sizeof(file));image.write(reinterpret_cast<const char*>(&info),sizeof(info));image.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
    if(image){app.frame_csv<<app.frames<<','<<name.str()<<','<<std::setprecision(10)<<elapsed<<','<<app.seconds<<','<<app.width<<','<<app.height<<'\n';++app.frames;}
}
void capture_rider(){
    if(!app.rider_json)return;
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-app.start).count();
    if(app.last_bone>=0&&elapsed-app.last_bone<app.bone_interval)return;
    app.last_bone=elapsed;
    auto point=[&](const auto&p){app.rider_json<<'[';for(std::size_t j=0;j<p.size();++j){if(j)app.rider_json<<',';if(std::isfinite(p[j]))app.rider_json<<p[j];else app.rider_json<<"null";}app.rider_json<<']';};
    auto pair=[&](const auto&p){app.rider_json<<'[';point(p[0]);app.rider_json<<',';point(p[1]);app.rider_json<<']';};
    const auto&p=app.out.rider;app.rider_json<<std::setprecision(17)<<"{\"index\":"<<app.bone_frames<<",\"elapsed_seconds\":"<<elapsed<<",\"simulation_seconds\":"<<app.seconds<<",\"utc_seconds\":"<<app.start_utc_seconds+elapsed<<",\"body_position\":";point(app.out.body.center_of_mass);
        app.rider_json<<",\"valid\":"<<(app.out.valid?"true":"false")<<",\"body_quaternion\":";point(app.out.body.orientation);app.rider_json<<",\"bike_root\":";point(app.out.bike_root);app.rider_json<<",\"hips\":";point(p.hips);app.rider_json<<",\"chest\":";point(p.chest);app.rider_json<<",\"head\":";point(p.head);
        app.rider_json<<",\"shoulders\":";pair(p.shoulders);app.rider_json<<",\"elbows\":";pair(p.elbows);app.rider_json<<",\"hands\":";pair(p.hands);app.rider_json<<",\"hips_joint\":";pair(p.hips_joint);app.rider_json<<",\"knees\":";pair(p.knees);app.rider_json<<",\"feet\":";pair(p.feet);
    app.rider_json<<",\"hand_release\":";point(p.hand_release);app.rider_json<<",\"foot_release\":";point(p.foot_release);
    if(app.observed_v2){app.rider_json<<",\"pose_reference\":{\"id\":\"independent_posture_holder\",\"skeleton_bound\":true,\"kind\":\"independent rider-driven presentation holder\",\"position\":";point(app.posture_reference.position);app.rider_json<<",\"rotation\":";point(app.posture_reference.orientation);app.rider_json<<'}';}
    app.rider_json<<",\"pose_track_id\":"<<(app.pose_track?int(app.pose_track->id):-1)<<",\"pose_elapsed\":"<<app.pose_elapsed<<",\"pose_sample_seconds\":"<<app.pose_sample_seconds<<",\"pose_recovering\":"<<(app.pose_recovering?"true":"false");
    app.rider_json<<",\"pose_semantics\":\""<<(app.pose_track?"observed neutral-relative holder-local posture retargeted by independent rider frame":"procedural world-space joint targets; hand/foot endpoints use independent anchors")<<"\",\"bones\":{";
    bool first=true;auto bone=[&](const char*name,const V&position){if(!first)app.rider_json<<',';first=false;app.rider_json<<'"'<<name<<"\":{\"position\":";point(position);app.rider_json<<",\"rotation\":null}";};
    if(app.pose_track){for(unsigned i=0;i<bmx::pose_bone_count;++i){const auto&b=app.posture[i];if(!b.available)continue;if(!first)app.rider_json<<',';first=false;app.rider_json<<'"'<<bmx::pose_bone_names[i]<<"\":{\"position\":";if(b.available&bmx::pose_position)point(b.position);else app.rider_json<<"null";app.rider_json<<",\"rotation\":";if(b.available&bmx::pose_rotation)point(b.rotation);else app.rider_json<<"null";app.rider_json<<'}';}}
    else{bone("Hips",p.hips);bone("Chest",p.chest);bone("Head",p.head);
        for(unsigned i=0;i<2;++i){const char*names[2][6]={{"LeftUpperArm","LeftLowerArm","LeftHand","LeftUpperLeg","LeftLowerLeg","LeftFoot"},{"RightUpperArm","RightLowerArm","RightHand","RightUpperLeg","RightLowerLeg","RightFoot"}};
            bone(names[i][0],p.shoulders[i]);bone(names[i][1],p.elbows[i]);bone(names[i][2],p.hands[i]);bone(names[i][3],p.hips_joint[i]);bone(names[i][4],p.knees[i]);bone(names[i][5],p.feet[i]);}}
    app.rider_json<<"},\"vehicle_nodes\":{";first=true;
    for(unsigned i=0;i<bmx::vehicle_pose_node_count;++i){const auto&b=app.vehicle_posture[i];if(!b.available)continue;
        if(!first)app.rider_json<<',';first=false;app.rider_json<<'"'<<bmx::vehicle_pose_node_names[i]<<"\":{\"position\":";
        const auto local=V{b.position[0],b.position[1],b.position[2]};const auto position=add(app.out.body.center_of_mass,rotate(app.out.body.orientation,local));
        if(b.available&bmx::pose_position)point(position);else app.rider_json<<"null";
        app.rider_json<<",\"rotation\":";if(b.available&bmx::pose_rotation)point(bmx::pose_detail::multiply(app.out.body.orientation,bmx::pose_detail::quaternion(b.rotation)));else app.rider_json<<"null";
        app.rider_json<<",\"animation_control_target\":"<<(app.vehicle_tracks[i]&&app.vehicle_tracks[i]->animation_control_target?"true":"false")<<'}';}
    app.rider_json<<"},\"ik_controls\":[";
    for(unsigned i=0;i<app.ik_commands.size();++i){if(i)app.rider_json<<',';const auto&ik=app.ik_commands[i];app.rider_json<<"{\"limb\":"<<i<<",\"available\":"<<unsigned(ik.available)<<",\"position_weight\":"<<ik.position_weight<<",\"rotation_weight\":"<<ik.rotation_weight<<",\"reference_weight\":"<<ik.reference_weight<<",\"target_selector\":"<<ik.target_selector<<",\"binding\":"<<unsigned(ik.binding)<<'}';}
    app.rider_json<<"]}\n";++app.bone_frames;
}
void color(float r,float g,float b){glColor3f(r,g,b);}
void sphere(V c,double radius){
    constexpr int rings=8,slices=12;
    for(int j=0;j<rings;++j){glBegin(GL_QUAD_STRIP);for(int i=0;i<=slices;++i){
        double a=2*pi*i/slices;
        for(int k=0;k<2;++k){double b=-pi*.5+pi*(j+k)/rings;V n{std::cos(a)*std::cos(b),std::sin(b),std::sin(a)*std::cos(b)};
            glNormal3dv(n.data());V p=add(c,mul(n,radius));glVertex3dv(p.data());}}
        glEnd();}
}
void tube(V a,V b,double radius){
    V axis=unit(sub(b,a));V x=unit(cross(axis,std::abs(axis[1])>.9?V{1,0,0}:V{0,1,0}));V y=cross(axis,x);
    glBegin(GL_QUAD_STRIP);for(int i=0;i<=12;++i){double t=2*pi*i/12;V n=add(mul(x,std::cos(t)),mul(y,std::sin(t)));
        glNormal3dv(n.data());V p=add(a,mul(n,radius));glVertex3dv(p.data());p=add(b,mul(n,radius));glVertex3dv(p.data());}glEnd();
}
void look_at(V eye,V at){
    V f=unit(sub(at,eye)),s=unit(cross(f,{0,1,0})),u=cross(s,f);
    const double m[]={s[0],u[0],-f[0],0,s[1],u[1],-f[1],0,s[2],u[2],-f[2],0,0,0,0,1};
    glMultMatrixd(m);glTranslated(-eye[0],-eye[1],-eye[2]);
}
void wheel(V hub,V axle,V up,double phase){
    V forward=unit(cross(axle,up));double radius=bmx::geometry::wheel_radius;
    color(.065f,.08f,.10f);
    for(int i=0;i<48;++i){double a=2*pi*i/48,b=2*pi*(i+1)/48;
        tube(add(hub,mul(add(mul(up,std::cos(a)),mul(forward,std::sin(a))),radius)),
             add(hub,mul(add(mul(up,std::cos(b)),mul(forward,std::sin(b))),radius)),.022);}
    color(.53f,.64f,.68f);glLineWidth(1.2f);glBegin(GL_LINES);
    for(int i=0;i<16;++i){double a=phase+2*pi*i/16;glVertex3dv(hub.data());V p=add(hub,mul(add(mul(up,std::cos(a)),mul(forward,std::sin(a))),radius*.93));glVertex3dv(p.data());}glEnd();
    color(.12f,.19f,.23f);tube(add(hub,mul(axle,-.07)),add(hub,mul(axle,.07)),.035);
}
void bike(const bmx::Output&o){
    auto wide=[](std::array<float,3> p)->V{return {p[0],p[1],p[2]};};
    auto hub=[&](bool front){return wide(bmx::geometry::wheel_hub(front,front?static_cast<float>(o.steering_angle):0));};
    V front=render_part_point(o,hub(true),bmx::VehiclePoseNode::front_wheel,false),rear=render_part_point(o,hub(false),bmx::VehiclePoseNode::rear_wheel,true);
    for(bool f:{true,false}){V p=hub(f),center=f?front:rear;V axle_local=f?V{std::cos(o.steering_angle),0,-std::sin(o.steering_angle)}:V{1,0,0};
        const auto node=f?bmx::VehiclePoseNode::front_wheel:bmx::VehiclePoseNode::rear_wheel;
        V axle=unit(sub(render_part_point(o,add(p,axle_local),node,!f),center));V up=unit(sub(render_part_point(o,add(p,{0,1,0}),node,!f),center));
        wheel(center,axle,up,f?o.front_wheel_phase:o.rear_wheel_phase);}
    const double wb=bmx::geometry::wheelbase;
    V bb=render_part_point(o,{0,.31,-.106},bmx::VehiclePoseNode::chassis),seat=render_part_point(o,{0,.59,-.23},bmx::VehiclePoseNode::chassis),head=render_part_point(o,{0,.68,wb*.3},bmx::VehiclePoseNode::headset,false);
    color(1.f,.43f,.22f);tube(rear,bb,.024);tube(rear,seat,.022);tube(bb,seat,.03);tube(bb,head,.029);tube(seat,head,.025);
    color(.77f,.82f,.85f);tube(front,head,.023);
    const auto grip=[&](bool left){if(app.vehicle_tracks[unsigned(bmx::VehiclePoseNode::headset)])return render_part_point(o,wide(bmx::geometry::grip(left).center),bmx::VehiclePoseNode::headset,false);return bmx::grip_point(o,left);};
    V lh=grip(true),rh=grip(false),mid=mul(add(lh,rh),.5);
    tube(head,mid,.024);tube(lh,rh,.02);
    color(.09f,.13f,.17f);tube(add(seat,rotate(o.body.orientation,{0,.025,-.09})),add(seat,rotate(o.body.orientation,{0,.025,.07})),.05);
    for(bool left:{true,false}){const auto node=left?bmx::VehiclePoseNode::left_pedal:bmx::VehiclePoseNode::right_pedal;
        V pedal=app.vehicle_tracks[unsigned(node)]?render_part_point(o,wide(bmx::geometry::pedal(left).contact),node):bmx::pedal_point(o,left);
        color(.7f,.75f,.78f);tube(bb,pedal,.012);color(.08f,.13f,.16f);tube(add(pedal,rotate(o.body.orientation,{-.055,0,0})),add(pedal,rotate(o.body.orientation,{.055,0,0})),.022);}
}
void rider(const bmx::Output&o){
    const auto&p=o.rider;
    const V collar=mul(add(p.shoulders[0],p.shoulders[1]),.5);
    color(.12f,.72f,.68f);tube(p.hips,collar,.13);sphere(collar,.14);
    color(.88f,.67f,.49f);tube(collar,p.head,.055);sphere(p.head,.095);
    color(.14f,.19f,.28f);sphere(add(p.head,rotate(o.body.orientation,{0,.025,0})),.102);
    for(int i=0;i<2;++i){
        color(.12f,.72f,.68f);tube(p.shoulders[i],p.elbows[i],.041);sphere(p.elbows[i],.044);
        color(.88f,.67f,.49f);tube(p.elbows[i],p.hands[i],.032);sphere(p.hands[i],.038);
        color(.17f,.23f,.34f);tube(p.hips_joint[i],p.knees[i],.061);sphere(p.knees[i],.065);tube(p.knees[i],p.feet[i],.046);
        color(.88f,.90f,.86f);tube(p.feet[i],add(p.feet[i],rotate(o.body.orientation,{0,-.025,.10})),.044);
    }
}
void text(float x,float y,const std::string&s,float r=.88f,float g=.92f,float b=.96f){
    color(r,g,b);glRasterPos2f(x,y);glListBase(app.font);glCallLists(static_cast<GLsizei>(s.size()),GL_UNSIGNED_BYTE,s.data());
}
void rect(float x,float y,float w,float h,float r,float g,float b){color(r,g,b);glBegin(GL_QUADS);glVertex2f(x,y);glVertex2f(x+w,y);glVertex2f(x+w,y+h);glVertex2f(x,y+h);glEnd();}
std::string number(double x,int precision=1){std::ostringstream s;s<<std::fixed<<std::setprecision(precision)<<x;return s.str();}
void draw(){
    int w=app.width,h=app.height;if(w<1||h<1)return;
    glViewport(0,0,w,h);glClearColor(.035f,.060f,.10f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();double aspect=double(w)/h;glFrustum(-.055*aspect,.055*aspect,-.055,.055,.1,160);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();V root=app.out.bike_root;
    if(app.camera_behind){
        auto forward=rotate(app.out.body.orientation,{0,0,1});forward[1]=0;forward=unit(forward);
        look_at(add(add(root,mul(forward,-2.5)),{0,1.65,0}),add(root,{0,.75,0}));
    }else look_at(add(root,{3.1,1.9,-3.8}),add(root,{0,.7,0}));
    glEnable(GL_DEPTH_TEST);glDisable(GL_LIGHTING);
    color(.065f,.105f,.15f);glBegin(GL_QUADS);glVertex3d(root[0]-70,-.012,root[2]-70);glVertex3d(root[0]-70,-.012,root[2]+70);glVertex3d(root[0]+70,-.012,root[2]+70);glVertex3d(root[0]+70,-.012,root[2]-70);glEnd();
    glLineWidth(1);glBegin(GL_LINES);for(int i=-25;i<=25;++i){double x=std::floor(root[0])+i,z=std::floor(root[2])+i;color(.11f,.17f,.23f);glVertex3d(x,0,root[2]-25);glVertex3d(x,0,root[2]+25);glVertex3d(root[0]-25,0,z);glVertex3d(root[0]+25,0,z);}glEnd();
    // A fixed-height ground shadow conveys takeoff without moving the floor.
    color(.035f,.055f,.08f);glBegin(GL_TRIANGLE_FAN);glVertex3d(root[0],.002,root[2]);for(int i=0;i<=40;++i){double a=2*pi*i/40;glVertex3d(root[0]+.28*std::cos(a),.002,root[2]+.8*std::sin(a));}glEnd();
    glEnable(GL_LIGHTING);glEnable(GL_LIGHT0);glEnable(GL_COLOR_MATERIAL);glColorMaterial(GL_FRONT_AND_BACK,GL_AMBIENT_AND_DIFFUSE);
    const GLfloat light[]={-.6f,1.f,-.2f,0},ambient[]={.3f,.36f,.42f,1};glLightfv(GL_LIGHT0,GL_POSITION,light);glLightModelfv(GL_LIGHT_MODEL_AMBIENT,ambient);
    bike(app.out);rider(app.out);
    glDisable(GL_LIGHTING);glDisable(GL_DEPTH_TEST);glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,1280,720,0,-1,1);glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    rect(28,27,1224,86,.055f,.087f,.135f);rect(28,27,5,86,.12f,.82f,.72f);
    text(51,58,"BMX  /  MOTION CORE",.96f,.97f,.99f);text(51,88,app.observed_v2?"Observed profile v2  /  coupled rider and suspension":app.observed?"Observed profile v1  /  calibrated reduced model":"Standalone C++ module   /   physics + rider + 58 trick selections",.48f,.62f,.73f);
    text(876,58,app.scripted?"SCRIPTED INPUT REPLAY":app.source,.12f,.86f,.74f);
    text(876,88,number(1/step_seconds(),0)+" Hz simulation  /  "+number(app.seconds)+" s",.53f,.65f,.77f);
    rect(28,133,234,133,.055f,.087f,.135f);text(47,163,"GROUND SPEED",.48f,.62f,.73f);
    double speed=std::hypot(app.out.body.linear_velocity[0],app.out.body.linear_velocity[2]);
    text(47,196,number(speed*3.6)+" km/h",.97f,.66f,.37f);text(47,231,app.out.grounded?"GROUND CONTACT":"AIRBORNE",.12f,.86f,.74f);
    text(47,251,"Takeoffs: "+std::to_string(app.hop_count),.53f,.65f,.77f);
    rect(28,565,1224,127,.055f,.087f,.135f);text(50,594,app.last_event,.97f,.66f,.37f);
    text(50,625,"PAD   A pedal   B brake   LS steer / pitch   RS preload / manual / trick   R3 hop",.75f,.83f,.88f);
    text(50,653,"KEYS  E / Q pedal / brake   arrows LS   I J K L RS   Space hop   Z / X shoulders   R reset",.48f,.62f,.73f);
    text(50,678,"Independent implementation; Streets control references do not establish identical handling.",.38f,.49f,.60f);
    if(!app.out.valid)text(860,158,"INVALID CONTACT SAMPLE",1,.30f,.3f);
    capture_frame();capture_rider();SwapBuffers(app.dc);
}
double key(int k){return (GetAsyncKeyState(k)&0x8000)?1.:0.;}
void sample(){
    bmx::RawInput in;in.focused=app.focused;in.available=true;in.device=100;app.reset_down=false;
    if(app.scripted){
        in.focused=true;in.device=200;double t=std::fmod(app.seconds,12.0);
        in.pedal=t>.5&&t<4.0?1:0;
        in.quick_hop=(t>4.05&&t<4.15)||(t>7.25&&t<7.35);
        if(t>4.30&&t<4.43){in.left_shoulder=true;in.right_y=-1;}
        if(t>7.5&&t<7.63){in.left_trigger=1;in.right_trigger=1;in.right_x=1;}
        if(t>9.1&&t<11.0)in.brake=1;
        app.source="SCRIPTED INPUT REPLAY";
    }else if(app.focused){
        bool pad=false;for(DWORD slot=0;app.get_state&&slot<4;++slot){XINPUT_STATE s{};if(app.get_state(slot,&s)!=ERROR_SUCCESS)continue;pad=true;
            const auto&g=s.Gamepad;auto axis=[](SHORT v){return std::clamp(double(v)/32767.,-1.,1.);};
            in.device=slot+1;in.left_x=axis(g.sThumbLX);in.left_y=axis(g.sThumbLY);in.right_x=axis(g.sThumbRX);in.right_y=axis(g.sThumbRY);
            in.pedal=(g.wButtons&XINPUT_GAMEPAD_A)?1:0;in.brake=(g.wButtons&XINPUT_GAMEPAD_B)?1:0;in.quick_hop=(g.wButtons&XINPUT_GAMEPAD_RIGHT_THUMB)!=0;
            in.left_shoulder=(g.wButtons&XINPUT_GAMEPAD_LEFT_SHOULDER)!=0;in.right_shoulder=(g.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER)!=0;
            in.left_trigger=g.bLeftTrigger/255.;in.right_trigger=g.bRightTrigger/255.;app.reset_down=(g.wButtons&XINPUT_GAMEPAD_DPAD_DOWN)!=0;app.source="LIVE XINPUT / SLOT "+std::to_string(slot);++app.pad_ticks;break;}
        if(!pad){in.left_x=key(VK_RIGHT)-key(VK_LEFT);in.left_y=key(VK_UP)-key(VK_DOWN);in.right_x=key('L')-key('J');in.right_y=key('I')-key('K');
            in.pedal=key('E');in.brake=key('Q');in.quick_hop=key(VK_SPACE)!=0;in.left_shoulder=key('Z')!=0;in.right_shoulder=key('X')!=0;in.left_trigger=key('C');in.right_trigger=key('V');app.source="KEYBOARD / NO XINPUT";}
    }
    app.raw=in;
}
void tick(){
    sample();if(app.reset_down&&!app.reset_held)reset_world();app.reset_held=app.reset_down;
    if(app.coupled){app.coupled_out=app.coupled->step(app.raw);app.out=app.coupled_out.bike;}
    else app.out=app.world.step(app.raw);
    app.seconds+=step_seconds();++app.ticks;update_posture();
    if(!app.out.valid)++app.bad;
    if(app.out.events.took_off){++app.hop_count;app.last_event="TAKEOFF  /  neutral RS, then select a trick";}
    if(app.out.events.bar_caught||app.out.events.tail_caught||app.out.events.movement_caught)app.last_event="TRICK CAUGHT  /  ready to land";
    if(app.out.events.combo_landed)app.last_event="COMBO LANDED  /  "+std::to_string(app.out.events.units)+" units";
    if(app.out.events.failed)app.last_event="COMBO FAILED  /  release controls and try again";
    if(app.out.actions.movement!=bmx::TrickId::none){for(const auto&e:bmx::catalog::entries)if(e.id==app.out.actions.movement){app.last_event=std::string(e.name)+"  /  requested";break;}}
    if(app.out.tricks.movement_id!=bmx::TrickId::none){
        for(const auto&e:bmx::catalog::entries)if(e.id==app.out.tricks.movement_id){app.last_event=std::string(e.name)+"  /  "+number(100*app.out.tricks.movement_progress,0)+"%";break;}}
    if(app.csv){const auto&p=app.out.body.center_of_mass;const auto&v=app.out.body.linear_velocity;
        const auto&q=app.out.body.orientation;const auto&w=app.out.body.angular_velocity;
        const double utc=std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        app.csv<<std::setprecision(17)<<app.seconds<<','<<app.raw.device<<','<<app.out.valid<<','<<app.out.grounded<<','<<p[0]<<','<<p[1]<<','<<p[2]<<','<<v[0]<<','<<v[1]<<','<<v[2]<<','<<app.raw.pedal<<','<<app.raw.brake<<','<<app.raw.quick_hop<<','<<app.raw.right_x<<','<<app.raw.right_y<<','<<int(app.out.tricks.movement_id)<<','<<app.out.tricks.bar_angle<<','<<app.out.tricks.tail_angle<<','<<app.out.events.combo_landed<<','<<utc<<','<<w[0]<<','<<w[1]<<','<<w[2]<<','<<q[0]<<','<<q[1]<<','<<q[2]<<','<<q[3]<<','<<app.out.actions.preload<<','<<app.out.actions.manual<<','<<app.out.actions.nose_manual<<','<<app.out.actions.air_pitch<<','<<app.out.actions.steer<<','<<app.out.crank_phase
            <<','<<app.out.hop_impulse[1]<<','<<app.out.events.failed<<','<<app.out.events.canceled<<','<<int(app.out.tricks.failure)
            <<','<<app.out.tricks.bar_caught<<','<<app.out.tricks.tail_caught<<','<<app.out.tricks.bar_release<<','<<app.out.tricks.tail_release
            <<','<<int(app.out.actions.movement)<<','<<app.out.tricks.movement_phase<<','<<app.out.tricks.movement_progress
            <<','<<app.out.tricks.movement_holding<<','<<app.out.tricks.movement_recovering<<','<<app.out.tricks.movement_caught
            <<','<<app.out.events.movement_caught<<','<<app.out.events.landed
            <<','<<app.out.tricks.tally.landed_movements<<','<<app.out.tricks.tally.landed_barspins<<','<<app.out.tricks.tally.landed_tailwhips<<','<<app.resets<<'\n';}
    if(app.coupled_json&&app.coupled){
        auto vector=[&](const auto&v){app.coupled_json<<'[';for(std::size_t i=0;i<v.size();++i){if(i)app.coupled_json<<',';if(std::isfinite(v[i]))app.coupled_json<<v[i];else app.coupled_json<<"null";}app.coupled_json<<']';};
        const auto&c=app.coupled_out;app.coupled_json<<std::setprecision(17)<<"{\"simulation_seconds\":"<<app.seconds<<",\"utc_seconds\":"<<std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()<<",\"valid\":"<<(c.valid?"true":"false")<<",\"converged\":"<<(c.converged?"true":"false")<<",\"coupling_residual\":"<<c.coupling_residual<<",\"reset_counter\":"<<app.resets<<",\"joint_actuation_available\":"<<(c.joint_actuation_available?"true":"false")<<",\"joint_actuation_id\":"<<c.joint_actuation_id<<",\"joint_actuation_seconds\":"<<c.joint_actuation_seconds<<",\"observed_chassis_com\":";vector(c.observed_chassis_com);
        app.coupled_json<<",\"rider_com\":";vector(c.rider.center_of_mass);app.coupled_json<<",\"rider_velocity\":";vector(c.rider.linear_velocity);app.coupled_json<<",\"rider_relative_com\":";vector(c.rider_relative_com);app.coupled_json<<",\"rider_quaternion\":";vector(c.rider.orientation);
        app.coupled_json<<",\"joint_displacement\":";vector(c.suspension.displacement);app.coupled_json<<",\"joint_velocity\":";vector(c.suspension.linear_velocity);app.coupled_json<<",\"rider_force\":";vector(c.rider_force);app.coupled_json<<",\"rider_torque\":";vector(c.rider_torque);app.coupled_json<<",\"rider_hop_impulse\":";vector(c.rider_hop_impulse);
        app.coupled_json<<",\"rider_stabilization_torque\":";vector(c.rider_stabilization.torque);app.coupled_json<<",\"chassis_stabilization_torque\":";vector(c.chassis_stabilization.torque);
        app.coupled_json<<",\"manual_pitch_setpoint_degrees\":"<<c.manual_pitch_setpoint_degrees<<",\"manual_chassis_torque\":";vector(c.manual_chassis_torque);app.coupled_json<<",\"manual_rider_torque\":";vector(c.manual_rider_torque);
        app.coupled_json<<",\"coupling_iterations\":"<<c.coupling_iterations<<",\"swept_plane_normal_mask\":"<<c.bike.swept_plane_normal_mask<<",\"swept_plane_normal_impulse\":";vector(c.bike.swept_plane_normal_impulse);
        app.coupled_json<<",\"hop_kind\":"<<int(c.bike.actions.hop_kind)<<",\"hop_request_accepted\":"<<(c.bike.hop_request_accepted?"true":"false")<<",\"flick_preloading\":"<<(c.bike.actions.flick_preloading?"true":"false");
        app.coupled_json<<",\"input_wheel_contacts\":[";
        for(unsigned i=0;i<c.input_contacts.size();++i){if(i)app.coupled_json<<',';const auto&query=c.input_contacts[i];
            app.coupled_json<<"{\"wheel\":\""<<(i==0?"front":"rear")<<"\",\"status\":"<<int(query.contact.status)<<",\"separation\":"<<query.separation<<",\"point\":";vector(query.contact.point);app.coupled_json<<",\"normal\":";vector(query.contact.normal);app.coupled_json<<'}';}
        app.coupled_json<<']';
        app.coupled_json<<",\"spring_energy\":"<<c.suspension.predicted_energy<<",\"saturated\":"<<(c.suspension.saturated?"true":"false")<<"}\n";
    }
    if(app.trick_events&&(app.out.actions.movement!=bmx::TrickId::none||app.out.events.took_off||app.out.events.landed||app.out.events.combo_landed||app.out.events.failed||app.out.events.canceled||app.out.events.bar_caught||app.out.events.tail_caught||app.out.events.movement_caught)){
        app.trick_events<<std::setprecision(17)<<"{\"simulation_seconds\":"<<app.seconds<<",\"utc_seconds\":"<<std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()
            <<",\"selected_action_trick\":"<<int(app.out.actions.movement)<<",\"movement_id\":"<<int(app.out.tricks.movement_id)<<",\"grounded\":"<<(app.out.grounded?"true":"false")
            <<",\"took_off\":"<<(app.out.events.took_off?"true":"false")<<",\"landed\":"<<(app.out.events.landed?"true":"false")<<",\"combo_landed\":"<<(app.out.events.combo_landed?"true":"false")
            <<",\"failed\":"<<(app.out.events.failed?"true":"false")<<",\"canceled\":"<<(app.out.events.canceled?"true":"false")
            <<",\"bar_caught\":"<<(app.out.events.bar_caught?"true":"false")<<",\"tail_caught\":"<<(app.out.events.tail_caught?"true":"false")<<",\"movement_caught\":"<<(app.out.events.movement_caught?"true":"false")
            <<",\"landed_by_id\":[";for(std::size_t i=0;i<app.out.tricks.tally.landed_by_id.size();++i){if(i)app.trick_events<<',';app.trick_events<<app.out.tricks.tally.landed_by_id[i];}app.trick_events<<"]}\n";
    }
    if(app.limit>0&&app.seconds>=app.limit)PostMessageW(app.window,WM_CLOSE,0,0);
}
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_SIZE){app.width=LOWORD(lp);app.height=HIWORD(lp);return 0;}
    if(message==WM_ACTIVATE){app.focused=LOWORD(wp)!=WA_INACTIVE;return 0;}
    if(message==WM_KEYDOWN&&wp=='R'){reset_world();return 0;}
    if(message==WM_CLOSE){DestroyWindow(window);return 0;}
    if(message==WM_DESTROY){app.running=false;PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,wp,lp);
}
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE,LPSTR command,int){
    std::istringstream args(command);std::string arg;
    auto path_arg=[&](std::string&path){args>>std::ws;if(args.peek()=='"'){args.get();std::getline(args,path,'"');}else args>>path;};
    while(args>>arg){if(arg=="--scripted")app.scripted=true;else if(arg=="--streets-observed-v1"){
            const auto profile=bmx::streets_observed_v1();app.tuning=profile.tuning;app.simulation=profile.simulation;app.rider_settings=profile.rider;app.observed=true;
        }else if(arg=="--streets-observed-v2"){
            const auto profile=bmx::streets_observed_v2();app.tuning=profile.movement.tuning;app.simulation=profile.movement.simulation;app.rider_settings=profile.movement.rider;app.observed_v2=true;app.observed=true;
        }else if(arg=="--camera-behind")app.camera_behind=true;else if(arg=="--seconds")args>>app.limit;else if(arg=="--csv")path_arg(app.csv_path);else if(arg=="--frames")path_arg(app.frame_path);
        else if(arg=="--step-seconds")args>>app.simulation.seconds;else if(arg=="--gravity")args>>app.tuning.gravity[1];else if(arg=="--mass")args>>app.simulation.mass;
        else if(arg=="--hop-speed")args>>app.tuning.hop_delta_speed;else if(arg=="--pedal-torque")args>>app.tuning.pedal_torque;
        else if(arg=="--brake-torque"){args>>app.tuning.front_brake_torque;app.tuning.rear_brake_torque=app.tuning.front_brake_torque*.75;}}
    if(!std::isfinite(app.simulation.seconds)||app.simulation.seconds<1./240||app.simulation.seconds>1./30)return 7;
    app.world=bmx::FlatWorld(app.tuning,app.simulation,app.rider_settings);
    if(app.observed_v2){auto profile=bmx::streets_observed_v2();profile.movement={app.tuning,app.simulation,app.rider_settings};app.coupled=std::make_unique<bmx::CoupledWorld>(profile);}
    if(!app.csv_path.empty()){app.csv.open(app.csv_path);app.csv<<"time,device,valid,grounded,x,y,z,vx,vy,vz,pedal,brake,quick_hop,rs_x,rs_y,trick_id,bar_angle,tail_angle,combo_landed,utc_seconds,wx,wy,wz,qx,qy,qz,qw,preload,manual,nose_manual,air_pitch,steer,crank_phase,hop_impulse_y,combo_failed,combo_canceled,trick_failure,bar_caught,tail_caught,bar_release,tail_release,selected_action_trick,movement_phase,movement_progress,movement_holding,movement_recovering,movement_caught,event_movement_caught,event_landed,landed_movements,landed_barspins,landed_tailwhips,reset_counter\n";}
    if(!app.frame_path.empty()){
        std::error_code error;if(!std::filesystem::create_directory(app.frame_path,error))return 5;
        app.frame_csv.open(std::filesystem::path(app.frame_path)/"frames.csv");app.frame_csv<<"index,file,elapsed_seconds,simulation_seconds,width,height\n";
        app.rider_json.open(std::filesystem::path(app.frame_path)/"rider.jsonl");
        app.trick_events.open(std::filesystem::path(app.frame_path)/"trick-events.jsonl");
        if(app.coupled)app.coupled_json.open(std::filesystem::path(app.frame_path)/"coupled-physics.jsonl");
        if(!app.frame_csv)return 6;
    }
    for(const auto*n:{L"xinput1_4.dll",L"xinput1_3.dll",L"xinput9_1_0.dll"}){app.xinput=LoadLibraryExW(n,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(app.xinput){app.get_state=reinterpret_cast<decltype(app.get_state)>(GetProcAddress(app.xinput,"XInputGetState"));if(app.get_state)break;FreeLibrary(app.xinput);app.xinput=nullptr;}}
    SetProcessDPIAware();WNDCLASSW wc{};wc.style=CS_OWNDC;wc.lpfnWndProc=window_proc;wc.hInstance=instance;wc.lpszClassName=L"BmxMovementStandalone";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
    app.window=CreateWindowW(wc.lpszClassName,L"BMX Movement | Standalone C++",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1296,759,nullptr,nullptr,instance,nullptr);
    if(!app.window)return 2;app.dc=GetDC(app.window);PIXELFORMATDESCRIPTOR pfd{};pfd.nSize=sizeof(pfd);pfd.nVersion=1;pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=24;pfd.cDepthBits=24;
    int pf=ChoosePixelFormat(app.dc,&pfd);if(!pf||!SetPixelFormat(app.dc,pf,&pfd))return 3;app.gl=wglCreateContext(app.dc);if(!app.gl||!wglMakeCurrent(app.dc,app.gl))return 4;
    HFONT font=CreateFontA(-16,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,ANSI_CHARSET,OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,FF_DONTCARE,"Consolas");SelectObject(app.dc,font);app.font=glGenLists(256);wglUseFontBitmapsA(app.dc,0,256,app.font);
    if(app.coupled){app.coupled_out=app.coupled->step({});app.out=app.coupled_out.bike;}else app.out=app.world.step({});ShowWindow(app.window,SW_SHOW);UpdateWindow(app.window);
    auto last=std::chrono::steady_clock::now();app.start=last;app.start_utc_seconds=std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();MSG msg{};
    while(app.running){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)app.running=false;TranslateMessage(&msg);DispatchMessageW(&msg);}if(!app.running)break;
        auto now=std::chrono::steady_clock::now();double elapsed=std::chrono::duration<double>(now-last).count();last=now;app.accumulator+=std::min(elapsed,.1);
        while(app.accumulator>=step_seconds()){tick();app.accumulator-=step_seconds();}draw();Sleep(1);}
    if(!app.csv_path.empty()){std::ofstream summary(app.csv_path+".summary.json");summary<<"{\n  \"scripted\": "<<(app.scripted?"true":"false")<<",\n  \"ticks\": "<<app.ticks<<",\n  \"invalid_ticks\": "<<app.bad<<",\n  \"xinput_ticks\": "<<app.pad_ticks<<",\n  \"takeoffs\": "<<app.hop_count<<",\n  \"landed_combos\": "<<app.out.tricks.tally.landed_combos<<"\n}\n";}
    if(!app.frame_path.empty()){
        std::ofstream summary(std::filesystem::path(app.frame_path)/"capture-summary.json");
        summary<<std::setprecision(17)<<"{\n  \"schema\": 2,\n  \"completed\": true,\n  \"frames\": "<<app.frames
            <<",\n  \"rider_pose_samples\": "<<app.bone_frames<<",\n  \"requested_rider_pose_hz\": "<<1/app.bone_interval
            <<",\n  \"rendered_live\": true,\n  \"window_capture\": false,\n  \"scripted\": "<<(app.scripted?"true":"false")
            <<",\n  \"profile\": \""<<(app.observed_v2?"streets_observed_v2":app.observed?"streets_observed_v1":"legacy_default")<<"\",\n  \"camera\": \""<<(app.camera_behind?"behind_adaptation":"diagonal_adaptation")
            <<"\",\n  \"start_utc_seconds\": "<<app.start_utc_seconds<<",\n  \"fixed_step\": "<<step_seconds()
            <<",\n  \"gravity_y\": "<<app.tuning.gravity[1]<<",\n  \"mass\": "<<app.simulation.mass<<",\n  \"hop_speed\": "<<app.tuning.hop_delta_speed
            <<",\n  \"hop_delay_seconds\": "<<app.simulation.hop_delay_seconds<<",\n  \"hop_impulse_steps\": "<<app.simulation.hop_impulse_steps
            <<",\n  \"brake_min_scale\": "<<app.tuning.brake_min_scale<<",\n  \"brake_max_scale\": "<<app.tuning.brake_max_scale<<",\n  \"brake_speed_gain\": "<<app.tuning.brake_speed_gain
            <<",\n  \"contact_gap\": "<<app.tuning.contact_gap<<",\n  \"force_limit\": "<<app.tuning.force_limit<<",\n  \"landing_max_closing_speed\": "<<app.simulation.trick_settings.maximum_closing_speed
            <<",\n  \"pedal_torque\": "<<app.tuning.pedal_torque<<",\n  \"invalid_ticks\": "<<app.bad<<"\n}\n";
    }
    wglMakeCurrent(nullptr,nullptr);wglDeleteContext(app.gl);ReleaseDC(app.window,app.dc);DeleteObject(font);if(app.xinput)FreeLibrary(app.xinput);return app.bad?1:0;
}
