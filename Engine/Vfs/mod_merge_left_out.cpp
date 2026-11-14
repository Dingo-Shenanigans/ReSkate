#include "mod_merge_internal.h"
#include "mod_store_copies.h"
#include "Engine/Resource/ebx_document.h"

#include <algorithm>

// The items a player left out of a mod (Mod::items_off, chosen in the launcher's mod manager): which
// of the mod's added assets go with them. The merge then reads the mod's bundles without those
// (mod_merge_overrides.cpp leave_out, mod_merge_combine.cpp), as if the mod had been built that way.
namespace dingosdk::mods::detail {

namespace {

// Every string and resource id of a document's values, whatever field they are in.
struct Walk {
    AssetLinks& links;
    void value(const fb::ebx::Value& value, int depth) {
        if (depth > 48) return;   // deeper than any asset's data goes; the walk ends on any document
        if (const auto* text = std::get_if<std::string>(&value.data)) {
            // (A name is a path; nothing longer is one.)
            if (!text->empty() && text->size() <= 512) links.strings.push_back(ascii_lower(*text));
        } else if (const auto* resource = std::get_if<fb::ebx::ResourceReference>(&value.data)) {
            if (resource->id) links.resources.push_back(resource->id);
        } else if (const auto* object = std::get_if<std::shared_ptr<fb::ebx::Object>>(&value.data)) {
            if (*object) this->object(**object, depth);
        } else if (const auto* array = std::get_if<fb::ebx::Value::Array>(&value.data)) {
            for (const auto& entry : *array) this->value(entry, depth + 1);
        }
    }
    void object(const fb::ebx::Object& object, int depth) {
        for (const auto& field : object.fields) value(field.value, depth + 1);
    }
};

template <class Value> void once(std::vector<Value>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::string_view last_part(std::string_view name) {
    const auto slash = name.rfind('/');
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

} // namespace

AssetLinks asset_links(const fb::ebx::Document& document) {
    AssetLinks links;
    links.file = document.fileGuid;
    links.type = ascii_lower(document.rootType);
    for (const auto& reference : document.imports) links.imports.push_back(reference.fileGuid);
    Walk walk{links};
    for (const auto& instance : document.instances)
        if (instance.object) walk.object(*instance.object, 0);
    once(links.imports);
    once(links.strings);
    once(links.resources);
    if (const auto item = item_content(document)) links.item = ascii_lower(item->key);
    return links;
}

LeftOutPlan plan_left_out(std::span<const AssetLinks> own, std::span<const AssetLinks* const> others,
                          const std::set<std::string>& off) {
    LeftOutPlan plan;
    plan.gone.assign(own.size(), false);
    if (own.empty() || off.empty()) return plan;

    // One document in several of the mod's bundles is one thing to keep or leave out: a group.
    std::vector<std::size_t> groupOf(own.size());
    std::vector<std::vector<std::size_t>> members;
    std::map<fb::Guid, std::size_t> byGuid;
    for (std::size_t at = 0; at < own.size(); ++at) {
        const auto [place, fresh] = byGuid.try_emplace(own[at].file, members.size());
        if (fresh) members.emplace_back();
        groupOf[at] = place->second;
        members[place->second].push_back(at);
    }
    const auto groups = members.size();
    // By name and by the last part of the name, which is how an item names what it looks like.
    std::multimap<std::string_view, std::size_t> byName;
    for (std::size_t at = 0; at < own.size(); ++at) {
        if (own[at].name.empty()) continue;
        byName.emplace(own[at].name, groupOf[at]);
        if (const auto last = last_part(own[at].name); last.size() != own[at].name.size()) byName.emplace(last, groupOf[at]);
    }
    std::vector<bool> isItem(groups);
    for (std::size_t at = 0; at < own.size(); ++at)
        if (!own[at].item.empty()) isItem[groupOf[at]] = true;
    const auto named = [&](const AssetLinks& links) {
        std::vector<std::size_t> out;
        for (const auto& guid : links.imports)
            if (const auto found = byGuid.find(guid); found != byGuid.end()) out.push_back(found->second);
        for (const auto& text : links.strings) {
            // An item's key is what it is called, not something it names: packs have items whose
            // key is the last part of another item's asset name (own_adidas_sticker_3 in
            // items/.../own_adidas_sticker), and neither wears the other. It still finds what is
            // not an item, a thumbnail called after the key.
            const bool key = !links.item.empty() && text == links.item;
            const auto [first, last] = byName.equal_range(std::string_view(text));
            for (auto at = first; at != last; ++at)
                if (!key || !isItem[at->second]) out.push_back(at->second);
        }
        once(out);
        return out;
    };
    // What each group names among the mod's additions (itself left out).
    std::vector<std::vector<std::size_t>> names(groups);
    for (std::size_t group = 0; group < groups; ++group) {
        for (const auto at : members[group])
            for (const auto target : named(own[at]))
                if (target != group) names[group].push_back(target);
        once(names[group]);
    }
    // And who names each: another group, or a document from outside (counted, an item list apart).
    std::vector<std::vector<std::size_t>> namedBy(groups);
    for (std::size_t group = 0; group < groups; ++group)
        for (const auto target : names[group]) namedBy[target].push_back(group);
    std::vector<bool> outside(groups), listed(groups), resident(groups);
    for (const auto* other : others) {
        if (!other) continue;
        for (const auto target : named(*other)) (other->item_list ? listed : outside)[target] = true;
    }
    for (std::size_t at = 0; at < own.size(); ++at)
        if (own[at].resident) resident[groupOf[at]] = true;

    const auto spread = [&](std::vector<bool>& in, std::vector<std::size_t> open, const std::vector<bool>* within) {
        while (!open.empty()) {
            const auto group = open.back();
            open.pop_back();
            for (const auto target : names[group]) {
                if (in[target] || (within && !(*within)[target])) continue;
                in[target] = true;
                open.push_back(target);
            }
        }
    };

    // The items left out, and all they lead to.
    std::vector<bool> item(groups), reached(groups);
    std::vector<std::size_t> open;
    for (std::size_t at = 0; at < own.size(); ++at)
        if (!own[at].item.empty() && off.contains(own[at].item) && !item[groupOf[at]]) {
            item[groupOf[at]] = reached[groupOf[at]] = true;
            open.push_back(groupOf[at]);
        }
    if (open.empty()) return plan;
    spread(reached, open, nullptr);
    // A mesh variation database names its mesh and nothing names it (the game finds it by the mesh):
    // one that describes only what goes, goes too. Left in, it would name a mesh that is not there.
    std::vector<std::size_t> describing;
    for (bool grew = true; grew;) {
        grew = false;
        for (std::size_t group = 0; group < groups; ++group) {
            if (reached[group] || names[group].empty() || !namedBy[group].empty() || outside[group] || listed[group] ||
                resident[group]) continue;
            if (!std::ranges::all_of(members[group], [&](std::size_t at) { return own[at].type == "meshvariationdatabase"; })) continue;
            if (!std::ranges::all_of(names[group], [&](std::size_t target) { return reached[target]; })) continue;
            reached[group] = grew = true;
            describing.push_back(group);
        }
    }
    // What something that stays still names, stays, with all it names in turn.
    std::vector<bool> kept(groups);
    open.clear();
    for (std::size_t group = 0; group < groups; ++group) {
        if (!reached[group]) continue;
        // (An item list names every item; its entry for one left out is taken out with it.)
        bool wanted = outside[group] || resident[group] || (listed[group] && !item[group]);
        for (const auto from : namedBy[group])
            if (!reached[from]) wanted = true;
        if (wanted) {
            kept[group] = true;
            open.push_back(group);
        }
    }
    spread(kept, open, &reached);
    // And a variation database stays with any mesh of its own that does: a mesh without one shows nothing.
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto group : describing) {
            if (kept[group] || !std::ranges::any_of(names[group], [&](std::size_t target) { return kept[target]; })) continue;
            kept[group] = grew = true;
            spread(kept, {group}, &reached);
        }
    }

    for (std::size_t group = 0; group < groups; ++group) {
        if (!reached[group]) continue;
        if (!kept[group]) {
            for (const auto at : members[group]) plan.gone[at] = true;
            if (item[group]) ++plan.items;
        } else if (item[group]) {
            for (const auto at : members[group])
                if (!own[at].item.empty() && off.contains(own[at].item)) plan.stayed.push_back(own[at].item);
        }
    }
    once(plan.stayed);
    return plan;
}

} // namespace dingosdk::mods::detail
