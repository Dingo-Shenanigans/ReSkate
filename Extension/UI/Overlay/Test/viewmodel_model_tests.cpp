#include "Extension/UI/Overlay/viewmodel_model.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

// A viewmodel's file read and its clips played (Extension/UI/Overlay/viewmodel_model.h): a small model
// made here, files broken in each way the reader refuses, and, given a .rsvm made by
// contrib/viewmodel_export.py, that one read and posed where a viewmodel should be.
namespace {
using namespace dingosdk::viewmodel;
int failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}
bool near(float a, float b, float within = 1e-4f) { return std::fabs(a - b) <= within; }

template <class T> void put(std::string& out, T value) { out.append(reinterpret_cast<const char*>(&value), sizeof value); }
void put_text(std::string& out, const std::string& text) {
    put<std::uint8_t>(out, static_cast<std::uint8_t>(text.size()));
    out += text;
}
void put_affine(std::string& out, const Affine& m) {
    for (const float v : m) put(out, v);
}
Affine moved(float x, float y, float z) { return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}; }

// Two bones (the second a child of the first), one material, one triangle weighted to bone 1, and
// one clip of three frames at 10 a second in which bone 1 moves 1 m along x a frame.
struct Options {
    std::uint8_t joint{1};
    std::uint32_t index{2};
    bool extra{};
};
std::string small_model(Options o = {}) {
    std::string f = "RSVM";
    put<std::uint32_t>(f, 1);
    for (const std::uint32_t count : {2u, 1u, 1u, 3u, 3u, 1u}) put(f, count);
    put_text(f, "Root");
    put<std::int16_t>(f, -1);
    put_affine(f, identity);
    put_text(f, "Muzzle");
    put<std::int16_t>(f, 0);
    put_affine(f, moved(0, 0, 0.5f));
    put_text(f, "gun");
    put_text(f, "gun_albedo.png");
    put_text(f, "");
    put_text(f, "");
    put(f, 0.4f);
    put(f, 0.5f);
    for (const std::uint32_t v : {0u, 0u, 3u}) put(f, v);
    for (int i = 0; i < 3; ++i) {
        Vertex v{};
        v.position[0] = static_cast<float>(i);
        v.position[2] = 0.5f;
        v.normal[2] = -1;
        v.tangent[0] = 1;
        v.tangent[3] = 1;
        v.joints[0] = o.joint;
        v.weights[0] = 255;
        f.append(reinterpret_cast<const char*>(&v), sizeof v);
    }
    for (const std::uint32_t i : {0u, 1u, o.index}) put(f, i);
    put_text(f, "Fire");
    put(f, 10.0f);
    put<std::uint32_t>(f, 3);
    for (int frame = 0; frame < 3; ++frame) {
        put_affine(f, identity);
        put_affine(f, moved(static_cast<float>(frame), 0, 0));
    }
    if (o.extra) f += "x";
    return f;
}

void reads_a_model_and_samples_its_clip() {
    std::string why;
    const auto model = parse(small_model(), why);
    expect(model.has_value(), ("a well made file reads: " + why).c_str());
    if (!model) return;
    expect(model->bones.size() == 2 && model->bones[1].parent == 0 && model->bone("Muzzle") == 1 && model->bone("Nope") == -1, "its bones");
    expect(model->materials[0].albedo == "gun_albedo.png" && near(model->materials[0].roughness, 0.4f), "its material");
    expect(model->clips.size() == 1 && model->clip("Fire") == 0 && near(static_cast<float>(model->clips[0].duration()), 0.2f), "its clip lasts 0.2 s");
    std::array<Affine, 2> skin{};
    sample(*model, model->clips[0], 0.05, false, skin);
    expect(near(skin[1][3], 0.5f) && near(skin[0][3], 0), "halfway between the first two frames");
    sample(*model, model->clips[0], 5.0, false, skin);
    expect(near(skin[1][3], 2.0f), "held on its last frame");
    sample(*model, model->clips[0], 0.25, true, skin);
    expect(near(skin[1][3], 0.5f), "looped: its last frame is its first");
}

void refuses_broken_files() {
    std::string why;
    expect(!parse("RSVX", why), "not a viewmodel");
    auto file = small_model();
    expect(!parse(file.substr(0, file.size() - 4), why) && why.find("ends") != std::string::npos, "cut short");
    expect(!parse(small_model({.extra = true}), why), "more after its last clip");
    expect(!parse(small_model({.joint = 7}), why) && why.find("bone") != std::string::npos, "weighted to a bone it does not have");
    expect(!parse(small_model({.index = 9}), why) && why.find("vertex") != std::string::npos, "a triangle with a vertex it does not have");
    auto version = small_model();
    version[4] = 2;
    expect(!parse(version, why) && why.find("version") != std::string::npos, "another version");
    auto huge = small_model();
    const std::uint32_t lots = 4'000'000'000u;
    std::memcpy(huge.data() + 8 + 12, &lots, 4); // the vertex count
    expect(!parse(huge, why), "a vertex count the file cannot hold");
}

