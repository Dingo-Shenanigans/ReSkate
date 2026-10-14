#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::graphics_creator {

inline constexpr std::uint32_t current_schema_version = 2;
inline constexpr std::size_t maximum_layers = 128;

struct Canvas {
    std::uint32_t width = 1024;
    std::uint32_t height = 1024;
    bool operator==(const Canvas&) const = default;
};

struct Color {
    std::uint8_t red = 255;
    std::uint8_t green = 255;
    std::uint8_t blue = 255;
    std::uint8_t alpha = 255;
    bool operator==(const Color&) const = default;
};

struct Transform {
    float x{};
    float y{};
    float rotation_degrees{};
    float scale_x = 1;
    float scale_y = 1;
    bool operator==(const Transform&) const = default;
};

enum class ShapeKind { rectangle, ellipse, triangle, diamond, star, hexagon, heart, cross, bolt, image };
enum class DesignTarget { sticker, board, beanie, hoodie, shirt, shoes };

struct Shape {
    ShapeKind kind = ShapeKind::rectangle;
    float width = 256;
    float height = 256;
    Color fill{};
    std::string source; // A safe file name in Create/Assets for image layers; never an arbitrary path.
    bool operator==(const Shape&) const = default;
};

struct Layer {
    std::string id;
    std::string name;
    bool visible = true;
    float opacity = 1;
    Transform transform{};
    Shape shape{};
    bool operator==(const Layer&) const = default;
};

struct GraphicDocument {
    std::uint32_t schema_version = current_schema_version;
    std::string id;
    std::string name;
    Canvas canvas{};
    std::vector<Layer> layers;
    DesignTarget target = DesignTarget::sticker;
    bool operator==(const GraphicDocument&) const = default;
};

bool valid_identifier(std::string_view value);
bool valid_asset_name(std::string_view value);
bool valid_display_name(std::string_view value);
void validate(const GraphicDocument& document);
std::string encode(const GraphicDocument& document);
GraphicDocument decode(std::string_view text);

} // namespace dingosdk::graphics_creator
