#include "plugin_picture.h"
#include "plugin_font_directwrite.h"
#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

// The renderer behind reskate.picture. Everything a plugin hands it is untrusted, so nothing of ours
// parses a file: WIC decodes the pictures (PNG and JPEG only) and encodes the PNG, and DirectWrite
// reads the fonts and lays the text out. The drawing itself is ours and on the CPU, on 8-bit
// surfaces whose colour is multiplied by alpha: the canvas starts opaque black and stays opaque, a
// layer starts clear. Edges are smoothed: a polygon by eight rows of exact spans to the pixel, a
// stretched picture and a layer by what each pixel covers and bilinear sampling, text by DirectWrite's
// own greyscale coverage.
namespace dingosdk::plugins::picture {
namespace {
using Microsoft::WRL::ComPtr;

// COM on this thread for as long as a call needs it (a thread that has it another way keeps its own).
struct Com {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
};
ComPtr<IWICImagingFactory> imaging() {
    ComPtr<IWICImagingFactory> factory;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    return factory;
}

// A file's bytes, or nothing for one that is not there, empty, or larger than `most`
// (plugin_font_directwrite.h, which reads the fonts with it too).
using font::read_file;

// Only a PNG or a JPEG: the other formats WIC knows (and any codec someone installed) are never
// handed a plugin's bytes. `pixels` false reads the size alone.
bool decode_with(IWICImagingFactory* factory, std::string_view bytes, Pixels& out, bool pixels) {
    if (!factory || bytes.empty() || bytes.size() > most_picture_bytes) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    GUID container{};
    UINT width{}, height{};
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetContainerFormat(&container)) || (container != GUID_ContainerFormatPng && container != GUID_ContainerFormatJpeg) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height ||
        width > most_picture_side || height > most_picture_side)
        return false;
    out.width = width;
    out.height = height;
    if (!pixels) return true;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()));
}

// ---- surfaces ----------------------------------------------------------------------------------------
struct Surface {
    int width{}, height{};
    std::vector<std::uint8_t> pixels; // red first, colour multiplied by alpha
    Surface(int w, int h, bool opaque) : width(w), height(h), pixels(static_cast<std::size_t>(w) * h * 4) {
        if (opaque)
            for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    }
    std::uint8_t* at(int x, int y) { return pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4; }
    const std::uint8_t* at(int x, int y) const { return pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4; }
};
using Ink = std::array<float, 4>; // 0 to 255 each, colour multiplied by alpha
Ink ink_of(const Colour& colour) {
    const auto part = [](float value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f; };
    const float a = part(colour.a) * 255.0f;
    return {part(colour.r) * a, part(colour.g) * a, part(colour.b) * a, a};
}
// `source` over the pixel, `share` of it (what the pixel is covered by, times any fade): the pixel
// keeps what the source's alpha leaves. On the canvas the alpha stays 255.
inline void over(std::uint8_t* pixel, const float* source, float share) {
    const float keep = 1.0f - source[3] * share * (1.0f / 255.0f);
    for (int i = 0; i < 4; ++i) pixel[i] = static_cast<std::uint8_t>(std::clamp(static_cast<float>(pixel[i]) * keep + source[i] * share + 0.5f, 0.0f, 255.0f));
}
inline void over(std::uint8_t* pixel, const std::uint8_t* source, float share) {
    const float texel[4] = {static_cast<float>(source[0]), static_cast<float>(source[1]), static_cast<float>(source[2]), static_cast<float>(source[3])};
    over(pixel, texel, share);
}

