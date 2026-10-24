#include "capture_matte.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dingosdk::capture {
Backgrounds measure_backgrounds(const PixelView& dark, const PixelView& light, Rect area) noexcept {
    // The median of each channel along the edge: a stray UI element on it does not shift it.
    Backgrounds out;
    if (area.empty()) return out;
    std::array<std::array<std::uint32_t, 256>, 6> counts{};
    std::uint64_t total = 0;
    const auto add = [&](std::uint32_t x, std::uint32_t y) {
        const auto d = dark.rgb(x, y), l = light.rgb(x, y);
        for (std::size_t c = 0; c < 3; ++c) ++counts[c][d[c]], ++counts[3 + c][l[c]];
        ++total;
    };
    const auto right = area.x + area.width - 1, bottom = area.y + area.height - 1;
    for (auto x = area.x; x <= right; ++x) add(x, area.y), add(x, bottom);
    for (auto y = area.y + 1; y < bottom; ++y) add(area.x, y), add(right, y);
    for (std::size_t k = 0; k < 6; ++k) {
        std::uint64_t seen = 0;
        std::size_t value = 0;
        while (value < 255 && (seen += counts[k][value]) * 2 < total) ++value;
        (k < 3 ? out.dark : out.light)[k % 3] = static_cast<float>(value);
    }
    return out;
}

