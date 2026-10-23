#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Torn paper for the Animator Studio MARKET (the shop's labels, its bands across the page, the covers' banners): the shape
// of a piece of paper torn off at both ends, or of a band torn along its top and its foot. Kept so it can be drawn as a
// strip of quads (each column's top and foot) and a fan at each end, which is how the studio draws a textured shape, and
// read as one outline by the cover maker, which fills it on the CPU. Seeded: a label keeps its tear frame to frame.
namespace dingosdk::overlay::torn {
struct Point {
    float x{}, y{};
};
struct Paper {
    std::vector<Point> top, foot; // the same columns, left to right: each one's top and its foot
    std::vector<Point> left, right; // the ragged ends, from the top down (empty for a band)
    // Clockwise all round: the top, the right end, the foot back, the left end up.
    std::vector<Point> outline() const {
        std::vector<Point> out(top);
        out.insert(out.end(), right.begin(), right.end());
        out.insert(out.end(), foot.rbegin(), foot.rend());
        out.insert(out.end(), left.rbegin(), left.rend());
        return out;
    }
};
struct Random {
    std::uint32_t s;
    explicit Random(std::uint32_t seed) : s(seed * 2654435761u ? seed * 2654435761u : 1u) {}
    float operator()() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s) / 4294967296.0f;
    }
};
// A slow wander along an edge: two waves, their phases this piece's own.
struct Wander {
    float amp, period, a, b;
    float operator()(float x) const { return amp * (std::sin(x / period + a) * 0.6f + std::sin(x / (period * 0.37f) + b) * 0.4f); }
};
// A piece w x h torn off at both ends: its long edges wander and nick, its ends are ragged. `edge` is how far the long
// edges stray, `ends` the ends, `step` how far apart the tear's points are.
inline Paper piece(float w, float h, std::uint32_t seed, float edge = 3.2f, float ends = 9, float step = 7) {
    Random r(seed);
    const Wander wt{edge * 1.2f, 70, r() * 6.28f, r() * 6.28f}, wb{edge * 1.2f, 64, r() * 6.28f, r() * 6.28f};
    const Wander wl{ends * 0.5f, 22, r() * 6.28f, r() * 6.28f}, wr{ends * 0.5f, 20, r() * 6.28f, r() * 6.28f};
    Paper p;
    // The columns run from just inside one end to just inside the other; the ends tear out past them.
    const float x0 = ends * 0.6f, x1 = std::max(x0 + 1, w - ends * 0.6f);
    const auto nick = [&] { return (r() - 0.5f) * edge * (r() < 0.08f ? 4.0f : 1.2f); };
    for (float x = x0;; x += step * (0.55f + r() * 0.9f)) {
        const float cx = std::min(x, x1);
        p.top.push_back({cx, wt(cx) + nick()});
        p.foot.push_back({cx, h + wb(cx) + nick()});
        if (cx >= x1) break;
    }
    const float top_l = p.top.front().y, foot_l = p.foot.front().y, top_r = p.top.back().y, foot_r = p.foot.back().y;
    for (float y = top_l + step * 0.5f; y < foot_l - step * 0.3f; y += step * (0.45f + r() * 0.7f))
        p.left.push_back({x0 - (ends * 0.25f + std::abs(wl(y) + (r() - 0.5f) * ends * (r() < 0.15f ? 1.6f : 0.8f))), y});
    for (float y = top_r + step * 0.5f; y < foot_r - step * 0.3f; y += step * (0.45f + r() * 0.7f))
        p.right.push_back({x1 + (ends * 0.25f + std::abs(wr(y) + (r() - 0.5f) * ends * (r() < 0.15f ? 1.6f : 0.8f))), y});
    return p;
}
// A band across the page, `w` wide (and a little past both sides), torn along its top and its foot.
inline Paper band(float w, float h, std::uint32_t seed, float edge = 14) {
    Random r(seed);
    const auto wave = [&](float amp, float period) {
        const float a = r() * 6.28f, b = r() * 6.28f, c = r() * 6.28f;
        return [=](float x) {
            return amp * (std::sin(x / period + a) * 0.55f + std::sin(x / (period * 0.43f) + b) * 0.3f + std::sin(x / (period * 0.13f) + c) * 0.15f);
        };
    };
    const auto top = wave(edge, 140), foot = wave(edge, 120);
    Paper p;
    for (float x = -20;; x += 5 + r() * 9) {
        const float cx = std::min(x, w + 20);
        p.top.push_back({cx, top(cx) + (r() - 0.5f) * 7 + (r() < 0.05f ? -r() * 14 : 0)});
        p.foot.push_back({cx, h + foot(cx) + (r() - 0.5f) * 7 + (r() < 0.05f ? r() * 14 : 0)});
        if (cx >= w + 20) break;
    }
    return p;
}
// The white core of torn paper showing along its edge: the piece pushed out a little, more in places.
inline Paper rim(const Paper& p, std::uint32_t seed, float most = 3.2f) {
    Random r(seed + 99);
    const auto out = [&] { return 0.6f + r() * most * (r() < 0.2f ? 1.8f : 1.0f); };
    Paper q = p;
    for (auto& v : q.top) v.y -= out();
    for (auto& v : q.foot) v.y += out();
    for (auto& v : q.left) v.x -= out();
    for (auto& v : q.right) v.x += out();
    return q;
}
} // namespace dingosdk::overlay::torn
