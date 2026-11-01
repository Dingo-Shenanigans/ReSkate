#pragma once
#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

// The kind of item a catalog category is, for what the skater wears (worn_items.cpp, reskate.outfit.worn):
// clothes and accessories by the slot they are worn in, from head to foot; nothing for the body's own
// parts, hair, tattoos, gestures or the board. Pure, so it is tested (Test/outfit_kind_tests.cpp).
namespace dingosdk::outfit_kind {
struct Kind {
    std::string label; // "Headwear", "Top", ... "Shoes"; a category this does not know, made readable
    int order;         // from head to foot
};
namespace detail {
struct Word { const char* word; const char* label; int order; };
constexpr Word words[] = {
    {"head", "Headwear", 0}, {"hat", "Headwear", 0}, {"cap", "Headwear", 0}, {"beanie", "Headwear", 0}, {"helmet", "Headwear", 0},
    {"eye", "Eyewear", 1}, {"glass", "Eyewear", 1}, {"mask", "Face", 1},
    {"neck", "Neckwear", 2}, {"jewel", "Jewellery", 2}, {"earring", "Earrings", 2},
    {"costume", "Costume", 3}, {"outfit", "Outfit", 3},
    {"outer", "Outerwear", 4}, {"jacket", "Outerwear", 4}, {"top", "Top", 5}, {"shirt", "Top", 5}, {"hood", "Top", 5},
    {"glove", "Gloves", 6}, {"wrist", "Wrist", 6}, {"watch", "Wrist", 6}, {"bag", "Bag", 7}, {"backpack", "Bag", 7},
    {"bottom", "Bottoms", 8}, {"pant", "Bottoms", 8}, {"short", "Bottoms", 8}, {"leg", "Bottoms", 8},
    {"sock", "Socks", 9}, {"foot", "Shoes", 10}, {"shoe", "Shoes", 10}};
constexpr const char* not_outfit[] = {"skin", "face_", "body", "hair", "beard", "brow", "makeup", "eyecolo", "eye_colo",
                                      "tattoo", "gesture", "emote", "voice", "bkobject", "board", "deck", "truck", "wheel", "grip",
                                      "locomotion", "lipcolo", "mole", "stubble", "freckle", "scar", "nose", "mouth", "teeth"};
} // namespace detail

inline std::optional<Kind> of(std::string_view category) {
    std::string folded(category);
    for (auto& c : folded) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto has = [&](const char* word) { return folded.find(word) != std::string::npos; };
    if (folded.empty()) return std::nullopt;
    // A full-body costume is worn over the whole skater, and its category is the body's kind of name
    // ("cust_fullBodyCostume"): taken before the body's own parts are left out.
    if (!has("costume")) {
        // The body's own parts are "cust..." (custhead, custeye, custnoses, custbodies, custmoles...).
        if (folded.starts_with("cust") || std::any_of(std::begin(detail::not_outfit), std::end(detail::not_outfit), has))
            return std::nullopt;
    }
    const auto word = std::find_if(std::begin(detail::words), std::end(detail::words), [&](const detail::Word& w) { return has(w.word); });
    if (word != std::end(detail::words)) return Kind{word->label, word->order};
    // A category this does not know: its name made readable.
    std::string label(category);
    std::replace(label.begin(), label.end(), '_', ' ');
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    return Kind{label, 20};
}
} // namespace dingosdk::outfit_kind
