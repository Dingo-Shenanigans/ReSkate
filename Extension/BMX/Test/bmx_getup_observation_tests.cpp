#include "Extension/BMX/bmx_getup_observation.h"
#include <cstdlib>
#include <iostream>
#include <limits>
namespace g=dingosdk::bmx::getup_observation;
void require(bool condition,const char* message){if(!condition){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    g::DomainCursor missing_cursor,late_cursor;bool late_found=false;unsigned callbacks=0,total_missing=0;
    for(;callbacks<200&&!late_found;++callbacks){unsigned missing_queries=0,late_queries=0;
        require(!missing_cursor.search(g::populated_domain_budget,[&](std::uint16_t){++missing_queries;return g::DomainResult::miss;}),"An absent first asset stays absent without consuming another asset's quota.");
        late_found=late_cursor.search(g::populated_domain_budget,[&](std::uint16_t domain){++late_queries;return domain==g::domain_count-1?g::DomainResult::found:g::DomainResult::miss;});
        require(missing_queries<=16&&late_queries<=16,"Each callback has a separate bounded quota for both assets.");total_missing+=missing_queries;
    }
    require(late_found&&callbacks==188&&total_missing==3008,"Round-robin progress finds the last possible loaded domain despite the other asset always missing.");
    require(late_cursor.next==0,"The domain cursor wraps at the exact table bound.");
    g::DomainCursor sparse;unsigned visited=0,loaded=0;
    require(sparse.search(1,[&](std::uint16_t domain){++visited;if(domain==3007){++loaded;return g::DomainResult::found;}return g::DomainResult::empty;})&&visited==3008&&loaded==1,
        "Empty slots do not exhaust the lookup quota; one pass remains bounded by the complete domain table.");
    g::Window window;
    require(!window.begin(0,0,0)&&!window.begin(1,std::numeric_limits<double>::quiet_NaN(),0),"No observation may start without a finite native epoch.");
    require(window.begin(34,.5666666667,1000),"A current fall native epoch starts a passive window.");
    require(window.commit(34,.5666666667,1000),"The first current edge is observable.");
    for(unsigned duplicate=0;duplicate<1000;++duplicate)
        require(window.consider(34,.5666666667,1000+duplicate)==g::Decision::wait,"A paused native edge cannot duplicate semantic samples despite elapsed wall time.");
    require(window.consider(35,.5833333334,2000)==g::Decision::wait,"One 60Hz step is below the diagnostic sampling interval.");
    require(window.consider(40,.6666666667,2000)==g::Decision::due&&window.commit(40,.6666666667,2000),"Six native steps permit a new semantic sample.");
    require(window.consider(39,.6833333334,2001)==g::Decision::clock_rejected,"A serial rewind cannot masquerade as the same fall.");
    require(window.consider(41,.65,2001)==g::Decision::clock_rejected,"A native-time rewind cannot be rescued by a fresh serial.");
    require(window.consider(40,.7,2001)==g::Decision::clock_rejected,"One native serial cannot carry two different times.");
    require(window.consider(41,.7,999)==g::Decision::clock_rejected,"A wall-time rewind invalidates the observation epoch.");
    require(window.consider(40,.6666666667,46000)==g::Decision::expired,"Wall timeout closes a paused native session.");
    require(window.consider(2000,30.7,2001)==g::Decision::expired,"Fast native simulation cannot run past its thirty-second bound.");
    require(window.begin(100,2,0),"A separate generation can start a fresh window.");
    for(unsigned step=0;step<=1800;++step){const auto serial=100+step;const double time=2+step/60.;
        if(window.consider(serial,time,step)==g::Decision::due)require(window.commit(serial,time,step),"Every admitted edge commits once.");}
    require(window.samples==301&&window.samples<g::capacity,"The complete thirty-second 60Hz session remains within its preallocated row capacity.");
    require(window.consider(1901,32.0166666667,1801)==g::Decision::expired,"The next native step cannot extend the old window.");
    g::BoolObservation unknown;unknown.guid=g::allowed_guid;
    require(!unknown.getter_available&&!unknown.binding_present_known,"An unmeasured default false is explicitly unavailable, never a proven false gate.");
    unknown.getter_available=true;unknown.value=false;
    require(!unknown.binding_present_known&&!unknown.binding_present,"A successful getter returning false does not prove a native binding slot exists.");
    require(g::allowed_guid!=g::getup_guid,"The readiness state and GetUp intent retain distinct exact asset identities.");
    std::cout<<"Passive get-up sampling epoch, bounds and unknown-value semantics passed.\n";
}
