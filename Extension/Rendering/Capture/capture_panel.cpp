#include "capture_panel.h"
#include <cctype>
#include <cmath>
#include <stdexcept>

// stb_truetype comes with Dear ImGui; imgui_draw.cpp keeps its copy private (STBTT_STATIC), so
// this file compiles its own.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include <imstb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace dingosdk::capture {
namespace {
constexpr Rgb card{18, 20, 24}, white{245, 246, 248}, grey{150, 156, 168}, accent{40, 120, 255};
constexpr std::uint8_t card_alpha = 230;

// Code points of UTF-8 text; bad bytes come out as U+FFFD.
std::u32string decode(std::string_view text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        const int extra = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
        if (extra < 0 || i + static_cast<std::size_t>(extra) >= text.size() + (extra ? 0 : 1)) {
            out.push_back(0xfffd), ++i;
            continue;
        }
        char32_t c = extra ? b & (0x3f >> extra) : b;
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((next & 0xc0) != 0x80) { ok = false; break; }
            c = c << 6 | (next & 0x3f);
        }
        out.push_back(ok ? c : 0xfffd);
        i += ok ? static_cast<std::size_t>(extra) + 1 : 1;
    }
    return out;
}

// Straight-alpha "over": `colour` at `coverage` (0..255) onto the pixel.
void blend(Image& image, std::int64_t x, std::int64_t y, Rgb colour, unsigned coverage) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height || !coverage) return;
    auto* p = image.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    const float a = static_cast<float>(coverage) / 255.0f, under = static_cast<float>(p[3]) / 255.0f, out = a + under * (1 - a);
    for (int c = 0; c < 3; ++c)
        p[c] = static_cast<std::uint8_t>(std::lround((colour[c] * a + p[c] * under * (1 - a)) / out));
    p[3] = static_cast<std::uint8_t>(std::lround(out * 255));
}

struct Font {
    stbtt_fontinfo info{};
    explicit Font(std::span<const std::uint8_t> file) {
        if (file.empty() || !stbtt_InitFont(&info, file.data(), stbtt_GetFontOffsetForIndex(file.data(), 0)))
            throw std::runtime_error("A menu font could not be read.");
    }
    float scale(float pixels) const { return stbtt_ScaleForPixelHeight(&info, pixels); }
    float ascent(float pixels) const {
        int a = 0, d = 0, g = 0;
        stbtt_GetFontVMetrics(&info, &a, &d, &g);
        return static_cast<float>(a) * scale(pixels);
    }
    float width(std::u32string_view text, float pixels) const {
        const float s = scale(pixels);
        float x = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            int advance = 0, bearing = 0;
            stbtt_GetCodepointHMetrics(&info, static_cast<int>(text[i]), &advance, &bearing);
            x += static_cast<float>(advance) * s;
            if (i + 1 < text.size())
                x += static_cast<float>(stbtt_GetCodepointKernAdvance(&info, static_cast<int>(text[i]), static_cast<int>(text[i + 1]))) * s;
        }
        return x;
    }
    // Draws `text` with its top at `top`.
    void draw(Image& image, std::u32string_view text, float left, float top, float pixels, Rgb colour) const {
        const float s = scale(pixels), baseline = top + ascent(pixels);
        float x = left;
        std::vector<unsigned char> glyph;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const int code = static_cast<int>(text[i]);
            const float shift = x - std::floor(x);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBoxSubpixel(&info, code, s, s, shift, 0, &x0, &y0, &x1, &y1);
            const int w = x1 - x0, h = y1 - y0;
            if (w > 0 && h > 0) {
                glyph.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
                stbtt_MakeCodepointBitmapSubpixel(&info, glyph.data(), w, h, w, s, s, shift, 0, code);
                const auto gx = static_cast<std::int64_t>(std::floor(x)) + x0;
                const auto gy = static_cast<std::int64_t>(std::lround(baseline)) + y0;
                for (int row = 0; row < h; ++row)
                    for (int col = 0; col < w; ++col)
                        blend(image, gx + col, gy + row, colour, glyph[static_cast<std::size_t>(row * w + col)]);
            }
            int advance = 0, bearing = 0;
            stbtt_GetCodepointHMetrics(&info, code, &advance, &bearing);
            x += static_cast<float>(advance) * s;
            if (i + 1 < text.size()) x += static_cast<float>(stbtt_GetCodepointKernAdvance(&info, code, static_cast<int>(text[i + 1]))) * s;
        }
    }
};