Image matte_pair(const PixelView& dark, const PixelView& light, Rect area, const Backgrounds& backgrounds,
                 float floor) {
    Image out(area.width, area.height);
    std::array<float, 3> step{};
    float step_power = 0;
    for (int c = 0; c < 3; ++c) {
        step[c] = backgrounds.light[c] - backgrounds.dark[c];
        step_power += step[c] * step[c];
    }
    // Backgrounds that look the same tell nothing apart: everything is taken as opaque.
    const bool usable = step_power > 3 * 16.0f * 16.0f;
    floor = std::clamp(floor, 0.0f, 0.45f);
    for (std::uint32_t y = 0; y < area.height; ++y)
        for (std::uint32_t x = 0; x < area.width; ++x) {
            const auto d = dark.rgb(area.x + x, area.y + y), l = light.rgb(area.x + x, area.y + y);
            auto* p = out.at(x, y);
            float seen = 0; // how much of the background shows: 0 opaque .. 1 empty
            if (usable) {
                // Least squares over the three channels of (light - dark) = seen * step.
                float dot = 0;
                for (int c = 0; c < 3; ++c) dot += (static_cast<float>(l[c]) - d[c]) * step[c];
                seen = std::clamp((dot / step_power - floor) / (1.0f - 2.0f * floor), 0.0f, 1.0f);
            }
            const float alpha = 1.0f - seen;
            if (alpha <= 0.0f) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
            for (int c = 0; c < 3; ++c) {
                // The board's own colour, from both captures with their background taken away.
                const float a = d[c] - seen * backgrounds.dark[c], b = l[c] - seen * backgrounds.light[c];
                p[c] = static_cast<std::uint8_t>(std::clamp(std::lround((a + b) * 0.5f / alpha), 0L, 255L));
            }
            p[3] = static_cast<std::uint8_t>(std::lround(alpha * 255.0f));
        }
    return out;
}
// Without a background of its own to swap, the board is keyed out against the backdrop, found from
// the turn itself. The backdrop keeps still and, across any one row, changes only gently (the Board
// screen's vignette); the board covers part of each row at most. So:
//  1. each pixel's steady colour over the turn (the one it keeps most of the time) is a candidate;
//  2. along each row, a candidate is the backdrop when it is close to the row's typical candidate:
//     one the board covers for most of the turn (a white deck, a white grip in the middle) has a
//     board colour for its steady one, and is not;
//  3. where a row has no backdrop candidate the backdrop is carried across from those either side;
//  4. the backdrop at a pixel is then the colour it shows, in any frame, close to that estimate.
// An exact backdrop lets the key be tight, so a black grip against the dark backdrop still reads as
// board. Matting (a stage that swaps the background) is better still; this is for when there is none.
void key_against_backdrop(std::vector<Image>& frames, bool solid_rows) {
    if (frames.empty()) return;
    const auto raw = frames;
    const auto w = frames.front().width, h = frames.front().height;
    if (w < 8 || h < 8) return;
    const auto luma = [](const std::uint8_t* p) { return (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8; };
    Image plate(w, h);
    std::vector<int> values(frames.size()), sorted(frames.size());
    std::vector<std::array<float, 3>> candidate(w);
    std::vector<bool> has(w), backdrop(w);
    std::vector<std::array<float, 3>> previous_row;
    for (std::uint32_t y = 0; y < h; ++y) {
        // 1. Steady colours along the row.
        for (std::uint32_t x = 0; x < w; ++x) {
            for (std::size_t f = 0; f < frames.size(); ++f) values[f] = luma(frames[f].at(x, y));
            sorted = values;
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
            const int middle = sorted[sorted.size() / 2];
            std::array<double, 3> sum{};
            std::size_t steady = 0;
            for (std::size_t f = 0; f < frames.size(); ++f)
                if (std::abs(values[f] - middle) <= 4) {
                    ++steady;
                    for (std::size_t c = 0; c < 3; ++c) sum[c] += frames[f].at(x, y)[c];
                }
            has[x] = steady * 100 >= frames.size() * 35;
            for (std::size_t c = 0; c < 3; ++c) candidate[x][c] = has[x] ? static_cast<float>(sum[c] / static_cast<double>(steady)) : 0.0f;
        }
        // 2. The row's typical candidate, and those close to it. Close to it locally too: compared
        // with the median of the candidates within half the row each way, so a vignette's slope across
        // the row is followed.
        const std::uint32_t reach = std::max<std::uint32_t>(4, w / 2);
        std::array<std::vector<float>, 3> window;
        for (std::uint32_t x = 0; x < w; ++x) {
            backdrop[x] = false;
            if (!has[x]) continue;
            const auto from = x > reach ? x - reach : 0u, to = std::min(w, x + reach + 1);
            for (auto& v : window) v.clear();
            for (auto k = from; k < to; ++k)
                if (has[k])
                    for (std::size_t c = 0; c < 3; ++c) window[c].push_back(candidate[k][c]);
            float off = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                auto& v = window[c];
                std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
                off = std::max(off, std::abs(candidate[x][c] - v[v.size() / 2]));
            }
            backdrop[x] = off <= 12.0f;
        }
        // 3. Carried across the gaps from the backdrop either side (or from the row above).
        std::vector<std::array<float, 3>> estimate(w);
        std::int64_t left = -1;
        for (std::uint32_t x = 0; x <= w; ++x) {
            if (x < w && !backdrop[x]) continue;
            const auto right = static_cast<std::int64_t>(x);
            for (auto k = left + 1; k < right; ++k) {
                const auto i = static_cast<std::size_t>(k);
                if (left < 0 && right >= static_cast<std::int64_t>(w)) {
                    estimate[i] = previous_row.empty() ? std::array<float, 3>{} : previous_row[i];
                } else if (left < 0) {
                    estimate[i] = candidate[static_cast<std::size_t>(right)];
                } else if (right >= static_cast<std::int64_t>(w)) {
                    estimate[i] = candidate[static_cast<std::size_t>(left)];
                } else {
                    const float t = static_cast<float>(k - left) / static_cast<float>(right - left);
                    for (std::size_t c = 0; c < 3; ++c)
                        estimate[i][c] = candidate[static_cast<std::size_t>(left)][c] +
                                         (candidate[static_cast<std::size_t>(right)][c] - candidate[static_cast<std::size_t>(left)][c]) * t;
                }
            }
            if (x < w) estimate[x] = candidate[x], left = right;
        }
        previous_row = estimate;
        // 4. What each pixel shows that is close to its estimate: the backdrop, exactly.
        for (std::uint32_t x = 0; x < w; ++x) {
            std::array<double, 3> sum{};
            std::size_t fits = 0;
            for (const auto& frame : frames) {
                const auto* p = frame.at(x, y);
                float off = 0;
                for (std::size_t c = 0; c < 3; ++c) off = std::max(off, std::abs(static_cast<float>(p[c]) - estimate[x][c]));
                if (off > 8.0f) continue;
                ++fits;
                for (std::size_t c = 0; c < 3; ++c) sum[c] += p[c];
            }
            auto* b = plate.at(x, y);
            for (std::size_t c = 0; c < 3; ++c)
                b[c] = static_cast<std::uint8_t>(std::clamp(std::lround(fits >= 2 ? sum[c] / static_cast<double>(fits) : estimate[x][c]), 0L, 255L));
        }
    }
    for (auto& frame : frames)
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                auto* p = frame.at(x, y);
                const auto* b = plate.at(x, y);
                int distance = 0;
                for (std::size_t c = 0; c < 3; ++c) distance = std::max(distance, std::abs(p[c] - b[c]));
                const float alpha = std::clamp((static_cast<float>(distance) - 3.0f) / 9.0f, 0.0f, 1.0f);
                if (alpha <= 0) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
                for (std::size_t c = 0; c < 3; ++c)
                    p[c] = static_cast<std::uint8_t>(std::clamp(std::lround((p[c] - (1 - alpha) * b[c]) / alpha), 0L, 255L));
                p[3] = static_cast<std::uint8_t>(std::lround(alpha * 255));
            }
    // Part of what turns can match the backdrop and key out as a hole in it: a dark joint or shirt,
    // part of a deck's graphic. A pocket it closes all round is filled from the frame. A board is
    // solid, so any pocket in it is board; a skater's bigger pockets are real backdrop (between an
    // arm and the body). (Not filled across each row: the backdrop between a deck and its wheels is
    // open above and below the truck, and stays clear.)
    for (std::size_t f = 0; f < frames.size(); ++f) {
        auto& frame = frames[f];
        const std::size_t n = std::size_t{w} * h;
        std::vector<std::uint8_t> outside(n);
        std::vector<std::uint32_t> todo;
        std::size_t opaque = 0;
        const auto clear = [&](std::size_t p) { return frame.rgba[p * 4 + 3] < 128; };
        for (std::size_t p = 0; p < n; ++p) {
            if (!clear(p)) { ++opaque; continue; }
            const auto x = p % w, y = p / w;
            if (x == 0 || y == 0 || x + 1 == w || y + 1 == h) outside[p] = 1, todo.push_back(static_cast<std::uint32_t>(p));
        }
        const auto spread = [&](std::vector<std::uint8_t>& mark, std::vector<std::uint32_t>& stack, std::vector<std::uint32_t>* region) {
            while (!stack.empty()) {
                const auto p = stack.back();
                stack.pop_back();
                if (region) region->push_back(p);
                const auto x = p % w, y = p / w;
                const std::uint32_t next[4] = {x > 0 ? p - 1 : p, x + 1 < w ? p + 1 : p, y > 0 ? p - w : p, y + 1 < h ? p + w : p};
                for (const auto q : next)
                    if (!mark[q] && clear(q)) mark[q] = 1, stack.push_back(q);
            }
        };
        spread(outside, todo, nullptr);
        const auto pocket_most = std::max<std::size_t>(16, solid_rows ? opaque / 2 : opaque * 3 / 100);
        std::vector<std::uint32_t> region;
        for (std::size_t p = 0; p < n; ++p) {
            if (outside[p] || !clear(p)) continue;
            region.clear();
            outside[p] = 1;
            todo.push_back(static_cast<std::uint32_t>(p));
            spread(outside, todo, &region);
            if (region.size() > pocket_most) continue;
            for (const auto q : region) {
                auto* px = frame.rgba.data() + std::size_t{q} * 4;
                std::copy_n(raw[f].rgba.data() + std::size_t{q} * 4, 3, px);
                px[3] = 255;
            }
        }
    }
}

} // namespace dingosdk::capture
