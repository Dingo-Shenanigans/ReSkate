#include "bmx_ramp_test.h"
#include "bmx_ramp_rehearsal.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/Build/image_identity.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Skater/no_bail.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <vector>

namespace dingosdk::bmx::ramp_test {
namespace {
using Vec = std::array<float, 3>;
constexpr unsigned side = 13, sample_count = side * side;
constexpr float spacing = .5f;
constexpr float corridor_begin_z=-6, corridor_end_z=8.5f, approach_z=7.5f;
constexpr unsigned corridor_rows=30;
constexpr std::uint64_t timeout_ms = 90000;
constexpr std::uintptr_t client_context_global = 0x73c7cb0;
struct PreserveError { DWORD value{GetLastError()}; ~PreserveError() { SetLastError(value); } };
struct Shared {
    std::mutex mutex;
    std::atomic<bool> pending{};
    Snapshot summary;
    RideRoute route;
    bool ride_requested{};
    std::filesystem::path root;
};
Shared& shared() { static auto* s = new Shared; return *s; }
struct Sample { Vec from{}, to{}; LocalSurfaceProbe hit; };
enum class Phase { waiting, baseline, acknowledge, settle, surface, picking, corridor, fine, teleport, done };
struct State {
    bool started{}, creation_attempted{}, ride{}, teleport_requested{}, dismount_requested{}, teleport_owner_adopted{};
    Phase phase{Phase::waiting};
    Snapshot summary;
    std::filesystem::path root;
    std::uintptr_t base{}, context{}, world{};
    NativeFrame owner;
    NativeFrame pre_teleport_owner, arrival_candidate;
    RideRoute route;
    Vec forward{}, right{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::uint64_t began{}, acknowledged_at{}, generation{}, revision{}, pick_id{};
    unsigned surface_pass{}, pick_index{};
    float baseline_min{}, baseline_max{};
    std::string placement_response, editor_status;
    std::vector<Sample> baseline, surface;
    std::vector<Json> previous_surface_passes;
    std::vector<Sample> corridor, fine;
    std::array<std::uint64_t, 2> fixture_body{};
    std::uint64_t teleport_at{}, stable_at{};
    std::uint64_t last_dismount_attempt_at{};
    unsigned dismount_attempts{}, dismount_accepted_count{};
    std::uint32_t last_physics_state{};
    bool last_physics_state_available{}, last_offboard{}, last_standable{};
    std::vector<Json> dismount_attempt_log;
    Vec teleport_target{}, settled_position{};
    Vec arrival_position{};
    float arrival_horizontal_error{}, arrival_vertical_error{}, arrival_height_above_floor{};
    bool arrival_position_available{}, arrival_errors_available{}, arrival_frame_available{}, arrival_gameplay_ready{};
    std::uint64_t arrival_observed_at{}, arrival_candidate_changes{}, arrival_candidate_observations{}, arrival_stable_resets{};
    const char* arrival_wait_reason="not_started";
    const char* arrival_reset_reason="none";
    EditorSurfaceHit picked;
};
State& state() { static auto* s = new State; return *s; }
bool opted_root(std::filesystem::path* out = nullptr) {
    wchar_t value[2]{};
    if (GetEnvironmentVariableW(L"RESKATE_BMX_RAMP_TEST", value, 2) != 1 || value[0] != L'1' ||
        !user_data::development_requested()) return false;
    if (!out) return user_data::runtime_development_selected(L"bmx-user-data-ramp-test");
    const auto root = user_data::override_root();
    if (!root || _wcsicmp(root->filename().c_str(), L"bmx-user-data-ramp-test") != 0) return false;
    if (out) *out = *root;
    return true;
}
bool ride_opted() noexcept {
    for(const auto* name:{L"RESKATE_BMX_RAMP_RIDE",L"RESKATE_BMX_NATIVE_CHASSIS"}) {
        wchar_t value[2]{};
        if(GetEnvironmentVariableW(name,value,2)!=1||value[0]!=L'1')return false;
    }
    return true;
}
bool finite(Vec v) noexcept {
    return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x) && std::abs(x) < 99990; });
}
bool same_owner(const NativeFrame& a, const NativeFrame& b) noexcept {
    return a.available && b.available && a.client == b.client && a.entity == b.entity && a.core == b.core &&
        a.context == b.context && a.rig == b.rig;
}
void publish(const State& s) {
    std::lock_guard lock(shared().mutex);
    shared().summary = s.summary;
    shared().route = s.route;
}
Json samples_json(const State& s, const std::vector<Sample>& samples, bool post) {
    auto result = Json::array();
    for (unsigned i = 0; i < samples.size(); ++i) {
        const auto& p = samples[i];
        Json row = Json::object();
        row["local_right_m"] = (static_cast<int>(i % side) - 6) * spacing;
        row["local_forward_m"] = (static_cast<int>(i / side) - 6) * spacing;
        row["from"] = p.from; row["to"] = p.to;
        row["available"] = p.hit.available; row["hit"] = p.hit.hit;
        row["context"] = p.hit.context; row["world"] = p.hit.world;
        row["entity"] = p.hit.hit_entity; row["body"] = p.hit.hit_body;
        row["excluded_bodies"] = p.hit.excluded_bodies;
        if (p.hit.hit) {
            row["position"] = p.hit.position; row["fraction"] = p.hit.fraction;
            if (post && i < s.baseline.size() && s.baseline[i].hit.hit)
                row["height_delta_m"] = p.hit.position[1] - s.baseline[i].hit.position[1];
        }
        result.push_back(std::move(row));
    }
    return result;
}
void write_report(const State& s) {
    Json report = Json::object();
    report["process_id"] = GetCurrentProcessId();
    report["completed"] = s.summary.completed; report["prepared"] = s.summary.prepared;
    report["failed"] = s.summary.failed; report["phase"] = s.summary.phase; report["issue"] = s.summary.issue;
    report["profile_root"] = s.root.string(); report["map"] = s.summary.map;
    report["generation"] = s.generation; report["revision"] = s.revision;
    report["creation_attempted"] = s.creation_attempted; report["placement_response"] = s.placement_response;
    report["existing_fixture_ride_preparation"] = s.ride;
    report["teleport_requested"] = s.teleport_requested; report["teleport_target"] = s.teleport_target;
    const auto owner_json=[](const NativeFrame& owner) {return Json{{"client",owner.client},{"entity",owner.entity},
        {"core",owner.core},{"context",owner.context},{"rig",owner.rig}};};
    report["owner_before_teleport"]=owner_json(s.pre_teleport_owner);
    report["owner_after_teleport"]=owner_json(s.route.ready?s.route.owner:s.arrival_candidate);
    report["teleport_owner_adopted"]=s.teleport_owner_adopted;
    report["dismount_attempts"]=s.dismount_attempts;report["dismount_accepted_count"]=s.dismount_accepted_count;
    report["last_physics_state_available"]=s.last_physics_state_available;report["last_physics_state"]=s.last_physics_state;
    report["last_offboard"]=s.last_offboard;report["last_standable"]=s.last_standable;
    report["dismount_attempt_log"]=s.dismount_attempt_log;
    const auto now=GetTickCount64();
    const bool target_available=s.teleport_requested&&finite(s.teleport_target);
    report["arrival"] = Json{{"position_available",s.arrival_position_available},
        {"errors_available",s.arrival_errors_available},{"teleport_target_available",target_available},
        {"last_frame_available",s.arrival_frame_available},{"last_gameplay_ready",s.arrival_gameplay_ready},
        {"stable_age_available",s.stable_at!=0},{"stable_age_ms",s.stable_at?now-s.stable_at:0},
        {"candidate_observations",s.arrival_candidate_observations},{"candidate_changes",s.arrival_candidate_changes},
        {"stability_resets",s.arrival_stable_resets},{"last_reset_reason",s.arrival_reset_reason},
        {"wait_reason",s.arrival_wait_reason}};
    auto& arrival=report["arrival"];
    arrival["position"]=s.arrival_position_available?Json(s.arrival_position):Json(nullptr);
    arrival["observed_age_ms"]=s.arrival_position_available?Json(now-s.arrival_observed_at):Json(nullptr);
    arrival["teleport_target"]=target_available?Json(s.teleport_target):Json(nullptr);
    arrival["horizontal_error_m"]=s.arrival_errors_available?Json(s.arrival_horizontal_error):Json(nullptr);
    arrival["vertical_error_to_teleport_target_m"]=s.arrival_errors_available?Json(s.arrival_vertical_error):Json(nullptr);
    arrival["height_above_start_floor_m"]=s.arrival_errors_available?Json(s.arrival_height_above_floor):Json(nullptr);
    report["route"] = Json{{"ready",s.route.ready},{"start",s.route.start},{"forward",s.route.forward},
        {"origin",s.route.origin},{"yaw",s.route.yaw},{"center_right",s.route.center_right},
        {"foot_z",s.route.foot_z},{"lip_z",s.route.lip_z},{"lip_height",s.route.lip_height},{"floor_y",s.route.floor_y}};
    report["route"]["prepared_at_ms"] = s.route.prepared_at_ms;
    report["route"]["world"] = s.route.world;report["route"]["generation"] = s.route.generation;report["route"]["revision"] = s.route.revision;
    report["route"]["owner"] = Json{{"client",s.route.owner.client},{"entity",s.route.owner.entity},
        {"core",s.route.owner.core},{"context",s.route.owner.context},{"rig",s.route.owner.rig}};
    report["editor_status"] = s.editor_status; report["item"] = s.summary.item;
    report["object_id"] = s.summary.object_id; report["fixture_origin"] = s.summary.fixture_origin;
    report["fixture_rotation_xyzw"] = s.rotation; report["fixture_yaw_radians"] = s.summary.fixture_yaw;
    report["local_positive_forward_world"] = s.forward; report["local_positive_right_world"] = s.right;
    report["authored_ramp_up_axis_known"] = false;
    report["driving_test_performed"] = false; report["landing_test_passed"] = false;
    report["world"] = s.world; report["client_context"] = s.context;
    report["baseline_min_y"] = s.baseline_min; report["baseline_max_y"] = s.baseline_max;
    report["grid_side"] = side; report["grid_spacing_m"] = spacing;
    report["raised_samples"] = s.summary.raised_samples; report["surface_passes"] = s.surface_pass;
    report["baseline"] = samples_json(s, s.baseline, false);
    report["surface"] = samples_json(s, s.surface, true);
    report["previous_surface_passes"] = s.previous_surface_passes;
    auto corridor = Json::array();
    for (unsigned i=0;i<s.corridor.size();++i) {
        const auto& sample=s.corridor[i];
        corridor.push_back(Json{{"local_right_m",s.route.center_right+(static_cast<int>(i%3)-1)*.4f},
            {"local_forward_m",corridor_begin_z+static_cast<float>(i/3)*.5f},{"from",sample.from},{"to",sample.to},
            {"available",sample.hit.available},{"hit",sample.hit.hit},{"position",sample.hit.position},
            {"world",sample.hit.world},{"context",sample.hit.context},{"entity",sample.hit.hit_entity},{"body",sample.hit.hit_body}});
    }
    report["corridor"] = std::move(corridor);
    report["corridor_begin_z"] = corridor_begin_z;report["corridor_end_z"] = corridor_end_z;report["approach_local_z"] = approach_z;
    auto fine = Json::array();
    for (unsigned i=0;i<s.fine.size();++i) {
        const auto& sample=s.fine[i];
        fine.push_back(Json{{"local_right_m",s.route.center_right},{"local_forward_m",-2.f+static_cast<float>(i)*.05f},
            {"from",sample.from},{"to",sample.to},{"position",sample.hit.position},{"body",sample.hit.hit_body},
            {"world",sample.hit.world},{"context",sample.hit.context},{"available",sample.hit.available},{"hit",sample.hit.hit}});
    }
    report["fine_ramp_profile"] = std::move(fine);
    report["pick"] = Json{{"available", s.picked.available}, {"hit", s.picked.hit},
        {"object", s.picked.object}, {"entity", s.picked.entity}, {"position", s.picked.position},
        {"request_id", s.pick_id}, {"generation", s.picked.generation}};
    report["wall_elapsed_ms"] = GetTickCount64() - s.began;
    // All writes remain in the exact opted-in isolated profile. No game assets
    // are written, and the authoritative placed object is retained for inspection.
    const auto temporary = s.root / (s.ride ? L"bmx-ramp-ride-preparation.tmp.json" : L"bmx-ramp-probe.tmp.json");
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot open ramp probe report");
    file << report.dump(2) << '\n'; file.close();
    if (!file) throw std::runtime_error("Cannot write ramp probe report");
    if (!MoveFileExW(temporary.c_str(), std::filesystem::path(s.summary.report_path).c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("Cannot commit ramp probe report");
}
void finish(State& s, const char* issue, bool prepared = false) {
    if(!prepared)s.route.ready=false;
    s.phase = Phase::done; s.summary.completed = true; s.summary.prepared = prepared;
    s.summary.failed = !prepared; s.summary.phase = prepared ? "collision_profile_ready" : "failed";
    s.summary.issue = issue;
    try { write_report(s); } catch (...) { s.route.ready=false;s.summary.failed = true; s.summary.prepared = false; s.summary.issue = "report_write_failed"; }
    publish(s);
}
bool session_matches(State& s, const NativeFrame& rider) {
    std::uintptr_t context{}, canonical{};
    return same_owner(s.owner, rider) && memory::peek(rider.client + 8, context) && context == s.context &&
        memory::peek(s.base + client_context_global, canonical) && canonical == s.context;
}
bool ray(State& s, const NativeFrame& rider, Vec from, Vec to, Sample& sample) {
    sample.from = from; sample.to = to;
    sample.hit = probe_local_surface(from, to, {}, rider.entity);
    const auto& h = sample.hit;
    if (!h.available || h.context != s.context || !h.world || (s.world && h.world != s.world)) return false;
    if (!s.world) s.world = h.world;
    if (!h.hit) return true;
    if (!finite(h.position) || !std::isfinite(h.fraction) || h.fraction < 0 || h.fraction > 1) return false;
    float error = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const float delta = h.position[i] - (from[i] + (to[i] - from[i]) * h.fraction);
        error += delta * delta;
    }
    return error <= .025f * .025f;
}
void grid(State& s, const NativeFrame& rider, std::vector<Sample>& samples, unsigned& budget) {
    while (samples.size() < sample_count && budget) {
        const auto i = static_cast<unsigned>(samples.size());
        const float x = (static_cast<int>(i % side) - 6) * spacing;
        const float z = (static_cast<int>(i / side) - 6) * spacing;
        Vec from = s.summary.fixture_origin;
        for (unsigned j = 0; j < 3; ++j) from[j] += s.right[j] * x + s.forward[j] * z;
        Vec to = from; from[1] += 4; to[1] -= 2;
        Sample p; --budget;
        if (!ray(s, rider, from, to, p)) { samples.push_back(p); finish(s, "surface_query_invalid_or_world_changed"); return; }
        samples.push_back(p);
    }
}
bool acknowledged(const State& s, const EditorObject& object) {
    if (!object.id || !object.spawned || object.item != s.summary.item || !std::isfinite(object.scale) || std::abs(object.scale - 1) > .001f) return false;
    float qdot = 0, norm = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(object.position[i]) || std::abs(object.position[i] - s.summary.fixture_origin[i]) > .005f) return false;
    for (unsigned i = 0; i < 4; ++i) {
        if(!std::isfinite(object.rotation[i]))return false;
        qdot += object.rotation[i] * s.rotation[i];norm += object.rotation[i] * object.rotation[i];
    }
    return std::abs(norm-1)<.001f && std::isfinite(qdot) && std::abs(std::abs(qdot) - 1) < .001f;
}
bool local_ray(State& s, const NativeFrame& rider, float x, float z, Sample& out) {
    Vec from=s.summary.fixture_origin;
    for(unsigned a=0;a<3;++a)from[a]+=s.right[a]*x+s.forward[a]*z;
    Vec to=from;from[1]+=4;to[1]-=2;
    return ray(s,rider,from,to,out)&&out.hit.hit;
}
void note_stability_reset(State& s,const char* reason) noexcept {
    if(s.stable_at){++s.arrival_stable_resets;s.arrival_reset_reason=reason;}
    else if(!s.arrival_stable_resets)s.arrival_reset_reason=reason;
}
const char* corridor_issue(const State& s) {
    if(s.corridor.size()!=corridor_rows*3)return "corridor_sample_count_invalid";
    float low=std::numeric_limits<float>::max(),high=std::numeric_limits<float>::lowest();
    bool previous_clear=false;std::array<float,3> previous{};
    for(unsigned row=0;row<corridor_rows;++row) {
        const float z=corridor_begin_z+static_cast<float>(row)*.5f;
        const bool clear=z<s.route.lip_z-.25f||z>s.route.foot_z+.25f;
        std::array<float,3> heights{};
        for(unsigned column=0;column<3;++column) {
            const auto& p=s.corridor[row*3+column].hit;
            if(!p.available||!p.hit||!finite(p.position))return "corridor_surface_unavailable";
            heights[column]=p.position[1];
            if(!clear) {
                if(p.hit_body!=s.fixture_body&&p.position[1]>s.baseline_min+.05f)return "foreign_raised_body_in_ramp_corridor";
                continue;
            }
            if(p.hit_body==s.fixture_body||std::abs(heights[column]-s.baseline_min)>.10f)return "approach_or_landing_obstruction";
            low=(std::min)(low,heights[column]);high=(std::max)(high,heights[column]);
            if(previous_clear&&std::abs(heights[column]-previous[column])>.08f)return "corridor_adjacent_step_limit";
        }
        if(clear) {
            const auto [minimum,maximum]=std::minmax_element(heights.begin(),heights.end());
            if(*maximum-*minimum>.04f)return "corridor_cross_width_limit";
        }
        previous=heights;previous_clear=clear; // never bridge across the excluded ramp rows
    }
    return high-low>.10f?"corridor_total_ground_variation_limit":nullptr;
}
void advance_ride(State& s, std::uintptr_t base, const NativeFrame& rider, bool gameplay_ready) {
    if(!ride_opted()){finish(s,"ramp_ride_opt_in_removed");return;}
    unsigned budget=4;
    if(s.phase==Phase::waiting) {
        if(!gameplay_ready||!rider.available)return;
        if(!supported_build::running_image_matches(base)||!finite(rider.position)) {finish(s,"unsupported_image_or_owner");return;}
        const auto editor=local_park_editor();
        if(!editor.available||editor.busy)return;
        if(editor.failed||editor.objects.size()!=1) {finish(s,"ride_requires_exactly_one_existing_fixture");return;}
        const auto& object=editor.objects.front();
        if(!object.spawned||!object.id||!finite(object.position)||!std::isfinite(object.scale)||std::abs(object.scale-1)>.001f||
           (object.item!="own_bkramps_generic_kickerflatlarge_00001"&&object.item!="own_bkramps_generic_kickerflatsmall_00001")) {
            finish(s,"existing_fixture_type_or_pose_invalid");return;
        }
        const auto& q=object.rotation;
        float norm=0;for(float value:q){if(!std::isfinite(value)){finish(s,"existing_fixture_rotation_invalid");return;}norm+=value*value;}
        if(std::abs(norm-1)>.001f||std::abs(q[0])>.001f||std::abs(q[2])>.001f) {finish(s,"existing_fixture_not_yaw_only");return;}
        s.owner=rider;s.base=base;
        if(!memory::peek(rider.client+8,s.context)||!s.context||!session_matches(s,rider)) {finish(s,"client_owner_context_unavailable");return;}
        s.rotation=q;s.summary.fixture_origin=object.position;s.summary.item=object.item;s.summary.object_id=object.id;
        s.summary.fixture_yaw=std::atan2(2*q[1]*q[3],1-2*q[1]*q[1]);
        s.forward={std::sin(s.summary.fixture_yaw),0,std::cos(s.summary.fixture_yaw)};
        s.right={s.forward[2],0,-s.forward[0]};
        s.summary.map=editor.map;s.generation=editor.generation;s.revision=editor.revision;
        s.route.origin=object.position;s.route.object_id=object.id;
        for(unsigned a=0;a<3;++a)s.route.forward[a]=-s.forward[a];
        s.route.yaw=std::atan2(s.route.forward[0],s.route.forward[2]);
        s.phase=Phase::surface;s.summary.phase="remeasuring_existing_fixture";
    }
    // The existing teleport temporarily makes read_native_frame unavailable.
    // Wait without touching native state; require a fresh canonical client
    // owner before accepting arrival, sampling the floor, or exposing a route.
    if(s.phase==Phase::teleport) {
        s.arrival_frame_available=rider.available;s.arrival_gameplay_ready=gameplay_ready;
    }
    if(s.phase==Phase::teleport&&(!gameplay_ready||!rider.available)) {
        s.arrival_wait_reason=!gameplay_ready?"waiting_gameplay_readiness":"waiting_available_frame";
        if(GetTickCount64()-s.teleport_at>12000)finish(s,"native_teleport_arrival_timeout");
        return;
    }
    if(s.phase==Phase::teleport) {
        std::uintptr_t context{},canonical{};
        // A native teleport may destroy and recreate the local skater. Only
        // this phase can accept that: the client/context remain pinned, while
        // read_native_frame resolves the entire new canonical local owner.
        // No original entity/core/rig memory is accessed after queuing.
        if(base!=s.base||rider.client!=s.pre_teleport_owner.client||
           !memory::peek(rider.client+8,context)||context!=s.context||
           !memory::peek(base+client_context_global,canonical)||canonical!=s.context||
           !same_owner(rider,read_native_frame(base,rider.client))||!finite(rider.position)) {
            s.arrival_wait_reason="canonical_owner_gate_rejected";finish(s,"teleport_client_or_canonical_owner_changed");return;
        }
        s.arrival_position=rider.position;s.arrival_position_available=true;s.arrival_observed_at=GetTickCount64();
        s.arrival_horizontal_error=std::hypot(rider.position[0]-s.teleport_target[0],rider.position[2]-s.teleport_target[2]);
        s.arrival_vertical_error=rider.position[1]-s.teleport_target[1];
        s.arrival_height_above_floor=rider.position[1]-s.route.floor_y;
        s.arrival_errors_available=std::isfinite(s.arrival_horizontal_error)&&std::isfinite(s.arrival_vertical_error)&&
            std::isfinite(s.arrival_height_above_floor);
        if(!same_owner(s.arrival_candidate,rider)) {
            if(s.arrival_candidate.available)++s.arrival_candidate_changes;
            ++s.arrival_candidate_observations;
            note_stability_reset(s,s.arrival_candidate.available?"candidate_owner_changed":"candidate_initialized");
            s.arrival_candidate=rider;s.stable_at=0;
        }
    } else if(!gameplay_ready||base!=s.base||!session_matches(s,rider)) {finish(s,"owner_or_gameplay_changed");return;}
    const auto editor=local_park_editor();s.editor_status=editor.status;
    if(!editor.available||editor.failed||editor.busy||editor.map!=s.summary.map||editor.generation!=s.generation||
       editor.revision!=s.revision||editor.objects.size()!=1||editor.objects.front().id!=s.summary.object_id||
       !acknowledged(s,editor.objects.front())) {finish(s,"existing_fixture_session_changed");return;}
    if(s.phase==Phase::surface) {
        grid(s,rider,s.surface,budget);s.summary.surface_samples=static_cast<unsigned>(s.surface.size());
        if(s.phase==Phase::done||s.surface.size()!=sample_count)return;
        s.baseline_min=std::numeric_limits<float>::max();
        for(const auto& p:s.surface){if(!p.hit.hit){finish(s,"fresh_grid_has_clear_rays");return;}s.baseline_min=(std::min)(s.baseline_min,p.hit.position[1]);}
        float xmin=3,xmax=-3,zmin=3,zmax=-3,top=-99990,y_at_min=0,y_at_max=0;
        s.summary.raised_samples=0;
        for(unsigned i=0;i<sample_count;++i) {
            const auto& p=s.surface[i];if(p.hit.position[1]<=s.baseline_min+.05f)continue;
            const float x=(static_cast<int>(i%side)-6)*spacing,z=(static_cast<int>(i/side)-6)*spacing;
            ++s.summary.raised_samples;xmin=(std::min)(xmin,x);xmax=(std::max)(xmax,x);
            if(z<zmin){zmin=z;y_at_min=p.hit.position[1];}if(z>zmax){zmax=z;y_at_max=p.hit.position[1];}
            if(p.hit.position[1]>top){top=p.hit.position[1];s.pick_index=i;}
        }
        if(s.summary.raised_samples<6||xmax-xmin<.5f||zmax-zmin<1||top-s.baseline_min<.3f||top-s.baseline_min>1.5f||
           (y_at_min-y_at_max)/(zmax-zmin)<.1f||(y_at_min-y_at_max)/(zmax-zmin)>.8f) {finish(s,"fresh_ramp_profile_not_expected_uphill_axis");return;}
        s.route.center_right=(xmin+xmax)/2;s.fixture_body=s.surface[s.pick_index].hit.hit_body;
        if(s.fixture_body[0]!=s.world||!s.fixture_body[1]) {finish(s,"fresh_ramp_body_unresolved");return;}
        for(const auto& p:s.surface)if(p.hit.position[1]>s.baseline_min+.05f&&p.hit.hit_body!=s.fixture_body) {finish(s,"raised_profile_contains_foreign_body");return;}
        s.pick_id=GetTickCount64()|1;EditorSurfaceRequest pick;pick.generation=s.generation;pick.id=s.pick_id;
        pick.origin=s.surface[s.pick_index].from;pick.direction={0,-1,0};pick.picking=true;
        if(!queue_local_park_surface(pick)){finish(s,"fixture_pick_request_rejected");return;}
        s.phase=Phase::picking;s.summary.phase="verifying_existing_fixture_collision";
    } else if(s.phase==Phase::picking&&editor.surface.id==s.pick_id&&editor.surface.generation==s.generation) {
        s.picked=editor.surface;
        if(!s.picked.available||!s.picked.hit||s.picked.object!=s.summary.object_id){finish(s,"fresh_collision_not_owned_by_fixture");return;}
        s.phase=Phase::corridor;s.summary.phase="measuring_approach_and_landing_corridor";
    } else if(s.phase==Phase::corridor) {
        constexpr unsigned count=corridor_rows*3;
        while(s.corridor.size()<count&&budget) {
            const auto i=static_cast<unsigned>(s.corridor.size());Sample p;--budget;
            const bool valid=local_ray(s,rider,s.route.center_right+(static_cast<int>(i%3)-1)*.4f,corridor_begin_z+static_cast<float>(i/3)*.5f,p);
            s.corridor.push_back(p);if(!valid){finish(s,"approach_or_landing_corridor_unavailable");return;}
        }
        if(s.corridor.size()!=count)return;
        s.phase=Phase::fine;s.summary.phase="measuring_actual_foot_and_lip";
    } else if(s.phase==Phase::fine) {
        constexpr unsigned count=81;
        while(s.fine.size()<count&&budget) {
            const auto i=static_cast<unsigned>(s.fine.size());Sample p;--budget;
            const bool valid=local_ray(s,rider,s.route.center_right,-2.f+static_cast<float>(i)*.05f,p);
            s.fine.push_back(p);if(!valid){finish(s,"fine_ramp_profile_unavailable");return;}
        }
        if(s.fine.size()!=count)return;
        unsigned first=count,last=0;
        for(unsigned i=0;i<count;++i)if(s.fine[i].hit.hit_body==s.fixture_body) {first=(std::min)(first,i);last=i;}
        if(first==count||first==0||last+1==count||last-first<20){finish(s,"fine_ramp_endpoints_not_bounded");return;}
        for(unsigned i=first;i<=last;++i)if(s.fine[i].hit.hit_body!=s.fixture_body){finish(s,"fine_ramp_body_discontinuous");return;}
        s.route.lip_z=-2.f+static_cast<float>(first)*.05f;s.route.foot_z=-2.f+static_cast<float>(last)*.05f;
        s.route.lip_height=s.fine[first].hit.position[1]-s.baseline_min;
        if(const auto issue=corridor_issue(s)){finish(s,issue);return;}
        const auto& start=s.corridor[27*3+1]; // exact measured z=+7.5, central column
        s.route.floor_y=start.hit.position[1];s.route.start=start.hit.position;
        // Keep the route boundary at the measured z=7.5 row, but request the
        // native teleport half a metre inside it. Native dismount can shift
        // the rider by ~.33m; accepting that behind the boundary is unsafe.
        s.teleport_target=s.corridor[26*3+1].hit.position;s.teleport_target[1]+=1;
        const auto current=read_native_frame(base,rider.client);const auto mp=multiplayer::model();
        if(!same_owner(s.owner,current)||mp.active||mp.lobby_joining){finish(s,"owner_or_solo_changed_before_teleport");return;}
        s.pre_teleport_owner=current;
        s.teleport_requested=true;s.teleport_at=GetTickCount64(); // one queue attempt, never retried
        if(!teleport_local_skater(s.teleport_target)){finish(s,"native_teleport_queue_unavailable");return;}
        s.phase=Phase::teleport;s.summary.phase="waiting_observed_native_teleport_arrival";
        s.arrival_wait_reason="waiting_native_teleport_arrival";
    } else if(s.phase==Phase::teleport) {
        if(GetTickCount64()-s.teleport_at>12000){finish(s,"native_teleport_arrival_timeout");return;}
        s.last_offboard=rider.offboard;s.last_standable=rider.standable;
        std::uint32_t again{};
        // NativeFrame.context is the freshly resolved core+3c0 skater context;
        // client_noclip.cpp and request_bmx_dismount read its current state here.
        // This passive diagnostic never decides whether a native request is safe.
        s.last_physics_state_available=memory::peek(rider.context+0x1414,s.last_physics_state)&&
            memory::peek(rider.context+0x1414,again)&&again==s.last_physics_state;
        if(std::hypot(rider.position[0]-s.teleport_target[0],rider.position[2]-s.teleport_target[2])>.35f||
           !ramp_rehearsal::entry_position_valid(rider.position,s.route.start,s.route.forward)||
           rider.position[1]<s.route.floor_y-.05f||rider.position[1]>s.route.floor_y+2.5f) {
            note_stability_reset(s,"outside_target_arrival_bounds");s.arrival_wait_reason="outside_target_arrival_bounds";
            s.stable_at=0;return;
        }
        float movement=0;for(unsigned a=0;a<3;++a)movement+=(rider.position[a]-s.settled_position[a])*(rider.position[a]-s.settled_position[a]);
        if(!s.stable_at||movement>.02f*.02f){
            note_stability_reset(s,s.stable_at?"position_moved_over_2cm":"stability_anchor_initialized");
            s.arrival_wait_reason="waiting_position_stability";s.stable_at=GetTickCount64();s.settled_position=rider.position;return;
        }
        if(!rider.offboard) {
            note_stability_reset(s,"rider_not_offboard");s.arrival_wait_reason="waiting_offboard_transition";
            s.stable_at=0; // the required stable interval begins on foot
            const auto now=GetTickCount64();
            if(!s.last_dismount_attempt_at||now-s.last_dismount_attempt_at>=350) {
                s.last_dismount_attempt_at=now;++s.dismount_attempts;
                const bool accepted=request_bmx_dismount(rider.client,rider.entity);
                if(accepted){s.dismount_requested=true;++s.dismount_accepted_count;}
                s.dismount_attempt_log.push_back(Json{{"wall_ms_after_teleport",now-s.teleport_at},
                    {"accepted",accepted},{"entity",rider.entity},{"physics_state_available",s.last_physics_state_available},
                    {"physics_state",s.last_physics_state},{"offboard",rider.offboard},{"standable",rider.standable}});
            }
            return;
        }
        if(!rider.standable){note_stability_reset(s,"rider_not_standable");s.arrival_wait_reason="waiting_standable";s.stable_at=0;return;}
        if(GetTickCount64()-s.stable_at<300){s.arrival_wait_reason="waiting_300ms_stable_onfoot";return;}
        Sample ground;
        Vec from=rider.position,to=rider.position;from[1]+=2;to[1]-=3;
        if(!ray(s,rider,from,to,ground)||!ground.hit.hit||
           std::abs(ground.hit.position[1]-s.route.floor_y)>.03f){finish(s,"arrival_floor_changed");return;}
        s.teleport_owner_adopted=!same_owner(s.pre_teleport_owner,rider);s.owner=rider;
        s.route.owner=rider;s.route.prepared_at_ms=GetTickCount64();
        s.route.world=s.world;s.route.generation=s.generation;s.route.revision=s.revision;
        s.arrival_wait_reason="arrival_verified";
        s.route.ready=true;finish(s,"existing_fixture_corridor_and_native_arrival_verified",true);
    }
}
void advance(State& s, std::uintptr_t base, const NativeFrame& rider, bool gameplay_ready) {
    if (GetTickCount64() - s.began >= timeout_ms) { finish(s, "probe_wall_timeout"); return; }
    const auto multiplayer_state = multiplayer::model();
    if (multiplayer_state.active || multiplayer_state.lobby_joining) { finish(s, "private_solo_world_required"); return; }
    std::filesystem::path root;
    if (!opted_root(&root) || root != s.root) { finish(s, "development_profile_changed"); return; }
    if(s.ride){advance_ride(s,base,rider,gameplay_ready);return;}
    unsigned budget = 4;
    if (s.phase == Phase::waiting) {
        if (!gameplay_ready || !rider.available) return;
        if (!supported_build::running_image_matches(base) || !finite(rider.position)) { finish(s, "unsupported_image_or_owner"); return; }
        auto editor = local_park_editor();
        if (!editor.available || editor.busy || !editor.assets || editor.assets->empty()) return;
        if (editor.failed || !editor.objects.empty()) { finish(s, "fixture_profile_map_not_empty_or_failed"); return; }
        for (const auto* key : {"own_bkramps_generic_kickerflatlarge_00001", "own_bkramps_generic_kickerflatsmall_00001"})
            if (std::any_of(editor.assets->begin(), editor.assets->end(), [&](const auto& item) { return item.key == key; })) {
                s.summary.item = key; break;
            }
        if (s.summary.item.empty()) { finish(s, "flat_kicker_not_in_runtime_catalog"); return; }
        s.owner = rider; s.base = base;
        if (!memory::peek(rider.client + 8, s.context) || !s.context || !session_matches(s, rider)) { finish(s, "client_owner_context_unavailable"); return; }
        const float length = std::hypot(rider.transform[8], rider.transform[10]);
        if (!std::isfinite(length) || length < .5f || length > 1.5f) { finish(s, "rider_heading_invalid"); return; }
        s.forward = {rider.transform[8] / length, 0, rider.transform[10] / length};
        s.right = {s.forward[2], 0, -s.forward[0]};
        s.summary.fixture_yaw = std::atan2(s.forward[0], s.forward[2]);
        s.rotation = {0, std::sin(s.summary.fixture_yaw / 2), 0, std::cos(s.summary.fixture_yaw / 2)};
        s.summary.fixture_origin = rider.position;
        for (unsigned i = 0; i < 3; ++i) s.summary.fixture_origin[i] += s.forward[i] * 8;
        Vec from = s.summary.fixture_origin, to = from; from[1] += 4; to[1] -= 4;
        Sample center; --budget;
        if (!ray(s, rider, from, to, center) || !center.hit.hit) { finish(s, "fixture_center_has_no_verified_floor"); return; }
        s.summary.fixture_origin[1] = center.hit.position[1];
        s.summary.map = editor.map; s.generation = editor.generation; s.revision = editor.revision;
        s.phase = Phase::baseline; s.summary.phase = "baseline_grid";
    }
    if (!gameplay_ready || base != s.base || !session_matches(s, rider)) { finish(s, "owner_or_gameplay_changed"); return; }
    const auto editor = local_park_editor(); s.editor_status = editor.status;
    if (!editor.available || editor.failed || editor.map != s.summary.map || editor.generation != s.generation) { finish(s, "editor_session_changed_or_failed"); return; }
    if (s.phase == Phase::baseline) {
        if (editor.busy || editor.revision != s.revision || !editor.objects.empty()) { finish(s, "editor_changed_during_baseline"); return; }
        grid(s, rider, s.baseline, budget); s.summary.baseline_samples = static_cast<unsigned>(s.baseline.size());
        if (s.phase == Phase::done || s.baseline.size() != sample_count) return;
        s.baseline_min = std::numeric_limits<float>::max(); s.baseline_max = std::numeric_limits<float>::lowest();
        for (const auto& p : s.baseline) {
            if (!p.hit.hit) { finish(s, "baseline_grid_has_clear_rays"); return; }
            s.baseline_min = (std::min)(s.baseline_min, p.hit.position[1]);
            s.baseline_max = (std::max)(s.baseline_max, p.hit.position[1]);
        }
        if (s.baseline_max - s.baseline_min > .05f) { finish(s, "baseline_patch_not_flat"); return; }
        if (!same_owner(s.owner, read_native_frame(base, rider.client))) { finish(s, "owner_changed_before_place"); return; }
        const auto before_place_session = multiplayer::model();
        if (before_place_session.active || before_place_session.lobby_joining) { finish(s, "private_solo_world_required"); return; }
        profile::PlacedObject object; object.item = s.summary.item; object.position = s.summary.fixture_origin; object.rotation = s.rotation;
        s.creation_attempted = true; // consumed before the only mutating API invocation
        s.placement_response = edit_local_park("place", s.summary.map, s.generation, s.revision, {}, object);
        if (s.placement_response.starts_with("error:")) { finish(s, "native_editor_place_rejected"); return; }
        s.phase = Phase::acknowledge; s.summary.phase = "waiting_native_placement_ack";
    } else if (s.phase == Phase::acknowledge) {
        if (editor.busy) return;
        if (editor.objects.size() != 1 || !acknowledged(s, editor.objects.front())) { finish(s, "placement_ack_pose_or_identity_mismatch"); return; }
        s.summary.object_id = editor.objects.front().id; s.revision = editor.revision;
        s.acknowledged_at = GetTickCount64(); s.phase = Phase::settle; s.summary.phase = "waiting_client_collision";
    } else {
        if (editor.busy || editor.revision != s.revision || editor.objects.size() != 1 ||
            editor.objects.front().id != s.summary.object_id || !acknowledged(s, editor.objects.front())) { finish(s, "acknowledged_fixture_changed"); return; }
        if (s.phase == Phase::settle && GetTickCount64() - s.acknowledged_at >= 1000) {
            s.phase = Phase::surface; s.summary.phase = "native_collision_grid"; ++s.surface_pass;
        }
        if (s.phase == Phase::surface) {
            grid(s, rider, s.surface, budget); s.summary.surface_samples = static_cast<unsigned>(s.surface.size());
            if (s.phase == Phase::done || s.surface.size() != sample_count) return;
            float maximum = 0; s.summary.raised_samples = 0;
            for (unsigned i = 0; i < sample_count; ++i) {
                if (!s.surface[i].hit.hit) continue;
                const float rise = s.surface[i].hit.position[1] - s.baseline[i].hit.position[1];
                if (rise > .05f) ++s.summary.raised_samples;
                if (rise > maximum) { maximum = rise; s.pick_index = i; }
            }
            if (s.summary.raised_samples < 3) {
                if (s.surface_pass >= 3) { finish(s, "acknowledged_ramp_collision_not_observed"); return; }
                s.previous_surface_passes.push_back(samples_json(s, s.surface, true)); s.surface.clear();
                s.acknowledged_at = GetTickCount64(); s.phase = Phase::settle; return;
            }
            const auto& top = s.surface[s.pick_index]; s.pick_id = GetTickCount64() | 1;
            EditorSurfaceRequest pick; pick.generation = s.generation; pick.id = s.pick_id;
            pick.origin = top.from; pick.direction = {0, -1, 0}; pick.picking = true;
            if (!queue_local_park_surface(pick)) { finish(s, "fixture_pick_request_rejected"); return; }
            s.phase = Phase::picking; s.summary.phase = "verifying_native_collision_object";
        } else if (s.phase == Phase::picking && editor.surface.id == s.pick_id && editor.surface.generation == s.generation) {
            s.picked = editor.surface;
            if (!s.picked.available || !s.picked.hit || s.picked.object != s.summary.object_id) { finish(s, "raised_collision_not_owned_by_fixture"); return; }
            finish(s, "actual_ramp_profile_measured_up_axis_not_inferred", true);
        }
    }
}
} // namespace
bool enabled() noexcept { PreserveError error; try { return opted_root(); } catch (...) { return false; } }
bool request() noexcept {
    PreserveError error;
    try {
        std::filesystem::path root; if (!opted_root(&root)) return false;
        auto& s = shared(); std::lock_guard lock(s.mutex);
        if (s.summary.requested) return false;
        s.root = std::move(root); s.summary.requested = true; s.summary.phase = "requested";
        s.summary.report_path = (s.root / L"bmx-ramp-probe.json").string();
        s.pending.store(true, std::memory_order_release); return true;
    } catch (...) { return false; }
}
bool request_ride() noexcept {
    PreserveError error;
    try {
        std::filesystem::path root;if(!opted_root(&root)||!ride_opted())return false;
        auto& s=shared();std::lock_guard lock(s.mutex);if(s.summary.requested)return false;
        s.root=std::move(root);s.summary.requested=true;s.summary.phase="ride_preparation_requested";
        s.summary.report_path=(s.root/L"bmx-ramp-ride-preparation.json").string();s.ride_requested=true;
        s.pending.store(true,std::memory_order_release);return true;
    }catch(...){return false;}
}
void tick(std::uintptr_t base, const NativeFrame& rider, bool gameplay_ready) noexcept {
    PreserveError error; auto& s = state();
    try {
        if (shared().pending.exchange(false, std::memory_order_acquire)) {
            std::lock_guard lock(shared().mutex); s.root = shared().root; s.summary = shared().summary;
            s.started = true; s.began = GetTickCount64(); s.summary.phase = "waiting_verified_owner";s.ride=shared().ride_requested;
        }
        if (!s.started || s.phase == Phase::done) return;
        advance(s, base, rider, gameplay_ready); publish(s);
    } catch (...) {
        if (s.started) try { finish(s, "probe_exception"); } catch (...) {}
    }
}
Snapshot snapshot() { std::lock_guard lock(shared().mutex); return shared().summary; }
RideRoute route() {std::lock_guard lock(shared().mutex);return shared().route;}
}