// ---- the pictures a request names -----------------------------------------------------------------
// Each is read and decoded once a request, and halved as often as a step shrinks it by more than two
// (bilinear sampling alone would skip texels). They are let go together when they pass 256 MB.
struct Picture {
    std::vector<Pixels> levels; // [0] as decoded, each next half the one before
};
class Pictures {
public:
    explicit Pictures(IWICImagingFactory* factory) : factory_(factory) {}
    Picture* get(const File& file) {
        if (file.name.empty() || file.file.empty()) return nullptr;
        if (const auto found = known_.find(file.file.native()); found != known_.end()) return found->second.get();
        std::unique_ptr<Picture> picture;
        Pixels pixels;
        if (const auto bytes = read_file(file.file, most_picture_bytes); !bytes.empty() && decode_with(factory_, bytes, pixels, true)) {
            if (held_ + pixels.rgba.size() > most_held) {
                known_.clear();
                held_ = 0;
            }
            held_ += pixels.rgba.size();
            picture = std::make_unique<Picture>();
            picture->levels.push_back(std::move(pixels));
        }
        return known_.emplace(file.file.native(), std::move(picture)).first->second.get();
    }
    // The level to sample when `texels` of the picture go to one pixel: the first where at most two do.
    const Pixels& level(Picture& picture, float texels) {
        std::size_t at = 0;
        while (texels > 2.0f) {
            if (at + 1 == picture.levels.size()) {
                const auto& from = picture.levels[at];
                if (from.width < 2 && from.height < 2) break;
                picture.levels.push_back(half(from));
                held_ += picture.levels.back().rgba.size();
            }
            ++at;
            texels *= 0.5f;
        }
        return picture.levels[at];
    }

private:
    static Pixels half(const Pixels& from) {
        Pixels to;
        to.width = std::max(1u, (from.width + 1) / 2);
        to.height = std::max(1u, (from.height + 1) / 2);
        to.rgba.resize(static_cast<std::size_t>(to.width) * to.height * 4);
        for (std::uint32_t y = 0; y < to.height; ++y) {
            const std::uint32_t y0 = y * 2, y1 = std::min(y0 + 1, from.height - 1);
            for (std::uint32_t x = 0; x < to.width; ++x) {
                const std::uint32_t x0 = x * 2, x1 = std::min(x0 + 1, from.width - 1);
                const auto texel = [&](std::uint32_t tx, std::uint32_t ty) { return from.rgba.data() + (static_cast<std::size_t>(ty) * from.width + tx) * 4; };
                const std::uint8_t *a = texel(x0, y0), *b = texel(x1, y0), *c = texel(x0, y1), *d = texel(x1, y1);
                auto* out = to.rgba.data() + (static_cast<std::size_t>(y) * to.width + x) * 4;
                for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>((a[i] + b[i] + c[i] + d[i] + 2) / 4);
            }
        }
        return to;
    }
    static constexpr std::size_t most_held = 256u << 20;
    IWICImagingFactory* factory_;
    std::map<std::wstring, std::unique_ptr<Picture>> known_;
    std::size_t held_{};
};

// Which texel of a tiled picture each pixel along a side shows: `texel` pixels to one, from the
// surface's top left, the nearest, wrapping.
std::vector<std::uint32_t> tiling(int pixels, std::uint32_t texels, float texel) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(pixels));
    for (int i = 0; i < pixels; ++i)
        out[static_cast<std::size_t>(i)] =
            std::min(static_cast<std::uint32_t>(std::fmod(std::floor((static_cast<double>(i) + 0.5) / static_cast<double>(texel)), static_cast<double>(texels))), texels - 1);
    return out;
}
struct Tiles {
    const Pixels* picture{};
    std::vector<std::uint32_t> columns, rows;
    Tiles(const Surface& surface, const Pixels* of, float texel) : picture(of) {
        if (!picture) return;
        columns = tiling(surface.width, picture->width, texel);
        rows = tiling(surface.height, picture->height, texel);
    }
    const std::uint8_t* at(int x, int y) const {
        return picture->rgba.data() + (static_cast<std::size_t>(rows[static_cast<std::size_t>(y)]) * picture->width + columns[static_cast<std::size_t>(x)]) * 4;
    }
};

// ---- the steps ---------------------------------------------------------------------------------------
void fill(Surface& surface, const Ink& ink) {
    for (int y = 0; y < surface.height; ++y) {
        auto* pixel = surface.at(0, y);
        for (int x = 0; x < surface.width; ++x, pixel += 4) over(pixel, ink.data(), 1.0f);
    }
}
void tile(Surface& surface, const Pixels& picture, float texel) {
    const Tiles tiles(surface, &picture, texel);
    for (int y = 0; y < surface.height; ++y) {
        auto* pixel = surface.at(0, y);
        for (int x = 0; x < surface.width; ++x, pixel += 4) over(pixel, tiles.at(x, y), 1.0f);
    }
}

