#pragma once
#include "anim_core.h"
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::anim_studio {
struct GameTrick {
    std::string category, label, trick, pop, file, asset;
    bool preview_only{}, recorded{};
    std::string stance{"regular"};
    std::optional<bool> switch_stance{false};
    AnimationSource source;
    std::string entry{"ground"};
    std::string position_codec;
    bool looping{};
};
inline bool safe_library_clip(const std::string& file) {
    return !file.empty() && file.find_first_of("/\\:") == std::string::npos &&
           file.find("..") == std::string::npos && std::filesystem::path(file).extension() == L".clip";
}
inline std::optional<std::pair<std::string, std::string>> recorded_clip_identity(const std::string& file) {
    if (!safe_library_clip(file)) return std::nullopt;
    const auto stem = std::filesystem::path(file).stem().string();
    const auto split = stem.rfind("__");
    if (split == std::string::npos || split == 0) return std::nullopt;
    auto pop = stem.substr(split + 2);
    if (pop != "low" && pop != "mid" && pop != "high") return std::nullopt;
    // Underscores belong to engine IDs (e.g. n_heelflip), not just display formatting.
    return std::pair{stem.substr(0, split), std::move(pop)};
}
inline std::vector<GameTrick> merge_game_library(std::vector<GameTrick> native, std::vector<GameTrick> recorded) {
    for (auto& take : recorded) {
        take.recorded = true;
        const auto found = std::find_if(native.begin(), native.end(), [&](const GameTrick& item) {
            return !item.preview_only && item.trick == take.trick && item.pop == take.pop && item.entry == take.entry;
        });
        if (found == native.end()) native.push_back(std::move(take));
        else { take.asset = found->asset; take.label = found->label; take.category = found->category; *found = std::move(take); }
    }
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
