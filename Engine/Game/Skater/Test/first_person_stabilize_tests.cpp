#include "Engine/Game/Skater/first_person_spring.h"

#include <cstdio>
#include <string_view>

// True first person (first_person::stabilize): animated head wobble in, a level and
// steady view out, while spins, flips and teleports still come through.
namespace {
using namespace dingosdk::first_person;
constexpr float pi = std::numbers::pi_v<float>, deg = pi / 180;
int failures = 0;

void expect(bool condition, std::string_view what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
}
// A head in the game's row layout (right, up, backward, position).
Matrix head_at(float yaw, float pitch, float roll, Vec3 position) {
    Matrix m{};
    m[15] = 1;
    write_level(m, yaw, pitch, roll);
    for (unsigned i = 0; i < 3; ++i) m[12 + i] = position[i];
    return m;
}
struct Angles { float yaw, pitch, roll; };
Angles angles_of(const Matrix& m) {
    const Vec3 forward{-m[8], -m[9], -m[10]};
    Angles a{std::atan2(forward[0], forward[2]), std::asin(std::clamp(forward[1], -1.0f, 1.0f)), 0};
    Matrix level{};
    write_level(level, a.yaw, a.pitch, 0);
    const Vec3 right{m[0], m[1], m[2]};
    a.roll = std::atan2(dot(right, {level[4], level[5], level[6]}), dot(right, {level[0], level[1], level[2]}));
    return a;
}

void frame_layout() {
    const auto a = angles_of(head_at(0.7f, -0.3f, 0.2f, {}));
    expect(std::abs(a.yaw - 0.7f) < 1e-4f && std::abs(a.pitch + 0.3f) < 1e-4f && std::abs(a.roll - 0.2f) < 1e-4f,
        "write_level round trips yaw, pitch and roll");
    const auto m = head_at(0.7f, -0.3f, 0.2f, {});
    const auto backward = cross({m[0], m[1], m[2]}, {m[4], m[5], m[6]});
    expect(std::abs(backward[0] - m[8]) + std::abs(backward[1] - m[9]) + std::abs(backward[2] - m[10]) < 1e-4f,
        "right x up = backward, as the game's camera rows");
}

// Cruising straight with the head nodding, tilting, swaying and bobbing.
void wobble() {
    const Settings settings;
    Spring spring;
    float roll{}, nod{}, sway{}, bob{};
    double t = 0;
    for (int i = 0; i < 600; ++i, t += 1.0 / 60) {
        const auto w = static_cast<float>(t);
        const Vec3 origin{w * 5, 0, 0};
        const auto head = head_at(1.0f + 6 * deg * std::sin(2 * pi * 2.0f * w),
            -10 * deg + 12 * deg * std::sin(2 * pi * 2.5f * w), 10 * deg * std::sin(2 * pi * 3.0f * w),
            {origin[0], 1.6f + 0.04f * std::sin(2 * pi * 2 * w), 0});
        const auto view = update(spring, head, settings, t, origin);
        if (i < 120) continue;
        const auto a = angles_of(view);
        roll = std::max(roll, std::abs(a.roll));
        nod = std::max(nod, std::abs(a.pitch + 10 * deg * settings.head_pitch * 0.01f));
        sway = std::max(sway, std::abs(wrap_angle(a.yaw - 1.0f)));
        bob = std::max(bob, std::abs(view[13] - 1.6f));
    }
    expect(roll < 0.01f * deg, "head tilt 0% keeps the horizon level");
    expect(nod < 12 * deg * settings.head_pitch * 0.01f * 0.5f, "the kept head nod loses over half its wobble");
    expect(sway < 3 * deg, "side-to-side sway loses half its wobble");
    expect(bob < 0.025f, "head bob is reduced");
}

void spin() {
    const Settings settings;
    Spring spring;
    float lag{};
    double t = 0;
    for (int i = 0; i < 120; ++i, t += 1.0 / 60) {
        const float yaw = 4 * pi * static_cast<float>(t); // two turns a second
        const auto view = update(spring, head_at(wrap_angle(yaw), -5 * deg, 0, {0, 1.6f, 0}), settings, t, {});
        if (i > 30) lag = std::max(lag, std::abs(wrap_angle(angles_of(view).yaw - yaw)));
    }
    expect(lag < 30 * deg, "a 720 keeps the view within 30 degrees");
}

void ollie() {
    const Settings settings;
    Spring spring;
    float gap{};
    double t = 0;
    for (int i = 0; i < 90; ++i, t += 1.0 / 60) {
        const float w = static_cast<float>(t) - 0.5f;
        const float rise = w < 0 ? 0 : w < 0.2f ? 0.5f * w / 0.2f : w < 0.6f ? 0.5f : std::max(0.0f, 0.5f - (w - 0.6f) * 2.5f);
        const auto view = update(spring, head_at(0, 0, 0, {0, 1.6f + rise, 0}), settings, t, {});
        if (w > 0.45f && w < 0.6f) gap = std::max(gap, std::abs(view[13] - 1.6f - rise));
    }
    expect(gap < 0.05f, "the eye catches up by the top of an ollie");
}

void snapping() {
    const Settings settings;
    Spring spring;
    auto view = update(spring, head_at(2.0f, 0, 0, {0, 1.6f, 0}), settings, 1.0, {});
    expect(std::abs(wrap_angle(angles_of(view).yaw - 2.0f)) < 1e-3f, "the first frame takes the head as it is");
    view = update(spring, head_at(-1.0f, 0, 0, {500, 1.6f, 0}), settings, 2.0, {500, 0, 0});
    expect(std::abs(wrap_angle(angles_of(view).yaw + 1.0f)) < 1e-3f && std::abs(view[12] - 500) < 1e-3f,
        "a pause or teleport snaps instead of smoothing across it");
}

// One full backflip: with Follow flips the view turns over, without it stays upright.
void flips() {
    Settings follow, upright;
    upright.follow_flips = false;
    Spring a, b;
    float lowest_follow = 1, lowest_upright = 1;
    bool finite = true;
    double t = 0;
    for (int i = 0; i < 90; ++i, t += 1.0 / 60) {
        const float turn = std::min(1.0f, static_cast<float>(t) / 1.2f) * 2 * pi;
        Matrix head{};
        head[15] = 1;
        write(head, multiply({0, std::sin(0.5f), 0, std::cos(0.5f)}, {std::sin(-turn / 2), 0, 0, std::cos(-turn / 2)}), {0, 1.6f, 0});
        const auto va = update(a, head, follow, t, {}), vb = update(b, head, upright, t, {});
        for (unsigned j = 0; j < 16; ++j) finite = finite && std::isfinite(va[j]) && std::isfinite(vb[j]);
        lowest_follow = std::min(lowest_follow, va[5]);
        lowest_upright = std::min(lowest_upright, vb[5]);
    }
    expect(finite, "a flip gives finite views");
    expect(lowest_follow < -0.5f, "Follow flips turns the view over");
    expect(lowest_upright > 0.1f, "without Follow flips the view stays upright");
}

void off_and_limits() {
    Settings settings;
    settings.stabilize = false;
    Spring spring;
    const auto head = head_at(0.3f, 0.2f, 0.4f, {1, 2, 3});
    const auto view = update(spring, head, settings, 1.0, {});
    float difference = 0;
    for (unsigned i = 0; i < 16; ++i) difference += std::abs(view[i] - head[i]);
    expect(difference < 1e-4f, "True first person off is the plain head camera");
    expect(valid(Settings{}), "the defaults are valid");
    settings.smoothing = 101;
    expect(!valid(settings), "smoothing above 100% is refused");
}
} // namespace

int main() {
    frame_layout();
    wobble();
    spin();
    ollie();
    snapping();
    flips();
    off_and_limits();
    if (failures) {
        std::printf("%d first-person stabilize test(s) failed\n", failures);
        return 1;
    }
    std::printf("first-person stabilize tests passed\n");
    return 0;
}
