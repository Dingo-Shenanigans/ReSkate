#pragma once
#include "anim_core.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace dingosdk::anim_studio {
struct GameTrick {
    std::string category, label, trick, pop, file, asset;
    bool preview_only{};
    std::string entry{"ground"};
    std::string position_codec;
    bool looping{};
};
inline bool safe_library_clip(const std::string& file) {
    return !file.empty() && file.find_first_of("/\\:") == std::string::npos &&
           file.find("..") == std::string::npos && std::filesystem::path(file).extension() == L".clip";
}
// The game's tricks in the order TRICKS lists them: by category, ollie and nollie first, then by name, high pop first.
inline std::vector<GameTrick> sort_game_library(std::vector<GameTrick> native) {
    const auto pop_rank = [](const std::string& pop) { return pop == "high" ? 0 : pop == "mid" ? 1 : pop == "low" ? 2 : 3; };
    std::sort(native.begin(), native.end(), [&](const GameTrick& a, const GameTrick& b) {
        if (a.category != b.category) return a.category < b.category;
        const bool first_a = a.trick == "ollie" || a.trick == "nollie", first_b = b.trick == "ollie" || b.trick == "nollie";
        if (first_a != first_b) return first_a;
        if (a.trick != b.trick) return a.trick < b.trick;
        return pop_rank(a.pop) < pop_rank(b.pop);
    });
    return native;
}
} // namespace dingosdk::anim_studio
