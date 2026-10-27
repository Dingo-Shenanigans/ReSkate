#include "studio_cover_maker.h"
#include "studio_market_art.h"
#include "studio_torn.h"
#include "Engine/Core/Json/json.h"

#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

// The cover is drawn twice its size and brought down, which smooths every edge (the paper's tear, the letters).
// Pictures are straight alpha; the layer a banner is drawn on is premultiplied, so it can be turned and laid down whole.
namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
constexpr float pi = 3.14159265f;

struct Rgba {
    float r{}, g{}, b{}, a{};
};
// The cover being drawn: opaque.
struct Canvas {
    int n{};
    std::vector<float> rgb; // n * n * 3, 0..1
    void blend(int x, int y, Rgba c) { // straight colour c over it
        if (x < 0 || y < 0 || x >= n || y >= n || c.a <= 0) return;
        float* p = &rgb[(static_cast<std::size_t>(y) * n + x) * 3];
        p[0] = p[0] * (1 - c.a) + c.r * c.a;
        p[1] = p[1] * (1 - c.a) + c.g * c.a;
        p[2] = p[2] * (1 - c.a) + c.b * c.a;
    }
};
// A banner or a tag, drawn flat before it is turned: premultiplied.
struct Layer {
    int w{}, h{};
    std::vector<Rgba> px;
    Layer(int width, int height) : w(width), h(height), px(static_cast<std::size_t>(width) * height) {}
    void over(int x, int y, Rgba c) { // premultiplied c over it
        if (x < 0 || y < 0 || x >= w || y >= h || c.a <= 0) return;
        auto& d = px[static_cast<std::size_t>(y) * w + x];
        d.r = c.r + d.r * (1 - c.a);
        d.g = c.g + d.g * (1 - c.a);
        d.b = c.b + d.b * (1 - c.a);
        d.a = c.a + d.a * (1 - c.a);
    }
};

