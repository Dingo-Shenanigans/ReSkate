// Unit tests for Extension/VR/vr_math.h. Platform independent:
//   cl /std:c++20 /EHsc /I <repo> Extension/VR/Test/vr_math_tests.cpp
//   g++ -std=c++20 -I <repo> Extension/VR/Test/vr_math_tests.cpp
#include "Extension/VR/vr_math.h"
#include <cstdio>
#include <cstdlib>

namespace {
using namespace dingosdk::vr::math;
namespace first_person = dingosdk::first_person;
int failures = 0;

void check(bool condition, const char* what, int line) {
    if (condition) return;
    ++failures;
    std::printf("FAILED line %d: %s\n", line, what);
}
#define CHECK(condition) check((condition), #condition, __LINE__)

bool near(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) <= tolerance; }
bool near(const Vec3& a, const Vec3& b, float tolerance = 1e-4f) {
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance);
}
Vec3 row(const Matrix& m, unsigned r) { return {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
bool orthonormal_right_handed(const Matrix& m) {
    const auto r = row(m, 0), u = row(m, 1), b = row(m, 2);
    return near(dot(r, r), 1) && near(dot(u, u), 1) && near(dot(b, b), 1) && near(dot(r, u), 0) &&
           near(dot(r, b), 0) && near(dot(u, b), 0) && near(cross(r, u), b);
}
Matrix camera(const Quat& q, const Vec3& position) {
    Matrix m{};
    m[3] = 7; m[7] = 8; m[11] = 9; m[15] = 1; // fourth lanes the camera owns
    first_person::write(m, q, position);
    return m;
}
Quat pitch(float radians) { return {std::sin(radians / 2), 0, 0, std::cos(radians / 2)}; }
}

int main() {
    // wrap keeps angles in (-pi, pi].
    CHECK(near(wrap(0), 0));
    CHECK(near(wrap(3 * pi), pi));
    CHECK(near(wrap(-pi), pi));
    CHECK(near(wrap(2 * pi + 0.25f), 0.25f));

    // Heading of a camera turned by yaw(h) is h, from the matrix and the quaternion.
    for (float h : {0.0f, 0.5f, -1.2f, 2.9f}) {
        const auto m = camera(yaw(h), {});
        CHECK(heading(m).has_value() && near(*heading(m), h));
        CHECK(near(heading(yaw(h)), h));
    }
    // Looking straight down still has a heading (from the up row).
    {
        const auto down = first_person::multiply(yaw(0.7f), pitch(-pi / 2));
        const auto m = camera(down, {});
        CHECK(heading(m).has_value() && near(*heading(m), 0.7f, 1e-3f));
        const auto up = first_person::multiply(yaw(-0.4f), pitch(pi / 2));
        CHECK(heading(camera(up, {})).has_value() && near(*heading(camera(up, {})), -0.4f, 1e-3f));
    }
    // Yaw by +90 degrees faces -X and its right is -Z.
    {
        const auto m = camera(yaw(pi / 2), {});
        CHECK(near(row(m, 2), {1, 0, 0}));
        CHECK(near(row(m, 0), {0, 0, -1}));
    }

    const Vec3 skater{10, 2, -5};
    // Steady heading: smooth follow, snap past the snap angle, nothing inside the dead zone.
    {
        HeadingRig rig;
        CHECK(near(follow_snap(rig, 0.0f, 0, 0.2f, 0.8f, 0), 0.0f));
        const float small = follow_snap(rig, 0.3f, 1.0f / 60, 0.2f, 0.8f, 0);
        CHECK(small > 0.0f && small < 0.3f); // followed smoothly
        CHECK(near(follow_snap(rig, 2.0f, 1.0f / 60, 0.2f, 0.8f, 0), 2.0f)); // the target jumped: snapped
        HeadingRig lagging;
        float view = 0;
        for (int i = 0; i <= 60; ++i) view = follow_snap(lagging, i * 0.05f, 1.0f / 60, 0.5f, 0.8f, 0);
        CHECK(view > 0.5f && view < 3.0f); // a steady turn that the view lags is never snapped
        CHECK(near(follow_snap(rig, 2.2f, 1.0f / 60, 0.2f, 0.8f, 0.35f), 2.0f)); // inside the dead zone
    }
    // Eyes heading: a fast spin is followed closely, small quick wobble is smoothed, and a spin
    // keeps going through frames where the head is upside down (its turn undefined).
    {
        EyesHeading spin;
        float view = 0;
        for (int i = 0; i <= 60; ++i) view = eyes_heading(spin, yaw(wrap(6.0f * i / 60.0f)), 1.0f / 60);
        CHECK(std::abs(wrap(view - wrap(6.0f))) < 0.35f); // within 20 degrees at the end of a 6 rad/s spin
        EyesHeading wobble;
        float low = 1e9f, high = -1e9f;
        for (int i = 0; i <= 240; ++i) {
            const float out = eyes_heading(wobble, yaw(0.1f * std::sin(2 * pi * 2.0f * i / 60.0f)), 1.0f / 60);
            if (i > 120) { low = std::min(low, out); high = std::max(high, out); }
        }
        CHECK((high - low) < 0.1f); // a 0.2 rad, 2 Hz wobble comes out at less than half
        EyesHeading upside;
        for (int i = 0; i <= 20; ++i) (void)eyes_heading(upside, yaw(3.0f * i / 60.0f), 1.0f / 60);
        float before = eyes_heading(upside, yaw(1.0f), 1.0f / 60), after = before;
        for (int i = 0; i < 6; ++i) after = eyes_heading(upside, pitch(pi), 1.0f / 60);
        CHECK(wrap(after - before) > 0.05f); // still turning the same way
    }
    // Eyes position: a steady offset passes; a crouch is scaled; no reset for small moves.
    {
        EyesPosition e;
        Vec3 out{};
        for (int i = 0; i < 120; ++i) out = eyes_position(e, {0, 1.6f, 0}, {0, 0.9f, 0}, 1.0f / 60, 0.5f);
        CHECK(near(out, {0, 1.6f, 0}));
        for (int i = 0; i < 60; ++i) out = eyes_position(e, {0, 1.2f, 0}, {0, 0.9f, 0}, 1.0f / 60, 0.5f);
        CHECK(out[1] > 1.32f && out[1] < 1.48f); // a 0.4 m crouch shows as about 0.2 m
    }
    // Steady tilt: the twist about +Y is removed; small wobble is smoothed, a fast flip followed.
    {
        CHECK(near(heading(untwisted(first_person::multiply(yaw(1.2f), pitch(0.4f)))), 0.0f));
        // The twist heading holds through a flip (pitch past vertical), where the forward axis turns round.
        CHECK(near(twist_heading(first_person::multiply(yaw(1.2f), pitch(2.5f))), 1.2f));
        CHECK(near(twist_heading(yaw(-2.0f)), -2.0f));
        // Lying on the back (a coffin): the turn stays defined (twist size about 0.7) and right;
        // upside down it is lost (size near 0), which the spin counting skips.
        const auto lying = first_person::multiply(yaw(1.0f), pitch(-pi / 2));
        CHECK(std::hypot(lying[1], lying[3]) > 0.6f);
        CHECK(near(twist_heading(lying), 1.0f));
        const auto inverted = pitch(pi);
        CHECK(std::hypot(inverted[1], inverted[3]) < 0.1f);
        TiltRig rig;
        (void)steady_tilt(rig, pitch(0), 1.0f / 60, 0.2f, 4.0f);
        const auto wobble = steady_tilt(rig, pitch(0.03f), 1.0f / 60, 0.2f, 4.0f);
        CHECK(std::abs(wobble[0]) < std::sin(0.015f)); // less than the 0.03 rad step
        const auto flip = steady_tilt(rig, pitch(0.6f), 1.0f / 60, 0.2f, 4.0f);
        const auto expected = pitch(0.6f);
        CHECK(std::abs(flip[0] * expected[0] + flip[1] * expected[1] + flip[2] * expected[2] + flip[3] * expected[3]) > 0.9999f);
    }
    // The seat nudge moves along the heading (forward is -Z at heading 0) and up, unscaled.
    {
        const auto base = camera(yaw(0), skater);
        const auto out = compose(base, 0, true, {}, {}, {}, true, 2.0f, 0.1f, 0.2f);
        CHECK(near(row(out, 3), {10, 2.2f, -5.1f}));
        const auto right = compose(base, 0, true, {}, {}, {}, true, 1.0f, 0, 0, 0.3f);
        CHECK(near(row(right, 3), {10.3f, 2, -5}));
    }
    // Headset at the anchor, looking ahead: the camera is the level base heading at the head.
    {
        const auto base = camera(first_person::multiply(yaw(1.0f), pitch(0.3f)), skater);
        const auto out = compose(base, 1.0f, true, {}, {}, {}, true, 1.0f);
        CHECK(orthonormal_right_handed(out));
        CHECK(near(heading(first_person::orientation(out)), 1.0f));
        CHECK(near(row(out, 1), {0, 1, 0})); // level: the base pitch is ignored
        CHECK(near(row(out, 3), skater));
        CHECK(out[3] == 7 && out[7] == 8 && out[11] == 9 && out[15] == 1);
    }
    // Following the full head orientation keeps the base pitch.
    {
        const auto base = camera(first_person::multiply(yaw(1.0f), pitch(0.3f)), skater);
        const auto out = compose(base, 1.0f, false, {}, {}, {}, true, 1.0f);
        CHECK(near(row(out, 1), row(base, 1)));
    }
    // Eye offsets turn with the skater: a right eye 3.2 cm along +X, base facing -X.
    {
        const auto base = camera(yaw(pi / 2), skater);
        const Pose eye{{0, 0, 0, 1}, {0.032f, 0, 0}};
        const auto out = compose(base, pi / 2, true, eye, {}, {}, true, 1.0f);
        CHECK(near(row(out, 3), {skater[0], skater[1], skater[2] - 0.032f}));
    }
    // Turning the head adds to the base heading; the anchor heading is subtracted.
    {
        const auto base = camera(yaw(1.0f), skater);
        const Pose eye{yaw(0.5f), {}};
        CHECK(near(heading(first_person::orientation(compose(base, 1.0f, true, eye, {}, {}, true, 1.0f))), 1.5f));
        const Anchor anchor{0.5f, {}};
        CHECK(near(heading(first_person::orientation(compose(base, 1.0f, true, eye, {}, anchor, true, 1.0f))), 1.0f));
    }
    // Head pitch from the headset reaches the camera.
    {
        const auto base = camera(yaw(0), skater);
        const Pose eye{pitch(0.4f), {}};
        const auto out = compose(base, 0, true, eye, {}, {}, true, 1.0f);
        CHECK(near(row(out, 1), first_person::rotate(pitch(0.4f), {0, 1, 0})));
    }
    // Positional tracking is relative to the anchor, rotated into the anchor's frame and scaled.
    {
        const auto base = camera(yaw(0), skater);
        const Anchor anchor{pi / 2, {1, 1.6f, 1}}; // recentered while facing -X
        const Pose eye{yaw(pi / 2), {0, 1.6f, 1}};  // moved 1 m towards -X: forward
        const auto out = compose(base, 0, true, eye, eye, anchor, true, 2.0f);
        CHECK(near(row(out, 3), {skater[0], skater[1], skater[2] - 2.0f}));
        CHECK(near(heading(first_person::orientation(out)), 0));
        // Without positional tracking only the eye's offset from the head remains.
        const Pose head{yaw(pi / 2), {0, 1.6f, 1}};
        const Pose left{yaw(pi / 2), {0, 1.6f, 1.032f}}; // facing -X, left is +Z
        const auto still = compose(base, 0, true, left, head, anchor, false, 1.0f);
        CHECK(near(row(still, 3), {skater[0] - 0.032f, skater[1], skater[2]}));
    }
    // Projection: one symmetric frustum covering two asymmetric eyes.
    {
        const auto left = tangents_from_angles(-0.95f, 0.78f, 0.84f, -0.95f);
        const auto right = tangents_from_angles(-0.78f, 0.95f, 0.84f, -0.95f);
        const float aspect = 1.0f;
        const float t = render_tangent({left, right}, aspect, 1.0f);
        CHECK(near(t, std::tan(0.95f)));
        CHECK(near(native_fov_degrees(t, aspect, true), 2 * 0.95f * 180 / pi, 1e-2f));
        const auto angles = layer_angles(t, aspect);
        CHECK(near(angles.left, -0.95f) && near(angles.right, 0.95f) && near(angles.up, 0.95f) && near(angles.down, -0.95f));
        // A wide image needs less vertical FOV to cover the same horizontal tangent.
        const float wide = render_tangent({left, right}, 2.0f, 1.0f);
        CHECK(near(wide, std::tan(0.95f)));
        CHECK(near(layer_angles(wide, 2.0f).right, std::atan(2 * std::tan(0.95f))));
        // A tall image needs more vertical FOV to cover the horizontal.
        const float tall = render_tangent({left, right}, 0.5f, 1.0f);
        CHECK(near(tall, 2 * std::tan(0.95f)));
        CHECK(near(native_fov_degrees(tall, 0.5f, false), 2 * 0.95f * 180 / pi, 1e-2f));
        CHECK(near(render_tangent({left, right}, 1.0f, 1.1f), 1.1f * std::tan(0.95f)));
    }
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("vr_math: all checks passed\n");
    return EXIT_SUCCESS;
}
