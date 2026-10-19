#include "integration.h"
#include "Extension/Skater/first_person_camera_host.h"
#include "Extension/Skater/first_person_settings_host.h"
#include "Upstream/src/settings.h"
#include "Upstream/src/glance_controls.h"
#include "glance_runtime.h"
#include "Upstream/src/look_smoothing.h"
#include "native_stick_filter.h"
#include "native_action_bindings.h"
#include "Upstream/src/bail_collision.h"
#include "Engine/Core/Config/profile_config.h"
#include "Engine/Core/Log/logging.h"
#include <atomic>
#include <mutex>
#include <stdexcept>
namespace fpmod = jishsfirstpersonmod;
namespace vendor = first_person_vendor;
namespace {
std::atomic<unsigned> initialization{};
std::atomic<bool> capture_active{}, reset_pending{};
dingosdk::first_person_integration::QueueCameraProfile profile_queue{};
std::mutex camera_mutex;
std::mutex bindings_mutex;
dingosdk::ControllerBindingsModel native_bindings;
std::mutex capture_mutex;
dingosdk::first_person_integration::StickCapture stick_capture;
dingosdk::first_person_integration::RightStickRuntime right_stick;
fpmod::Config live_config;
dingosdk::first_person_integration::GlanceRuntime glance;
fpmod::LookSmoothing smoothing;
std::array<float,2> angles{};
fpmod::BailCollision bail_collision;
dingosdk::first_person::Vec3 last_camera_position{};
bool camera_position_ready{},bail_camera_active{},previous_bail_camera{};
double collision_return_until{};
bool on_foot{}, live{};
std::array<dingosdk::first_person_integration::NativeStickHold,6> native_holds;
std::uintptr_t camera_owner{};
double frame{};
void publish_capture(const dingosdk::first_person_integration::StickCapture& value) {
    std::lock_guard lock(capture_mutex);stick_capture=value;
    capture_active.store(value.modifier!=0 || value.watch_modifier!=0,std::memory_order_release);
}
void reset_locked() { glance.disarm_pose();smoothing.reset();right_stick.reset();publish_capture({});dingosdk::first_person_host::motion_enabled.store(false,std::memory_order_release);angles={};live=false;bail_collision.reset();camera_position_ready=false;bail_camera_active=previous_bail_camera=false;collision_return_until=0; }
void sample_settings(dingosdk::first_person::Settings& settings, double now) noexcept {
    std::unique_lock lock(camera_mutex,std::try_to_lock);
    if(lock && reset_pending.exchange(false)) reset_locked();
    if(!lock || !live) return;
    frame=now;
    if(on_foot && live_config.disable_spring_on_foot && !live_config.look_only) settings.enabled=false;
    if(bail_camera_active) {settings.enabled=false;settings.stabilize=false;settings.follow_flips=true;}
}
void compose(std::uintptr_t, dingosdk::first_person::Matrix& pose) noexcept {
    std::unique_lock lock(camera_mutex,std::try_to_lock);
    if(lock && reset_pending.exchange(false)) reset_locked();
    if(!lock || !live) return;
    auto settings=fpmod::glance_settings(live_config);
    if(live_config.link_smoothing && on_foot && live_config.disable_spring_on_foot && !live_config.look_only) settings.enabled=false;
    pose=right_stick.compose(pose,GetTickCount64());
    if(live_config.glances_enabled) pose=smoothing.apply(pose,settings,{angles[0],angles[1]},frame,live_config.link_smoothing);
    pose=right_stick.transition(pose,live_config,frame);
    const dingosdk::first_person::Vec3 desired{pose[12],pose[13],pose[14]};
    if(previous_bail_camera && !bail_camera_active && bail_collision.blocked())collision_return_until=frame+.1;
    if(bail_camera_active)collision_return_until=0;
    const bool returning=frame<collision_return_until;
    previous_bail_camera=bail_camera_active;
    if((bail_camera_active || returning) && live_config.bail_camera_collision && camera_position_ready) {
        const auto world=fpmod::bail_collision_world();
        if(world) {
            const auto safe=bail_collision.resolve(desired,last_camera_position,frame,[&](auto from,auto to,float radius) {
                return fpmod::native_bail_sweep(world,from,to,radius);
            },returning,&desired,returning ? collision_return_until-frame:-1);
            for(unsigned i=0;i<3;++i)pose[12+i]=safe[i];
        }
    } else bail_collision.reset();
    last_camera_position={pose[12],pose[13],pose[14]};camera_position_ready=true;
}
void reset() noexcept {
    reset_pending.store(true,std::memory_order_release);
    dingosdk::first_person_host::motion_enabled.store(false,std::memory_order_release);
    publish_capture({});
    std::unique_lock lock(camera_mutex,std::try_to_lock);
    if(lock) {reset_locked();reset_pending.store(false,std::memory_order_release);}
}
bool motion(std::uintptr_t rig,std::uintptr_t context,dingosdk::first_person::Matrix& pose) noexcept {
    std::unique_lock lock(camera_mutex,std::try_to_lock);
    if(lock && reset_pending.exchange(false)) reset_locked();
    return lock && live && right_stick.motion(rig,context,pose,GetTickCount64());
}
const dingosdk::first_person_host::CameraCallbacks camera_hooks{sample_settings,compose,reset,motion};
bool camera_sink(const fpmod::Config& before,const fpmod::Config& after,std::string& error) {
    using A=dingosdk::overlay::DebugAction;
    std::vector<dingosdk::overlay::DebugRequest> requests;
    const auto number=[&](A action,float old_value,float value) {
        if(old_value!=value) requests.push_back({action,false,value});
    };
    const auto flag=[&](A action,bool old_value,bool value) {
        if(old_value!=value) requests.push_back({action,value});
    };
    flag(A::set_first_person_stabilize,before.camera.stabilize,after.camera.stabilize);
    flag(A::set_first_person_follow_flips,before.camera.follow_flips,after.camera.follow_flips);
    flag(A::set_first_person_camera_cycle,before.camera.camera_cycle,after.camera.camera_cycle);
    number(A::set_first_person_smoothing,before.camera.smoothing,after.camera.smoothing);
    number(A::set_first_person_head_pitch,before.camera.head_pitch,after.camera.head_pitch);
    number(A::set_first_person_head_roll,before.camera.head_roll,after.camera.head_roll);
    number(A::set_first_person_bob,before.camera.bob,after.camera.bob);
    number(A::set_first_person_fov,before.fov,after.fov);
    flag(A::set_first_person_spring,before.spring.enabled,after.spring.enabled);
    flag(A::set_first_person_board_only,before.on_board_only,after.on_board_only);
    const A offsets[]{A::set_first_person_offset_x,A::set_first_person_offset_y,A::set_first_person_offset_z};
    const A rotations[]{A::set_first_person_pitch,A::set_first_person_yaw,A::set_first_person_roll};
    for(unsigned i=0;i<3;++i) {
        number(offsets[i],before.spring.offset[i],after.spring.offset[i]);
        number(rotations[i],before.spring.rotation[i],after.spring.rotation[i]);
    }
    number(A::set_first_person_spring_up,before.spring.up,after.spring.up);
    number(A::set_first_person_spring_down,before.spring.down,after.spring.down);
    number(A::set_first_person_spring_left,before.spring.left,after.spring.left);
    number(A::set_first_person_spring_right,before.spring.right,after.spring.right);
    if(requests.empty()) return true;
    char result[512]{};
    if(profile_queue && profile_queue(requests,result,sizeof(result))) return true;
    error=*result ? result:"Native camera profile queue unavailable.";return false;
}
DWORD WINAPI initialize(void*) noexcept {
    try {
        if(!fpmod::initialize_settings()) { initialization.store(3,std::memory_order_release);return 0; }
        fpmod::set_camera_sink(camera_sink);
        dingosdk::first_person_host::camera_callbacks.store(&camera_hooks,std::memory_order_release);
        initialization.store(2,std::memory_order_release);
        fpmod::log("Native First Person settings, glance and right-stick camera connected after overlay readiness.");
    } catch(const std::exception& e) { fpmod::log(e.what());initialization.store(3,std::memory_order_release); }
      catch(...) { initialization.store(3,std::memory_order_release); }
    return 0;
}
}
namespace jishsfirstpersonmod {
std::filesystem::path module_directory() {
    wchar_t path[32768]{};HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&module_directory),&module) || !GetModuleFileNameW(module,path,32768))
        throw std::runtime_error("Cannot locate runtime scripts directory.");
    return std::filesystem::path(path).parent_path()/L"scripts/Custom";
}
std::filesystem::path data_directory() { return dingosdk::profile_config::directory(); }
void log(const std::string& text) noexcept {
    dingosdk::logging::write(dingosdk::logging::Level::info,dingosdk::logging::Channel::skater,text);
}
}
namespace dingosdk::first_person_integration {
bool ready() noexcept { return initialization.load(std::memory_order_acquire)==2; }
bool failed() noexcept { return initialization.load(std::memory_order_acquire)==3; }

void set_callbacks(const overlay::CallbacksV3&, QueueCameraProfile queue) noexcept { profile_queue=queue; }
void start_when_ready() noexcept {
    unsigned expected=0;
    if(!initialization.compare_exchange_strong(expected,1)) return;
    const auto worker=CreateThread(nullptr,0,initialize,nullptr,0,nullptr);
    if(worker) CloseHandle(worker); else { initialization.store(3,std::memory_order_release);fpmod::log("First-person settings worker could not start."); }
}
void observe(const first_person::Settings& settings,float fov,bool active,bool available,bool requested,
             std::optional<bool> foot,std::uintptr_t client,const ControlsContext& context) noexcept {
    if(!ready()) return;
    try {
        vendor::first_person::Settings shared;
        shared.enabled=settings.enabled;shared.offset=settings.offset;shared.rotation=settings.rotation;
        shared.up=settings.up;shared.down=settings.down;shared.left=settings.left;shared.right=settings.right;
        fpmod::publish_shared_camera(fov,shared,settings.board_only,{settings.stabilize,settings.follow_flips,settings.camera_cycle,settings.smoothing,settings.head_pitch,settings.head_roll,settings.bob});
        fpmod::publish_runtime(active,available,available ? "Camera ready.":"Waiting for the native camera.");
        const auto snapshot=fpmod::menu_snapshot();
        static std::uintptr_t collision_base{};
        if(context.verified && context.game_base && collision_base!=context.game_base) {
            fpmod::initialize_bail_collision(context.game_base);collision_base=context.game_base;
        }
        fpmod::observe_collision_engine_thread();
        const auto controller=overlay::read_gameplay_controller();
        dingosdk::ControllerBindingsModel bindings;
        {std::lock_guard lock(bindings_mutex);bindings=native_bindings;}
        auto native=controller.input;
        const auto native_glance=glance_input(snapshot.config,bindings,native,controller.ready);
        const auto input=native_glance.input;
        const bool blocked=!active || !available || !foot.has_value() || !input.available || !context.verified || !context.eligible;
        auto glance_controls=native_glance.controls;
        const auto conflicts=fpmod::find_binding_conflicts(native_glance.config);
        for(unsigned i=0;i<4;++i) if(conflicts.glance[i]) glance_controls[i].combo=0;
        std::unique_lock lock(camera_mutex,std::try_to_lock);
        if(!lock) return;
        if(reset_pending.exchange(false)) reset_locked();
        if(camera_owner!=client) { reset_locked();camera_owner=client; }
        live_config=snapshot.config;on_foot=context.verified ? context.on_foot:foot.value_or(false);
        live_config.board.modifier=bindings.available ? bindings.action_combos[5]:0;
        bail_camera_active=context.bail && !snapshot.config.third_person_bailing;
        angles=glance.step(native_glance.config,glance_controls,input,blocked,requested,context);
        if(!live_config.glances_enabled) {angles={};smoothing.reset();}
        auto controls=context;
        // Eligible=false still carries verified bail/mount handoffs to the camera policy.
        if(!active || !available || !input.available) controls.verified=false;
        right_stick.step(live_config,controls,controller);
        publish_capture(right_stick.capture());
        first_person_host::motion_enabled.store(right_stick.turning(),std::memory_order_release);
        live=active && available && foot.has_value() && context.verified;
        if(blocked) smoothing.reset();
    } catch(...) { reset(); }
}
bool stick_filter_active() noexcept { return capture_active.load(std::memory_order_acquire); }
void filter_stick(unsigned slot,overlay::GamepadValues& pad) noexcept {
    if(slot>=4)return;
    struct PreserveError { DWORD value=GetLastError();~PreserveError(){SetLastError(value);} } error;
    const auto foreground=GetForegroundWindow();DWORD pid{};
    if(!foreground || !GetWindowThreadProcessId(foreground,&pid) || pid!=GetCurrentProcessId()) return;
    std::lock_guard lock(capture_mutex);
    const auto now=GetTickCount64();
    auto capture=native_holds[slot].sample(stick_capture,pad,now);
    capture.apply(slot,pad,now);
}
void filter_stick_report(PlayStationPad kind,std::span<std::uint8_t> report) noexcept {
    struct PreserveError { DWORD value=GetLastError();~PreserveError(){SetLastError(value);} } error;
    const auto foreground=GetForegroundWindow();DWORD pid{};
    if(!foreground || !GetWindowThreadProcessId(foreground,&pid) || pid!=GetCurrentProcessId()) return;
    const auto parsed=parse_playstation_report(kind,playstation_bluetooth(report.size()),report.data(),report.size());
    if(!parsed)return;
    const unsigned slot=kind==PlayStationPad::dualsense ? 5:4;
    overlay::GamepadValues raw{parsed->buttons,parsed->left_trigger,parsed->right_trigger,
        parsed->left_x,parsed->left_y,parsed->right_x,parsed->right_y};
    std::lock_guard lock(capture_mutex);
    const auto now=GetTickCount64();
    const auto capture=native_holds[slot].sample(stick_capture,raw,now);
    filter_sony_stick(capture,kind,report,now);
}
void observe_bindings(const ControllerBindingsModel& bindings) noexcept {
    if(!ready()) return;
    try {
        {std::lock_guard lock(bindings_mutex);native_bindings=bindings;}
        fpmod::NativeControls native;native.active=bindings.available;
        const std::uint32_t fixed[]{bindings.freecam_combo,bindings.tp_to_freecam_combo,bindings.noclip_combo,
            bindings.forward_velocity_combo,bindings.up_velocity_combo,bindings.offboard_up_velocity_combo,
            bindings.vote_yes_combo,bindings.vote_no_combo};
        std::copy(std::begin(fixed),std::end(fixed),native.bindings.begin());
        std::size_t index=8;
        for(std::size_t i=0;i<action_binds.size();++i) if(!action_binds[i].held && index<native.bindings.size())
            native.bindings[index++]=bindings.action_combos[i];
        fpmod::publish_native_controls(native);
    } catch(...) {}
}
}
