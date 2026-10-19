#pragma once
#include "trick_metadata.h"
#include "trick_name_text.h"
#include <map>
#include <vector>
#include <algorithm>

namespace dingosdk::trick_names {
// The ticker's base label is already localized in live play. Resolve it using
// the game's own localization of native scoring identifiers, never an English
// name/category guess. Multiple identifiers for one label remain ambiguous.
class NativeTrickCatalog {
    std::map<std::string,std::vector<std::string>,std::less<>> labels_;
    static bool riding_identity(std::string_view id) {
        for(const auto& entry:skate::metadata)if(entry.id==id)
            return (entry.categories & (0x7eu | 0xfe000u))!=0;
        return false;
    }
public:
    void add(std::string_view id,std::string_view display) {
        const auto text=plain_text(display);
        if(text.empty() || text==id || text.find("ID_")!=std::string::npos)return;
        auto& ids=labels_[text];
        if(std::find(ids.begin(),ids.end(),id)==ids.end())ids.emplace_back(id);
    }
    std::size_t unique_labels() const {
        std::size_t count{};for(const auto& [label,ids]:labels_)if(ids.size()==1)++count;return count;
    }
    std::size_t ambiguous_labels() const {return labels_.size()-unique_labels();}
    std::string resolve(std::string_view observed,bool on_board=false) const {
        const auto clean=plain_text(observed);
        for(const auto& entry:skate::metadata)if(entry.id==clean)return clean;
        const auto it=labels_.find(clean);if(it==labels_.end())return {};
        std::string selected;
        for(const auto& id:it->second) {
            if(on_board && !riding_identity(id))continue;
            if(!selected.empty())return {}; // Never guess between two riding identities.
            selected=id;
        }
        return selected;
    }
    Json annotate(Json row,bool on_board=false) const {
        if(!row.contains("raw_labels"))return row;
        auto labels=row.at("raw_labels");
        const auto id=resolve(labels.value("base_name",""),on_board);
        labels["base_name"]=id;
        row["identity_labels"]=std::move(labels);
        row["identity_resolved"]=!id.empty();
        row["identity_source"]=on_board?"native_localization_catalog_onboard":"native_localization_catalog";
        return row;
    }
};
inline const Json& matching_labels(const Json& row) {
    return row.contains("identity_labels")?row.at("identity_labels"):row.at("raw_labels");
}
}
