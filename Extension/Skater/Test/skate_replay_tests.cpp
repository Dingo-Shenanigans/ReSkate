#include "Extension/Skater/skate_detector.h"
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
namespace dingosdk::skate {
namespace {
Json load(const char* path){std::ifstream file(path);if(!file)throw std::runtime_error("Cannot read rejected-defense fixture");const std::string text{std::istreambuf_iterator<char>(file),{}};return Json::parse(text);}
Normalized replay(const Json& recording,Json* findings=nullptr,Rules rules={0xff,true}) {
    Normalized missing;missing.signature.error="Missing replay evidence";
    if(recording.is_null() || !recording.contains("replay") || recording.at("replay").is_null())return missing;
    const auto& raw=recording.at("replay");trick_logger::TrickSetTracker tracker{true};std::optional<trick_logger::TrickSetResult> completed;
    for(const auto& x:raw.at("samples")) {
        trick_logger::SetSample s;s.entity=1;s.now=x.value("now",std::uint64_t{});s.steps=x.value("steps",std::uint64_t{});
        s.sequence_ends=x.value("sequence_ends",std::uint64_t{});s.wipeouts=x.value("wipeouts",std::uint64_t{});s.state=x.value("state",0u);
        s.valid=x.value("valid",false);s.goofy=x.value("goofy",false);s.yaw=x.value("yaw",0.0);
        if(auto done=tracker.update(s)){completed=done;break;}
    }
    if(!completed){missing.signature.error="Replay does not establish native sequence completion";return missing;}
    auto r=Json::object();r["landed"]=completed->landed?Json(*completed->landed):Json{};r["rotation_complete"]=completed->rotation_complete;
    auto spins=Json::array();for(const auto& spin:completed->segments) {
        auto j=Json::object();j["accepted_spin"]=spin.degrees;j["direction"]=spin.direction;j["starting_stance"]=spin.stance;
        j["net_degrees"]=spin.accepted_net;j["goofy"]=spin.goofy?Json(*spin.goofy):Json{};spins.push_back(j);
    }r["spins"]=spins;
    auto phases=Json::array();for(const auto& phase:completed->phases){auto j=Json::object();j["id"]=phase.id;j["kind"]=phase.kind;j["segment"]=phase.segment;phases.push_back(j);}r["movement_phases"]=phases;
    auto rows=Json::array();for(auto row:raw.at("rows")) {
        if(row.value("preexisting",false))continue;
        const auto seen=row.value("first_seen_ms",completed->start);
        const auto original_segment=row.value("candidate_segment_id",std::uint64_t{});
        if(associate_observation(row,tracker,nullptr,seen)) {
            if(findings && original_segment!=row.value("candidate_segment_id",std::uint64_t{})) {
                auto finding=Json::object();finding["identity"]=trick_names::matching_labels(row).value("base_name","");finding["old_segment"]=original_segment;finding["corrected_segment"]=row.at("candidate_segment_id");findings->push_back(finding);
            }
            rows.push_back(row);
        }
    }r["ticker_candidates"]=rows;return normalize(r,rules);
}
}
void audit_ignored_turns(const char* path) {
    const auto fixture=load(path);const Rules all{0xff,true};unsigned total{},plain_ollies{};
    for(const auto& c:fixture.at("cases")) {
        unsigned captures{};
        for(const auto& event:c.at("events")) {
            if(event.value("kind","")!="detector_evidence")continue;
            const auto normalized=normalize(event.at("result"),all);
            if(!normalized.signature.error.empty())throw std::runtime_error("Recorded core trick lost after ignored movement");
            if(normalized.display=="Ollie")++plain_ollies;
            ++captures;++total;
        }
        if(!captures)throw std::runtime_error("Missing recorded post-ignored core capture");
    }
    if(total!=13 || plain_ollies!=1)throw std::runtime_error("Ignored-turn fixture coverage changed");
    std::cout<<"PASS: 13 recorded post-submission captures retained, including Sam's zero-spin Ollie\n";
}
void audit_rejections(const char* path,const char* output) {
    const auto fixture=load(path);auto report=Json::object();auto cases=Json::array();std::map<std::string,unsigned> counts;unsigned paired{},boned_fixed{},bounded_recorded{};
    if(fixture.at("cases").size()!=44)throw std::runtime_error("Rejected-defense fixture incomplete");
    for(const auto& item:fixture.at("cases")) {
        const auto id=item.at("id").get<unsigned>();
        if(id==35 || id==36 || id==38) {
            const auto full=replay(item.at("target")),flat=replay(item.at("target"),nullptr,{0xf9,true});
            if(!full.signature.error.empty() || !flat.signature.error.empty() || full.phases.size()!=3 || flat.phases.size()!=1 || flat.capture.last!=1)
                throw std::runtime_error("Recorded grind-route boundary regression "+std::to_string(id)+": "+full.signature.error+" / "+flat.signature.error);
            ++bounded_recorded;
        }
        auto entry=Json::object();entry["id"]=item.at("id");entry["time"]=item.at("time");entry["target"]=item.at("target_display");
        const auto& target=item.at("target");const auto& defense=item.at("defense");std::string classification;
        if(target.is_null() || defense.is_null())classification="insufficient_evidence";
        else {
            if(defense.at("result").value("landed",false))++paired;
            auto target_findings=Json::array(),defense_findings=Json::array();
            const auto a=replay(target,&target_findings),b=replay(defense,&defense_findings);
            entry["target_capture_corrections"]=target_findings;entry["defense_capture_corrections"]=defense_findings;
            entry["new_target"]=a.display;entry["new_defense"]=b.display;entry["target_replay_error"]=a.signature.error;entry["defense_replay_error"]=b.signature.error;
            if(!a.signature.error.empty() || !b.signature.error.empty())classification=(a.signature.error.find("Missing replay")!=std::string::npos || b.signature.error.find("Missing replay")!=std::string::npos || a.signature.error.find("does not establish")!=std::string::npos || b.signature.error.find("does not establish")!=std::string::npos)?"insufficient_evidence":"capture_defect";
            else if(a.signature.tokens==b.signature.tokens) {
                classification="corrected_false_mismatch";
                if(defense.at("result").value("display","").find("Boned")!=std::string::npos)++boned_fixed;
            } else classification="recorded_trick_difference";
            entry["difference"]=trick_logger::match_difference(a.signature,b.signature);entry["target_phases"]=a.phases;entry["defense_phases"]=b.phases;
        }
        entry["classification"]=classification;++counts[classification];cases.push_back(entry);
    }
    if(paired!=13 || !boned_fixed)throw std::runtime_error("Recorded Boned Ollie false rejection was not corrected");
    if(bounded_recorded!=3)throw std::runtime_error("Missing recorded skate.11 route-boundary regressions");
    report["bounded_recorded_cases"]=bounded_recorded;
    report["rule_version"]=rules_version;report["cases"]=cases;report["counts"]=Json::object();
    for(const auto& [key,count]:counts){report["counts"][key]=count;std::cout<<key<<": "<<count<<'\n';}
    report["limitation"]="Offline capture/matching replay; original native eligibility and two-client gameplay are not simulated. Missing opponent recordings cannot establish false rejection.";
    if(output){std::ofstream file(output);file<<report.dump(2)<<'\n';}
}
}
