#include "deck_preview_model.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

namespace dingosdk::overlay::deck_preview::model {
namespace {
constexpr float tilt = -0.22f; // the top leans back a little, so the kicks read as 3D
constexpr std::size_t maximum_triangles = 400000;
constexpr std::uintmax_t maximum_obj_bytes = 64ull * 1024 * 1024;
using Vec3 = std::array<float, 3>;
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 operator+(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 operator*(const Vec3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalised(const Vec3& a, const Vec3& fallback = {0, 0, 1}) {
    const float l = length(a);
    return l > 1e-12f && std::isfinite(l) ? a * (1.0f / l) : fallback;
}

// A mesh as read: positions, texture coordinates, normals, and triangles of indices into them
// (-1 for none).
struct Corner {
    int position = -1, uv = -1, normal = -1;
};
struct Source {
    std::vector<Vec3> positions, normals;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<Corner, 3>> triangles;
    bool reskate_axes{}; // "# reskate-preview": x across, y along with the nose at -y (the game's), z up
};

std::string_view next_word(std::string_view& line) {
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    std::size_t end = start;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r') ++end;
    const auto word = line.substr(start, end - start);
    line.remove_prefix(end);
    return word;
}
bool read_float(std::string_view word, float& out) {
    if (word.empty()) return false;
    const auto result = std::from_chars(word.data(), word.data() + word.size(), out);
    return result.ec == std::errc{} && std::isfinite(out);
}
bool read_int(std::string_view word, int& out) {
    if (word.empty()) return false;
    const auto result = std::from_chars(word.data(), word.data() + word.size(), out);
    return result.ec == std::errc{};
}
// An OBJ index (1-based, negative from the end) as 0-based, or -1.
int resolve(int index, std::size_t count) {
    if (index > 0 && static_cast<std::size_t>(index) <= count) return index - 1;
    if (index < 0 && static_cast<std::size_t>(-index) <= count) return static_cast<int>(count) + index;
    return -1;
}

bool read_obj(std::string_view text, Source& out, std::string& problem) {
    std::string_view rest = text;
    std::vector<Corner> polygon;
    while (!rest.empty()) {
        const auto newline = rest.find('\n');
        auto line = rest.substr(0, newline);
        rest.remove_prefix(newline == std::string_view::npos ? rest.size() : newline + 1);
        if (line.starts_with("# reskate-preview")) { out.reskate_axes = true; continue; }
        const auto key = next_word(line);
        if (key == "v") {
            Vec3 p{};
            if (!read_float(next_word(line), p[0]) || !read_float(next_word(line), p[1]) || !read_float(next_word(line), p[2])) {
                problem = "has a vertex that is not three numbers";
                return false;
            }
            out.positions.push_back(p);
        } else if (key == "vt") {
            std::array<float, 2> t{};
            if (!read_float(next_word(line), t[0]) || !read_float(next_word(line), t[1])) t = {0, 0};
            out.uvs.push_back(t);
        } else if (key == "vn") {
            Vec3 n{};
            if (!read_float(next_word(line), n[0]) || !read_float(next_word(line), n[1]) || !read_float(next_word(line), n[2])) n = {0, 0, 1};
            out.normals.push_back(n);
        } else if (key == "f") {
            polygon.clear();
            for (auto word = next_word(line); !word.empty(); word = next_word(line)) {
                Corner corner;
                std::array<std::string_view, 3> parts{};
                std::size_t part = 0;
                for (std::size_t i = 0, from = 0; i <= word.size() && part < 3; ++i)
                    if (i == word.size() || word[i] == '/') { parts[part++] = word.substr(from, i - from); from = i + 1; }
                int index{};
                if (read_int(parts[0], index)) corner.position = resolve(index, out.positions.size());
                if (read_int(parts[1], index)) corner.uv = resolve(index, out.uvs.size());
                if (read_int(parts[2], index)) corner.normal = resolve(index, out.normals.size());
                if (corner.position < 0) {
                    problem = "has a face with a vertex it does not have";
                    return false;
                }
                polygon.push_back(corner);
            }
            for (std::size_t i = 2; i < polygon.size(); ++i) out.triangles.push_back({polygon[0], polygon[i - 1], polygon[i]});
            if (out.triangles.size() > maximum_triangles) {
                problem = "has more than 400,000 triangles (decimate it for the preview)";
                return false;
            }
        }
    }
    if (out.triangles.empty()) {
        problem = "has no faces";
        return false;
    }
    return true;
}

// A source mesh turned into preview space and into the renderer's vertices. Any axes: the longest
// extent is the length, the shortest the thickness. The grip is the side the ends turn up to (a
// deck's kicks), unless the file says it uses the game's axes.
bool build(Source source, Mesh& out, std::string& problem) {
    if (source.positions.size() < 3) { problem = "has too few vertices"; return false; }
    Vec3 low = source.positions[0], high = source.positions[0];
    for (const auto& p : source.positions)
        for (int k = 0; k < 3; ++k) low[k] = std::min(low[k], p[k]), high[k] = std::max(high[k], p[k]);
    const Vec3 extent = high - low;
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return extent[a] > extent[b]; });
    int along = order[0], across = order[1], up = order[2];
    float sign_x = 1, sign_y = 1, sign_z = 1;
    if (source.reskate_axes) {
        along = 1, across = 0, up = 2;
        sign_x = -1, sign_y = -1; // nose from -y to +y: half a turn about z
    } else {
        // An odd shuffle of the axes would mirror the deck (and its texture): flip x back.
        const int parity = (order[0] > order[1]) + (order[0] > order[2]) + (order[1] > order[2]);
        // (x, y, z) take (order[1], order[0], order[2]): one swap away from `order` itself.
        if (parity % 2 == 0) sign_x = -1;
    }
    const float length_of = extent[along];
    if (!(length_of > 1e-6f) || extent[up] <= 0) { problem = "is flat or a point"; return false; }
    const Vec3 centre = (low + high) * 0.5f;
    const float scale = 2.0f / length_of;
    auto place = [&](const Vec3& p) {
        const Vec3 q = p - centre;
        return Vec3{q[across] * sign_x * scale, q[along] * sign_y * scale, q[up] * sign_z * scale};
    };
    auto turn = [&](const Vec3& n) { return Vec3{n[across] * sign_x, n[along] * sign_y, n[up] * sign_z}; };
    std::vector<Vec3> positions(source.positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = place(source.positions[i]);
    if (!source.reskate_axes) {
        // The kicks turn up toward the grip: the ends of the deck sit higher than its middle.
        double ends = 0, middle = 0;
        std::size_t ends_n = 0, middle_n = 0;
        for (const auto& p : positions) {
            if (std::abs(p[1]) > 0.78f) ends += p[2], ++ends_n;
            else if (std::abs(p[1]) < 0.35f) middle += p[2], ++middle_n;
        }
        if (ends_n && middle_n && ends / ends_n < middle / middle_n) {
            // Upside down: half a turn about y (x and z flip, so nothing is mirrored).
            sign_x = -sign_x, sign_z = -sign_z;
            for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = place(source.positions[i]);
        }
    }
    // Faces turned outward: a closed mesh wound inside out has a negative volume.
    double volume = 0;
    for (const auto& t : source.triangles)
        volume += dot(positions[static_cast<std::size_t>(t[0].position)],
                      cross(positions[static_cast<std::size_t>(t[1].position)], positions[static_cast<std::size_t>(t[2].position)]));
    if (volume < 0)
        for (auto& t : source.triangles) std::swap(t[1], t[2]);

    // Face normals (by area), and the faces at each position, for smooth normals that keep the
    // deck's edges sharp where a file brings none.
    std::vector<Vec3> faces(source.triangles.size());
    std::vector<std::vector<std::uint32_t>> at(positions.size());
    for (std::size_t i = 0; i < source.triangles.size(); ++i) {
        const auto& t = source.triangles[i];
        const auto& a = positions[static_cast<std::size_t>(t[0].position)];
        faces[i] = cross(positions[static_cast<std::size_t>(t[1].position)] - a, positions[static_cast<std::size_t>(t[2].position)] - a);
        for (const auto& c : t) at[static_cast<std::size_t>(c.position)].push_back(static_cast<std::uint32_t>(i));
    }
    // Where each position is in the deck's thickness: the lowest and highest point of the deck in
    // a grid of strips along and across it.
    constexpr int rows = 128, columns = 12;
    std::vector<float> floor_of(rows * columns, 1e9f), roof_of(rows * columns, -1e9f);
    float half_width = 1e-6f;
    for (const auto& p : positions) half_width = std::max(half_width, std::abs(p[0]));
    const auto cell = [&](const Vec3& p) {
        const int r = std::clamp(static_cast<int>((p[1] + 1.0f) * 0.5f * rows), 0, rows - 1);
        const int c = std::clamp(static_cast<int>((p[0] / half_width + 1.0f) * 0.5f * columns), 0, columns - 1);
        return static_cast<std::size_t>(r * columns + c);
    };
    for (const auto& p : positions) {
        const auto k = cell(p);
        floor_of[k] = std::min(floor_of[k], p[2]);
        roof_of[k] = std::max(roof_of[k], p[2]);
    }
    const bool textured = !source.uvs.empty();
    const float sharp = std::cos(45.0f * pi / 180.0f);
    out.vertices.clear();
    out.vertices.reserve(source.triangles.size() * 3);
    for (std::size_t i = 0; i < source.triangles.size(); ++i) {
        const auto face = normalised(faces[i], {0, 0, 0});
        if (face[0] == 0 && face[1] == 0 && face[2] == 0) continue; // no area
        const float side = face[2] > 0.45f ? 1.0f : face[2] < -0.45f ? -1.0f : 0.0f;
        for (const auto& c : source.triangles[i]) {
            Vertex v{};
            const auto& p = positions[static_cast<std::size_t>(c.position)];
            v.position[0] = p[0], v.position[1] = p[1], v.position[2] = p[2];
            Vec3 n{};
            if (c.normal >= 0) n = normalised(turn(source.normals[static_cast<std::size_t>(c.normal)]), face);
            if (c.normal < 0 || dot(n, face) < 0.0f) {
                Vec3 sum{};
                for (const auto other : at[static_cast<std::size_t>(c.position)])
                    if (dot(normalised(faces[other], {0, 0, 0}), face) > sharp) sum = sum + faces[other];
                n = normalised(sum, face);
            }
            v.normal[0] = n[0], v.normal[1] = n[1], v.normal[2] = n[2];
            if (textured && c.uv >= 0) {
                v.uv[0] = source.uvs[static_cast<std::size_t>(c.uv)][0];
                v.uv[1] = 1.0f - source.uvs[static_cast<std::size_t>(c.uv)][1]; // OBJ's v runs up, a texture's down
            }
            const auto k = cell(p);
            const float span = roof_of[k] - floor_of[k];
            v.ply = span > 1e-6f ? std::clamp((p[2] - floor_of[k]) / span, 0.0f, 1.0f) : 0.5f;
            v.side = side;
            out.vertices.push_back(v);
        }
    }
    if (out.vertices.empty()) { problem = "has no faces with an area"; return false; }
    return true;
}

}

// The stand-in: a plain 8.25" popsicle with kicks and concave, for a deck without its preview.
Mesh make_stand_in() {
    Source source;
    constexpr int rows = 96, columns = 14;
    constexpr float width = 0.52f, thickness = 0.034f, kick_from = 0.60f, kick_rise = 0.16f, concave = 0.018f;
    const float radius = width * 0.5f;
    const auto half_width = [&](float y) {
        const float into = std::abs(y) - (1.0f - radius);
        if (into <= 0) return radius;
        return radius * std::sqrt(std::max(0.0f, 1.0f - (into / radius) * (into / radius)));
    };
    const auto lift = [&](float x, float y) {
        const float k = std::max(0.0f, (std::abs(y) - kick_from) / (1.0f - kick_from));
        return kick_rise * k * k + concave * (x / radius) * (x / radius);
    };
    const auto index = [&](int r, int c, int layer) { return (r * (columns + 1) + c) * 2 + layer; };
    for (int r = 0; r <= rows; ++r) {
        // Closer rows at the ends, where the deck rounds off and turns up.
        const float y = -std::cos(pi * static_cast<float>(r) / rows);
        const float hw = half_width(y);
        for (int c = 0; c <= columns; ++c) {
            const float x = -hw + 2.0f * hw * static_cast<float>(c) / columns;
            const float z = lift(x, y);
            source.positions.push_back({x, y, z + thickness * 0.5f});
            source.positions.push_back({x, y, z - thickness * 0.5f});
        }
    }
    const auto quad = [&](int a, int b, int c, int d) {
        source.triangles.push_back({Corner{a}, Corner{b}, Corner{c}});
        source.triangles.push_back({Corner{a}, Corner{c}, Corner{d}});
    };
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            quad(index(r, c, 0), index(r, c + 1, 0), index(r + 1, c + 1, 0), index(r + 1, c, 0)); // grip
            quad(index(r, c, 1), index(r + 1, c, 1), index(r + 1, c + 1, 1), index(r, c + 1, 1)); // underside
        }
        quad(index(r, 0, 1), index(r, 0, 0), index(r + 1, 0, 0), index(r + 1, 0, 1));                         // left edge
        quad(index(r, columns, 0), index(r, columns, 1), index(r + 1, columns, 1), index(r + 1, columns, 0)); // right edge
    }
    Mesh mesh;
    std::string problem;
    build(std::move(source), mesh, problem);
    mesh.stand_in = true;
    return mesh;
}

