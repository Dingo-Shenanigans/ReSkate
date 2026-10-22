#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cmath>
#include <cstdint>

namespace dingosdk::bmx::snapshot_retry {
enum class Issue : unsigned {
    none,owner_changed,producer_failed,read_unavailable,invalid_ring,invalid_mass,
    invalid_local_frame,invalid_quaternion,invalid_velocity,direct_array_changed,
    configuration_changed,clock_changed,ring_header_changed,ring_content_changed
};
inline constexpr bool retryable(Issue issue) noexcept {
    return issue==Issue::ring_header_changed||issue==Issue::ring_content_changed;
}
// A rejected sample is never rescued here. Only an explicitly witnessed,
// released user bike may withdraw and request a NEW complete observation on a
// later update. Known bad data/owner/configuration/producer state stays strict.
inline bool suspend_released_observation(Issue issue,bool opted_in,bool released_witness) noexcept {
    return opted_in&&released_witness&&(issue==Issue::read_unavailable||issue==Issue::clock_changed||
        issue==Issue::ring_header_changed||issue==Issue::ring_content_changed||issue==Issue::direct_array_changed);
}
inline const char* describe(Issue issue) noexcept {
    switch(issue){
    case Issue::none:return "native measured chassis";
    case Issue::owner_changed:return "native chassis snapshot owner changed";
    case Issue::producer_failed:return "native chassis producer failed";
    case Issue::read_unavailable:return "native chassis snapshot read unavailable";
    case Issue::invalid_ring:return "native chassis snapshot ring invalid";
    case Issue::invalid_mass:return "native chassis snapshot mass invalid";
    case Issue::invalid_local_frame:return "native chassis snapshot local frame invalid";
    case Issue::invalid_quaternion:return "native chassis snapshot quaternion invalid";
    case Issue::invalid_velocity:return "native chassis snapshot velocity invalid";
    case Issue::direct_array_changed:return "native chassis snapshot direct array changed";
    case Issue::configuration_changed:return "native chassis snapshot configuration proof unavailable";
    case Issue::clock_changed:return "native chassis clock changed during snapshot";
    case Issue::ring_header_changed:return "native chassis snapshot ring header changed between reads";
    case Issue::ring_content_changed:return "native chassis snapshot ring content changed between reads";
    }
    return "native chassis snapshot issue unknown";
}
struct Epoch {
    // client,entity,core,skater context,rig,world,native state,client context.
    std::array<std::uintptr_t,8> owner_world{};
    std::uint32_t body_index=0xffffffffU,body_generation{};
    std::uint64_t token{},serial{};
    double time{};
    float seconds{};
};
struct Qualification {
    bool valid{},calibrated{};
    Epoch epoch;
    Issue issue=Issue::owner_changed;
};
// Historical scalar evidence only. These headers and words are copied from a
// rejected read already performed by snapshot(); no telemetry pointer is read.
struct VelocityRingHeader {
    std::uintptr_t values{};
    std::uint32_t elements{};
    std::uint16_t current_plane{},planes{};
    bool operator==(const VelocityRingHeader&) const = default;
};
struct VelocityRejection {
    Epoch initial_epoch;
    bool initial_calibrated{};
    unsigned read_attempt{},validation_pass{},ring_index{}; // attempt1..3; pass0..2; index2/3
    VelocityRingHeader first_header,rejected_header;
    std::array<std::uint32_t,4> raw_words{}; // bit-exact x,y,z,unnamed W; never JSON floats
};
struct VelocityRejections {
    // First event for each full qualified pool/body/token key, process lifetime.
    // Park/reset/invalidate do not clear this bounded historical publication.
    std::array<VelocityRejection,4> records{};
    unsigned stored{};
    std::uint64_t observations{},same_key_suppressed{},capacity_drops{};
};
static_assert(sizeof(VelocityRejections)<=1024);
inline bool same_velocity_pool(const Epoch& a,const Epoch& b) noexcept {
    return a.owner_world==b.owner_world&&a.body_index==b.body_index&&
        a.body_generation==b.body_generation&&a.token==b.token;
}
inline void increment_velocity_counter(std::uint64_t& value) noexcept {
    if(value!=~std::uint64_t{})++value;
}
inline void record_velocity_rejection(VelocityRejections& history,const Qualification& initial,
        unsigned read_attempt,unsigned validation_pass,unsigned ring_index,
        const VelocityRingHeader& first_header,const VelocityRingHeader& rejected_header,
        const std::array<float,4>& rejected_value) noexcept {
    // Called only after the existing invalid_velocity decision. This function
    // cannot admit a snapshot or authorize a retry, body access or native write.
    if(!initial.valid||!std::isfinite(initial.epoch.time)||!std::isfinite(initial.epoch.seconds)||
       read_attempt<1||read_attempt>3||validation_pass>2||ring_index<2||ring_index>3)return;
    increment_velocity_counter(history.observations);
    for(unsigned i=0;i<history.stored&&i<history.records.size();++i) {
        if(same_velocity_pool(history.records[i].initial_epoch,initial.epoch)) {
            increment_velocity_counter(history.same_key_suppressed);return;
        }
    }
    if(history.stored>=history.records.size()) {
        increment_velocity_counter(history.capacity_drops);return;
    }
    auto& record=history.records[history.stored++];
    record.initial_epoch=initial.epoch;record.initial_calibrated=initial.calibrated;
    record.read_attempt=read_attempt;record.validation_pass=validation_pass;record.ring_index=ring_index;
    record.first_header=first_header;record.rejected_header=rejected_header;
    record.raw_words=std::bit_cast<std::array<std::uint32_t,4>>(rejected_value);
}
struct Result {
    bool accepted{},exhausted{};
    unsigned reads{},races{};
    Issue issue=Issue::read_unavailable,last_race=Issue::none;
};
inline Issue compare(const Qualification& initial,const Qualification& current) noexcept {
    if(!current.valid)return current.issue==Issue::none?Issue::owner_changed:current.issue;
    const auto& a=initial.epoch;const auto& b=current.epoch;
    if(a.owner_world!=b.owner_world||a.body_index!=b.body_index||a.body_generation!=b.body_generation||a.token!=b.token)
        return Issue::owner_changed;
    if(!std::isfinite(b.time)||!std::isfinite(b.seconds)||a.serial!=b.serial||a.time!=b.time||a.seconds!=b.seconds)
        return Issue::clock_changed;
    return Issue::none;
}
// One synchronous observation only. A valid later clock is still a different
// epoch and cannot rescue this sample. No cached body or actuation is provided.
template<class Qualify,class Read> Result run(const Qualification& initial,Qualify&& qualify,Read&& read) noexcept {
    Result out;
    if(!initial.valid){out.issue=initial.issue==Issue::none?Issue::owner_changed:initial.issue;return out;}
    if(!std::isfinite(initial.epoch.time)||!std::isfinite(initial.epoch.seconds)){
        out.issue=Issue::clock_changed;return out;
    }
    for(unsigned attempt=0;attempt<3;++attempt){
        if(attempt){
            const auto current=qualify();out.issue=compare(initial,current);
            if(out.issue!=Issue::none)return out;
            if(!initial.calibrated||!current.calibrated){out.issue=Issue::clock_changed;return out;}
        }
        ++out.reads;out.issue=read();
        if(out.issue==Issue::none){
            const auto final=qualify();out.issue=compare(initial,final);
            if(out.issue!=Issue::none)return out;
            if(attempt&&(!initial.calibrated||!final.calibrated)){out.issue=Issue::clock_changed;return out;}
            out.accepted=true;return out;
        }
        if(!retryable(out.issue))return out;
        ++out.races;out.last_race=out.issue;
        // Detached observation keeps its existing coherent, uncalibrated
        // first-read semantics. It never gains retry permission that way.
        if(!initial.calibrated)return out;
        if(attempt==2){out.exhausted=true;return out;}
    }
    return out;
}
}
