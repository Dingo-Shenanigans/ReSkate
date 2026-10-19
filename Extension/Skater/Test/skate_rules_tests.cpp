#include "Extension/Skater/skate_detector.h"
#include "Extension/Throwdowns/throwdown_wire.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <tuple>
#include <stdexcept>
namespace dingosdk::skate {void body_regressions(const char*);void capture_regressions();void phase_regressions();void audit_ignored_turns(const char*);void audit_rejections(const char*,const char*);}
using namespace dingosdk;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Json row(const char* id,const char* display,unsigned observation=1) {
    auto j=Json::object();j["name"]=display;j["display_name"]=display;j["name_resolved"]=true;
    j["observation_id"]=observation;j["candidate_segment_id"]=1;
    auto labels=Json::object();labels["base_name"]=id;labels["stance"]="";j["raw_labels"]=labels;return j;
}
Json result(std::initializer_list<Json> rows) {
    auto j=Json::object();j["landed"]=true;auto names=Json::array();for(const auto& r:rows)names.push_back(r);j["ticker_candidates"]=names;
    auto spins=Json::array();auto spin=Json::object();spin["accepted_spin"]=360;spin["direction"]="FS";spin["starting_stance"]="normal";spins.push_back(spin);j["spins"]=spins;return j;
}
}
int main(int argc,char** argv)try {
    using namespace skate;
    phase_regressions();
    capture_regressions();
    const auto flip=row("ID_TRICK_KICKFLIP","Kickflip"),bone=row("ID_TRICK_BONEDOLLIE","Boned Ollie"),grind=row("ID_TRICK_TAILSLIDE","Tailslide"),grab=row("ID_TRICK_NOSEGRAB","Nosegrab"),manual=row("ID_TRICK_MANUAL","Manual");
    const Rules all{0xff,true};
    check(argc>=2 && argc<=6,"Expected native ticker and optional rejected-defense fixture");
    if(argc>=3)audit_rejections(argv[2],argc>=4?argv[3]:nullptr);
    if(argc>=5)audit_ignored_turns(argv[4]);
    if(argc==6)body_regressions(argv[5]);
    std::ifstream fixture_file(argv[1]);check(fixture_file.good(),"Cannot read ticker fixture");
    const std::string fixture_text{std::istreambuf_iterator<char>(fixture_file),std::istreambuf_iterator<char>()};
    const auto fixture=Json::parse(fixture_text);
    trick_names::NativeTrickCatalog catalog;
    for(const auto& [id,name]:fixture.at("native_localization_pairs").items())catalog.add(id,name.string());
    std::size_t recorded{};
    for(const auto& observed:fixture.at("observations")) {
        const auto canonical=catalog.annotate(observed);
        check(canonical.at("raw_labels")==observed.at("raw_labels"),"Raw labels were overwritten");
        check(trick_names::matching_labels(canonical).value("base_name","")==observed.value("expected_native_id",""),"Recorded localized row did not resolve native identity");
        if(observed.at("raw_labels").value("base_name","")!="Revert" && !manual_identity(trick_names::matching_labels(canonical).value("base_name","")))
            check(normalize(result({canonical}),all).signature.error.empty(),"Recorded ticker row still fails category detection");
        ++recorded;
    }
    check(recorded>=10,"Recorded ticker fixture unexpectedly empty");
    auto live_flip=catalog.annotate(row("Kickflip","Kickflip"));
    check(!normalize(result({live_flip}),{0xfe,true}).signature.error.empty(),"Disabled-only flip created a result");
    check(normalize(result({live_flip}),all).signature.tokens==normalize(result({flip}),all).signature.tokens,"Localized and identifier rows mismatch");
    auto ambiguity=catalog;ambiguity.add("ID_TRICK_HEELFLIP","Kickflip");
    check(ambiguity.resolve("Kickflip").empty(),"Ambiguous localized name guessed an identity");
    check(!normalize(result({ambiguity.annotate(row("Kickflip","Kickflip"))}),all).signature.error.empty(),"Ambiguous identity passed matching");
    check(catalog.resolve("Made up trick").empty(),"Unknown label was guessed");
    check(catalog.resolve("[font color=Red]Kickflip[/font]")=="ID_TRICK_KICKFLIP","Native markup changed identity");
    for(auto observed:fixture.at("recorded_extended_tricks")) {
        const auto n=normalize(result({observed}),all);
        check(n.signature.error.empty(),"Recorded hospital/underflip trick rejected");
        const auto& labels=trick_names::matching_labels(observed);
        if(!labels.value("underflip","").empty())check(n.display.find("Underflip")!=std::string::npos,"Recorded underflip missing from readable name");
        else check(n.display.find("Hospital")!=std::string::npos,"Recorded hospital flip missing from readable name");
    }
    for(const auto& attempt:fixture.at("recorded_manual_attempts")) {
        auto r=result({});r["ticker_candidates"]=attempt.at("rows");
        const auto n=normalize(r,all);
        check(n.violation!=Category::manuals,"Recorded manual still fails the attempt");
    }
    for(const auto& replay:fixture.at("recorded_landing_replays")) {
        trick_logger::TrickSetTracker tracker{true};std::optional<trick_logger::TrickSetResult> completed;
        for(const auto& x:replay.at("samples")) {
            trick_logger::SetSample s;s.entity=1;s.now=x.value("now",std::uint64_t{});s.state=x.value("state",0u);s.steps=x.value("steps",std::uint64_t{});
            s.sequence_ends=x.value("sequence_ends",std::uint64_t{});s.yaw=x.value("yaw",0.0);s.valid=x.value("valid",false);s.goofy=x.value("goofy",false);
            if(auto done=tracker.update(s))completed=std::move(done);
        }
        check(completed && completed->landed==true && !completed->segments.empty(),"Recorded landing replay did not complete");
        if(completed->segments.back().degrees!=replay.value("expected_spin",0))std::cerr<<"Replay expected "<<replay.value("expected_spin",0)<<" got "<<completed->segments.back().degrees<<" air "<<completed->segments.back().air<<" landing "<<completed->segments.back().landing<<" end "<<completed->end<<"\n";
        check(completed->segments.back().degrees==replay.value("expected_spin",0),"Recorded delayed revert spin wrong");
    }
    const auto a=normalize(result({flip}),all);check(a.signature.error.empty(),"Native flip identity missing");
    check(normalize(result({bone,flip}),all).signature.tokens==a.signature.tokens,"Boned Ollie was not suppressed by flip");
    check(normalize(result({bone}),all).signature.tokens!=a.signature.tokens,"Standalone Boned Ollie lost");
    check(normalize(result({bone,manual}),all).signature.tokens==normalize(result({bone}),all).signature.tokens,"Manual changed the core Boned Ollie");
    for(const auto& [r,bit]:{std::pair{flip,0},std::pair{grab,1},std::pair{grind,2},std::pair{row("ID_TRICK_WALLRIDE","Ground"),3}}) {
        const Rules rules{static_cast<std::uint8_t>(0xff&~(1<<bit)),true};
        const auto n=normalize(result({bone,r,flip}),rules);check(n.violation==Category::none,"Disabled category still fails the attempt");
        check(!normalize(result({r}),rules).signature.error.empty(),"Disabled-only category produced a set");
    }
    check(!normalize(result({row("ID_UNKNOWN","Unknown")}),all).signature.error.empty(),"Unknown category accepted");
    check(!normalize(result({flip}),{0xff,false}).signature.error.empty(),"Unverified setup accepted");
    begin({0xfb,true});turn(true);observe(grind);const auto failure=take();
    check(!failure,"Disabled grind failed the attempt");
    observe(flip);check(!take(),"Observation without landing created a result");end();
    begin(all);turn(true);
    trick_logger::SetSample sample;sample.entity=1;sample.valid=true;sample.goofy=false;sample.state=100;sample.now=1;sample.steps=1;skate::sample(sample);
    sample.now=17;sample.steps=2;sample.state=201;skate::sample(sample);observe(live_flip);
    native_outcome(true,{}); // Native eligibility without legacy identity.
    sample.now=33;sample.steps=3;sample.yaw=170;skate::sample(sample);
    sample.now=49;sample.steps=4;sample.yaw=340;skate::sample(sample);
    sample.now=65;sample.steps=5;sample.state=101;sample.yaw=350;skate::sample(sample);
    sample.now=81;sample.steps=6;sample.state=100;sample.sequence_ends=1;skate::sample(sample);
    const auto detected=take();check(detected && detected->landed && detected->signature.front()=="spin:normal:360:FS","Automatic detector did not emit measured landed spin");
    check(!take(),"Completed result delivered twice");end();
    begin(all);turn(true);native_outcome(false,{});
    check(take().has_value(),"Native bail before airborne tracking left turn unresolved");
    check(!take(),"Native bail delivered twice");end();
    begin(all);turn(true);sample={};sample.valid=true;sample.entity=1;sample.state=100;sample.now=1;skate::sample(sample);
    sample.now=17;sample.state=300;skate::sample(sample);
    const auto grounded_bail=take();check(grounded_bail && !grounded_bail->landed,"Grounded native wipeout was ignored");end();
    begin(all);turn(true);sample={};sample.valid=true;sample.entity=1;sample.state=100;sample.now=1;sample.steps=1;skate::sample(sample);
    sample.now=17;sample.steps=2;sample.state=201;skate::sample(sample);observe(flip);
    sample.now=33;sample.steps=3;sample.state=100;sample.sequence_ends=1;skate::sample(sample);
    sample.now=49;sample.steps=4;sample.sequence_ends=1;skate::sample(sample);
    check(!take(),"Unknown circle eligibility passed");
    check(!take_diagnostics().empty(),"Missing eligibility was silently discarded");
    native_outcome(true,{});const auto empty_record=take();
    check(empty_record && empty_record->landed && empty_record->native==std::array<std::uint8_t,28>{},"Zero native record still blocks valid completion");end();
    begin(all);turn(true);sample={};sample.valid=true;sample.entity=1;sample.goofy=false;sample.state=100;sample.now=1;sample.steps=1;skate::sample(sample);
    sample.now=17;sample.steps=2;sample.state=201;skate::sample(sample);observe(flip);
    for(unsigned i=0;i<36;++i){sample.now+=100;++sample.steps;sample.yaw=std::remainder(sample.yaw+15,360.0);skate::sample(sample);}
    sample.now+=16;++sample.steps;sample.state=101;skate::sample(sample);
    sample.now+=16;++sample.steps;sample.state=100;sample.sequence_ends=1;skate::sample(sample);
    native_outcome(true,{});const auto long_gap=take();check(long_gap && long_gap->landed && long_gap->signature.front()=="spin:normal:540:FS","Long gap or spin above 360 was restricted");end();
    begin(all);turn(true);sample={};sample.valid=true;sample.entity=1;sample.goofy=false;sample.state=100;sample.now=1;sample.steps=1;skate::sample(sample);
    sample.now=17;sample.steps=2;sample.state=201;skate::sample(sample);observe(flip);
    sample.now=33;++sample.steps;sample.valid=false;skate::sample(sample);
    sample.now=49;++sample.steps;sample.valid=true;sample.state=100;sample.sequence_ends=1;skate::sample(sample);
    sample.now=65;++sample.steps;sample.sequence_ends=1;skate::sample(sample);native_outcome(true,{});
    check(!take(),"Native success revived incomplete rotation evidence");
    const auto incomplete_notes=take_diagnostics();bool explained{};
    for(const auto& note:incomplete_notes)explained|=note.find("incomplete rotation")!=std::string::npos;
    check(explained,"Incomplete rotation was silently discarded");end();
    for(const auto& observed:fixture.at("in_match_extended_tricks")) {
        begin(all);turn(true);
        sample={};sample.valid=true;sample.entity=1;sample.goofy=false;sample.state=100;sample.now=1;sample.steps=1;skate::sample(sample);
        sample.now+=16;++sample.steps;sample.state=201;skate::sample(sample);observe(observed);
        sample.now+=16;++sample.steps;sample.state=100;skate::sample(sample);
        sample.now+=16;++sample.steps;sample.sequence_ends=1;skate::sample(sample);
        check(!take(),"Extended identity bypassed native eligibility");native_outcome(true,{});const auto extended_result=take();check(extended_result && extended_result->landed && !extended_result->signature.empty(),"In-match hospital/underflip lost native eligibility");
        end();
    }
    // An accepted previous turn must not authorize a later completed attempt.
    begin(all);turn(true);native_outcome(true,{});turn(true);
    sample={};sample.valid=true;sample.entity=1;sample.goofy=false;sample.state=100;sample.now=1;sample.steps=1;skate::sample(sample);
    sample.now+=16;++sample.steps;sample.state=201;skate::sample(sample);observe(flip);
    sample.now+=16;++sample.steps;sample.state=100;skate::sample(sample);
    sample.now+=16;++sample.steps;sample.sequence_ends=1;skate::sample(sample);
    check(!take(),"Previous-turn eligibility leaked");
    reset_native_eligibility();native_outcome(true,{});check(!take(),"World reset retained a completed candidate");end();
    // Core identity is independent of how the same spin was completed.
    auto mr=manual;mr["raw_labels"]["manual_type"]="ID_TRICK_REVERT";mr["landing_revert_episode"]=42;
    auto transitional=row("ID_TRICK_NOSEMANUAL","Nose Manual",2);transitional["landing_revert_episode"]=42;
    auto revert=row("ID_TRICK_REVERT","Revert",3);
    check(normalize(result({flip,mr,transitional}),all).signature.tokens==a.signature.tokens,"Manual revert route changes core matching");
    check(normalize(result({flip,revert}),all).signature.tokens==a.signature.tokens,"Normal revert route changes core matching");
    transitional["landing_revert_episode"]=41;
    check(normalize(result({flip,transitional,mr}),all).signature.tokens==a.signature.tokens,"Ordinary manual changed matching");
    check(!normalize(result({manual}),all).signature.error.empty(),"Manual-only spinning accepted");
    auto under=flip;under["raw_labels"]["underflip"]="ID_TRICK_UNDERFLIP";
    check(normalize(result({under}),all).signature.tokens!=a.signature.tokens,"Underflip modifier disappeared");
    auto ambiguous_flip=catalog;ambiguous_flip.add("ID_TRICK_FLIP","Flip");ambiguous_flip.add("ID_TRICK_FLUMP_FLIP","Flip");
    check(ambiguous_flip.resolve("Flip",true)=="ID_TRICK_FLIP" && ambiguous_flip.resolve("Flip").empty(),"Onboard flip versus slam disambiguation failed");
    // Native all-wheel sequence completion permits a late same-direction revert
    // and cancels an opposite-direction correction, without a time window.
    for(const bool opposite:{false,true}) {
        trick_logger::TrickSetTracker tracker{true};sample={};sample.valid=true;sample.entity=1;sample.goofy=false;sample.state=100;sample.now=1;sample.steps=1;tracker.update(sample);
        sample.now+=16;++sample.steps;sample.state=201;tracker.update(sample);
        sample.now+=16;++sample.steps;sample.yaw=175;tracker.update(sample);
        sample.now+=16;++sample.steps;sample.state=100;tracker.update(sample);
        for(unsigned frame=0;frame<32;++frame){sample.now+=16;++sample.steps;check(!tracker.update(sample),"Completed before native sequence ended");}
        sample.now+=16;++sample.steps;sample.state=101;sample.yaw=opposite?0:350;tracker.update(sample);
        sample.now+=16;++sample.steps;sample.state=100;sample.sequence_ends=1;const auto recovered=tracker.update(sample);
        check(recovered && recovered->segments.front().degrees==(opposite?0:360),"Delayed landing revert rotation or cancellation lost");
    }
    // Exhaust recovery with repeated same-direction OR reversing grounded turns.
    // Neither another revert nor direction reversal replenishes the budget.
    for(const bool reversing:{false,true}) {
        begin(all);turn(true);sample={};sample.valid=true;sample.entity=1;sample.goofy=false;sample.state=100;sample.now=1;sample.steps=1;skate::sample(sample);
        sample.now+=16;++sample.steps;sample.state=201;skate::sample(sample);observe(flip);
        sample.now+=16;++sample.steps;sample.state=101;skate::sample(sample);
        for(unsigned frame=0;frame<30;++frame){sample.now+=16;++sample.steps;sample.yaw+=reversing && frame%2?-15:15;skate::sample(sample);}
        const auto rejected=take();check(rejected && !rejected->landed && rejected->violation==Category::grounded_rotation,"Excessive recovery did not fail");
        sample.now+=16;++sample.steps;sample.state=100;sample.sequence_ends=1;skate::sample(sample);observe(flip);check(!take(),"Excessive recovery revived after landing");end();
    }
    // Genuine new air resets the recovery allowance; an airborne 720 has none.
    {
        trick_logger::TrickSetTracker tracker{true};sample={};sample.valid=true;sample.entity=1;sample.state=100;sample.now=1;sample.steps=1;tracker.update(sample);
        sample.now+=16;++sample.steps;sample.state=201;tracker.update(sample);
        for(unsigned i=0;i<48;++i){sample.now+=16;++sample.steps;sample.yaw+=15;check(!tracker.update(sample),"Airborne 720 was capped");}
        sample.now+=16;++sample.steps;sample.state=101;tracker.update(sample);
        for(unsigned i=0;i<28;++i){sample.now+=16;++sample.steps;sample.yaw+=15;check(!tracker.update(sample),"Forgiving 360 recovery was rejected");}
        sample.now+=16;++sample.steps;sample.state=201;check(!tracker.update(sample),"New air did not reset allowance");
        sample.now+=16;++sample.steps;sample.state=101;tracker.update(sample);
        for(unsigned i=0;i<28;++i){sample.now+=16;++sample.steps;sample.yaw+=15;check(!tracker.update(sample),"Prior landing's allowance leaked into new air");}
    }
    begin(all);turn(true);
    for(const auto cat:{1u,16u,18u,19u})check(native_category_permission(cat)==true,"Advanced flip category excluded");
    check(native_category_permission(5)==false,"Manual category allowed");
    check(!native_category_permission(999),"Unknown native category overridden");
    turn(false);check(!native_category_permission(19),"Opponent native category overridden");end();
    begin({0xfe,true});turn(true);check(native_category_permission(19)==false,"Disabled hospital flip allowed");end();
    begin(all);DetectedAttempt target;target.id=1;target.landed=true;target.signature=a.signature.tokens;target.display="Kickflip";target.native[0]=0;
    prepare_native(2,target);auto native=target.native;bool landed=true;
    check(judge_native(2,true,native,landed) && landed,"Setter failed");
    auto copy=target;copy.native[0]=99;prepare_native(3,copy);native=copy.native;
    check(judge_native(3,false,native,landed) && landed && native==target.native,"Native follower record did not match canonical target");
    copy.signature.push_back("different");prepare_native(3,copy);landed=true;
    check(judge_native(3,false,native,landed) && !landed,"Canonical mismatch passed native comparison");
    prepare_native(2,target);landed=true;check(judge_native(2,true,native,landed) && !landed,"Repeated set accepted");end();
    using namespace multiplayer;ThrowdownMessage m;m.kind=ThrowdownMessage::Kind::attempt;m.leader=76561198000000010ULL;m.id=1;m.value=1;m.rules_mask=0x6f;m.detected={1,2,3};
    const auto bytes=encode_throwdown(m);check(decode_throwdown(bytes)==m,"Detector wire roundtrip failed");
    auto short_bytes=bytes;short_bytes.pop_back();check(!decode_throwdown(short_bytes),"Truncated detector payload accepted");
    m.detected.clear();check(decode_throwdown(encode_throwdown(m))==m,"An attempt without a detector result was refused");
    m.detected.resize(3501);check(!valid_throwdown(m),"Oversized detector accepted");
    ThrowdownMessage start;start.kind=ThrowdownMessage::Kind::start;start.leader=m.leader;start.id=1;start.order={m.leader};start.rules_mask=0x6f;start.settings={4,5};
    check(decode_throwdown(encode_throwdown(start))==start,"S.K.A.T.E. start setup roundtrip failed");
    ThrowdownMessage offer;offer.kind=ThrowdownMessage::Kind::offer;offer.leader=m.leader;offer.id=1;offer.series="ThrowdownSkate";offer.placement={1};offer.settings={4};offer.rules_mask=0x6f;
    check(decode_throwdown(encode_throwdown(offer))==offer,"S.K.A.T.E. offer setup roundtrip failed");
    std::cout<<"PASS: recorded localized ticker identities, category enforcement, Boned Ollie, native comparison bridge, repeated sets and detector wire\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