// ---- the pictures, read once and kept while covers are made ----
struct Picture {
    std::vector<std::uint8_t> rgba;
    int w{}, h{};
    Rgba at(float x, float y) const { // bilinear, straight alpha, clamped at the edges
        x = std::clamp(x, 0.0f, static_cast<float>(w - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        const float fx = x - x0, fy = y - y0;
        Rgba out;
        const auto add = [&](int px, int py, float wgt) {
            const auto* p = &rgba[(static_cast<std::size_t>(py) * w + px) * 4];
            out.r += p[0] / 255.0f * wgt;
            out.g += p[1] / 255.0f * wgt;
            out.b += p[2] / 255.0f * wgt;
            out.a += p[3] / 255.0f * wgt;
        };
        add(x0, y0, (1 - fx) * (1 - fy));
        add(x1, y0, fx * (1 - fy));
        add(x0, y1, (1 - fx) * fy);
        add(x1, y1, fx * fy);
        return out;
    }
    Rgba tiled(float x, float y) const { // wrapping, nearest-ish (for the tiling textures)
        const int px = ((static_cast<int>(std::floor(x)) % w) + w) % w, py = ((static_cast<int>(std::floor(y)) % h) + h) % h;
        const auto* p = &rgba[(static_cast<std::size_t>(py) * w + px) * 4];
        return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    }
};
struct Store {
    std::mutex mutex;
    std::map<std::wstring, std::shared_ptr<const Picture>> pictures;
};
Store& store() {
    static auto* s = new Store;
    return *s;
}
std::shared_ptr<const Picture> picture(const std::wstring& resource, bool file = false) {
    auto& s = store();
    {
        std::lock_guard lock(s.mutex);
        if (const auto found = s.pictures.find(resource); found != s.pictures.end()) return found->second;
    }
    auto decoded = file ? decode_picture_file(resource.c_str(), 2048) : decode_studio_resource(resource.c_str());
    if (!decoded) return nullptr;
    auto made = std::make_shared<Picture>();
    made->rgba = std::move(decoded->rgba);
    made->w = decoded->width;
    made->h = decoded->height;
    std::lock_guard lock(s.mutex);
    return s.pictures[resource] = made;
}

// ---- type ----
struct Face {
    stbtt_fontinfo info{};
    bool ok{};
};
const Face& face(const wchar_t* resource) {
    static std::mutex mutex;
    static std::map<std::wstring, std::unique_ptr<Face>> faces;
    std::lock_guard lock(mutex);
    auto& slot = faces[resource];
    if (!slot) {
        slot = std::make_unique<Face>();
        const auto [bytes, size] = studio_resource_bytes(resource);
        slot->ok = bytes && size && stbtt_InitFont(&slot->info, bytes, stbtt_GetFontOffsetForIndex(bytes, 0));
    }
    return *slot;
}
const wchar_t* font_resource(std::string_view font) {
    if (font == "bebas") return L"FONT_BEBAS";
    if (font == "dirt") return L"FONT_DIRT";
    if (font == "sedgwick") return L"FONT_SEDGWICK";
    if (font == "knewave") return L"FONT_KNEWAVE";
    if (font == "marker") return L"FONT_BRUSH";
    return L"FONT_ANTON";
}
float text_width(const Face& f, std::string_view text, float px) {
    if (!f.ok) return 0;
    const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, px);
    float w = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&f.info, static_cast<unsigned char>(text[i]), &advance, &bearing);
        w += advance * scale;
        if (i + 1 < text.size()) w += stbtt_GetCodepointKernAdvance(&f.info, static_cast<unsigned char>(text[i]), static_cast<unsigned char>(text[i + 1])) * scale;
    }
    return w;
}
// The words centred on `cx`, sitting on `baseline`, in colour c (straight).
void draw_text(Layer& layer, const Face& f, std::string_view text, float px, float cx, float baseline, Rgba c) {
    if (!f.ok) return;
    const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, px);
    float x = cx - text_width(f, text, px) * 0.5f;
    std::vector<unsigned char> bitmap;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const int cp = static_cast<unsigned char>(text[i]);
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&f.info, cp, &advance, &bearing);
        const float shift = x - std::floor(x);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBoxSubpixel(&f.info, cp, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            bitmap.assign(static_cast<std::size_t>(gw) * gh, 0);
            stbtt_MakeCodepointBitmapSubpixel(&f.info, bitmap.data(), gw, gh, gw, scale, scale, shift, 0, cp);
            const int ox = static_cast<int>(std::floor(x)) + x0, oy = static_cast<int>(std::lround(baseline)) + y0;
            for (int yy = 0; yy < gh; ++yy)
                for (int xx = 0; xx < gw; ++xx) {
                    const float a = bitmap[static_cast<std::size_t>(yy) * gw + xx] / 255.0f * c.a;
                    if (a > 0) layer.over(ox + xx, oy + yy, {c.r * a, c.g * a, c.b * a, a});
                }
        }
        x += advance * scale;
        if (i + 1 < text.size()) x += stbtt_GetCodepointKernAdvance(&f.info, cp, static_cast<unsigned char>(text[i + 1])) * scale;
    }
}

