#include "Extension/Skater/skate_detector.h"
#include "Extension/Skater/trick_diagnostics.h"
#include <stdexcept>
namespace dingosdk::skate {
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Json row(const char* id,const char* display,unsigned observation=1,unsigned phase=1,unsigned segment=1) {
    auto r=Json::object();r["name"]=display;r["name_resolved"]=true;r["observation_id"]=observation;r["phase_id"]=phase;r["candidate_segment_id"]=segment;
    r["raw_labels"]=Json::object();r["raw_labels"]["base_name"]=id;return r;
}
Json attempt(std::initializer_list<Json> rows) {
    auto r=Json::object();r["landed"]=true;r["spins"]=Json::array();r["ticker_candidates"]=Json::array();
    for(unsigned i=0;i<2;++i){auto spin=Json::object();spin["accepted_spin"]=i?540:180;spin["direction"]=i?"FS":"BS";spin["starting_stance"]="normal";r["spins"].push_back(spin);}
    for(const auto& x:rows)r["ticker_candidates"].push_back(x);return r;
}
trick_logger::SetSample sample(unsigned state,std::uint64_t now,double yaw=0) {
    trick_logger::SetSample s;s.entity=1;s.valid=true;s.goofy=false;s.state=state;s.now=now;s.steps=now;s.yaw=yaw;return s;
}
}
void phase_regressions() {
    const Rules all{0xff,true};const auto flip=row("ID_TRICK_KICKFLIP","Kickflip");const auto ollie=row("ID_TRICK_OLLIE","Ollie");
    const auto bone=row("ID_TRICK_BONEDOLLIE","Boned Ollie");auto manual=row("ID_TRICK_MANUAL","Manual",4);
    const auto base=normalize(attempt({flip}),all);
    require(normalize(attempt({bone}),all).signature.tokens==normalize(attempt({ollie}),all).signature.tokens,"Boned/pop equivalence lost");
    auto extra_pop=ollie;extra_pop["observation_id"]=2;
    require(normalize(attempt({bone,extra_pop}),all).signature.tokens==normalize(attempt({ollie}),all).signature.tokens,"Duplicate pop inflated set");
    require(normalize(attempt({bone}),all).display=="BS 180 Ollie","Canonical Boned Ollie display wrong");
    require(normalize(attempt({flip,manual}),all).signature.tokens==base.signature.tokens,"Manual changed core signature");
    require(!normalize(attempt({manual}),all).signature.error.empty(),"Manual-only set succeeded");
    auto ground_grab=row("ID_TRICK_NOSEGRAB","Nosegrab",5);ground_grab["grounded_only"]=true;
    require(normalize(attempt({flip,ground_grab}),{0xfd,true}).signature.tokens==base.signature.tokens,"Disabled grounded grab failed valid flip");
    ground_grab["grounded_only"]=false;
    require(normalize(attempt({flip,ground_grab}),{0xfd,true}).signature.tokens==base.signature.tokens,"Disabled airborne grab changed the eligible flip");
    const auto stall=row("ID_TRICK_5_0_STALL","5-0 Stall",6);
    const auto without_stall=normalize(attempt({flip}),{0xfb,true});
    require(normalize(attempt({flip,stall}),{0xfb,true}).signature.tokens==without_stall.signature.tokens,"Accidental stall changed set");
    auto second_flip=flip;second_flip["observation_id"]=2;
    require(normalize(attempt({flip,second_flip}),all).signature.tokens==base.signature.tokens,"Consecutive equivalent flips were counted twice");
    auto late=second_flip;late["raw_labels"]["late"]="ID_TRICK_LATE";
    auto under=second_flip;under["raw_labels"]["underflip"]="ID_TRICK_UNDERFLIP";
    const auto under_only=normalize(attempt({under}),all);
    require(under_only.phases.at(0).at("components").size()==1,"Underflip split into multiple tricks");
    require(under_only.phases.at(0).at("components").at(0).at("identity").value("late","").empty(),"Underflip became a late flip");
    require(normalize(attempt({flip,late}),all).phases.at(0).at("components").size()==2,"Late flip merged into ordinary flip");
    require(normalize(attempt({flip,under}),all).phases.at(0).at("components").size()==2,"Underflip merged into ordinary flip");
    require(under_only.signature.tokens!=normalize(attempt({late}),all).signature.tokens,"Underflip and late flip became equivalent");
    auto heel_between=row("ID_TRICK_HEELFLIP","Heelflip",3);second_flip["observation_id"]=4;
    require(normalize(attempt({flip,heel_between,second_flip}),all).phases.at(0).at("components").size()==3,"Nonconsecutive repeat was removed");
    const auto grind=row("ID_TRICK_TAILSLIDE","Tailslide",2,2,1),heel=row("ID_TRICK_HEELFLIP","Heelflip",3,3,2);
    const auto combination=normalize(attempt({flip,grind,heel}),all);
    require(combination.display=="BS 180 Kickflip | Tailslide | FS 540 Heelflip","Per-phase spin wording wrong");
    auto moved_flip=flip,moved_grind=grind,moved_heel=heel;moved_flip["phase_id"]=5;moved_grind["phase_id"]=7;moved_heel["phase_id"]=9;
    require(normalize(attempt({moved_flip,moved_grind,moved_heel}),all).signature.tokens==combination.signature.tokens,"Internal phase IDs leaked into matching");
    {
        trick_logger::TrickSetTracker t{true};t.update(sample(100,1));t.update(sample(201,17));
        auto first=flip;first["first_seen_ms"]=20;require(associate_observation(first,t,nullptr,20),"Entry row did not associate");
        t.update(sample(403,33,90));t.update(sample(201,49,90));
        auto exit=heel;exit["first_seen_ms"]=50;exit["raw_labels"]["stance"]="ID_STANCE_NOLLIE";
        require(associate_observation(exit,t,nullptr,50),"Exit row did not associate");
        auto revised=first;revised["revision"]=2;require(associate_observation(revised,t,&first,55),"Revised entry missing");
        require(revised.at("phase_id")==first.at("phase_id") && revised.value("candidate_segment_id",0u)==1,"Ticker revision moved entry into exit");
        t.update(sample(100,65,90));auto s=sample(100,81,90);s.sequence_ends=1;const auto done=t.update(s);
        require(done && done->segments[0].stance=="normal" && done->segments[1].stance=="nollie","Delayed labels overwrote phase stance");
    }
    const std::pair<double,int> grind_angles[]{{0,0},{90,0},{114.9,0},{115,180},{115.1,180},{134.9,180},{135,180},{180,180},{360,360},{540,540},{720,720}};
    for(const auto& [entry,entry_expected]:grind_angles)for(const auto& [exit,exit_expected]:grind_angles)
    for(const double entry_sign:{-1.0,1.0})for(const double exit_sign:{-1.0,1.0}) {
        trick_logger::TrickSetTracker t{true,true,true};std::uint64_t now=1;double yaw{};
        const auto step=[&](unsigned state){now+=16;t.update(sample(state,now,yaw));};
        const auto rotate=[&](double angle){const auto steps=static_cast<unsigned>(std::max(1.0,std::ceil(std::abs(angle)/90)));for(unsigned i=0;i<steps;++i){yaw+=angle/steps;step(201);}};
        step(100);step(201);t.retain_from(1);t.stance(1,"normal");rotate(entry*entry_sign);
        step(403);yaw+=90;step(403);step(201);t.stance(2,"normal");rotate(exit*exit_sign);step(100);
        auto s=sample(100,now+16,yaw);s.sequence_ends=1;const auto done=t.update(s);
        require(done && done->segments.size()==2,"Grind entry/exit not separate");
        if(done->segments[0].degrees!=entry_expected || done->segments[1].degrees!=exit_expected)
            throw std::runtime_error("Grind threshold: entry "+std::to_string(entry*entry_sign)+" exit "+std::to_string(exit*exit_sign)+
                " measured "+std::to_string(done->segments[0].accepted_net)+" / "+std::to_string(done->segments[1].accepted_net)+
                " rounded "+std::to_string(done->segments[0].degrees)+" / "+std::to_string(done->segments[1].degrees));
        if(entry_expected)require(done->segments[0].direction==(entry_sign>0?"FS":"BS"),"Grind entry spin direction changed");
        if(exit_expected)require(done->segments[1].direction==(exit_sign>0?"FS":"BS"),"Grind exit spin direction changed");
    }
    // Ordinary airborne rounding must not inherit the grind-only minimum.
    for(const auto& [angle,expected]:{std::pair{89.9,0},std::pair{90.0,180},std::pair{114.9,180},std::pair{115.0,180}})
    for(const double sign:{-1.0,1.0}) {
        trick_logger::TrickSetTracker t{true};t.update(sample(100,1));t.update(sample(201,17));t.stance(1,"normal");
        t.update(sample(201,33,angle*sign));t.update(sample(100,49,angle*sign));
        auto s=sample(100,65,angle*sign);s.sequence_ends=1;const auto done=t.update(s);
        require(done && done->segments[0].degrees==expected,"Ordinary airborne rounding changed with grind minimum");
    }
    {
        trick_logger::TrickSetTracker t{true};t.update(sample(100,1));t.update(sample(201,17,170));t.stance(1,"normal");t.update(sample(602,33,170));
        for(unsigned i=0;i<24;++i)require(!t.update(sample(602,49+i*16,170+(i+1)*45)),"Manual rotation exhausted recovery allowance");
        auto s=sample(100,449,1250);s.sequence_ends=1;const auto done=t.update(s);
        require(done && done->segments[0].degrees==180 && done->grounded_travel==0,"Manual accrued matching rotation");
    }
    begin({0xfd,true});turn(true);
    require(native_category_permission(2)==false,"Ground-only grab can seed native eligibility");
    skate::sample(sample(100,1));skate::sample(sample(201,17));observe(flip);observe(manual);
    require(!take(),"Manual immediately failed active set");
    skate::sample(sample(100,33));auto grab=row("ID_TRICK_NOSEGRAB","Nosegrab",8);observe(grab);
    require(!take(),"Ground-only disabled grab failed active set");native_outcome(true,{});
    auto landing=sample(100,49);landing.sequence_ends=1;skate::sample(landing);const auto clean=take();
    require(clean && clean->landed,"Ignored grounded actions prevented gameplay completion");end();
    for(const auto id:{"ID_TRICK_POWERSLIDE","ID_TRICK_TRICKLESS_AIR","ID_TRICK_PUMPING"}) {
        auto ignored=row(id,"Excluded movement",10);require(!normalize(attempt({ignored}),all).signature.error.empty(),"Excluded movement set a trick");
        require(normalize(attempt({flip,ignored}),all).signature.tokens==base.signature.tokens,"Excluded movement changed valid trick");

    }
    auto plain=attempt({ollie});plain["spins"].at(0)["accepted_spin"]=0;plain["spins"].at(0)["direction"]="";
    const auto pop=normalize(plain,all);
    require(pop.signature.error.empty() && pop.display=="Ollie","Single zero-spin Ollie did not normalize");
    begin(all);turn(true);skate::sample(sample(100,1));skate::sample(sample(201,17));observe(ollie);
    auto landed=sample(100,33);landed.sequence_ends=1;skate::sample(landed);landed.now=49;landed.steps=49;skate::sample(landed);
    native_outcome(true,{});auto set=take();require(set && set->landed && set->display=="Ollie","Plain Ollie capture failed");
    prepare_native(2,*set);std::array<std::uint8_t,28> record{};bool successful=true;
    require(judge_native(2,true,record,successful) && successful,"Plain Ollie setter failed");
    prepare_native(3,*set);successful=true;require(judge_native(3,false,record,successful) && successful,"Plain Ollie defender failed");end();
    begin(all);turn(true);native_outcome(false,{});require(take().has_value(),"Unclassified native failure was ignored");end();
}
}
