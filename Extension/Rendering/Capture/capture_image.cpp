#include "capture_image.h"
#include <cmath>

namespace dingosdk::capture {
namespace {
// Per-pixel weights of one axis of an area (box) resample from `from` to `to` samples.
struct Tap { std::uint32_t first{}; std::vector<float> weights; };
std::vector<Tap> area_taps(std::uint32_t from, std::uint32_t to) {
    std::vector<Tap> taps(to);
    const double scale = static_cast<double>(from) / to;
    for (std::uint32_t i = 0; i < to; ++i) {
        auto& tap = taps[i];
        if (scale <= 1.0) {
            // Enlarging: linear between the two nearest source samples.
            const double centre = (i + 0.5) * scale - 0.5;
            const double floor = std::floor(centre);
            const auto left = static_cast<std::int64_t>(floor);
            const auto t = static_cast<float>(centre - floor);
            const auto a = static_cast<std::uint32_t>(std::clamp<std::int64_t>(left, 0, from - 1));
            const auto b = static_cast<std::uint32_t>(std::clamp<std::int64_t>(left + 1, 0, from - 1));
            tap.first = a;
            tap.weights.assign(b - a + 1, 0.0f);
            tap.weights[0] += 1.0f - t;
            tap.weights[b - a] += t;
            continue;
        }
        const double start = i * scale, end = (i + 1) * scale;
        tap.first = static_cast<std::uint32_t>(std::floor(start));
        const auto last = std::min<std::uint32_t>(from - 1, static_cast<std::uint32_t>(std::ceil(end)) - 1);
        float total = 0;
        for (auto s = tap.first; s <= last; ++s) {
            const auto w = static_cast<float>(std::min<double>(end, s + 1.0) - std::max<double>(start, s));
            tap.weights.push_back(w);
            total += w;
        }
        for (auto& w : tap.weights) w /= total;
    }
    return taps;
}
} // namespace

Image over_matte(const Image& image, Rgb matte) {
    Image out = image;
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        const unsigned a = out.rgba[i + 3];
        for (int c = 0; c < 3; ++c)
            out.rgba[i + c] = static_cast<std::uint8_t>((out.rgba[i + c] * a + matte[c] * (255 - a) + 127) / 255);
        out.rgba[i + 3] = 255;
    }
    return out;
}

Rect content_bounds(const Image& image, std::uint8_t threshold) {
    std::uint32_t x0 = image.width, y0 = image.height, x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x)
            if (image.at(x, y)[3] >= threshold) {
                x0 = std::min(x0, x), y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
            }
    if (x0 >= x1 || y0 >= y1) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

Rect united(Rect a, Rect b) noexcept {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const auto x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const auto x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

Rect padded_square(Rect rect, std::uint32_t margin, std::uint32_t width, std::uint32_t height) noexcept {
    if (rect.empty() || !width || !height) return {};
    const auto side = std::min({std::max(rect.width, rect.height) + margin * 2, width, height});
    const auto centre_x = rect.x + rect.width / 2, centre_y = rect.y + rect.height / 2;
    const auto place = [side](std::uint32_t centre, std::uint32_t limit) {
        const auto half = side / 2;
        const auto start = centre > half ? centre - half : 0u;
        return std::min(start, limit - side);
    };
    return {place(centre_x, width), place(centre_y, height), side, side};
}

Image square_around(const Image& image, Rect content, std::uint32_t margin) {
    if (content.empty()) return {};
    const auto side = std::max(content.width, content.height) + margin * 2;
    // The square's corner, which may lie outside the image (left of it, above it).
    const auto x0 = static_cast<std::int64_t>(content.x) + static_cast<std::int64_t>(content.width / 2) - static_cast<std::int64_t>(side / 2);
    const auto y0 = static_cast<std::int64_t>(content.y) + static_cast<std::int64_t>(content.height / 2) - static_cast<std::int64_t>(side / 2);
    Image out(side, side);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            const auto sx = x0 + x, sy = y0 + y;
            if (sx >= 0 && sy >= 0 && sx < image.width && sy < image.height)
                std::copy_n(image.at(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy)), 4, out.at(x, y));
        }
    return out;
}

Image cropped(const Image& image, Rect rect) {
    Image out(rect.width, rect.height);
    for (std::uint32_t y = 0; y < rect.height; ++y)
        for (std::uint32_t x = 0; x < rect.width; ++x) {
            const auto sx = rect.x + x, sy = rect.y + y;
            if (sx < image.width && sy < image.height) std::copy_n(image.at(sx, sy), 4, out.at(x, y));
        }
    return out;
}

Image resized(const Image& image, std::uint32_t w, std::uint32_t h) {
    if (image.empty() || !w || !h) return {};
    const auto columns = area_taps(image.width, w), rows = area_taps(image.height, h);
    // Horizontal pass into premultiplied floats, then vertical.
    std::vector<float> wide(std::size_t{w} * image.height * 4);
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto& tap = columns[x];
            float sum[4]{};
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const auto* p = image.at(tap.first + static_cast<std::uint32_t>(k), y);
                const float a = p[3] / 255.0f, weight = tap.weights[k];
                for (int c = 0; c < 3; ++c) sum[c] += p[c] * a * weight;
                sum[3] += p[3] * weight;
            }
            std::copy_n(sum, 4, wide.data() + (std::size_t{y} * w + x) * 4);
        }
    Image out(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto& tap = rows[y];
            float sum[4]{};
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const auto* p = wide.data() + ((tap.first + k) * w + x) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += p[c] * tap.weights[k];
            }
            auto* d = out.at(x, y);
            const float alpha = std::clamp(sum[3], 0.0f, 255.0f);
            d[3] = static_cast<std::uint8_t>(std::lround(alpha));
            for (int c = 0; c < 3; ++c)
                d[c] = d[3] ? static_cast<std::uint8_t>(std::clamp(std::lround(sum[c] * 255.0f / alpha), 0L, 255L)) : 0;
        }
    return out;
}

Image fitted(const Image& image, std::uint32_t size) {
    if (image.empty() || !size) return {};
    const double scale = static_cast<double>(size) / std::max(image.width, image.height);
    const auto w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(image.width * scale)));
    const auto h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(image.height * scale)));
    const auto small = resized(image, w, h);
    Image out(size, size);
    const auto left = (size - w) / 2, top = (size - h) / 2;
    for (std::uint32_t y = 0; y < h; ++y) std::copy_n(small.at(0, y), std::size_t{w} * 4, out.at(left, top + y));
    return out;
}

Image side_by_side(const Image& a, const Image& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    Image out(a.width + b.width, std::max(a.height, b.height));
    const auto place = [&out](const Image& part, std::uint32_t left) {
        const auto top = (out.height - part.height) / 2;
        for (std::uint32_t y = 0; y < part.height; ++y)
            std::copy_n(part.at(0, y), std::size_t{part.width} * 4, out.at(left, top + y));
    };
    place(a, 0);
    place(b, a.width);
    return out;
}
} // namespace dingosdk::capture
