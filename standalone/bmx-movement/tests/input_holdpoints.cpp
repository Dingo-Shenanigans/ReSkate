#include <bmx/detail/source/bmx_input.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>

namespace {
using namespace bmx::detail::source;
unsigned checks{};
void check(bool value,std::source_location location=std::source_location::current()) {
    ++checks;
    if (!value) { std::cerr << "input holdpoint assertion " << checks << " failed at line "<<location.line()<<'\n'; std::exit(1); }
}
bool near(double a, double b, double epsilon=1e-12) { return std::abs(a-b)<epsilon; }
struct Fixture {
    InputSettings settings;
    InputMapper mapper;
    RawInput raw;
    InputContext context;
    explicit Fixture(bool observed=true, bool authored_deadzone=false, bool flick=false)
        : settings(), mapper([&] {
            settings.observed_balance_holdpoints=observed;
            settings.observed_flick_hop=flick;
            if(flick){settings.observed_flick_strength=4.5/6.4;
                settings.observed_flick_preload_threshold=.6;settings.observed_flick_forward_threshold=.45;settings.observed_flick_seconds=.2;}
            settings.deadzone=authored_deadzone ? .125 : 0;
            settings.maximum_magnitude=authored_deadzone ? .925 : 1;
            return settings;
        }()) {
        raw.available=raw.focused=true; raw.device=1;
        check(mapper.update(raw,context,.01).active);
    }
    Actions tick(double y, unsigned count=1, double x=0, double dt=.01) {
        Actions out; raw.right_y=y;raw.right_x=x;
        for (unsigned i=0;i<count;++i) out=mapper.update(raw,context,dt);
        return out;
    }
};
}
int main() {
    // Source-bound .125/.925 processing and 127-step stage quantization explain
    // the recorded .4 HID request -> .346-ish processed / .44094488 hold output.
    Fixture authored(true,true);
    auto a=authored.tick(-.4,30);
    check(near(a.preload,-44./127)); check(near(a.manual,56./127)); check(a.nose_manual==0);
    check(near(authored.tick(-.925).preload,-1));
    check(near(authored.tick(-1).preload,-1));
    check(near(authored.tick(0).preload,0));

    // The first two calls retain nonzero two-sample input velocity. Steady hold
    // time then differs by hand; a brief step never enters a balance state.
    Fixture manny;
    check(manny.tick(-.4,2).manual==0);
    check(manny.tick(-.4,18).manual==0);
    check(near(manny.tick(-.4,4).manual,64./127));
    Fixture nosey;
    check(nosey.tick(.4,2).nose_manual==0);
    check(nosey.tick(.4,8).nose_manual==0);
    check(near(nosey.tick(.4,4).nose_manual,64./127));
    check(nosey.tick(.4).manual==0);

    // Entry band and latched cancellation are different. Full travel preserves
    // an already latched state but cannot initially enter it.
    check(near(manny.tick(-1).manual,1));
    check(manny.tick(-.2).manual==0);
    check(manny.tick(-1,40).manual==0);
    check(manny.tick(-.81,40).manual==0);
    check(manny.tick(-.19,40).manual==0);
    check(near(manny.tick(-.21,30).manual,39./127));
    check(manny.tick(.4).manual==0);
    check(near(manny.tick(.4,20).nose_manual,64./127));

    // Conflicting horizontal travel blocks entry, whereas an existing latch
    // follows the source Y-only cancellation condition.
    Fixture conflict;
    check(conflict.tick(-.4,40,.7).manual==0);
    check(near(conflict.tick(-.4,30,0).manual,64./127));
    check(near(conflict.tick(-.4,5,.7).manual,64./127));
    conflict.tick(0);
    check(conflict.tick(-.4,40,.7).manual==0);

    // A changing input above the strict source velocity limit cannot accumulate
    // the initial steady hold. Stable input subsequently does.
    Fixture moving;
    for (unsigned i=0;i<40;++i) check(moving.tick(-(i%4<2?.25:.65)).manual==0);
    check(near(moving.tick(-.4,30).manual,64./127));

    // Full signed preload and R3 remain independent; the optional mapper does
    // not synthesize a hop from a right-stick flick.
    Fixture full;
    for (double y:{-1.,1.}) {
        a=full.tick(y,40);check(a.manual==0&&a.nose_manual==0&&!a.hop_pressed);check(a.preload==y);
    }
    full.raw.quick_hop=true;a=full.tick(0);check(a.hop_pressed&&a.hop_strength==1&&a.preload==0);
    check(!full.tick(0).hop_pressed);
    full.context.grounded=false;check(!full.tick(0).hop_pressed);
    full.context.grounded=true;check(!full.tick(0).hop_pressed);

    // The observed balance mapping can independently opt into a back/forward
    // Jhop. It must not replace signed preload or the source holdpoint outputs.
    Fixture flick(true,true,true);unsigned flick_events=0;
    for(unsigned tick=0;tick<40;++tick){a=flick.tick(-1);check(!a.hop_pressed&&a.preload==-1&&a.manual==0);}
    for(unsigned tick=0;tick<10;++tick){a=flick.tick(1);flick_events+=a.hop_pressed;
        check(a.preload==1&&a.nose_manual==0);if(a.hop_pressed)check(near(a.hop_strength,4.5/6.4));}
    check(flick_events==1);
    // Neither partial holdpoint input reaches a full-back preload threshold.
    Fixture partial(true,true,true);
    for(double y:{-.4,.4}){for(unsigned tick=0;tick<40;++tick){a=partial.tick(y);check(!a.hop_pressed);}
        check(near(a.preload,(y<0?-44.:44.)/127));check(near(y<0?a.manual:a.nose_manual,56./127));}
    // Native Jhop arms immediately: a minimum charge-time gate is not added.
    Fixture short_flick(true,true,true);check(!short_flick.tick(-1).hop_pressed);
    a=short_flick.tick(1);check(a.hop_pressed&&a.hop_kind==HopKind::ollie&&near(a.hop_strength,4.5/6.4));
    check(!short_flick.tick(1,40).hop_pressed);
    Fixture strict(true,false,true);check(!strict.tick(-.6).hop_pressed);check(!strict.tick(1).hop_pressed);
    Fixture strict_cross(true,false,true);strict_cross.tick(-.60001);
    check(!strict_cross.tick(.45).hop_pressed);check(strict_cross.tick(.45001).hop_pressed);
    Fixture paused(true,false,true);paused.tick(-1);paused.tick(0,10);paused.tick(-1,40);
    check(paused.tick(1).hop_pressed); // Time on preload side does not consume the release window.
    Fixture expired(true,false,true);expired.tick(-1);expired.tick(0,21);check(!expired.tick(1).hop_pressed);
    Fixture repeat(true,false,true);repeat.tick(-1);check(repeat.tick(.5).hop_pressed);
    repeat.tick(0);repeat.tick(-1);check(!repeat.tick(.5).hop_pressed); // Native per-instance .2s repeat interval.
    repeat.tick(0,30);repeat.tick(-1);check(repeat.tick(.5).hop_pressed);
    Fixture nollie(true,true,true);nollie.tick(1,40);a=nollie.tick(-1);
    check(a.hop_pressed&&a.hop_kind==HopKind::nollie&&near(a.hop_strength,4.5/6.4));
    check(!nollie.tick(-1,40).hop_pressed);
    Fixture cross_cancel(true,false,true);cross_cancel.tick(-1);a=cross_cancel.tick(1);
    check(a.hop_kind==HopKind::ollie);check(!cross_cancel.tick(.10001).hop_pressed);
    check(!cross_cancel.tick(.1).hop_pressed);a=cross_cancel.tick(-.46);
    check(a.hop_pressed&&a.hop_kind==HopKind::nollie); // Opposite instance has an independent cooldown.
    Fixture diagonal(true,false,true);diagonal.tick(0,1,.8);diagonal.tick(-.8,1,.8);
    check(diagonal.tick(.8,1,.8).hop_pressed); // Source consumes processedY, without a legacy lateral gate.
    Fixture positive(true,true,true);check(!positive.tick(1,40).hop_pressed);positive.tick(0);
    check(!positive.tick(1).hop_pressed);
    Fixture held_manny(true,true,true);a=held_manny.tick(-.4,40);
    check(a.manual>0&&!a.flick_preloading);check(held_manny.tick(1).hop_pressed);
    // A sustained neutral release expires the pending gesture; focus, menu,
    // device, airborne context and explicit reset also cancel it safely.
    for(unsigned cancel=0;cancel<6;++cancel){Fixture canceled(true,true,true);canceled.tick(-1,30);
        switch(cancel){
        case 0:check(!canceled.tick(0,40).hop_pressed);break;
        case 1:canceled.raw.focused=false;canceled.tick(-1);canceled.raw.focused=true;break;
        case 2:canceled.raw.menu_open=true;canceled.tick(-1);canceled.raw.menu_open=false;break;
        case 3:++canceled.raw.device;canceled.tick(-1);break;
        case 4:canceled.context.grounded=false;canceled.tick(-1);canceled.context.grounded=true;break;
        case 5:canceled.mapper.reset();break;}
        check(!canceled.tick(1).hop_pressed);check(canceled.tick(0).active);check(!canceled.tick(1).hop_pressed);}
    // R3 supersedes a same-frame flick, cancels its pending state, and cannot
    // retrigger as the held button crosses ground/air context boundaries.
    Fixture combined(true,true,true);combined.tick(-1,40);combined.raw.quick_hop=true;
    a=combined.tick(1);check(a.hop_pressed&&a.hop_kind==HopKind::quick_hop&&a.hop_strength==1&&a.preload==0);
    check(!combined.tick(1,40).hop_pressed);combined.raw.quick_hop=false;check(!combined.tick(1).hop_pressed);
    combined.tick(0,30);combined.tick(-1);check(combined.tick(1).hop_pressed);
    check(!InputSettings{}.observed_flick_hop);

    // Context, ownership, invalid-time and focus changes erase held state and
    // retain the existing all-controls-neutral ownership gate.
    Fixture reset;
    check(reset.tick(-.4,30).manual>0);
    reset.raw.focused=false;check(!reset.tick(-.4).active);
    reset.raw.focused=true;check(!reset.tick(-.4,30).active);
    check(reset.tick(0).active);check(reset.tick(-.4,2).manual==0);
    check(reset.tick(-.4,30).manual>0);
    reset.context.grounded=false;reset.tick(0);reset.context.grounded=true;
    check(reset.tick(-.4,2).manual==0);
    reset.raw.device=2;check(!reset.tick(-.4).active);check(reset.tick(0).active);
    check(reset.tick(-.4,30).manual>0);
    check(!reset.tick(-.4,1,0,std::numeric_limits<double>::quiet_NaN()).active);
    check(!reset.tick(-.4,30).active);check(reset.tick(0).active);

    // Optional settings leave default legacy full-back/flick behavior intact.
    Fixture legacy(false);
    check(legacy.tick(-1,10).preload>0);
    check(legacy.tick(1).hop_pressed);
    check(!InputSettings{}.observed_balance_holdpoints);
    check(InputSettings{}.maximum_magnitude==1);
    InputSettings invalid;invalid.deadzone=.4;invalid.maximum_magnitude=.1;
    InputMapper sanitized(invalid);check(sanitized.settings().maximum_magnitude>=.45);
    invalid.maximum_magnitude=std::numeric_limits<double>::quiet_NaN();sanitized.configure(invalid);
    check(sanitized.settings().maximum_magnitude==1);
    std::cout << checks << " input holdpoint assertions passed\n";
}
