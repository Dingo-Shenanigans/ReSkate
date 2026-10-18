#pragma once
#include <optional>
#include <cstdint>
namespace jishsfirstpersonmod {
inline bool native_camera_cycle_choice(bool enabled,bool observed,bool previous,bool current,bool ready,
    bool queued,bool suppressed,bool game_owned) {
    return enabled && observed && previous!=current && ready && !queued && !suppressed && !game_owned;
}
struct PlayerState {
    std::uintptr_t identity{};
    bool valid{}, board_verified{}, bail_verified{}, riding{}, bailing{};
    std::uint32_t physics{};
    bool animation_valid{},wipeout_request{},animation_wipeout{},recovery{};
    bool selector_valid{};
    std::uint32_t selector_current{},selector_next{};
    bool normal_foot{},wipeout_event{};
    std::uintptr_t client{},entity{},context{},rig{};
};
struct BailEvidence {
    std::uintptr_t identity{};
    bool valid{},riding{},normal_foot{},signals_valid{},wipeout{},recovery{};
};
struct BailLatch {
    std::uintptr_t identity{};
    bool active{};
    unsigned normal_samples{};
    std::optional<bool> update(const BailEvidence& evidence) {
        if(!evidence.valid) { identity=0;active=false;normal_samples=0;return {}; }
        if(identity!=evidence.identity) { identity=evidence.identity;active=false;normal_samples=0; }
        if(evidence.wipeout) { active=true;normal_samples=0; }
        else if(evidence.signals_valid && !evidence.recovery && (evidence.riding || evidence.normal_foot)) {
            if(active && ++normal_samples>=2) active=false;
        } else normal_samples=0;
        if(active) return true;
        if(evidence.signals_valid && (evidence.riding || evidence.normal_foot)) return false;
        return {};
    }
};
struct ComfortPolicy {
    bool initialized{}, master{};
    void observe_native(bool active) { if(!initialized) { initialized=true; master=active; } }
    void request(bool enabled) { initialized=true; master=enabled; }
    std::optional<bool> desired(const PlayerState& state,bool bail,bool board,bool recovery=false) const {
        if(!initialized) return {};
        if(!master) return false;
        if(recovery) return false;
        if(!bail && !board) return true;
        if(!state.valid || (bail && !state.bail_verified) || (board && !state.board_verified)) return {};
        return !(bail && state.bailing) && !(board && !state.riding);
    }
};
struct RecoveryCameraLatch {
    std::uintptr_t identity{};bool active{};
    bool update(bool enabled,std::uintptr_t owner,bool valid,bool bail,bool getting_up,bool controlled,bool normal,bool renewed) {
        if(!enabled || !valid) { *this={};return false; }
        if(identity!=owner) { *this={};identity=owner; }
        if(renewed) { active=false;return false; }
        if(controlled || normal) active=false;
        else if(bail && getting_up) active=true;
        return active;
    }
};
struct ComfortSnapshot {
    bool master{},initialized{},board_available{},bail_available{},state_valid{},suppressed{};
    std::uint32_t physics{};
    bool animation_valid{},wipeout_request{},animation_wipeout{},recovery{},selector_valid{};
    std::uint32_t selector_current{},selector_next{};
    bool bailing{},riding{},suspended{};
    bool normal_foot{};
    std::uintptr_t core{},client{},entity{},context{},rig{};
};
ComfortSnapshot comfort_snapshot();
void comfort_user_request(bool enabled);
bool comfort_requested_view(bool master);
void comfort_observe_native(bool active);
void update_comfort(std::uintptr_t client,std::uintptr_t base);
void clear_comfort_state();
void install_comfort_observer(std::uintptr_t base);
}
