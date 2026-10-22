#pragma once
#include <atomic>
#include <cstdint>

namespace dingosdk::bmx::intent_observer::detail {
// No native pointers are invoked here. Every dispatch receives a NEW current
// callback key and resolver/qualification functions. A pending reset is an
// obligation independent of the short permission to start the pulse.
enum class PulsePhase : unsigned { armed,reset_pending,reset,preexisting_true,owner_lost };
enum class PulseStage : unsigned { idle,qualify,resolve,prior_get,prior_true,entry,readback,reset_verified,owner_lost };
struct PulseKey {std::uintptr_t binding{};std::uint64_t handle{},serial{};};
inline constexpr bool blocks_rearm(bool armed,bool completed_or_cancelled,bool reset_pending,
    std::uint64_t true_calls,bool reset_verified) noexcept {
    return armed&&(!completed_or_cancelled||reset_pending||(true_calls&&!reset_verified));
}
struct Pulse {
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    std::atomic<PulsePhase> phase{PulsePhase::armed};
    std::atomic<PulseStage> stage{};
    std::atomic<std::uintptr_t> binding{};
    std::atomic<std::uint64_t> handle{},true_serial{},reset_serial{},true_ms{},reset_ms{};
    std::atomic<std::uint64_t> getters{},true_calls{},reset_calls{},preflight_serial{},last_reset_attempt{};
    std::atomic<std::uint64_t> generation{};
    std::atomic<bool> prior_available{},prior_false{},true_returned{},written{},true_readback_available{},
        true_readback{},reset_returned{},reset_readback_available{},reset_false{},native_cleared{},faulted{};
    bool pending() const noexcept {return phase.load(std::memory_order_acquire)==PulsePhase::reset_pending;}
    // The lifecycle owner also serializes its native owner/anchor storage. This
    // reservation independently rejects a rearm during a setter or uncertain
    // cleanup. Losing an owner never proves that a possible write was reset.
    bool rearm(bool completed_or_cancelled) noexcept {
        if(busy.test_and_set(std::memory_order_acquire))return false;
        struct Unlock {std::atomic_flag& flag;~Unlock(){flag.clear(std::memory_order_release);}} unlock{busy};
        const auto old=phase.load(std::memory_order_acquire);
        if(blocks_rearm(generation.load()!=0,completed_or_cancelled,pending(),true_calls.load(),reset_false.load())||
           generation.load()==UINT64_MAX)return false;
        if(old!=PulsePhase::armed&&old!=PulsePhase::reset&&old!=PulsePhase::preexisting_true&&old!=PulsePhase::owner_lost)return false;
        binding.store(0);handle.store(0);true_serial.store(0);reset_serial.store(0);true_ms.store(0);reset_ms.store(0);
        getters.store(0);true_calls.store(0);reset_calls.store(0);preflight_serial.store(0);last_reset_attempt.store(0);
        prior_available.store(false);prior_false.store(false);true_returned.store(false);written.store(false);
        true_readback_available.store(false);true_readback.store(false);reset_returned.store(false);
        reset_readback_available.store(false);reset_false.store(false);native_cleared.store(false);faulted.store(false);
        stage.store(PulseStage::idle);phase.store(PulsePhase::armed,std::memory_order_release);
        generation.fetch_add(1,std::memory_order_release);return true;
    }
    template<class Resolve,class Qualify,class Get,class Set>
    void dispatch(PulseKey key,std::uint64_t started,std::uint64_t now,bool eligible,bool cancelled,bool owner_changed,
        Resolve&& resolve,Qualify&& qualify,Get&& get,Set&& set) noexcept {
        if(busy.test_and_set(std::memory_order_acquire))return;
        struct Unlock {std::atomic_flag& flag;~Unlock(){flag.clear(std::memory_order_release);}} unlock{busy};
        const auto current=phase.load(std::memory_order_acquire);
        if(owner_changed){stage.store(PulseStage::owner_lost);phase.store(PulsePhase::owner_lost,std::memory_order_release);return;}
        if(!key.binding||!key.handle||!key.serial)return;
        const bool cleanup=current==PulsePhase::reset_pending;
        if(cleanup){
            if(key.binding!=binding.load()||key.handle!=handle.load()||key.serial<=true_serial.load()||
               key.serial==last_reset_attempt.load())return;
            last_reset_attempt.store(key.serial);
        }else {
            if(current!=PulsePhase::armed||!eligible||cancelled||now<started||now-started>=250||
               key.serial==preflight_serial.load())return;
            preflight_serial.store(key.serial);
        }
        // Resolution and qualification are intentionally repeated on cleanup;
        // the saved binding/handle are equality keys, never invocation targets.
        std::uintptr_t property{};
        stage.store(PulseStage::qualify);if(!qualify(cleanup))return;
        stage.store(PulseStage::resolve);if(!resolve(property)||!property)return;
        stage.store(PulseStage::qualify);if(!qualify(cleanup))return;
        std::uint8_t prior{};getters.fetch_add(1,std::memory_order_relaxed);
        stage.store(PulseStage::prior_get);
        if(!get(property,key.binding,prior)||prior>1)return;
        if(!cleanup){
            prior_available.store(true);prior_false.store(prior==0);
            if(prior){stage.store(PulseStage::prior_true);phase.store(PulsePhase::preexisting_true,std::memory_order_release);return;}
        }else if(!prior){
            reset_serial.store(key.serial);reset_ms.store(now);native_cleared.store(true);
            reset_readback_available.store(true);reset_false.store(true);
            stage.store(PulseStage::reset_verified);
            phase.store(PulsePhase::reset,std::memory_order_release);return;
        }
        if(!qualify(cleanup))return;
        stage.store(PulseStage::entry);
        if(!cleanup){
            binding.store(key.binding);handle.store(key.handle);true_serial.store(key.serial);true_ms.store(now);
            // Publish the cleanup obligation BEFORE entering a call that could
            // complete its write and then fault. Such an entry is never retried.
            phase.store(PulsePhase::reset_pending,std::memory_order_release);
            true_calls.fetch_add(1,std::memory_order_relaxed);
        }else reset_calls.fetch_add(1,std::memory_order_relaxed);
        const bool returned=set(property,key.binding,cleanup?std::uint8_t{0}:std::uint8_t{1});
        if(cleanup)reset_returned.store(returned);else true_returned.store(returned);
        if(!returned)faulted.store(true);
        if(!qualify(cleanup))return;
        std::uint8_t after{};getters.fetch_add(1,std::memory_order_relaxed);
        stage.store(PulseStage::readback);
        const bool available=get(property,key.binding,after)&&after<=1;
        if(!cleanup){
            true_readback_available.store(available);true_readback.store(available&&after==1);
            written.store(returned&&available&&after==1);
        }else {
            reset_readback_available.store(available);reset_false.store(available&&after==0);
            if(returned&&available&&after==0){
                reset_serial.store(key.serial);reset_ms.store(now);
                stage.store(PulseStage::reset_verified);
                phase.store(PulsePhase::reset,std::memory_order_release);
            }
        }
    }
};
inline constexpr bool admit_callback(bool passive,std::uint64_t passive_started,std::uint64_t now,
    bool active,bool cleanup) noexcept {
    return active||cleanup||(passive&&now>=passive_started&&now-passive_started<200000);
}
template<class Apply> bool apply_available(bool available,Apply&& apply) noexcept {
    if(!available)return false;
    apply();return true;
}
}
