#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Board Break's deck preview (deck_preview.h): the model, as read from a deck's preview.obj, and
// the camera it is seen through. No graphics API here, so it can be tested on its own.
namespace dingosdk::overlay::deck_preview::model {
inline constexpr float pi = 3.14159265358979f;
// Preview space: x across the deck, y along it (the nose at +y), z up out of the grip; the deck is
// 2 long and centred on the origin. A triangle list, three vertices to a triangle.
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    float ply;  // 0 at the underside, 1 at the grip: where in the deck's thickness (the edge's plies)
    float side; // 1 a grip face, -1 an underside face, 0 the edge
};
static_assert(sizeof(Vertex) == 40);

struct Mesh {
    std::vector<Vertex> vertices;
    bool stand_in{}; // no preview.obj: the built-in popsicle
};


// A Wavefront OBJ as a preview model: the file, or its text. False with `problem` saying what is
// wrong with it ("has no faces").
bool read(const std::filesystem::path& obj, Mesh& out, std::string& problem);
bool parse(std::string_view text, Mesh& out, std::string& problem);
// The stand-in for a deck without a preview.obj: a plain 8.25" popsicle with kicks and concave.
Mesh make_stand_in();

// Row-major 4x4, applied to column vectors (m * v).
struct Matrix {
    float m[4][4]{};
    static Matrix identity();
};
Matrix operator*(const Matrix& a, const Matrix& b);
// The model turned by `yaw` about its length (at 0 the underside faces the camera) and leant back
// a little; the camera on +z looking down -z at a window `aspect` wide for 1 tall, the deck
// fitting it whatever its turn; D3D's depth, 0 to 1.
struct Camera {
    Matrix model, view_projection;
    float distance{};
};
Camera camera(float yaw, float aspect);
}
