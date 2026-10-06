#include "chat_rich.h"
#include "chat_emotes.h"
#include <algorithm>
#include <cfloat>

namespace dingosdk::overlay::chat_rich {
namespace {
bool known_emote(std::string_view name) {
    ChatEmote emote;
    return !name.empty() && chat_emote(name, emote);
}
bool spaces_only(std::string_view part) { return part.find_first_not_of(' ') == std::string_view::npos; }
} // namespace

bool has_emotes(std::string_view text) {
    if (!chat_emotes_loaded()) return false;
    for (std::size_t at = text.find(':'); at != std::string_view::npos; at = text.find(':', at + 1)) {
        const auto end = text.find(':', at + 1);
        if (end == std::string_view::npos) return false;
        if (end > at + 1 && known_emote(text.substr(at + 1, end - at - 1))) return true;
    }
    return false;
}

bool only_emotes(std::string_view text, std::size_t &count) {
    count = 0;
    if (!chat_emotes_loaded()) return false;
    for (std::size_t at = 0; at < text.size();) {
        if (text[at] == ' ') {
            ++at;
            continue;
        }
        if (text[at] != ':') return false;
        const auto end = text.find(':', at + 1);
        if (end == std::string_view::npos || !known_emote(text.substr(at + 1, end - at - 1))) return false;
        ++count;
        at = end + 1;
    }
    return count > 0;
}

RichText lay_out(ImFont *font, float size, std::string_view text, float wrap, float indent, std::size_t max_lines,
                 float emote_scale) {
    RichText out;
    const float text_height = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "Ay").y, emote_height = text_height * emote_scale;
    std::vector<float> lines{text_height};
    float x = indent;
    const auto offset = [&](std::string_view part) { return static_cast<std::size_t>(part.data() - text.data()); };
    const auto measure = [&](std::string_view part) {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, part.data(), part.data() + part.size());
    };
    const auto new_line = [&] {
        x = 0.0f;
        lines.push_back(text_height);
    };
    const auto place = [&](RichItem item) {
        if (x > 0.0f && x + item.size.x > wrap) new_line();
        item.at.x = x;
        item.line = lines.size() - 1;
        lines.back() = std::max(lines.back(), item.size.y);
        x += item.size.x;
        out.items.push_back(item);
    };
    // Text: whole where it fits on a line; longer than a line, broken where it must be, each
    // piece filling what is left of its line.
    const auto place_text = [&](std::string_view part) {
        while (!part.empty()) {
            const auto whole = measure(part);
            if (whole.x <= wrap - x || (x == 0.0f && whole.x <= wrap)) {
                place({offset(part), part.size(), false, {}, ImVec2(whole.x, text_height)});
                return;
            }
            if (whole.x <= wrap) { // fits a line of its own
                new_line();
                continue;
            }
            if (x > 0.0f && measure(part.substr(0, 1)).x > wrap - x) new_line();
            std::size_t fit = 1;
            while (fit < part.size() && measure(part.substr(0, fit + 1)).x <= wrap - x) ++fit;
            // Never split a UTF-8 character.
            const auto continuation = [&](std::size_t i) {
                return i < part.size() && (static_cast<unsigned char>(part[i]) & 0xC0) == 0x80;
            };
            while (fit > 1 && continuation(fit)) --fit;
            while (continuation(fit)) ++fit; // a single character wider than the line
            const auto piece = part.substr(0, fit);
            place({offset(piece), piece.size(), false, {}, ImVec2(measure(piece).x, text_height)});
            part.remove_prefix(fit);
            if (!part.empty()) new_line();
        }
    };
    for (std::size_t at = 0; at < text.size();) {
        // A word and the spaces after it.
        auto end = text.find(' ', at);
        end = end == std::string_view::npos ? text.size() : text.find_first_not_of(' ', end);
        if (end == std::string_view::npos) end = text.size();
        const auto token = text.substr(at, end - at);
        const auto last = token.find_last_not_of(' ');
        const auto word = last == std::string_view::npos ? std::string_view{} : token.substr(0, last + 1);
        at = end;
        // Emotes can sit inside a word too (":blob::blob:"): each :name: of a known emote is an
        // image, the rest text.
        std::size_t done = 0;
        for (auto open = word.find(':'); open != std::string_view::npos;) {
            const auto close = word.find(':', open + 1);
            if (close == std::string_view::npos) break;
            ChatEmote emote;
            if (close > open + 1 && chat_emote(word.substr(open + 1, close - open - 1), emote)) {
                if (open > done) place_text(word.substr(done, open - done));
                place({offset(word) + open + 1, close - open - 1, true, {}, ImVec2(emote_height * emote.aspect, emote_height)});
                done = close + 1;
                open = word.find(':', done);
            } else {
                open = close; // it may open the next one
            }
        }
        if (done < word.size()) place_text(word.substr(done));
        const auto spaces = token.substr(word.size());
        if (!spaces.empty()) place({offset(spaces), spaces.size(), false, {}, measure(spaces)});
    }
    // Past `max_lines`: cut, and make room for "..." at the end of the last line kept.
    if (max_lines && lines.size() > max_lines) {
        out.truncated = true;
        lines.resize(max_lines);
        std::erase_if(out.items, [&](const RichItem &item) { return item.line >= max_lines; });
        const float dots = measure("...").x;
        const auto line_end = [&] {
            float end = 0.0f;
            for (const auto &item : out.items)
                if (item.line + 1 == max_lines) end = std::max(end, item.at.x + item.size.x);
            return end;
        };
        while (!out.items.empty() && out.items.back().line + 1 == max_lines && line_end() + dots > wrap)
            out.items.pop_back();
        // Trailing spaces before the dots read oddly.
        while (!out.items.empty() && out.items.back().line + 1 == max_lines && !out.items.back().emote &&
               spaces_only(text.substr(out.items.back().begin, out.items.back().length)))
            out.items.pop_back();
        out.ellipsis.x = line_end();
    }
    std::vector<float> tops(lines.size());
    for (std::size_t i = 1; i < lines.size(); ++i) tops[i] = tops[i - 1] + lines[i - 1];
    for (auto &item : out.items) item.at.y = tops[item.line] + (lines[item.line] - item.size.y) * 0.5f;
    out.height = tops.back() + lines.back();
    out.lines = lines.size();
    for (const auto &item : out.items)
        if (item.emote || !spaces_only(text.substr(item.begin, item.length)))
            out.width = std::max(out.width, item.at.x + item.size.x);
    if (out.truncated) {
        out.ellipsis.y = tops.back() + (lines.back() - text_height) * 0.5f;
        out.width = std::max(out.width, out.ellipsis.x + measure("...").x);
    }
    return out;
}

