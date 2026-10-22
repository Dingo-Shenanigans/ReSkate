#include "Extension/BMX/bmx_snapshot_retry.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace r=dingosdk::bmx::snapshot_retry;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
r::Qualification initial(){
    r::Qualification q;q.valid=q.calibrated=true;q.issue=r::Issue::none;
    q.epoch.owner_world={1,2,3,4,5,6,7,8};q.epoch.body_index=19;q.epoch.body_generation=7;
    q.epoch.token=4;q.epoch.serial=61;q.epoch.time=61.0/60;q.epoch.seconds=1.f/60;return q;
}
struct Reader {
    r::Qualification start=initial();
    std::vector<r::Issue> outcomes{r::Issue::none};
    std::vector<r::Qualification> proofs;
    unsigned reads{},qualifications{};
    r::Result run(){
        return r::run(start,[&]() noexcept {
            const auto index=qualifications++;
            return index<proofs.size()?proofs[index]:start;
        },[&]() noexcept {
            const auto index=reads++;
            return index<outcomes.size()?outcomes[index]:outcomes.back();
        });
    }
};
}
int main(){try{
    for(unsigned value=0;value<=static_cast<unsigned>(r::Issue::ring_content_changed);++value) {
        const auto issue=static_cast<r::Issue>(value);
        const bool transient=issue==r::Issue::read_unavailable||issue==r::Issue::clock_changed||
            issue==r::Issue::ring_header_changed||issue==r::Issue::ring_content_changed||issue==r::Issue::direct_array_changed;
        for(unsigned flags=0;flags<4;++flags)
            require(r::suspend_released_observation(issue,bool(flags&1),bool(flags&2))==(transient&&flags==3),
                "only opted-in released witnesses may suspend unavailable samples; known invalid data stays strict");
    }
    require(!r::suspend_released_observation(static_cast<r::Issue>(999),true,true),"unknown issue cannot gain suspension permission");
    Reader stable;auto result=stable.run();
    require(result.accepted&&result.reads==1&&stable.qualifications==1&&!result.races,
        "coherent body uses one read and a fresh final qualification");
    Reader published;published.outcomes={r::Issue::ring_header_changed,r::Issue::none};result=published.run();
    require(result.accepted&&result.reads==2&&result.races==1&&result.last_race==r::Issue::ring_header_changed&&published.qualifications==2,
        "a fully qualified same-epoch publication race can recover without advancing the physics clock");
    Reader inplace;inplace.outcomes={r::Issue::ring_content_changed,r::Issue::ring_header_changed,r::Issue::none};result=inplace.run();
    require(result.accepted&&result.reads==3&&result.races==2,"two observed races permit only two additional same-call reads");
    Reader spinning;spinning.outcomes={r::Issue::ring_content_changed};result=spinning.run();
    require(!result.accepted&&result.exhausted&&spinning.reads==3&&result.issue==r::Issue::ring_content_changed,
        "permanently mutating ring cannot spin or fabricate a cached success");
    for(const auto issue:{r::Issue::owner_changed,r::Issue::producer_failed,r::Issue::read_unavailable,r::Issue::invalid_ring,
        r::Issue::invalid_mass,r::Issue::invalid_local_frame,r::Issue::invalid_quaternion,r::Issue::invalid_velocity,
        r::Issue::direct_array_changed,r::Issue::configuration_changed,r::Issue::clock_changed}){
        Reader fatal;fatal.outcomes={issue,r::Issue::none};result=fatal.run();
        require(!result.accepted&&fatal.reads==1&&!fatal.qualifications&&!result.exhausted&&result.issue==issue,
            "ownership, read, invariant, direct-SoA and clock failures cannot be hidden by a later good read");
    }
    for(unsigned key=0;key<8;++key){
        Reader replaced;auto changed=replaced.start;++changed.epoch.owner_world[key];replaced.proofs={changed};
        replaced.outcomes={r::Issue::ring_header_changed,r::Issue::none};result=replaced.run();
        require(!result.accepted&&result.issue==r::Issue::owner_changed&&replaced.reads==1,
            "every owner/world identity component remains pinned before retry");
    }
    for(unsigned key=0;key<3;++key){
        Reader replaced;auto changed=replaced.start;
        if(key==0)++changed.epoch.token;else if(key==1)++changed.epoch.body_index;else ++changed.epoch.body_generation;
        replaced.proofs={changed};replaced.outcomes={r::Issue::ring_content_changed,r::Issue::none};result=replaced.run();
        require(!result.accepted&&replaced.reads==1&&result.issue==r::Issue::owner_changed,
            "body token and full native body handle cannot be replaced during recovery");
    }
    for(unsigned clock=0;clock<3;++clock){
        Reader advanced;auto changed=advanced.start;
        if(clock==0)++changed.epoch.serial;else if(clock==1)changed.epoch.time+=1.0/60;else changed.epoch.seconds*=2;
        advanced.proofs={changed};advanced.outcomes={r::Issue::ring_header_changed,r::Issue::none};result=advanced.run();
        require(!result.accepted&&advanced.reads==1&&result.issue==r::Issue::clock_changed,
            "a later serial/time/step cannot rescue the original observation");
    }
    Reader final_clock;auto advanced=final_clock.start;++advanced.epoch.serial;final_clock.proofs={advanced};result=final_clock.run();
    require(!result.accepted&&result.reads==1&&result.issue==r::Issue::clock_changed,
        "even a coherent first read fails if the final clock proof changed");
    Reader final_owner;auto lost=final_owner.start;lost.valid=false;lost.issue=r::Issue::owner_changed;
    final_owner.outcomes={r::Issue::ring_content_changed,r::Issue::none};final_owner.proofs={final_owner.start,lost};result=final_owner.run();
    require(!result.accepted&&result.reads==2&&result.issue==r::Issue::owner_changed,
        "successful retry content cannot conceal owner loss at final qualification");
    Reader final_config;lost=final_config.start;lost.valid=false;lost.issue=r::Issue::configuration_changed;
    final_config.proofs={lost};result=final_config.run();
    require(!result.accepted&&result.issue==r::Issue::configuration_changed,"configuration remains mandatory after a coherent read");
    Reader uncalibrated;uncalibrated.start.calibrated=false;result=uncalibrated.run();
    require(result.accepted&&result.reads==1,"existing detached coherent first-read semantics remain available");
    uncalibrated=Reader{};uncalibrated.start.calibrated=false;uncalibrated.outcomes={r::Issue::ring_header_changed,r::Issue::none};result=uncalibrated.run();
    require(!result.accepted&&uncalibrated.reads==1&&!uncalibrated.qualifications,
        "uncalibrated observation never gains race retry permission");
    Reader lost_calibration;auto unclear=lost_calibration.start;unclear.calibrated=false;
    lost_calibration.proofs={unclear};lost_calibration.outcomes={r::Issue::ring_content_changed,r::Issue::none};result=lost_calibration.run();
    require(!result.accepted&&lost_calibration.reads==1&&result.issue==r::Issue::clock_changed,
        "retry requires current calibrated timing as well as matching clock values");
    Reader invalid;invalid.start.epoch.time=std::numeric_limits<double>::quiet_NaN();result=invalid.run();
    require(!result.accepted&&!invalid.reads&&result.issue==r::Issue::clock_changed,"nonfinite initial clock cannot start observation");
    // A rejected unnamed W lane must remain distinguishable from XYZ without
    // accepting it, converting to JSON floats, or replacing its first evidence.
    r::VelocityRejections history;
    const r::VelocityRingHeader first_header{0x100000,2048,1,3};
    const r::VelocityRingHeader rejected_header{0x200000,2048,2,3};
    const std::array<float,4> w_only{1.f,-0.f,3.f,100.125f};
    const auto first_qualification=initial();
    r::record_velocity_rejection(history,first_qualification,2,1,3,first_header,rejected_header,w_only);
    require(history.stored==1&&history.observations==1&&history.records[0].raw_words==
        std::array<std::uint32_t,4>{0x3f800000U,0x80000000U,0x40400000U,0x42c84000U},
        "all four rejected words, including signed zero and unnamed W, are retained bit exactly");
    const auto& retained=history.records[0];
    require(retained.read_attempt==2&&retained.validation_pass==1&&retained.ring_index==3&&
        retained.first_header==first_header&&retained.rejected_header==rejected_header&&
        retained.initial_calibrated&&retained.initial_epoch.serial==first_qualification.epoch.serial&&
        retained.initial_epoch.time==first_qualification.epoch.time&&
        retained.initial_epoch.seconds==first_qualification.epoch.seconds,
        "rejected pass/header and initial owner/clock provenance remain separate from snapshot acceptance");
    auto later=first_qualification;++later.epoch.serial;later.epoch.time+=1.0/60;
    r::record_velocity_rejection(history,later,1,0,2,rejected_header,rejected_header,{120.f,0.f,0.f,0.f});
    require(history.stored==1&&history.same_key_suppressed==1&&history.records[0].raw_words==
        std::bit_cast<std::array<std::uint32_t,4>>(w_only)&&
        history.records[0].initial_epoch.serial==first_qualification.epoch.serial,
        "a later same-pool/token rejection cannot overwrite the first event after park or clock advancement");
    for(unsigned key=0;key<11;++key) {
        r::VelocityRejections separate;auto next=first_qualification;
        if(key<8)++next.epoch.owner_world[key];
        else if(key==8)++next.epoch.body_index;
        else if(key==9)++next.epoch.body_generation;
        else ++next.epoch.token;
        r::record_velocity_rejection(separate,first_qualification,1,0,2,first_header,first_header,w_only);
        r::record_velocity_rejection(separate,next,3,2,3,first_header,rejected_header,{0.f,0.f,130.f,0.f});
        require(separate.stored==2&&r::same_velocity_pool(separate.records[1].initial_epoch,next.epoch)&&
            separate.records[0].initial_epoch.token==first_qualification.epoch.token,
            "each full owner/world/body/token component separates historical rejection keys");
    }
    for(unsigned token=1;token<=5;++token) {
        auto next=first_qualification;next.epoch.token+=token;
        r::record_velocity_rejection(history,next,1,0,2,first_header,first_header,w_only);
    }
    require(history.stored==4&&history.capacity_drops==2&&history.observations==7&&
        history.records[0].raw_words==std::bit_cast<std::array<std::uint32_t,4>>(w_only),
        "fixed historical capacity reports drops and never evicts the original failed ride evidence");
    r::record_velocity_rejection(history,first_qualification,1,0,3,first_header,first_header,{0.f,200.f,0.f,0.f});
    require(history.same_key_suppressed==2&&history.capacity_drops==2,
        "a known key still deduplicates when bounded history is full");
    r::VelocityRejections refused;auto unqualified=first_qualification;unqualified.valid=false;
    r::record_velocity_rejection(refused,unqualified,1,0,2,first_header,first_header,w_only);
    for(unsigned bad=0;bad<5;++bad) {
        auto q=first_qualification;
        if(bad==0)q.epoch.time=std::numeric_limits<double>::quiet_NaN();
        r::record_velocity_rejection(refused,q,bad==1?0U:bad==2?4U:1U,bad==3?3U:0U,
            bad==4?1U:2U,first_header,first_header,w_only);
    }
    require(!refused.stored&&!refused.observations,
        "no invented event can be published from absent qualification, nonfinite clock or unknown read phase");
    r::VelocityRejections saturated;saturated.observations=~std::uint64_t{};
    r::record_velocity_rejection(saturated,first_qualification,1,0,2,first_header,first_header,w_only);
    saturated.same_key_suppressed=~std::uint64_t{};
    r::record_velocity_rejection(saturated,first_qualification,1,0,2,first_header,first_header,w_only);
    require(saturated.observations==~std::uint64_t{}&&saturated.same_key_suppressed==~std::uint64_t{},
        "process-lifetime diagnostic counters saturate instead of wrapping");
    std::cout<<"snapshot retry qualification tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