// One side of a stretched picture: for each pixel the box touches, the two texels it lies between,
// how far towards the second, and how much of the pixel the box covers.
struct Tap {
    std::uint32_t first{}, second{};
    float blend{}, share{};
};
std::vector<Tap> taps(int from, int to, float start, float length, std::uint32_t texels) {
    std::vector<Tap> out;
    out.reserve(static_cast<std::size_t>(std::max(0, to - from)));
    const float last = static_cast<float>(texels - 1);
    for (int i = from; i < to; ++i) {
        const float low = static_cast<float>(i);
        const float at = std::clamp((low + 0.5f - start) / length * static_cast<float>(texels) - 0.5f, 0.0f, last);
        const float whole = std::floor(at);
        Tap tap;
        tap.first = static_cast<std::uint32_t>(whole);
        tap.second = std::min(tap.first + 1, texels - 1); // (the edge texels are repeated past the edge)
        tap.blend = at - whole;
        tap.share = std::clamp(std::min(low + 1.0f, start + length) - std::max(low, start), 0.0f, 1.0f);
        out.push_back(tap);
    }
    return out;
}
void image(Surface& surface, Pictures& pictures, Picture& picture, float x, float y, float w, float h, float alpha) {
    if (!(w > 0 && h > 0) || !(alpha > 0)) return;
    const auto& whole = picture.levels.front();
    const auto& from = pictures.level(picture, std::min(static_cast<float>(whole.width) / w, static_cast<float>(whole.height) / h));
    const int left = std::clamp(static_cast<int>(std::floor(x)), 0, surface.width), right = std::clamp(static_cast<int>(std::ceil(x + w)), 0, surface.width);
    const int top = std::clamp(static_cast<int>(std::floor(y)), 0, surface.height), bottom = std::clamp(static_cast<int>(std::ceil(y + h)), 0, surface.height);
    if (left >= right || top >= bottom) return;
    const auto columns = taps(left, right, x, w, from.width), rows = taps(top, bottom, y, h, from.height);
    for (int py = top; py < bottom; ++py) {
        const auto& row = rows[static_cast<std::size_t>(py - top)];
        const auto* upper = from.rgba.data() + static_cast<std::size_t>(row.first) * from.width * 4;
        const auto* lower = from.rgba.data() + static_cast<std::size_t>(row.second) * from.width * 4;
        auto* pixel = surface.at(left, py);
        for (int px = left; px < right; ++px, pixel += 4) {
            const auto& column = columns[static_cast<std::size_t>(px - left)];
            const std::uint8_t *a = upper + column.first * 4, *b = upper + column.second * 4, *c = lower + column.first * 4, *d = lower + column.second * 4;
            float texel[4];
            for (int i = 0; i < 4; ++i) {
                const float above = static_cast<float>(a[i]) + (static_cast<float>(b[i]) - static_cast<float>(a[i])) * column.blend;
                const float below = static_cast<float>(c[i]) + (static_cast<float>(d[i]) - static_cast<float>(c[i])) * column.blend;
                texel[i] = above + (below - above) * row.blend;
            }
            over(pixel, texel, alpha * column.share * row.share);
        }
    }
}
void gradient(Surface& surface, const Step& step) {
    if (!(step.y1 > step.y0)) return;
    Colour colour = step.colour;
    colour.a = 1;
    const auto ink = ink_of(colour);
    const int top = std::clamp(static_cast<int>(std::ceil(step.y0 - 0.5f)), 0, surface.height);
    for (int y = top; y < surface.height && static_cast<float>(y) + 0.5f < step.y1; ++y) {
        const float along = (static_cast<float>(y) + 0.5f - step.y0) / (step.y1 - step.y0);
        const float alpha = std::clamp(step.a0 + (step.a1 - step.a0) * along, 0.0f, 1.0f);
        auto* pixel = surface.at(0, y);
        for (int x = 0; x < surface.width; ++x, pixel += 4) over(pixel, ink.data(), alpha);
    }
}

