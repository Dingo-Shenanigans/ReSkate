#include "Extension/BMX/bmx_body_guard.h"
#include "Extension/BMX/bmx_collision_envelope.h"
#include <cstdlib>
#include <iostream>
using namespace dingosdk::bmx;
namespace {
void check(bool ok,const char* why) {if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
struct Plane {
    body_guard::Vec normal{0,1,0};float offset{};
    body_guard::Ray operator()(body_guard::Vec a,body_guard::Vec b) const {
        const auto start=body_guard::math::dot(a,normal)-offset,end=body_guard::math::dot(b,normal)-offset;
        if(start<=0||end>0)return {true,false};
        const auto t=static_cast<float>(start/(start-end));
        return {true,true,body_guard::math::add(a,body_guard::math::mul(body_guard::math::sub(b,a),t)),t};
    }
};
}
int main() {
    auto clear=[](auto,auto){return body_guard::Ray{true,false};};
    auto x=body_guard::search({0,3,0},.16f,{0,-5,0},{0,3.08f,0},{0,-5,0},true,clear);
    check(x.available&&!x.hit,"clear air never synthesizes a fall contact");
    x=body_guard::search({0,.16f,0},.16f,{0,-.1f,0},{0,.24f,0},{0,-5,0},true,Plane{});
    check(x.available&&x.hit&&std::abs(x.separation)<1e-5&&x.normal[1]>.999&&x.closing_speed==5,
        "fresh floor fit preserves measured incoming speed after collision response");
    x=body_guard::search({.02f,1,0},.055f,{-4,0,0},{.09f,1,0},{-4,0,0},true,Plane{{1,0,0}});
    check(x.available&&x.hit&&!x.swept_crossing&&x.normal[0]>.999&&x.separation<0,"handlebar sphere overlapping wall is detected by current witness");
    x=body_guard::search({-.4f,1,0},.16f,{-30,0,0},{.3f,1,0},{-30,0,0},true,Plane{{1,0,0}});
    check(x.available&&x.hit&&x.swept_crossing&&x.separation<-.5f,"thin wall crossing survives a center now beyond the face");
    x=body_guard::search({0,-.02f,0},.16f,{0,-6,0},{0,.08f,0},{},true,Plane{});
    check(x.available&&x.hit&&x.swept_crossing&&std::abs(x.separation+.18f)<1e-5f&&x.closing_speed==6,
        "shallow sphere overlap cannot hide the next witnessed center crossing");
    bail_policy::Policy policy;
    bail_policy::Sample impact;impact.owner={1,2,3,4,5,6,7,8,9};impact.serial=10;
    impact.seconds=bail_policy::native_seconds;impact.mounted=true;impact.body_up={0,1,0};impact.contact_count=1;
    auto& contact=impact.contacts[0];contact.region=bail_policy::Region::head;contact.available=contact.hit=true;
    contact.query_serial=impact.serial;contact.normal={0,1,0};contact.separation=-.08;contact.closing_speed=0;
    auto decision=policy.step(impact);
    check(decision.valid&&!decision.bail,"previous shallow nonclosing contact seeds policy history without a bail");
    ++impact.serial;contact.query_serial=impact.serial;contact.separation=x.separation;contact.closing_speed=x.closing_speed;
    contact.swept_crossing=x.swept_crossing;
    decision=policy.step(impact);
    check(decision.valid&&decision.bail&&decision.reason==bail_policy::Reason::swept_body_crossing,
        "consecutive guard center crossing beyond ordinary depth is accepted by impact policy");
    bail_policy::Policy no_history;
    check(!no_history.step(impact).valid,"impact policy independently rejects a crossing without consecutive history");
    x=body_guard::search({0,-.03f,0},.16f,{0,-1.8f,0},{0,-.01f,0},{0,-1.8f,0},true,Plane{});
    check(x.available&&!x.hit&&!x.swept_crossing,"existing deep overlap without a new center crossing stays unauthorized");
    x=body_guard::search({0,-.02f,0},.16f,{0,-6,0},{0,.08f,0},{0,-6,0},false,Plane{});
    check(x.available&&!x.hit&&!x.swept_crossing,"center crossing requires consecutive history");
    auto finite_floor=[](auto a,auto b) {
        auto hit=Plane{}(a,b);
        if(hit.hit&&hit.point[0]>0)hit.hit=false;
        return hit;
    };
    x=body_guard::search({-.02f,-.02f,0},.16f,{16.8f,-6,0},{-.30f,.08f,0},{},true,finite_floor);
    check(x.available&&x.hit&&x.swept_crossing,"finite face accepts a historical center crossing inside its real edge");
    x=body_guard::search({-.02f,-.02f,0},.16f,{-19.2f,-6,0},{.30f,.08f,0},{-19.2f,-6,0},true,finite_floor);
    check(x.available&&!x.hit&&!x.swept_crossing,
        "finite face beneath current center cannot authorize a historical crossing beyond its edge");
    auto bad=[](auto,auto){return body_guard::Ray{false,false};};
    check(!body_guard::search({0,.16f,0},.16f,{},{},{},false,bad).available,"unavailable query is not clear");
    auto invalid=[](auto,auto){return body_guard::Ray{true,true,{7,8,9},.5f};};
    check(!body_guard::search({0,.16f,0},.16f,{},{},{},false,invalid).available,"malformed hit is rejected");
    auto center_only=[](auto a,auto b) {
        if(std::abs(a[0])>.001f||std::abs(a[2])>.001f)return body_guard::Ray{true,false};
        return Plane{}(a,b);
    };
    x=body_guard::search({0,.16f,0},.16f,{0,-5,0},{},{},false,center_only);
    check(x.available&&!x.hit,"one ray cannot invent a surface normal");
    const float q=.70710678118f;
    x=body_guard::search({0,.16f/q,0},.16f,{0,-3,0},{},{},false,Plane{{q,q,0}});
    check(x.available&&x.hit&&std::abs(x.normal[0]-q)<.001f&&std::abs(x.separation)<.001f,"oblique current surface is fitted");
    const float slope=.4040262258f,norm=std::sqrt(1+slope*slope);
    auto finite_lip=[&](auto a,auto b) {
        auto hit=Plane{{-slope/norm,1/norm,0}}(a,b);
        if(hit.hit&&hit.point[0]>0)hit.hit=false;
        return hit;
    };
    x=body_guard::search({-.026f,.1815f,0},.18f,{0,-3,0},{},{},false,finite_lip);
    check(x.available&&!x.hit,"finite lip cannot become an infinite collision plane beyond its edge");
    for(const auto& shape:collision_envelope::capsules) {
        check(shape.radius>0,"all collision shapes have volume");
        check(std::min(shape.a[1],shape.b[1])-shape.radius>=.029f,"upright envelope remains above rolling tire floor");
    }
    for(unsigned i=0;i<4;++i) {
        const auto& guard=collision_envelope::guards[i];
        const auto& shape=collision_envelope::capsules[i<2?2:3];
        const auto expected=i%2==0?shape.a:shape.b;
        check(guard.center==expected&&guard.radius==shape.radius,
            "frame and bar evidence guards share the scaled native capsule endpoints");
    }
    check(collision_envelope::capsules.size()==8&&collision_envelope::guards.size()==8&&
        collision_envelope::capsules[5].radius==.16f&&collision_envelope::capsules[6].radius==.18f&&
        collision_envelope::capsules[7].radius==.16f,
        "retargeting preserves all eight shapes/guards and the original human radii");
    // Independent extrema of each named joint in the source-bound retarget grid.
    // These box corners extend beyond individual recorded rows. They do not
    // establish skinned-body thickness or released trick-limb collision coverage.
    struct CenterFixture {collision_envelope::Vec point;unsigned shape;};
    constexpr CenterFixture centers[]{
        {{-5.775690079e-05f,0.8201791644f,-0.3228176832f},5},
        {{-5.775690079e-05f,0.8201791644f,-0.2852166295f},5},
        {{-5.775690079e-05f,0.957916081f,-0.3228176832f},5},
        {{-5.775690079e-05f,0.957916081f,-0.2852166295f},5},
        {{5.588680506e-05f,0.8201791644f,-0.3228176832f},5},
        {{5.588680506e-05f,0.8201791644f,-0.2852166295f},5},
        {{5.588680506e-05f,0.957916081f,-0.3228176832f},5},
        {{5.588680506e-05f,0.957916081f,-0.2852166295f},5},
        {{-9.286403656e-05f,0.9113448262f,-0.3467159867f},6},
        {{-9.286403656e-05f,0.9113448262f,-0.3090572655f},6},
        {{-9.286403656e-05f,1.049087763f,-0.3467159867f},6},
        {{-9.286403656e-05f,1.049087763f,-0.3090572655f},6},
        {{9.274482727e-05f,0.9113448262f,-0.3467159867f},6},
        {{9.274482727e-05f,0.9113448262f,-0.3090572655f},6},
        {{9.274482727e-05f,1.049087763f,-0.3467159867f},6},
        {{9.274482727e-05f,1.049087763f,-0.3090572655f},6},
        {{-0.0001180171967f,0.9386093616f,-0.2828730345f},6},
        {{-0.0001180171967f,0.9386093616f,-0.2467764616f},6},
        {{-0.0001180171967f,1.082424283f,-0.2828730345f},6},
        {{-0.0001180171967f,1.082424283f,-0.2467764616f},6},
        {{0.0001178383827f,0.9386093616f,-0.2828730345f},6},
        {{0.0001178383827f,0.9386093616f,-0.2467764616f},6},
        {{0.0001178383827f,1.082424283f,-0.2828730345f},6},
        {{0.0001178383827f,1.082424283f,-0.2467764616f},6},
        {{-0.0001030862331f,1.004715323f,-0.2174772024f},6},
        {{-0.0001030862331f,1.004715323f,-0.184902072f},6},
        {{-0.0001030862331f,1.154595494f,-0.2174772024f},6},
        {{-0.0001030862331f,1.154595494f,-0.184902072f},6},
        {{0.0001163184643f,1.004715323f,-0.2174772024f},6},
        {{0.0001163184643f,1.004715323f,-0.184902072f},6},
        {{0.0001163184643f,1.154595494f,-0.2174772024f},6},
        {{0.0001163184643f,1.154595494f,-0.184902072f},6},
        {{-0.0001207590103f,1.161830902f,-0.1168912053f},6},
        {{-0.0001207590103f,1.161830902f,-0.09262928367f},6},
        {{-0.0001207590103f,1.320885777f,-0.1168912053f},6},
        {{-0.0001207590103f,1.320885777f,-0.09262928367f},6},
        {{0.0001341402531f,1.161830902f,-0.1168912053f},6},
        {{0.0001341402531f,1.161830902f,-0.09262928367f},6},
        {{0.0001341402531f,1.320885777f,-0.1168912053f},6},
        {{0.0001341402531f,1.320885777f,-0.09262928367f},6},
        {{-0.004229366779f,1.273285151f,0.1128697395f},7},
        {{-0.004229366779f,1.273285151f,0.1542053819f},7},
        {{-0.004229366779f,1.482898712f,0.1128697395f},7},
        {{-0.004229366779f,1.482898712f,0.1542053819f},7},
        {{0.004224300385f,1.273285151f,0.1128697395f},7},
        {{0.004224300385f,1.273285151f,0.1542053819f},7},
        {{0.004224300385f,1.482898712f,0.1128697395f},7},
        {{0.004224300385f,1.482898712f,0.1542053819f},7},
    };
    const auto capsule_distance=[](auto point,const auto& shape){
        const auto axis=body_guard::math::sub(shape.b,shape.a);
        const double t=std::clamp(body_guard::math::dot(body_guard::math::sub(point,shape.a),axis)/body_guard::math::dot(axis,axis),0.,1.);
        const auto delta=body_guard::math::sub(point,body_guard::math::add(shape.a,body_guard::math::mul(axis,static_cast<float>(t))));
        return std::sqrt(body_guard::math::dot(delta,delta))-shape.radius;
    };
    for(const auto& sample:centers){
        check(capsule_distance(sample.point,collision_envelope::capsules[sample.shape])<-.025f,
            "named joint extrema retain25mm center margin inside their own human capsule");
        double closest=100;
        const unsigned first=sample.shape==5?4:sample.shape==6?5:7,last=sample.shape==6?6:first;
        for(unsigned i=first;i<=last;++i){const auto& guard=collision_envelope::guards[i];
            const auto delta=body_guard::math::sub(sample.point,guard.center);
            closest=std::min(closest,std::sqrt(body_guard::math::dot(delta,delta))-guard.radius);}
        check(closest<-.025f,"named joint extrema have own-region guard coverage rather than another region's union");
    }
    const collision_envelope::Capsule obsolete_head{{0,1.45f,.29f},{0,1.47f,.29f},.16f};
    check(capsule_distance(collision_envelope::Vec{0,1.273285151f,.1128697395f},obsolete_head)>0,
        "negative control exposes the old head location rather than accepting torso union coverage");
    check(collision_envelope::capsules.back().b[1]+collision_envelope::capsules.back().radius>1.6f,
        "inverted chassis now has upper-body coverage");
    std::cout<<"BMX body guard tests passed\n";
}