// ---- paper ----
using torn::Point;
// Fills a polygon (even-odd, sampled at pixel centres) on the layer; `paint` gives each pixel its premultiplied colour.
template <class Paint> void fill(Layer& layer, const std::vector<Point>& poly, float dx, float dy, Paint paint) {
    if (poly.size() < 3) return;
    float top = 1e9f, bottom = -1e9f;
    for (const auto& p : poly) top = std::min(top, p.y + dy), bottom = std::max(bottom, p.y + dy);
    std::vector<float> xs;
    for (int y = std::max(0, static_cast<int>(top)); y < std::min(layer.h, static_cast<int>(bottom) + 1); ++y) {
        const float cy = y + 0.5f;
        xs.clear();
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const float ay = poly[j].y + dy, by = poly[i].y + dy;
            if ((ay > cy) != (by > cy)) xs.push_back(poly[j].x + dx + (cy - ay) / (by - ay) * (poly[i].x - poly[j].x));
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2)
            for (int x = std::max(0, static_cast<int>(std::ceil(xs[i] - 0.5f))); x < std::min(layer.w, static_cast<int>(std::ceil(xs[i + 1] - 0.5f))); ++x)
                layer.over(x, y, paint(x, y));
    }
}
struct Kind {
    const wchar_t* texture;
    Rgba rim;
    Rgba ink;
};
Kind kind_of(std::string_view banner) {
    if (banner == "white") return {L"STUDIO_ART_PAPER_WHITE", {0.984f, 0.98f, 0.965f, 1}, {0.043f, 0.047f, 0.059f, 1}};
    if (banner == "grip") return {L"STUDIO_ART_GRIP", {0.557f, 0.565f, 0.592f, 1}, {1, 1, 1, 1}};
    return {L"STUDIO_ART_PAPER_BLUE", {0.906f, 0.933f, 0.984f, 1}, {1, 1, 1, 1}};
}
// A piece of torn paper (or tape) w x h at (x, y) on the layer: its shadow, its white core, the paper.
void paper_on(Layer& layer, std::string_view banner, float x, float y, float w, float h, std::uint32_t seed, float k) {
    if (banner == "tape") {
        torn::Random r(seed);
        std::vector<Point> poly;
        for (int i = 0; i <= 10; ++i) poly.push_back({x + w * i / 10, y + (r() - 0.5f) * 3 * k});
        for (int j = 0; j <= 6; ++j) poly.push_back({x + w + (j % 2 ? 5.0f : -2.0f) * k, y + h * j / 6});
        for (int i = 10; i >= 0; --i) poly.push_back({x + w * i / 10, y + h + (r() - 0.5f) * 3 * k});
        for (int j = 6; j >= 0; --j) poly.push_back({x + (j % 2 ? -5.0f : 2.0f) * k, y + h * j / 6});
        fill(layer, poly, 0, 0, [](int, int) { return Rgba{0.925f * 0.92f, 0.91f * 0.92f, 0.839f * 0.92f, 0.92f}; });
        return;
    }
    const auto kind = kind_of(banner);
    const auto texture = picture(kind.texture);
    const auto shape = torn::piece(w, h, seed, 2.4f * k, 9 * k, 7 * k);
    const auto outline = shape.outline(), rim = torn::rim(shape, seed, 3 * k).outline();
    fill(layer, outline, x + 6 * k, y + 8 * k, [](int, int) { return Rgba{0, 0, 0, 0.45f}; });
    fill(layer, rim, x, y, [&](int, int) { return kind.rim; });
    fill(layer, outline, x, y, [&](int px, int py) {
        if (!texture) return Rgba{kind.rim.r * 0.4f, kind.rim.g * 0.4f, kind.rim.b * 0.4f, 1};
        auto c = texture->tiled(static_cast<float>(px), static_cast<float>(py));
        c.a = 1;
        return c;
    });
}
// The layer laid on the cover, turned by `angle` about its centre, its centre at (cx, cy).
void lay(Canvas& canvas, const Layer& layer, float cx, float cy, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), hw = layer.w * 0.5f, hh = layer.h * 0.5f;
    const float reach = std::sqrt(hw * hw + hh * hh) + 2;
    for (int y = std::max(0, static_cast<int>(cy - reach)); y < std::min(canvas.n, static_cast<int>(cy + reach) + 1); ++y)
        for (int x = std::max(0, static_cast<int>(cx - reach)); x < std::min(canvas.n, static_cast<int>(cx + reach) + 1); ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float lx = dx * c + dy * s + hw - 0.5f, ly = -dx * s + dy * c + hh - 0.5f;
            if (lx < -1 || ly < -1 || lx > layer.w || ly > layer.h) continue;
            const int x0 = static_cast<int>(std::floor(lx)), y0 = static_cast<int>(std::floor(ly));
            const float fx = lx - x0, fy = ly - y0;
            Rgba p;
            const auto add = [&](int px, int py, float wgt) {
                if (px < 0 || py < 0 || px >= layer.w || py >= layer.h) return;
                const auto& q = layer.px[static_cast<std::size_t>(py) * layer.w + px];
                p.r += q.r * wgt, p.g += q.g * wgt, p.b += q.b * wgt, p.a += q.a * wgt;
            };
            add(x0, y0, (1 - fx) * (1 - fy));
            add(x0 + 1, y0, fx * (1 - fy));
            add(x0, y0 + 1, (1 - fx) * fy);
            add(x0 + 1, y0 + 1, fx * fy);
            if (p.a <= 0.001f) continue;
            canvas.blend(x, y, {p.r / p.a, p.g / p.a, p.b / p.a, std::min(1.0f, p.a)});
        }
}
// A picture drawn over the box (x, y, w, h), at `alpha`.
void draw_picture(Canvas& canvas, const Picture& pic, float x, float y, float w, float h, float alpha) {
    for (int py = std::max(0, static_cast<int>(y)); py < std::min(canvas.n, static_cast<int>(y + h) + 1); ++py)
        for (int px = std::max(0, static_cast<int>(x)); px < std::min(canvas.n, static_cast<int>(x + w) + 1); ++px) {
            const float u = (px + 0.5f - x) / w * pic.w - 0.5f, v = (py + 0.5f - y) / h * pic.h - 0.5f;
            if (u < -0.5f || v < -0.5f || u > pic.w - 0.5f || v > pic.h - 0.5f) continue;
            auto c = pic.at(u, v);
            c.a *= alpha;
            canvas.blend(px, py, c);
        }
}
// A photo filling the cover, its middle kept (`focus` across).
void cover_fit(Canvas& canvas, const Picture& pic, float focus) {
    const float n = static_cast<float>(canvas.n), scale = std::max(n / pic.w, n / pic.h), w = pic.w * scale, h = pic.h * scale;
    draw_picture(canvas, pic, (n - w) * focus, (n - h) * 0.5f, w, h, 1);
}

