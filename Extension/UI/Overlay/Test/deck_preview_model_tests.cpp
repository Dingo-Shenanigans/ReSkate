#include "Extension/UI/Overlay/deck_preview_model.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {
using namespace dingosdk::overlay::deck_preview::model;
int failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}

// A slab 0.8 long (y), 0.2 wide (x), 0.02 thick (z), its ends raised toward +z, written with the
// axes `order` maps to (x, y, z) of the file.
std::string slab(int ax, int ay, int az, bool upside_down) {
    std::string text;
    const auto vertex = [&](float x, float y, float z) {
        if (upside_down) z = -z, x = -x;
        float p[3]{};
        p[ax] = x, p[ay] = y, p[az] = z;
        text += "v " + std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]) + "\n";
    };
    constexpr int rows = 10;
    for (int r = 0; r <= rows; ++r) {
        const float y = -0.4f + 0.8f * r / rows;
        const float lift = std::abs(y) > 0.3f ? (std::abs(y) - 0.3f) : 0.0f;
        vertex(-0.1f, y, lift + 0.01f), vertex(0.1f, y, lift + 0.01f);
        vertex(-0.1f, y, lift - 0.01f), vertex(0.1f, y, lift - 0.01f);
    }
    for (int r = 0; r < rows; ++r) {
        const int a = r * 4 + 1, b = a + 4;
        text += "f " + std::to_string(a) + " " + std::to_string(a + 1) + " " + std::to_string(b + 1) + " " + std::to_string(b) + "\n";
        text += "f " + std::to_string(a + 2) + " " + std::to_string(b + 2) + " " + std::to_string(b + 3) + " " + std::to_string(a + 3) + "\n";
        text += "f " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(b + 2) + " " + std::to_string(a + 2) + "\n";
        text += "f " + std::to_string(a + 1) + " " + std::to_string(a + 3) + " " + std::to_string(b + 3) + " " + std::to_string(b + 1) + "\n";
    }
    return text;
}

// The grip faces point up (+z) and the deck is 2 long, along y, whatever axes the file uses.
void check(const Mesh& mesh, const char* what) {
    float low = 1e9f, high = -1e9f, up = 0;
    int grip = 0;
    for (const auto& v : mesh.vertices) {
        low = std::min(low, v.position[1]), high = std::max(high, v.position[1]);
        if (v.side > 0.5f) up += v.normal[2], ++grip;
    }
    expect(std::abs(high - low - 2.0f) < 1e-3f, what);
    expect(grip > 0 && up / grip > 0.8f, what);
}
}

int main() {
    Mesh mesh;
    std::string problem;
    expect(parse(slab(0, 1, 2, false), mesh, problem), "z-up slab parses");
    check(mesh, "z-up slab");
    expect(parse(slab(0, 2, 1, false), mesh, problem), "y-up slab parses");
    check(mesh, "y-up slab");
    expect(parse(slab(2, 0, 1, true), mesh, problem), "shuffled, upside-down slab parses");
    check(mesh, "shuffled, upside-down slab");
    expect(parse("# reskate-preview\n" + slab(0, 1, 2, false), mesh, problem), "game axes parse");
    check(mesh, "game axes");
    expect(!parse("v 0 0 0\nv 1 0 0\n", mesh, problem), "no faces is refused");
    expect(!parse("v 0 0 0\nf 1 2 3\n", mesh, problem), "a face past the vertices is refused");
    mesh = make_stand_in();
    expect(mesh.stand_in && !mesh.vertices.empty(), "the stand-in has a model");
    check(mesh, "stand-in");
    // Every turn of the deck fits the window: no vertex is projected outside it.
    for (int i = 0; i < 48; ++i) {
        const float aspects[3] = {0.5f, 1.5f, 3.0f};
        const auto cam = camera((i % 16) * 0.3927f, aspects[i / 16]);
        const auto m = cam.view_projection * cam.model;
        for (const auto& v : mesh.vertices) {
            float c[4]{};
            for (int r = 0; r < 4; ++r) c[r] = m.m[r][0] * v.position[0] + m.m[r][1] * v.position[1] + m.m[r][2] * v.position[2] + m.m[r][3];
            if (std::abs(c[0] / c[3]) > 1.0f || std::abs(c[1] / c[3]) > 1.0f || c[2] / c[3] < 0 || c[2] / c[3] > 1) {
                expect(false, "the deck stays inside the window at every turn");
                i = 48;
                break;
            }
        }
    }
    if (failures) return 1;
    std::printf("deck preview model: all passed\n");
    return 0;
}
