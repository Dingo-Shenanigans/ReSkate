#pragma once
#include <string>
#include <string_view>
#include <functional>
#include "Engine/Core/Json/json.h"
namespace dingosdk::trick_names {
// Resolve individual native localization tokens; preserve all raw keys in
// the JSON record. An unresolved token stays visible rather than guessed.
inline std::string resolve_text(std::string_view raw,
    const std::function<std::string(std::string_view)>& lookup, bool& complete) {
    std::string out;
    complete = true;
    for (std::size_t at = 0; at < raw.size();) {
        if (raw.substr(at, 3) != "ID_") { out += raw[at++]; continue; }
        auto end = at + 3;
        while (end < raw.size() && ((raw[end] >= 'A' && raw[end] <= 'Z') ||
            (raw[end] >= '0' && raw[end] <= '9') || raw[end] == '_')) ++end;
        const auto key = raw.substr(at, end - at);
        const auto text = lookup(key);
        if (text.empty() || text == key) { complete = false; out += key; }
        else out += text;
        at = end;
    }
    return out;
}
inline std::string plain_text(std::string_view text) {
    // The native ticker can include [font ...] markup. Keep its text content,
    // retain the original decorated value separately for diagnosis.
    std::string out;
    bool space{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '[' && (text.substr(i, 5) == "[font" || text.substr(i, 6) == "[/font")) {
            const auto end = text.find(']', i);
            if (end != std::string_view::npos) { i = end; continue; }
        }
        const auto c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { space = !out.empty(); continue; }
        if (space) out += ' ';
        space = false; out += c;
    }
    return out;
}
inline std::string comparison_key(const Json& raw_labels) {
    auto canonical = Json::object();
    for (const auto& [key, value] : raw_labels.items())
        if (key != "completed_info") canonical[key] = plain_text(value.string());
    // Preserve the native identity labels, including direction and spin text.
    // Do not compare score, cosmetic markup, row IDs, or unverified float units.
    return canonical.dump();
}
}