void fades_between_clips() {
    std::string why;
    auto model = *parse(small_model(), why);
    // A second clip with bone 1 still at 10 m along x.
    Clip still;
    still.name = "Idle";
    still.fps = 10;
    still.frames = 1;
    for (const auto& m : {identity, moved(10, 0, 0)}) still.skin.insert(still.skin.end(), m.begin(), m.end());
    model.clips.push_back(still);
    Animator animator;
    std::array<Affine, 2> skin{};
    animator.pose(model, 0, skin);
    expect(near(skin[1][3], 0) && skin[1] == identity, "the rest pose with nothing playing");
    animator.play(model, {0, false, 1, 0.1}, 100.0);
    expect(animator.state(model, 100.1).clip == 0 && !animator.state(model, 100.1).done, "Fire plays");
    expect(animator.state(model, 100.25).done && near(static_cast<float>(animator.state(model, 100.25).time), 0.2f), "and ends");
    animator.play(model, {1, true, 1, 0.2}, 101.0);
    animator.pose(model, 101.1, skin);
    expect(near(skin[1][3], 6.0f), "halfway through the fade from Fire's last frame (2) to Idle (10)");
    animator.pose(model, 101.5, skin);
    expect(near(skin[1][3], 10.0f), "faded in");
    animator.play(model, {0, false, 2, 0}, 102.0);
    animator.pose(model, 102.05, skin);
    expect(near(skin[1][3], 1.0f), "at twice the speed, with no fade");
    animator.play(model, {5, false, 1, 0}, 103.0);
    expect(animator.state(model, 103.0).clip == 0, "a clip it does not have is not played");
}

void places_the_model() {
    const auto turned = placement({0, 0, 0}, 90, 0, 0);
    expect(near(turned[0 * 4 + 2], 1) && near(turned[2 * 4 + 2], 0), "yaw 90 turns its front (+z) to the right (+x)");
    const auto tipped = placement({0, 0, 0}, 0, 90, 0);
    expect(near(tipped[1 * 4 + 2], 1), "pitch 90 tips its front up (+y)");
    const auto moved_by = placement({0.1f, -0.2f, 0.3f}, 0, 0, 0);
    expect(near(moved_by[3], 0.1f) && near(moved_by[7], -0.2f) && near(moved_by[11], 0.3f) && near(moved_by[0], 1), "moved, not turned");
    const auto chained = multiply(moved(1, 0, 0), moved(0, 2, 0));
    expect(near(chained[3], 1) && near(chained[7], 2), "moves add");
}

// An exported model: every clip samples, and the gun's body (contrib/viewmodel_export.py's Head_Cam
// view) is in front of the eye, below it and to the right, as a right-handed pistol is held.
void reads_an_exported_model(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("(no exported viewmodel at %s: skipped)\n", path);
        return;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    std::string why;
    const auto model = parse(bytes, why);
    expect(model.has_value(), ("the exported viewmodel reads: " + why).c_str());
    if (!model) return;
    std::printf("%s: %zu bones, %zu vertices, %zu triangles, %zu clips\n", path, model->bones.size(), model->vertices.size(),
                model->indices.size() / 3, model->clips.size());
    const int body = model->bone("PBody");
    const int idle = model->clip("FPS_Pistol_Idle");
    if (body < 0 || idle < 0) {
        std::printf("(not the pistol: placement not checked)\n");
        return;
    }
    std::array<Affine, max_bones> skin{};
    sample(*model, model->clips[static_cast<std::size_t>(idle)], 0.5, true, std::span(skin).first(model->bones.size()));
    const auto pose = multiply(skin[static_cast<std::size_t>(body)], model->bones[static_cast<std::size_t>(body)].bind);
    std::printf("the gun's body in idle: %.3f right, %.3f up, %.3f forward\n", pose[3], pose[7], pose[11]);
    expect(pose[11] > 0.1f && pose[11] < 1.0f, "the gun is in front of the eye");
    expect(pose[7] < 0, "and below it");
    expect(pose[3] > 0, "and to the right");
    for (const auto& clip : model->clips) {
        sample(*model, clip, clip.duration() * 0.5, false, std::span(skin).first(model->bones.size()));
        bool finite = true;
        for (std::size_t i = 0; i < model->bones.size(); ++i)
            for (const float v : skin[i]) finite = finite && std::isfinite(v);
        expect(finite, ("clip " + clip.name + " samples").c_str());
    }
}
} // namespace

int main(int argc, char** argv) {
    reads_a_model_and_samples_its_clip();
    refuses_broken_files();
    fades_between_clips();
    places_the_model();
    if (argc > 1) reads_an_exported_model(argv[1]);
    if (failures) {
        std::printf("%d viewmodel check(s) failed\n", failures);
        return 1;
    }
    std::printf("viewmodel checks passed\n");
    return 0;
}
