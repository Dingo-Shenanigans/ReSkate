#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A picture composed from a short list of drawing steps and written as a PNG (reskate.picture): what
// the plugin API hands the game (plugin_game.h's picture_make), and the renderer that draws it
// (plugin_picture.cpp, Windows only). Nothing here knows what a picture is of: a step is a fill, a
// tiled or stretched picture, a gradient, a polygon, a line of text, or a layer of those laid down at
// an angle.
namespace dingosdk::plugins::picture {
inline constexpr int most_side = 2048;         // the canvas, a side
inline constexpr int most_layer_side = 4096;   // a layer, a side
inline constexpr int most_steps = 256;         // a layer's inner steps count
inline constexpr int most_points = 20000;      // of all the polygons together
inline constexpr std::size_t most_text_bytes = 512;
// What a picture or a font a step names may be: the limits plugins' drawn pictures have already
// (Extension/UI/Overlay/plugin_images.cpp), and 8 MB for a font.
inline constexpr std::size_t most_picture_bytes = 16u << 20, most_font_bytes = 8u << 20;
inline constexpr std::uint32_t most_picture_side = 4096;
// Every coordinate and size a step gives is within this many pixels of 0, so the drawing's own sums
// stay finite.
inline constexpr float most_coordinate = 1e6f;
// A plugin starts at most this many pictures a second, and has at most this many being drawn.
inline constexpr int most_a_second = 4;
inline constexpr std::size_t most_going = 8;

// Straight alpha, each 0 to 1. "Over" is out = under * (1 - a) + colour * a.
struct Colour {
    float r{}, g{}, b{}, a{1};
    bool operator==(const Colour&) const = default;
};
// A picture or a font a step names. The API fills in `name` and one of `file` (a file of the plugin's
// package, checked to be inside it) and `data` (a picture of its data folder, as data_path checked
// it); the game puts where a data folder's picture is on disk into `file` before it is drawn. One
// with no name is none.
struct File {
    std::string name;           // as the plugin named it
    std::filesystem::path file; // where it is on disk
    std::string data;           // its path in the data folder ("<folder>:<path>" in a granted one)
    bool operator==(const File&) const = default;
};
enum class Align : std::uint8_t { left, center, right };
struct Step {
    enum class Kind : std::uint8_t { fill, tile, image, gradient, layer, poly, text };
    Kind kind{};
    Colour colour;                // fill, gradient (its a is not used), poly, text
    File picture;                 // tile, image, and a poly filled with one
    float texel{1};               // tile, poly: canvas pixels to one texel of the picture
    // image: its box. layer: w and h its size, x and y where its middle goes. text: x where it starts,
    // centres or ends, y its baseline.
    float x{}, y{}, w{}, h{};
    bool cover{};                 // image, fit = "cover": fills the surface keeping its shape (the box is not used)
    float focus{0.5f};            // and then which part shows when it is wider: 0 its left to 1 its right
    float alpha{1};               // image
    float y0{}, y1{}, a0{}, a1{}; // gradient: its rows, and the alpha at each end
    float angle{};                // layer: radians, positive clockwise as seen
    std::vector<Step> steps;      // layer: drawn on it, in its own pixels (poly and text)
    std::vector<std::array<float, 2>> points; // poly: closed, filled even-odd
    File font;                    // text
    std::string text;             // one line, UTF-8
    float size{};                 // text: pixels to the em
    Align align{};
};
struct Request {
    std::string path; // the PNG, in the plugin's data folder, as data_path checked it
    int width{}, height{};
    std::vector<Step> steps;
};
// A picture being drawn. Drawing takes tens of milliseconds (a photo alone is most of a frame to
// decode), so the game draws it off the plugin's thread: whoever draws sets `why` (empty: it was
// written) and then `done`, and a later tick of the plugin calls back with it.
struct Job {
    std::atomic<bool> done{};
    std::string why;
};

// ---- the renderer (plugin_picture.cpp) -------------------------------------------------------------
// Windows reads everything a plugin gives: WIC its pictures, DirectWrite its fonts. Any thread; a
// call holds nothing of the caller's once it returns.

// A decoded picture: 8 bits a channel, red first, colour multiplied by alpha.
struct Pixels {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};
// A PNG or a JPEG's bytes decoded; false for anything else, or one past most_picture_side.
bool decode(std::string_view bytes, Pixels& out);
// A picture file's size in pixels, read from its header; nothing for one that is not a picture a
// step would draw.
std::optional<std::pair<std::uint32_t, std::uint32_t>> size_of(const std::filesystem::path& file);
// How wide a line of text is in a font file at `size` pixels to the em, as a text step lays it out
// (kerning and all); nothing when the font cannot be read. Fonts are kept by their file and its time,
// so asking again is cheap.
std::optional<double> text_width(const std::filesystem::path& font, std::string_view text, float size);
// Draws the request's steps in order on an opaque black canvas and encodes it as a PNG into `png`.
// A step whose picture or font cannot be read draws nothing (a poly then fills with its colour).
// Empty when done, else why not. One picture is drawn at a time in the process, so what they hold
// together stays bounded.
std::string render(const Request& request, std::string& png);
// The same, written to `file` whole or not at all (beside it as "<file>.tmp", then moved into
// place). The request's own path is not used: the caller has placed it.
std::string make(const Request& request, const std::filesystem::path& file);
// The same on a thread of its own, as a plugin's preloads are read: `job` says how it went.
void make_later(Request request, std::filesystem::path file, std::shared_ptr<Job> job);
}
