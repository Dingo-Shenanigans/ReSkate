#pragma once
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// The game's own animation pieces that are a trick a skater knows by name - a grind's way in, its loop and its pop-outs,
// a grab's way in, its hold and its way out, a manual's poses - so TRICKS can show them as trick cards beside the flip
// tricks, sorted into GRINDS, SLIDES, GRABS and so on. Read from the clips' names alone and for showing only, never as
// what the game plays: a name that does not read cleanly is left out (it stays in GAME ANIMATIONS), so nothing is
// guessed. Additive layers and setups are left out too: they are not a trick on their own.
namespace dingosdk::anim_studio {
struct NativeTrickPart {
    std::string kind;  // the list it is in: "Grinds", "Slides", "Grabs", ...
    std::string trick; // the card it is on: "Boardslide"
    std::string side;  // the way it is done: "FS", "BS", "FAKIE FS", "NOLLIE", a grab's name; "" when there is one way
    std::string part;  // which piece of it: "INTO", "LOOP", "OUT FS AIR", "HOLD MID", ...
    int kind_order{}, trick_order{}, part_order{};
};

namespace native_trick_detail {
struct Named {
    std::string_view token;
    const char* label;
    int order;
};
inline const Named* find(const auto& table, std::string_view token) {
    for (const auto& named : table)
        if (named.token == token) return &named;
    return nullptr;
}
inline std::string upper(std::string_view text) {
    std::string out(text);
    for (auto& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
inline std::string join(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    return std::string(a) + " " + std::string(b);
}
inline bool digits(std::string_view token) {
    if (token.empty()) return false;
    for (const char c : token)
        if (c < '0' || c > '9') return false;
    return true;
}
// The name's words; "5_0" (the 5-0 grind) stays one word.
inline std::vector<std::string_view> words(std::string_view name) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= name.size()) {
        auto end = name.find('_', start);
        if (end == std::string_view::npos) end = name.size();
        out.push_back(name.substr(start, end - start));
        start = end + 1;
    }
    for (std::size_t i = 0; i + 1 < out.size(); ++i)
        if (out[i] == "5" && out[i + 1] == "0") {
            out[i] = std::string_view(out[i].data(), 3);
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        }
    return out;
}
using Words = std::vector<std::string_view>;
inline bool has(const Words& w, std::string_view token) {
    for (const auto t : w)
        if (t == token) return true;
    return false;
}

enum Kind { grinds = 10, slides, grabs, fingerflips, old_school, inverts, manuals, powerslides, flip_variations };
inline const char* kind_name(int kind) {
    switch (kind) {
    case grinds: return "Grinds";
    case slides: return "Slides";
    case grabs: return "Grabs";
    case fingerflips: return "Fingerflips";
    case old_school: return "Old school";
    case inverts: return "Inverts";
    case manuals: return "Manuals";
    case powerslides: return "Powerslides & turns";
    default: return "Flip variations";
    }
}
inline NativeTrickPart part(int kind, std::string trick, int trick_order, std::string side, std::string piece, int piece_order) {
    return {kind_name(kind), std::move(trick), std::move(side), std::move(piece), kind, trick_order, piece_order};
}

// ---- grinds and slides ----
// A clip is often two tricks at once: the same body on the rail is a FS boardslide one way and a BS lipslide the other
// ("fs_boardslide_bs_lipslide"), so it goes on both cards.
inline constexpr std::array<Named, 17> rail_tricks{{{"5050", "50-50", 0},          {"5_0", "5-0", 1},
                                                    {"nosegrind", "Nosegrind", 2}, {"crook", "Crooked grind", 3},
                                                    {"overcrook", "Overcrook", 4}, {"smith", "Smith grind", 5},
                                                    {"feeble", "Feeble grind", 6}, {"willy", "Willy grind", 7},
                                                    {"overwilly", "Overwilly", 8}, {"salad", "Salad grind", 9},
                                                    {"oversalad", "Oversalad", 10}, {"boardslide", "Boardslide", 100},
                                                    {"lipslide", "Lipslide", 101}, {"noseslide", "Noseslide", 102},
                                                    {"tailslide", "Tailslide", 103}, {"blunt", "Bluntslide", 104},
                                                    {"noseblunt", "Nosebluntslide", 105}}};
inline std::vector<NativeTrickPart> rail(const Words& w, std::size_t from, std::size_t to, bool fakie, const std::string& piece, int order) {
    std::vector<NativeTrickPart> out;
    std::vector<std::string_view> sides;
    for (std::size_t i = from; i < to; ++i) {
        if (w[i] == "fs" || w[i] == "bs") {
            sides.push_back(w[i]);
            continue;
        }
        const auto* trick = find(rail_tricks, w[i]);
        if (!trick) return {};
        if (sides.empty()) sides.push_back({});
        for (const auto side : sides)
            out.push_back(part(trick->order >= 100 ? slides : grinds, trick->label, trick->order % 100, join(fakie ? "FAKIE" : "", upper(side)), piece, order));
        sides.clear();
    }
    if (!sides.empty()) return {};
    return out;
}
inline std::vector<NativeTrickPart> grind(const Words& w) {
    // grind_[bf_|fakie_]<tricks>_rail_<into | loop | popout_fs|bs [inair]> [dark]: "bf" is the fakie way in and loop
    // (the board the other way round), "fakie" its pop-outs.
    std::size_t i = 1;
    const bool fakie = w.size() > 1 && (w[1] == "bf" || w[1] == "fakie");
    if (fakie) ++i;
    std::size_t at_rail = i;
    while (at_rail < w.size() && w[at_rail] != "rail") ++at_rail;
    if (at_rail >= w.size()) return {};
    bool dark = false, air = false;
    std::string piece;
    int order = 0;
    for (std::size_t j = at_rail + 1; j < w.size(); ++j) {
        if (w[j] == "dark") dark = true;
        else if (w[j] == "inair") air = true;
        else if (w[j] == "into" && piece.empty()) piece = "INTO", order = 0;
        else if (w[j] == "loop" && piece.empty()) piece = "LOOP", order = 1;
        else if (w[j] == "popout" && piece.empty() && j + 1 < w.size() && (w[j + 1] == "fs" || w[j + 1] == "bs")) {
            piece = "OUT " + upper(w[j + 1]);
            order = w[j + 1] == "fs" ? 2 : 3;
            ++j;
        } else return {};
    }
    if (piece.empty()) return {};
    if (air) piece += " AIR", order += 2;
    if (dark) piece = "DARK " + piece, order += 10;
    return rail(w, i, at_rail, fakie, piece, order);
}
inline std::vector<NativeTrickPart> grind_tweak(const Words& w) {
    // adjust_<left|right|nose|tail>_<tricks>_<tucked|extended>_pose[_dark]: leaning into the grind.
    if (w.size() < 5) return {};
    std::size_t end = 2;
    while (end < w.size() && w[end] != "tucked" && w[end] != "extended") ++end;
    if (end + 1 >= w.size() || w[end + 1] != "pose") return {};
    const bool dark = end + 2 < w.size() && w[end + 2] == "dark";
    if (end + 2 + (dark ? 1 : 0) != w.size()) return {};
    std::string piece = w[end] == "tucked" ? "TUCKED" : "EXTENDED";
    if (dark) piece = "DARK " + piece;
    return rail(w, 2, end, false, piece, (w[end] == "tucked" ? 20 : 21) + (dark ? 10 : 0));
}
inline std::vector<NativeTrickPart> slappy(const Words& w) {
    // slappy_[fakie_]<fs|bs>_<trick>_into: onto the rail without popping.
    std::size_t i = 1;
    const bool fakie = w.size() > 1 && w[1] == "fakie";
    if (fakie) ++i;
    if (w.size() < i + 3 || w.back() != "into") return {};
    return rail(w, i, w.size() - 1, fakie, "SLAPPY IN", 40);
}

// ---- grabs and fingerflips ----
inline constexpr std::array<Named, 10> grab_names{{{"fs", "FS grab", 0},
                                                   {"bs", "BS grab", 1},
                                                   {"nose", "Nose grab", 2},
                                                   {"tail", "Tail grab", 3},
                                                   {"stale", "Stalefish", 4},
                                                   {"weddle", "Weddle", 5},
                                                   {"crail", "Crail grab", 6},
                                                   {"seatbelt", "Seatbelt", 7},
                                                   {"double", "Double grab", 8},
                                                   {"coffin", "Coffin", 9}}};
inline constexpr std::array<Named, 8> grab_tweaks{{{"melon", "Melon", 20},
                                                   {"method", "Method", 21},
                                                   {"japan", "Japan", 22},
                                                   {"crossbone", "Crossbone", 23},
                                                   {"stiffy", "Stiffy", 24},
                                                   {"tailbone", "Tailbone", 25},
                                                   {"nosebone", "Nosebone", 26},
                                                   {"tuckknee", "Tuck knee", 27}}};
inline std::string grab_label(std::string_view token) {
    if (token == "dbl") token = "double";
    if (token == "grab") return {};
    const auto* g = find(grab_names, token);
    return g ? g->label : std::string{};
}
inline std::vector<NativeTrickPart> fingerflip(const Words& w, std::size_t from, const std::string& base, bool fakie) {
    // <style> fingerflip [varial] <into | intro | exit | to inair | to [grab] <grab>>, or <grab> to <grab> fingerflip.
    std::size_t at = from;
    while (at < w.size() && w[at] != "fingerflip") ++at;
    if (at >= w.size()) return {};
    std::string style;
    int order = 0;
    std::string piece;
    int piece_order = 0;
    const Words before(w.begin() + static_cast<std::ptrdiff_t>(from), w.begin() + static_cast<std::ptrdiff_t>(at));
    if (before.empty()) style = "Fingerflip";
    else if (before.size() == 1 && before[0] == "heel") style = "Heel fingerflip", order = 1;
    else if (before.size() == 1 && before[0] == "kick") style = "Kick fingerflip", order = 2;
    else if (before.size() == 2 && before[1] == "shuv" && (before[0] == "bs" || before[0] == "fs"))
        style = upper(before[0]) + " shuv fingerflip", order = before[0] == "bs" ? 4 : 5;
    else if (before.size() == 2 && before[0] == "to" && !grab_label(before[1]).empty()) {
        style = "Fingerflip", piece = "TO " + upper(grab_label(before[1])), piece_order = 4;
        if (at + 1 != w.size()) return {};
    } else return {};
    std::size_t j = at + 1;
    if (j < w.size() && w[j] == "varial") style = "Varial fingerflip", order = 3, ++j;
    if (piece.empty()) {
        if (j + 1 == w.size() && (w[j] == "into" || w[j] == "intro")) piece = "INTO", piece_order = 0;
        else if (j + 1 == w.size() && w[j] == "exit") piece = "EXIT", piece_order = 1;
        else if (j + 2 == w.size() && w[j] == "to" && w[j + 1] == "inair") piece = "TO AIR", piece_order = 2;
        else if (j < w.size() && w[j] == "to") {
            const auto target = grab_label(j + 2 < w.size() ? w[j + 2] : w[j + 1]);
            if (target.empty() || j + (j + 2 < w.size() ? 3 : 2) != w.size()) return {};
            piece = "TO " + upper(target), piece_order = 3;
        } else return {};
    }
    return {part(fingerflips, style, order, join(fakie ? "FAKIE" : "", upper(base)), piece, piece_order)};
}
inline std::vector<NativeTrickPart> in_air(const Words& w) {
    // inair_[fakie_]<fs|bs>_grab_..., inair_[fakie_]double_grab_..., inair_[fakie_]grab_<grab>_..., inair_coffin_...
    std::size_t i = 1;
    const bool fakie = w.size() > i && w[i] == "fakie";
    if (fakie) ++i;
    std::string_view base;
    if (i + 1 < w.size() && (w[i] == "fs" || w[i] == "bs" || w[i] == "double") && w[i + 1] == "grab") base = w[i], i += 2;
    else if (i + 1 < w.size() && w[i] == "grab" && find(grab_names, w[i + 1])) base = w[i + 1], i += 2;
    else if (i < w.size() && w[i] == "coffin") base = w[i], i += 1;
    else return {};
    const auto* grab = find(grab_names, base);
    if (has(w, "fingerflip")) return fingerflip(w, i, grab->label, fakie);
    const Words rest(w.begin() + static_cast<std::ptrdiff_t>(i), w.end());
    const auto side = fakie ? std::string("FAKIE") : std::string{};
    const int kind = base == "coffin" ? old_school : grabs;
    const int order = base == "coffin" ? 40 : grab->order;
    if (rest.size() == 1 && (rest[0] == "into" || rest[0] == "exit" || rest[0] == "loop"))
        return {part(kind, grab->label, order, side, rest[0] == "into" ? "INTO" : rest[0] == "loop" ? "HOLD" : "EXIT",
                     rest[0] == "into" ? 0 : rest[0] == "loop" ? 1 : 2)};
    if (rest.size() == 2 && rest[0] == "loop" && digits(rest[1])) return {part(kind, grab->label, order, side, "HOLD 2", 3)};
    // into_<grab>: from this grab to another
    if (rest.size() == 2 && rest[0] == "into" && !grab_label(rest[1]).empty())
        return {part(kind, grab->label, order, side, "TO " + upper(grab_label(rest[1])), 10)};
    // exit_to_<grab>_grab: out of a coffin into a grab
    if (rest.size() == 4 && rest[0] == "exit" && rest[1] == "to" && rest[3] == "grab" && !grab_label(rest[2]).empty())
        return {part(kind, grab->label, order, side, "OUT TO " + upper(grab_label(rest[2])), 11)};
    if (rest.empty() || rest.back() != "loop") return {};
    const bool mid = rest.size() >= 2 && rest[rest.size() - 2] == "mid";
    const Words tweak(rest.begin(), rest.end() - (mid ? 2 : 1));
    if (tweak.empty()) return {};
    const std::string hold = mid ? "HOLD MID" : "HOLD";
    // The double grab's ways of holding it.
    if (base == "double" && tweak.size() == 1 && (tweak[0] == "front" || tweak[0] == "back" || tweak[0] == "left" || tweak[0] == "right"))
        return {part(kind, grab->label, order, side, upper(tweak[0]) + (mid ? " MID" : ""), 20 + (mid ? 4 : 0))};
    // A shifty on a grab: its own card ("Crail shifty"), done FS or BS.
    if (tweak.size() == 2 && tweak[1] == "shifty" && (tweak[0] == "fs" || tweak[0] == "bs"))
        return {part(grabs, std::string(grab->label) + " shifty", 30 + grab->order, join(side, upper(tweak[0])), hold, mid ? 1 : 0)};
    // Named tweaks (melon, method, tuck knee...): their own card, done on the grab named as its side.
    std::string label;
    int tweak_order = 0;
    int number = 0;
    for (const auto t : tweak) {
        if (digits(t)) {
            number = std::max(number, 2);
            continue;
        }
        const auto* named = find(grab_tweaks, t);
        if (!named) {
            label.clear();
            break;
        }
        if (label.empty()) label = named->label, tweak_order = named->order;
        else {
            std::string lower = named->label;
            lower[0] = static_cast<char>(lower[0] - 'A' + 'a');
            label += " " + lower;
            tweak_order = 28; // a pair of tweaks after the single ones
        }
    }
    if (!label.empty())
        return {part(grabs, label, tweak_order, join(side, upper(grab->label)), hold + (number ? " 2" : ""), (mid ? 1 : 0) + (number ? 2 : 0))};
    // Anything else held on the grab stays on the grab's card, by its own words.
    std::string words_label;
    for (const auto t : tweak) {
        if (find(grab_names, t) || t == "nose" || t == "tail") words_label = join(words_label, upper(t));
        else return {};
    }
    return {part(kind, grab->label, order, side, words_label + " " + hold, 30 + (mid ? 1 : 0))};
}

// ---- old school: the on-board grabs and plants, no complies, caspers, primos ----
inline constexpr std::array<Named, 14> old_school_tricks{{{"judo", "Judo air", 0},
                                                          {"christ", "Christ air", 1},
                                                          {"super", "Superman", 2},
                                                          {"nofoot", "No footer", 3},
                                                          {"onefoot", "One footer", 4},
                                                          {"frigid", "Frigid air", 5},
                                                          {"dogpiss", "Dog piss", 6},
                                                          {"airwalk", "Airwalk", 7},
                                                          {"benihanna", "Benihana", 8},
                                                          {"tailwalk", "Tailwalk", 9},
                                                          {"bs_footplant", "BS footplant", 20},
                                                          {"fs_footplant", "FS footplant", 21},
                                                          {"fastplant_footplant", "Fastplant", 22},
                                                          {"judo_footplant", "Judo footplant", 23}}};
inline std::vector<NativeTrickPart> board_grab(const Words& w) {
    std::size_t i = 1;
    const bool fakie = w.size() > i && w[i] == "fakie";
    if (fakie) ++i;
    const auto fakie_word = fakie ? std::string("FAKIE") : std::string{};
    // Ground grabs: grab_[fakie_]ground_<fs|bs|dbl>_<into|loop|exit|land|to_<fs|bs|dbl>>[_turnleft|_turnright]
    if (i < w.size() && w[i] == "ground") {
        if (i + 2 >= w.size()) return {};
        const auto hand = w[i + 1] == "dbl" ? std::string("DOUBLE") : upper(w[i + 1]);
        if (w[i + 1] != "fs" && w[i + 1] != "bs" && w[i + 1] != "dbl") return {};
        std::size_t j = i + 2;
        std::string piece;
        int order = 0;
        if (w[j] == "into" || w[j] == "loop" || w[j] == "exit" || w[j] == "land") {
            piece = w[j] == "into" ? "INTO" : w[j] == "loop" ? "HOLD" : w[j] == "exit" ? "EXIT" : "LAND";
            order = w[j] == "into" ? 0 : w[j] == "loop" ? 1 : w[j] == "exit" ? 2 : 3;
            ++j;
        } else if (w[j] == "to" && j + 1 < w.size() && (w[j + 1] == "fs" || w[j + 1] == "bs" || w[j + 1] == "dbl")) {
            piece = "TO " + (w[j + 1] == "dbl" ? std::string("DOUBLE") : upper(w[j + 1]));
            order = 4;
            j += 2;
        } else return {};
        if (j < w.size() && (w[j] == "turnleft" || w[j] == "turnright")) piece += w[j] == "turnleft" ? " TURN L" : " TURN R", order += w[j] == "turnleft" ? 10 : 20, ++j;
        if (j != w.size()) return {};
        return {part(grabs, "Ground grab", 40, join(fakie_word, hand), piece, order)};
    }
    // grab_rocket_<into|loop|exit|to_nose_exit>
    if (i < w.size() && w[i] == "rocket") {
        const Words rest(w.begin() + static_cast<std::ptrdiff_t>(i) + 1, w.end());
        if (rest.size() == 1 && (rest[0] == "into" || rest[0] == "loop" || rest[0] == "exit"))
            return {part(old_school, "Rocket air", 10, fakie_word, rest[0] == "into" ? "INTO" : rest[0] == "loop" ? "HOLD" : "EXIT",
                         rest[0] == "into" ? 0 : rest[0] == "loop" ? 1 : 2)};
        if (rest.size() == 3 && rest[0] == "to" && rest[1] == "nose" && rest[2] == "exit") return {part(old_school, "Rocket air", 10, fakie_word, "TO NOSE GRAB", 5)};
        return {};
    }
    // grab_[fakie_]<hand>_<1ft|2ft>_<trick>_<into|loop|exit|full|to_<trick>_exit>
    if (i + 2 >= w.size()) return {};
    const auto hand = w[i];
    if (hand != "bs" && hand != "fs" && hand != "nose" && hand != "tail" && hand != "double") return {};
    if (w[i + 1] != "1ft" && w[i + 1] != "2ft") return {};
    std::size_t j = i + 2;
    std::string name;
    while (j < w.size() && w[j] != "into" && w[j] != "loop" && w[j] != "exit" && w[j] != "full" && w[j] != "to") {
        name += (name.empty() ? "" : "_") + std::string(w[j]);
        ++j;
    }
    const auto* trick = find(old_school_tricks, name);
    if (!trick || j >= w.size()) return {};
    std::string piece;
    int order = 0;
    if (j + 1 == w.size()) {
        piece = w[j] == "into" ? "INTO" : w[j] == "loop" ? "HOLD" : w[j] == "exit" ? "EXIT" : w[j] == "full" ? "WHOLE" : "";
        order = w[j] == "into" ? 0 : w[j] == "loop" ? 1 : w[j] == "exit" ? 2 : 3;
    } else if (w[j] == "to" && j + 2 < w.size() && w.back() == "exit") {
        std::string target;
        for (std::size_t t = j + 1; t + 1 < w.size(); ++t) target += (target.empty() ? "" : "_") + std::string(w[t]);
        const auto* to = find(old_school_tricks, target);
        if (!to) return {};
        piece = "TO " + upper(to->label);
        order = 5;
    }
    if (piece.empty()) return {};
    return {part(old_school, trick->label, trick->order, join(fakie_word, hand == "double" ? std::string("DOUBLE") : upper(hand)), piece, order)};
}
inline std::vector<NativeTrickPart> coffin_to_ground(const Words& w) {
    // coffin_to_ground_grab_<fs|bs|dbl>
    if (w.size() != 5 || w[1] != "to" || w[2] != "ground" || w[3] != "grab") return {};
    if (w[4] != "fs" && w[4] != "bs" && w[4] != "dbl") return {};
    return {part(old_school, "Coffin", 40, "", "TO GROUND GRAB " + (w[4] == "dbl" ? std::string("DOUBLE") : upper(w[4])), 12)};
}
inline std::vector<NativeTrickPart> boneless(const Words& w) {
    // boneless_[fakie_]<fs|bs|fsl|bsl>_<rolling | still | [still_]to_[grab_<hand>_<1ft|2ft>_]<trick>>
    std::size_t i = 1;
    const bool fakie = w.size() > i && w[i] == "fakie";
    if (fakie) ++i;
    if (w.size() < i + 2 || (w[i] != "fs" && w[i] != "bs" && w[i] != "fsl" && w[i] != "bsl")) return {};
    const auto side = join(fakie ? "FAKIE" : "", upper(w[i]));
    std::size_t j = i + 1;
    if (w.size() == j + 1 && (w[j] == "rolling" || w[j] == "still")) return {part(old_school, "Boneless", 30, side, upper(w[j]), w[j] == "rolling" ? 0 : 1)};
    const bool still = w[j] == "still";
    if (still) ++j;
    if (j >= w.size() || w[j] != "to") return {};
    ++j;
    if (j + 2 < w.size() && w[j] == "grab") j += 3; // grab_<hand>_<feet>_: the trick's own name follows
    std::string name;
    for (; j < w.size(); ++j) name += (name.empty() ? "" : "_") + std::string(w[j]);
    std::string target;
    if (name == "footplant") target = "FOOTPLANT";
    else if (const auto* trick = find(old_school_tricks, name)) target = upper(trick->label);
    else return {};
    return {part(old_school, "Boneless", 30, side, std::string(still ? "STILL " : "") + "TO " + target, still ? 3 : 2)};
}
inline std::vector<NativeTrickPart> wallie(const Words& w) {
    // wallie_regular_<fs|bs|straight>_high
    if (w.size() != 4 || w[1] != "regular" || (w[2] != "fs" && w[2] != "bs" && w[2] != "straight") || w[3] != "high") return {};
    return {part(old_school, "Wallie", 35, upper(w[2]), "HIGH", 0)};
}
inline std::vector<NativeTrickPart> hippie_setup(const Words& w) {
    // setup_hippy_[fakie_]<into|loop|pose>
    std::size_t i = 2;
    const bool fakie = w.size() > i && w[i] == "fakie";
    if (fakie) ++i;
    if (w.size() != i + 1 || (w[i] != "into" && w[i] != "loop" && w[i] != "pose")) return {};
    return {part(old_school, "Hippie jump", 34, fakie ? "FAKIE" : "", w[i] == "into" ? "CROUCH INTO" : w[i] == "loop" ? "CROUCH HOLD" : "CROUCH POSE",
                 w[i] == "into" ? 10 : w[i] == "loop" ? 11 : 12)};
}
// ---- inverts: handplants and the plants done on a lip ----
inline constexpr std::array<Named, 12> invert_tricks{{{"handplant", "Handplant", 0},
                                                      {"gymnast", "Gymnast plant", 1},
                                                      {"onefootplant", "One foot handplant", 2},
                                                      {"judoplant", "Judo plant", 3},
                                                      {"methodhandplant", "Method handplant", 4},
                                                      {"crossbonehandplant", "Crossbone handplant", 5},
                                                      {"tuckkneeplant", "Tuck knee plant", 6},
                                                      {"tweakplant", "Tweak plant", 7},
                                                      {"bonedplant", "Boned plant", 8},
                                                      {"frigidplant", "Frigid plant", 9},
                                                      {"stiffyhand", "Stiffy handplant", 10},
                                                      {"tailboneplant", "Tailbone plant", 11}}};
inline std::vector<NativeTrickPart> invert(const Words& w) {
    // invert_[fakie_]<fs|bs>_<plant>[_antic]
    std::size_t i = 1;
    const bool fakie = w.size() > i && w[i] == "fakie";
    if (fakie) ++i;
    if (w.size() < i + 2 || (w[i] != "fs" && w[i] != "bs")) return {};
    const auto* trick = find(invert_tricks, w[i + 1]);
    if (!trick) return {};
    const bool antic = w.size() == i + 3 && w[i + 2] == "antic";
    if (w.size() != i + 2 + (antic ? 1 : 0)) return {};
    return {part(inverts, trick->label, trick->order, join(fakie ? "FAKIE" : "", upper(w[i])), antic ? "ANTIC" : "WHOLE", antic ? 1 : 0)};
}

// ---- flip tricks done another way: dark and light catches, underflips, held and late flips ----
struct FlipName {
    std::array<std::string_view, 2> tokens;
    const char* label;
    int order;
};
inline constexpr std::array<FlipName, 21> flip_names{{{{"kickflip", {}}, "Kickflip", 0},
                                                      {{"heelflip", {}}, "Heelflip", 1},
                                                      {{"hardflip", {}}, "Hardflip", 2},
                                                      {{"inwardheel", {}}, "Inward heelflip", 3},
                                                      {{"varialflip", {}}, "Varial flip", 4},
                                                      {{"varialheel", {}}, "Varial heelflip", 5},
                                                      {{"popshuv", "bs"}, "BS pop shove-it", 6},
                                                      {{"popshuv", "fs"}, "FS pop shove-it", 7},
                                                      {{"360flip", {}}, "360 flip", 8},
                                                      {{"laserflip", {}}, "Laser flip", 9},
                                                      {{"360hardflip", {}}, "360 hardflip", 10},
                                                      {{"360inwardheel", {}}, "360 inward heelflip", 11},
                                                      {{"360popshuv", "bs"}, "BS 360 shove-it", 12},
                                                      {{"360popshuv", "fs"}, "FS 360 shove-it", 13},
                                                      {{"impossible", {}}, "Impossible", 14},
                                                      {{"frontfoot", "impossible"}, "Front foot impossible", 15},
                                                      {{"backfoot", "impossible"}, "Back foot impossible", 16},
                                                      {{"hospital", "flip"}, "Hospital flip", 17},
                                                      {{"hospital", "heelflip"}, "Hospital heelflip", 18},
                                                      {{"ollie", {}}, "Ollie", 19},
                                                      {{"nollie", {}}, "Nollie", 20}}};
inline std::vector<NativeTrickPart> flip_variation(const Words& w) {
    // trick_<o|n>_<flip>[_<low|mid|high>]_<style>
    if (w.size() < 4 || (w[1] != "o" && w[1] != "n")) return {};
    const FlipName* flip = nullptr;
    std::size_t j = 2;
    for (const auto& f : flip_names) {
        const std::size_t n = f.tokens[1].empty() ? 1 : 2;
        if (w.size() >= 2 + n && w[2] == f.tokens[0] && (n == 1 || w[3] == f.tokens[1])) {
            flip = &f;
            j = 2 + n;
            break;
        }
    }
    if (!flip) return {};
    int pop = -1;
    std::string style;
    int style_order = 0;
    const auto take_pop = [&](std::size_t at) {
        if (at < w.size() && (w[at] == "low" || w[at] == "mid" || w[at] == "high")) {
            pop = w[at] == "low" ? 0 : w[at] == "mid" ? 1 : 2;
            return true;
        }
        return false;
    };
    if (take_pop(j)) ++j;
    const Words rest(w.begin() + static_cast<std::ptrdiff_t>(j), w.end());
    const auto is = [&](std::initializer_list<std::string_view> words) {
        if (words.size() != rest.size()) return false;
        std::size_t k = 0;
        for (const auto word : words)
            if (rest[k++] != word) return false;
        return true;
    };
    if (rest.empty() && pop >= 0) style = "POP", style_order = 0;
    else if (is({"dark"})) style = "DARK CATCH", style_order = 1;
    else if (is({"light"})) style = "LIGHT CATCH", style_order = 2;
    else if (is({"uflip"})) style = "UNDERFLIP", style_order = 3;
    else if (is({"held", "into"})) style = "HELD INTO", style_order = 4;
    else if (is({"held", "loop"})) style = "HELD LOOP", style_order = 5;
    else if (is({"held", "exit"})) style = "HELD EXIT", style_order = 6;
    else if (is({"lateflip", "start"}) || is({"lateflip"})) style = "LATE FLIP", style_order = 7;
    else if (is({"lateflip", "exit"})) style = "LATE FLIP EXIT", style_order = 8;
    else if (is({"lateflip", "dark", "catch"})) style = "LATE DARK CATCH", style_order = 9;
    else if (is({"lateflip", "light", "catch"})) style = "LATE LIGHT CATCH", style_order = 10;
    else if (is({"quickpop"})) style = "QUICK POP", style_order = 11;
    else if (is({"nosebone", "loop"})) style = "NOSEBONE", style_order = 12;
    else if (is({"nosebone", "fakie", "loop"})) style = "NOSEBONE FAKIE", style_order = 13;
    else return {};
    static constexpr std::array<const char*, 3> pops{"LOW", "MID", "HIGH"};
    const std::string piece = style == "POP" ? std::string(pops[static_cast<std::size_t>(pop)]) : join(style, pop >= 0 ? pops[static_cast<std::size_t>(pop)] : "");
    return {part(flip_variations, flip->label, flip->order, w[1] == "n" ? "NOLLIE" : "", piece, style_order * 4 + pop + 1)};
}
inline std::vector<NativeTrickPart> other_trick(const Words& w) {
    if (w.size() < 2) return {};
    // trick_nc_[f_]nocomply_<fs|bs|straight>[_old]
    if (w[1] == "nc") {
        std::size_t i = 2;
        const bool fakie = w.size() > i && w[i] == "f";
        if (fakie) ++i;
        if (w.size() < i + 2 || w[i] != "nocomply") return {};
        const auto way = w[i + 1];
        if (way != "fs" && way != "bs" && way != "straight") return {};
        const bool old = w.size() == i + 3 && w[i + 2] == "old";
        if (w.size() != i + 2 + (old ? 1 : 0)) return {};
        return {part(old_school, "No comply", 31, join(fakie ? "FAKIE" : "", upper(way)), old ? "OLD STYLE" : "TRICK", old ? 1 : 0)};
    }
    // trick_casper_<into|exit[_flip1|2]|loop_low|med|high>
    if (w[1] == "casper") {
        const Words rest(w.begin() + 2, w.end());
        std::string piece;
        int order = 0;
        if (rest.size() == 1 && rest[0] == "into") piece = "INTO";
        else if (rest.size() == 1 && rest[0] == "exit") piece = "EXIT", order = 5;
        else if (rest.size() == 2 && rest[0] == "exit" && (rest[1] == "flip1" || rest[1] == "flip2"))
            piece = rest[1] == "flip1" ? "EXIT FLIP" : "EXIT FLIP 2", order = rest[1] == "flip1" ? 6 : 7;
        else if (rest.size() == 2 && rest[0] == "loop" && (rest[1] == "low" || rest[1] == "med" || rest[1] == "high"))
            piece = rest[1] == "low" ? "HOLD LOW" : rest[1] == "med" ? "HOLD MID" : "HOLD HIGH", order = rest[1] == "low" ? 1 : rest[1] == "med" ? 2 : 3;
        else return {};
        return {part(old_school, "Casper", 32, "", piece, order)};
    }
    // trick_primo[2]_<into|loop|idle|exit_fs_shuvit>
    if (w[1] == "primo" || w[1] == "primo2") {
        const Words rest(w.begin() + 2, w.end());
        std::string piece;
        int order = 0;
        if (rest.size() == 1 && (rest[0] == "into" || rest[0] == "loop" || rest[0] == "idle"))
            piece = rest[0] == "into" ? "INTO" : "HOLD", order = rest[0] == "into" ? 0 : 1;
        else if (rest.size() == 3 && rest[0] == "exit" && rest[1] == "fs" && rest[2] == "shuvit") piece = "OUT FS SHOVE-IT", order = 2;
        else return {};
        return {part(old_school, "Primo", 33, w[1] == "primo2" ? "STYLE 2" : "", piece, order)};
    }
    // trick_h_hippy_<low|mid|high>
    if (w[1] == "h" && w.size() == 4 && w[2] == "hippy" && (w[3] == "low" || w[3] == "mid" || w[3] == "high"))
        return {part(old_school, "Hippie jump", 34, "", upper(w[3]), w[3] == "low" ? 0 : w[3] == "mid" ? 1 : 2)};
    // trick_spin_[nollie_]<fs|bs>: the body turning with the board
    if (w[1] == "spin") {
        std::size_t i = 2;
        const bool nollie = w.size() > i && w[i] == "nollie";
        if (nollie) ++i;
        if (w.size() != i + 1 || (w[i] != "fs" && w[i] != "bs")) return {};
        return {part(powerslides, "Spin", 3, join(nollie ? "NOLLIE" : "", upper(w[i])), "POP", 0)};
    }
    if (w[1] == "o" || w[1] == "n") return flip_variation(w);
    return {};
}

// ---- manuals, powerslides and reverts ----
inline std::vector<NativeTrickPart> manual(const Words& w) {
    // manual_<t|n>_[fakie_]<into | loop_med | exit | low|med|high_pose | low|med|high_turn_fs|bs | brake_rolling_slow |
    // revert_[fakie_]fs|bs>
    if (w.size() < 3 || (w[1] != "t" && w[1] != "n")) return {};
    const char* trick = w[1] == "n" ? "Nose manual" : "Manual";
    const int trick_order = w[1] == "n" ? 1 : 0;
    std::size_t i = 2;
    bool fakie = w[i] == "fakie";
    if (fakie) ++i;
    const Words rest(w.begin() + static_cast<std::ptrdiff_t>(i), w.end());
    const auto height = [](std::string_view h) { return h == "low" ? std::string("LOW") : h == "med" ? std::string("MID") : h == "high" ? std::string("HIGH") : std::string{}; };
    const auto height_order = [](std::string_view h) { return h == "low" ? 0 : h == "med" ? 1 : 2; };
    std::string piece;
    int order = 0;
    if (rest.size() == 1 && rest[0] == "into") piece = "INTO";
    else if (rest.size() == 2 && rest[0] == "loop" && rest[1] == "med") piece = "LOOP", order = 1;
    else if (rest.size() == 1 && rest[0] == "exit") piece = "EXIT", order = 2;
    else if (rest.size() == 2 && rest[1] == "pose" && !height(rest[0]).empty()) piece = "POSE " + height(rest[0]), order = 3 + height_order(rest[0]);
    else if (rest.size() == 3 && rest[1] == "turn" && !height(rest[0]).empty() && (rest[2] == "fs" || rest[2] == "bs"))
        piece = "TURN " + upper(rest[2]) + " " + height(rest[0]), order = 10 + (rest[2] == "bs" ? 3 : 0) + height_order(rest[0]);
    else if (rest.size() == 3 && rest[0] == "brake" && rest[1] == "rolling" && rest[2] == "slow") piece = "BRAKE", order = 20;
    else if (rest.size() >= 2 && rest[0] == "revert") {
        std::size_t k = 1;
        if (rest[k] == "fakie") fakie = true, ++k;
        if (k + 1 != rest.size() || (rest[k] != "fs" && rest[k] != "bs")) return {};
        piece = "REVERT " + upper(rest[k]), order = rest[k] == "fs" ? 21 : 22;
    } else return {};
    return {part(manuals, trick, trick_order, fakie ? "FAKIE" : "", piece, order)};
}
inline std::vector<NativeTrickPart> powerslide(const Words& w) {
    // powerslide_[fakie_into_]<fs|bs>_<fast|slow>_<into|loop|exit_<0|90|fakie|early_reg>>
    std::string side, speed, piece, detail;
    bool fakie = false;
    int order = 0;
    for (std::size_t j = 1; j < w.size(); ++j) {
        const auto t = w[j];
        if (t == "fs" || t == "bs") side = upper(t);
        else if (t == "fast" || t == "slow") speed = upper(t);
        else if (piece.empty() && t == "fakie") fakie = true;
        else if (piece.empty() && (t == "into" || t == "loop" || t == "exit")) piece = upper(t), order = t == "into" ? 0 : t == "loop" ? 1 : 2;
        else if (piece == "EXIT" && (t == "0" || t == "90")) detail = join(detail, t), order += t == "90" ? 1 : 0;
        else if (piece == "EXIT" && t == "fakie") detail = join(detail, "TO FAKIE"), order += 2;
        else if (piece == "EXIT" && t == "early") detail = join(detail, "EARLY"), order += 3;
        else if (piece == "EXIT" && (t == "reg" || digits(t))) continue;
        else return {};
    }
    if (side.empty() || speed.empty() || piece.empty()) return {};
    return {part(powerslides, "Powerslide", 0, join(fakie ? "FAKIE" : "", side), join(join(speed, piece), detail), (speed == "SLOW" ? 10 : 0) + order)};
}
inline std::vector<NativeTrickPart> revert(const Words& w) {
    // revert_<normal|fakie>_<fs|bs>_<fast|slow>, land_<reg|fakie>_revert_[45_][fakie_]<fs|bs>_<light|medium|heavy>
    if (w[0] == "revert") {
        if (w.size() != 4 || (w[1] != "normal" && w[1] != "fakie") || (w[2] != "fs" && w[2] != "bs") || (w[3] != "fast" && w[3] != "slow")) return {};
        return {part(powerslides, "Revert", 1, join(w[1] == "fakie" ? "FAKIE" : "", upper(w[2])), upper(w[3]), w[3] == "fast" ? 0 : 1)};
    }
    if (w.size() < 5 || (w[1] != "reg" && w[1] != "fakie") || w[2] != "revert") return {};
    std::size_t j = 3;
    const bool angle = w[j] == "45";
    if (angle) ++j;
    const bool to_fakie = j < w.size() && w[j] == "fakie";
    if (to_fakie) ++j;
    if (j + 2 != w.size() || (w[j] != "fs" && w[j] != "bs")) return {};
    const auto weight = w[j + 1];
    if (weight != "light" && weight != "medium" && weight != "heavy") return {};
    const std::string piece = std::string("LAND") + (angle ? " 45" : "") + (to_fakie ? " TO FAKIE" : "") + " " +
                              (weight == "light" ? "LIGHT" : weight == "medium" ? "MID" : "HEAVY");
    return {part(powerslides, "Revert", 1, join(w[1] == "fakie" ? "FAKIE" : "", upper(w[j])), piece,
                 10 + (angle ? 3 : 0) + (to_fakie ? 6 : 0) + (weight == "light" ? 0 : weight == "medium" ? 1 : 2))};
}
} // namespace native_trick_detail

// Which trick cards a clip of the game's (animation/dingo/...) is a part of: none when its name does not read as one.
inline std::vector<NativeTrickPart> native_trick_parts(std::string_view asset) {
    using namespace native_trick_detail;
    const auto slash = asset.rfind('/');
    std::string_view name = slash == std::string_view::npos ? asset : asset.substr(slash + 1);
    // On the board only: c_proto_onb_..., and the in-air grabs some of which are c_proto_inair_...
    if (name.starts_with("c_proto_onb_")) name.remove_prefix(12);
    else if (name.starts_with("c_proto_inair_")) name.remove_prefix(8);
    else return {};
    const auto w = words(name);
    if (w.empty() || has(w, "addi") || has(w, "override") || has(w, "test")) return {};
    const auto first = w[0];
    if (first == "grind") return grind(w);
    if (first == "adjust") return grind_tweak(w);
    if (first == "slappy") return slappy(w);
    if (first == "inair") return in_air(w);
    if (first == "grab") return board_grab(w);
    if (first == "coffin") return coffin_to_ground(w);
    if (first == "boneless") return boneless(w);
    if (first == "trick") return other_trick(w);
    if (first == "manual") return manual(w);
    if (first == "powerslide") return powerslide(w);
    if (first == "revert" || (first == "land" && has(w, "revert"))) return revert(w);
    if (first == "invert") return invert(w);
    if (first == "wallie") return wallie(w);
    if (first == "setup" && w.size() > 1 && w[1] == "hippy") return hippie_setup(w);
    if (first == "kickturn") {
        // kickturn_<fs|bs>_<in|loop|out>
        if (w.size() != 3 || (w[1] != "fs" && w[1] != "bs") || (w[2] != "in" && w[2] != "loop" && w[2] != "out")) return {};
        return {part(powerslides, "Kickturn", 2, upper(w[1]), w[2] == "in" ? "INTO" : w[2] == "loop" ? "LOOP" : "EXIT", w[2] == "in" ? 0 : w[2] == "loop" ? 1 : 2)};
    }
    if (first == "spin") {
        // spin_regular_land_neutral_<fs|bs>_<180|360>_<light|medium|heavy>: landing a body spin
        if (w.size() != 7 || w[1] != "regular" || w[2] != "land" || w[3] != "neutral" || (w[4] != "fs" && w[4] != "bs") ||
            (w[5] != "180" && w[5] != "360") || (w[6] != "light" && w[6] != "medium" && w[6] != "heavy"))
            return {};
        return {part(powerslides, "Spin", 3, upper(w[4]), "LAND " + std::string(w[5]) + " " + (w[6] == "light" ? "LIGHT" : w[6] == "medium" ? "MID" : "HEAVY"),
                     (w[5] == "180" ? 10 : 20) + (w[6] == "light" ? 0 : w[6] == "medium" ? 1 : 2))};
    }
    return {};
}
} // namespace dingosdk::anim_studio