// A closed polygon filled even-odd. Each pixel row is cut into `slices` thin rows; in each, the edges
// crossing its middle are sorted and every other gap between them is inside, counted to the fraction
// of a pixel at both ends. How much of a pixel is inside is the sum over its slices: finer than drawing at twice
// the size and averaging (two rows of two samples).
void polygon(Surface& surface, const std::vector<std::array<float, 2>>& points, const Ink& ink, const Pixels* picture, float texel) {
    constexpr int slices = 8;
    constexpr float weight = 1.0f / slices;
    struct Edge {
        float top, bottom, x, slope; // x at the top, and how far it moves a pixel down
    };
    std::vector<Edge> edges;
    edges.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        auto a = points[i], b = points[(i + 1) % points.size()];
        if (a[1] == b[1]) continue;
        if (a[1] > b[1]) std::swap(a, b);
        edges.push_back({a[1], b[1], a[0], (b[0] - a[0]) / (b[1] - a[1])});
    }
    if (edges.empty()) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.top < b.top; });
    float lowest = edges.front().bottom;
    for (const auto& edge : edges) lowest = std::max(lowest, edge.bottom);
    const int first = std::clamp(static_cast<int>(std::floor(edges.front().top)), 0, surface.height);
    const int last = std::clamp(static_cast<int>(std::ceil(lowest)), 0, surface.height);
    const Tiles tiles(surface, picture, texel);
    const float width = static_cast<float>(surface.width);
    // How much of each pixel of a row is inside: `part` what a span adds to the pixels at its two ends, `run` where the whole
    // pixels between them start (+) and stop (-), summed along the row afterwards.
    std::vector<float> part(static_cast<std::size_t>(surface.width) + 1), run(static_cast<std::size_t>(surface.width) + 2);
    std::vector<std::size_t> active;
    std::vector<float> crossings;
    std::size_t next = 0;
    for (int row = first; row < last; ++row) {
        int low = surface.width, high = -1;
        for (int slice = 0; slice < slices; ++slice) {
            const float y = static_cast<float>(row) + (static_cast<float>(slice) + 0.5f) * weight;
            while (next < edges.size() && edges[next].top <= y) active.push_back(next++);
            crossings.clear();
            for (std::size_t i = 0; i < active.size();) {
                const auto& edge = edges[active[i]];
                if (edge.bottom <= y) { // (an edge counts from its top up to, not at, its bottom: a corner is crossed once)
                    active[i] = active.back();
                    active.pop_back();
                    continue;
                }
                // (Kept on the surface, and 0 for the one that is not a number: an edge next to no height at all.)
                const float x = edge.x + (y - edge.top) * edge.slope;
                crossings.push_back(x >= 0 ? std::min(x, width) : 0.0f);
                ++i;
            }
            std::sort(crossings.begin(), crossings.end());
            for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
                const float from = crossings[i], to = crossings[i + 1];
                if (!(to > from)) continue;
                const int a = static_cast<int>(from), b = static_cast<int>(to);
                if (a == b) {
                    part[static_cast<std::size_t>(a)] += (to - from) * weight;
                } else {
                    part[static_cast<std::size_t>(a)] += (static_cast<float>(a + 1) - from) * weight;
                    run[static_cast<std::size_t>(a) + 1] += weight;
                    run[static_cast<std::size_t>(b)] -= weight;
                    part[static_cast<std::size_t>(b)] += (to - static_cast<float>(b)) * weight;
                }
                low = std::min(low, a);
                high = std::max(high, std::min(b, surface.width - 1));
            }
        }
        if (high < low) continue;
        float whole = 0;
        auto* pixel = surface.at(low, row);
        for (int x = low; x <= high; ++x, pixel += 4) {
            const auto at = static_cast<std::size_t>(x);
            whole += run[at];
            const float inside = std::min(part[at] + whole, 1.0f);
            part[at] = run[at] = 0;
            if (inside < 1.0f / 512.0f) continue;
            if (picture) over(pixel, tiles.at(x, row), inside);
            else over(pixel, ink.data(), inside);
        }
        part[static_cast<std::size_t>(high) + 1] = run[static_cast<std::size_t>(high) + 1] = 0;
    }
}

