#include "Extension/Console/commands.h"
#include "bmx_runtime.h"
#include "bmx_skeleton_phase_probe.h"
#include "bmx_board_handoff_trace.h"
#include "bmx_chassis_probe.h"
#include "bmx_chassis_ride.h"
#include "bmx_ramp_test.h"
#include "bmx_world.h"
#include "bmx_spawn_diagnostics.h"
#include "bmx_prepare_diagnostics.h"
#include "bmx_player_startup.h"
#include "bmx_local_profile.h"
#include "bmx_private_input_status_export.h"
#include "Extension/Boot/offline_boot.h"
#include "Extension/Objects/local_object_runtime.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/UI/Overlay/input_capture.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include <Windows.h>

namespace dingosdk::console {
void register_bmx_commands(Commands& registry) {
    auto status = action("bmx status", "Show BMX development status", Group::movement);
    status.aliases = {"bmx"};
    status.execution = Execution::local;
    status.run = [](const Model&, const Values&, const Output& out) {
        out(bmx::status());
        out("Bike visual: eight articulated native world models, including separate left and right pedals.");
        out(bmx::chassis_ride::enabled() ?
            "Physical chassis: independent native body, tire forces, balance assistance and measured full rotation; calibrated at 60 Hz." :
            "Legacy prototype: solo on-foot movement, two wheel ground rays, obstacle ray fan; speed capped at 6 m/s.");
        out("The bike uses native world rendering. BMX trick tallies are separate from Skate scoring; grinds and multiplayer are unfinished.");
    };
    registry.add(std::move(status));
    auto handoff_arm=action("bmx handoff-trace arm", "Arm the explicitly selected native-only passive observer; does not mount equipment", Group::movement);
    handoff_arm.execution=Execution::local;
    handoff_arm.run=[](const Model&,const Values&,const Output& out) {
        out(bmx::board_handoff_trace::request_native_control_arm()?
            "Private native-only observation queued; ordinary Y/Triangle mount is a separate active control action.":
            "Native-only handoff observation is disabled, already used, or unavailable.");
    };
    registry.add(std::move(handoff_arm));
    auto handoff_native=action("bmx handoff-trace native-board", "Queue the separately selected ACTIVE one-shot software native-board control; no controller input", Group::movement);
    handoff_native.execution=Execution::local;
    handoff_native.run=[](const Model&,const Values&,const Output& out) {
        out(bmx::board_handoff_trace::request_native_control_arm(true)?
            "ACTIVE NativeOnly software board request queued after passive untouched standing baseline; not ordinary Y or hardware.":
            "ACTIVE NativeOnly board control disabled, already used, or unavailable.");
    };
    registry.add(std::move(handoff_native));
    auto handoff_input=action("bmx handoff-trace native-input", "Private XInput slot0 North experiment plus passive observer; no selector request", Group::movement);
    handoff_input.execution=Execution::local;
    handoff_input.run=[](const Model&,const Values&,const Output& out) {
        const bool queued=overlay::detail::queue_private_native_board_input();
        if(!queued){out("Private input bridge refused. Requires its launch opt-in, untouched passive NativeOnly/deferred profile and fresh on-foot owner.");return;}
        if(!bmx::board_handoff_trace::request_native_control_arm()) {
            overlay::detail::cancel_private_native_board_input();out("Private input bridge canceled: passive observer could not arm.");return;
        }
        out("Private XInput slot0 bridge queued. Close the console with sticks/buttons released; one bounded North pulse follows the armed baseline. No native consumption is claimed.");
    };
    registry.add(std::move(handoff_input));
    auto handoff_input_status=action("bmx handoff-trace input-status", "Read the private game-facing input report ledger; no input action", Group::movement);
    handoff_input_status.execution=Execution::local;
    handoff_input_status.run=[](const Model&,const Values&,const Output& out) {out(overlay::detail::private_native_board_input_status());};
    registry.add(std::move(handoff_input_status));
    auto handoff_input_export=action("bmx handoff-trace input-export", "Save the full private game-facing input ledger without console truncation", Group::movement);
    handoff_input_export.execution=Execution::local;
    handoff_input_export.run=[](const Model&,const Values&,const Output& out) {
        bmx::board_handoff_trace::ErrorGuard error;
        namespace trace=bmx::board_handoff_trace;
        namespace export_status=bmx::private_input_status_export;
        try {
            // Read-only selection plus fresh filesystem validation at the
            // command boundary. No owner read, input action or hook file I/O.
            if(!trace::selected()||trace::store.mode!=trace::Mode::native_only||
               trace::store.automated_native_board||!trace::store.deferred_native_board_arm||
               !trace::flag(L"RESKATE_BMX_NATIVE_BOARD_INPUT_BRIDGE")||!trace::flag(L"RESKATE_BMX_NATIVE_CHASSIS")){
                out("Private input export refused: qualified passive NativeOnly input-bridge profile required.");return;
            }
            wchar_t mode[16]{};const auto length=GetEnvironmentVariableW(L"RESKATE_BMX_LAUNCH_MODE",mode,16);
            if(length!=10||std::wstring_view(mode,length)!=L"human-ride"){
                out("Private input export refused: human-ride launch mode required.");return;
            }
            const auto text=overlay::detail::private_native_board_input_status();
            const auto root=user_data::override_root();
            if(!trace::selected()||!root||root->native()!=trace::private_root||root->native()!=trace::store.root.native()){
                out("Private input export refused: private root changed or unavailable.");return;
            }
            static std::atomic<unsigned> attempts{};const auto sequence=export_status::reserve(attempts);
            if(!sequence){out("Private input export refused: eight-attempt process limit reached.");return;}
            const auto result=export_status::write(*root,text,GetCurrentProcessId(),trace::store.creation,sequence);
            if(!result.saved){out(std::string("Private input export failed: ")+result.issue+".");return;}
            out("Saved private input status: "+result.path.string()+" ("+std::to_string(result.bytes)+" bytes). Snapshot only; input_consumed remains null.");
        }catch(...){out("Private input export failed: snapshot or filesystem exception.");}
    };
    registry.add(std::move(handoff_input_export));
    auto handoff_status=action("bmx handoff-trace status", "Read the selected observer arm/stop flags; does not mount equipment", Group::movement);
    handoff_status.execution=Execution::local;
    handoff_status.run=[](const Model&,const Values&,const Output& out) {
        const auto& trace=bmx::board_handoff_trace::store;
        out(std::string("Private handoff observer: enabled=")+(trace.enabled.load()?"1":"0")+
            " armed="+(trace.armed.load()?"1":"0")+" sealed="+(trace.sealed.load()?"1":"0")+
            " stop="+trace.stop.load()+". This status issues no native action.");
    };
    registry.add(std::move(handoff_status));
    auto phase_dump=action("bmx phase-dump", "Save the explicitly enabled private SkeletonResponse ordering diagnostic", Group::movement);
    phase_dump.execution=Execution::local;
    phase_dump.run=[](const Model&,const Values&,const Output& out) {
        out(bmx::skeleton_phase_probe::request_report()?"Private phase report queued for the next idle client checkpoint.":
            "Phase probe is disabled; requires its explicit private lab flag and profile.");
    };
    registry.add(std::move(phase_dump));
    auto diagnose = action("bmx diagnose", "Show native loading and rider readiness for BMX", Group::movement);
    diagnose.execution = Execution::local;
    diagnose.run = [](const Model& model, const Values&, const Output& out) {
        out(offline_boot_observation().json);
        out(profile_runtime::bmx_flow_diagnostics());
        out(bmx::source_status());
        out(bmx::spawn_diagnostics(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))));
        out(bmx::prepare_diagnostics());
        out(bmx::player_startup_status());
        out(bmx::local_profile_status());
        const auto input = overlay::read_bmx_input();
        const auto ui = multiplayer::read_game_ui_state(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
        out("BMX input readiness: overlay=" + std::to_string(input.overlay_ready) +
            "; focused=" + std::to_string(input.focused) + "; input=" + std::to_string(input.available) +
            "; overlay_menu=" + std::to_string(input.menu_blocked) + "; other_capture=" + std::to_string(input.capture_blocked) +
            "; native_ui=" + std::to_string(ui.available) + "; native_menu=" + std::to_string(ui.in_menu));
        for (const auto* name : {"DingoTest.SpawnLocalSkater", "DingoTest.ClientNum", "DingoOnline.Enable"})
            out(std::string(name) + " = " + named_setting_value(name).value_or("unavailable"));
        out("Local skater position ready: " + std::string(model.debug.skater_position_valid ? "yes" : "no") +
            "; movement guard: " + model.debug.noclip_unavailable);
        for (const auto& level : model.levels) {
            if (level.asset.find("BAM_LevelRoot/BAM_LevelRoot") == std::string::npos &&
                level.asset.find("DingoLevel_MPR/DingoLevel_MPR") == std::string::npos) continue;
            out("Start points for " + level.asset + ":");
            for (const auto& start : level.start_points) out("  " + start);
        }
    };
    registry.add(std::move(diagnose));

