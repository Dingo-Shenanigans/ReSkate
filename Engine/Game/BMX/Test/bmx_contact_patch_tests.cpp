#include "Engine/Game/BMX/bmx_contact_patch.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace p=dingosdk::bmx::contact_patch;
void check(bool value,const char* why) {if(!value){std::cerr<<why<<'\n';std::exit(1);}}
float distance(p::Vec a,p::Vec b) {float s=0;for(unsigned i=0;i<3;++i)s+=(a[i]-b[i])*(a[i]-b[i]);return std::sqrt(s);}
std::array<p::Sample,5> plane(float along,float across,float span=.1f) {
    return {{{true,{0,0,0}},{true,{0,along*span,span}},{true,{0,-along*span,-span}},
        {true,{span,across*span,0}},{true,{-span,-across*span,0}}}};
}
int main() {
    const p::Vec up{0,1,0},forward{0,0,1},side{1,0,0};
    for(float along:{-1.f,-.4f,0.f,.4f,1.f})for(float across:{-.4f,0.f,.4f}) {
        auto samples=plane(along,across);const auto r=p::fit(samples,up,forward,side);
        const float divisor=std::sqrt(1+along*along+across*across);
        const p::Vec expected{-across/divisor,1/divisor,-along/divisor};
        check(r.available&&distance(r.normal,expected)<1e-6,"measured planes recover the analytic normal");
        samples[1].hit=false;const auto lip=p::fit(samples,up,forward,side);
        check(lip.available&&distance(lip.normal,expected)<1e-6,"forward ray leaving a lip does not discard center support");
        samples=plane(along,across);samples[2].hit=samples[3].hit=false;
        const auto edge=p::fit(samples,up,forward,side);
        check(edge.available&&distance(edge.normal,expected)<1e-6,"rear or side edge is symmetric with a forward edge");
        samples[0].hit=false;check(!p::fit(samples,up,forward,side).available,"auxiliary hits never bridge a missing center hit");
    }
    auto curve=plane(0,0);curve[1].point[1]=curve[2].point[1]=.005f;
    const auto centered=p::fit(curve,up,forward,side);
    check(centered.available&&distance(centered.normal,up)<1e-6,"symmetric curved samples have no one-sided slope bias");
    auto step=plane(0,0);step[1].point[1]=.4f;
    check(p::fit(step,up,forward,side).available,"a remote step is discarded without losing the actual center plane");
    for(unsigned i=1;i<5;++i)step[i].point[1]=-.4f;
    check(!p::fit(step,up,forward,side).available,"unrelated lower floors cannot manufacture support");
    auto kink=plane(0,0);kink[1].point[1]=.1f;
    check(!p::fit(kink,up,forward,side).available,"a broad sharp seam requests a smaller patch");
    kink=plane(0,0,.03f);kink[1].point[1]=.03f;
    check(p::fit(kink,up,forward,side).available,"a local measured patch can resolve a seam without a height snap");
    auto bad=plane(0,0);bad[2].point[0]=std::numeric_limits<float>::quiet_NaN();
    check(!p::fit(bad,up,forward,side).available,"nonfinite positions cannot create a normal");
    check(!p::fit(curve,{0,2,0},forward,side).available,"invalid caller axes are rejected");
    std::cout<<"BMX measured contact-patch planes, lips, edges and curvature tests passed.\n";
}