// A layer laid over the surface: turned by `angle` about its middle (positive clockwise as seen, y
// being down), its middle at (cx, cy). Each pixel of the surface is taken back into the layer and
// sampled between its four texels there, clear past its edge, which is what smooths the edge.
void lay(Surface& surface, const Surface& layer, float cx, float cy, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    const float half_w = static_cast<float>(layer.width) * 0.5f, half_h = static_cast<float>(layer.height) * 0.5f;
    const float reach_x = std::fabs(c) * half_w + std::fabs(s) * half_h + 1.0f, reach_y = std::fabs(s) * half_w + std::fabs(c) * half_h + 1.0f;
    const int left = std::clamp(static_cast<int>(std::floor(cx - reach_x)), 0, surface.width), right = std::clamp(static_cast<int>(std::ceil(cx + reach_x)), 0, surface.width);
    const int top = std::clamp(static_cast<int>(std::floor(cy - reach_y)), 0, surface.height), bottom = std::clamp(static_cast<int>(std::ceil(cy + reach_y)), 0, surface.height);
    static constexpr std::uint8_t clear[4]{};
    const auto texel = [&](int x, int y) { return x < 0 || y < 0 || x >= layer.width || y >= layer.height ? clear : layer.at(x, y); };
    for (int py = top; py < bottom; ++py) {
        auto* pixel = surface.at(left, py);
        const float dy = static_cast<float>(py) + 0.5f - cy;
        for (int px = left; px < right; ++px, pixel += 4) {
            const float dx = static_cast<float>(px) + 0.5f - cx;
            const float lx = c * dx + s * dy + half_w - 0.5f, ly = c * dy - s * dx + half_h - 0.5f;
            if (lx <= -1.0f || ly <= -1.0f || lx >= static_cast<float>(layer.width) || ly >= static_cast<float>(layer.height)) continue;
            const float fx = std::floor(lx), fy = std::floor(ly);
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
            const std::uint8_t *a = texel(x0, y0), *b = texel(x0 + 1, y0), *d = texel(x0, y0 + 1), *e = texel(x0 + 1, y0 + 1);
            if (!(a[3] | b[3] | d[3] | e[3])) continue;
            const float tx = lx - fx, ty = ly - fy;
            float sample[4];
            for (int i = 0; i < 4; ++i) {
                const float above = static_cast<float>(a[i]) + (static_cast<float>(b[i]) - static_cast<float>(a[i])) * tx;
                const float below = static_cast<float>(d[i]) + (static_cast<float>(e[i]) - static_cast<float>(d[i])) * tx;
                sample[i] = above + (below - above) * ty;
            }
            over(pixel, sample, 1.0f);
        }
    }
}

// ---- text: DirectWrite ---------------------------------------------------------------------------------
// A plugin's font is a file of its package that DirectWrite reads from memory, through a factory that
// is ours alone: plugin_font_directwrite.h, which the texts plugins draw on the overlay share. Here a
// font lays out and draws a text step's one line.
using font::Font;
using font::Unknown;
struct Text : font::Library {
    // The one line laid out: its width (with any spaces at its end) and how far its baseline is below
    // its top. A control character counts as a space, so it stays one line.
    ComPtr<IDWriteTextLayout> layout(const Font& font, std::string_view text, float size, float& width, float& baseline) {
        std::wstring wide(text.size(), L'\0');
        const int units = text.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), static_cast<int>(wide.size()));
        wide.resize(static_cast<std::size_t>(std::max(units, 0)));
        for (auto& unit : wide)
            if (unit < 0x20 || unit == 0x7f || unit == 0x85 || unit == 0x2028 || unit == 0x2029) unit = L' ';
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> made;
        DWRITE_TEXT_METRICS metrics{};
        DWRITE_LINE_METRICS line{};
        UINT32 lines{};
        if (FAILED(factory->CreateTextFormat(font.family.c_str(), font.collection.Get(), font.weight, font.style, font.stretch, size, L"", &format)) ||
            FAILED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) ||
            FAILED(factory->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()), format.Get(), 1048576.0f, 1048576.0f, &made)) ||
            FAILED(made->GetMetrics(&metrics)) || FAILED(made->GetLineMetrics(&line, 1, &lines)))
            return nullptr;
        width = metrics.widthIncludingTrailingWhitespace;
        baseline = line.baseline;
        return made;
    }
};
// Two of them, each with a factory of its own and so sharing nothing: one for the pictures being drawn
// (on their thread) and one for measuring (on the plugin's), so a measure never waits for a text that
// is being drawn. Both lay a text out the same way, so they measure it the same. Kept for the process:
// DirectWrite may still hold a font's stream.
Text& drawing() {
    static auto* value = new Text;
    return *value;
}
Text& measuring() {
    static auto* value = new Text;
    return *value;
}