void draw_rich(ImDrawList *draw, ImFont *font, float size, ImVec2 origin, std::string_view text, const RichText &rich,
               ImU32 colour, float alpha, float scale) {
    const auto image = IM_COL32(255, 255, 255, static_cast<int>(255.0f * std::clamp(alpha, 0.0f, 1.0f)));
    for (const auto &item : rich.items) {
        if (item.begin > text.size() || item.length > text.size() - item.begin) continue;
        const auto part = text.substr(item.begin, item.length);
        const ImVec2 at(origin.x + item.at.x * scale, origin.y + item.at.y * scale);
        if (item.emote) {
            // Looked up as it is drawn, so an animated emote shows its current frame.
            ChatEmote emote;
            if (chat_emote(part, emote))
                draw->AddImage(emote.texture, at, ImVec2(at.x + item.size.x * scale, at.y + item.size.y * scale), emote.uv0,
                               emote.uv1, image);
        } else {
            draw->AddText(font, size * scale, at, colour, part.data(), part.data() + part.size());
        }
    }
    if (rich.truncated)
        draw->AddText(font, size * scale, ImVec2(origin.x + rich.ellipsis.x * scale, origin.y + rich.ellipsis.y * scale),
                      colour, "...");
}

std::string restore_emotes(std::string_view masked, std::string_view raw) {
    std::string result(masked);
    if (raw.empty() || raw.size() != masked.size() || masked == raw || !chat_emotes_loaded()) return result;
    for (auto open = raw.find(':'); open != std::string_view::npos;) {
        const auto close = raw.find(':', open + 1);
        if (close == std::string_view::npos) break;
        if (known_emote(raw.substr(open + 1, close - open - 1))) {
            result.replace(open, close - open + 1, raw.substr(open, close - open + 1));
            open = raw.find(':', close + 1);
        } else {
            open = close;
        }
    }
    return result;
}
} // namespace dingosdk::overlay::chat_rich
