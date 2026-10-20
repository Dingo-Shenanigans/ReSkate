#pragma once
#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::anim_studio {
// Display/search only. Never use these inferred words as native asset identities or
// gameplay bindings. Unknown tokens stay visible rather than acquiring guessed meaning.
inline std::string native_search_key(std::string_view text) {
    std::string out;
    for (unsigned char c : text) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += static_cast<char>(c);
    }
    return out;
}
inline std::vector<std::string> native_search_terms(std::string_view query) {
    std::vector<std::string> out;
    std::string term;
    for (unsigned char c : query) {
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!term.empty()) { out.push_back(native_search_key(term)); term.clear(); }
        } else term += static_cast<char>(c);
    }
    if (!term.empty()) out.push_back(native_search_key(term));
    std::erase(out, std::string{});
    return out;
}
inline bool native_search_matches(std::string_view key, const std::vector<std::string>& terms) {
    return std::all_of(terms.begin(), terms.end(), [&](const auto& term) { return key.find(term) != std::string_view::npos; });
}
struct NativeAssetName { std::string label, context, search; };
inline NativeAssetName native_asset_name(std::string_view asset, std::string_view root_type = {}, std::string_view category = {}) {
    NativeAssetName result;
    const auto append = [](std::string& into, std::string_view word, std::string_view separator = " ") {
        if (word.empty()) return;
        if (!into.empty()) into += separator;
        into += word;
    };
    if (asset.starts_with("_af/")) {
        result.label = "Channel Bindings / " + std::string(asset.substr(4, 18));
        result.context = "Rig data";
    } else {
        auto name = asset.substr(asset.rfind('/') == std::string_view::npos ? 0 : asset.rfind('/') + 1);
        std::vector<std::string> tokens;
        for (std::size_t start = 0; start < name.size();) {
            auto end = name.find('_', start);
            if (end == std::string_view::npos) end = name.size();
            if (end > start) tokens.emplace_back(name.substr(start, end - start));
            start = end + 1;
        }
        if (!tokens.empty() && (tokens.front() == "c" || tokens.front() == "f")) {
            if (tokens.front() == "f") append(result.context, "Face", " / ");
            tokens.erase(tokens.begin());
        }
        while (!tokens.empty()) {
            const auto& t = tokens.front();
            std::string context;
            if (t == "proto") context = "Prototype";
            else if (t == "onb") context = "On board";
            else if (t == "ofb" || t == "ofbnb") context = "Off board";
            else if (t == "skatepedia") context = "Skatepedia";
            else if (t == "demo") context = "Demo";
            else break;
            append(result.context, context, " / "); tokens.erase(tokens.begin());
        }
        struct Word { std::string_view token, label, aliases; };
        static constexpr Word words[] = {
            {"360flip", "360 Flip", "treflip tre flip"}, {"360hardflip", "360 Hardflip", ""},
            {"360inwardheel", "360 Inward Heelflip", ""}, {"inwardheel", "Inward Heelflip", ""},
            {"varialflip", "Varial Kickflip", "varial flip"}, {"varialheel", "Varial Heelflip", ""},
            {"laserflip", "Laser Flip", ""}, {"popshuv", "Pop Shuv-it", "pop shove it shuvit"},
            {"360popshuv", "360 Pop Shuv-it", "360 shove it shuvit"},
            {"5050", "50-50", "5050 fifty fifty"}, {"50", "50", ""}, {"bf", "BF", ""},
            {"fs", "FS", "frontside front side"}, {"bs", "BS", "backside back side"},
            {"into", "Entry", "into enter entree"}, {"enter", "Entry", "into enter entree"},
            {"exit", "Exit", "sortie"}, {"popout", "Pop Out", "exit sortie"},
            {"lockin", "Lock-in", "entry"}, {"loop", "Loop", "boucle"},
            {"low", "Low", "bas"}, {"mid", "Medium", "mid moyen"}, {"med", "Medium", "mid moyen"},
            {"high", "High", "haut"}, {"lfoot", "Left Foot", "lfoot"}, {"rfoot", "Right Foot", "rfoot"},
            {"frontfoot", "Front Foot", ""}, {"backfoot", "Back Foot", ""},
            {"inair", "In Air", "airborne"}, {"preland", "Before Landing", "preland"},
            {"dbl", "Double", "dbl"}, {"fwd", "Forward", "fwd"}, {"bwd", "Backward", "bwd"},
            {"reg", "Regular", "reg"}, {"fem", "Female", "fem"}, {"masc", "Male", "masc"},
            {"turnleft", "Turn Left", ""}, {"turnright", "Turn Right", ""},
            {"drasticturn", "Sharp Turn", "drastic turn"}, {"1step", "1 Step", ""}, {"2step", "2 Steps", ""},
            {"thankyou", "Thank You", ""}, {"makeitrain", "Make It Rain", ""},
            {"rubshoulder", "Rub Shoulder", ""}, {"adjustshirt", "Adjust Shirt", ""},
            {"birdwings", "Bird Wings", ""}, {"awardpoints", "Award Points", ""},
            {"bswallride", "BS Wallride", "backside wallride"}, {"fswallride", "FS Wallride", "frontside wallride"},
            {"bsgroundgrab", "BS Ground Grab", "backside ground grab"}, {"fsgroundgrab", "FS Ground Grab", "frontside ground grab"},
            {"inairtuck", "In Air Tuck", ""}, {"quickollie", "Quick Ollie", ""},
            {"deadhang", "Dead Hang", ""}, {"cathang", "Cat Hang", ""}, {"wallhang", "Wall Hang", ""},
            {"darkcatch", "Dark Catch", ""}, {"lateflip", "Late Flip", ""}, {"addi", "Additive", "addi"},
            {"lhand", "Left Hand", "lhand"}, {"rhand", "Right Hand", "rhand"},
            {"lefthand", "Left Hand", ""}, {"righthand", "Right Hand", ""},
            {"bellyslide", "Belly Slide", ""}, {"wallrun", "Wall Run", ""},
            {"usedcardealershipmascot", "Used Car Dealership Mascot", ""},
            {"mocapsuit", "Motion Capture Suit", "mocap suit"}, {"dance", "Dance", "danse"},
            {"seawave", "Sea Wave", ""}, {"selfhighfive", "Self High Five", ""},
            {"fsonefootplant", "FS One Foot Plant", "frontside one foot plant"},
            {"bsonefootplant", "BS One Foot Plant", "backside one foot plant"},
            {"onefootplant", "One Foot Plant", ""}, {"nocomply", "No Comply", ""},
            {"pipewipeout", "Pipe Wipeout", ""}, {"fakesteez", "Fake Steez", ""},
            {"animbase", "Base Animation", ""}
        };
        bool trick = false;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];
            if (token == "5" && i + 1 < tokens.size() && tokens[i + 1] == "0") {
                append(result.label, "5-0"); append(result.search, "five o five oh"); ++i; continue;
            }
            if (token == "trick") { trick = true; continue; }
            // Only these two pop-family codes are confirmed. Do not infer switch or
            // goofy from a single unknown character or from left/right foot names.
            if (trick && i > 0 && tokens[i - 1] == "trick" && (token == "o" || token == "n")) {
                if (token == "n" && (i + 1 == tokens.size() || tokens[i + 1] != "nollie")) append(result.label, "Nollie");
                continue;
            }
            const auto found = std::find_if(std::begin(words), std::end(words), [&](const auto& w) { return w.token == token; });
            if (found != std::end(words)) {
                append(result.label, found->label); append(result.search, found->aliases);
            } else {
                if (token.size() > 1 && (token.back() == 'l' || token.back() == 'r') &&
                    std::all_of(token.begin(), token.end() - 1, [](char c) { return c >= '0' && c <= '9'; })) {
                    append(result.label, token.substr(0, token.size() - 1) + (token.back() == 'l' ? " Left" : " Right"));
                    continue;
                }
                // Keep unrecognized abbreviations/variant numbers, split CamelCase.
                std::string word;
                for (std::size_t c = 0; c < token.size(); ++c) {
                    const auto ch = token[c];
                    if (c && ch >= 'A' && ch <= 'Z' && token[c - 1] >= 'a' && token[c - 1] <= 'z') word += ' ';
                    word += ch;
                }
                if (!word.empty() && word[0] >= 'a' && word[0] <= 'z') word[0] -= 'a' - 'A';
                append(result.label, word);
            }
        }
        if (result.label.empty()) result.label = std::string(name);
    }
    append(result.search, result.label); append(result.search, result.context);
    append(result.search, asset); append(result.search, root_type); append(result.search, category);
    result.search = native_search_key(result.search);
    return result;
}

