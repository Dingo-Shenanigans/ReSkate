#include "Extension/Skater/FirstPerson/Upstream/src/look_smoothing.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
using namespace first_person_vendor::first_person;
using jishsfirstpersonmod::LookSmoothing;
#define check(value) do { if(!(value)) { std::fprintf(stderr,"Failed line %d\n",__LINE__); std::exit(1); } } while(0)
int main() {
    Matrix pose{1,0,0,7,0,1,0,8,0,0,1,9,3,4,5,1};
    Settings settings; settings.enabled=true;
    LookSmoothing smoothing;
    check(smoothing.apply(pose,settings,{},0)==pose);
    auto held=smoothing.apply(pose,settings,{0,90},0.016);
    check(held[0]>0.99f && held[0]<1);
    check(held[12]==3 && held[13]==4 && held[14]==5 && held[3]==7 && held[7]==8 && held[11]==9);
    check(smoothing.apply(pose,settings,{0,90},0.016)==held);
    for(int i=2;i<=400;++i) held=smoothing.apply(pose,settings,{0,90},i*.016);
    check(std::abs(held[0])<.001f);
    auto released=smoothing.apply(pose,settings,{},401*.016);
    check(released[0]<.01f);
    for(int i=402;i<=800;++i) released=smoothing.apply(pose,settings,{},i*.016);
    check(released[0]>.999f);
    settings.enabled=false;
    check(smoothing.apply(pose,settings,{0,90},13)==target(pose,{}, {0,90}));
    check(smoothing.apply(pose,settings,{},13.016)==pose);
    settings.enabled=true; settings.up=settings.down=settings.left=settings.right=0;
    auto instant=smoothing.apply(pose,settings,{30,90},14); auto expected=target(pose,{}, {30,90}); for(unsigned i=0;i<16;++i) check(std::abs(instant[i]-expected[i])<.00001f);
    settings.up=settings.down=settings.left=settings.right=50;
    smoothing.reset();
    auto seeded=smoothing.apply(pose,settings,{0,90},15); for(unsigned i=0;i<16;++i) check(std::abs(seeded[i]-pose[i])<.00001f);
    auto first=smoothing.apply(pose,settings,{0,90},15.016);
    auto moved=pose; moved[12]=100;
    auto second=smoothing.apply(moved,settings,{0,90},15.016);
    check(second[0]==first[0] && second[12]==100);
}
