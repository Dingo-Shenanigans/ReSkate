#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The pictures a board turnaround is made of: straight (not premultiplied) 8-bit RGBA, rows top to
// bottom with no padding. Kept free of Windows and the game so the encoders and matting can be
// tested anywhere (Extension/Rendering/Turnaround/Test).
namespace dingosdk::turnaround {
using Rgb = std::array<std::uint8_t, 3>;

struct Image {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba; // width * height * 4

    Image() = default;
    Image(std::uint32_t w, std::uint32_t h) : width(w), height(h), rgba(std::size_t{w} * h * 4, 0) {}
    bool empty() const noexcept { return !width || !height; }
    std::uint8_t* at(std::uint32_t x, std::uint32_t y) noexcept { return rgba.data() + (std::size_t{y} * width + x) * 4; }
    const std::uint8_t* at(std::uint32_t x, std::uint32_t y) const noexcept {
        return rgba.data() + (std::size_t{y} * width + x) * 4;
    }
};

// A rectangle of pixels in a captured frame.
struct Rect {
    std::uint32_t x{}, y{}, width{}, height{};
    bool empty() const noexcept { return !width || !height; }
};

// Rows of 4-byte pixels as read back from the GPU: `pitch` bytes apart, channel order given by `bgra`.
struct PixelView {
    const std::uint8_t* data{};
    std::uint32_t width{}, height{}, pitch{};
    bool bgra{};
    // The game's depth for the same frame, when there is one: 32-bit floats a pixel, reversed (1
    // near, 0 at the far plane), `depth_pitch` bytes a row, the frame's size.
    const std::uint8_t* depth{};
    std::uint32_t depth_pitch{};
    float depth_at(std::uint32_t x, std::uint32_t y) const noexcept {
        float v;
        std::memcpy(&v, depth + std::size_t{y} * depth_pitch + std::size_t{x} * 4, 4);
        return v;
    }
    std::array<std::uint8_t, 3> rgb(std::uint32_t x, std::uint32_t y) const noexcept {
        const auto* p = data + std::size_t{y} * pitch + std::size_t{x} * 4;
        return bgra ? std::array<std::uint8_t, 3>{p[2], p[1], p[0]} : std::array<std::uint8_t, 3>{p[0], p[1], p[2]};
    }
};

// `image` flattened over `matte`: every pixel opaque.
Image over_matte(const Image& image, Rgb matte);
// The smallest rectangle holding every pixel at least `threshold` opaque, or an empty one.
Rect content_bounds(const Image& image, std::uint8_t threshold = 8);
// The union of two rectangles (an empty one counts as nothing).
Rect united(Rect a, Rect b) noexcept;
// `rect` grown by `margin` on every side and made square, kept inside `width` x `height`.
Rect padded_square(Rect rect, std::uint32_t margin, std::uint32_t width, std::uint32_t height) noexcept;
// A square around `content`, `margin` wider on every side, centred on it: it may reach past the
// image, and what is outside is transparent (a board taller than the area it was taken from).
Image square_around(const Image& image, Rect content, std::uint32_t margin);
// The part of `image` inside `rect`; pixels outside the image are transparent.
Image cropped(const Image& image, Rect rect);
// `image` resized to exactly `width` x `height` (filtered as fitted is).
Image resized(const Image& image, std::uint32_t width, std::uint32_t height);
// `image` resized to fit inside `size` x `size` (keeping its shape, centred, transparent around it),
// filtered in premultiplied alpha so edges do not pick up the colour of transparent pixels.
Image fitted(const Image& image, std::uint32_t size);
// `b` placed to the right of `a`, both centred vertically on a canvas as tall as the taller.
Image side_by_side(const Image& a, const Image& b);
} // namespace dingosdk::turnaround
