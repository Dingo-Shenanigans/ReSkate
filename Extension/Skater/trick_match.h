#pragma once
#include "Engine/Core/Json/json.h"
#include "trick_set.h"
#include "trick_name_text.h"
#include "trick_identity.h"
#include <vector>
namespace dingosdk::trick_logger {
struct MatchSignature {std::vector<std::string> tokens;std::string error;};
inline MatchSignature match_signature(const Json& result) {
    MatchSignature out;
    if(!result.value("rotation_complete",true)){out.error="Incomplete rotation evidence";return out;}
    if(!result.contains("landed") || !result.at("landed").is_boolean() || !result.at("landed").get<bool>()) {out.error="Attempt did not land";return out;}
    if(!result.contains("spins") || !result.contains("ticker_candidates")){out.error="Missing trick data";return out;}
    if(result.contains("canonical_phases")) {
        unsigned tricks{};
        for(const auto& phase:result.at("canonical_phases")) {
            const auto kind=phase.value("kind","");
            if(kind=="air") {
                const auto spin=phase.value("spin",0);const auto stance=phase.value("stance","");const auto direction=phase.value("direction","");
                if(stance.empty() || (spin && direction.empty())){out.error="Stance or spin direction unknown";return out;}
                out.tokens.push_back("spin:"+stance+":"+std::to_string(spin)+":"+(spin?direction:""));
            } else out.tokens.push_back("phase:"+kind);
            for(const auto& component:phase.at("components")) {out.tokens.push_back(component.at("identity").dump());++tricks;}
        }
        if(!tricks)out.error="No recognized trick";
        return out;
    }
    for(const auto& spin:result.at("spins")) {
        const auto degrees=spin.value("accepted_spin",0);
        const auto stance=spin.value("starting_stance","");
        const auto direction=spin.value("direction","");
        if(stance.empty() || (degrees && direction.empty())) {out.error="Stance or spin direction unknown";return out;}
        out.tokens.push_back("spin:"+stance+":"+std::to_string(degrees)+":"+(degrees?direction:""));
    }
    std::string previous;unsigned tricks{};
    for(const auto& row:result.at("ticker_candidates")) {
        const auto base=row.value("name","");
        if(base=="Revert" || base=="Powerslide")continue;
        if(!row.value("name_resolved",false) || !row.contains("raw_labels")){out.error="Unresolved trick identity";return out;}
        const auto& raw=trick_names::matching_labels(row);auto identity=Json::object();
        auto stable_base=raw.value("base_name",base.c_str());
        if(stable_base=="ID_TRICK_NOLLIE")stable_base="ID_TRICK_OLLIE";
        if(stable_base=="ID_TRICK_BONEDNOLLIE")stable_base="ID_TRICK_BONEDOLLIE";
        identity["base"]=trick_names::plain_text(stable_base);
        identity["segment"]=row.value("candidate_segment_id",std::uint64_t{});
        identity["stance"]=starting_stance(raw.value("stance",""),raw.value("nollie",""),raw.value("base_name",base.c_str()));
        // Body spin is measured independently. Keep board/grind direction and all
        // native flip/late/manual modifiers; exclude text formatting and row ids.
        const bool body_direction=raw.value("base_name","")=="ID_TRICK_OLLIE" || raw.value("base_name","")=="ID_TRICK_NOLLIE" || raw.value("base_name","")=="ID_TRICK_BONEDOLLIE" || raw.value("base_name","")=="ID_TRICK_BONEDNOLLIE";
        identity["direction"]=body_direction?"":trick_names::plain_text(raw.value("direction",""));
        for(const auto key:{"held_rotation","late","body_flip_rotation","body_flip_direction","dark_catch","dark_flip","manual_type","underflip"})identity[key]=trick_names::plain_text(raw.value(key,""));
        const auto token=identity.dump();
        if(token==previous)continue; // Repeated identical native rows during one hold.
        previous=token;out.tokens.push_back(token);++tricks;
    }
    if(!tricks)out.error="No recognized trick";
    return out;
}
inline std::string match_difference(const MatchSignature& target,const MatchSignature& attempt) {
    if(!attempt.error.empty())return attempt.error;
    if(target.tokens.size()!=attempt.tokens.size())return "Different number of tricks or spin segments";
    for(std::size_t i=0;i<target.tokens.size();++i)if(target.tokens[i]!=attempt.tokens[i])
        {
            if(target.tokens[i].starts_with("spin:") || attempt.tokens[i].starts_with("spin:"))
                return "Spin, direction or stance differs: target "+target.tokens[i]+", attempt "+attempt.tokens[i];
            try {
                const auto a=Json::parse(target.tokens[i]),b=Json::parse(attempt.tokens[i]);std::string fields;
                for(const auto& [key,value]:a.items())if(!b.contains(key) || b.at(key)!=value) {
                    if(!fields.empty())fields+=", ";fields+=key+": "+value.dump()+" vs "+(b.contains(key)?b.at(key).dump():"missing");
                }
                return "Trick component differs: "+fields;
            } catch(...) {return "Phase order differs";}
        }
    return {};
}
}
