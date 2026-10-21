#include <bmx/suspension.hpp>
#include <bmx/observed_trick_timing.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace bmx {
namespace {
namespace m=detail::source::tire::detail;
struct Row {Vec3 chassis_linear{},chassis_angular{},rider_linear{},rider_angular{};};
Quat conjugate(Quat q) noexcept{return {-q[0],-q[1],-q[2],q[3]};}
Quat multiply(Quat a,Quat b) noexcept {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
bool unit_quaternion(Quat q) noexcept {
    double square=0;for(double value:q){if(!std::isfinite(value))return false;square+=value*value;}
    return std::abs(square-1)<=1e-5;
}
Quat normalized(Quat q) noexcept {double square=0;for(auto value:q)square+=value*value;for(auto& value:q)value/=std::sqrt(square);return q;}
Quat increment(Vec3 omega,double seconds) noexcept {
    const double speed=std::sqrt(m::dot(omega,omega));
    if(speed<1e-12)return normalized({omega[0]*seconds*.5,omega[1]*seconds*.5,omega[2]*seconds*.5,1});
    const double scale=std::sin(speed*seconds*.5)/speed;
    return {omega[0]*scale,omega[1]*scale,omega[2]*scale,std::cos(speed*seconds*.5)};
}
Vec3 tensor(const Body& body,Vec3 omega) noexcept {
    const auto principal=m::inertia_rotation(body);auto local=rotate(conjugate(principal),omega);
    for(unsigned i=0;i<3;++i)local[i]*=body.inertia_body[i];
    return rotate(principal,local);
}
double kinetic(const Body& b) noexcept {
    return .5*b.mass*m::dot(b.linear_velocity,b.linear_velocity)+.5*m::dot(b.angular_velocity,tensor(b,b.angular_velocity));
}
Body predict(const Body& body,Vec3 linear_impulse,Vec3 angular_impulse,double seconds,double angular_speed_limit=0) noexcept {
    auto next=body;next.linear_velocity=m::add(body.linear_velocity,m::mul(linear_impulse,1/body.mass));
    next.center_of_mass=m::add(body.center_of_mass,m::mul(next.linear_velocity,seconds));
    auto momentum=m::add(tensor(body,body.angular_velocity),angular_impulse);
    if(angular_speed_limit>0){const auto omega=m::inverse_inertia(body,momentum);const double size=std::sqrt(m::dot(omega,omega));
        if(size>angular_speed_limit)momentum=m::mul(momentum,angular_speed_limit/size);}
    auto middle_omega=m::inverse_inertia(body,momentum);
    // Bounded midpoint orientation solve transports the world angular momentum
    // rather than adding angular velocity in a changing principal-axis frame.
    for(unsigned i=0;i<8;++i){
        Body middle=body;middle.orientation=normalized(multiply(increment(middle_omega,seconds*.5),body.orientation));
        middle_omega=m::inverse_inertia(middle,momentum);
    }
    next.orientation=normalized(multiply(increment(middle_omega,seconds),body.orientation));
    next.angular_velocity=m::inverse_inertia(next,momentum);return next;
}
Vec3 angular_error(Quat chassis,Quat rider,const SuspensionSettings& settings) noexcept {
    const auto base=multiply(chassis,settings.chassis_frame),passenger=multiply(rider,settings.rider_frame);
    auto error=multiply(multiply(conjugate(base),passenger),conjugate(settings.target_rotation));
    if(error[3]<0)for(auto& value:error)value=-value;
    const double sine=std::hypot(error[0],error[1],error[2]);
    const double scale=sine<1e-10?2:2*std::atan2(sine,std::clamp(error[3],0.,1.))/sine;
    return {error[0]*scale,error[1]*scale,error[2]*scale};
}
std::array<double,7> coordinates(const Body& chassis,const Body& rider,const SuspensionSettings& settings) noexcept {
    const auto base=multiply(chassis.orientation,settings.chassis_frame);
    const auto pa=m::add(chassis.center_of_mass,rotate(chassis.orientation,settings.chassis_anchor));
    const auto pb=m::add(rider.center_of_mass,rotate(rider.orientation,settings.rider_anchor));
    const auto position=rotate(conjugate(base),m::sub(pb,pa));
    const auto angle=angular_error(chassis.orientation,rider.orientation,settings);
    return {position[0],position[1],position[2],angle[0],angle[1],angle[2],std::sqrt(m::dot(position,position))};
}
bool valid_drive(const SuspensionDrive& drive) noexcept {
    return std::isfinite(drive.spring)&&drive.spring>=0&&drive.spring<=1e8&&
        std::isfinite(drive.damper)&&drive.damper>=0&&drive.damper<=1e7&&
        std::isfinite(drive.maximum_effort)&&drive.maximum_effort>=0&&drive.maximum_effort<=1e6&&
        (drive.mode==SuspensionDriveMode::force||drive.mode==SuspensionDriveMode::acceleration);
}
double huber(double displacement,double spring,double bound) noexcept {
    if(spring==0||bound==0)return 0;
    const double distance=std::abs(displacement),transition=bound/spring;
    return distance<=transition?.5*spring*distance*distance:bound*(distance-.5*transition);
}
const SuspensionDrive& drive_at(const SuspensionSettings& s,unsigned i) noexcept {
    static constexpr SuspensionDrive no_drive{};return i<3?s.linear[i]:i<6?s.angular[i-3]:no_drive;
}
const SuspensionLimit& limit_at(const SuspensionSettings& s,unsigned i) noexcept{return i<3?s.linear_limits[i]:i<6?s.angular_limits[i-3]:s.linear_radial_limit;}
double target_velocity(const SuspensionSettings& s,unsigned i) noexcept{return i<3?s.target_linear_velocity[i]:i<6?s.target_angular_velocity[i-3]:0;}
double conversion(const SuspensionDrive& drive,double inverse_mass) noexcept{return drive.mode==SuspensionDriveMode::acceleration?1/inverse_mass:1;}
double potential(const std::array<double,7>& x,const SuspensionSettings& s,const std::array<double,7>& w) noexcept {
    double energy=0;
    for(unsigned i=0;i<7;++i){
        const auto& drive=drive_at(s,i);const double target=i<3?s.target_position[i]:0;
        energy+=huber(x[i]-target,drive.spring*conversion(drive,w[i]),drive.maximum_effort);
        const auto& limit=limit_at(s,i);
        if(limit.enabled)energy+=huber(x[i]-std::clamp(x[i],limit.minimum,limit.maximum),
            limit.drive.spring*conversion(limit.drive,w[i]),limit.drive.maximum_effort);
    }
    return energy;
}
double velocity(const Row& row,const Body& chassis,const Body& rider) noexcept {
    return m::dot(row.chassis_linear,chassis.linear_velocity)+m::dot(row.chassis_angular,chassis.angular_velocity)+
        m::dot(row.rider_linear,rider.linear_velocity)+m::dot(row.rider_angular,rider.angular_velocity);
}
double response(const Row& a,const Row& b,const Body& chassis,const Body& rider) noexcept {
    return m::dot(a.chassis_linear,b.chassis_linear)/chassis.mass+m::dot(a.rider_linear,b.rider_linear)/rider.mass+
        m::dot(a.chassis_angular,m::inverse_inertia(chassis,b.chassis_angular))+
        m::dot(a.rider_angular,m::inverse_inertia(rider,b.rider_angular));
}
}

bool valid_suspension(const SuspensionSettings& s) noexcept {
    if(!m::finite(s.chassis_anchor,5)||!m::finite(s.rider_anchor,5)||!m::finite(s.target_position,5)||
       !m::finite(s.target_linear_velocity,20)||!m::finite(s.target_angular_velocity,40)||
       !unit_quaternion(s.chassis_frame)||!unit_quaternion(s.rider_frame)||!unit_quaternion(s.target_rotation)||
       !std::isfinite(s.maximum_linear_force)||s.maximum_linear_force<=0||s.maximum_linear_force>1e6||
       !std::isfinite(s.maximum_angular_torque)||s.maximum_angular_torque<=0||s.maximum_angular_torque>1e6||
       s.iterations<1||s.iterations>128)return false;
    if(s.acceleration_reference.enabled){
        auto chassis=detail::source::chassis_control::proposed_body(),rider=chassis;
        chassis.mass=s.acceleration_reference.chassis_mass;chassis.inertia_body=s.acceleration_reference.chassis_inertia;
        chassis.inertia_orientation=s.acceleration_reference.chassis_inertia_orientation;
        rider.mass=s.acceleration_reference.rider_mass;rider.inertia_body=s.acceleration_reference.rider_inertia;
        rider.inertia_orientation=s.acceleration_reference.rider_inertia_orientation;
        if(!detail::source::tire::valid(chassis)||!detail::source::tire::valid(rider)||
           !m::finite(s.acceleration_reference.chassis_com_offset,5)||!m::finite(s.acceleration_reference.rider_com_offset,5))return false;
    }
    for(unsigned i=0;i<7;++i){
        if(!valid_drive(drive_at(s,i)))return false;
        const auto& limit=limit_at(s,i);
        const double bound=i<3||i==6?5:std::numbers::pi;
        if(!valid_drive(limit.drive)||!std::isfinite(limit.minimum)||!std::isfinite(limit.maximum)||
           limit.minimum>limit.maximum||limit.minimum < -bound||limit.maximum>bound)return false;
    }
    return true;
}
SuspensionResult solve_suspension(double seconds,const Body& chassis,const Body& rider,const SuspensionSettings& settings) noexcept {
    SuspensionResult out;
    if(!std::isfinite(seconds)||seconds<minimum_fixed_step||seconds>maximum_fixed_step){out.issue=SuspensionIssue::invalid_step;return out;}
    if(!valid_suspension(settings)){out.issue=SuspensionIssue::invalid_settings;return out;}
    if(!detail::source::tire::valid(chassis)||!detail::source::tire::valid(rider)||
       !unit_quaternion(chassis.orientation)||!unit_quaternion(rider.orientation)||
       !m::finite(m::sub(rider.center_of_mass,chassis.center_of_mass),10))return out;
    const auto x=coordinates(chassis,rider,settings);
    if(std::hypot(x[3],x[4],x[5])>std::numbers::pi-1e-5){out.issue=SuspensionIssue::ambiguous_rotation;return out;}
    out.chassis_anchor=m::add(chassis.center_of_mass,rotate(chassis.orientation,settings.chassis_anchor));
    out.rider_anchor=m::add(rider.center_of_mass,rotate(rider.orientation,settings.rider_anchor));
    std::array<Row,7> rows;const auto frame=multiply(chassis.orientation,settings.chassis_frame);
    for(unsigned i=0;i<3;++i){
        Vec3 local{};local[i]=1;const auto axis=rotate(frame,local);
        // Reaction is evaluated at the same measured rider anchor. This is the
        // exact derivative of a chassis-frame position coordinate, including
        // its rotating axes, and conserves total world angular momentum.
        rows[i]={m::mul(axis,-1),m::mul(m::cross(m::sub(out.rider_anchor,chassis.center_of_mass),axis),-1),
            axis,m::cross(m::sub(out.rider_anchor,rider.center_of_mass),axis)};
    }
    constexpr double epsilon=1e-6;
    for(unsigned world_axis=0;world_axis<3;++world_axis){
        Vec3 direction{};direction[world_axis]=1;
        const auto high=angular_error(chassis.orientation,multiply(increment(direction,epsilon),rider.orientation),settings);
        const auto low=angular_error(chassis.orientation,multiply(increment(direction,-epsilon),rider.orientation),settings);
        for(unsigned i=0;i<3;++i){
            const double derivative=(high[i]-low[i])/(2*epsilon);
            rows[i+3].rider_angular[world_axis]=derivative;rows[i+3].chassis_angular[world_axis]=-derivative;
        }
    }
    // A radial limit is a central anchor spring, distinct from an axis box.
    // Its Jacobian is the projection of the three exact translation rows.
    for(unsigned i=0;i<3;++i){
        const double projection=x[6]>1e-9?x[i]/x[6]:(i==1?1:0);
        rows[6].chassis_linear=m::add(rows[6].chassis_linear,m::mul(rows[i].chassis_linear,projection));
        rows[6].chassis_angular=m::add(rows[6].chassis_angular,m::mul(rows[i].chassis_angular,projection));
        rows[6].rider_linear=m::add(rows[6].rider_linear,m::mul(rows[i].rider_linear,projection));
        rows[6].rider_angular=m::add(rows[6].rider_angular,m::mul(rows[i].rider_angular,projection));
    }
    std::array<std::array<double,7>,7> matrix{};std::array<double,7> inverse_mass{},speed{};
    for(unsigned i=0;i<7;++i){
        speed[i]=velocity(rows[i],chassis,rider);
        for(unsigned j=0;j<7;++j)matrix[i][j]=response(rows[i],rows[j],chassis,rider);
        inverse_mass[i]=matrix[i][i];if(!std::isfinite(inverse_mass[i])||inverse_mass[i]<=1e-12)return out;
        if(i<3){out.displacement[i]=x[i];out.linear_velocity[i]=speed[i];}
        else if(i<6){out.rotation[i-3]=x[i];out.angular_velocity[i-3]=speed[i];}
        const auto& limit=limit_at(settings,i);
        if(limit.enabled)out.limit_violation=std::max(out.limit_violation,std::abs(x[i]-std::clamp(x[i],limit.minimum,limit.maximum)));
    }
    // The physical response matrix always uses the measured bodies. Only gain
    // conversion can opt into a separately identified source-body reference.
    if(settings.acceleration_reference.enabled){
        auto reference_chassis=chassis,reference_rider=rider;const auto& reference=settings.acceleration_reference;
        reference_chassis.mass=reference.chassis_mass;reference_chassis.inertia_body=reference.chassis_inertia;
        reference_chassis.inertia_orientation=reference.chassis_inertia_orientation;
        reference_rider.mass=reference.rider_mass;reference_rider.inertia_body=reference.rider_inertia;
        reference_rider.inertia_orientation=reference.rider_inertia_orientation;
        reference_chassis.center_of_mass=m::add(chassis.center_of_mass,rotate(chassis.orientation,reference.chassis_com_offset));
        reference_rider.center_of_mass=m::add(rider.center_of_mass,rotate(rider.orientation,reference.rider_com_offset));
        auto reference_rows=rows;
        for(unsigned i=0;i<3;++i){
            reference_rows[i].chassis_angular=m::mul(m::cross(m::sub(out.rider_anchor,reference_chassis.center_of_mass),rows[i].rider_linear),-1);
            reference_rows[i].rider_angular=m::cross(m::sub(out.rider_anchor,reference_rider.center_of_mass),rows[i].rider_linear);
        }
        reference_rows[6].chassis_angular=m::mul(m::cross(m::sub(out.rider_anchor,reference_chassis.center_of_mass),rows[6].rider_linear),-1);
        reference_rows[6].rider_angular=m::cross(m::sub(out.rider_anchor,reference_rider.center_of_mass),rows[6].rider_linear);
        for(unsigned i=0;i<7;++i){
            inverse_mass[i]=response(reference_rows[i],reference_rows[i],reference_chassis,reference_rider);
            if(!std::isfinite(inverse_mass[i])||inverse_mass[i]<=1e-12)return out;
        }
    }
    std::array<double,7> impulse{};
    if(settings.unilateral_soft_limits){
        std::array<double,7> drive_impulse{},lower_impulse{},upper_impulse{};
        std::array<double,7> diagonal{},right{},bound{},stop_diagonal{},stop_bound{},lower_right{},upper_right{};
        for(unsigned i=0;i<7;++i){
            const auto& drive=drive_at(settings,i);const double factor=conversion(drive,inverse_mass[i]);
            const double spring=drive.spring*factor,damper=drive.damper*factor;
            const double implicit=seconds*damper+seconds*seconds*spring;
            if(implicit>0){
                diagonal[i]=matrix[i][i]+1/implicit;
                right[i]=-speed[i]+seconds*damper*target_velocity(settings,i)/implicit-
                    seconds*spring*(x[i]-(i<3?settings.target_position[i]:0))/implicit;
                bound[i]=seconds*drive.maximum_effort;
            }
            const auto& stop=limit_at(settings,i);if(!stop.enabled)continue;
            const double stop_factor=conversion(stop.drive,inverse_mass[i]);
            const double stiffness=stop.drive.spring*stop_factor,damping=stop.drive.damper*stop_factor;
            const double compliance=seconds*damping+seconds*seconds*stiffness;
            if(compliance<=0)continue;
            stop_diagonal[i]=matrix[i][i]+1/compliance;stop_bound[i]=seconds*stop.drive.maximum_effort;
            const double erp=seconds*seconds*stiffness/compliance;
            // Within the interval use the boundary-reaching velocity, so a
            // newly touching stop has zero impulse continuously. Outside use
            // the ordinary implicit spring bias, with a unilateral sign that
            // never attracts a body already escaping the stop.
            const double lower_gap=stop.minimum-x[i],upper_gap=stop.maximum-x[i];
            lower_right[i]=(lower_gap>0?erp:1)*lower_gap/seconds-speed[i];
            upper_right[i]=(upper_gap<0?erp:1)*upper_gap/seconds-speed[i];
        }
        for(unsigned iteration=0;iteration<settings.iterations;++iteration)for(unsigned i=0;i<7;++i){
            const auto solve_row=[&](double rhs,double divisor,double own){
                for(unsigned j=0;j<7;++j)rhs-=matrix[i][j]*impulse[j];
                return (rhs+matrix[i][i]*own)/divisor;
            };
            if(diagonal[i]>0){
                const double wanted=solve_row(right[i],diagonal[i],drive_impulse[i]);
                const double value=std::clamp(wanted,-bound[i],bound[i]);
                impulse[i]+=value-drive_impulse[i];drive_impulse[i]=value;
                out.saturated=out.saturated||std::abs(wanted)>bound[i]+1e-12;
            }
            if(stop_diagonal[i]>0){
                const double lower_wanted=solve_row(lower_right[i],stop_diagonal[i],lower_impulse[i]);
                const double lower=std::clamp(lower_wanted,0.,stop_bound[i]);
                impulse[i]+=lower-lower_impulse[i];lower_impulse[i]=lower;
                const double upper_wanted=solve_row(upper_right[i],stop_diagonal[i],upper_impulse[i]);
                const double upper=std::clamp(upper_wanted,-stop_bound[i],0.);
                impulse[i]+=upper-upper_impulse[i];upper_impulse[i]=upper;
                out.saturated=out.saturated||lower_wanted>stop_bound[i]+1e-12||upper_wanted < -stop_bound[i]-1e-12;
            }
        }
    }else{
    for(unsigned active_pass=0;active_pass<4;++active_pass){
        std::array<double,7> diagonal{},right{},bound{};
        for(unsigned i=0;i<7;++i){
            const auto& drive=drive_at(settings,i);const double factor=conversion(drive,inverse_mass[i]);
            double spring=drive.spring*factor,damper=drive.damper*factor;
            double gradient=spring*(x[i]-(i<3?settings.target_position[i]:0));
            double maximum=(drive.spring>0||drive.damper>0)?drive.maximum_effort:0;
            double predicted_speed=speed[i];for(unsigned j=0;j<7;++j)predicted_speed+=matrix[i][j]*impulse[j];
            const auto& limit=limit_at(settings,i);const double predicted=x[i]+seconds*predicted_speed;
            if(limit.enabled&&(predicted<limit.minimum||predicted>limit.maximum||limit.minimum==limit.maximum)){
                const double stop=std::clamp(predicted,limit.minimum,limit.maximum);
                const double stop_factor=conversion(limit.drive,inverse_mass[i]);
                const double stop_spring=limit.drive.spring*stop_factor;
                spring+=stop_spring;damper+=limit.drive.damper*stop_factor;gradient+=stop_spring*(x[i]-stop);
                maximum+=limit.drive.maximum_effort;
            }
            const double implicit=seconds*damper+seconds*seconds*spring;
            if(implicit<=0){impulse[i]=0;continue;}
            const double target_speed=target_velocity(settings,i);
            diagonal[i]=matrix[i][i]+1/implicit;
            right[i]=-speed[i]+seconds*damper*target_speed/implicit-seconds*gradient/implicit;
            bound[i]=seconds*maximum;
        }
        for(unsigned iteration=0;iteration<settings.iterations;++iteration)for(unsigned i=0;i<7;++i){
            if(diagonal[i]==0)continue;
            double rhs=right[i];for(unsigned j=0;j<7;++j)if(j!=i)rhs-=matrix[i][j]*impulse[j];
            const double wanted=rhs/diagonal[i];impulse[i]=std::clamp(wanted,-bound[i],bound[i]);
            out.saturated=out.saturated||std::abs(wanted)>bound[i]+1e-12;
        }
    }
    }
    const auto impulses=[&](const std::array<double,7>& lambda,Vec3& chassis_linear,Vec3& chassis_angular,Vec3& rider_linear,Vec3& rider_angular){
        chassis_linear={};chassis_angular={};rider_linear={};rider_angular={};
        for(unsigned i=0;i<7;++i){
            chassis_linear=m::add(chassis_linear,m::mul(rows[i].chassis_linear,lambda[i]));
            chassis_angular=m::add(chassis_angular,m::mul(rows[i].chassis_angular,lambda[i]));
            rider_linear=m::add(rider_linear,m::mul(rows[i].rider_linear,lambda[i]));
            rider_angular=m::add(rider_angular,m::mul(rows[i].rider_angular,lambda[i]));
        }
    };
    Vec3 ca,ct,ra,rt;impulses(impulse,ca,ct,ra,rt);
    Vec3 angular_drive{};for(unsigned i=3;i<6;++i)angular_drive=m::add(angular_drive,m::mul(rows[i].rider_angular,impulse[i]));
    const double linear_size=std::sqrt(m::dot(ra,ra)),angular_size=std::sqrt(m::dot(angular_drive,angular_drive));
    const double fraction=std::min({1.,settings.maximum_linear_force*seconds/std::max(linear_size,1e-12),
        settings.maximum_angular_torque*seconds/std::max(angular_size,1e-12)});
    if(fraction<1){for(auto& value:impulse)value*=fraction;out.saturated=true;}
    out.energy_before=kinetic(chassis)+kinetic(rider)+potential(x,settings,inverse_mass);
    const auto original=impulse;bool accepted=false;
    for(unsigned attempt=0;attempt<12;++attempt){
        const double scale=std::ldexp(1.,-static_cast<int>(attempt));
        for(unsigned i=0;i<7;++i)impulse[i]=original[i]*scale;
        impulses(impulse,ca,ct,ra,rt);
        const auto next_chassis=predict(chassis,ca,ct,seconds),next_rider=predict(rider,ra,rt,seconds);
        if(!detail::source::tire::valid(next_chassis)||!detail::source::tire::valid(next_rider))continue;
        const auto next_x=coordinates(next_chassis,next_rider,settings);
        out.predicted_energy=kinetic(next_chassis)+kinetic(next_rider)+potential(next_x,settings,inverse_mass);
        out.actuator_work=0;
        for(unsigned i=0;i<7;++i)out.actuator_work+=std::max(0.,impulse[i]*target_velocity(settings,i));
        if(out.predicted_energy<=out.energy_before+out.actuator_work+1e-7*std::max(1.,out.energy_before)){
            accepted=true;out.energy_limited=attempt>0;break;
        }
    }
    if(!accepted){out.issue=SuspensionIssue::energy_increase;return out;}
    out.chassis_force=m::mul(ca,1/seconds);out.chassis_torque=m::mul(ct,1/seconds);
    out.rider_force=m::mul(ra,1/seconds);out.rider_torque=m::mul(rt,1/seconds);out.impulse=impulse;
    out.dissipated_energy=std::max(0.,out.energy_before+out.actuator_work-out.predicted_energy);
    out.valid=true;out.issue=SuspensionIssue::none;return out;
}
const char* suspension_issue_name(SuspensionIssue issue) noexcept {
    switch(issue){case SuspensionIssue::none:return "none";case SuspensionIssue::invalid_step:return "invalid step";
    case SuspensionIssue::invalid_settings:return "invalid settings";case SuspensionIssue::invalid_body:return "invalid body";
    case SuspensionIssue::ambiguous_rotation:return "ambiguous rotation";case SuspensionIssue::energy_increase:return "energy increase";}
    return "unknown";
}

namespace {
CoupledProfile coupled_profile(CoupledProfile profile) noexcept {
    profile.movement.simulation.predict_hop_parts=true;return profile;
}
bool valid_coupled(const CoupledProfile& profile) noexcept {
    const auto& c=profile.coupled;auto rider=detail::source::chassis_control::proposed_body();
    for(double value:{c.manual_pitch.manny_angle_degrees,c.manual_pitch.nosey_angle_degrees,
        c.manual_pitch.ground_lerp_rate,c.manual_pitch.air_lerp_rate})
        if(!std::isfinite(value)||value<0||value>180)return false;
    if(!std::isfinite(c.manual_pitch.direct_torque)||c.manual_pitch.direct_torque<0||c.manual_pitch.direct_torque>20000)return false;
    rider.mass=c.rider_mass;rider.inertia_body=c.rider_inertia;rider.inertia_orientation=c.rider_inertia_orientation;
    if(!valid_suspension(c.suspension)||!valid_attitude_pid(c.rider_stabilization)||!valid_attitude_pid(c.chassis_stabilization)||
       !detail::source::tire::valid(rider)||!m::finite(c.chassis_com_from_root,5)||
       !m::finite(c.observed_chassis_com_from_root,5)||!m::finite(c.rider_initial_com_from_root,5)||
       !m::finite(c.rider_neutral_com_from_root,5)||!unit_quaternion(c.rider_orientation_from_chassis)||
       !m::finite(c.posture_translation_gain,2)||c.coupling_iterations<1||c.coupling_iterations>96||
       !std::isfinite(c.coupling_tolerance)||c.coupling_tolerance<=0||c.coupling_tolerance>10)return false;
    for(double value:{c.rider_hop_multiplier,c.rider_linear_drag,c.rider_angular_drag,c.chassis_angular_drag,
        c.air_angular_drag,
        c.chassis_body_angular_speed_limit,c.rider_body_angular_speed_limit,
        c.rider_air_yaw_acceleration,c.rider_air_pitch_acceleration,c.rider_maximum_angular_speed})
        if(!std::isfinite(value)||value<0||value>100)return false;
    if(c.actuation.enabled){
        for(const auto& values:{c.actuation.ground_inertia,c.actuation.air_inertia})
            for(double value:values)if(!std::isfinite(value)||value<=0||value>1e6)return false;
        for(double value:{c.actuation.inertia_rate,c.actuation.ground_limit,c.actuation.air_limit,c.actuation.down_damper,
            c.actuation.up_damper,c.actuation.falling_max_damper,c.actuation.up_velocity_threshold,c.actuation.ground_damper_rate,
            c.actuation.falling_damper_rate,c.actuation.drive_lerp_rate})
            if(!std::isfinite(value)||value<0||value>1e6)return false;
        if(c.actuation.ground_limit>10||c.actuation.air_limit>10||c.actuation.down_damper>c.actuation.falling_max_damper)return false;
    }
    return c.rider_hop_multiplier<=4&&c.rider_maximum_angular_speed>0;
}
Vec3 damping_torque(const Body& body,double rate,double seconds) noexcept {
    return m::mul(tensor(body,body.angular_velocity),-rate/(1+rate*seconds));
}
Body kicked(Body body,Vec3 force,Vec3 torque,Vec3 impulse,Vec3 angular_impulse,Vec3 gravity,double seconds) noexcept {
    body.linear_velocity=m::add(body.linear_velocity,m::add(m::mul(gravity,seconds),
        m::mul(m::add(m::mul(force,seconds),impulse),1/body.mass)));
    body.angular_velocity=m::add(body.angular_velocity,m::inverse_inertia(body,m::add(m::mul(torque,seconds),angular_impulse)));
    return body;
}
void presentation(CoupledOutput& out,const Body& chassis,const Body& rider,const CoupledProfile& profile) noexcept {
    const auto& c=profile.coupled;out.bike.body=chassis;out.rider=rider;
    out.bike.bike_root=m::sub(chassis.center_of_mass,rotate(chassis.orientation,c.chassis_com_from_root));
    out.bike.bike_com_from_root=c.chassis_com_from_root;
    out.observed_chassis_com=m::add(out.bike.bike_root,rotate(chassis.orientation,c.observed_chassis_com_from_root));
    out.rider_relative_com=rotate(conjugate(chassis.orientation),m::sub(rider.center_of_mass,out.observed_chassis_com));
    const auto neutral=m::add(out.bike.bike_root,rotate(chassis.orientation,c.rider_neutral_com_from_root));
    auto offset=rotate(conjugate(chassis.orientation),m::sub(rider.center_of_mass,neutral));
    for(unsigned i=0;i<3;++i)offset[i]*=c.posture_translation_gain[i];
    out.posture_frame.center_of_mass=m::add(neutral,rotate(chassis.orientation,offset));
    out.posture_frame.orientation=c.posture_follows_rider_rotation?rider.orientation:multiply(chassis.orientation,c.rider_orientation_from_chassis);
    out.posture_frame.neutral_center_from_bike_root=c.rider_neutral_com_from_root;
    out.posture_frame.neutral_orientation_from_bike=c.rider_orientation_from_chassis;
    out.bike.rider=rider_pose(out.bike,profile.movement.rider,&out.posture_frame);
}
}
CoupledMovement::CoupledMovement(CoupledProfile profile) noexcept:profile_(coupled_profile(profile)),
    movement_(profile_.movement.tuning,profile_.movement.simulation,profile_.movement.rider){reset_actuation();}
void CoupledMovement::reset_actuation() noexcept {
    actuation_track_=nullptr;actuation_seconds_=air_seconds_=0;actuation_weight_=1;since_hop_=1e6;
    wanted_y_damper_=current_y_damper_=profile_.coupled.suspension.linear[1].damper;
    current_radial_limit_=profile_.coupled.suspension.linear_radial_limit.maximum;
    actuation_identity_known_=false;actuation_owner_=0;actuation_device_=0;
    approach_velocity_={};approach_reserved_=approach_air_seen_=false;
    manual_pitch_angle_=manual_ground_seconds_=manny_input_seconds_=0;
    actuation_phase_offset_={};actuation_phase_rotation_={0,0,0,1};actuation_command_known_=false;
}
void CoupledMovement::reset() noexcept{movement_.reset();reset_actuation();rider_stabilizer_.reset();chassis_stabilizer_.reset();}
void CoupledMovement::invalidate_sample() noexcept{movement_.invalidate_sample();reset_actuation();rider_stabilizer_.reset();chassis_stabilizer_.reset();}
CoupledOutput CoupledMovement::step(const RawInput& raw,const Body& chassis,const Body& rider,
    const std::array<ContactSample,2>& contacts,std::uint64_t identity,const CoupledTargets& targets) noexcept {
    CoupledOutput out;out.input_contacts=contacts;presentation(out,chassis,rider,profile_);
    if(!valid_coupled(profile_)||!detail::source::tire::valid(chassis)||!detail::source::tire::valid(rider)||!identity){invalidate_sample();return out;}
    const auto& c=profile_.coupled;const double seconds=fixed_seconds();
    auto joint=targets.suspension_available?targets.suspension:c.suspension;
    Body controlled_rider=rider;
    if(targets.rider_inertia_available){controlled_rider.inertia_body=targets.rider_inertia;controlled_rider.inertia_orientation=targets.rider_inertia_orientation;}
    const auto rig_velocity=m::mul(m::add(m::mul(chassis.linear_velocity,chassis.mass),m::mul(rider.linear_velocity,rider.mass)),1/(chassis.mass+rider.mass));
    TrickRotationObservation rider_rotation{true,controlled_rider.orientation,true,approach_reserved_?approach_velocity_:rig_velocity};
    Output intent;
    {
        auto intent_controller=movement_;intent=intent_controller.step(raw,chassis,contacts,identity,{},rider_rotation);
        if(!intent.valid){invalidate_sample();out.bike=intent;return out;}
    }
    if(c.actuation.enabled||c.manual_pitch.enabled){
        if(!actuation_identity_known_||identity!=actuation_owner_||raw.device!=actuation_device_){
            reset_actuation();actuation_identity_known_=true;actuation_owner_=identity;actuation_device_=raw.device;
        }
    }
    if(c.actuation.enabled&&!targets.suspension_available){
        // Obtain the same accepted input event from a disposable controller
        // copy. Physical/contact iteration below still commits it exactly once.
        if(!intent.actions.active){actuation_track_=nullptr;actuation_seconds_=0;}
        else {
            if(actuation_track_)actuation_seconds_+=seconds;
            auto begin=[&](std::uint16_t id,JointActuationPhase phase){
                if(const auto* track=find_joint_actuation(id,phase);track&&valid_joint_actuation(*track)){
                    actuation_phase_offset_={};actuation_phase_rotation_={0,0,0,1};
                    if(id==actuation_preload&&phase!=JointActuationPhase::enter&&actuation_command_known_){
                        const auto first=sample_joint_actuation(*track,0).command;
                        const auto first_position=m::add(c.suspension.target_position,m::sub(first.target_position,track->reference.target_position));
                        const auto first_rotation=multiply(multiply(first.target_rotation,conjugate(track->reference.target_rotation)),c.suspension.target_rotation);
                        actuation_phase_offset_=m::sub(last_actuation_target_,first_position);
                        actuation_phase_rotation_=multiply(last_actuation_rotation_,conjugate(first_rotation));
                        if(actuation_phase_rotation_[3]<0)for(auto& value:actuation_phase_rotation_)value=-value;
                    }
                    actuation_track_=track;actuation_seconds_=0;return true;}return false;
            };
            if(intent.hop_request_accepted){
                const bool full_preload=actuation_track_&&actuation_track_->id==actuation_preload;
                if(intent.actions.hop_kind!=HopKind::ollie||!full_preload||!begin(actuation_preload,JointActuationPhase::transition))
                    begin(actuation_quick_hop,JointActuationPhase::timeline);
            }else if(intent.grounded&&intent.actions.flick_preloading&&
                     (!actuation_track_||(actuation_track_->id==actuation_preload&&actuation_track_->phase==JointActuationPhase::recover)))
                begin(actuation_preload,JointActuationPhase::enter);
            else if(actuation_track_&&actuation_track_->id==actuation_preload&&
                    actuation_track_->phase==JointActuationPhase::enter&&!intent.actions.flick_preloading){
                if(!begin(actuation_preload,JointActuationPhase::recover)){actuation_track_=nullptr;actuation_seconds_=0;}
            }
            const auto selected=intent.actions.movement;
            const bool confirmed=selected!=TrickId::none&&!intent.grounded&&
                (intent.tricks.movement_id==selected||
                 (catalog::legacy_rotation(selected)&&(intent.tricks.bar_rotating||intent.tricks.tail_rotating)));
            if(confirmed){const auto id=static_cast<std::uint16_t>(selected);
                if(!begin(id,JointActuationPhase::enter))begin(id,JointActuationPhase::timeline);}
            if(actuation_track_&&actuation_track_->id<80&&actuation_track_->phase==JointActuationPhase::enter&&intent.tricks.movement_recovering)
                begin(actuation_track_->id,JointActuationPhase::recover);
            // Pump observations contain separately identified driven phases.
            // Endpoint holding is allowed only while the matching input is held.
            const double pump_input=std::round(intent.actions.front_weight*127)/127;
            const std::uint16_t pump=pump_input<-.1?actuation_pump_negative:pump_input>.1?actuation_pump_positive:0;
            const auto pump_track=[](std::uint16_t id){return id==actuation_pump_negative||id==actuation_pump_positive;};
            if(intent.grounded&&(!actuation_track_||pump_track(actuation_track_->id))){
                if(pump&&(!actuation_track_||actuation_track_->id!=pump)){
                    const bool transition=actuation_track_&&actuation_track_->id==actuation_pump_negative&&pump==actuation_pump_positive;
                    if(!(transition&&begin(pump,JointActuationPhase::transition))&&
                       !begin(pump,JointActuationPhase::enter)&&!begin(pump,JointActuationPhase::timeline)){
                        actuation_track_=nullptr;actuation_seconds_=0;}
                }else if(!pump&&actuation_track_&&pump_track(actuation_track_->id)&&actuation_track_->phase!=JointActuationPhase::recover){
                    if(!begin(actuation_track_->id,JointActuationPhase::recover)){actuation_track_=nullptr;actuation_seconds_=0;}
                }
            }
            if(actuation_track_&&actuation_seconds_>actuation_track_->keys.back().seconds+seconds&&
               actuation_track_->phase!=JointActuationPhase::enter&&
               !(actuation_track_->phase==JointActuationPhase::transition&&pump_track(actuation_track_->id))){
                actuation_track_=nullptr;actuation_seconds_=0;}
        }
        since_hop_+=seconds;
        if(m::dot(intent.hop_impulse,intent.hop_impulse)>0)since_hop_=0;
        air_seconds_=intent.grounded?0:air_seconds_+seconds;
        const bool air=air_seconds_>0||since_hop_<.35;
        const auto& policy=c.actuation;
        // The native consumer uses VECTOR MoveTowards, not one independent
        // component step and not an exponential interpolation.
        if(!targets.rider_inertia_available){const auto target=air?policy.air_inertia:policy.ground_inertia;
            const auto delta=m::sub(target,rider.inertia_body);const double distance=std::sqrt(m::dot(delta,delta));
            controlled_rider.inertia_body=distance<=policy.inertia_rate*seconds?target:
                m::add(rider.inertia_body,m::mul(delta,policy.inertia_rate*seconds/distance));}
        if(m::dot(rider.linear_velocity,rotate(rider.orientation,{0,1,0}))>policy.up_velocity_threshold)wanted_y_damper_=policy.up_damper;
        if(intent.grounded)wanted_y_damper_=std::max(policy.down_damper,wanted_y_damper_-seconds*policy.ground_damper_rate);
        else if(rider.linear_velocity[1]<0)wanted_y_damper_=std::min(policy.falling_max_damper,wanted_y_damper_+seconds*policy.falling_damper_rate);
        const double blend=std::clamp(seconds*policy.drive_lerp_rate,0.,1.);
        current_y_damper_+=(wanted_y_damper_-current_y_damper_)*blend;
        double wanted_limit=air?policy.air_limit:policy.ground_limit;
        if(air){const Vec3 source_target{joint.target_position[0],-joint.target_position[1],-joint.target_position[2]};
            for(double component:source_target)if(component>policy.air_limit)current_radial_limit_=wanted_limit=std::abs(component)+.5;}
        current_radial_limit_+=(wanted_limit-current_radial_limit_)*blend;
        joint.linear[1].damper=current_y_damper_;joint.linear_radial_limit.maximum=current_radial_limit_;
        if(actuation_track_){const auto sample=sample_joint_actuation(*actuation_track_,actuation_seconds_);
            if(sample.available){const auto& command=sample.command;const auto& reference=actuation_track_->reference;
                // Rebase each attempt's measured neutral. A trick track replaces
                // the overlapping hop track; the two animations are never added.
                double weight=1;if(actuation_track_->id==actuation_pump_negative||actuation_track_->id==actuation_pump_positive){
                    if(actuation_track_->phase!=JointActuationPhase::recover)
                        actuation_weight_=std::clamp(std::abs(std::round(intent.actions.front_weight*127)/127)/actuation_track_->request_amplitude,0.,1.);
                    weight=actuation_weight_;}
                joint.target_position=m::add(c.suspension.target_position,m::mul(m::sub(command.target_position,reference.target_position),weight));
                auto delta=multiply(command.target_rotation,conjugate(reference.target_rotation));if(delta[3]<0)for(auto& value:delta)value=-value;
                for(unsigned i=0;i<3;++i)delta[i]*=weight;
                delta[3]=1+(delta[3]-1)*weight;
                joint.target_rotation=multiply(normalized(delta),c.suspension.target_rotation);
                if(actuation_track_->id==actuation_preload&&actuation_track_->phase!=JointActuationPhase::enter){
                    // Independent command retarget: join separately observed
                    // phases at the preceding actual target, then remove only
                    // that boundary offset by this measured phase's endpoint.
                    const double duration=actuation_track_->keys.back().seconds;
                    const double remaining=duration>0?1-std::clamp(actuation_seconds_/duration,0.,1.):0;
                    joint.target_position=m::add(joint.target_position,m::mul(actuation_phase_offset_,remaining));
                    auto rotation=actuation_phase_rotation_;for(unsigned i=0;i<3;++i)rotation[i]*=remaining;
                    rotation[3]=1+(rotation[3]-1)*remaining;
                    joint.target_rotation=multiply(normalized(rotation),joint.target_rotation);
                }
                last_actuation_target_=joint.target_position;last_actuation_rotation_=joint.target_rotation;actuation_command_known_=true;
                for(unsigned axis=0;axis<3;++axis){joint.linear[axis].spring=command.linear_spring[axis];joint.linear[axis].damper=command.linear_damper[axis];
                    joint.angular[axis].spring=command.angular_spring[axis];joint.angular[axis].damper=command.angular_damper[axis];
                    joint.linear[axis].mode=(command.acceleration_mask&(1u<<axis))?SuspensionDriveMode::acceleration:SuspensionDriveMode::force;
                    joint.angular[axis].mode=(command.acceleration_mask&(1u<<(axis+3)))?SuspensionDriveMode::acceleration:SuspensionDriveMode::force;}
                joint.linear_radial_limit.maximum=command.radial_limit;
                // Playback supplies the actual current native drive state.
                // Continue its stateful ground/air relaxation when a track
                // ends, rather than jumping back to a hidden stale baseline.
                current_radial_limit_=command.radial_limit;
                current_y_damper_=wanted_y_damper_=command.linear_damper[1];
                if(!targets.rider_inertia_available){controlled_rider.inertia_body=command.rider_inertia;controlled_rider.inertia_orientation=command.rider_inertia_orientation;}
                out.joint_actuation_available=true;out.joint_actuation_id=actuation_track_->id;out.joint_actuation_seconds=actuation_seconds_;
            }
        }
    }
    // Alignment is relative to the measured approach momentum, rather than
    // internal chassis recoil or tiny contact drift created by the trick.
    // A stationary launch has no travel direction; full rotation/catch and real
    // contact quality remain independently required by the trick controller.
    if(!approach_reserved_)approach_velocity_=rig_velocity;
    if(intent.actions.hop_pressed||!intent.grounded)approach_reserved_=true;
    if(!intent.grounded)approach_air_seen_=true;
    rider_rotation.travel_velocity=approach_velocity_;
    if(joint.acceleration_reference.enabled){
        // The rider role is not folded into another body in this reduction;
        // its observed inertia (including native ground/air changes) is exact.
        joint.acceleration_reference.rider_mass=controlled_rider.mass;
        joint.acceleration_reference.rider_inertia=controlled_rider.inertia_body;
        joint.acceleration_reference.rider_inertia_orientation=controlled_rider.inertia_orientation;
    }
    out.applied_suspension=joint;
    auto reaction=solve_suspension(seconds,chassis,controlled_rider,joint);
    if(!reaction.valid){invalidate_sample();out.suspension=reaction;return out;}
    const auto baseline=movement_;
    Vec3 wanted_normal{};
    for(const auto& contact:contacts)if(detail::source::chassis_control::eligible_support(chassis,contact))
        wanted_normal=m::add(wanted_normal,contact.balance_normal_available?contact.balance_normal:contact.contact.normal);
    if(!m::unit(wanted_normal,wanted_normal)&&!m::unit(m::mul(profile_.movement.tuning.gravity,-1),wanted_normal))wanted_normal={0,1,0};
    auto rider_pid=c.rider_stabilization,chassis_pid=c.chassis_stabilization;
    if(c.manual_pitch.enabled){
        const double manny=intent.actions.active?intent.actions.manual:0;
        const double nosey=intent.actions.active?intent.actions.nose_manual:0;
        if(!intent.actions.active)manual_pitch_angle_=manny_input_seconds_=0;
        manual_ground_seconds_=intent.grounded?manual_ground_seconds_+seconds:0;
        manny_input_seconds_=manny>.05?manny_input_seconds_+seconds:0;
        const bool direct_manny=manny>.05&&c.manual_pitch.manny_direct_torque&&manny_input_seconds_>.25&&manual_ground_seconds_>.25;
        const bool direct_nosey=nosey>.05&&c.manual_pitch.nosey_direct_torque&&manual_ground_seconds_>.25;
        const bool direct=direct_manny||direct_nosey;
        const auto source_angle=[](double input,double angle){
            const double t=std::clamp(2*(std::abs(input-.2)-.2),0.,1.);
            return angle*(.4+.6*t);
        };
        double wanted=manny>.05?-source_angle(manny,c.manual_pitch.manny_angle_degrees):
            nosey>.05?source_angle(nosey,c.manual_pitch.nosey_angle_degrees):0;
        if(nosey>.05&&!intent.grounded)wanted+=7;
        if(direct)wanted=0;
        if(since_hop_<.15)wanted=0;
        manual_pitch_angle_+=(wanted-manual_pitch_angle_)*std::clamp(seconds*(intent.grounded?
            c.manual_pitch.ground_lerp_rate:c.manual_pitch.air_lerp_rate),0.,1.);
        rider_pid.pitch_setpoint_degrees=chassis_pid.pitch_setpoint_degrees=manual_pitch_angle_;
        if(intent.grounded&&std::abs(manny+nosey)>.25){
            rider_pid.ground_pitch_control=direct?.06:1;
            if(c.manual_pitch.chassis_pid){chassis_pid.enabled=true;chassis_pid.ground_pitch_control=direct?.06:1;}
        }
        const bool moving_forward=m::dot(chassis.linear_velocity,rotate(chassis.orientation,{0,0,1}))>=0;
        double chassis_pitch{},rider_pitch{};
        if(direct_manny){
            chassis_pitch=-c.manual_pitch.direct_torque*manny*(moving_forward?.9:1.3);
            rider_pitch=-c.manual_pitch.direct_torque*manny*(moving_forward?1:1.4);
        }else if(direct_nosey){
            chassis_pitch=c.manual_pitch.direct_torque*nosey*(moving_forward?1.38:1.08);
            rider_pitch=c.manual_pitch.direct_torque*nosey*(moving_forward?1.5:1.2);
        }
        out.manual_chassis_torque=rotate(chassis.orientation,{chassis_pitch,0,0});
        out.manual_rider_torque=rotate(rider.orientation,{rider_pitch,0,0});
        out.manual_pitch_setpoint_degrees=manual_pitch_angle_;
    }
    if(c.actuation.enabled&&!intent.grounded){
        const double flip=std::clamp(std::abs(intent.actions.air_pitch),0.,1.);
        const double upright=std::max(.02,std::clamp(1-flip+.25,0.,1.));
        rider_pid.air_pitch_control*=1-flip;chassis_pid.air_pitch_control*=1-flip;
        rider_pid.force_multiplier*=upright;chassis_pid.force_multiplier*=upright;
    }
    out.rider_stabilization=rider_stabilizer_.step(seconds,controlled_rider,wanted_normal,intent.grounded,rider_pid);
    out.chassis_stabilization=chassis_stabilizer_.step(seconds,chassis,wanted_normal,intent.grounded,chassis_pid);
    if(!out.rider_stabilization.valid||!out.chassis_stabilization.valid){invalidate_sample();return out;}
    const double chassis_drag=c.use_air_angular_drag&&!intent.grounded?c.air_angular_drag:c.chassis_angular_drag;
    const double rider_drag_rate=c.use_air_angular_drag&&!intent.grounded?c.air_angular_drag:c.rider_angular_drag;
    const auto base_drag=m::add(m::add(damping_torque(chassis,chassis_drag,seconds),out.chassis_stabilization.torque),out.manual_chassis_torque);
    const auto rider_drag=m::add(m::add(damping_torque(controlled_rider,rider_drag_rate,seconds),out.rider_stabilization.torque),out.manual_rider_torque);
    const auto rider_drag_force=m::mul(rider.linear_velocity,-rider.mass*c.rider_linear_drag/(1+c.rider_linear_drag*seconds));
    const auto gravity=profile_.movement.tuning.gravity;
    const auto gravity_compensation=c.chassis_gravity?Vec3{}:m::mul(gravity,-chassis.mass);
    Movement accepted=baseline;
    const auto run_control=[&](const SuspensionResult& spring,Movement& trial){
        trial=baseline;
        auto result=trial.step(raw,chassis,contacts,identity,
            {m::add(spring.chassis_force,gravity_compensation),m::add(spring.chassis_torque,base_drag)},rider_rotation);
        result.force=m::sub(result.force,gravity_compensation);return result;
    };
    const auto rider_actuation=[&](const Output& bike){
        Vec3 delta{};
        if(bike.actions.active&&!bike.grounded){
            Vec3 up;if(m::unit(m::mul(gravity,-1),up))delta=m::mul(up,-bike.actions.steer*c.rider_air_yaw_acceleration*seconds);
            delta=m::add(delta,rotate(chassis.orientation,{bike.actions.air_pitch*c.rider_air_pitch_acceleration*seconds,0,0}));
        }
        if(m::dot(delta,delta)==0)return Vec3{};
        auto wanted=m::add(rider.angular_velocity,delta);const double speed=std::sqrt(m::dot(wanted,wanted));
        if(speed>c.rider_maximum_angular_speed)wanted=m::mul(wanted,c.rider_maximum_angular_speed/speed);
        return tensor(controlled_rider,m::sub(wanted,rider.angular_velocity));
    };
    const auto evaluate=[&](const SuspensionResult& seed,Movement& trial,Output& bike,SuspensionResult& next){
        bike=run_control(seed,trial);++out.coupling_iterations;
        if(!bike.valid)return false;
        const auto rider_hop=m::mul(bike.hop_velocity_change,rider.mass*c.rider_hop_multiplier);
        const auto rider_angular=rider_actuation(bike);
        const auto pre_chassis=kicked(chassis,m::sub(bike.force,seed.chassis_force),m::sub(bike.torque,seed.chassis_torque),
            bike.hop_impulse,{},c.chassis_gravity?gravity:Vec3{},seconds);
        const auto pre_rider=kicked(controlled_rider,rider_drag_force,rider_drag,rider_hop,rider_angular,c.rider_gravity?gravity:Vec3{},seconds);
        next=solve_suspension(seconds,pre_chassis,pre_rider,joint);return next.valid;
    };
    using Wrench=std::array<double,6>;
    const auto components=[](const SuspensionResult& value){return Wrench{value.chassis_force[0],value.chassis_force[1],value.chassis_force[2],
        value.chassis_torque[0],value.chassis_torque[1],value.chassis_torque[2]};};
    const auto difference=[&](const SuspensionResult& a,const SuspensionResult& b){auto result=components(a),other=components(b);
        for(unsigned i=0;i<6;++i)result[i]-=other[i];
        return result;};
    const auto norm=[](const Wrench& value){double linear=0,angular=0;for(unsigned i=0;i<3;++i){linear+=value[i]*value[i];angular+=value[i+3]*value[i+3];}
        return std::sqrt(std::max(linear,angular));};
    const auto with_components=[&](SuspensionResult value,const Wrench& wrench){
        for(unsigned i=0;i<3;++i){value.chassis_force[i]=wrench[i];value.chassis_torque[i]=wrench[i+3];}
        // Every disposable seed conserves momentum about the unchanged COMs.
        value.rider_force=m::mul(value.chassis_force,-1);
        value.rider_torque=m::add(m::mul(value.chassis_torque,-1),m::cross(m::sub(controlled_rider.center_of_mass,chassis.center_of_mass),value.chassis_force));
        return value;
    };
    Output evaluated_bike;SuspensionResult next;
    for(unsigned iteration=0;iteration<c.coupling_iterations;++iteration){
        if(!evaluate(reaction,accepted,evaluated_bike,next)){
            movement_=accepted;invalidate_sample();out.bike=evaluated_bike;out.suspension=next;return out;
        }
        out.coupling_residual=norm(difference(next,reaction));
        // reaction is already an admitted suspension result. Certify the
        // exact applied point, rather than replacing it by the untested F(x).
        if(out.coupling_residual<=c.coupling_tolerance){out.converged=true;break;}
        reaction=next;
    }
    if(!out.converged){
        // Contact active sets can make unrelaxed Picard alternate between two
        // wrenches. A bounded six-dimensional Newton fallback solves the SAME
        // residual and keeps every trial's controller/history disposable.
        auto seed=reaction;Movement scratch=baseline;Output bike;SuspensionResult mapped;
        const auto certify=[&](){
            if(norm(difference(mapped,seed))>c.coupling_tolerance)return false;
            // Raw Newton seeds have no energy admission. Certify an actual
            // admitted solve at the exact point ultimately applied.
            auto projected=mapped;Movement certified=baseline;Output certified_bike;SuspensionResult certified_next;
            if(!evaluate(projected,certified,certified_bike,certified_next))return false;
            const double checked=norm(difference(certified_next,projected));
            if(checked>c.coupling_tolerance)return false;
            reaction=projected;accepted=certified;evaluated_bike=certified_bike;
            out.coupling_residual=checked;out.converged=true;return true;
        };
        if(evaluate(seed,scratch,bike,mapped))for(unsigned attempt=0;attempt<12&&!out.converged;++attempt){
            auto residual=difference(mapped,seed);const double current=norm(residual);
            if(certify())break;
            const auto x=components(seed);double matrix[6][7]{};bool jacobian_valid=true;
            for(unsigned column=0;column<6&&jacobian_valid;++column){
                double delta=std::max(.01,std::abs(x[column])*1e-5);auto displaced=x;displaced[column]+=delta;
                Movement probe=baseline;Output probe_bike;SuspensionResult probe_next;auto probe_seed=with_components(seed,displaced);
                if(!evaluate(probe_seed,probe,probe_bike,probe_next)){
                    delta=-delta;displaced=x;displaced[column]+=delta;probe_seed=with_components(seed,displaced);
                    if(!evaluate(probe_seed,probe,probe_bike,probe_next)){jacobian_valid=false;break;}
                }
                const auto r=difference(probe_next,probe_seed);for(unsigned row=0;row<6;++row)matrix[row][column]=(r[row]-residual[row])/delta;
            }
            if(!jacobian_valid)break;
            for(unsigned row=0;row<6;++row)matrix[row][6]=-residual[row];
            bool invertible=true;
            for(unsigned column=0;column<6;++column){
                unsigned pivot=column;for(unsigned row=column+1;row<6;++row)if(std::abs(matrix[row][column])>std::abs(matrix[pivot][column]))pivot=row;
                if(!std::isfinite(matrix[pivot][column])||std::abs(matrix[pivot][column])<1e-9){invertible=false;break;}
                for(unsigned j=column;j<7;++j)std::swap(matrix[column][j],matrix[pivot][j]);
                const double divisor=matrix[column][column];for(unsigned j=column;j<7;++j)matrix[column][j]/=divisor;
                for(unsigned row=0;row<6;++row)if(row!=column){const double factor=matrix[row][column];
                    for(unsigned j=column;j<7;++j)matrix[row][j]-=factor*matrix[column][j];}
            }
            if(!invertible)break;
            bool improved=false;
            for(unsigned line=0;line<12;++line){
                const double scale=std::ldexp(1.,-static_cast<int>(line));auto candidate=x;bool finite=true;
                for(unsigned i=0;i<6;++i){candidate[i]+=scale*matrix[i][6];finite=finite&&std::isfinite(candidate[i]);}
                if(!finite)continue;
                auto trial_seed=with_components(seed,candidate);Movement trial=baseline;Output trial_bike;SuspensionResult trial_next;
                if(evaluate(trial_seed,trial,trial_bike,trial_next)&&norm(difference(trial_next,trial_seed))<current){
                    seed=trial_seed;mapped=trial_next;improved=true;break;
                }
            }
            if(!improved)break;
        }
        // The last allowed line-search step receives the same certification.
        if(!out.converged&&mapped.valid)certify();
    }
    if(!out.converged){
        SuspensionResult final_map;
        if(!evaluate(reaction,accepted,evaluated_bike,final_map)){
            invalidate_sample();out.bike=evaluated_bike;out.suspension=final_map;return out;
        }
        out.coupling_residual=norm(difference(final_map,reaction));
    }
    out.bike=evaluated_bike;
    if(!out.bike.valid){movement_=accepted;invalidate_sample();return out;}
    movement_=accepted;out.suspension=reaction;
    if(out.bike.events.landed&&approach_air_seen_)approach_reserved_=approach_air_seen_=false;
    out.bike.physical_trick_rotation=out.joint_actuation_available&&out.joint_actuation_id<80;
    out.rider_force=m::add(reaction.rider_force,rider_drag_force);out.rider_torque=m::add(reaction.rider_torque,rider_drag);
    out.rider_hop_impulse=m::mul(out.bike.hop_velocity_change,rider.mass*c.rider_hop_multiplier);
    out.rider_angular_impulse=rider_actuation(out.bike);out.valid=true;
    presentation(out,chassis,controlled_rider,profile_);return out;
}
CoupledWorld::CoupledWorld(CoupledProfile profile) noexcept:movement_(profile){reset();}
CoupledProfile streets_observed_v2() noexcept {
    CoupledProfile p;p.movement=streets_observed_v1();auto& s=p.movement.simulation;auto& c=p.coupled;
    // Seven selected non-Spring roles form this explicit rigid reduction. Their
    // full parallel-axis tensor is retained in principal axes; avatar/hair are
    // not counted. See reference/selected-rig-reduction-v2.json for source hash.
    s.mass=30;s.inertia_body={4.675897481766363,7.735800355946515,8.197402936563929};
    s.inertia_orientation={-.008153934985667407,.7009318349198123,-.02591052927366069,.7127108253793774};
    s.predict_hop_parts=true;s.hop_impulse_multiplier=4./3;p.movement.tuning.hop_delta_speed=6.4;
    s.swept_plane_normals=true;
    // Input-to-first-pulse latency includes the observed controller/animation
    // pipeline. This .11 s fit is distinct from the native .07 s coroutine wait.
    s.hop_delay_seconds=.11;p.movement.tuning.torque_limit=20000;
    s.input_settings.observed_balance_holdpoints=true;s.input_settings.deadzone=.125;s.input_settings.maximum_magnitude=.925;
    s.input_settings.observed_flick_hop=true;
    s.input_settings.observed_flick_preload_threshold=.6;s.input_settings.observed_flick_forward_threshold=.45;
    s.input_settings.observed_flick_seconds=.2; // current authored values consumed by native Jhop
    s.input_settings.observed_flick_strength=4.5/6.4; // directly observed RS flick force, separate from full-strength R3
    s.observed_nollie_support_policy=true;
    // Native AddForce applies index0 then enumerates all eight selected roles.
    // Chassis10 therefore receives twice; folding the other20kg into this30kg
    // base gives(2*10+20)/30, while the separate Spring receives one pulse.
    apply_streets_observed_trick_timing(s.trick_settings);
    p.movement.tuning.air_yaw_torque=p.movement.tuning.air_pitch_torque=0;
    p.movement.tuning.neutral_air_damping_rate=0;
    c.chassis_com_from_root={-.0035410596600473155,.2979104485111192,-.0028531682992827116};
    c.rider_initial_com_from_root=c.rider_neutral_com_from_root={-.00628652,.698707,.05029697};
    c.rider_orientation_from_chassis={.00535184265,-.000196113083,.00855447283,.999949067};
    c.rider_angular_drag=c.chassis_angular_drag=1;
    c.use_air_angular_drag=true;c.air_angular_drag=2.75;
    c.coupling_iterations=64; // numerical budget; force caps and convergence tolerance are unchanged
    c.actuation.enabled=true;
    // RunPID applies external default-Force body-local torques to Spring and
    // Chassis. Fresh neutral air uses caller pitch control.2, ground neutral0;
    // flip input attenuates that control and the whole upright multiplier.
    c.rider_stabilization.enabled=true;c.chassis_stabilization.enabled=false;
    c.manual_pitch.enabled=true;
    p.movement.tuning.manual_pitch=0;
    p.movement.tuning.manual_pitch_balance=false;
    c.rider_stabilization.air_pitch_control=c.chassis_stabilization.air_pitch_control=.2;
    c.rider_air_yaw_acceleration=28;c.rider_air_pitch_acceleration=28*1.0367503;
    c.chassis_body_angular_speed_limit=c.rider_body_angular_speed_limit=15;
    auto& j=c.suspension;
    j.unilateral_soft_limits=true;
    j.chassis_anchor=m::sub({0,.35,0},c.chassis_com_from_root);j.rider_anchor={0,-.45,0};
    // Unity's connected-body target has the opposite translation/rotation
    // convention to this rider-minus-chassis coordinate; reflect X once.
    j.target_position={-.0002830745652318001,.0019056200981140137,.04454246163368225};
    j.target_rotation={0,0,.01044384203851223,.9999454617500305};
    j.linear={SuspensionDrive{100000,10000,1000000,SuspensionDriveMode::acceleration},
        SuspensionDrive{1100,140,1000000,SuspensionDriveMode::acceleration},
        SuspensionDrive{10000,1500,1000000,SuspensionDriveMode::acceleration}};
    for(auto& drive:j.angular)drive={6000,600,1000000,SuspensionDriveMode::acceleration};
    // Native limited axes use a hard radial constraint. This reduced reference
    // host uses a bounded dissipative radial stop; its force law is explicit.
    j.linear_radial_limit={true,0,.3,{1000000,5000,150000}};
    auto& reference=j.acceleration_reference;reference.enabled=true;
    reference.chassis_mass=10;reference.chassis_inertia={1,1,1};
    reference.rider_mass=80;reference.rider_inertia={6,8,6};
    reference.chassis_com_offset=m::sub({0,.3,0},c.chassis_com_from_root);
    return p;
}
void CoupledWorld::reset(Vec3 root,double yaw) noexcept {
    movement_.reset();const auto& p=profile();const auto& c=p.coupled;
    chassis_=detail::source::chassis_control::proposed_body();chassis_.mass=p.movement.simulation.mass;
    chassis_.inertia_body=p.movement.simulation.inertia_body;chassis_.inertia_orientation=p.movement.simulation.inertia_orientation;
    chassis_.orientation={0,std::sin(yaw*.5),0,std::cos(yaw*.5)};
    chassis_.center_of_mass=m::add(root,rotate(chassis_.orientation,c.chassis_com_from_root));
    rider_={};rider_.mass=c.rider_mass;rider_.inertia_body=c.rider_inertia;rider_.inertia_orientation=c.rider_inertia_orientation;
    rider_.orientation=multiply(chassis_.orientation,c.rider_orientation_from_chassis);
    rider_.center_of_mass=m::add(root,rotate(chassis_.orientation,c.rider_initial_com_from_root));
    output_={};output_.valid=valid_coupled(p)&&detail::source::tire::valid(chassis_)&&detail::source::tire::valid(rider_);
    output_.bike.valid=output_.valid;output_.bike.grounded=true;presentation(output_,chassis_,rider_,p);
}
CoupledOutput CoupledWorld::step(const RawInput& raw,const CoupledTargets& targets) noexcept {
    const auto& p=profile();const auto& c=p.coupled;
    auto observed_rider=rider_;
    if(targets.rider_inertia_available){observed_rider.inertia_body=targets.rider_inertia;observed_rider.inertia_orientation=targets.rider_inertia_orientation;}
    auto measured=output_.bike;measured.body=chassis_;
    std::array<ContactSample,2> contacts;
    for(unsigned i=0;i<2;++i){
        const auto hub=wheel_hub(measured,i==0);auto& sample=contacts[i];
        sample.separation=hub[1]-p.movement.simulation.wheels[i].radius;
        sample.contact.status=sample.separation<=p.movement.tuning.contact_gap?ContactStatus::hit:ContactStatus::clear;
        sample.contact.point={hub[0],0,hub[2]};sample.contact.normal={0,1,0};sample.contact.forward=rotate(chassis_.orientation,{0,0,1});
        if(std::hypot(sample.contact.forward[0],sample.contact.forward[2])<1e-8)sample.contact.forward={0,0,1};
        sample.balance_normal_available=true;sample.balance_normal={0,1,0};sample.verified_plane=true;
    }
    auto trial=movement_;auto next=trial.step(raw,chassis_,observed_rider,contacts,1,targets);
    if(next.valid){
        const double seconds=fixed_seconds();
        const auto chassis_impulse=m::add(m::mul(next.bike.force,seconds),m::add(next.bike.hop_impulse,c.chassis_gravity?m::mul(p.movement.tuning.gravity,chassis_.mass*seconds):Vec3{}));
        const auto rider_impulse=m::add(m::mul(next.rider_force,seconds),m::add(next.rider_hop_impulse,c.rider_gravity?m::mul(p.movement.tuning.gravity,observed_rider.mass*seconds):Vec3{}));
        const auto new_chassis=predict(chassis_,chassis_impulse,m::mul(next.bike.torque,seconds),seconds,c.chassis_body_angular_speed_limit);
        const auto new_rider=predict(next.rider,rider_impulse,m::add(m::mul(next.rider_torque,seconds),next.rider_angular_impulse),seconds,c.rider_body_angular_speed_limit);
        if(detail::source::tire::valid(new_chassis)&&detail::source::tire::valid(new_rider)){movement_=trial;chassis_=new_chassis;rider_=new_rider;}
        else next.valid=false;
    }
    if(!next.valid){
        movement_.invalidate_sample();next.bike.valid=false;next.bike.force=next.bike.torque=next.bike.hop_impulse={};
        next.rider_force=next.rider_torque=next.rider_hop_impulse=next.rider_angular_impulse={};
    }
    presentation(next,chassis_,rider_,p);output_=next;return output_;
}
}