bool parse(std::string_view text, Mesh& out, std::string& problem) {
    Source source;
    return read_obj(text, source, problem) && build(std::move(source), out, problem);
}
bool read(const std::filesystem::path& obj, Mesh& out, std::string& problem) {
    std::error_code error;
    const auto size = std::filesystem::file_size(obj, error);
    if (error || size == 0 || size > maximum_obj_bytes) {
        problem = error ? "cannot be read" : size == 0 ? "is empty" : "is larger than 64 MB";
        return false;
    }
    std::ifstream in(obj, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        problem = "cannot be read";
        return false;
    }
    return parse(text, out, problem);
}

Matrix Matrix::identity() {
    Matrix r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    return r;
}
Matrix operator*(const Matrix& a, const Matrix& b) {
    Matrix r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
namespace {
Matrix rotate_x(float a) {
    auto r = Matrix::identity();
    r.m[1][1] = std::cos(a), r.m[1][2] = -std::sin(a), r.m[2][1] = std::sin(a), r.m[2][2] = std::cos(a);
    return r;
}
Matrix rotate_y(float a) {
    auto r = Matrix::identity();
    r.m[0][0] = std::cos(a), r.m[0][2] = std::sin(a), r.m[2][0] = -std::sin(a), r.m[2][2] = std::cos(a);
    return r;
}
}
Camera camera(float yaw, float aspect) {
    Camera c;
    // Half a turn more, so the underside (the graphic) faces the camera at yaw 0.
    c.model = rotate_x(tilt) * rotate_y(yaw + pi);
    const float fov = 28.0f * pi / 180.0f, f = 1.0f / std::tan(fov * 0.5f);
    // Far enough that the whole length fits the height, and the width the window's width.
    const float fit_height = 1.06f, fit_width = 0.40f;
    c.distance = std::max(fit_height * f, fit_width * f / std::max(aspect, 0.05f)) + 0.12f;
    const float near_plane = 0.05f, far_plane = c.distance + 4.0f;
    Matrix projection;
    projection.m[0][0] = f / aspect;
    projection.m[1][1] = f;
    projection.m[2][2] = far_plane / (near_plane - far_plane);
    projection.m[2][3] = near_plane * far_plane / (near_plane - far_plane);
    projection.m[3][2] = -1;
    auto view = Matrix::identity();
    view.m[2][3] = -c.distance;
    c.view_projection = projection * view;
    return c;
}
}
