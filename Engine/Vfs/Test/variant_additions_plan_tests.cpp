#include "Engine/Vfs/variant_additions_plan.h"
#include <cstdlib>
#include <iostream>

using namespace dingosdk::mods::detail;
namespace {
void check(bool ok, const char* why) {
    if (!ok) { std::cerr << why << '\n'; std::exit(1); }
}
VariantAsset asset(std::string name, std::string id, std::string hash = "original") {
    return {std::move(name), std::move(id), std::move(hash), {}};
}
}
int main() {
    const std::vector stock{asset("ebx:deck", "ebx:1"), asset("res:mesh", "res:2"), asset("chunk:3", "chunk:3")};
    auto selected = stock;
    selected[1].fingerprint = "variant replacement";
    auto donor = stock;
    donor.push_back(asset("ebx:private", "ebx:4"));
    donor.back().dependencies = {"res:5"};
    donor.push_back(asset("res:private", "res:5"));
    auto plan = plan_variant_additions(true, true, stock, selected, donor, {});
    check(plan && plan.additions == std::vector<std::size_t>{3, 4}, "only new assets are projected, never the stock mesh");
    check(!plan_variant_additions(false, true, stock, selected, donor, {}), "variant permission is mandatory");
    check(!plan_variant_additions(true, false, stock, selected, donor, {}), "adding author permission is mandatory");
    for (const auto& original : stock) {
        auto collision = donor;
        collision.push_back(asset("renamed", original.identity));
        check(!plan_variant_additions(true, true, stock, selected, collision, {}), "renaming cannot hide an EBX GUID, resource RID or chunk collision");
    }
    auto collision = donor;
    collision.push_back(asset("ebx:second", "ebx:4"));
    check(!plan_variant_additions(true, true, stock, selected, collision, {}), "two additions cannot share an identity");
    auto new_variant = selected;
    new_variant.push_back(asset("res:variant", "res:5"));
    check(!plan_variant_additions(true, true, stock, new_variant, donor, {}), "a new selected variant must be checked afresh");
    auto removed = selected;
    removed.erase(removed.begin() + 1);
    plan = plan_variant_additions(true, true, stock, removed, donor, {});
    check(plan && plan.additions == std::vector<std::size_t>{3, 4}, "removed stock asset stays removed despite donor scaffold");
    auto dependent = donor;
    dependent[3].dependencies.push_back("res:2");
    check(!plan_variant_additions(true, true, stock, removed, dependent, {}), "addition cannot depend on an original the variant removed");
    auto changed = donor;
    changed[0].fingerprint = "different type or metadata or payload";
    check(!plan_variant_additions(true, true, stock, selected, changed, {}), "changed original is not an addition");
    changed = donor;
    changed.erase(changed.begin());
    check(!plan_variant_additions(true, true, stock, selected, changed, {}), "dropped original is not an addition");
    const std::vector other{asset("res:other", "res:5")};
    check(!plan_variant_additions(true, true, stock, selected, donor, other), "another admitted pack's identity is reserved");
    const std::vector external{asset("res:external", "res:6")};
    dependent = donor;
    dependent[3].dependencies.push_back("res:6");
    check(plan_variant_additions(true, true, stock, selected, dependent, external).additions.size() == 2,
          "verified external dependency can satisfy an addition");
    std::cout << "variant additions planning tests passed\n";
}
