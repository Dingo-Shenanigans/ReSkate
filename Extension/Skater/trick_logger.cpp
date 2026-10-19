#include "trick_logger.h"
#include "skate_detector.h"
#include "riding_heading.h"
#include "native_trick_metadata.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "trick_set.h"
#include "trick_match.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/supported_build.h"
#include <Windows.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <thread>
#include <cmath>
#include <map>
#include <cctype>

namespace dingosdk::trick_logger {
namespace {
struct State {
    std::atomic<bool> active{};
    std::atomic<bool> full{};
    std::atomic<std::uint64_t> generation{};
    std::mutex control, mutex;
    std::condition_variable wake;
    struct Entry { std::string json, text; };
    std::deque<Entry> queue;
    std::thread writer;
    std::ofstream file, readable;
    std::string path, text_path, error, name_source;
    std::uint64_t sequence{}, marker{}, dropped{}, written{};
};
// Process lifetime: no blocking thread teardown from DllMain.
State& state() { static auto* s = new State; return *s; }
std::mutex attempt_mutex;
TrickSetTracker tracker;
std::uint64_t attempt_generation{};
std::map<std::uint64_t, Json> attempt_names;
std::atomic<std::uint64_t> physics_steps{}, physics_wipeouts{};
std::atomic<std::uint64_t> hud_sequence_ends{};
std::atomic<int> natural_goofy{-1};
std::mutex feedback_mutex;
std::deque<std::string> feedback_results;
std::string feedback_current;
std::atomic<bool> feedback_visible{false};
std::mutex practice_mutex;
bool practice_armed{},practice_target{};
std::uint64_t practice_after{},practice_target_id{};
MatchSignature practice_signature;
std::string practice_description,practice_verdict;
std::mutex setup_mutex;
SetupFlags setup_state;
bool enqueue(Json value, bool new_marker = false) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.active) return false;
    if (s.queue.size() >= 4096) { ++s.dropped; return false; }
    if (new_marker) ++s.marker;
    value["sequence"] = ++s.sequence;
    value["uptime_ms"] = GetTickCount64();
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    value["qpc_ticks"] = counter.QuadPart;
    value["marker"] = s.marker;
    value["dropped_records"] = s.dropped;
    std::string readable;
    const auto kind = value.value("kind", "");
    if ((kind == "trick_name" || kind == "trick_name_update") && !s.full)
        readable = std::format("[{} ms] #{} r{} {}{}{}", GetTickCount64(),
            value.value("observation_id", std::uint64_t{}), value.value("revision", std::uint64_t{1}),
            value.value("preexisting", false) ? "[already visible] " : "",
            value.value("name_resolved", false) ? "" : "[unresolved] ", value.value("display_name", ""));
    else if (kind == "marker") readable = "MARK: " + value.value("label", "");
    else if (kind == "gameplay_bool_observation" && value.value("key","")=="DefaultStanceIsGoofy") readable=std::format("FOOTING: {}",value.value("value",false)?"goofy":"regular");
    else if (kind == "natural_stance_observation") readable=std::format("STANCE: {} (native IsStanceGoofy input; requires live validation)",value.value("is_goofy",false)?"goofy":"regular");
    else if (kind == "trick_set") readable=std::format("SET {} — {}: {}",value.value("set_id",std::uint64_t{}),value.value("outcome",""),value.value("readable_result",""));
    else if (kind == "practice_result") readable="PRACTICE: "+value.value("verdict","");
    s.queue.push_back({value.dump(), std::move(readable)});
    s.wake.notify_one();
    return true;
}
Json record(const char* kind) { Json j = Json::object(); j["kind"] = kind; return j; }
const bool diagnostics_registered=[] {
    diagnostic_sink.store(+[](const char* kind,Json value) {if(full_capture()){value["kind"]=kind;enqueue(std::move(value));}});return true;
}();
void finish_attempt(const TrickSetResult& result) {
    auto j=record("trick_set");
    j["set_id"]=result.id;j["launch_ms"]=result.start;j["end_ms"]=result.end;j["outcome"]=result.outcome;
    j["landed"]=result.landed?Json(*result.landed):Json{};
    j["rotation_complete"]=result.rotation_complete;
    j["grounded_recovery_travel_degrees"]=result.grounded_travel;
    const auto rules=setup_flags();auto setup=Json::object();setup["known_mask"]=rules.valid;
    const char* rule_names[]={"score_flips","score_grabs","score_grinds","score_ground_tricks","score_slams","allow_offboard","allow_session_markers","force_cannonball"};
    for(unsigned i=0;i<8;++i)if(rules.valid&(1u<<i))setup[rule_names[i]]=(rules.captured&(1u<<i))!=0;
    j["setup_rules"]=std::move(setup);
    j["landing_evidence"]="Riding state 100/103 after touchdown, later physics step, HUD sequence-ended observed, no captured wipeout. Native WasSuccess and four-wheel contact are not decoded.";
    j["association_note"]="Original phase assignment retained across ticker revisions; ground-only grabs, manuals and stalls ignored";
    auto spins=Json::array();std::string readable;
    for(const auto& spin:result.segments) {
        auto s=Json::object();s["air_degrees"]=spin.air;s["landing_degrees"]=spin.landing;
        s["net_degrees"]=spin.air+spin.landing;s["accepted_spin"]=spin.degrees;
        s["accepted_net_degrees"]=spin.accepted_net;s["excluded_ground_degrees"]=spin.excluded_ground;
        s["spin_policy"]="Signed air plus native revert recovery; separate grind entry/exit with inclusive 115-degree minimum; no airborne cap.";
        s["direction"]=spin.direction;s["starting_stance"]=spin.stance;
        s["goofy"]=spin.goofy?Json(*spin.goofy):Json{};s["grind_adjacent"]=spin.grind_adjacent;s["begin_ms"]=spin.begin;s["end_ms"]=spin.end;
        spins.push_back(s);
        if(!readable.empty())readable+=" / ";
        readable+=std::format("{}{}{}",spin.direction.empty()&&spin.degrees?"direction unknown ":spin.direction,
            spin.degrees?" "+std::to_string(spin.degrees):"no body spin",spin.stance.empty()?"":" ("+spin.stance+")");
    }
    j["spins"]=std::move(spins);
    auto phases=Json::array();for(const auto& phase:result.phases) {
        auto p=Json::object();p["id"]=phase.id;p["kind"]=phase.kind;p["segment"]=phase.segment;p["begin_ms"]=phase.begin;p["end_ms"]=phase.end;phases.push_back(p);
    }j["movement_phases"]=phases;
    auto names=Json::array();for(auto& [id,name]:attempt_names) {
        if(name.value("phase_provisional",false))skate::associate_observation(name,tracker,nullptr,result.end);
        names.push_back(name);
    }
    std::string tricks;for(const auto& [id,name]:attempt_names) {
        const auto base=name.value("name","");if(base=="Revert"||base=="Powerslide")continue;
        if(!tricks.empty())tricks+=" + ";tricks+=name.value("display_name","");
    }
    j["readable_result"]=tricks+" | measured: "+readable;
    j["ticker_candidates"]=names;
    const auto normalized=skate::normalize(j,{0xff,true});j["canonical_phases"]=normalized.phases;j["ignored"]=normalized.ignored;
    if(normalized.signature.error.empty())j["readable_result"]=normalized.display;
    {
        std::lock_guard lock(practice_mutex);
        if((practice_armed && result.start>=practice_after) || practice_target) {
            const auto signature=normalized.signature;
            if(practice_armed) {
                if(signature.error.empty()) {
                    practice_signature=signature;practice_target=true;practice_armed=false;practice_target_id=result.id;
                    practice_description=j.value("readable_result","");practice_verdict="TARGET SET";
                } else practice_verdict="TARGET PENDING: "+signature.error;
            } else {
                const auto reason=match_difference(practice_signature,signature);
                practice_verdict=reason.empty()?"MATCH":!result.rotation_complete?"INCOMPLETE: missing rotation samples":!result.landed.has_value()?"INCOMPLETE: "+result.outcome:"MISMATCH: "+reason;
            }
            j["local_match"]=practice_verdict;
            auto comparison=record("practice_result");comparison["set_id"]=result.id;comparison["target_set_id"]=practice_target_id;
            comparison["verdict"]=practice_verdict;comparison["rules_version"]=1;
            auto tokens=Json::array();for(const auto& token:signature.tokens)tokens.push_back(token);
            comparison["signature"]=tokens;enqueue(std::move(comparison));
        }
    }
    {
        std::lock_guard lock(feedback_mutex);
        feedback_current.clear();
        auto line=std::format("#{} {}\n{}",result.id,result.outcome,j.value("readable_result",""));
        if(line.size()>650)line=line.substr(0,647)+"...";
        feedback_results.push_front(std::move(line));
        while(feedback_results.size()>3)feedback_results.pop_back();
    }
    j["ticker_candidates"]=std::move(names);attempt_names.clear();enqueue(std::move(j));
}
template<class T, std::size_t N> Json array(const std::array<T, N>& values) {
    Json result = Json::array();
    for (const auto v : values) result.push_back(v);
    return result;
}
Json transform(const multiplayer::Transform& t) {
    Json j = Json::object();
    j["position_m"] = array(t.position);
    j["rotation_xyzw"] = array(t.rotation);
    return j;
}
Json motion(std::uintptr_t base, std::uintptr_t physics, std::uintptr_t expected, std::uint32_t count) {
    namespace layout = game::build::v20260929::skater_state;
    Json j = Json::object();
    std::uintptr_t vtable{}, bodies{}, owner{};
    std::uint32_t actual{};
    std::array<float, 3> velocity{}, spin{};
    const bool valid = memory::peek(physics, vtable) && vtable == base + expected &&
        memory::peek(physics + layout::physics_bodies_offset, bodies) && memory::peek(bodies, actual) && actual == count &&
        memory::peek(bodies + layout::body_owner_offset, owner) && owner == physics &&
        memory::peek(bodies + layout::body_velocity_offset, velocity) && memory::peek(bodies + layout::body_spin_offset, spin);
    j["known"] = valid;
    if (valid) { j["velocity_mps"] = array(velocity); j["angular_velocity_radps"] = array(spin); }
    return j;
}
void write_loop() noexcept {
    auto& s = state();
    try {
        std::uint64_t bytes{};
        for (;;) {
            State::Entry line;
            {
                std::unique_lock lock(s.mutex);
                s.wake.wait(lock, [&] { return !s.active || !s.queue.empty(); });
                if (s.queue.empty()) break;
                line = std::move(s.queue.front()); s.queue.pop_front();
            }
            s.file << line.json << '\n';
            if (!line.text.empty()) s.readable << line.text << '\n';
            bytes += line.json.size() + line.text.size() + 2;
            bool flush{};
            {
                std::lock_guard lock(s.mutex);
                ++s.written;
                flush = !line.text.empty() || s.written % 60 == 0;
            }
            if (flush) { s.file.flush(); s.readable.flush(); }
            if (!s.file || !s.readable || bytes >= 256ULL * 1024 * 1024) {
                std::lock_guard lock(s.mutex);
                s.error = !s.file || !s.readable ? "File write failed" : "256 MiB session limit reached";
                s.active = false;
                s.dropped += s.queue.size();
                s.queue.clear();
                break;
            }
        }
        s.file.flush();
        s.readable.flush();
        if (!s.file || !s.readable) { std::lock_guard lock(s.mutex); s.error = "Final file flush failed"; }
        s.file.close(); s.readable.close();
    } catch (...) {
        std::lock_guard lock(s.mutex);
        s.error = "Writer failed"; s.active = false;
    }
}
}
bool enabled() noexcept { return state().active.load(); }
SetupFlags setup_flags() noexcept {std::lock_guard lock(setup_mutex);return setup_state;}
void observe_setup_menu(std::uint8_t mask,std::uint8_t valid) noexcept {
    bool changed{};
    {std::lock_guard lock(setup_mutex);changed=setup_state.captured!=mask || setup_state.valid!=valid;
        setup_state.valid=valid;setup_state.captured=mask;}
    if(changed)try {auto j=Json::object();j["source"]="active_skate_match";j["mask"]=mask;j["valid_mask"]=valid;setup_snapshot(std::move(j));}catch(...){}
}
std::string feedback_text() noexcept {
    if(!full_capture() || !feedback_visible.load())return {};
    try {
        std::lock_guard lock(feedback_mutex);
        std::string text="Trick detector (client landing rule)\n";
        text+=feedback_current.empty()?"Ready\n":feedback_current+"\n";
        {
            std::lock_guard practice_lock(practice_mutex);
            if(practice_armed)text+="Practice: land the next target set\n";
            if(practice_target)text+="Target #"+std::to_string(practice_target_id)+": "+practice_description.substr(0,350)+"\n";
            if(!practice_verdict.empty())text+=practice_verdict+"\n";
        }
        for(const auto& line:feedback_results)text+="\n"+line+"\n";
        return text;
    }catch(...){return {};}
}
bool gameplay_capture() noexcept { return full_capture() || skate::active(); }
bool full_capture() noexcept { return enabled() && state().full.load(); }
std::uint64_t generation() noexcept { return state().generation.load(); }
bool named_trick(Json value) noexcept {
    try {
        skate::observe(value);
        if(full_capture()) {
            std::lock_guard lock(attempt_mutex);
            if(attempt_generation==generation() && tracker.active_id() && !value.value("preexisting",false)) {
                value["candidate_attempt_id"]=tracker.active_id();
                const auto id=value.value("observation_id",std::uint64_t{});const auto previous=attempt_names.find(id);
                if(skate::associate_observation(value,tracker,previous==attempt_names.end()?nullptr:&previous->second,GetTickCount64()) &&
                   (attempt_names.size()<128 || previous!=attempt_names.end()))attempt_names[id]=value;
                {std::lock_guard feedback_lock(feedback_mutex);feedback_current="Tracking: "+value.value("display_name","");}
            }
        }
        return enqueue(std::move(value)) || skate::active();
    } catch (...) { return false; }
}
void name_status(std::string text) noexcept {
    try {
        { std::lock_guard lock(state().mutex); if (state().name_source == text) return; state().name_source = text; }
        auto j = record("name_source_status"); j["message"] = std::move(text); enqueue(std::move(j));
    } catch (...) {}
}
std::string command(std::string_view args) {
    auto& s = state();
    std::lock_guard control(s.control);
    if(args=="practice set" || args=="practice clear" || args=="practice off" || args=="practice status") {
        if(args=="practice set" && !full_capture())return "Start trick-log on full first, then trick-log practice set.";
        std::lock_guard lock(practice_mutex);
        if(args=="practice status")return practice_armed?"Practice: next landed recognized set will be the target.":practice_target?"Practice target: "+practice_description+" | "+practice_verdict:"Practice is off.";
        practice_armed=args=="practice set";practice_target=false;practice_signature={};practice_target_id=0;
        practice_after=GetTickCount64();practice_description.clear();practice_verdict.clear();
        return practice_armed?"Practice armed: your next landed recognized set becomes the target. Repeat it to compare.":"Practice target cleared.";
    }
    if(args=="feedback on" || args=="feedback off") {
        feedback_visible=args=="feedback on";
        return feedback_visible?"Trick feedback visible during trick-log on full.":"Trick feedback hidden.";
    }
    if (args == "on" || args == "on full") {
        if (s.active) return "Trick logger already recording: " + s.path;
        if (s.writer.joinable()) s.writer.join();
        const auto folder = std::filesystem::current_path() / "logs" / "tricks";
        std::filesystem::create_directories(folder);
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        LARGE_INTEGER unique{}; QueryPerformanceCounter(&unique);
        const auto name = std::format("tricks-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}-{}.jsonl",
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds, GetCurrentProcessId(), unique.QuadPart);
        s.path = (folder / name).string();
        s.file.clear(); s.file.open(s.path, std::ios::binary | std::ios::trunc);
        if (!s.file) return "error: Cannot open " + s.path;
        s.text_path = (folder / std::filesystem::path(name).replace_extension(".txt")).string();
        s.readable.clear(); s.readable.open(s.text_path, std::ios::binary | std::ios::trunc);
        if (!s.readable) { s.file.close(); return "error: Cannot open " + s.text_path; }
        s.readable << "Native local trick-ticker observations. Names alone do not establish a landed outcome.\n";
        {
            std::lock_guard lock(s.mutex);
            s.queue.clear(); s.sequence = s.marker = s.dropped = s.written = 0; s.error.clear();
            s.name_source = "Waiting for native ticker capture";
            s.full = args == "on full";
            ++s.generation;
            s.active = true;
        }
        {std::lock_guard lock(feedback_mutex);feedback_current.clear();feedback_results.clear();}
        try { s.writer = std::thread(write_loop); }
        catch (...) { s.active = false; s.file.close(); s.readable.close(); throw; }
        auto j = record("session");
        j["schema"] = 14; j["game_sha256"] = supported_build::game_sha256;
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        j["qpc_frequency"] = frequency.QuadPart;
        j["client_build"] = "2.1.0-skate.10";
        j["native_trace_note"]="Client trick sets use measured signed spin and physical phases, HUD sequence completion and captured wipeout evidence. Native WasSuccess not decoded; ticker/sequence ownership association needs live validation.";
        j["capture_mode"] = s.full ? "full" : "names";
        j["rotation_note"] = "Raw transforms; no inferred FS/BS or spin-count claim";
        j["body_candidate_note"] = "rig+0x4720 from first-person mod; axes and heading require riding validation. Yaw is atan2(axis.x,axis.z), degrees, Y-up; not FS/BS.";
        j["test_note"] = "Mark natural footing, intended trick, starting stance and auto-revert setting. Capture at least one second before launch and after landing.";
        enqueue(std::move(j));
        return "Trick logger recording: " + s.text_path + " (readable names) and " + s.path + ". Use trick-log status to check native capture; trick-log off to finish.";
    }
    if (args == "off") {
        { std::lock_guard lock(attempt_mutex); if(attempt_generation==generation())
            if(auto result=tracker.stop(GetTickCount64())) finish_attempt(*result); }
        enqueue(record("session_end"));
        { std::lock_guard lock(s.mutex); s.active = false; }
        s.wake.notify_one();
        if (s.writer.joinable()) s.writer.join();
        std::lock_guard lock(s.mutex);
        return std::format("Trick logger stopped. {} records; {} dropped. {} {}", s.written, s.dropped, s.path, s.error);
    }
    if (args.starts_with("mark ") && args.size() > 5) {
        if (!s.active) return "error: Start with trick-log on.";
        if (args.size() > 261) return "error: Label must be at most 256 characters.";
        auto j = record("marker"); j["label"] = args.substr(5);
        if (!enqueue(std::move(j), true)) return "error: Marker was not saved; check trick-log status and retry.";
        return "Attempt marker saved: " + std::string(args.substr(5));
    }
    if (args.empty() || args == "status") {
        std::lock_guard lock(s.mutex);
        return std::format("Trick logger {} ({}). Written {}; queued {}; dropped {}. {} {} {}",
            s.active ? "ON" : "OFF", s.full ? "full" : "names", s.written, s.queue.size(), s.dropped, s.text_path, s.name_source, s.error);
    }
    return "Usage: trick-log on full | practice set/clear/status | feedback on/off | mark <label> | status | off";
}
void sample(const multiplayer::NativeFrame& frame, std::string_view map) noexcept {
    if (!full_capture() && !skate::active()) return;
    try {
        auto j = record("sample"); j["map"] = map; j["ready"] = frame.ready;
        j["native_physics_steps"]=physics_steps.load();j["native_step_wipeouts"]=physics_wipeouts.load();
        if (frame.ready) {
            j["skater"] = transform(frame.pose.root);
            if (frame.board_root) j["board"] = transform(*frame.board_root);
            const auto watch = watched_physics_state();
            j["physics_known"] = watch.valid;
            if (watch.valid) {
                j["physics_state"] = watch.state; j["previous_state"] = watch.previous;
                j["previous_seconds"] = watch.previous_seconds; j["state_changes"] = watch.changes;
                j["wipeouts"] = watch.wipeouts;
            }
            NoBailSkater native;
            if (no_bail_skater(native) && native.entity == frame.entity) {
                namespace layout = game::build::v20260929::skater_state;
                j["riding_body_candidate_known"] = false;
                std::array<float,16> body{};
                if (native.rig && memory::peek(native.rig + 0x4720, body)) {
                    bool valid = true;
                    for (float v : body) valid = valid && std::isfinite(v);
                    for (unsigned axis=0; axis<3; ++axis) {
                        const auto i=axis*4;
                        const float n=body[i]*body[i]+body[i+1]*body[i+1]+body[i+2]*body[i+2];
                        valid = valid && n>.5f && n<1.5f;
                    }
                    j["riding_body_candidate_known"] = valid;
                    if (valid) {
                        j["riding_body_candidate_matrix"] = array(body);
                        if(const auto heading=riding_heading(body)) {
                            j["riding_heading_yaw_deg"]=*heading;j["riding_heading_source"]="body_right_axis";
                        }
                        // Record both axes: which one corresponds to riding heading must be verified.
                        for (const auto axis : {0u,2u}) {
                            const unsigned i=axis*4;
                            const float horizontal=std::hypot(body[i],body[i+2]);
                            const auto prefix=axis==0 ? "body_right" : "body_forward";
                            j[std::string(prefix)+"_horizontal_length"] = horizontal;
                            if(horizontal>.1f)
                                j[std::string(prefix)+"_yaw_deg"] = std::atan2(body[i],body[i+2])*57.29577951308232f;
                        }
                    }
                }
                std::uint32_t selected{};
                if(memory::peek(native.context + layout::physics_state_offset, selected)) {
                    j["selected_physics_state"] = selected;
                    j["riding_airborne"] = selected==200 || selected==201;
                    j["offboard"] = selected==504;
                }
                std::uintptr_t holder{}, physics{}, trick{}, offboard{};
                std::uintptr_t animation{};
                // Existing validated animation publisher fields documented by no_bail.h.
                if(memory::peek(native.core+layout::trick_state_offset,trick) && memory::peek(trick+0x38,animation)) {
                    for(const auto offset : {0x87u,0x8du,0x9eu}) {
                        std::uint8_t flag{};
                        if(memory::peek(animation+offset,flag) && flag<=1)
                            j[offset==0x87?"animation_wipeout_request":offset==0x8d?"animation_wipeout":"animation_recovery_runout"]=flag;
                    }
                }
                if (memory::peek(native.core + layout::board_holder_offset, holder) &&
                    memory::peek(holder + layout::board_physics_offset, physics))
                    j["board_root_motion"] = motion(native.base, physics, layout::board_physics_vtable, layout::board_body_count);
                std::uint8_t ragdoll{}, air{};
                if (memory::peek(native.core + layout::trick_state_offset, trick) &&
                    memory::peek(trick + layout::offboard_state_offset, offboard) &&
                    memory::peek(offboard + layout::ragdoll_offset, ragdoll) &&
                    memory::peek(offboard + layout::in_the_air_offset, air)) {
                    j["offboard_ragdoll_raw"] = ragdoll; j["offboard_air_raw"] = air;
                }
            }
        } else j["detail"] = frame.detail;
        {
            std::lock_guard lock(attempt_mutex);
            if(attempt_generation!=generation()) { tracker={};attempt_names.clear();attempt_generation=generation(); }
            const auto* heading=j.contains("riding_heading_yaw_deg")?&j.at("riding_heading_yaw_deg"):nullptr;
            const auto* selected=j.contains("selected_physics_state")?&j.at("selected_physics_state"):nullptr;
            const auto code=selected ? selected->get<std::uint32_t>():0;
            SetSample sample;sample.now=GetTickCount64();sample.steps=physics_steps.load();sample.wipeouts=physics_wipeouts.load();
            sample.sequence_ends=hud_sequence_ends.load();sample.entity=frame.entity;sample.state=code;
            sample.position=frame.pose.root.position;sample.position_known=frame.ready;
            sample.valid=frame.ready && heading && j.value("riding_body_candidate_known",false);
            sample.yaw=heading?heading->get<double>():0;sample.ragdoll=code==504 && j.value("offboard_ragdoll_raw",0u)!=0;
            const int footing=natural_goofy.load();if(footing>=0)sample.goofy=footing!=0;
            skate::sample(sample);
            if(full_capture())if(auto result=tracker.update(sample)) finish_attempt(*result);
            j["candidate_attempt_id"]=tracker.active_id();
            j["native_hud_sequence_ends"]=sample.sequence_ends;
            if(footing>=0)j["natural_goofy"]=footing!=0;
        }
        enqueue(std::move(j));
    } catch (...) {}
}
void physics_transition(std::uint32_t before,std::uint32_t after) noexcept {
    try {if(skate::active())skate::physics_transition(before,after,GetTickCount64());}catch(...){}
    if(!full_capture()) return;
    try { auto j=record("native_physics_transition");j["before"]=before;j["after"]=after;
        j["pre_offboard_remap"]=true;enqueue(std::move(j)); } catch(...) {}
}
void physics_step(std::uintptr_t entity,float seconds,bool wipeout) noexcept {
    if(!full_capture() && !skate::active()) return;
    ++physics_steps;
    if(!wipeout) return;
    ++physics_wipeouts;
    try {auto j=record("native_wipeout");j["entity_raw"]=entity;j["step_seconds"]=seconds;
        j["after_no_bail_filter"]=true;enqueue(std::move(j));}catch(...){}
}
void native_capture_trace(bool ignored,bool banned,bool allowed,std::uint32_t first_valid) noexcept {
    if(!full_capture())return;
    try {auto j=record("native_skate_capture_trace");j["ignored"]=ignored;j["banned"]=banned;j["allowed"]=allowed;
        j["first_valid_scorable_id"]=first_valid;j["stage"]="native scoring graph completed";
        j["note"]="Verified graph locals; does not read or modify area membership or AttemptDone";
        enqueue(std::move(j));}catch(...){}
}
void native_observation(std::string_view name,bool original,bool selected) noexcept {
    if(!full_capture())return;
    try {auto j=record("native_skate_observation");j["scorable_name"]=name;
        j["original_ignored"]=original;j["detector_ignored"]=selected;
        bool known=false;for(const auto& entry:skate::native_tricks)if(entry.name==name) {
            j["native_identity"]=entry.identity;j["native_category_mask"]=entry.categories;
            if(selected && skate::slappy_entry(entry.identity))j["exclusion_reason"]="slappy entry observation";
            known=true;break;
        }
        j["classification"]=!known?"unrecognized observation":selected?"excluded movement":"eligible core observation";
        j["source"]="ClientSkateThrowdown.OnCompositeTrickScoringEvent ignored-trick membership before FirstValidTrick";
        enqueue(std::move(j));}catch(...){}
}
void native_filter(std::uint32_t category,bool original,bool selected) noexcept {
    if(!full_capture())return;
    try {auto j=record("native_skate_category_filter");j["native_category"]=category;
        j["original_allowed"]=original;j["detector_allowed"]=selected;
        j["source"]="ClientSkateThrowdown.OnCompositeTrickScoringEvent allowed-category membership";enqueue(std::move(j));}catch(...){}
}
void scoring_expression(std::uintptr_t vm,std::uint32_t pc,bool after) noexcept {
    if(!full_capture() && !skate::active() && !multiplayer::local_throwdown_active()) return;
    try {
        std::uintptr_t resource{},instance{};std::uint32_t key{};
        if(!memory::peek(vm+0x38,resource)||!memory::peek(resource+0x10,key))return;
        struct Graph {std::uint32_t hash;const char* name;std::array<std::uint32_t,10> layout;};
        static constexpr Graph graphs[]{
            {0xc882a12b,"UpdateStanceInPlayerInfo",{48,0,0,14,4,0,0,1,0,0}},
            {0x6decdf7a,"PlayerCard.OnLocalPlayerDefaultStanceIsGoofyChanged",{64,184,26,66,5,0,3,327683,720896,0}},
            {0xe6b5042d,"ScoringHud.OnScorableSequenceEnded",{80,528,203,331,6,0,6,1310735,2883607,0}},
            {0xafef12e9,"ScoringTelemetry.OnScorableSequenceEnded",{1136,2312,2870,4094,14,0,7,2293991,6750242,67584}},
            {0xd33b687d,"SequenceScoringManager.OnEndScorableSequence",{144,5296,8105,3424,15,0,29,1310881,7274533,395265}},
            {0x54a48b08,"LineScoringManager.OnEndScorableLine",{80,944,652,1219,11,0,4,1310784,3670049,133120}},
            {0x174ead43,"TrickScoringManager.OnScoringActionStarted",{256,3928,3960,5287,21,0,33,1966389,11075629,17041920}},
            {0x9046b003,"TrickScoringManager.OnScoringActionEnded",{1344,1344,3559,1526,8,0,4,1966147,5373985,919552}},
            {0xdcd5d079u,"ClientSkateThrowdown.OnScorableSequenceEnded",{64,48,10,83,5,0,0,327682,11,2048}}
        };
        for(const auto& graph:graphs) {
            if(key!=graph.hash)continue;
            std::array<std::uint32_t,10> layout{};std::uintptr_t check{},inputs{};
            if(!memory::peek(resource+0x20,layout)||layout!=graph.layout||!memory::peek(vm+0x30,instance)||
                !memory::peek(instance,check)||check!=resource) return;
            if(key==0xe6b5042d && after && pc==0)++hud_sequence_ends;
            auto j=record("native_scoring_expression");j["graph_hash"]=key;j["name"]=graph.name;
            j["phase"]=after?"exit":"entry";j["pc"]=pc;j["vm_raw"]=vm;j["instance_raw"]=instance;
            j["owner_verified"]=false;j["payload_decoded"]=false;
            if(memory::peek(instance+((layout[0]+15u)&~15u)+8,inputs)) {
                // This standalone expression has exactly one Boolean input, page 1 offset 0.
                if(key==0xc882a12b && !after) {
                    std::uint8_t goofy{};
                    if(memory::peek(inputs,goofy) && goofy<=1) {
                        auto stance=record("natural_stance_observation");stance["is_goofy"]=goofy!=0;
                        stance["source"]="UpdateStanceInPlayerInfo.IsStanceGoofy";
                        stance["mapping_live_verified"]=false;enqueue(std::move(stance));
                    }
                }
                std::array<std::uint64_t,2> words{};
                if(memory::peek(inputs,words)) {
                    j["input_words_raw"]=array(words);
                    auto snapshots=Json::array();
                    for(const auto pointer:words) {
                        std::array<std::uint8_t,64> bytes{};
                        auto snapshot=Json::object();snapshot["address_raw"]=pointer;
                        if(pointer>=0x10000 && pointer<0x00007fffffff0000ULL && memory::peek(pointer,bytes))
                            snapshot["bytes_raw"]=array(bytes);
                        snapshots.push_back(std::move(snapshot));
                    }
                    j["input_pointee_candidates"]=std::move(snapshots);
                }
            }
            enqueue(std::move(j));return;
        }
    }catch(...){}
}
void gameplay_bool(std::string_view key,bool value,std::string_view source) noexcept {
    if(key=="DefaultStanceIsGoofy")natural_goofy.store(value?1:0);
    if(!full_capture() || key.size()>256) return;
    try {
        std::string lower(key);for(auto& c:lower)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(lower.find("stance")==std::string::npos && lower.find("goofy")==std::string::npos &&
           lower.find("skate")==std::string::npos && lower.find("trick")==std::string::npos &&
           lower.find("grab")==std::string::npos && lower.find("grind")==std::string::npos &&
           lower.find("manual")==std::string::npos && lower.find("allow")==std::string::npos)return;
        static std::mutex mutex;static std::map<std::string,bool> seen;static std::uint64_t epoch{};
        {std::lock_guard lock(mutex);if(epoch!=generation()){seen.clear();epoch=generation();}
            const auto it=seen.find(std::string(key));if(it!=seen.end()&&it->second==value)return;
            if(seen.size()>=64 && it==seen.end())return;seen[std::string(key)]=value;}
        auto j=record("gameplay_bool_observation");j["key"]=key;j["value"]=value;j["source"]=source;
        enqueue(std::move(j));
    }catch(...){}
}
void detected_attempt(Json value) noexcept {
    if(!enabled())return;
    try {auto j=record("detected_attempt");j["result"]=std::move(value);enqueue(std::move(j));}catch(...){}
}
void setup_snapshot(Json value) noexcept {
    if(!full_capture())return;
    try {value["kind"]="game_setup_snapshot";enqueue(std::move(value));}catch(...){}
}
void event(std::uint32_t hash) noexcept {
    if (!full_capture()) return;
    try { auto j = record("network_event"); j["hash"] = hash; enqueue(std::move(j)); } catch (...) {}
}
void attempt(bool landed, const std::array<std::uint8_t, 28>& bytes) noexcept {
    if (!enabled()) return;
    try {
        auto j = record("skate_attempt"); j["landed"] = landed; j["raw_bytes"] = array(bytes);
        const auto u32 = [&](std::size_t offset) { std::uint32_t v{}; std::memcpy(&v, bytes.data() + offset, 4); return v; };
        j["stance_raw"] = u32(0); j["direction_raw"] = u32(4); j["spins_raw"] = u32(8);
        j["held_flip_bits"] = u32(12); j["late_raw"] = bytes[16];
        j["scorable_id"] = u32(20); j["held_category_raw"] = u32(24);
        enqueue(std::move(j));
    } catch (...) {}
}
void score(std::uint32_t player, bool add, float value, std::uint32_t graph) noexcept {
    if (!full_capture()) return;
    try {
        auto j = record("leaderboard_score"); j["player_raw"] = player; j["add"] = add;
        j["value"] = value; j["graph_hash"] = graph; enqueue(std::move(j));
    } catch (...) {}
}
}
