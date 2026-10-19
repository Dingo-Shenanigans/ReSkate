#include "Extension/Skater/skate_detector.h"
#include <Windows.h>
#include <stdexcept>
#include <iostream>
namespace dingosdk::skate {
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
Json row(const char* id,unsigned phase,unsigned segment,unsigned observation) {
    auto r=Json::object();r["observation_id"]=observation;r["phase_id"]=phase;r["candidate_segment_id"]=segment;
    r["raw_labels"]=Json::object();r["raw_labels"]["base_name"]=id;r["name_resolved"]=true;return r;
}
Json recording(std::initializer_list<const char*> phases,std::initializer_list<Json> rows) {
    auto r=Json::object();r["landed"]=true;r["rotation_complete"]=true;r["movement_phases"]=Json::array();r["spins"]=Json::array();r["ticker_candidates"]=Json::array();
    unsigned id{},segment{};
    for(const auto kind:phases) {
        if(std::string_view(kind)=="air") {
            ++segment;auto spin=Json::object();spin["accepted_spin"]=180;spin["direction"]="BS";spin["starting_stance"]="normal";r["spins"].push_back(spin);
        }
        auto p=Json::object();p["id"]=++id;p["kind"]=kind;p["segment"]=segment;r["movement_phases"].push_back(p);
    }
    for(const auto& x:rows)r["ticker_candidates"].push_back(x);return r;
}
struct Run {
    trick_logger::SetSample s;
    explicit Run(Rules rules){begin(rules);turn(true);s.valid=true;s.entity=1;s.goofy=false;s.now=GetTickCount64();step(100);}
    ~Run(){end();}
    void step(unsigned state,double yaw=0,bool sequence=false,bool valid=true){s.now+=16;++s.steps;s.state=state;s.yaw=yaw;s.valid=valid;if(sequence)++s.sequence_ends;skate::sample(s);}
    void observe_row(Json r){if(!r.contains("first_seen_ms"))r["first_seen_ms"]=s.now;observe(r);}
    std::optional<DetectedAttempt> complete(double yaw=0){native_outcome(true,{});step(100,yaw,true);step(100,yaw);return take();}
};
}
void capture_regressions() {
    const Rules all{0xff,true},flat{0xf9,true},grabs{0xfb,true};
    for(const auto direction:{"ID_FLIPDIRECTION_FRONTFLIP","ID_FLIPDIRECTION_BACKFLIP"}) {
        Run r({1,true});r.step(201);
        auto carrier=row("ID_TRICK_TAILGRAB",1,1,1);carrier["raw_labels"]["body_flip_direction"]=direction;
        r.observe_row(carrier);
        const auto done=r.complete();
        check(done && done->landed && done->signature.size()==2,"Flips-only body-flip carrier was discarded during gameplay capture");
        check(done->display.find("Flip")!=std::string::npos,"Captured body flip lacked readable name");
    }
    for(const auto id:{"ID_TRICK_SLAPPYENTER","ID_TRICK_SLAPPYENTER_STALL"}) {
        const auto slappy=row(id,1,0,1),core=row("ID_TRICK_KICKFLIP",1,1,2);
        check(ignored_reason(slappy)=="slappy entry observation","Slappy entry exclusion reason missing");
        check(!normalize(recording({"grind","ground"},{slappy}),all).signature.error.empty(),"Slappy-only result set a trick");
        check(normalize(recording({"air","ground"},{slappy,core}),all).signature.tokens==
            normalize(recording({"air","ground"},{core}),all).signature.tokens,"Slappy entry changed matching signature");
        for(const auto eligible:{"ID_TRICK_OLLIE","ID_TRICK_KICKFLIP","ID_TRICK_TAILSLIDE"}) {
            Run r(all);r.step(403);
            for(unsigned observation=1;observation<=20;++observation)r.observe_row(row(id,1,0,observation));
            r.step(100);r.step(100,0,true);check(!take(),"Ignored Slappy activity consumed the turn");
            r.step(std::string_view(eligible)=="ID_TRICK_TAILSLIDE"?403:201);
            r.observe_row(row(eligible,1,1,21));
            const auto done=r.complete();check(done && done->landed && done->display.find("SLAPPY")==std::string::npos,
                "Eligible trick after Slappy entry was blocked or retained Slappy text");
        }
    }
    auto flip=row("ID_TRICK_KICKFLIP",1,1,1),late=row("ID_TRICK_HEELFLIP",1,1,2),mute=row("ID_TRICK_MUTENOSEBONEGRAB",1,1,3);
    late["raw_labels"]["late"]="ID_TRICK_LATE";
    auto extra=row("ID_TRICK_HOSPITALFLIP",3,2,4);
    {
        const auto repeat=row("ID_TRICK_KICKFLIP",3,2,4);
        auto adjacent=recording({"air","grind","air","ground"},{flip,repeat});
        check(normalize(adjacent,all).phases.size()==1,"Equivalent adjacent trick across empty alignment phases doubled");
        adjacent["spins"].at(1)["accepted_spin"]=360;
        check(normalize(adjacent,all).phases.size()==2,"Different independent spin requirements collapsed");
        auto separated=recording({"air","grind","air","ground"},{flip,row("ID_TRICK_TAILSLIDE",2,1,3),repeat});
        check(normalize(separated,all).phases.size()==3,"Grind-separated entry and exit tricks collapsed");
    }
    const auto base=recording({"air","ground","air","ground"},{flip,late,mute,extra});
    const auto n=normalize(base,flat),without_extra=normalize(recording({"air","ground"},{flip,late}),flat);
    check(n.signature.error.empty() && n.signature.tokens==without_extra.signature.tokens,"Flat capture did not preserve late flip and exclude later jump");
    check(n.capture.first==1 && n.capture.last==1 && n.capture.closed,"Flat capture boundary incorrect");
    const auto with_grab=normalize(base,grabs);
    check(with_grab.signature.error.empty() && with_grab.signature.tokens!=n.signature.tokens,"Enabled grab was omitted");
    const auto disabled_rail=recording({"air","grind","air","ground"},{flip,row("ID_TRICK_TAILSLIDE",2,1,5),extra});
    check(normalize(disabled_rail,flat).signature.tokens==normalize(recording({"air","ground"},{flip}),flat).signature.tokens,"Disabled rail enabled a second jump");
    auto chain=recording({"air","grind","air","grind","air","ground","air","grind"},
        {flip,row("ID_TRICK_TAILSLIDE",2,1,5),row("ID_TRICK_NOSESLIDE",2,1,6),row("ID_TRICK_HEELFLIP",3,2,7),
         row("ID_TRICK_TAILSLIDE",4,2,8),row("ID_TRICK_KICKFLIP",5,3,9),row("ID_TRICK_MUTENOSEBONEGRAB",5,3,10),row("ID_TRICK_HEELFLIP",7,4,11),row("ID_TRICK_NOSESLIDE",8,4,12)});
    const auto chained=normalize(chain,all);
    check(chained.signature.error.empty() && chained.capture.last==5 && chained.phases.size()==5,"Rail transfer or grind change was truncated");
    auto prefix=chain;prefix["ticker_candidates"]=Json::array();for(std::size_t i=0;i<7;++i)prefix["ticker_candidates"].push_back(chain.at("ticker_candidates").at(i));
    check(normalize(prefix,all).signature.tokens==chained.signature.tokens,"Later jump/grind extended grounded combination");
    auto disabled_first=recording({"air","ground","air","ground"},{flip,row("ID_TRICK_MUTENOSEBONEGRAB",3,2,8)});
    const auto grab_only=normalize(disabled_first,{0xfe,true});
    check(grab_only.signature.error.empty() && grab_only.capture.first==3,"Disabled-only movement consumed first eligible jump");
    check(!normalize(recording({"air","ground"},{flip}),{0xfe,true}).signature.error.empty(),"Disabled-only attempt succeeded");
    {
        Run r(flat);r.step(201);r.observe_row(flip);r.step(201,170);r.step(103,170);r.step(101,340);
        r.step(201,350);r.observe_row(extra);r.step(201,1000);r.step(101,1000);
        // Excluded later recovery cannot inflate the retained spin or exhaust its budget.
        for(unsigned i=0;i<32;++i)r.step(101,1000+15*i);
        const auto done=r.complete(1465);
        check(done && done->landed && done->signature.front()=="spin:normal:360:FS","Manual-linked jump altered retained spin or recovery budget");
        check(done->display.find("Hospital")==std::string::npos,"Extra jump appeared in target text");check(!take(),"Bounded result delivered twice");
    }
    {
        Run r(flat);r.step(201);r.observe_row(flip);r.step(103);r.step(201);r.step(201,0,false,false);r.step(100);
        const auto done=r.complete();check(done && done->landed,"Missing excluded rotation poisoned retained evidence");
    }
    {
        Run r(all);r.step(201);r.observe_row(flip);r.step(403);r.observe_row(row("ID_TRICK_TAILSLIDE",2,1,2));
        r.step(403,90);r.observe_row(row("ID_TRICK_NOSESLIDE",2,1,3));r.step(201,90);r.observe_row(row("ID_TRICK_HEELFLIP",3,2,4));
        r.step(403,180);r.observe_row(row("ID_TRICK_TAILSLIDE",4,2,5));r.step(201,180);r.observe_row(row("ID_TRICK_KICKFLIP",5,3,6));
        r.step(103,180);r.step(201,180);r.observe_row(row("ID_TRICK_HEELFLIP",7,4,7));
        const auto done=r.complete(180);check(done && done->landed && done->evidence.at("capture_boundary").value("last_phase",0u)==5,"Live detector did not bound chained grind route at manual landing");
    }
    {
        Run r(flat);r.step(201);auto delayed=flip;delayed["first_seen_ms"]=r.s.now;r.observe_row(delayed);r.step(103);r.step(201);
        delayed["raw_labels"]["underflip"]="ID_TRICK_UNDERFLIP";delayed["labels"]=Json::object();delayed["labels"]["base_name"]="Kickflip";delayed["labels"]["underflip"]="Underflip";r.observe_row(delayed);
        const auto done=r.complete();check(done && done->landed && done->display.find("Underflip")!=std::string::npos,"Retained ticker revision lost after cutoff");
    }
    {
        Run r({0xfe,true});r.step(201);r.observe_row(flip);r.step(100);r.step(100,0,true);
        check(!take(),"Disabled-only first jump set a trick");r.step(201);r.observe_row(row("ID_TRICK_MUTENOSEBONEGRAB",3,2,8));
        const auto done=r.complete();check(done && done->landed,"Valid grab after disabled-only activity was blocked");
    }
    {
        Run r(flat);r.step(201);r.observe_row(flip);r.step(103);r.step(201);r.step(300);
        const auto done=take();check(done && !done->landed,"Closed capture bypassed native bail failure");
    }
    {
        Run r({0xfa,true});r.step(201);
        for(unsigned id=1;id<=160;++id)r.observe_row(row("ID_TRICK_KICKFLIP",1,1,id));
        r.observe_row(row("ID_TRICK_MUTENOSEBONEGRAB",1,1,161));
        const auto done=r.complete();check(done && done->landed,"Disabled observations filled storage ahead of an eligible grab");
    }
    {
        Run r(flat);r.step(201);r.observe_row(flip);r.step(103);r.step(201);
        for(unsigned id=2;id<=320;++id)r.observe_row(row("ID_TRICK_HEELFLIP",3,2,id));
        const auto done=r.complete();
        check(done && done->landed && done->evidence.at("phase_association").size()==1 &&
            done->evidence.at("measured_air_recovery_net").size()==1,"Excluded continuation enlarged retained network evidence");
    }
    std::cout<<"PASS: bounded jumps, late flips, grabs, grind changes/transfers, manual landings, delayed revisions and excluded continuation\n";
}
}
