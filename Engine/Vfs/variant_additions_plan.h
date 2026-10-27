#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::mods::detail {

// Native readers supply canonical, kind-qualified names and GUID/RID identities.
// The fingerprint includes payload hash, type, metadata and logical sizes. This
// planner neither reads archives nor grants permission on an author's behalf.
struct VariantAsset {
    std::string name, identity, fingerprint;
    std::vector<std::string> dependencies;
};
struct VariantAdditionPlan {
    std::string refusal;
    std::vector<std::size_t> additions;
    explicit operator bool() const { return refusal.empty(); }
};

// Only private additions are returned. In particular, originals absent from the
// selected variant are never resurrected from the donor's authoring scaffold.
// `available` is the final dependency inventory outside this bundle, not every
// asset that happened to exist before a variant removed it.
inline VariantAdditionPlan plan_variant_additions(
    bool variant_accepts, bool donor_accepts,
    std::span<const VariantAsset> stock, std::span<const VariantAsset> selected,
    std::span<const VariantAsset> donor, std::span<const VariantAsset> available) {
    const auto refuse = [](std::string why) { return VariantAdditionPlan{std::move(why), {}}; };
    if (!variant_accepts || !donor_accepts) return refuse("both authors must permit private additions");
    using Index = std::map<std::string, const VariantAsset*, std::less<>>;
    Index stock_names, stock_ids, selected_names, selected_ids, donor_names, donor_ids, outside_names, outside_ids;
    const auto index = [](std::span<const VariantAsset> assets, Index& names, Index& ids) {
        for (const auto& asset : assets) {
            if (asset.name.empty() || asset.identity.empty() || asset.fingerprint.empty() ||
                !names.emplace(asset.name, &asset).second || !ids.emplace(asset.identity, &asset).second) return false;
        }
        return true;
    };
    if (!index(stock, stock_names, stock_ids) || !index(selected, selected_names, selected_ids) ||
        !index(donor, donor_names, donor_ids) || !index(available, outside_names, outside_ids))
        return refuse("missing or repeated native asset name or identity");
    for (const auto& asset : stock) {
        const auto copy = donor_names.find(asset.name);
        if (copy == donor_names.end() || copy->second->identity != asset.identity ||
            copy->second->fingerprint != asset.fingerprint)
            return refuse("adding pack changes or drops an original asset: " + asset.name);
    }
    VariantAdditionPlan result;
    for (std::size_t at = 0; at < donor.size(); ++at) {
        const auto& asset = donor[at];
        if (stock_names.contains(asset.name)) continue;
        if (stock_ids.contains(asset.identity) || selected_names.contains(asset.name) ||
            selected_ids.contains(asset.identity) || outside_names.contains(asset.name) || outside_ids.contains(asset.identity))
            return refuse("private addition collides with a retained asset: " + asset.name);
        result.additions.push_back(at);
    }
    Index final_ids = outside_ids;
    for (const auto& [id, asset] : selected_ids) final_ids.insert_or_assign(id, asset);
    for (const auto at : result.additions) final_ids.emplace(donor[at].identity, &donor[at]);
    for (const auto at : result.additions)
        for (const auto& id : donor[at].dependencies)
            if (!final_ids.contains(id)) return refuse("private addition has an unavailable dependency: " + donor[at].name);
    return result;
}

} // namespace dingosdk::mods::detail
