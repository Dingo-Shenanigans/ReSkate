#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dingosdk::bmx::native_coupled::policy {
using Vec=std::array<float,3>;
using Quat=std::array<float,4>;
inline constexpr double native_seconds=.01666666567325592;
struct BodyKey {
    std::uintptr_t world{};std::uint32_t index=0xffffffffU,generation{};
    bool operator==(const BodyKey&) const noexcept=default;
};
struct Epoch {
    std::array<std::uintptr_t,8> owner_world{};
    std::array<BodyKey,2> bodies{};
    std::uint64_t token{},serial{};double time{};float seconds{};
    bool operator==(const Epoch&) const noexcept=default;
};
struct RoleConfiguration {float mass{};Vec principal_moments{},com_from_bike_root{};Quat principal_to_model{0,0,0,1};};
inline constexpr std::array<RoleConfiguration,2> v2_roles{{
    {30,{4.67589748f,7.73580036f,8.19740294f},{-.00354105966f,.297910449f,-.00285316830f},
        {-.00815393499f,.700931835f,-.0259105293f,.712710825f}},
    {80,{6,8,6},{-.00628652f,.698707f,.05029697f},{0,0,0,1}}
}};
inline constexpr Quat rider_from_chassis{.00535184265f,-.000196113083f,.00855447283f,.999949067f};
template<std::size_t N> inline bool finite(const std::array<float,N>& value,float bound=1000000) noexcept {
    for(float x:value)if(!std::isfinite(x)||std::abs(x)>bound)return false;
    return true;
}
inline bool unit(Quat input,Quat& output) noexcept {
    if(!finite(input,2))return false;
    double norm{};for(float x:input)norm+=double(x)*x;
    if(!std::isfinite(norm)||std::abs(norm-1)>.001)return false;
    norm=std::sqrt(norm);for(unsigned i=0;i<4;++i)output[i]=static_cast<float>(input[i]/norm);
    return true;
}
inline Quat conjugate(Quat q) noexcept{return {-q[0],-q[1],-q[2],q[3]};}
inline Quat multiply(Quat a,Quat b) noexcept {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
inline Vec rotate(Quat q,Vec v) noexcept {
    const auto r=multiply(multiply(q,{v[0],v[1],v[2],0}),conjugate(q));return {r[0],r[1],r[2]};
}
inline Quat native_orientation(Quat model,Quat principal_to_model) noexcept{return multiply(model,principal_to_model);}
inline Quat model_orientation(Quat native,Quat principal_to_model) noexcept{return multiply(native,conjugate(principal_to_model));}
inline Vec native_local_point(Vec model_point,Quat principal_to_model) noexcept{return rotate(conjugate(principal_to_model),model_point);}
inline bool pose_matches(Vec measured,Quat measured_q,Vec requested,Quat requested_q) noexcept {
    if(!finite(measured)||!finite(requested))return false;
    double distance{};for(unsigned i=0;i<3;++i){const double difference=double(measured[i])-requested[i];distance+=difference*difference;}
    if(distance>.00025*.00025)return false;
    Quat a,b;if(!unit(measured_q,a)||!unit(requested_q,b))return false;
    double product{};for(unsigned i=0;i<4;++i)product+=double(a[i])*b[i];
    const double sign=product<0?-1:1;
    for(unsigned i=0;i<4;++i)if(std::abs(double(a[i])-sign*b[i])>.00001)return false;
    return true;
}
inline bool valid_epoch(const Epoch& e) noexcept {
    for(auto pointer:e.owner_world)if(!pointer)return false;
    if(!e.token||!e.serial||!std::isfinite(e.time)||e.time<0||!std::isfinite(e.seconds)||
       std::abs(double(e.seconds)-native_seconds)>1e-7)return false;
    for(const auto& b:e.bodies)if(b.world!=e.owner_world[5]||b.index==0xffffffffU||b.index>=1048576)return false;
    return e.bodies[0].index!=e.bodies[1].index;
}
struct RoleCommand {Vec force{},torque{},hop_impulse{},angular_impulse{},target_principal_inertia{};};
struct Command {std::array<RoleCommand,2> roles{};std::uint64_t hop_part{};};
struct PreparedCommand {bool valid{},has_hop{};std::array<Vec,2> force{},torque{};unsigned inertia_required{};};
inline double length(const Vec& v) noexcept{return std::hypot(double(v[0]),double(v[1]),double(v[2]));}
inline PreparedCommand prepare(const Command& command,double h) noexcept {
    PreparedCommand out;
    if(!std::isfinite(h)||std::abs(h-native_seconds)>1e-7)return out;
    for(unsigned role=0;role<2;++role){
        const auto& c=command.roles[role];
        if(c.target_principal_inertia!=Vec{}){
            if(!finite(c.target_principal_inertia,150))return out;
            for(unsigned axis=0;axis<3;++axis){const float base=v2_roles[role].principal_moments[axis],target=c.target_principal_inertia[axis];
                if(role==0?std::abs(target-base)>.00001f:target<base-.00001f||target>150)return out;}
            out.inertia_required|=1u<<role;
        }
        if(!finite(c.force)||!finite(c.torque)||!finite(c.hop_impulse)||!finite(c.angular_impulse)||
           length(c.force)>40000||length(c.torque)>4000||length(c.hop_impulse)>600)return out;
        out.has_hop|=length(c.hop_impulse)>0;
        for(unsigned i=0;i<3;++i){
            const double force=double(c.force[i])+c.hop_impulse[i]/h;
            const double torque=double(c.torque[i])+c.angular_impulse[i]/h;
            if(!std::isfinite(force)||!std::isfinite(torque)||std::abs(force)>40000||std::abs(torque)>4000)return out;
            out.force[role][i]=static_cast<float>(force);out.torque[role][i]=static_cast<float>(torque);
        }
        if(length(out.force[role])>40000||length(out.torque[role])>4000)return out;
    }
    if(out.has_hop!=(command.hop_part!=0))return out;
    out.valid=true;return out;
}
enum class ReceiptState {idle,queued,submitted,terminal,cancelled,expired};
struct Receipt {ReceiptState state{ReceiptState::idle};std::uint64_t token{},serial{},hop_part{};unsigned attempted{},completed{};
    Epoch epoch;unsigned inertia_required{},inertia_attempted{},inertia_completed{};
    std::array<Vec,2> verified_principal_inertia{};bool inertia_before_wrenches{};};
// Consume before ANY native call; a failure can never replay the group or hop.
// This is a logical once-only transaction, not physical rollback or a world lock.
class Once {
    std::uint64_t token_{},serial_{},hop_{};bool terminal_{};
    Epoch identity_{};
public:
    void reset(std::uint64_t token) noexcept{token_=token;serial_=hop_=0;terminal_=false;identity_={};}
    bool terminal() const noexcept{return terminal_;}
    template<class Proof,class Apply> Receipt run(const Epoch& epoch,const Command& command,Proof&& proof,Apply&& apply) noexcept {
        return run(epoch,command,proof,[](unsigned,const Vec&,Vec&) noexcept{return false;},apply);
    }
    template<class Proof,class ApplyInertia,class Apply> Receipt run(const Epoch& epoch,const Command& command,Proof&& proof,ApplyInertia&& apply_inertia,Apply&& apply) noexcept {
        Receipt r;r.token=epoch.token;r.serial=epoch.serial;r.hop_part=command.hop_part;r.epoch=epoch;
        const auto prepared=prepare(command,epoch.seconds);
        r.inertia_required=prepared.inertia_required;
        if(terminal_||!valid_epoch(epoch)||epoch.token!=token_||epoch.serial<=serial_||!prepared.valid||
           (prepared.has_hop&&command.hop_part<=hop_))return r;
        if(serial_&&(epoch.owner_world!=identity_.owner_world||epoch.bodies!=identity_.bodies||epoch.time<=identity_.time)){
            terminal_=true;r.state=ReceiptState::terminal;return r;
        }
        identity_=epoch;
        serial_=epoch.serial;if(prepared.has_hop)hop_=command.hop_part;
        for(unsigned role=0;role<2;++role)if(prepared.inertia_required&(1u<<role)){
            if(!proof()){terminal_=true;r.state=ReceiptState::terminal;return r;}
            r.inertia_attempted|=1u<<role;
            Vec verified{};
            if(!apply_inertia(role,command.roles[role].target_principal_inertia,verified)||!finite(verified,151)){
                terminal_=true;r.state=ReceiptState::terminal;return r;}
            for(unsigned axis=0;axis<3;++axis)if(verified[axis]<=0||std::abs(verified[axis]-command.roles[role].target_principal_inertia[axis])>.0001f){
                terminal_=true;r.state=ReceiptState::terminal;return r;}
            r.inertia_completed|=1u<<role;r.verified_principal_inertia[role]=verified;
            if(!proof()){terminal_=true;r.state=ReceiptState::terminal;return r;}
        }
        r.inertia_before_wrenches=true;
        for(unsigned call=0;call<4;++call){
            if(!proof()){terminal_=true;r.state=ReceiptState::terminal;return r;}
            r.attempted|=1u<<call;
            const unsigned role=call/2;const auto& value=(call&1)?prepared.torque[role]:prepared.force[role];
            if(!apply(role,(call&1)!=0,value)){terminal_=true;r.state=ReceiptState::terminal;return r;}
            r.completed|=1u<<call;
        }
        if(!proof()){terminal_=true;r.state=ReceiptState::terminal;return r;}
        r.state=ReceiptState::submitted;return r;
    }
};
}