    auto recover = action("bmx recover player", "Publish the authored initial-player event in an isolated development session", Group::movement);
    recover.execution = Execution::local;
    recover.run = [](const Model&, const Values&, const Output& out) {
        out(bmx::request_player_startup() ? "Initial-player recovery requested. Native guards and event metadata are checked before one publication; use bmx diagnose for the actual outcome." :
            "Recovery requires --dev-data-root and RESKATE_BMX_PLAYER_STARTUP_REQUEST=1. No event was requested.");
    };
    registry.add(std::move(recover));

    auto mount = action("bmx mount", "Mount the BMX prototype after stepping off the skateboard", Group::movement);
    mount.execution = Execution::local;
    mount.run = [](const Model&, const Values&, const Output& out) {
        bmx::request_mount(true);
        out("BMX mount queued. Close menus, stand on clear ground off the skateboard, then center both sticks.");
    };
    registry.add(std::move(mount));
    auto toggle = action("bmx toggle", "Switch between BMX and the native skateboard (F6)", Group::movement);
    toggle.execution = Execution::local;
    toggle.run = [](const Model&, const Values&, const Output& out) {
        bmx::request_toggle();
        out("Equipment switch queued. F6 also switches BMX/skateboard; close menus and stand on clear ground.");
    };
    registry.add(std::move(toggle));
    auto dismount = action("bmx dismount", "Dismount BMX and restore normal game controls", Group::movement);
    dismount.execution = Execution::local;
    dismount.run = [](const Model&, const Values&, const Output& out) {
        bmx::request_mount(false);
        out("BMX dismount queued.");
    };
    registry.add(std::move(dismount));
    auto interact = action("bmx interact", "Get on or off the nearby bike (F7 or tap Y/Triangle)", Group::movement);
    interact.execution = Execution::local;
    interact.run = [](const Model&,const Values&,const Output& out) {bmx::request_interaction();out("Bike interaction queued.");};
    registry.add(std::move(interact));
    auto carry = action("bmx carry", "Pick up or set down the nearby bike (F8 or hold Y/Triangle)", Group::movement);
    carry.execution = Execution::local;
    carry.run = [](const Model&,const Values&,const Output& out) {bmx::request_interaction(true);out("Bike carry interaction queued.");};
    registry.add(std::move(carry));
    auto quick_test=action("bmx quicktest", "Run the private virtual-source LB+North native switch rehearsal", Group::movement);
    quick_test.execution=Execution::local;
    quick_test.run=[](const Model&,const Values&,const Output& out) {
        out(bmx::request_quick_switch_test()?"Private synthetic quick-switch rehearsal queued; keep the test game focused and physical controls centered.":
            "Quick-switch rehearsal requires exact private SwitchTest+NativeChassis+QuickSwitchTest; no hardware packets are tested.");
    };
    registry.add(std::move(quick_test));
    auto getup_test=action("bmx getup-test", "Request one native GetUp intent in the separate private impact lab", Group::movement);
    getup_test.execution=Execution::local;
    getup_test.run=[](const Model&,const Values&,const Output& out) {
        out(bmx::request_getup_test()?"One private native GetUp test queued.":
            "GetUp test requires its separate explicit private impact lab; ordinary controls are unchanged.");
    };
    registry.add(std::move(getup_test));
    auto rehearsal = action("bmx selftest", "Run an isolated native BMX riding rehearsal", Group::movement);
    rehearsal.execution = Execution::local;
    rehearsal.run = [](const Model&, const Values&, const Output& out) {
        out(bmx::request_selftest() ? "BMX native rehearsal queued. Keep the game focused. It will pedal, steer, brake and hop, then dismount." :
            "BMX rehearsal requires a separate --dev-data-root test profile.");
    };
    registry.add(std::move(rehearsal));
    auto unattended = action("bmx selftest unattended", "Run a bounded isolated BMX rehearsal without requiring window focus", Group::movement);
    unattended.execution = Execution::local;
    unattended.run = [](const Model&, const Values&, const Output& out) {
        out(bmx::request_selftest(true) ? "Unattended BMX rehearsal queued for at most 120 wall seconds. Native menus must be closed; the test uses nine actual native simulation seconds and then dismounts." :
            "Unattended BMX rehearsal requires a separate --dev-data-root test profile.");
    };
    registry.add(std::move(unattended));