struct Lines {
    std::vector<std::string> lines;
    float size{};
};
// The title in at most two lines as wide as `most`, as big as that allows.
Lines wrap_title(const Face& f, const std::string& text, float most, float start, float step) {
    std::vector<std::string> words;
    for (std::size_t i = 0; i < text.size();) {
        const auto j = text.find(' ', i);
        const auto word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!word.empty()) words.push_back(word);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (words.empty()) return {};
    for (float size = start; size > 20; size -= step) {
        Lines out{{}, size};
        std::string line;
        for (const auto& w : words) {
            const auto joined = line.empty() ? w : line + " " + w;
            if (!line.empty() && text_width(f, joined, size) > most) {
                out.lines.push_back(line);
                line = w;
            } else line = joined;
        }
        out.lines.push_back(line);
        if (out.lines.size() <= 2 && std::all_of(out.lines.begin(), out.lines.end(), [&](const auto& l) { return text_width(f, l, size) <= most; }))
            return out;
    }
    return {{text}, 20};
}
std::string upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
} // namespace

std::vector<std::uint8_t> make_cover(const CoverDesign& c, int side) {
    side = std::clamp(side, 64, 1024);
    Canvas canvas;
    canvas.n = side * 2;
    canvas.rgb.assign(static_cast<std::size_t>(canvas.n) * canvas.n * 3, 0.08f);
    const float n = static_cast<float>(canvas.n), k = n / 512.0f;
    torn::Random r(c.seed ? c.seed : 3);
    const bool own = c.base == "own" || c.as_is;
    if (own) {
        if (const auto pic = c.own.empty() ? nullptr : picture(wide(c.own), true)) cover_fit(canvas, *pic, 0.5f);
    }
    if (!c.as_is) {
        // What is behind it.
        const bool photo = c.base == "skater" || c.base == "print" || own;
        if (!own) {
            const wchar_t* name = c.base == "print"      ? L"STUDIO_ART_PRINT"
                                  : c.base == "grip"     ? L"STUDIO_ART_GRIP"
                                  : c.base == "concrete" ? L"STUDIO_ART_CONCRETE"
                                  : c.base == "paper"    ? L"STUDIO_ART_PAPER_WHITE"
                                  : c.base == "blue"     ? L"STUDIO_ART_PAPER_BLUE"
                                                         : L"STUDIO_ART_SKATER";
            if (const auto pic = picture(name)) {
                if (photo) cover_fit(canvas, *pic, c.base == "skater" ? 0.62f : 0.55f);
                else {
                    const float texel = k * 0.62f;
                    for (int y = 0; y < canvas.n; ++y)
                        for (int x = 0; x < canvas.n; ++x) canvas.blend(x, y, pic->tiled(x / texel, y / texel));
                }
            }
        }
        if (!photo) {
            // A dark texture gets the shop's blue splash in a corner; every one a halftone, so no two covers are the same.
            if (c.base == "grip" || c.base == "concrete") {
                if (c.base == "concrete")
                    for (int y = 0; y < canvas.n; ++y)
                        for (int x = 0; x < canvas.n; ++x) canvas.blend(x, y, {0.04f, 0.047f, 0.07f, 0.55f});
                if (const auto splash = picture(r() < 0.5f ? L"STUDIO_ART_SPLATTER_A" : L"STUDIO_ART_SPLATTER_B")) {
                    const float size = n * (1 + r() * 0.5f);
                    draw_picture(canvas, *splash, n - size * (0.55f + r() * 0.25f), -size * (0.2f + r() * 0.3f), size,
                                 size * splash->h / static_cast<float>(splash->w), 0.9f);
                }
            }
            if (const auto dots = picture(L"STUDIO_ART_HALFTONE")) draw_picture(canvas, *dots, -n * 0.2f, n * 0.35f, n * 1.1f, n * 0.82f, c.base == "blue" ? 0.35f : 0.7f);
        } else
            for (int y = static_cast<int>(n * 0.45f); y < canvas.n; ++y) {
                const float a = 0.55f * (y - n * 0.45f) / (n * 0.55f);
                for (int x = 0; x < canvas.n; ++x) canvas.blend(x, y, {0, 0, 0, a});
            }
        // The title on its banner.
        const auto title = upper(c.title);
        if (!title.empty()) {
            const auto& f = face(font_resource(c.font));
            const auto lines = wrap_title(f, title, n * 0.82f, n * 0.2f, 4 * k);
            const float size = lines.size, lh = size * (c.font == "knewave" || c.font == "sedgwick" ? 1.02f : 0.95f);
            float widest = 0;
            for (const auto& l : lines.lines) widest = std::max(widest, text_width(f, l, size));
            const float bw = widest + 46 * k, bh = lh * static_cast<float>(lines.lines.size()) + 26 * k;
            const float y0 = c.place == "top" ? n * 0.1f : c.place == "middle" ? (n - bh) / 2 : n - bh - n * 0.12f;
            const float tilt = ((c.seed % 2) ? -1.0f : 1.0f) * (1.5f + r() * 1.5f) * pi / 180;
            const float pad = 40 * k;
            Layer layer(static_cast<int>(bw + pad * 2), static_cast<int>(bh + pad * 2));
            if (c.banner != "none") paper_on(layer, c.banner, pad, pad, bw, bh, c.seed ? c.seed : 3, k);
            const bool dark_ink = c.banner == "white" || c.banner == "tape";
            const Rgba ink = dark_ink ? Rgba{0.043f, 0.047f, 0.059f, 1} : Rgba{1, 1, 1, 1};
            for (std::size_t i = 0; i < lines.lines.size(); ++i) {
                const float baseline = pad + 13 * k + lh * (static_cast<float>(i) + 0.82f);
                if (c.banner == "none") draw_text(layer, f, lines.lines[i], size, pad + bw / 2 + 4 * k, baseline + 4 * k, {0, 0, 0, 0.6f});
                draw_text(layer, f, lines.lines[i], size, pad + bw / 2, baseline, ink);
            }
            lay(canvas, layer, n / 2, y0 + bh / 2, tilt);
        }
        // The tag, stuck in a corner.
        if (!c.tag.empty()) {
            const auto& f = face(L"FONT_BRUSH");
            const float size = 30 * k, tw = text_width(f, c.tag, size) + 34 * k, th = 50 * k, pad = 24 * k;
            const float tx = 26 * k, ty = c.place == "top" ? n - th - 30 * k : 30 * k;
            const bool white = c.tag == "ALL STANCES";
            Layer layer(static_cast<int>(tw + pad * 2), static_cast<int>(th + pad * 2));
            paper_on(layer, white ? "white" : "grip", pad, pad, tw, th, (c.seed ? c.seed : 3) + 7, k * 0.6f);
            draw_text(layer, f, c.tag, size, pad + tw / 2, pad + th * 0.72f, white ? Rgba{0.043f, 0.047f, 0.059f, 1} : Rgba{1, 1, 1, 1});
            lay(canvas, layer, tx + tw / 2, ty + th / 2, -4 * pi / 180);
        }
    }
    // Brought down to its size: each pixel the four under it.
    std::vector<std::uint8_t> out(static_cast<std::size_t>(side) * side * 4);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            float sum[3]{};
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const float* p = &canvas.rgb[(static_cast<std::size_t>(y * 2 + dy) * canvas.n + x * 2 + dx) * 3];
                    sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
                }
            auto* o = &out[(static_cast<std::size_t>(y) * side + x) * 4];
            for (int i = 0; i < 3; ++i) o[i] = static_cast<std::uint8_t>(std::clamp(sum[i] * 0.25f * 255.0f + 0.5f, 0.0f, 255.0f));
            o[3] = 255;
        }
    return out;
}

