#include "layer_materials.h"
#include <algorithm>
#include <array>
#include <mutex>

namespace dingosdk::layer_materials {
namespace {
struct Kind {
    std::string_view folder;
    int type;
    const char* material;
};
// From the loader's create...Item functions (2026-10-21).
constexpr std::array<Kind, 20> kinds{{
    {"cust_tops", 3, "Top_mat"},
    {"cust_bottoms", 3, "Bottom_mat"},
    {"cust_socks", 3, "Sock_mat"},
    {"cust_shoes", 3, "Footwear_mat"},
    {"cust_hats", 3, "Headwear_mat"},
    {"cust_eyewear", 3, "Eyewear_mat"},
    {"cust_eyewear", 16, "EyewearTransparent_mat"},
    {"cust_handwear", 3, "Handwear_mat"},
    {"cust_earwear", 3, "Earwear_Left_mat"},
    {"cust_nosewear", 3, "Nosewear_Left_mat"},
    {"cust_outfit", 3, "Outfit_Top_mat"},
    {"cust_outfit", 16, "Outfit_Bottom_mat"},
    {"cust_fullbodycostume", 19, "FullbodyTop_mat"},
    {"cust_fullbodycostume", 20, "FullbodyBottom_mat"},
    {"cust_fullbodycostume", 21, "FullbodyFootwear_mat"},
    {"cust_fullbodycostume", 22, "FullbodyHeadwear_mat"},
    {"cust_fullbodycostume", 23, "FullbodyHandwear_mat"},
    {"cust_fullbodycostume", 24, "FullbodyAccessories_mat"},
    {"cust_fullbodycostume", 25, "FullbodyHardware_mat"},
    {"cust_fullbodycostume", 26, "FullbodyFX_mat"},
}};
std::string folded(std::string_view text) {
    std::string out(text);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
    return out;
}
// A preset's own name: what follows its last folder.
std::string_view leaf(std::string_view name) {
    const auto slash = name.find_last_of("/\\");
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}
struct Registry {
    std::mutex mutex;
    std::vector<std::pair<std::string, const char*>> presets; // folded leaf names
};
Registry& registry() {
    static auto* value = new Registry; // process lifetime: the loader's threads read it
    return *value;
}
} // namespace

const char* material_of(std::string_view folder, int type) noexcept {
    try {
        const auto name = folded(folder);
        for (const auto& kind : kinds)
            if (kind.folder == name && kind.type == type) return kind.material;
    } catch (...) {}
    return nullptr;
}

void set(std::vector<std::pair<std::string, const char*>> presets) {
    for (auto& [name, material] : presets) name = folded(leaf(name));
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    r.presets = std::move(presets);
}

const char* material_for(std::string_view preset) noexcept {
    try {
        auto& r = registry();
        std::lock_guard lock(r.mutex);
        if (r.presets.empty()) return nullptr;
        const auto name = folded(leaf(preset));
        for (const auto& [known, material] : r.presets)
            if (known == name) return material;
    } catch (...) {}
    return nullptr;
}
} // namespace dingosdk::layer_materials