inline std::string native_asset_folder(std::string_view asset, std::string_view type, std::string_view fallback = {}) {
    const auto name = asset.substr(asset.rfind('/') == std::string_view::npos ? 0 : asset.rfind('/') + 1);
    std::vector<std::string_view> tokens;
    for (std::size_t start = 0; start < name.size();) {
        auto end = name.find('_', start);
        if (end == std::string_view::npos) end = name.size();
        tokens.push_back(name.substr(start, end - start)); start = end + 1;
    }
    const auto has = [&](std::string_view token) { return std::find(tokens.begin(), tokens.end(), token) != tokens.end(); };
    if (asset.starts_with("_af/")) return "Rig/Channel bindings";
    if (type != "ClipControllerAsset") {
        if (type.find("Skeleton") != std::string_view::npos || type.find("RigAsset") != std::string_view::npos ||
            type.find("DofSet") != std::string_view::npos || type.find("Pose") != std::string_view::npos || type.find("JointMap") != std::string_view::npos)
            return "Rig/Skeletons and poses";
        if (type.find("IK") != std::string_view::npos || type.find("Bone") != std::string_view::npos || type.find("RigOp") != std::string_view::npos)
            return "Rig/Constraints and IK";
        if (type.find("Transition") != std::string_view::npos) return "Game logic/Transitions";
        if (type.find("Sequence") != std::string_view::npos) return "Game logic/Sequences";
        if (type.find("Context") != std::string_view::npos || type.starts_with("PTM")) return "Game logic/Selection and contexts";
        if (type.find("Physics") != std::string_view::npos) return "Game logic/Physics";
        if (type.find("Controller") != std::string_view::npos || type.find("Blend") != std::string_view::npos) return "Game logic/Controllers and blends";
        if (type.find("Bool") != std::string_view::npos || type.find("Float") != std::string_view::npos || type.find("Vector") != std::string_view::npos ||
            type.find("Integer") != std::string_view::npos || type.find("Enum") != std::string_view::npos || type.find("Expression") != std::string_view::npos)
            return "Game logic/Parameters and expressions";
        return "Game logic/Other data";
    }
    if (has("f") && !has("trick")) return "Face/Animations";
    const auto phase = [&]() -> std::string {
        if (has("skatepedia")) return "References";
        if (has("popout") || has("exit") || has("stop")) return "Exits";
        if (has("into") || has("enter") || has("lockin") || has("start")) return "Entries";
        if (has("loop")) return "Loops";
        if (has("to") || has("transition")) return "Transitions";
        if (has("pose") || has("static")) return "Poses";
        return "Other movements";
    };
    if (has("grind")) {
        std::string family = name.find("_5_0_") != std::string_view::npos || name.ends_with("_5_0") ? "5-0" : "";
        for (auto token : tokens) {
            if (token == "5050" || token == "50" || token == "nosegrind" || token == "boardslide" || token == "lipslide" ||
                token == "noseslide" || token == "tailslide" || token == "noseblunt" || token == "blunt" || token == "feeble" ||
                token == "smith" || token == "salad" || token == "crook" || token == "overcrook" || token == "willy" ||
                token == "overwilly" || token == "oversalad") {
                if (!family.empty()) family += " + ";
                auto word = token == "5050" ? std::string("50-50") : std::string(token);
                if (word[0] >= 'a' && word[0] <= 'z') word[0] -= 'a' - 'A';
                family += word;
            }
        }
        return "Grinds/" + (family.empty() ? "General" : family) + '/' + phase();
    }
    if (has("manual")) return "Manuals/" + phase();
    if (has("grab") || has("bsgroundgrab") || has("fsgroundgrab")) return "Grabs/" + phase();
    if (has("bail") || has("wipeout") || has("pipewipeout") || has("recovery") || has("getup")) return "Falls and recovery/" + phase();
    if (has("gesture") || has("dance")) return std::string("Gestures/") + (has("dance") ? "Dances/" : "Emotes/") + phase();
    if (has("trick")) {
        std::string family;
        auto it = std::find(tokens.begin(), tokens.end(), "trick");
        ++it;
        if (it != tokens.end() && (*it == "o" || *it == "n")) ++it;
        if (it != tokens.end()) {
            if (*it == "nc") family = "No Comply";
            else if (*it == "h" && has("hippy")) family = "Hippy";
            else if ((*it == "frontfoot" || *it == "backfoot") && has("impossible")) family = "Impossible";
            else family = native_asset_name(*it).label;
        }
        return "Tricks/" + (family.empty() ? "Other tricks" : family) + (has("skatepedia") ? "/References" : "/Variants");
    }
    if (has("setup")) return "Tricks/Preparation/" + phase();
    if (has("land") || has("preland")) return "Tricks/Landings/" + phase();
    if (has("inair") || has("inairtuck")) return "Tricks/In air/" + phase();
    if (has("slappy") || has("wallride") || has("bswallride") || has("fswallride") || has("boneless") || has("powerslide") || has("primo"))
        return "Tricks/Special movements/" + phase();
    if (has("climb") || has("climbing") || has("mantle") || has("vault")) return "Off board/Climbing and vaulting/" + phase();
    if (has("mount") || has("dismount") || has("attach")) return "Board mounts and dismounts/" + phase();
    const bool off = has("ofb") || has("ofbnb");
    if (has("idle") || has("adjust") || has("override")) return std::string(off ? "Off board/" : "On board/") + "Idle and small movements";
    for (auto movement : {"walk", "run", "sprint", "jump", "push", "brake", "rolling", "slide", "roll"}) {
        if (has(movement)) return std::string(off ? "Off board/" : "On board/") + native_asset_name(movement).label + '/' + phase();
    }
    if (has("skatepedia")) return "References/Skatepedia";
    if (has("f") || fallback.starts_with("Face/")) return "Face/Animations";
    return std::string(off ? "Off board/" : "On board/") + "Other movements";
}

// Equal readable names still represent distinct native assets. Add known context,
// then stable variant numbers only where needed, instead of hiding/merging sources.
template<class Asset> void native_disambiguate_names(std::vector<Asset>& assets) {
    std::map<std::pair<std::string, std::string>, std::vector<Asset*>> groups;
    for (auto& asset : assets) groups[{asset.category, asset.label}].push_back(&asset);
    for (auto& [_, group] : groups) {
        if (group.size() < 2) continue;
        std::sort(group.begin(), group.end(), [](const auto* a, const auto* b) { return a->asset < b->asset; });
        std::map<std::string, std::size_t> totals, index;
        for (const auto* asset : group) ++totals[asset->context];
        for (auto* asset : group) {
            if (!asset->context.empty()) asset->label += " / " + asset->context;
            if (totals[asset->context] > 1) asset->label += " / Variant " + std::to_string(++index[asset->context]);
            asset->search += native_search_key(asset->label);
        }
    }
}
} // namespace dingosdk::anim_studio