// What a layout draws through: each run of glyphs asked of DirectWrite as grey coverage where it
// meets the surface, and the ink laid over by it. Positions are kept to the fraction of a pixel.
class Glyphs final : public Unknown<IDWriteTextRenderer> {
public:
    Glyphs(Text& text, Surface& surface, const Ink& ink) : text_(text), surface_(surface), ink_(ink) {}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        *transform = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override {
        *pixels = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE measuring, const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown*) override {
        // (Grey coverage, as the glyphs of the texts plugins draw on the overlay are made: Library::coverage.)
        DWRITE_TEXTURE_TYPE kind{};
        const auto analysis = text_.coverage(*run, measuring, x, y, kind);
        RECT bounds{};
        if (!analysis || FAILED(analysis->GetAlphaTextureBounds(kind, &bounds))) return S_OK; // (a run that cannot be drawn is left out)
        bounds.left = std::max<LONG>(bounds.left, 0);
        bounds.top = std::max<LONG>(bounds.top, 0);
        bounds.right = std::min<LONG>(bounds.right, surface_.width);
        bounds.bottom = std::min<LONG>(bounds.bottom, surface_.height);
        if (bounds.left >= bounds.right || bounds.top >= bounds.bottom) return S_OK;
        const std::size_t wide = static_cast<std::size_t>(bounds.right - bounds.left), tall = static_cast<std::size_t>(bounds.bottom - bounds.top);
        const std::size_t each = kind == DWRITE_TEXTURE_ALIASED_1x1 ? 1 : 3;
        greys_.resize(wide * tall * each);
        if (FAILED(analysis->CreateAlphaTexture(kind, &bounds, greys_.data(), static_cast<UINT32>(greys_.size())))) return S_OK;
        for (std::size_t row = 0; row < tall; ++row) {
            const auto* from = greys_.data() + row * wide * each;
            auto* pixel = surface_.at(static_cast<int>(bounds.left), static_cast<int>(bounds.top) + static_cast<int>(row));
            for (std::size_t column = 0; column < wide; ++column, from += each, pixel += 4) {
                const int grey = each == 1 ? from[0] : (from[0] + from[1] + from[2] + 1) / 3;
                if (grey) over(pixel, ink_.data(), static_cast<float>(grey) * (1.0f / 255.0f));
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

private:
    Text& text_;
    Surface& surface_;
    Ink ink_;
    std::vector<std::uint8_t> greys_;
};
void text(Surface& surface, const Step& step) {
    if (step.font.name.empty() || step.text.empty() || !(step.size > 0)) return;
    auto& state = drawing();
    std::lock_guard lock(state.mutex);
    const auto* font = state.font_of(step.font.file);
    if (!font) return;
    float width{}, baseline{};
    const auto layout = state.layout(*font, step.text, step.size, width, baseline);
    if (!layout) return;
    const float left = step.align == Align::center ? step.x - width * 0.5f : step.align == Align::right ? step.x - width : step.x;
    ComPtr<Glyphs> glyphs;
    glyphs.Attach(new Glyphs(state, surface, ink_of(step.colour)));
    layout->Draw(nullptr, glyphs.Get(), left, step.y - baseline);
}

// ---- a request ---------------------------------------------------------------------------------------
// Why a request is not one to draw (the API checks the same before the game sees it): a size, a
// count or a number out of range. Empty for one that is.
struct Counts {
    int steps{}, points{};
};
std::string problem(const std::vector<Step>& steps, bool layer, Counts& counts) {
    const auto number = [](float value) { return std::isfinite(value) && std::fabs(value) <= most_coordinate; };
    for (const auto& step : steps) {
        if (++counts.steps > most_steps) return "a picture has at most 256 steps";
        for (const float value : {step.texel, step.x, step.y, step.w, step.h, step.focus, step.alpha, step.y0, step.y1, step.a0, step.a1, step.angle, step.size})
            if (!number(value)) return "a step's number is not finite, or is past a million";
        for (const auto& point : step.points)
            if (!number(point[0]) || !number(point[1])) return "a point is not finite, or is past a million";
        if (layer && step.kind != Step::Kind::poly && step.kind != Step::Kind::text) return "a layer's steps are polygons and texts";
        if ((step.kind == Step::Kind::tile || step.kind == Step::Kind::poly) && !(step.texel > 0)) return "texel is more than 0";
        if (step.kind == Step::Kind::poly) {
            if (step.points.size() < 3) return "a polygon has at least 3 points";
            counts.points += static_cast<int>(std::min<std::size_t>(step.points.size(), most_points + 1));
            if (counts.points > most_points) return "a picture's polygons have at most 20000 points";
        }
        if (step.kind == Step::Kind::text && step.text.size() > most_text_bytes) return "a text is at most 512 bytes";
        if (step.kind == Step::Kind::text && step.size > static_cast<float>(most_layer_side)) return "a text's size is at most 4096 pixels";
        if (step.kind == Step::Kind::layer) {
            const float most = static_cast<float>(most_layer_side);
            if (!(step.w >= 1 && step.w <= most && step.h >= 1 && step.h <= most)) return "a layer is 1 to 4096 pixels wide and high";
            if (auto why = problem(step.steps, true, counts); !why.empty()) return why;
        }
    }
    return {};
}
void draw(Surface& surface, const std::vector<Step>& steps, Pictures& pictures) {
    for (const auto& step : steps) {
        switch (step.kind) {
        case Step::Kind::fill:
            fill(surface, ink_of(step.colour));
            break;
        case Step::Kind::tile:
            if (auto* picture = pictures.get(step.picture)) tile(surface, picture->levels.front(), step.texel);
            break;
        case Step::Kind::image:
            if (auto* picture = pictures.get(step.picture)) {
                if (!step.cover) {
                    image(surface, pictures, *picture, step.x, step.y, step.w, step.h, std::clamp(step.alpha, 0.0f, 1.0f));
                    break;
                }
                // Filling the surface with its shape kept: the larger of the two scales, the overhang
                // shared out sideways by the focus and evenly above and below.
                const float pw = static_cast<float>(picture->levels.front().width), ph = static_cast<float>(picture->levels.front().height);
                const float scale = std::max(static_cast<float>(surface.width) / pw, static_cast<float>(surface.height) / ph);
                const float w = pw * scale, h = ph * scale;
                image(surface, pictures, *picture, (static_cast<float>(surface.width) - w) * std::clamp(step.focus, 0.0f, 1.0f),
                      (static_cast<float>(surface.height) - h) * 0.5f, w, h, std::clamp(step.alpha, 0.0f, 1.0f));
            }
            break;
        case Step::Kind::gradient:
            gradient(surface, step);
            break;
        case Step::Kind::layer: {
            Surface layer(static_cast<int>(std::lround(step.w)), static_cast<int>(std::lround(step.h)), false);
            draw(layer, step.steps, pictures);
            lay(surface, layer, step.x, step.y, step.angle);
            break;
        }
        case Step::Kind::poly: {
            auto* picture = pictures.get(step.picture);
            polygon(surface, step.points, ink_of(step.colour), picture ? &picture->levels.front() : nullptr, step.texel);
            break;
        }
        case Step::Kind::text:
            text(surface, step);
            break;
        }
    }
}
// The canvas as a PNG without an alpha channel: it is opaque, so every reader gets alpha 255.
std::string encode(IWICImagingFactory* factory, const Surface& surface, std::string& png) {
    static constexpr char failed[] = "the picture could not be encoded";
    const auto width = static_cast<UINT>(surface.width), height = static_cast<UINT>(surface.height);
    std::vector<std::uint8_t> rows(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0, count = static_cast<std::size_t>(width) * height; i < count; ++i) {
        rows[i * 3] = surface.pixels[i * 4 + 2];
        rows[i * 3 + 1] = surface.pixels[i * 4 + 1];
        rows[i * 3 + 2] = surface.pixels[i * 4];
    }
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    STATSTG stat{};
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)) ||
        format != GUID_WICPixelFormat24bppBGR || FAILED(frame->WritePixels(height, width * 3, static_cast<UINT>(rows.size()), rows.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()) || FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart <= 0 ||
        stat.cbSize.QuadPart > (256ll << 20))
        return failed;
    png.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
    ULONG read{};
    if (FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr)) || FAILED(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read)) || read != png.size())
        return failed;
    return {};
}
}

