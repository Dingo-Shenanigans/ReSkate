#pragma once
#include "Engine/Game/BMX/bmx_native_coupled_policy.h"
#include "Runtime/startup_recording_gate.h"

namespace dingosdk::bmx::trial_marker {
namespace np=native_coupled::policy;
using Identity=::dingosdk::runtime::recording_gate::CaptureIdentity;
struct Activation {np::Epoch epoch;std::uint64_t begun{},succeeded{},expiry{};};
class Producer {
    Identity identity_;std::uint64_t generation_{};bool initialized_{},activation_claimed_{},terminal_claimed_{};
    Activation activation_;
public:
    void initialize(const Identity& identity,std::uint64_t generation) noexcept {if(initialized_)return;initialized_=true;identity_=identity;generation_=generation;}
    bool configured() const noexcept {return identity_.released&&identity_.process_id&&identity_.creation_filetime&&identity_.released_tick_ms&&generation_&&
        ::dingosdk::runtime::recording_gate::valid_nonce(std::string_view(identity_.nonce.data(),identity_.nonce.size()));}
    bool activation(std::uint64_t begun,std::uint64_t succeeded,const np::Epoch& epoch,bool call_succeeded,bool entry_ready,
                    bool render_verified,bool actual_pair_qualified,bool backend_active,bool normal_adoption) noexcept {
        if(activation_claimed_||terminal_claimed_||!configured())return false;activation_claimed_=true;
        if(!call_succeeded||!entry_ready||!render_verified||!actual_pair_qualified||!backend_active||normal_adoption||!np::valid_epoch(epoch)||
           epoch.seconds!=static_cast<float>(np::native_seconds)||begun<generation_||begun<identity_.released_tick_ms||
           begun>std::numeric_limits<std::uint64_t>::max()-10000||succeeded<begun||succeeded>=begun+10000)return false;
        activation_={epoch,begun,succeeded,begun+10000};return true;
    }
    bool terminal() noexcept {if(terminal_claimed_||!configured())return false;terminal_claimed_=true;return true;}
    bool terminal_epoch(const np::Epoch& actual) const noexcept {return np::valid_epoch(actual)&&(!activation_.expiry||
        (actual.owner_world==activation_.epoch.owner_world&&actual.bodies==activation_.epoch.bodies&&actual.token==activation_.epoch.token));}
    const Identity& identity() const noexcept{return identity_;}std::uint64_t generation() const noexcept{return generation_;}
    const Activation& activated() const noexcept{return activation_;}
};
template<class Row> struct Trace {
    std::array<Row,48> entry;std::array<Row,640> active;std::array<Row,16> events;
    std::uint64_t entry_total{},active_total{},event_total{};bool entry_overflow{},active_overflow{},event_overflow{};
    void append(const Row& row,bool active_phase) noexcept {
        if(active_phase){if(active_total<active.size())active[static_cast<std::size_t>(active_total)]=row;else active_overflow=true;
            if(active_total!=std::numeric_limits<std::uint64_t>::max())++active_total;else active_overflow=true;
        }else{entry[static_cast<std::size_t>(entry_total%entry.size())]=row;if(entry_total>=entry.size())entry_overflow=true;
            if(entry_total!=std::numeric_limits<std::uint64_t>::max())++entry_total;else entry_overflow=true;}
    }
    void event(const Row& row) noexcept {if(event_total<events.size())events[static_cast<std::size_t>(event_total)]=row;else event_overflow=true;
        if(event_total!=std::numeric_limits<std::uint64_t>::max())++event_total;else event_overflow=true;}
};
}