    auto chassis = action("bmx chassis probe", "Run the isolated one-body physics experiment", Group::movement);
    chassis.execution = Execution::local;
    chassis.run = [](const Model&, const Values&, const Output& out) {
        out(bmx::request_chassis_probe() ?
            "One-body chassis probe queued. This is a development experiment; the isolated game process must exit after its report." :
            "Chassis probe requires an explicit development profile and RESKATE_BMX_CHASSIS_PROBE=1, and runs once per process.");
    };
    registry.add(std::move(chassis));

    auto pause_test = action("bmx chassis pause-test", "Verify the same isolated BMX body parks and resumes", Group::movement);
    pause_test.execution = Execution::local;
    pause_test.run = [](const Model&, const Values&, const Output& out) {
        out(bmx::request_chassis_pause_test() ? "BMX chassis pause/resume test queued." :
            "Pause test requires an active physical chassis in an explicit development profile.");
    };
    registry.add(std::move(pause_test));

    auto fall_observation=action("bmx native-fall observe", "Observe a native skateboard fall in the explicitly opted-in private test profile", Group::movement);
    fall_observation.execution=Execution::local;
    fall_observation.run=[](const Model&,const Values&,const Output& out) {
        bmx::request_native_fall_observation();out("Private native fall observation queued; eligibility is checked on the client tick.");
    };
    registry.add(std::move(fall_observation));