bool decode(std::string_view bytes, Pixels& out) {
    const Com com;
    return decode_with(imaging().Get(), bytes, out, true);
}
std::optional<std::pair<std::uint32_t, std::uint32_t>> size_of(const std::filesystem::path& file) {
    try {
        const auto bytes = read_file(file, most_picture_bytes);
        const Com com;
        Pixels pixels;
        if (!decode_with(imaging().Get(), bytes, pixels, false)) return std::nullopt;
        return std::pair{pixels.width, pixels.height};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
std::optional<double> text_width(const std::filesystem::path& font, std::string_view text, float size) {
    try {
        if (!(size > 0) || !std::isfinite(size) || text.size() > most_text_bytes) return std::nullopt;
        auto& state = measuring();
        std::lock_guard lock(state.mutex);
        const auto* found = state.font_of(font);
        if (!found) return std::nullopt;
        if (text.empty()) return 0.0;
        float width{}, baseline{};
        if (!state.layout(*found, text, size, width, baseline)) return std::nullopt;
        return static_cast<double>(width);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
std::string render(const Request& request, std::string& png) {
    try {
        if (request.width < 1 || request.width > most_side || request.height < 1 || request.height > most_side) return "a picture is 1 to 2048 pixels wide and high";
        Counts counts;
        if (auto why = problem(request.steps, false, counts); !why.empty()) return why;
        // (A canvas, a layer and the pictures a request names can be hundreds of megabytes for a while.)
        static std::mutex one_at_a_time;
        std::lock_guard lock(one_at_a_time);
        const Com com;
        const auto factory = imaging();
        if (!factory) return "Windows' picture codecs are not available";
        Surface canvas(request.width, request.height, true);
        Pictures pictures(factory.Get());
        draw(canvas, request.steps, pictures);
        return encode(factory.Get(), canvas, png);
    } catch (const std::bad_alloc&) {
        return "there is not enough memory for it";
    } catch (const std::exception&) {
        return "it could not be drawn";
    }
}
std::string make(const Request& request, const std::filesystem::path& file) {
    try {
        std::string png;
        if (auto why = render(request, png); !why.empty()) return why;
        auto temporary = file;
        temporary += L".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out || !out.write(png.data(), static_cast<std::streamsize>(png.size())) || !out.flush()) {
                out.close();
                std::error_code error;
                std::filesystem::remove(temporary, error);
                return "the file cannot be written";
            }
        }
        // (In place of the picture there, in one step: a reader sees the old one or the new one.)
        if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code error;
            std::filesystem::remove(temporary, error);
            return "the file cannot be written";
        }
        return {};
    } catch (const std::exception&) {
        return "the file cannot be written";
    }
}
void make_later(Request request, std::filesystem::path file, std::shared_ptr<Job> job) {
    try {
        // (The thread holds only what it was given; pictures are drawn one at a time all the same.)
        std::thread([request = std::move(request), file = std::move(file), job] {
            job->why = make(request, file);
            job->done.store(true, std::memory_order_release);
        }).detach();
    } catch (const std::system_error&) {
        job->why = "a thread could not be started for it";
        job->done.store(true, std::memory_order_release);
    }
}
}
