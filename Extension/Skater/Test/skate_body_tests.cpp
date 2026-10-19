#include "Extension/Skater/skate_detector.h"
#include "Extension/Skater/riding_heading.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace dingosdk::skate {
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::array<float,16> pose(double yaw,double pitch) {
    yaw/=57.29577951308232;pitch/=57.29577951308232;
    const auto sy=std::sin(yaw),cy=std::cos(yaw),sp=std::sin(pitch),cp=std::cos(pitch);
    return {float(cy),0,float(-sy),0,float(sy*sp),float(cp),float(cy*sp),0,float(sy*cp),float(-sp),float(cy*cp),0,0,0,0,1};
}
}
void body_regressions(const char* path) {
    const auto body_record=[](const char* base,const char* direction,const char* count="") {
        auto r=Json::object();r["landed"]=true;r["rotation_complete"]=true;
        auto row=Json::object();row["phase_id"]=1;row["phase_kind"]="air";row["candidate_segment_id"]=1;row["observation_id"]=1;row["name_resolved"]=true;
        auto labels=Json::object();labels["base_name"]=base;labels["body_flip_direction"]=direction;labels["body_flip_rotation"]=count;
        row["raw_labels"]=labels;r["ticker_candidates"]=Json::array();r["ticker_candidates"].push_back(row);
        auto spin=Json::object();spin["accepted_spin"]=0;spin["starting_stance"]="normal";spin["direction"]="";
        r["spins"]=Json::array();r["spins"].push_back(spin);return r;
    };
    for(const auto direction:{"ID_FLIPDIRECTION_FRONTFLIP","ID_FLIPDIRECTION_BACKFLIP"}) {
        const auto grabbed=body_record("ID_TRICK_TAILGRAB",direction);
        const auto pop=body_record("ID_TRICK_OLLIE",direction);
        const auto flips=normalize(grabbed,{1,true}),ollie=normalize(pop,{1,true});
        check(flips.signature.error.empty() && flips.signature.tokens==ollie.signature.tokens,"Body flip depended on disabled grab carrier");
        check(flips.display.find(direction==std::string_view("ID_FLIPDIRECTION_FRONTFLIP")?"Front Flip":"Back Flip")!=std::string::npos && flips.display.find("Ollie")==std::string::npos,"Body flip displayed as plain Ollie");
        check(flips.phases.at(0).at("components").size()==1,"Disabled grab remained in body-flip match");
        const auto grab_only=normalize(grabbed,{2,true});
        const auto plain_grab=normalize(body_record("ID_TRICK_TAILGRAB",""),{2,true});
        check(grab_only.signature.error.empty() && grab_only.signature.tokens==plain_grab.signature.tokens,"Disabled body flip contaminated enabled grab");
        check(normalize(grabbed,{0,true}).signature.error=="No recognized trick","Disabled-only body flip set a trick");
        check(!normalize(body_record("ID_TRICK_TAILGRAB",""),{1,true}).signature.error.empty(),"Plain disabled grab became a body flip");
        const auto both=normalize(grabbed,{3,true});
        check(both.signature.error.empty() && both.phases.at(0).at("components").size()==2 && both.signature.tokens!=flips.signature.tokens,"Enabled grab requirement was lost");
        const auto double_flip=normalize(body_record("ID_TRICK_TAILGRAB",direction,"ID_BODYFLIPROTATION_DOUBLE"),{1,true});
        check(double_flip.signature.tokens!=flips.signature.tokens,"Body flip count was erased");
        auto ground=grabbed;ground["ticker_candidates"][0]["phase_kind"]="ground";ground["ticker_candidates"][0]["grounded_only"]=true;
        check(!normalize(ground,{1,true}).signature.error.empty(),"Ground-only modifier became an airborne flip");
        auto repeated=grabbed;auto copy=grabbed.at("ticker_candidates").at(0);copy["observation_id"]=2;repeated["ticker_candidates"].push_back(copy);
        check(normalize(repeated,{1,true}).signature.tokens==flips.signature.tokens,"Repeated somersault modifier doubled the trick");
        const auto flipped=normalize(body_record("ID_TRICK_KICKFLIP",direction),{1,true});
        check(flipped.signature.error.empty() && flipped.phases.at(0).at("components").size()==2,"Eligible board flip was lost from body flip");
    }
    check(normalize(body_record("ID_TRICK_OLLIE","ID_FLIPDIRECTION_FRONTFLIP"),{1,true}).signature.tokens!=
          normalize(body_record("ID_TRICK_OLLIE","ID_FLIPDIRECTION_BACKFLIP"),{1,true}).signature.tokens,"Front and back flip matched each other");
    for(const double total:{-720.0,-360.0,-180.0,0.0,180.0,360.0,720.0})for(const double pitch:{-720.0,-360.0,360.0,720.0}) {
        double accumulated{},previous{};
        for(unsigned i=0;i<=120;++i) {
            const auto heading=trick_logger::riding_heading(pose(total*i/120,pitch*i/120));
            check(heading.has_value(),"Valid front/backflip lost heading");
            if(i)accumulated+=std::remainder(*heading-previous,360.0);previous=*heading;
        }
        check(std::abs(accumulated-total)<.001,"Somersault altered measured horizontal spin");
    }
    auto invalid=pose(0,0);invalid[0]=std::numeric_limits<float>::quiet_NaN();
    check(!trick_logger::riding_heading(invalid),"Invalid pose granted heading");
    invalid=pose(0,0);invalid[0]=0;invalid[1]=1;
    check(!trick_logger::riding_heading(invalid),"Vertical right axis granted unsupported heading");
    std::ifstream file(path);check(file.good(),"Missing recorded body-flip fixture");
    const std::string text{std::istreambuf_iterator<char>(file),{}};const auto fixture=Json::parse(text);
    unsigned landed{},bailed{},duplicates{};
    for(const auto& c:fixture.at("body_flips")) {
        trick_logger::TrickSetTracker stable{true,true,true},old{true,true,true};
        std::optional<trick_logger::TrickSetResult> result,old_result;
        for(const auto& x:c.at("samples")) {
            trick_logger::SetSample s;s.entity=1;s.now=x.at("now").get<std::uint64_t>();s.steps=x.at("steps").get<std::uint64_t>();
            s.state=x.at("state").get<unsigned>();s.sequence_ends=x.at("sequence_ends").get<std::uint64_t>();s.wipeouts=x.at("wipeouts").get<std::uint64_t>();s.goofy=x.at("goofy").get<bool>();
            std::optional<double> heading;
            if(x.at("matrix").is_array() && x.at("matrix").size()==16) {
                std::array<float,16> matrix{};for(std::size_t i=0;i<16;++i)matrix[i]=x.at("matrix").at(i).get<float>();heading=trick_logger::riding_heading(matrix);
            }
            s.valid=x.at("ready").get<bool>() && heading.has_value();s.yaw=heading.value_or(0);
            if(!result){if(auto done=stable.update(s))result=done;stable.retain_from(1);stable.stance(1,"normal");}
            s.valid=x.at("ready").get<bool>() && !x.at("old_heading").is_null();s.yaw=s.valid?x.at("old_heading").get<double>():0;
            if(!old_result){if(auto done=old.update(s))old_result=done;old.retain_from(1);old.stance(1,"normal");}
        }
        check(result && result->landed.has_value(),"Recorded flip did not complete");
        if(c.at("landed").get<bool>()) {
            check(result->landed==true && result->rotation_complete,"Recorded landed frontflip lost complete rotation evidence");
            check(old_result && !old_result->rotation_complete,"Recorded old forward-axis failure was not reproduced");
            check(result->segments.at(0).degrees==0,"Frontflip was miscounted as horizontal spin");
            auto r=Json::object();r["landed"]=true;r["rotation_complete"]=true;r["ticker_candidates"]=Json::array();r["spins"]=Json::array();r["movement_phases"]=Json::array();
            std::map<std::uint64_t,Json> latest;for(const auto& row:c.at("rows"))latest[row.at("observation_id").get<std::uint64_t>()]=row;
            for(const auto& [id,row]:latest)r["ticker_candidates"].push_back(row);
            for(const auto& spin:result->segments){auto j=Json::object();j["accepted_spin"]=spin.degrees;j["starting_stance"]=spin.stance;j["direction"]=spin.direction;r["spins"].push_back(j);}
            for(const auto& phase:result->phases){auto j=Json::object();j["id"]=phase.id;j["kind"]=phase.kind;j["segment"]=phase.segment;r["movement_phases"].push_back(j);}
            const auto n=normalize(r,{0xff,true});check(n.signature.error.empty() && n.display.find("Front Flip")!=std::string::npos,"Recorded frontflip modifier was not retained");
            const auto flips_only=normalize(r,{1,true});
            check(flips_only.signature.error.empty() && flips_only.display=="Front Flip","Recorded body flip required Grabs enabled");++landed;
        } else {check(result->landed==false,"Recorded backflip bail became success");++bailed;}
    }
    for(const auto& c:fixture.at("duplicate_cases")) {
        const auto n=normalize(c.at("recording"),{0xff,true});
        check(n.signature.error.empty() && n.phases.size()==1,"Recorded duplicate case lost its phase");
        check(n.phases.at(0).at("components").size()==c.at("expected_components").get<std::size_t>(),"Recorded adjacent ticker duplicates retained");++duplicates;
    }
    check(landed==1 && bailed==1 && duplicates==2,"Recorded body/duplicate coverage changed");
    std::cout<<"PASS: recorded frontflip heading recovery, preserved backflip bail, consecutive duplicates and independent somersault/yaw measurement\n";
}
}
