#include "skate_detector.h"
#include "trick_diagnostics.h"
#include "native_trick_metadata.h"
#include <atomic>
#include <Windows.h>
#include <map>
#include <set>
namespace dingosdk::skate {
namespace {
struct State {
    std::mutex mutex;
    std::atomic<bool> enabled{},bridge_failed{};
    bool recovery_seen{};
    std::uint32_t native_state{};
    std::uint64_t turn_started{};
    Rules rules;
    struct Phase {std::uint64_t begin,end;};
    std::vector<Phase> reverts;
    bool own{},done{},sampled{},native_eligible{};
    std::optional<std::uint64_t> wipeouts;
    std::vector<std::string> diagnostics;
    std::set<std::uint64_t> excluded;
    bool boundary_logged{};
    std::uint64_t serial{};
    trick_logger::TrickSetTracker tracker{true,true,false};
    std::map<std::uint64_t,Json> rows;
    std::optional<DetectedAttempt> ready;
    std::optional<std::array<std::uint8_t,28>> legacy;
    std::map<std::uint32_t,DetectedAttempt> pending;
    std::vector<std::string> target;
    std::string target_display;
    Json target_evidence{Json::object()};
    std::array<std::uint8_t,28> target_native{};
    std::set<std::vector<std::string>> history;
};
State& state(){static State s;return s;}
void fail(State& s,Category c=Category::none) {
    if(s.done)return;
    DetectedAttempt result;result.id=++s.serial;result.violation=c;
    s.ready=std::move(result);s.done=true;
    s.diagnostics.push_back(c==Category::none?"Attempt failed":forbidden(c));
}
}
bool active() noexcept{return state().enabled.load();}
void bridge_error() noexcept{state().bridge_failed=true;}
bool take_bridge_error() noexcept{return state().bridge_failed.exchange(false);}
void begin(Rules rules) {
    end();auto& s=state();std::lock_guard lock(s.mutex);s.rules=rules;s.tracker=trick_logger::TrickSetTracker{true,true,rules.allows(Category::grinds)};s.enabled=rules.verified;
}
void end() {
    auto& s=state();std::lock_guard lock(s.mutex);s.enabled=false;s.own=s.done=false;s.bridge_failed=false;
    s.tracker=trick_logger::TrickSetTracker{true,true,s.rules.allows(Category::grinds)};s.rows.clear();s.ready.reset();s.legacy.reset();s.pending.clear();
    s.native_eligible=false;s.reverts.clear();s.wipeouts.reset();s.diagnostics.clear();s.excluded.clear();s.boundary_logged=false;
    s.target.clear();s.target_display.clear();s.target_evidence=Json::object();s.history.clear();s.serial=0;
}
void turn(bool own) {
    auto& s=state();std::lock_guard lock(s.mutex);s.recovery_seen=false;s.reverts.clear();s.native_state=0;s.own=own;s.done=false;s.sampled=false;s.turn_started=GetTickCount64();
    s.tracker=trick_logger::TrickSetTracker{true,true,s.rules.allows(Category::grinds)};s.rows.clear();s.ready.reset();s.legacy.reset();
    s.native_eligible=false;s.wipeouts.reset();
    s.excluded.clear();s.boundary_logged=false;
    s.diagnostics.push_back(own?"Own turn started; gameplay capture active without logger commands":"Opponent turn started; local detection paused");
}
void observe(const Json& input) {
    auto& s=state();std::lock_guard lock(s.mutex);
    if(!s.enabled || !s.own || s.done || input.value("preexisting",false))return;
    if(input.contains("first_seen_ms") && input.at("first_seen_ms").get<std::uint64_t>()<s.turn_started)return;
    if(!input.contains("raw_labels"))return;
    const auto& labels=trick_names::matching_labels(input);const auto id=labels.value("base_name","");
    auto row=input;
    const auto seen=input.value("first_seen_ms",GetTickCount64());
    for(const auto& phase:s.reverts)if(seen>=phase.begin && (!phase.end || seen<phase.end))row["landing_revert_episode"]=phase.begin;
    const auto c=category(id);
    // Disabled and ignored observations cannot initialize a continuation,
    // including a manual's provisional label before its revert update arrives.
    if(ignored_reason(row).empty() && s.rules.allows(c) && !s.tracker.active_id() && (c==Category::ground || c==Category::grinds)) {
        s.ready.reset();s.rows.clear();s.legacy.reset();s.native_eligible=false;s.recovery_seen=false;
        s.tracker.begin_continuation();
        s.excluded.clear();s.boundary_logged=false;
    }
    const auto key=row.value("observation_id",std::uint64_t{});const auto previous=s.rows.find(key);
    const bool associated=associate_observation(row,s.tracker,previous==s.rows.end()?nullptr:&previous->second,GetTickCount64());
    auto ignored=ignored_reason(row);
    if(ignored.empty() && c!=Category::unknown && !s.rules.allows(c) && !eligible_body_flip(row,s.rules))ignored="disabled category";
    if(c==Category::grabs && !associated) {
        // Without a live attempt, verified ground-only grabbing is not a set.
        if(!s.tracker.active_id())ignored="grounded grab without an active trick";
        else return; // Ambiguous phase evidence cannot grant a result.
    }
    if(!ignored.empty()) {
        if(previous==s.rows.end())s.diagnostics.push_back("Ignored "+id+": "+ignored);
    }
    if(!s.tracker.active_id() || !associated)return;
    if(ignored.empty() && c!=Category::unknown && !row.value("phase_provisional",false)) {
        const auto before=s.tracker.capture();s.tracker.retain_from(row.value("phase_id",std::uint64_t{}));
        const auto capture=s.tracker.capture();
        if(capture.first!=before.first)s.diagnostics.push_back("Retaining combination from physical phase "+std::to_string(capture.first));
        if(s.tracker.excessive_recovery()){fail(s,Category::grounded_rotation);return;}
    }
    const auto span=s.tracker.capture();
    if(span.first && !span.contains(row.value("phase_id",std::uint64_t{}))) {
        if(s.excluded.size()<128 && s.excluded.insert(key).second)s.diagnostics.push_back("Excluded continuation '"+id+"' in phase "+std::to_string(row.value("phase_id",std::uint64_t{})));
        return;
    }
    // Ignored observations must never fill the bounded storage ahead of a core
    // trick. The original ticker/logging stream still preserves every raw row.
    if(s.rows.size()>=128 && previous==s.rows.end() && ignored.empty()) {
        const auto disposable=std::find_if(s.rows.begin(),s.rows.end(),[&](const auto& item) {
            const auto& r=item.second;const auto c=category(trick_names::matching_labels(r).value("base_name",""));
            return !ignored_reason(r).empty() || (c!=Category::unknown && !s.rules.allows(c) && !eligible_body_flip(r,s.rules));
        });
        if(disposable!=s.rows.end())s.rows.erase(disposable);
        else {s.diagnostics.push_back("Too many retained trick observations; attempt failed without truncating its identity");fail(s);return;}
    }
    if(s.rows.size()<128 || previous!=s.rows.end()) {
        if(previous==s.rows.end())s.diagnostics.push_back("Observed trick '"+input.at("raw_labels").value("base_name","")+"', native identity '"+id+"', phase "+std::to_string(row.value("phase_id",std::uint64_t{})));
        s.rows[key]=std::move(row);
    }
}
void sample(trick_logger::SetSample sample) {
    auto& s=state();std::lock_guard lock(s.mutex);if(!s.enabled || !s.own || s.done)return;
    s.native_state=sample.state;
    if(!s.sampled){
        s.sampled=true;s.diagnostics.push_back("First gameplay sample: state="+std::to_string(sample.state)+
            ", physics_steps="+std::to_string(sample.steps)+", rotation_valid="+(sample.valid?"yes":"no"));
    }

    const bool bailed=sample.state==300 || sample.ragdoll || (s.wipeouts && sample.wipeouts>*s.wipeouts);
    s.wipeouts=sample.wipeouts;
    if(bailed){fail(s);return;}
    const auto before=s.tracker.active_id();const auto segment_before=s.tracker.segment_id();auto finished=s.tracker.update(sample);
    const auto span=s.tracker.capture();
    if(span.closed && !s.boundary_logged){s.boundary_logged=true;s.diagnostics.push_back("Combination capture closed after phase "+std::to_string(span.last)+"; awaiting native landing/eligibility");}
    if(s.tracker.segment_id()!=segment_before)s.recovery_seen=false;
    if(sample.state==101 && (s.tracker.active_id() || s.ready))s.recovery_seen=true;
    if(!before && s.tracker.active_id()) {
        if(s.ready)s.diagnostics.push_back("Previous completed trick lacked native submission eligibility; new attempt started");
        s.diagnostics.push_back("Physical attempt started");
        s.rows.clear();s.ready.reset();s.legacy.reset();s.native_eligible=false;
        s.excluded.clear();s.boundary_logged=false;
    }
    if(!finished)return;
    if(finished->landed==false){
        if(finished->outcome=="excessive_grounded_rotation") {
            s.diagnostics.push_back("Grounded rotation travelled "+std::to_string(finished->grounded_travel)+" degrees; recovery allowance exceeded");
            fail(s,Category::grounded_rotation);
        } else fail(s);
        return;
    }
    if(finished->landed!=true || !finished->rotation_complete){
        s.diagnostics.push_back(!finished->rotation_complete?"Completed attempt rejected: incomplete rotation evidence":"Attempt interrupted: "+finished->outcome);
        s.rows.clear();return;
    }
    auto result=Json::object();result["landed"]=true;result["rotation_complete"]=true;
    auto names=Json::array();for(auto& [id,row]:s.rows) {
        if(row.value("phase_provisional",false))associate_observation(row,s.tracker,nullptr,finished->end);
        names.push_back(row);
    }result["ticker_candidates"]=names;
    auto spins=Json::array();for(const auto& spin:finished->segments) {
        auto j=Json::object();j["accepted_spin"]=spin.degrees;j["direction"]=spin.direction;j["starting_stance"]=spin.stance;
        j["net_degrees"]=spin.accepted_net;j["goofy"]=spin.goofy?Json(*spin.goofy):Json{};j["grind_adjacent"]=spin.grind_adjacent;j["rotation_complete"]=spin.rotation_complete;j["recovery_travel"]=spin.recovery_travel;spins.push_back(j);
    }result["spins"]=spins;
    auto phases=Json::array();for(const auto& phase:finished->phases) {
        auto p=Json::object();p["id"]=phase.id;p["kind"]=phase.kind;p["segment"]=phase.segment;p["begin"]=phase.begin;p["end"]=phase.end;phases.push_back(p);
    }result["movement_phases"]=phases;
    const auto normalized=normalize(result,s.rules);
    result["canonical_phases"]=normalized.phases;result["ignored"]=normalized.ignored;
    auto capture=Json::object();capture["first_phase"]=normalized.capture.first;capture["last_phase"]=normalized.capture.last;capture["closed"]=normalized.capture.closed;
    result["capture_boundary"]=capture;
    trick_logger::detector_evidence(std::move(result));s.rows.clear();
    if(normalized.violation!=Category::none){fail(s,normalized.violation);return;}
    if(!normalized.signature.error.empty()){s.diagnostics.push_back("Completed attempt rejected: "+normalized.signature.error);return;}
    DetectedAttempt attempt;attempt.id=++s.serial;attempt.landed=true;
    attempt.evidence["capture_boundary"]=capture;
    attempt.signature=normalized.signature.tokens;attempt.display=normalized.display;attempt.evidence["physical_attempt"]=finished->id;
    std::set<std::size_t> retained_segments;for(const auto& phase:finished->phases)
        if(phase.kind=="air" && normalized.capture.contains(phase.id))retained_segments.insert(phase.segment);
    auto measurements=Json::array();for(std::size_t i=0;i<finished->segments.size();++i) {
        if(!retained_segments.contains(i+1))continue;const auto& spin=finished->segments[i];
        auto values=Json::array();for(const double value:{spin.air,spin.landing,spin.accepted_net})values.push_back(std::round(value*10)/10);measurements.push_back(values);
    }attempt.evidence["measured_air_recovery_net"]=measurements;
    auto association=Json::array();for(const auto& phase:finished->phases) {
        if(phase.kind=="ground" || !normalized.capture.contains(phase.id))continue;auto p=Json::array();p.push_back(phase.kind);p.push_back(phase.id);p.push_back(phase.segment);association.push_back(p);
    }attempt.evidence["phase_association"]=association;
    auto ignored=Json::object();for(const auto& item:normalized.ignored) {
        const auto reason=item.value("reason","");ignored[reason]=ignored.value(reason,0u)+1;
    }attempt.evidence["ignored_counts"]=ignored;
    s.ready=std::move(attempt);
    s.done=s.native_eligible;
    s.diagnostics.push_back(s.done?"Detected landed attempt ready; native eligibility confirmed":"Detected landed attempt waiting for native S.K.A.T.E. submission");
}
void physics_transition(std::uint32_t before,std::uint32_t after,std::uint64_t now) {
    auto& s=state();std::lock_guard lock(s.mutex);if(!s.enabled)return;
    s.native_state=after;
    if(before==101 && !s.reverts.empty() && !s.reverts.back().end)s.reverts.back().end=now;
    if(after==101){if(s.reverts.size()>=128)s.reverts.erase(s.reverts.begin());s.reverts.push_back({now,0});}
}
std::optional<bool> native_ignore_observation(std::string_view name) {
    auto& s=state();std::lock_guard lock(s.mutex);
    if(!s.enabled || !s.own || s.done)return {};
    const NativeTrickMetadata* known{};
    for(const auto& entry:native_tricks)if(entry.name==name){known=&entry;break;}
    if(!known)return true; // Unknown evidence cannot seed the native attempt.
    const auto id=known->identity;
    const bool ignored=(category(id)!=Category::unknown && !s.rules.allows(category(id))) || id=="ID_TRICK_POWERSLIDE" || id=="ID_TRICK_TRICKLESS_AIR" ||
        id=="ID_TRICK_PUMPING" || id=="ID_TRICK_REVERT" || manual_identity(id) ||
        slappy_entry(id) || stall_identity(id) || (known->categories&(1u<<15))!=0;
    // Native ground-grab scoring definitions are distinct from airborne grabs.
    // Do not use current grounded state: a native event can arrive after landing.
    if(ignored)return true;
    if(category(id)==Category::unknown)return true;
    // Explicit core identity, including zero-spin Ollie, overrides native
    // IgnoredTricks. The following native category/area/sequence branches still run.
    return false;
}
std::optional<bool> native_category_permission(std::uint32_t value) {
    auto& s=state();std::lock_guard lock(s.mutex);
    if(!s.enabled || !s.own || s.done)return {};
    const auto c=value==1 || value==16 || value==18 || value==19?Category::flips:
        value==2 || value==13 || value==14 || value==15?Category::grabs:
        value==3 || value==4?Category::grinds:value==5?Category::manuals:value==6?Category::ground:Category::unknown;
    if(c==Category::unknown)return {};
    return s.rules.allows(c);
}
void reset_native_eligibility() {
    auto& s=state();std::lock_guard lock(s.mutex);s.recovery_seen=false;s.tracker=trick_logger::TrickSetTracker{true,true,s.rules.allows(Category::grinds)};s.rows.clear();s.native_eligible=false;s.legacy.reset();s.ready.reset();s.excluded.clear();s.boundary_logged=false;
}
void native_outcome(bool landed,const std::array<std::uint8_t,28>& record) {
    auto& s=state();std::lock_guard lock(s.mutex);if(!s.enabled || !s.own)return;
    if(!landed) {
        s.diagnostics.push_back("Native S.K.A.T.E. rejected the attempt; following native failure path");
        if(s.ready && s.ready->landed){s.ready.reset();s.done=false;}
        fail(s);return;
    }
    if(s.done && !s.ready)return;
    s.legacy=record;s.native_eligible=true;
    s.diagnostics.push_back("Native S.K.A.T.E. accepted physical attempt "+std::to_string(s.tracker.capture_id())+" through its area and sequence checks");
    if(s.ready)s.done=true;
}
std::vector<std::string> take_diagnostics() {
    auto& s=state();std::lock_guard lock(s.mutex);return std::exchange(s.diagnostics,{});
}
std::optional<DetectedAttempt> take() {
    auto& s=state();std::lock_guard lock(s.mutex);
    if(s.bridge_failed || !s.ready || !s.done)return {};
    // CompositeTrickRecord is seven scalar fields, not a pointer or scoring authority.
    // A zero-initialized carrier is valid here: the verified native handler's setter
    // branch tests WasSuccessful, and the normalized signature controls comparison.
    // The original record, when present, only improves native compatibility text.
    auto result=std::move(s.ready);s.ready.reset();result->evidence["eligibility"]="native_accepted";if(s.legacy)result->native=*s.legacy;return result;
}
void prepare_native(std::uint32_t player,DetectedAttempt attempt) {
    auto& s=state();std::lock_guard lock(s.mutex);if(s.enabled)s.pending[player]=std::move(attempt);
}
bool native_pending(std::uint32_t player) {
    auto& s=state();std::lock_guard lock(s.mutex);return s.enabled && (player?s.pending.contains(player):!s.pending.empty());
}
bool judge_native(std::uint32_t player,bool setter,std::array<std::uint8_t,28>& record,bool& landed) {
    auto& s=state();std::lock_guard lock(s.mutex);if(!s.enabled)return false;
    const auto it=s.pending.find(player);if(it==s.pending.end()){landed=false;return true;}
    auto attempt=std::move(it->second);s.pending.erase(it);landed=attempt.landed;
    if(!landed)return true;
    if(setter) {
        if(!s.history.insert(attempt.signature).second){landed=false;return true;}
        s.target_evidence=attempt.evidence;s.target=std::move(attempt.signature);s.target_display=std::move(attempt.display);s.target_native=record;
    } else {
        landed=!s.target.empty() && s.target==attempt.signature;
        auto comparison=Json::object();comparison["target"]=Json::array();comparison["attempt"]=Json::array();
        for(const auto& token:s.target)comparison["target"].push_back(token);
        for(const auto& token:attempt.signature)comparison["attempt"].push_back(token);
        comparison["target_evidence"]=s.target_evidence;comparison["attempt_evidence"]=attempt.evidence;
        comparison["player"]=player;comparison["attempt_id"]=attempt.id;comparison["matched"]=landed;
        comparison["difference"]=trick_logger::match_difference({s.target,{}},{attempt.signature,{}});
        trick_logger::comparison(std::move(comparison));
        if(landed)record=s.target_native;
        else s.diagnostics.push_back("Match failed: "+trick_logger::match_difference({s.target,{}},{attempt.signature,{}}));
    }
    return true;
}
std::string target_text(){auto& s=state();std::lock_guard lock(s.mutex);return s.enabled?s.target_display:std::string{};}
}
