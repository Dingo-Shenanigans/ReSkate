#pragma once
#include "controls_context.h"
#include "Upstream/src/mod.h"
#include "Upstream/src/camera_transition.h"
#include "Upstream/src/camera_policy.h"
#include "Extension/UI/Overlay/gameplay_controller.h"
#include <algorithm>
namespace dingosdk::first_person_integration {
namespace fpmod = jishsfirstpersonmod;
struct StickCapture {
    std::uint32_t modifier{}, xinput_sources{}, watch_modifier{};
    bool keep_native_input{};
    bool sony{};
    std::int16_t x{}, y{};
    std::uint64_t published{};
    bool matches(std::uint32_t buttons, std::uint64_t now) const noexcept {
        return modifier && now >= published && now-published <= 100 &&
            (modifier == ~0u || (buttons & modifier) == modifier);
    }
    bool apply(unsigned slot, overlay::GamepadValues& pad, std::uint64_t now) const noexcept {
        if (slot>=4 || !(xinput_sources & (1u<<slot))) return false;
        std::uint32_t buttons=pad.buttons;
        if(pad.left_trigger>30) buttons|=0x10000;
        if(pad.right_trigger>30) buttons|=0x20000;
        if(!matches(buttons,now)) return false;
        pad.right_x=x;pad.right_y=y;return true;
    }
};
inline bool equal_pad(const overlay::GamepadValues& a,const overlay::GamepadValues& b) noexcept {
    return a.buttons==b.buttons && a.left_trigger==b.left_trigger && a.right_trigger==b.right_trigger &&
        a.left_x==b.left_x && a.left_y==b.left_y && a.right_x==b.right_x && a.right_y==b.right_y;
}
// Latch at the game-facing input edge, before a later camera tick can see a centered stick.
struct NativeStickHold {
    std::uint32_t modifier{};bool held{};
    std::int16_t x{},y{};
    StickCapture sample(StickCapture capture,const overlay::GamepadValues& raw,std::uint64_t now) {
        if(modifier!=capture.watch_modifier || now<capture.published || now-capture.published>100) {
            *this={};modifier=capture.watch_modifier;
        }
        std::uint32_t buttons=raw.buttons;
        if(raw.left_trigger>30)buttons|=0x10000;
        if(raw.right_trigger>30)buttons|=0x20000;
        const bool down=modifier && (buttons&modifier)==modifier;
        if(down && !held) {x=raw.right_x;y=raw.right_y;}
        held=down;
        if(capture.keep_native_input && modifier) {capture.x=x;capture.y=y;}
        return capture;
    }
};
// Change a game-facing packet when the filtered output changes even if hardware
// is stationary. Physical axes are returned directly as soon as capture releases.
struct PacketSequence {
    bool ready{};std::uint32_t physical{}, packet{};overlay::GamepadValues previous;
    std::uint32_t update(std::uint32_t current,const overlay::GamepadValues& raw,
                         const overlay::GamepadValues& filtered) noexcept {
        if(!ready) {
            if(equal_pad(raw,filtered)) return current;
            ready=true;packet=current+1;
        } else if(physical!=current || !equal_pad(previous,filtered)) ++packet;
        physical=current;previous=filtered;return packet;
    }
};
class RightStickRuntime {
    fpmod::BoardLookState board_;
    fpmod::StickLookState foot_;
    fpmod::StickAim aim_;
    ControlsContext context_;
    StickCapture capture_;
    bool heading_ready_{}, foot_mode_{}, turn_{}, recenter_handoff_{};
    fpmod::CameraTransition transition_;
    float heading_{}, pitch_{};
public:
    void reset() { board_.reset();foot_.reset();aim_={};context_={};capture_={};heading_ready_=foot_mode_=turn_=recenter_handoff_=false;transition_.reset(); }
    const StickCapture& capture() const { return capture_; }
    const fpmod::StickAim& aim() const { return aim_; }
    bool turning() const { return turn_ && aim_.active && context_.verified && context_.on_foot && context_.eligible; }
    void step(const fpmod::Config& config,const ControlsContext& context,const overlay::GameplayController& controller) {
        const auto& input=controller.input;
        if(!context.verified || !input.available || !input.device) {
            if(context.telemetry_valid && context.bail && context.owner[0] && context.owner==context_.owner && input.available) {
                recenter_handoff_=true;foot_.reset();board_.reset();aim_={};capture_={};turn_=false;context_=context;
            } else reset();
            return;
        }
        if(context.owner!=context_.owner) reset();
        const bool changed_mode=context_.verified && context.on_foot!=context_.on_foot;
        context_=context;
        if(context.bail || !context.on_foot) recenter_handoff_=true;
        if(!context.eligible) { foot_.reset();board_.reset();aim_={};capture_={};turn_=false;return; }
        const fpmod::StickAxes axes{fpmod::normalize_axis(controller.pad.right_x),fpmod::normalize_axis(controller.pad.right_y)};
        const first_person_vendor::ControllerInput vendor{input.available,input.buttons,input.device,
            static_cast<first_person_vendor::ControllerStyle>(input.style)};
        capture_={};
        if(!context.on_foot) {
            foot_.reset();foot_mode_=false;turn_=false;
            aim_=board_.update(config.board,axes,true,vendor,context.time);
            capture_.watch_modifier=config.board.enabled ? config.board.modifier:0;
            capture_.keep_native_input=config.board.keep_native_input;
            if(aim_.capture) {
                const auto native=board_.native_axes(config.board);
                capture_.modifier=config.board.modifier;capture_.x=fpmod::stick_short(native.x);capture_.y=fpmod::stick_short(native.y);
            }
        } else {
            board_.reset();
            const bool turn=!config.look_only && config.stick.turn_character && context.turn_allowed && context.motion_ready;
            if(turn && recenter_handoff_) {foot_.recenter_facing();recenter_handoff_=false;}
            if(turn && !turn_ && heading_ready_ && !changed_mode) foot_.rebase_facing(fpmod::matrix_heading(context.body));
            turn_=turn;foot_mode_=true;
            auto settings=config.stick;settings.turn_character=turn;
            aim_=foot_.update(settings,axes,heading_ready_,input.device,context.time,heading_,
                fpmod::matrix_heading(context.body),config.stick_spring,pitch_);
            if(aim_.capture) capture_.modifier=~0u;
        }
        capture_.xinput_sources=input.device<=15 ? input.device:0;
        capture_.sony=input.style!=ControllerStyle::xbox;
        capture_.published=context.tick;
    }
    first_person::Matrix compose(first_person::Matrix pose,std::uint64_t now) {
        if(!context_.verified || now<context_.tick || now-context_.tick>100) return pose;
        if(aim_.active) pose=foot_mode_ ? fpmod::aim_camera(pose,aim_):fpmod::aim_head_relative(pose,aim_);
        const auto back=first_person_vendor::first_person::rotate(first_person_vendor::first_person::orientation(pose),{0,0,1});
        heading_=std::atan2(back[0],back[2])*180/std::numbers::pi_v<float>;
        pitch_=std::asin(std::clamp(-back[1],-1.f,1.f))*180/std::numbers::pi_v<float>;
        heading_ready_=true;return pose;
    }
    first_person::Matrix transition(first_person::Matrix pose,const fpmod::Config& config,double now) {
        if(!context_.verified) {transition_.reset();return pose;}
        first_person_vendor::first_person::Settings handoff;handoff.enabled=config.stick.smooth_transitions && !config.look_only;
        if(context_.bail || context_.teleport) {transition_.reset();return pose;}
        const bool mounting=context_.mounting;
        handoff.up=handoff.down=handoff.left=handoff.right=(mounting || !context_.on_foot) ?
            config.stick.mount_smoothing:config.stick.dismount_smoothing;
        return transition_.apply(pose,handoff,context_.owner[0],context_.on_foot ? 1:2,false,now,
            {},nullptr,nullptr,false,true,false,mounting,true);
    }
    bool motion(std::uintptr_t rig,std::uintptr_t context,first_person::Matrix& pose,std::uint64_t now) const {
        if(!turn_ || !aim_.active || !context_.verified || !context_.on_foot || !context_.turn_allowed ||
            context_.owner[3]!=rig || context_.owner[2]!=context || now<context_.tick || now-context_.tick>100) return false;
        pose=fpmod::face_heading(pose,aim_.body_heading);return true;
    }
};
}
