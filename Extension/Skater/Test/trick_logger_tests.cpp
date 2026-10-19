#include "Extension/Skater/trick_logger.h"
#include "Extension/Skater/skate_detector.h"
#include "Extension/Skater/trick_attempt.h"
#include "Extension/Skater/trick_set.h"
#include "Extension/Skater/trick_match.h"
#include "Extension/Skater/trick_name_text.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Engine/Core/Json/json.h"
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace dingosdk::multiplayer {bool local_throwdown_active() noexcept {return false;}}
namespace dingosdk {
PhysicsStateWatch watched_physics_state() noexcept { PhysicsStateWatch s; s.valid = true; s.state = 200; return s; }
bool no_bail_skater(NoBailSkater&) noexcept { return false; }
}
void native_filter_regressions();
int main(int argc, char** argv) {
    using namespace dingosdk;
    try {
        if (argc < 2 || argc > 3) throw std::runtime_error("Expected isolated fixture directory and optional replay log");
        const auto fixture = std::filesystem::path(argv[1]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(fixture);
        std::filesystem::current_path(fixture);
        const auto require = [](bool v) { if (!v) throw std::runtime_error("Logger regression failed"); };
        native_filter_regressions();
        require(!trick_logger::enabled());
        skate::begin({0xff,true});skate::turn(true);
        require(trick_logger::gameplay_capture());
        trick_logger::physics_step(1,.016f,false);
        skate::end();require(!trick_logger::gameplay_capture());
        require(!trick_logger::enabled() && !std::filesystem::exists("logs/tricks"));
        require(trick_logger::starting_stance("ID_STANCE_NOLLIE","")=="nollie");
        require(trick_logger::starting_stance("","ID_NOLLIE")=="nollie");
        require(trick_logger::starting_stance("","","Nollie")=="nollie");
        {
            trick_logger::TrickSetTracker t;trick_logger::SetSample s;s.entity=1;s.valid=true;s.state=100;t.update(s);
            s.now=16;s.steps=1;s.state=200;t.update(s);
            s.now=32;s.valid=false;require(!t.update(s));
            s.now=48;s.valid=true;s.yaw=120;s.steps=3;require(!t.update(s));
            s.now=64;s.state=100;s.sequence_ends=1;s.steps=4;t.update(s);
            s.now=80;s.steps=5;const auto result=t.update(s);
            require(result && result->landed==true && !result->rotation_complete);
        }
        {
            trick_logger::TrickSetTracker t;trick_logger::SetSample s;s.entity=1;s.valid=true;s.state=100;t.update(s);
            s.now=16;s.steps=1;s.state=200;t.update(s);
            s.now=32;s.state=403;t.update(s);
            s.now=48;s.state=701;require(!t.update(s));
            s.now=64;s.state=200;require(!t.update(s));require(t.segment_id()==2);
            s.now=80;s.state=700;const auto result=t.update(s);
            require(result && result->outcome=="position_reset" && !result->landed.has_value());
        }
        {
            trick_logger::TrickSetTracker t;trick_logger::SetSample s;s.entity=1;s.valid=true;s.state=100;s.position_known=true;t.update(s);
            s.now=16;s.state=200;t.update(s);s.now=32;s.position[0]=10;
            const auto result=t.update(s);require(result && result->outcome=="position_reset");
        }
        trick_logger::observe_setup_menu(0x6f,0x7f);
        require(trick_logger::setup_flags().captured==0x6f && trick_logger::setup_flags().valid==0x7f);
        for(const auto angles:{std::array<double,2>{-81.8,-93.8},std::array<double,2>{79.6,71.6}}) {
            trick_logger::TrickSetTracker t;trick_logger::SetSample s;s.entity=1;s.valid=true;s.state=100;t.update(s);
            s.now=100;s.steps=1;s.state=201;s.yaw=angles[0];t.update(s);
            s.now=200;s.steps=2;s.state=101;t.update(s);
            s.now=300;s.steps=3;s.yaw+=angles[1];t.update(s);
            s.now=400;s.steps=4;s.state=100;s.sequence_ends=1;
            const auto result=t.update(s);require(result && result->segments[0].degrees==180);
        }
        {
            trick_logger::TrickSetTracker t;trick_logger::SetSample s;s.entity=1;s.valid=true;s.goofy=false;s.state=100;t.update(s);
            s.now=100;s.steps=1;s.state=201;s.yaw=-170;t.update(s);t.stance(1,"normal");
            s.now=200;s.steps=2;s.yaw=-337;t.update(s);
            s.now=300;s.steps=3;s.state=101;t.update(s);
            s.now=400;s.steps=4;s.yaw=-437;t.update(s);
            s.now=500;s.steps=5;s.yaw=-539;t.update(s);
            s.now=600;s.steps=6;s.state=100;s.sequence_ends=1;
            const auto result=t.update(s);
            require(result && result->segments[0].degrees==540 && result->segments[0].landing==-202 && result->segments[0].excluded_ground==0);
        }
        {
            auto result=Json::object();result["landed"]=true;
            auto spins=Json::array();auto spin=Json::object();spin["accepted_spin"]=360;spin["direction"]="BS";spin["starting_stance"]="normal";spins.push_back(spin);result["spins"]=spins;
            auto rows=Json::array();
            for(const auto base:{"Kickflip","Tailslide","Manual"}) {
                auto row=Json::object();row["name"]=base;row["name_resolved"]=true;row["candidate_segment_id"]=1;
                auto raw=Json::object();raw["base_name"]=base;raw["direction"]=std::string(base)=="Tailslide"?"ID_TRICK_BS":"";
                row["raw_labels"]=raw;rows.push_back(row);
            }
            result["ticker_candidates"]=rows;
            const auto target=trick_logger::match_signature(result);require(target.error.empty());
            require(trick_logger::match_difference(target,trick_logger::match_signature(result)).empty());
            auto changed=result;auto changed_spins=Json::array();spin["accepted_spin"]=180;changed_spins.push_back(spin);changed["spins"]=changed_spins;
            require(!trick_logger::match_difference(target,trick_logger::match_signature(changed)).empty());
            changed=result;changed["ticker_candidates"]=Json::array();require(!trick_logger::match_signature(changed).error.empty());
            changed=result;changed["landed"]=false;require(!trick_logger::match_signature(changed).error.empty());
            changed=result;auto reversed=Json::array();reversed.push_back(rows.at(2));reversed.push_back(rows.at(1));reversed.push_back(rows.at(0));changed["ticker_candidates"]=reversed;
            require(!trick_logger::match_difference(target,trick_logger::match_signature(changed)).empty());
        }
        {
            trick_logger::TrickSetTracker tracker;
            trick_logger::SetSample s; s.valid=true;s.entity=1;s.goofy=false;s.state=100;
            tracker.update(s);
            for(unsigned i=1;i<=3;++i){s.now=i*100;s.steps=i;s.state=201;s.yaw=-90.0*i;tracker.update(s);}
            tracker.stance(1,"normal");
            s.now=400;s.steps=4;s.state=101;s.yaw=-315;require(!tracker.update(s));
            s.now=500;s.steps=5;s.state=100;s.yaw=-360;s.sequence_ends=1;
            const auto result=tracker.update(s);
            require(result && result->landed==true && result->segments[0].degrees==360 && result->segments[0].direction=="BS");
            // Partial excursion reverses to starting heading: no 180.
            s.now=600;s.steps=6;s.state=201;s.yaw=-430;tracker.update(s);
            tracker.stance(1,"normal");
            s.now=700;s.steps=7;s.state=201;s.yaw=-360;tracker.update(s);
            s.now=800;s.steps=8;s.state=100;require(!tracker.update(s));
            s.now=900;s.steps=9;s.sequence_ends=2;
            const auto zero=tracker.update(s);require(zero && zero->segments[0].degrees==0);
            // Air->grind->air retains the set; rail turning is not body spin.
            s.now=1000;s.steps=10;s.state=201;s.yaw=-270;tracker.update(s);
            s.now=1100;s.steps=11;s.state=403;require(!tracker.update(s));
            const auto id=tracker.active_id();
            s.now=1200;s.steps=12;s.yaw=-180;tracker.update(s);
            s.now=1300;s.steps=13;s.state=201;tracker.update(s);require(tracker.active_id()==id);
            s.now=1400;s.steps=14;s.state=100;tracker.update(s);
            s.now=1500;s.steps=15;s.sequence_ends=3;
            const auto grind=tracker.update(s);require(grind && grind->segments.size()==2 && grind->segments[0].air==90);
            s.now=1600;s.steps=16;s.state=201;tracker.update(s);
            s.now=1700;s.steps=17;s.state=504;s.wipeouts=1;s.valid=false;
            const auto bail=tracker.update(s);require(bail && bail->landed==false && bail->outcome=="bailed");
        }
        for(const bool goofy:{false,true})for(const auto stance:{"normal","switch","nollie","fakie"})for(const int sign:{-1,1}) {
            trick_logger::TrickSetTracker t;trick_logger::SetSample s;s.entity=1;s.valid=true;s.goofy=goofy;s.state=100;t.update(s);
            s.now=100;s.steps=1;s.state=201;s.yaw=sign*90;t.update(s);t.stance(1,stance);
            s.now=200;s.steps=2;s.yaw=sign*180;t.update(s);
            s.now=300;s.steps=3;s.state=100;s.sequence_ends=1;t.update(s);
            s.now=400;s.steps=4;const auto result=t.update(s);
            const bool fs=((sign>0)!=goofy)!=(std::string(stance)=="switch");
            require(result && result->segments[0].direction==(fs?"FS":"BS"));
        }
        if(argc==3) {
            std::ifstream input(argv[2]);require(input.good());std::string line;
            trick_logger::TrickSetTracker replay;unsigned count{},settled{},interrupted{};
            std::uint64_t now{},sequences{};std::optional<bool> goofy;
            while(std::getline(input,line)) {
                const auto sample=Json::parse(line);const auto kind=sample.value("kind","");
                if(kind=="gameplay_bool_observation" && sample.value("key","")=="DefaultStanceIsGoofy")goofy=sample.at("value").get<bool>();
                if(kind=="native_scoring_expression" && sample.value("name","")=="ScoringHud.OnScorableSequenceEnded" && sample.value("phase","")=="exit" && sample.value("pc",0u)==0)++sequences;
                if(kind!="sample")continue;
                now=sample.at("uptime_ms").get<std::uint64_t>();
                const auto code=sample.value("selected_physics_state",0u);
                const bool valid=sample.value("ready",false) && sample.value("riding_body_candidate_known",false) && sample.contains("body_forward_yaw_deg");
                trick_logger::SetSample evidence;evidence.now=now;evidence.steps=sample.value("native_physics_steps",std::uint64_t{});
                evidence.wipeouts=sample.value("native_step_wipeouts",std::uint64_t{});evidence.sequence_ends=sequences;
                evidence.entity=1;evidence.state=code;evidence.valid=valid;evidence.ragdoll=code==504 && sample.value("offboard_ragdoll_raw",0u)!=0;
                evidence.yaw=sample.value("body_forward_yaw_deg",0.0);evidence.goofy=goofy;
                if(const auto result=replay.update(evidence)) {
                    ++count;if(result->outcome=="landed")++settled;
                    if(result->outcome=="tracking_lost")++interrupted;
                }
            }
            if(replay.stop(now))++count;
            require(count>0);
            std::cout<<"Replay: "<<count<<" sets, "<<settled<<" client-landed, "<<interrupted<<" tracking losses\n";
        }
        {
            trick_logger::AttemptTracker tracker;
            tracker.update(0,true,false,true,0);
            tracker.update(100,true,true,false,90);
            tracker.update(200,true,true,false,180);
            tracker.update(300,true,true,false,-90);
            tracker.update(400,true,false,true,0);
            tracker.update(500,true,false,true,0);
            const auto result=tracker.update(600,true,false,true,0);
            require(result && result->air_degrees==270 && result->correction_degrees==90 &&
                result->provisional_spin==360 && result->outcome=="riding_settled_provisional");
            tracker.update(700,true,true,false,90);
            const auto bail=tracker.update(800,true,false,false,100);
            require(bail && bail->outcome=="offboard_or_unsupported_state");
            tracker.update(900,true,true,false,180);
            const auto gap=tracker.update(1200,true,true,false,-90);
            require(gap && gap->outcome=="tracking_interrupted");
            require(!tracker.active_id());
        }
        require(trick_logger::command("mark before start").starts_with("error:"));
        require(trick_logger::command("on full").starts_with("Trick logger recording:"));
        require(trick_logger::feedback_text().empty());
        trick_logger::command("feedback off");require(trick_logger::feedback_text().empty());
        trick_logger::command("feedback on");require(!trick_logger::feedback_text().empty());
        require(trick_logger::command("on").find("already") != std::string::npos);
        trick_logger::command("mark goofy BS kickflip 720 \"test\"");
        multiplayer::NativeFrame frame;
        frame.ready = true; frame.pose.root.position = {1, 2, 3};
        frame.board_root = multiplayer::Transform{};
        trick_logger::sample(frame, "test-map");
        std::array<std::uint8_t, 28> raw{};
        const std::uint32_t spins = 720, id = 0xfeedabcd;
        std::memcpy(raw.data() + 8, &spins, 4); std::memcpy(raw.data() + 20, &id, 4);
        trick_logger::attempt(true, raw);
        trick_logger::event(0xe85319b1);
        trick_logger::score(2, true, 100, 3);
        trick_logger::physics_transition(201,300);
        trick_logger::physics_step(123,1.f/60,true);
        {
            std::array<std::uint8_t,128> vm{},resource{};
            std::array<std::uint8_t,256> instance{};
            std::array<std::uint64_t,2> inputs{};
            const auto put=[](auto& buffer,std::size_t at,auto value){std::memcpy(buffer.data()+at,&value,sizeof(value));};
            put(vm,0x38,reinterpret_cast<std::uintptr_t>(resource.data()));
            put(vm,0x30,reinterpret_cast<std::uintptr_t>(instance.data()));
            put(instance,0,reinterpret_cast<std::uintptr_t>(resource.data()));
            put(instance,64+8,reinterpret_cast<std::uintptr_t>(inputs.data()));
            put(resource,0x10,0xdcd5d079u);
            const std::array<std::uint32_t,10> layout{64,48,10,83,5,0,0,327682,11,2048};
            std::memcpy(resource.data()+0x20,layout.data(),sizeof(layout));
            trick_logger::scoring_expression(reinterpret_cast<std::uintptr_t>(vm.data()),0,false);
            trick_logger::scoring_expression(reinterpret_cast<std::uintptr_t>(vm.data()),0,true);
            put(resource,0x20,65u); // Refuse a mismatched resource layout.
            trick_logger::scoring_expression(reinterpret_cast<std::uintptr_t>(vm.data()),0,false);
            put(resource,0x10,0xc882a12bu);
            const std::array<std::uint32_t,10> stance_layout{48,0,0,14,4,0,0,1,0,0};
            std::memcpy(resource.data()+0x20,stance_layout.data(),sizeof(stance_layout));
            put(instance,48+8,reinterpret_cast<std::uintptr_t>(inputs.data()));
            inputs[0]=0;trick_logger::scoring_expression(reinterpret_cast<std::uintptr_t>(vm.data()),0,false);
            inputs[0]=1;trick_logger::scoring_expression(reinterpret_cast<std::uintptr_t>(vm.data()),0,false);
        }
        trick_logger::gameplay_bool("Test.IsGoofy",false,"test");
        trick_logger::gameplay_bool("Test.IsGoofy",false,"test");
        trick_logger::gameplay_bool("Test.IsGoofy",true,"test");
        trick_logger::gameplay_bool("UnrelatedSetting",true,"test");
        trick_logger::command("mark no detector event");
        trick_logger::sample(frame, "test-map");
        require(trick_logger::command("off").find("0 dropped") != std::string::npos);
        require(!trick_logger::enabled());
        bool attempt{}, marker{}, sample_without_event{}, end{};
        unsigned traces{},transitions{},wipeouts{},stances{},settings{};
        std::uint64_t previous{};
        for (const auto& file : std::filesystem::directory_iterator("logs/tricks")) {
            if (file.path().extension() != ".jsonl") continue;
            std::ifstream in(file.path()); std::string line;
            while (std::getline(in, line)) {
                const auto j = Json::parse(line);
                const auto sequence = j.at("sequence").get<std::uint64_t>();
                require(sequence == ++previous);
                require(j.at("qpc_ticks").get<std::uint64_t>() > 0);
                const auto kind = j.at("kind").get<std::string>();
                if (kind == "skate_attempt") {
                    attempt = true;
                    require(j.at("spins_raw").get<std::uint32_t>() == spins);
                    require(j.at("scorable_id").get<std::uint32_t>() == id);
                    require(j.at("raw_bytes").size() == raw.size());
                }
                if (kind == "marker" && j.at("marker").get<unsigned>() == 1)
                    marker = j.at("label").get<std::string>() == "goofy BS kickflip 720 \"test\"";
                if (kind == "sample" && j.at("marker").get<unsigned>() == 2) sample_without_event = true;
                if (kind == "session_end") end = true;
                if(kind=="native_scoring_expression") {++traces;require(!j.at("payload_decoded").get<bool>() && j.at("input_words_raw").size()==2);}
                if(kind=="native_physics_transition") {++transitions;require(j.at("after").get<unsigned>()==300);}
                if(kind=="native_wipeout")++wipeouts;
                if(kind=="natural_stance_observation") { require(j.at("is_goofy").get<bool>()==(stances==1));++stances; }
                if(kind=="gameplay_bool_observation")++settings;
            }
        }
        require(attempt && marker && sample_without_event && end);
        require(traces==4 && transitions==1 && wipeouts==1 && stances==2 && settings==2);
        require(trick_logger::command("on").starts_with("Trick logger recording:"));
        require(!trick_logger::full_capture());
        const auto epoch = trick_logger::generation(); require(epoch == 2);
        trick_logger::sample(frame, "ignored in names mode");
        for (unsigned id2 : {1u, 2u}) {
            auto j = Json::object(); j["kind"] = "trick_name"; j["name"] = "Kickflip";
            j["display_name"] = "BS 540 Kickflip"; j["name_resolved"] = true;
            j["observation_id"] = id2; j["revision"] = 1;
            require(trick_logger::named_trick(j));
        }
        trick_logger::command("off");
        unsigned names{}; bool text_found{};
        for (const auto& file : std::filesystem::directory_iterator("logs/tricks")) {
            std::ifstream in(file.path()); std::string line;
            if (file.path().extension() == ".txt") {
                while (std::getline(in,line)) if (line.find("BS 540 Kickflip") != std::string::npos) text_found = true;
            } else if (file.path().extension() == ".jsonl") {
                bool names_mode{};
                while (std::getline(in,line)) {
                    const auto j=Json::parse(line);const auto kind=j.at("kind").string();
                    if(kind=="session") names_mode=j.at("capture_mode").string()=="names";
                    if(names_mode) require(kind!="sample");
                    if(kind=="trick_name")++names;
                }
            }
        }
        require(names==2 && text_found);
        bool resolved{};
        const auto lookup=[](std::string_view key) { return key=="ID_KICKFLIP"?std::string("Kickflip"):std::string{}; };
        require(trick_names::resolve_text("BS 540 ID_KICKFLIP",lookup,resolved)=="BS 540 Kickflip" && resolved);
        require(trick_names::resolve_text("ID_UNKNOWN",lookup,resolved)=="ID_UNKNOWN" && !resolved);
        require(trick_names::plain_text(" [font color=Red]BS[/font]  540\nKickflip ")=="BS 540 Kickflip");
        auto identity=Json::object(); identity["base_name"]="ID_KICKFLIP"; identity["direction"]="BS";
        identity["body_rotation"]="540"; identity["completed_info"]="";
        const auto original_key=trick_names::comparison_key(identity);
        identity["completed_info"]="landed label";
        identity["direction"]=" [font color=Red]BS[/font] ";
        require(trick_names::comparison_key(identity)==original_key);
        identity["direction"]="FS";require(trick_names::comparison_key(identity)!=original_key);
        identity["direction"]="BS";identity["body_rotation"]="720";
        require(trick_names::comparison_key(identity)!=original_key);
        require(trick_logger::command("on full").starts_with("Trick logger recording:") && trick_logger::full_capture());
        trick_logger::command("off");require(!trick_logger::enabled());
        std::cout << "PASS: raw records, name/text export, duplicate names in distinct rows, names-only capture, localization and restart\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
