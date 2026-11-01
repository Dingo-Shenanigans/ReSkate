// What a catalog category is worn as (Extension/Customization/outfit_kind.h): clothes by their slot,
// full-body costumes too, and the body's own parts left out.
#include "Extension/Customization/outfit_kind.h"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
using namespace dingosdk;

void check(bool ok, const std::string& message) {
    if (ok) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}
void worn_as(const char* category, const char* label) {
    const auto kind = outfit_kind::of(category);
    check(kind && kind->label == label, std::string(category) + " is worn as " + label);
}
void not_worn(const char* category) { check(!outfit_kind::of(category), std::string(category) + " is not worn"); }
} // namespace

int main() {
    // Clothes and accessories, by the slot they are worn in.
    worn_as("tops", "Top");
    worn_as("headgear", "Headwear");
    worn_as("eyewear", "Eyewear");
    worn_as("bottoms", "Bottoms");
    worn_as("shoes", "Shoes");
    // A full-body costume (a mod's costume is one, Own_... in cust_fullBodyCostume): worn, though its
    // category starts like the body's own parts and names the body.
    worn_as("cust_fullBodyCostume", "Costume");
    // The body's own parts, and what is not clothing.
    not_worn("custhead");
    not_worn("custbodies");
    not_worn("custnoses");
    not_worn("hair");
    not_worn("tattoos");
    not_worn("gestures");
    not_worn("");
    // A category this does not know: worn, its name made readable, after what it knows.
    const auto unknown = outfit_kind::of("patches_x");
    check(unknown && unknown->label == "Patches x" && unknown->order == 20, "an unknown category is its name made readable");
    // From head to foot.
    check(outfit_kind::of("headgear")->order < outfit_kind::of("cust_fullBodyCostume")->order &&
              outfit_kind::of("cust_fullBodyCostume")->order < outfit_kind::of("shoes")->order,
          "a costume sits between the head and the feet");
    std::cout << "outfit_kind tests passed\n";
}