    auto ramp = action("bmx ramp probe", "Measure one native ramp in the private ramp test profile", Group::movement);
    ramp.execution = Execution::local;
    ramp.run = [](const Model&, const Values&, const Output& out) {
        out(bmx::ramp_test::request() ? "Private ramp collision probe queued; no riding input is injected." :
            "Ramp probe requires the explicit isolated ramp profile and RESKATE_BMX_RAMP_TEST=1; one request per process.");
    };
    registry.add(std::move(ramp));
    auto ramp_ride = action("bmx ramp ride", "Prepare a measured private ramp route for one physical rehearsal", Group::movement);
    ramp_ride.execution = Execution::local;
    ramp_ride.run = [](const Model&, const Values&, const Output& out) {
        out(bmx::ramp_test::request_ride() ? "Private ramp riding preparation queued; validates current collision before mounting." :
            "Ramp ride requires the explicit private ramp riding launch and its one existing fixture.");
    };
    registry.add(std::move(ramp_ride));

    auto controls = action("bmx controls", "Show the BMX front/rear stick layout", Group::movement);
    controls.execution = Execution::local;
    controls.run = [](const Model&, const Values&, const Output& out) {
        out("Left stick: steer the handlebars; in the air, control bike yaw and pitch.");
        out("Right stick: rear-wheel control; down then up preloads/releases a grounded bunny hop.");
        out("Airborne: LB/L1 + right stick down = left barspin; RB/R1 + down = right barspin. Both triggers + right stick left/right = tailwhip.");
        if (bmx::chassis_ride::enabled())
            out("Partial right down/up: manual/nose manual. In the air, center the left stick first, then down/up pitches the bike; return to neutral to catch tricks. D-pad keeps Skate actions.");
        out("BMX Streets layout: A/Cross pedals; B/Circle brakes; R3 quick hops; Y/Triangle dismounts.");
        out("LB/L1 + a fresh Y/Triangle press switches bike/skateboard. F6 also switches. Plain Y tap (F7) gets on/off; hold (F8) picks up/sets down the nearby bike.");
        out("Return sticks and buttons to neutral to rearm. Menus, focus loss and disconnect clear pending gestures.");
        out("No-pad keyboard: WASD front, IJKL rear, E pedal, Q brake; Z/C shoulders and 1/3 triggers. Start/Back/Escape releases capture for menus.");
        out("bmx mount equips after stepping off the skateboard; bmx dismount restores walking. BMX trick tallies are separate from Skate scoring; grinds and multiplayer are unfinished.");
    };
    registry.add(std::move(controls));
}
} // namespace dingosdk::console
