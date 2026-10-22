#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dingosdk::bmx::bail_policy {
using Vec=std::array<double,3>;
inline constexpr double native_seconds=.01666666567325592;
enum class Region { none,frame,torso,head };
enum class Reason { none,hard_rider_contact,severe_contact_tilt,large_frame_impact_proxy,swept_body_crossing };
enum class Issue { none,invalid_sample,unsupported_step,stale_contact };
struct Owner {
    std::uintptr_t client{},entity{},core{},context{},rig{},world{};
    std::uint32_t body_index{},body_generation{};
    std::uint64_t mount_token{};
    bool operator==(const Owner&) const=default;
};
struct GuardContact {
    Region region{Region::none};
    bool available{},hit{};
    std::uint64_t query_serial{};
    Vec point{},normal{}; // real query normal directed from surface toward body
    double separation{}; // measured guard surface gap; positive means clear
    double closing_speed{}; // m/s along inward normal; negative means opening
    // Caller proves consecutive same-owner centers crossed this freshly queried
    // face from its positive side to its negative side, with an actual segment
    // witness. The previous sphere may overlap; arbitrary existing deep overlap
    // without a new center-side crossing is not authorized.
    bool swept_crossing{};
};
struct Sample {
    Owner owner;
    std::uint64_t serial{};
    double seconds{};
    bool mounted{};
    Vec body_up{};
    std::array<GuardContact,8> contacts{};
    std::size_t contact_count{};
    // A force/velocity residual is a proxy, not a direct native collision flag.
    bool residual_available{};
    double residual_impulse_ns{};
};
// Initial prototype thresholds, not recovered effective Streets settings.
struct Tuning {
    double contact_gap{.005},maximum_penetration{.15};
    double hard_rider_closing{2},severe_alignment{.25},immediate_tilt_closing{.5};
    double slow_opening_limit{.05},normal_continuity{.95};
    unsigned slow_tilt_steps{2};
    double frame_proxy_impulse{200},frame_proxy_closing{2};
    double swept_penetration{1.5},swept_closing{.5};
};
struct Result {
    bool valid{},bail{},latched{};
    Issue issue{Issue::none};
    Reason reason{Reason::none};
    Region region{Region::none};
    std::size_t contact_index{};
    double alignment{},closing_speed{};
    unsigned tilt_steps{};
};
namespace detail {
inline bool finite(Vec v,double limit=1e6) noexcept {
    for(double x:v)if(!std::isfinite(x)||std::abs(x)>limit)return false;
    return true;
}
inline double dot(Vec a,Vec b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline bool unit(Vec v) noexcept {return finite(v,2)&&std::abs(dot(v,v)-1)<=.001;}
inline bool owner(const Owner& o) noexcept {
    return o.client&&o.entity&&o.core&&o.context&&o.rig&&o.world&&o.mount_token;
}
inline bool tuning(const Tuning& t) noexcept {
    return std::isfinite(t.contact_gap)&&t.contact_gap>=0&&t.contact_gap<=.01&&
        std::isfinite(t.maximum_penetration)&&t.maximum_penetration>=0&&t.maximum_penetration<=.25&&
        std::isfinite(t.hard_rider_closing)&&t.hard_rider_closing>0&&t.hard_rider_closing<=20&&
        std::isfinite(t.severe_alignment)&&t.severe_alignment>=-1&&t.severe_alignment<=.5&&
        std::isfinite(t.immediate_tilt_closing)&&t.immediate_tilt_closing>=0&&t.immediate_tilt_closing<=20&&
        std::isfinite(t.slow_opening_limit)&&t.slow_opening_limit>=0&&t.slow_opening_limit<=.1&&
        std::isfinite(t.normal_continuity)&&t.normal_continuity>=.5&&t.normal_continuity<=1&&
        t.slow_tilt_steps>=2&&t.slow_tilt_steps<=6&&
        std::isfinite(t.frame_proxy_impulse)&&t.frame_proxy_impulse>53&&t.frame_proxy_impulse<=10000&&
        std::isfinite(t.frame_proxy_closing)&&t.frame_proxy_closing>0&&t.frame_proxy_closing<=20&&
        std::isfinite(t.swept_penetration)&&t.swept_penetration>=t.maximum_penetration&&t.swept_penetration<=1.5&&
        std::isfinite(t.swept_closing)&&t.swept_closing>0&&t.swept_closing<=20;
}
}
class Policy {
    Tuning tuning_;
    Owner owner_{};
    std::uint64_t serial_{};
    bool latched_{};
    Region tilt_region_{Region::none};
    Vec tilt_normal_{};
    unsigned tilt_steps_{};
    void clear_history() noexcept {serial_=0;tilt_region_=Region::none;tilt_normal_={};tilt_steps_=0;}
public:
    explicit Policy(Tuning tuning={}) noexcept:tuning_(tuning){}
    void reset() noexcept {owner_={};latched_=false;clear_history();}
    // Pause discards time/contact history but cannot replay an already fired
    // bail for the same mount. Only reset/new mount ownership rearms it.
    void pause() noexcept {clear_history();}
    Result step(const Sample& s) noexcept {
        Result out;out.latched=latched_;
        if(!s.mounted) {pause();out.valid=true;return out;}
        if(!detail::tuning(tuning_)||!detail::owner(s.owner)||!s.serial||
           !detail::unit(s.body_up)||s.contact_count>s.contacts.size()||
           (s.residual_available&&(!std::isfinite(s.residual_impulse_ns)||s.residual_impulse_ns<0||s.residual_impulse_ns>1e6))) {
            clear_history();out.issue=Issue::invalid_sample;return out;
        }
        if(!std::isfinite(s.seconds)||std::abs(s.seconds-native_seconds)>1e-8) {
            clear_history();out.issue=Issue::unsupported_step;return out;
        }
        if(s.owner!=owner_) {owner_=s.owner;latched_=false;clear_history();out.latched=false;}
        if(s.serial==serial_) {out.valid=true;return out;} // duplicate calls cannot earn another frame
        const bool consecutive=serial_&&s.serial==serial_+1;
        if(!consecutive) {tilt_steps_=0;tilt_region_=Region::none;tilt_normal_={};}
        // Validate every supplied hit before evaluating any trigger. A stale or
        // malformed later entry must not be hidden by an earlier severe hit.
        for(std::size_t i=0;i<s.contact_count;++i) {
            const auto& c=s.contacts[i];
            if(!c.hit)continue;
            if(!c.available||c.region<Region::frame||c.region>Region::head||!detail::finite(c.point)||
               !detail::unit(c.normal)||!std::isfinite(c.separation)||!std::isfinite(c.closing_speed)||
               std::abs(c.closing_speed)>100||c.separation< -(c.swept_crossing?tuning_.swept_penetration:tuning_.maximum_penetration)||
               (c.swept_crossing&&(!consecutive||c.separation>=0))) {
                clear_history();out.issue=Issue::invalid_sample;return out;
            }
            if(c.query_serial!=s.serial) {clear_history();out.issue=Issue::stale_contact;return out;}
        }
        serial_=s.serial;out.valid=true;
        if(latched_) {out.latched=true;return out;}
        const auto trigger=[&](Reason reason,const GuardContact& c,std::size_t index,double alignment) {
            latched_=true;out.bail=true;out.latched=true;out.reason=reason;
            out.region=c.region;out.contact_index=index;out.alignment=alignment;out.closing_speed=c.closing_speed;
        };
        const GuardContact* slow=nullptr;std::size_t slow_index=0;double slow_alignment=1;
        for(std::size_t i=0;i<s.contact_count;++i) {
            const auto& c=s.contacts[i];
            if(!c.hit||c.separation>tuning_.contact_gap)continue;
            const double alignment=detail::dot(s.body_up,c.normal);
            if(c.swept_crossing&&c.closing_speed>=tuning_.swept_closing) {
                trigger(Reason::swept_body_crossing,c,i,alignment);return out;
            }
            if((c.region==Region::head||c.region==Region::torso)&&c.closing_speed>=tuning_.hard_rider_closing) {
                trigger(Reason::hard_rider_contact,c,i,alignment);return out;
            }
            if(alignment<tuning_.severe_alignment) {
                if(c.closing_speed>=tuning_.immediate_tilt_closing) {
                    trigger(Reason::severe_contact_tilt,c,i,alignment);return out;
                }
                if(c.closing_speed>=-tuning_.slow_opening_limit&&alignment<slow_alignment) {
                    slow=&c;slow_index=i;slow_alignment=alignment;
                }
            }
            if(c.region==Region::frame&&s.residual_available&&
               s.residual_impulse_ns>=tuning_.frame_proxy_impulse&&c.closing_speed>=tuning_.frame_proxy_closing) {
                trigger(Reason::large_frame_impact_proxy,c,i,alignment);return out;
            }
        }
        if(slow) {
            const bool same=consecutive&&tilt_region_==slow->region&&detail::dot(tilt_normal_,slow->normal)>=tuning_.normal_continuity;
            tilt_steps_=same?tilt_steps_+1:1;tilt_region_=slow->region;tilt_normal_=slow->normal;
            out.tilt_steps=tilt_steps_;
            if(tilt_steps_>=tuning_.slow_tilt_steps)trigger(Reason::severe_contact_tilt,*slow,slow_index,slow_alignment);
        } else {tilt_steps_=0;tilt_region_=Region::none;tilt_normal_={};}
        return out;
    }
};
}
