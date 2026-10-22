#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dingosdk::bmx::fall_lifecycle {
using Vec=std::array<float,3>;
template<class T> struct Measured { bool available{};T value{}; };
struct RiderIdentity {
    std::uintptr_t client{},entity{},core{},context{},rig{};
    friend bool operator==(const RiderIdentity&,const RiderIdentity&)=default;
};
struct BodyIdentity {
    std::uintptr_t world{},native_state{};
    std::uint32_t index=0xffffffffU,generation{};
    std::uint64_t token{};
    friend bool operator==(const BodyIdentity&,const BodyIdentity&)=default;
};
struct Identity { RiderIdentity rider;BodyIdentity body; };
struct Clock { std::uint64_t serial{};double time{};float seconds{}; };
// Stored exactly once. Freshness is supplied by the request caller, never
// reconstructed from retained global fall flags or a nonzero request counter.
struct RequestReceipt {
    bool attempted{},accepted{},status_fresh{},status_available{},typed{};
    std::uintptr_t entity{};
    std::uint64_t request_id{},generation{};
};
enum class ReceiptBinding : unsigned { unbound,accepted_request,rejected_request };
inline ReceiptBinding qualify(const RequestReceipt& r,const RiderIdentity& owner) noexcept {
    if(!r.attempted||!r.status_fresh||!r.status_available||!r.request_id||r.entity!=owner.entity||
       (r.typed&&!r.generation))return ReceiptBinding::unbound;
    return r.accepted?ReceiptBinding::accepted_request:ReceiptBinding::rejected_request;
}
struct RiderState {
    Vec root{};
    bool offboard{},standable{},motion_supported{};
    // These optional lanes require a fresh guarded native read in THIS epoch.
    // In particular bail::status() retained flags are not fresh native samples.
    Measured<std::uintptr_t> active_state,pending_state;
    Measured<std::uint32_t> physics_state,offboard_flags;
    Measured<std::uint32_t> impact_flags,animation_request_mask,state_kind,active_offset,pending_offset;
    Measured<std::uintptr_t> active_vtable_rva;
    Measured<std::uint8_t> transition_lock,ragdoll_byte;
    Measured<bool> ragdoll_mode;
    Measured<Vec> motion_velocity;
    // Canonical typed getter observations, only when independently qualified
    // to this sample's owner and native epoch. Unknown binding stays null.
    Measured<bool> getup_allowed,getup_intent,getup_allowed_binding_present,getup_intent_binding_present;
    Measured<std::uint64_t> getup_generation;
    Measured<bool> protection_active,input_focused;
    Measured<std::uint32_t> input_buttons,input_device;
    bool handoff_requested{};
    Measured<std::array<float,4>> context_830,context_7c0,active_160;
    Measured<std::array<float,16>> rig_4720,rig_4760;
    // A readable native lane may contain a non-coordinate fourth/padding word.
    // Preserve its bits independently of the finite coordinate projection.
    Measured<std::array<std::uint32_t,4>> context_830_words,context_7c0_words,active_160_words;
    Measured<Vec> context_830_xyz,context_7c0_xyz,active_160_xyz;
    Measured<std::array<std::uint32_t,16>> rig_4720_words,rig_4760_words;
    Measured<Vec> rig_4720_translation,rig_4760_translation;
    Measured<std::uintptr_t> physical_rig,physical_rig_parts;
    std::array<Measured<std::array<float,4>>,4> physical_body_positions;
    std::array<Measured<std::array<std::uint32_t,4>>,4> physical_body_position_words;
    std::array<Measured<Vec>,4> physical_body_position_xyz;
    std::array<Measured<std::uint32_t>,4> physical_body_flags;
};
struct BodyState { Vec com{},linear_velocity{},angular_velocity{};std::array<float,4> orientation{0,0,0,1}; };
struct Counters { std::uint64_t velocity_updates{},rider_poses{},render_poses{},force_applications{}; };
struct CaptureState {
    Measured<bool> bmx_capture,interaction_capture,rider_motion_lease,drive_active,board_switch_pending;
};
enum class SnapshotIssue : unsigned { none,read_unavailable,epoch_mismatch,invalid_value };
struct Observation {
    Measured<RiderIdentity> rider_identity;
    Measured<BodyIdentity> body_identity;
    Measured<Clock> clock;
    // All AVAILABLE raw rider/body lanes must belong to clock's qualified
    // epoch. Missing body observations remain missing; the old body binding
    // does not grant them freshness or erase a successfully read rider sample.
    bool coherent_epoch{};
    Measured<RiderState> rider;
    Measured<BodyState> body;
    Measured<Counters> counters;
    CaptureState capture;
    // Optional caller context only; this recorder never sends native input and
    // cannot establish that any input was a get-up action or caused a change.
    Measured<bool> explicit_native_input_provided;
    SnapshotIssue snapshot_issue=SnapshotIssue::none;
    // Optional bounded caller request to retain this already-qualified edge.
    // It changes storage cadence only, never identity/clock validation or facts.
    bool record_edge{};
};
struct Sample {
    Clock clock;
    double elapsed{};
    Measured<RiderState> rider;
    Measured<BodyState> body;
    Measured<Counters> counters;
    CaptureState capture;
    Measured<bool> explicit_native_input_provided;
    SnapshotIssue snapshot_issue=SnapshotIssue::none;
};
enum class Phase : unsigned { idle,recording,frozen };
enum class Reason : unsigned {
    none,window_elapsed,body_released,caller_cancelled,runtime_stopped,wall_timeout,
    invalid_begin,rider_unavailable,owner_changed,body_changed,clock_unavailable,
    clock_backwards,clock_discontinuity
};
inline const char* describe(Reason reason) noexcept {
    switch(reason) {
    case Reason::none:return "none";
    case Reason::window_elapsed:return "native observation window elapsed";
    case Reason::body_released:return "bound bike body explicitly released";
    case Reason::caller_cancelled:return "caller cancelled observation";
    case Reason::runtime_stopped:return "runtime stopped observation";
    case Reason::wall_timeout:return "external wall timeout; native window incomplete";
    case Reason::invalid_begin:return "invalid initial identity, clock or configuration";
    case Reason::rider_unavailable:return "fresh rider identity unavailable";
    case Reason::owner_changed:return "fresh rider identity changed";
    case Reason::body_changed:return "fresh bike body identity changed";
    case Reason::clock_unavailable:return "fresh native clock unavailable";
    case Reason::clock_backwards:return "native clock moved backwards";
    case Reason::clock_discontinuity:return "native clock discontinuity";
    }
    return "unknown observation end";
}
struct Config {
    double window_seconds=30,sample_period=.1,maximum_clock_gap=.25;
    std::uint64_t maximum_serial_gap=30;
};
struct Status {
    Phase phase=Phase::idle;
    Reason reason=Reason::none;
    Identity identity;
    RequestReceipt receipt;
    ReceiptBinding receipt_binding=ReceiptBinding::unbound;
    Clock initial_clock,last_clock;
    double native_elapsed{};
    std::uint64_t accepted_edges{},duplicates{},paused_edges{},decimated_edges{},
        written_samples{},overwritten_samples{},snapshot_failures{};
    bool nonstandable_observed{},standable_observed{},ragdoll_observed{},standable_after_nonstandable{},
        standable_after_ragdoll{},counter_advance_observed{},counter_regression_observed{};
    Measured<Clock> first_nonstandable,first_standable,first_ragdoll,first_standable_after_nonstandable;
};
enum class Result : unsigned { ignored,duplicate,decimated,recorded,frozen };
inline bool finite(Vec v) noexcept {for(float x:v)if(!std::isfinite(x)||std::abs(x)>1000000)return false;return true;}
inline bool valid(const RiderIdentity& r) noexcept {return r.client&&r.entity&&r.core&&r.context&&r.rig;}
inline bool valid(const BodyIdentity& b) noexcept {return b.world&&b.native_state&&b.token&&b.index!=0xffffffffU;}
inline bool valid(const Clock& c) noexcept {
    return c.serial&&std::isfinite(c.time)&&c.time>=0&&std::isfinite(c.seconds)&&c.seconds>=0&&c.seconds<=.1f;
}
// Single caller thread. Fixed storage; begin/observe/finish allocate nothing,
// perform no IO and never actuate the rider/bike. A frozen history is immutable
// until an explicit reset, so a later global status cannot rewrite a prior fall.
template<std::size_t Capacity=320> class History {
    static_assert(Capacity>0);
public:
    bool begin(const Identity& identity,const RequestReceipt& receipt,const Observation& initial,
               Config config={}) noexcept {
        if(status_.phase!=Phase::idle)return false;
        status_.identity=identity;status_.receipt=receipt;status_.receipt_binding=qualify(receipt,identity.rider);
        if(!valid(identity.rider)||!valid(identity.body)||!initial.rider_identity.available||
           initial.rider_identity.value!=identity.rider||!initial.body_identity.available||
           initial.body_identity.value!=identity.body||!initial.clock.available||!valid(initial.clock.value)||
           !std::isfinite(config.window_seconds)||config.window_seconds<15||config.window_seconds>30||
           !std::isfinite(config.sample_period)||config.sample_period<.1||config.sample_period>1||
           !std::isfinite(config.maximum_clock_gap)||config.maximum_clock_gap<=0||config.maximum_clock_gap>1||
           !config.maximum_serial_gap) {
            seal(Reason::invalid_begin);return false;
        }
        config_=config;status_.phase=Phase::recording;status_.initial_clock=status_.last_clock=initial.clock.value;
        status_.accepted_edges=1;auto first=make_sample(initial);facts(first);initial_=first;append(first);
        next_due_=config_.sample_period;return true;
    }
    Result observe(const Observation& observation) noexcept {
        if(status_.phase!=Phase::recording)return Result::ignored;
        if(!observation.rider_identity.available)return seal(Reason::rider_unavailable);
        if(observation.rider_identity.value!=status_.identity.rider)return seal(Reason::owner_changed);
        if(observation.body_identity.available&&observation.body_identity.value!=status_.identity.body)
            return seal(Reason::body_changed);
        if(!observation.clock.available)return seal(Reason::clock_unavailable);
        const auto clock=observation.clock.value;
        if(!valid(clock))return seal(Reason::clock_discontinuity);
        const auto& previous=status_.last_clock;
        if(clock.serial<previous.serial||clock.time<previous.time)return seal(Reason::clock_backwards);
        if(clock.serial==previous.serial) {
            if(clock.time!=previous.time)return seal(Reason::clock_discontinuity);
            ++status_.duplicates;return Result::duplicate;
        }
        const double delta=clock.time-previous.time;
        const auto edges=clock.serial-previous.serial;
        if(edges>config_.maximum_serial_gap||delta>config_.maximum_clock_gap||delta>double(edges)*.1+1e-5||
           (edges==1&&std::abs(delta-double(clock.seconds))>1e-5)||
           (delta==0&&clock.seconds!=0))return seal(Reason::clock_discontinuity);
        status_.last_clock=clock;status_.native_elapsed=clock.time-status_.initial_clock.time;++status_.accepted_edges;
        if(delta==0)++status_.paused_edges;
        auto sample=make_sample(observation);sample.elapsed=status_.native_elapsed;
        const bool new_fact=facts(sample);
        const bool window_elapsed=status_.native_elapsed>=config_.window_seconds;
        if(observation.record_edge||sample.elapsed+1e-8>=next_due_||new_fact||window_elapsed) {
            append(sample);
            next_due_=(std::floor((sample.elapsed+1e-8)/config_.sample_period)+1)*config_.sample_period;
            if(window_elapsed)return seal(Reason::window_elapsed);
            return Result::recorded;
        }
        ++status_.decimated_edges;return Result::decimated;
    }
    // External lifecycle events end observation without manufacturing a new
    // native sample, extending native elapsed time, or asserting get-up success.
    bool finish(Reason reason) noexcept {
        if(status_.phase!=Phase::recording)return false;
        if(reason!=Reason::body_released&&reason!=Reason::caller_cancelled&&reason!=Reason::runtime_stopped&&
           reason!=Reason::wall_timeout)return false;
        seal(reason);return true;
    }
    // Hint for OPTIONAL expensive detail reads only. Every new native edge must
    // still reach observe(), including paused edges and off-cadence state facts.
    // Asking the hint neither advances the clock nor consumes a sampling slot.
    bool sample_due(const Clock& clock) const noexcept {
        if(status_.phase!=Phase::recording||!valid(clock)||clock.serial<=status_.last_clock.serial||
           clock.time<status_.last_clock.time)return false;
        const double elapsed=clock.time-status_.initial_clock.time;
        return elapsed+1e-8>=next_due_||elapsed>=config_.window_seconds;
    }
    void reset() noexcept {
        status_={};config_={};initial_={};last_counters_={};next_=size_=0;next_due_=0;
    }
    const Status& status() const noexcept {return status_;}
    const Sample& initial() const noexcept {return initial_;}
    std::size_t size() const noexcept {return size_;}
    static constexpr std::size_t capacity() noexcept {return Capacity;}
    template<class Visit> void each(Visit&& visit) const {
        const auto first=(next_+Capacity-size_)%Capacity;
        for(std::size_t i=0;i<size_;++i)visit(samples_[(first+i)%Capacity]);
    }
private:
    Result seal(Reason reason) noexcept {status_.phase=Phase::frozen;status_.reason=reason;return Result::frozen;}
    void append(const Sample& sample) noexcept {
        samples_[next_]=sample;next_=(next_+1)%Capacity;
        if(size_<Capacity)++size_;else ++status_.overwritten_samples;
        ++status_.written_samples;
    }
    Sample make_sample(const Observation& o) noexcept {
        Sample s;s.clock=o.clock.value;s.rider=o.rider;s.body=o.body;s.counters=o.counters;s.capture=o.capture;
        s.explicit_native_input_provided=o.explicit_native_input_provided;s.snapshot_issue=o.snapshot_issue;
        if(!o.coherent_epoch) {
            s.rider={};s.body={};s.snapshot_issue=SnapshotIssue::epoch_mismatch;
        } else {
            if(!o.body_identity.available)s.body={};
            if(s.rider.available&&!finite(s.rider.value.root)) {s.rider={};s.snapshot_issue=SnapshotIssue::invalid_value;}
            if(s.rider.available&&s.rider.value.motion_velocity.available&&!finite(s.rider.value.motion_velocity.value)) {
                s.rider.value.motion_velocity={};s.snapshot_issue=SnapshotIssue::invalid_value;
            }
            if(s.body.available) {
                bool good=finite(s.body.value.com)&&finite(s.body.value.linear_velocity)&&finite(s.body.value.angular_velocity);
                double norm=0;for(float v:s.body.value.orientation){good=good&&std::isfinite(v);norm+=double(v)*v;}
                if(!good||std::abs(norm-1)>.001){s.body={};s.snapshot_issue=SnapshotIssue::invalid_value;}
            }
        }
        if(!s.rider.available||!s.body.available) {
            if(s.snapshot_issue==SnapshotIssue::none)s.snapshot_issue=SnapshotIssue::read_unavailable;
        }
        if(s.snapshot_issue!=SnapshotIssue::none)++status_.snapshot_failures;
        return s;
    }
    bool facts(const Sample& sample) noexcept {
        bool first=false;
        if(sample.rider.available) {
            const auto& r=sample.rider.value;
            const bool earlier_nonstandable=status_.nonstandable_observed&&
                sample.clock.serial>status_.first_nonstandable.value.serial;
            const bool earlier_ragdoll=status_.ragdoll_observed&&sample.clock.serial>status_.first_ragdoll.value.serial;
            if(!r.standable&&!status_.nonstandable_observed) {
                status_.nonstandable_observed=true;status_.first_nonstandable={true,sample.clock};first=true;
            }
            if(r.ragdoll_mode.available&&r.ragdoll_mode.value&&!status_.ragdoll_observed) {
                status_.ragdoll_observed=true;status_.first_ragdoll={true,sample.clock};first=true;
            }
            if(r.standable) {
                if(!status_.standable_observed){status_.standable_observed=true;status_.first_standable={true,sample.clock};first=true;}
                if(earlier_nonstandable&&!status_.standable_after_nonstandable) {
                    status_.standable_after_nonstandable=true;status_.first_standable_after_nonstandable={true,sample.clock};first=true;
                }
                if(earlier_ragdoll&&!status_.standable_after_ragdoll) {status_.standable_after_ragdoll=true;first=true;}
            }
        }
        if(sample.counters.available) {
            if(last_counters_.available) {
                const auto a=last_counters_.value,b=sample.counters.value;
                status_.counter_advance_observed|=b.velocity_updates>a.velocity_updates||b.rider_poses>a.rider_poses||
                    b.render_poses>a.render_poses||b.force_applications>a.force_applications;
                status_.counter_regression_observed|=b.velocity_updates<a.velocity_updates||b.rider_poses<a.rider_poses||
                    b.render_poses<a.render_poses||b.force_applications<a.force_applications;
            }
            last_counters_=sample.counters;
        }
        return first;
    }
    std::array<Sample,Capacity> samples_{};
    Status status_;
    Config config_;
    Sample initial_;
    Measured<Counters> last_counters_;
    std::size_t next_{},size_{};
    double next_due_{};
};
} // namespace dingosdk::bmx::fall_lifecycle
