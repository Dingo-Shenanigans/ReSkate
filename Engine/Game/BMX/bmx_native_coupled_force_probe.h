#pragma once
#include "bmx_native_coupled_calibration.h"
#include <array>

// One finite small-wrench native diagnostic. This policy integrates no bodies,
// predicts no controller state, raises no force budget and authorizes no ride.
namespace dingosdk::bmx::native_coupled::force_probe {
namespace np=policy;namespace cal=calibration;
using Vec=np::Vec;using Quat=np::Quat;using Sample=cal::Sample;
inline constexpr unsigned command_count=6;
inline constexpr float initial_clearance=.5f;
enum class Phase {idle,running,complete,failed};
struct Report {
    Phase phase{};const char* issue="not_started";bool parked{},bounded_application_candidate{};
    std::array<Sample,command_count+1> actual{};std::array<np::Receipt,command_count> receipts{};
    std::array<np::Command,command_count> commands{};unsigned count{},completed{};
    std::array<double,command_count> linear_response{},angular_response{},expected_linear{},expected_angular{};
    std::array<std::array<double,2>,command_count> quaternion_delta{};
    bool rejected_available{};Sample rejected;unsigned rejected_role=0xffffffffU;
    const char* rejected_check="none";double rejected_value{},rejected_limit{};
};
inline double dot(Vec a,Vec b) noexcept{return cal::dot(a,b);}
inline double quaternion_delta(Quat before,Quat after) noexcept {
    Quat a,b;if(!np::unit(before,a)||!np::unit(after,b))return std::numeric_limits<double>::quiet_NaN();
    // atan2 of the relative vector remains useful for sub-milliradian native
    // changes where acos(dot) can round to zero. q/-q give the same angle.
    const auto d=np::multiply(b,np::conjugate(a));
    return 2*std::atan2(std::hypot(double(d[0]),double(d[1]),double(d[2])),std::abs(double(d[3])));
}
template<class Capsules> inline cal::WorldPlacement placement(const Capsules& capsules,unsigned role,Vec plane,Quat q,Vec up) noexcept {
    auto out=cal::world_placement(capsules,role,plane,q,up);if(!out.valid)return out;
    const float delta=initial_clearance-cal::world_fixture_clearance;out.root=cal::add_scaled(out.root,up,delta);
    out.com=cal::add_scaled(out.com,up,delta);out.translation+=delta;
    const auto actual=cal::proxy_plane_clearance(capsules,role,out.com,out.native_orientation,plane,up);
    out.valid=actual&&std::abs(*actual-initial_clearance)<=.0001f;return out;
}
inline np::Command command(unsigned phase,const Sample& before) noexcept {
    np::Command out;if(phase<1||phase>4)return out;const unsigned role=(phase-1)%2;
    const float sign=phase<=2?1.f:-1.f;out.roles[role].force={sign*(role?160.f:60.f),0,0};
    out.roles[role].torque=np::rotate(before.principal_orientation[role],{0,sign*8,0});return out;
}
inline bool config(const Sample& s) noexcept {
    if(!np::valid_epoch(s.epoch))return false;
    for(unsigned role=0;role<2;++role){Quat q;
        if(!np::finite(s.position[role])||!np::finite(s.linear[role],3)||np::length(s.linear[role])>3||
           !np::finite(s.angular[role],.25f)||np::length(s.angular[role])>.25||
           !std::isfinite(s.clearance[role])||!np::unit(s.principal_orientation[role],q)||
           s.maximum_angular_speed[role]!=15||!s.integrates_angular_accumulator[role]||!std::isfinite(s.mass[role])||
           std::abs(s.mass[role]-np::v2_roles[role].mass)>.0001f)return false;
        for(unsigned axis=0;axis<3;++axis)if(!std::isfinite(s.inertia[role][axis])||
            std::abs(s.inertia[role][axis]-np::v2_roles[role].principal_moments[axis])>.0001f)return false;
    }
    for(auto category:s.categories)if(category!=0)return false;return true;
}
class Monitor {
    Report report_;
    bool rejected(const Sample& actual,unsigned role,const char* check,double value=0,double limit=0) noexcept {
        report_.phase=Phase::failed;report_.issue=check;report_.rejected_available=true;report_.rejected=actual;
        report_.rejected_role=role;report_.rejected_check=check;report_.rejected_value=value;report_.rejected_limit=limit;return false;
    }
public:
    const Report& report() const noexcept{return report_;}
    void fail(const char* issue) noexcept {report_.phase=Phase::failed;report_.issue=issue;}
    void parked(bool yes) noexcept {report_.parked=yes;if(!yes)fail("probe_both_role_park_not_proved");}
    bool begin(const Sample& actual) noexcept {
        if(report_.phase!=Phase::idle||!config(actual))return false;
        for(auto mask:actual.collides_with)if(mask!=0)return false;
        for(unsigned role=0;role<2;++role)if(std::abs(actual.clearance[role]-initial_clearance)>.0005f||
            np::length(actual.linear[role])>.001||np::length(actual.angular[role])>.001)return false;
        report_.actual[0]=actual;report_.count=1;report_.phase=Phase::running;report_.issue="native_small_wrench_probe_running";
        report_.commands[0]=command(0,actual);return true;
    }
    np::Command pending_command() const noexcept{return report_.completed<command_count?report_.commands[report_.completed]:np::Command{};}
    bool accept(const np::Receipt& receipt,const Sample& actual) noexcept {
        if(report_.phase!=Phase::running||report_.completed>=command_count)return false;
        const auto index=report_.completed;const auto& before=report_.actual[index];
        if(!config(actual)||!cal::same_pair(before.epoch,actual.epoch)||before.epoch.serial==std::numeric_limits<std::uint64_t>::max()||
           actual.epoch.serial!=before.epoch.serial+1||actual.epoch.seconds!=before.epoch.seconds||actual.epoch.time!=before.epoch.time+before.epoch.seconds)
            return rejected(actual,0xffffffffU,"probe_config_owner_or_exact_successor_changed");
        if(receipt.state!=np::ReceiptState::submitted||receipt.epoch!=before.epoch||receipt.token!=before.epoch.token||receipt.serial!=before.epoch.serial||
           receipt.hop_part||receipt.attempted!=15||receipt.completed!=15||receipt.inertia_required||receipt.inertia_attempted||receipt.inertia_completed||!receipt.inertia_before_wrenches)
            return rejected(actual,0xffffffffU,"probe_exact_all_four_write_receipt_missing");
        for(auto mask:actual.collides_with)if(mask!=0x101)return rejected(actual,0xffffffffU,"probe_actual_world_masks_changed");
        std::array<double,2> angles{};
        for(unsigned role=0;role<2;++role){
            const auto displacement=np::length(cal::sub(actual.position[role],report_.actual[0].position[role]));
            if(displacement>.15)return rejected(actual,role,"probe_total_displacement_cap",displacement,.15);
            if(actual.clearance[role]<.3f)return rejected(actual,role,"probe_air_clearance_cap",actual.clearance[role],.3);
            angles[role]=quaternion_delta(before.principal_orientation[role],actual.principal_orientation[role]);
            if(!std::isfinite(angles[role])||angles[role]>.02)return rejected(actual,role,"probe_actual_quaternion_step_cap",angles[role],.02);
        }
        double dv{},dw{},expected_v{},expected_w{};
        if(index>=1&&index<=4){const unsigned role=(index-1)%2;const float sign=index<=2?1.f:-1.f;
            const auto axis=np::rotate(before.principal_orientation[role],{0,1,0});
            dv=(actual.linear[role][0]-before.linear[role][0])*sign;
            dw=dot(cal::sub(actual.angular[role],before.angular[role]),axis)*sign;
            expected_v=(role?160.:60.)/before.mass[role]*actual.epoch.seconds;
            expected_w=8./before.inertia[role][1]*actual.epoch.seconds;
            if(dv<.2*expected_v||dv>3*expected_v)return rejected(actual,role,"probe_signed_force_response_outside_small_pulse_window",dv,expected_v);
            if(dw<.2*expected_w||dw>3*expected_w)return rejected(actual,role,"probe_signed_torque_response_outside_small_pulse_window",dw,expected_w);
        }
        report_.linear_response[index]=dv;report_.angular_response[index]=dw;report_.expected_linear[index]=expected_v;report_.expected_angular[index]=expected_w;
        report_.quaternion_delta[index]=angles;report_.receipts[index]=receipt;report_.actual[report_.count++]=actual;++report_.completed;
        if(report_.completed==command_count){report_.phase=Phase::complete;report_.bounded_application_candidate=true;report_.issue="bounded_six_epoch_actual_small_force_torque_application_candidate";}
        else report_.commands[report_.completed]=command(report_.completed,actual);
        return true;
    }
};
}