// `text` broken at spaces into at most `max_lines` lines of `width`; the last one cut short with "...".
std::vector<std::u32string> wrapped(const Font& font, std::u32string text, float pixels, float width, std::size_t max_lines) {
    std::vector<std::u32string> lines;
    while (!text.empty() && lines.size() < max_lines) {
        if (font.width(text, pixels) <= width) { lines.push_back(text); break; }
        if (lines.size() + 1 == max_lines) {
            while (!text.empty() && font.width(text + U"...", pixels) > width) text.pop_back();
            while (!text.empty() && text.back() == U' ') text.pop_back();
            lines.push_back(text + U"...");
            break;
        }
        // The longest run of words that fits; a single word too long for a line is split anywhere.
        std::size_t cut = 0;
        for (std::size_t i = 1; i <= text.size(); ++i) {
            if (font.width(std::u32string_view(text).substr(0, i), pixels) > width) break;
            if (i == text.size() || text[i] == U' ') cut = i;
        }
        if (!cut) {
            cut = 1;
            while (cut < text.size() && font.width(std::u32string_view(text).substr(0, cut + 1), pixels) <= width) ++cut;
        }
        lines.push_back(text.substr(0, cut));
        text.erase(0, cut);
        while (!text.empty() && text.front() == U' ') text.erase(0, 1);
    }
    return lines;
}

// Coverage (0..1) of a rounded rectangle at a pixel centre.
float rounded(float px, float py, float w, float h, float radius) {
    const float dx = std::max({radius - px, px - (w - radius), 0.0f});
    const float dy = std::max({radius - py, py - (h - radius), 0.0f});
    return std::clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
}
} // namespace

Image render_parts_panel(std::string_view title, std::span<const Part> parts, std::uint32_t height, const PanelFonts& fonts) {
    if (height < 64) throw std::invalid_argument("The parts card is too small.");
    const Font brush(fonts.brush), heading(fonts.heading), body(fonts.body);
    const auto H = static_cast<float>(height);
    const auto width = static_cast<std::uint32_t>(std::lround(H * 0.72f));
    const float pad = H * 0.07f, inner = static_cast<float>(width) - pad * 2;

    // Lay the card out at full size; if it does not fit, again with everything smaller.
    struct Line { const Font* font; std::u32string text; float x, y, size; Rgb colour; };
    std::vector<Line> lines;
    float scale = 1.0f, bottom = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        lines.clear();
        const float title_size = H * 0.11f * scale, label_size = H * 0.042f * scale, value_size = H * 0.058f * scale;
        float y = pad;
        auto title_text = decode(title);
        while (!title_text.empty() && brush.width(title_text, title_size) > inner) title_text.pop_back();
        lines.push_back({&brush, title_text, pad, y, title_size, white});
        y += title_size * 1.05f + H * 0.035f * scale; // room for the accent rule
        for (const auto& part : parts) {
            std::string label = part.slot;
            for (auto& c : label) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            lines.push_back({&body, decode(label), pad, y, label_size, grey});
            y += label_size * 1.25f;
            const auto name = part.name.empty() ? std::string("None") : part.name;
            for (auto& text : wrapped(heading, decode(name), value_size, inner, 2)) {
                lines.push_back({&heading, std::move(text), pad, y, value_size, white});
                y += value_size * 1.1f;
            }
            y += H * 0.03f * scale;
        }
        bottom = y;
        if (bottom <= H - pad * 0.5f) break;
        scale *= 0.88f;
    }

    Image out(width, height);
    const float radius = H * 0.04f;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const float cover = rounded(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(width), H, radius);
            auto* p = out.at(x, y);
            p[0] = card[0], p[1] = card[1], p[2] = card[2];
            p[3] = static_cast<std::uint8_t>(std::lround(cover * card_alpha));
        }
    // The accent rule under the title.
    const auto& title_line = lines.front();
    const float rule_y = title_line.y + title_line.size * 1.05f + H * 0.008f;
    const float rule_w = std::min(inner, std::max(H * 0.12f, brush.width(title_line.text, title_line.size)));
    for (auto y = static_cast<std::int64_t>(rule_y); y < static_cast<std::int64_t>(rule_y + std::max(2.0f, H * 0.008f)); ++y)
        for (auto x = static_cast<std::int64_t>(pad); x < static_cast<std::int64_t>(pad + rule_w); ++x) blend(out, x, y, accent, 255);
    for (const auto& line : lines) line.font->draw(out, line.text, line.x, line.y, line.size, line.colour);
    return out;
}
} // namespace dingosdk::capture
