#pragma once
#include <cstdint>
namespace dingosdk::bmx {
enum class BikeInteraction { none,tap,hold };
// One North/Y interaction per physical press. A long hold never also generates
// a tap, and focus/menu/disconnect requires a new neutral sample before rearming.
class BikeInteractionButton {
    bool armed_{},pressed_{},consumed_{};
    std::uint64_t down_at_{},last_{};
public:
    void reset() noexcept {*this={};}
    BikeInteraction update(bool down,bool allowed,std::uint64_t now) noexcept {
        if(!allowed||(last_&&now<last_)) {reset();return BikeInteraction::none;}
        last_=now;
        if(!down) {
            const bool tap=pressed_&&!consumed_;
            pressed_=consumed_=false;armed_=true;
            return tap?BikeInteraction::tap:BikeInteraction::none;
        }
        if(!pressed_) {
            if(!armed_)return BikeInteraction::none;
            pressed_=true;consumed_=false;armed_=false;down_at_=now;
        }
        if(!consumed_&&now-down_at_>=500) {consumed_=true;return BikeInteraction::hold;}
        return BikeInteraction::none;
    }
};

// The equipment shortcut claims one fresh North press while L1/LB is held.
// It never converts an existing tap/carry hold into a switch. Owner checks are
// observations only; the existing equipment lifecycle still admits the action.
struct BikeQuickSwitchOwner {
    std::uintptr_t client{},entity{},core{},context{},rig{};
    bool operator==(const BikeQuickSwitchOwner&) const=default;
    bool valid() const noexcept {return client&&entity&&core&&context&&rig;}
};
struct BikeQuickSwitchEvent {
    bool pressed{},suppress_north{},capture_north{};
};
class BikeQuickSwitchButton {
    bool armed_{},north_was_down_{},claimed_{},clock_seen_{};
    std::uint32_t device_{};
    std::uint64_t last_{};
    BikeQuickSwitchOwner owner_{};
public:
    void reset() noexcept {*this={};}
    bool owner_sample_needed(bool north,bool shoulder) const noexcept {
        return (claimed_&&(north||shoulder))||(armed_&&!north_was_down_&&north&&shoulder);
    }
    BikeQuickSwitchEvent update(bool north,bool shoulder,bool allowed,std::uint32_t device,
        bool owner_observed,BikeQuickSwitchOwner owner,std::uint64_t now) noexcept {
        if(!allowed||!device) {reset();return {};}
        if(device!=device_||(clock_seen_&&now<last_)) {
            const bool quarantine=claimed_&&(north||shoulder);
            reset();device_=device;claimed_=quarantine;
        }
        last_=now;clock_seen_=true;
        // A fresh chord on an invalidated/unarmed source still belongs to
        // this shortcut, not to the separately armed ordinary Y tap/hold.
        const bool chord_edge=!north_was_down_&&north&&shoulder;
        const bool fresh_chord=armed_&&chord_edge;
        if(owner_observed&&(!owner.valid()||(owner_.valid()&&owner_!=owner))) {
            // A failed/changed owner can consume this deliberate chord but
            // cannot defer it until a later owner becomes available.
            claimed_=claimed_||chord_edge;armed_=false;north_was_down_=north;
            owner_=owner.valid()?owner:BikeQuickSwitchOwner{};
            return {false,claimed_,claimed_};
        }
        if(owner_observed)owner_=owner;
        if(!north&&!shoulder) {
            const bool release_capture=claimed_;
            claimed_=false;north_was_down_=false;armed_=true;
            // Keep North filtered on the observed release tick. The next
            // fresh neutral sample releases this independent expiring lease.
            return {false,false,release_capture};
        }
        north_was_down_=north;
        const bool pressed=fresh_chord&&owner_observed&&owner.valid();
        if(chord_edge) {claimed_=true;armed_=false;}
        if(claimed_)return {pressed,true,true};
        // Reserve North while the deliberate modifier is held, before the
        // next device poll can expose the chord's North edge to native Skate.
        return {false,false,armed_&&shoulder&&!north};
    }
};
enum class QuickBikeAction { none,skateboard,nearby_bike,equip_bike };
inline QuickBikeAction quick_bike_action(bool active,bool pending,bool mounting,
                                       bool parked,bool crashed) noexcept {
    if(pending||mounting)return QuickBikeAction::none;
    if(active)return QuickBikeAction::skateboard;
    if(parked||crashed)return QuickBikeAction::nearby_bike;
    return QuickBikeAction::equip_bike;
}
}