std::string cover_png(const std::vector<std::uint8_t>& rgba, int side) {
    if (rgba.size() != static_cast<std::size_t>(side) * side * 4) return {};
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    std::string out;
    // The PNG encoder takes blue, green, red, alpha.
    std::vector<std::uint8_t> bgra(rgba);
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(static_cast<UINT>(side), static_cast<UINT>(side))) &&
            SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
            SUCCEEDED(frame->WritePixels(static_cast<UINT>(side), static_cast<UINT>(side) * 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
            STATSTG stat{};
            if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))) {
                out.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
                const LARGE_INTEGER start{};
                ULONG read = 0;
                if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)) || FAILED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read)) ||
                    read != out.size())
                    out.clear();
            }
        }
    }
    if (com) CoUninitialize();
    return out;
}

void release_cover_art() {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    s.pictures.clear();
}

std::string cover_design_json(const CoverDesign& c) {
    Json j = Json::object();
    j["base"] = c.base;
    j["own"] = c.own;
    j["as_is"] = c.as_is;
    j["title"] = c.title;
    j["font"] = c.font;
    j["banner"] = c.banner;
    j["place"] = c.place;
    j["tag"] = c.tag;
    j["seed"] = static_cast<std::int64_t>(c.seed);
    return j.dump();
}
bool read_cover_design(const std::string& text, CoverDesign& out) {
    try {
        const auto j = Json::parse(text);
        if (!j.is_object()) return false;
        CoverDesign c;
        const auto str = [&](const char* key, std::string& field) {
            if (j.contains(key) && j.at(key).is_string()) field = j.at(key).string().substr(0, 260);
        };
        str("base", c.base);
        str("own", c.own);
        str("title", c.title);
        str("font", c.font);
        str("banner", c.banner);
        str("place", c.place);
        str("tag", c.tag);
        if (j.contains("as_is") && j.at("as_is").is_boolean()) c.as_is = j.at("as_is").get<bool>();
        if (j.contains("seed") && j.at("seed").is_number()) c.seed = static_cast<std::uint32_t>(j.at("seed").get<double>());
        out = c;
        return true;
    } catch (...) { return false; }
}
} // namespace dingosdk::overlay
