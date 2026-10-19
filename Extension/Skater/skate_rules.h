#pragma once
#include "trick_metadata.h"
#include "trick_match.h"
#include <optional>
#include <set>
#include <algorithm>
#include <tuple>

namespace dingosdk::skate {
// The detector's matching-rules revision, for logs and replay reports only: players never
// compare it, so any client can play S.K.A.T.E. with any other.
inline constexpr std::uint16_t rules_version=14;
enum class Category : std::uint8_t { none, flips, grabs, grinds, ground, manuals, grounded_rotation, unknown };
struct Rules {
    std::uint8_t values{};
    bool verified{};
    bool allows(Category c) const {
        if(c==Category::manuals)return false;
        const auto bit=c==Category::flips?0:c==Category::grabs?1:c==Category::grinds?2:c==Category::ground?3:8;
        return verified && (bit==8 || (values&(1u<<bit))!=0);
    }
};
inline const char* forbidden(Category c) {
    switch(c) {
    case Category::flips:return "Flips are not allowed.";
    case Category::grabs:return "Grabs are not allowed.";
    case Category::grinds:return "Grinds are not allowed.";
    case Category::grounded_rotation:return "Excessive grounded rotation.";
    case Category::manuals:return "Manuals are not allowed.";
    case Category::ground:return "Ground tricks are not allowed.";
    default:return "Unknown trick category.";
    }
}
inline bool manual_identity(std::string_view id) {
    return id=="ID_TRICK_MANUAL" || id=="ID_TRICK_NOSEMANUAL" || id=="ID_TRICK_MANUALBRAKE";
}
inline bool explicit_manual_revert(const Json& row) {
    if(!row.contains("raw_labels"))return false;
    const auto& labels=trick_names::matching_labels(row);
    return manual_identity(labels.value("base_name","")) && labels.value("manual_type","")=="ID_TRICK_REVERT";
}
inline bool recovery_row(const Json& row,const std::set<std::uint64_t>& episodes={}) {
    if(!row.contains("raw_labels"))return false;
    const auto& labels=trick_names::matching_labels(row);const auto id=labels.value("base_name","");
    if(id=="ID_TRICK_REVERT" || id=="ID_TRICK_POWERSLIDE" || explicit_manual_revert(row))return true;
    // An initially untagged manual row can be a transition within the same native
    // 101 action as an explicitly tagged revert. A held manual outside that action
    // is never rescued by a subsequent revert elsewhere in the combination.
    const auto episode=row.value("landing_revert_episode",std::uint64_t{});
    return manual_identity(id) && labels.value("manual_type","").empty() && episode && episodes.contains(episode);
}
inline Category category(std::string_view id) {
    if(id=="ID_FLIPDIRECTION_FRONTFLIP" || id=="ID_FLIPDIRECTION_BACKFLIP")return Category::flips;
    if(manual_identity(id))return Category::manuals;
    for(const auto& row:metadata)if(row.id==id) {
        const auto m=row.categories;
        if(m&((1u<<3)|(1u<<4)))return Category::grinds;
        if(m&((1u<<2)|(1u<<13)|(1u<<14)|(1u<<15)))return Category::grabs;
        if(m&((1u<<1)|(1u<<16)|(1u<<18)|(1u<<19)))return Category::flips;
        if(m&(1u<<5))return Category::ground;
        if(m&(1u<<6))return Category::ground;
    }
    return Category::unknown;
}
inline bool foundational_pop(std::string_view id) {return id=="ID_TRICK_OLLIE" || id=="ID_TRICK_NOLLIE" || id=="ID_TRICK_QUICKOLLIE" || id=="ID_TRICK_QUICKNOLLIE" || id=="ID_TRICK_BONEDOLLIE" || id=="ID_TRICK_BONEDNOLLIE";}
inline bool boned(std::string_view id) {return id=="ID_TRICK_BONEDOLLIE" || id=="ID_TRICK_BONEDNOLLIE";}
inline bool stall_identity(std::string_view id) {
    for(const auto& row:metadata)if(row.id==id)return (row.categories&(1u<<4))!=0;
    return false;
}
inline bool slappy_entry(std::string_view id) {
    return id=="ID_TRICK_SLAPPYENTER" || id=="ID_TRICK_SLAPPYENTER_STALL";
}
inline std::string ignored_reason(const Json& row) {
    if(!row.contains("raw_labels"))return {};
    const auto id=trick_names::matching_labels(row).value("base_name","");
    if(slappy_entry(id))return "slappy entry observation";
    if(id=="ID_TRICK_TRICKLESS_AIR" || id=="ID_TRICK_PUMPING")return "trickless movement";
    if(id=="ID_TRICK_POWERSLIDE")return "powerslide";
    if(manual_identity(id))return explicit_manual_revert(row)?"manual revert recovery":"ordinary manual";
    if(stall_identity(id))return "stall observation";
    if(recovery_row(row))return "recovery action";
    if(category(id)==Category::grabs && row.value("grounded_only",false))return "grounded grab";
    return {};
}
inline std::string canonical_pop(std::string id) {
    if(id=="ID_TRICK_BONEDOLLIE" || id=="ID_TRICK_QUICKOLLIE")return "ID_TRICK_OLLIE";
    if(id=="ID_TRICK_BONEDNOLLIE" || id=="ID_TRICK_QUICKNOLLIE")return "ID_TRICK_NOLLIE";
    return id;
}
// Somersault evidence is supplied by native ticker modifiers, independently of
// whether its carrier is an enabled grab. Never infer it from measured yaw.
inline bool airborne_body_flip(const Json& row) {
    const auto& labels=trick_names::matching_labels(row);
    const auto direction=labels.value("body_flip_direction","");
    const auto c=category(labels.value("base_name",""));
    return (direction=="ID_FLIPDIRECTION_FRONTFLIP" || direction=="ID_FLIPDIRECTION_BACKFLIP") &&
        (c==Category::flips || c==Category::grabs) && row.value("phase_kind","")=="air" &&
        !row.value("phase_provisional",false) && !row.value("grounded_only",false);
}
inline bool eligible_body_flip(const Json& row,Rules rules) {return rules.allows(Category::flips) && airborne_body_flip(row);}
inline std::vector<Json> separate_body_flip(const Json& input) {
    if(!input.contains("raw_labels") || !airborne_body_flip(input))return {input};
    auto core=input,body=input;
    const auto source=trick_names::matching_labels(input);
    for(const auto key:{"raw_labels","identity_labels","labels"})if(core.contains(key)) {
        core[key]["body_flip_direction"]="";core[key]["body_flip_rotation"]="";
    }
    auto identity=Json::object();
    for(const auto& [key,value]:source.items())identity[key]="";
    const auto direction=source.value("body_flip_direction","");
    identity["base_name"]=direction;identity["body_flip_direction"]=direction;
    identity["body_flip_rotation"]=source.value("body_flip_rotation","");
    identity["stance"]=source.value("stance","");identity["nollie"]=source.value("nollie","");
    body["identity_labels"]=identity;body["raw_labels"]=identity;
    const std::string name=direction=="ID_FLIPDIRECTION_FRONTFLIP"?"Front Flip":"Back Flip";
    auto readable=identity;for(const auto& [key,value]:identity.items())readable[key]="";
    readable["base_name"]=name;
    if(input.contains("labels"))readable["body_flip_rotation"]=input.at("labels").value("body_flip_rotation","");
    body["labels"]=readable;body["name"]=name;body["name_resolved"]=true;body["body_flip_component"]=true;
    return {core,body};
}
inline bool associate_observation(Json& row,trick_logger::TrickSetTracker& tracker,const Json* previous,std::uint64_t now) {
    if(previous && previous->contains("phase_id") && !previous->value("phase_provisional",false)) {
        for(const auto key:{"phase_id","phase_kind","candidate_segment_id","grounded_only"})
            if(previous->contains(key))row[key]=previous->at(key);
    } else {
        const auto* phase=tracker.phase_at(row.value("first_seen_ms",now));
        if(!phase)return false;
        const auto id=trick_names::matching_labels(row).value("base_name","");const auto c=category(id);
        if(c==Category::flips && phase->kind!="air") {
            const auto segment=phase->segment;
            for(const auto& p:tracker.phases())if(p.kind=="air" && p.segment==segment){phase=&p;break;}
        }
        if(c==Category::grinds && phase->kind!="grind") {
            const auto seen=row.value("first_seen_ms",now);const trick_logger::MovementPhase* candidate{};
            for(const auto& p:tracker.phases())if(p.kind=="grind") {
                if(p.begin<=seen)candidate=&p;else if(!candidate){candidate=&p;break;}
            }
            if(candidate)phase=candidate;
        }
        row["phase_provisional"]=c==Category::grinds && phase->kind!="grind";
        row["phase_id"]=phase->id;row["phase_kind"]=phase->kind;row["candidate_segment_id"]=phase->segment;
    }
    const auto& labels=trick_names::matching_labels(row);const auto id=labels.value("base_name","");
    if(category(id)==Category::grabs) {
        const auto* current=tracker.phase_at(now);
        const bool overlaps=row.value("phase_kind","")=="air" || (previous && !previous->value("grounded_only",true)) || (current && current->kind=="air");
        row["grounded_only"]=!overlaps;
        if(overlaps && row.value("phase_kind","")!="air" && current && current->kind=="air") {
            row["phase_id"]=current->id;row["phase_kind"]=current->kind;row["candidate_segment_id"]=current->segment;
        }
    }
    if(row.value("phase_kind","")=="air" && category(id)!=Category::grinds && category(id)!=Category::manuals && !recovery_row(row))
        tracker.stance(row.value("candidate_segment_id",std::size_t{}),trick_logger::starting_stance(labels.value("stance",""),labels.value("nollie",""),id),row.value("observation_id",std::uint64_t{}));
    return true;
}
struct Normalized {
    trick_logger::MatchSignature signature;
    std::string display;
    Category violation{Category::none};
    Json phases{Json::array()},ignored{Json::array()};
    trick_logger::CaptureSpan capture;
};
inline Normalized normalize(Json result,Rules rules) {
    Normalized out;
    if(!rules.verified){out.signature.error="Setup rules are not verified";return out;}
    if(!result.contains("ticker_candidates")){out.signature.error="Missing trick data";return out;}
    struct Group {std::uint64_t id{},segment{};std::string kind;std::vector<Json> rows;};
    std::vector<Json> ordered;
    for(const auto& row:result.at("ticker_candidates"))
        for(auto component:separate_body_flip(row))ordered.push_back(std::move(component));
    std::stable_sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) {
        return std::tuple{a.value("phase_id",a.value("candidate_segment_id",std::uint64_t{})),a.value("first_seen_ms",std::uint64_t{}),a.value("observation_id",std::uint64_t{})}<
               std::tuple{b.value("phase_id",b.value("candidate_segment_id",std::uint64_t{})),b.value("first_seen_ms",std::uint64_t{}),b.value("observation_id",std::uint64_t{})};
    });
    std::vector<trick_logger::MovementPhase> movement;
    if(result.contains("movement_phases"))for(const auto& phase:result.at("movement_phases"))
        movement.push_back({phase.value("id",std::uint64_t{}),phase.value("begin",std::uint64_t{}),phase.value("end",std::uint64_t{}),phase.value("segment",std::size_t{}),phase.value("kind","")});
    else for(const auto& row:ordered) {
        const auto id=row.value("phase_id",row.value("candidate_segment_id",std::uint64_t{1}));
        if(!movement.empty() && movement.back().id==id)continue;
        const auto c=category(row.contains("raw_labels")?trick_names::matching_labels(row).value("base_name",""):"");
        movement.push_back({id,0,0,row.value("candidate_segment_id",std::size_t{1}),row.value("phase_kind",c==Category::grinds?"grind":c==Category::ground?"ground":"air")});
    }
    for(const auto& row:ordered) {
        if(!row.contains("raw_labels") || !ignored_reason(row).empty())continue;
        const auto c=category(trick_names::matching_labels(row).value("base_name",""));
        if(c==Category::unknown || !rules.allows(c))continue;
        out.capture=trick_logger::capture_span(movement,row.value("phase_id",row.value("candidate_segment_id",std::uint64_t{1})),rules.allows(Category::grinds));break;
    }
    std::vector<Group> groups;
    for(const auto& row:ordered) {
        if(!row.contains("raw_labels")){out.signature.error="Unresolved trick identity";return out;}
        const auto id=trick_names::matching_labels(row).value("base_name","");
        auto why=ignored_reason(row);const auto c=category(id);
        if(why.empty() && c!=Category::unknown && !rules.allows(c))why="disabled category";
        const auto phase=row.value("phase_id",row.value("candidate_segment_id",std::uint64_t{1}));
        if(why.empty() && out.capture.first && !out.capture.contains(phase))why="outside retained combination";
        if(!why.empty()) {
            auto j=Json::object();j["observation_id"]=row.value("observation_id",std::uint64_t{});j["identity"]=id;j["reason"]=why;out.ignored.push_back(j);continue;
        }
        if(row.value("phase_provisional",false)){out.signature.error="Unconfirmed native movement phase";return out;}
        if(c==Category::unknown){out.signature.error="Unknown trick category: "+id;return out;}
        const std::string kind=c==Category::grinds?"grind":c==Category::ground?"ground":"air";
        if(groups.empty() || groups.back().id!=phase || groups.back().kind!=kind)
            groups.push_back({phase,row.value("candidate_segment_id",std::uint64_t{1}),kind,{}});
        groups.back().rows.push_back(row);
    }
    std::stable_sort(groups.begin(),groups.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    std::string preceding_trick,preceding_kind,preceding_direction;int preceding_spin{};
    for(auto& group:groups) {
        // A somersault describes the airborne phase, rather than the particular
        // grab/flip ticker row that happened to carry its final native modifier.
        std::stable_partition(group.rows.begin(),group.rows.end(),[](const auto& row){return !row.value("body_flip_component",false);});
        auto phase=Json::object();phase["kind"]=group.kind;auto components=Json::array();
        int degrees{};std::string direction,stance;
        if(group.kind=="air") {
            if(!result.contains("spins") || !group.segment || group.segment>result.at("spins").size()){out.signature.error="Missing phase rotation";return out;}
            const auto& spin=result.at("spins").at(group.segment-1);degrees=spin.value("accepted_spin",0);direction=spin.value("direction","");stance=spin.value("starting_stance","");
            if(!spin.value("rotation_complete",true)){out.signature.error="Incomplete retained phase rotation";return out;}
            // A delayed ticker may be the first reliable stance evidence for a
            // completed phase. Use only that phase's own riding trick labels.
            if(stance.empty() && !group.rows.empty()) {
                const auto& labels=trick_names::matching_labels(group.rows.front());
                stance=trick_logger::starting_stance(labels.value("stance",""),labels.value("nollie",""),labels.value("base_name",""));
                if(degrees && spin.contains("net_degrees") && spin.contains("goofy") && spin.at("goofy").is_boolean()) {
                    bool fs=spin.value("net_degrees",0.0)>0;if(spin.at("goofy").get<bool>())fs=!fs;if(stance=="switch")fs=!fs;direction=fs?"FS":"BS";
                }
            }
            phase["spin"]=degrees;phase["direction"]=direction;phase["stance"]=stance;
        }
        bool substantive{};
        for(const auto& row:group.rows)substantive|=!foundational_pop(trick_names::matching_labels(row).value("base_name",""));
        std::set<std::pair<std::uint64_t,std::string>> observations;std::set<std::string> pops;
        std::set<std::string> body_flips;
        std::string previous_identity;bool collapsed{};
        for(const auto& row:group.rows) {
            const auto& raw=trick_names::matching_labels(row);auto id=canonical_pop(raw.value("base_name",""));
            const bool pop=foundational_pop(id);
            if(pop && substantive && group.kind=="air")continue;
            auto identity=Json::object();identity["base"]=id=="ID_TRICK_NOLLIE"?"ID_TRICK_OLLIE":id;
            identity["stance"]=group.kind=="air"?stance:trick_logger::starting_stance(raw.value("stance",""),raw.value("nollie",""),id);
            identity["direction"]=pop?"":trick_names::plain_text(raw.value("direction",""));
            for(const auto key:{"held_rotation","late","body_flip_rotation","body_flip_direction","dark_catch","dark_flip","underflip"})
                identity[key]=trick_names::plain_text(raw.value(key,""));
            const auto token=identity.dump();
            if(row.value("body_flip_component",false) && !body_flips.insert(token).second)continue;
            if(!observations.emplace(row.value("observation_id",std::uint64_t{}),token).second ||
               (pop && !pops.insert(token).second))continue;
            if(token==previous_identity || (token==preceding_trick && group.kind==preceding_kind && degrees==preceding_spin && (!degrees || direction==preceding_direction))) {
                collapsed=true;auto duplicate=Json::object();duplicate["observation_id"]=row.value("observation_id",std::uint64_t{});duplicate["identity"]=id;
                duplicate["reason"]="consecutive equivalent trick";out.ignored.push_back(duplicate);continue;
            }
            previous_identity=token;
            preceding_trick=token;preceding_kind=group.kind;preceding_spin=degrees;preceding_direction=direction;
            std::string display;
            if(pop) {
                display=id=="ID_TRICK_NOLLIE" || stance=="nollie"?"Nollie":"Ollie";
                if(stance=="switch")display="Switch "+display;else if(stance=="fakie")display="Fakie "+display;
            }
            else if(row.contains("labels")) {
                const auto& labels=row.at("labels");
                for(const auto key:{"stance","nollie","direction","held_rotation","late","base_name","underflip","dark_catch","dark_flip","body_flip_direction","body_flip_rotation"}) {
                    const auto text=labels.value(key,"");if(!text.empty())display+=(display.empty()?"":" ")+text;
                }
            } else display=row.value("name",id.c_str());
            auto component=Json::object();component["identity"]=identity;component["display"]=display;components.push_back(component);
        }
        if(components.empty() && (!degrees || collapsed))continue;
        phase["components"]=components;out.phases.push_back(phase);
        if(!out.display.empty())out.display+=" | ";
        if(group.kind=="air" && degrees)out.display+=direction+" "+std::to_string(degrees)+" ";
        bool first=true;for(const auto& component:components){if(!first)out.display+=" + ";first=false;out.display+=component.at("display").string();}
    }
    result["canonical_phases"]=out.phases;out.signature=trick_logger::match_signature(result);return out;
}
} // namespace dingosdk::skate
