#include "Engine/Game/BMX/bmx_automated_ride.h"
#include <cstdlib>
#include <iostream>

int main() {
    using dingosdk::bmx::automated_ride::configuration_selected;
    const auto check=[](bool good,const char* text){if(!good){std::cerr<<text<<'\n';std::exit(1);}};
    check(configuration_selected(true,L"bmx-user-data-chassis-probe",L"1"),"exact private rehearsal configuration selected");
    check(!configuration_selected(false,L"bmx-user-data-chassis-probe",L"1"),"ordinary data root never disables manual BMX input");
    for(const auto profile:{L"",L"bmx-user-data",L"bmx-user-data-working-profile",L"bmx-user-data-ramp-test",
        L"bmx-user-data-chassis-probe-extra",L"BMX-USER-DATA-CHASSIS-PROBE"})
        check(!configuration_selected(true,profile,L"1"),"profile selection is exact and does not affect ordinary Ride or Switch profiles");
    for(const auto value:{L"",L"0",L"true",L"11",L"1 ",L" 1"})
        check(!configuration_selected(true,L"bmx-user-data-chassis-probe",value),"only explicit exact environment value1 selects the test gate");
    std::cout<<"BMX automated ride private-profile configuration tests passed.\n";
}
