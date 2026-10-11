// Texture Quality tier mapping for issue #160: choice keys to the engine controls the tier sets.
#include "Extension/Rendering/texture_tier.h"
#include <iostream>

namespace tt = dingosdk::texture_tier;
namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAILED: " << what << '\n'; ++failures; }
}
}

int main() {
    static_assert(tt::from_choice_key("texture_quality_ultra") == tt::ultra);
    check(tt::from_choice_key("texture_quality_low") == tt::low, "low key");
    check(tt::from_choice_key("texture_quality_medium") == tt::medium, "medium key");
    check(tt::from_choice_key("texture_quality_high") == tt::high, "high key");
    check(tt::from_choice_key("texture_quality_custom") == tt::unknown, "custom is not recognised");
    check(tt::from_choice_key("texture_filtering_ultra") == tt::unknown, "other setting's key");
    check(tt::from_choice_key("") == tt::unknown, "empty key");
    check(tt::settings_for(tt::unknown) == nullptr, "unknown tier has no settings");

    // The values that were wrong in #160: Ultra must not leave mips skipped or the compositor Low.
    const auto* ultra = tt::settings_for(tt::ultra);
    check(ultra && (*ultra)[0].name == "Texture.SkipMipmapCount" && (*ultra)[0].value == "0", "ultra mips");
    check(ultra && (*ultra)[2].value == "3", "ultra compositor is 3, not the High value 2");
    const auto* low = tt::settings_for(tt::low);
    check(low && (*low)[0].value == "1" && (*low)[2].value == "0", "low keeps skipped mips");
    const auto* medium = tt::settings_for(tt::medium);
    check(medium && (*medium)[0].value == "1", "medium keeps skipped mips");
    const auto* high = tt::settings_for(tt::high);
    check(high && (*high)[0].value == "0" && (*high)[2].value == "2", "high");

    if (failures) return 1;
    std::cout << "texture_tier tests passed\n";
}
