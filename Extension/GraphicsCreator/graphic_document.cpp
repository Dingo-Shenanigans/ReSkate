#include "graphic_document.h"
#include "Engine/Core/Json/json.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace dingosdk::graphics_creator {
namespace {
constexpr std::size_t maximum_document_bytes = 1024 * 1024;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool finite(float value) { return std::isfinite(value); }

const char* shape_name(ShapeKind kind) {
    switch (kind) {
    case ShapeKind::rectangle: return "rectangle";
    case ShapeKind::ellipse: return "ellipse";
    case ShapeKind::triangle: return "triangle";
    case ShapeKind::diamond: return "diamond";
    case ShapeKind::star: return "star";
    case ShapeKind::image: return "image";
    }
    throw std::runtime_error("Unknown graphic shape kind.");
}

ShapeKind shape_kind(std::string_view value) {
    if (value == "rectangle") return ShapeKind::rectangle;
    if (value == "ellipse") return ShapeKind::ellipse;
    if (value == "triangle") return ShapeKind::triangle;
    if (value == "diamond") return ShapeKind::diamond;
    if (value == "star") return ShapeKind::star;
    if (value == "image") return ShapeKind::image;
    throw std::runtime_error("Graphic layer has an unsupported shape kind.");
}

const char* target_name(DesignTarget target) {
    switch (target) {
    case DesignTarget::sticker: return "sticker";
    case DesignTarget::board: return "board";
    case DesignTarget::beanie: return "beanie";
    case DesignTarget::hoodie: return "hoodie";
    case DesignTarget::shirt: return "shirt";
    }
    throw std::runtime_error("Unknown graphic design target.");
}

DesignTarget target_kind(std::string_view value) {
    if (value == "sticker") return DesignTarget::sticker;
    if (value == "board") return DesignTarget::board;
    if (value == "beanie") return DesignTarget::beanie;
    if (value == "hoodie") return DesignTarget::hoodie;
    if (value == "shirt") return DesignTarget::shirt;
    throw std::runtime_error("Graphic document has an unsupported design target.");
}

const Json& required(const Json& object, std::string_view key) {
    require(object.is_object() && object.contains(key), "Graphic document is missing a required field.");
    return object.at(key);
}

std::string required_string(const Json& object, std::string_view key) {
    const auto& value = required(object, key);
    require(value.is_string(), "Graphic document field must be text.");
    return value.string();
}

float required_float(const Json& object, std::string_view key) {
    const auto& value = required(object, key);
    require(value.is_number(), "Graphic document field must be numeric.");
    return value.get<float>();
}

std::uint32_t required_unsigned(const Json& object, std::string_view key) {
    const auto& value = required(object, key);
    require(value.is_number_integer(), "Graphic document field must be an integer.");
    const auto number = value.get<std::int64_t>();
    require(number >= 0 && number <= std::numeric_limits<std::uint32_t>::max(),
            "Graphic document integer is out of range.");
    return static_cast<std::uint32_t>(number);
}

bool required_bool(const Json& object, std::string_view key) {
    const auto& value = required(object, key);
    require(value.is_boolean(), "Graphic document field must be true or false.");
    return value.get<bool>();
}

Json color_json(const Color& color) {
    return Json::array({color.red, color.green, color.blue, color.alpha});
}

Color decode_color(const Json& value) {
    require(value.is_array() && value.size() == 4, "Graphic fill color must contain four channels.");
    Color color;
    auto* channels = &color.red;
    for (std::size_t i = 0; i < 4; ++i) {
        require(value.at(i).is_number_integer(), "Graphic fill color channels must be integers.");
        const auto channel = value.at(i).get<std::int64_t>();
        require(channel >= 0 && channel <= 255, "Graphic fill color channel is out of range.");
        channels[i] = static_cast<std::uint8_t>(channel);
    }
    return color;
}
} // namespace

bool valid_identifier(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    for (const auto c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) return false;
    return true;
}

bool valid_display_name(std::string_view value) {
    if (value.empty() || value.size() > 96 || value.front() == ' ' || value.back() == ' ') return false;
    for (const auto c : value)
        if (static_cast<unsigned char>(c) < 32 || c == 127) return false;
    return true;
}

bool valid_asset_name(std::string_view value) {
    if (value.empty() || value.size() > 96 || value.front() == '.' || value.find("..") != std::string_view::npos)
        return false;
    for (const auto c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.')) return false;
    return value.ends_with(".png") || value.ends_with(".jpg") || value.ends_with(".jpeg");
}

void validate(const GraphicDocument& document) {
    require(document.schema_version == current_schema_version, "Unsupported graphic document version.");
    require(valid_identifier(document.id), "Graphic document ID must use 1-64 letters, numbers, hyphens or underscores.");
    require(valid_display_name(document.name), "Graphic document name is invalid.");
    require(document.canvas.width >= 64 && document.canvas.width <= 4096 &&
            document.canvas.height >= 64 && document.canvas.height <= 4096,
            "Graphic canvas must be between 64 and 4096 pixels on each side.");
    require(document.layers.size() <= maximum_layers, "Graphic document has too many layers.");

    std::unordered_set<std::string> ids;
    for (const auto& layer : document.layers) {
        require(valid_identifier(layer.id), "Graphic layer ID is invalid.");
        require(ids.insert(layer.id).second, "Graphic layer IDs must be unique.");
        require(valid_display_name(layer.name), "Graphic layer name is invalid.");
        require(finite(layer.opacity) && layer.opacity >= 0 && layer.opacity <= 1,
                "Graphic layer opacity must be between zero and one.");
        require(finite(layer.transform.x) && finite(layer.transform.y) &&
                std::abs(layer.transform.x) <= 16384 && std::abs(layer.transform.y) <= 16384,
                "Graphic layer position is invalid.");
        require(finite(layer.transform.rotation_degrees) &&
                std::abs(layer.transform.rotation_degrees) <= 360000,
                "Graphic layer rotation is invalid.");
        require(finite(layer.transform.scale_x) && finite(layer.transform.scale_y) &&
                layer.transform.scale_x >= .01f && layer.transform.scale_x <= 100 &&
                layer.transform.scale_y >= .01f && layer.transform.scale_y <= 100,
                "Graphic layer scale must be between 0.01 and 100.");
        require(finite(layer.shape.width) && finite(layer.shape.height) &&
                layer.shape.width >= 1 && layer.shape.width <= 8192 &&
                layer.shape.height >= 1 && layer.shape.height <= 8192,
                "Graphic shape dimensions must be between 1 and 8192 pixels.");
        (void)shape_name(layer.shape.kind);
        if (layer.shape.kind == ShapeKind::image)
            require(valid_asset_name(layer.shape.source), "Graphic image layer has an invalid local asset name.");
        else
            require(layer.shape.source.empty(), "Only image layers may reference a local asset.");
    }
    (void)target_name(document.target);
}

std::string encode(const GraphicDocument& document) {
    validate(document);
    Json root{{"schema_version", document.schema_version}, {"id", document.id}, {"name", document.name},
              {"canvas", Json{{"width", document.canvas.width}, {"height", document.canvas.height}}},
              {"target", target_name(document.target)},
              {"layers", Json::array()}};
    for (const auto& layer : document.layers) {
        root["layers"].push_back(Json{
            {"id", layer.id}, {"name", layer.name}, {"visible", layer.visible}, {"opacity", layer.opacity},
            {"transform", Json{{"x", layer.transform.x}, {"y", layer.transform.y},
                                {"rotation_degrees", layer.transform.rotation_degrees},
                                {"scale_x", layer.transform.scale_x}, {"scale_y", layer.transform.scale_y}}},
            {"shape", Json{{"kind", shape_name(layer.shape.kind)}, {"width", layer.shape.width},
                            {"height", layer.shape.height}, {"fill_rgba", color_json(layer.shape.fill)},
                            {"source", layer.shape.source}}}});
    }
    return root.dump(2) + '\n';
}

GraphicDocument decode(std::string_view text) {
    require(text.size() <= maximum_document_bytes, "Graphic document exceeds 1 MB.");
    const auto root = Json::parse(text, {maximum_document_bytes, 16, 8192});
    require(root.is_object(), "Graphic document root must be an object.");

    GraphicDocument document;
    const auto stored_version = required_unsigned(root, "schema_version");
    require(stored_version == 1 || stored_version == current_schema_version,
            "Unsupported graphic document version.");
    document.schema_version = current_schema_version;
    document.id = required_string(root, "id");
    document.name = required_string(root, "name");
    const auto& canvas = required(root, "canvas");
    document.canvas = {required_unsigned(canvas, "width"), required_unsigned(canvas, "height")};
    document.target = stored_version >= 2 ? target_kind(required_string(root, "target")) : DesignTarget::sticker;
    const auto& layers = required(root, "layers");
    require(layers.is_array() && layers.size() <= maximum_layers, "Graphic layers must be a bounded array.");
    for (const auto& value : layers) {
        require(value.is_object(), "Graphic layer must be an object.");
        Layer layer;
        layer.id = required_string(value, "id");
        layer.name = required_string(value, "name");
        layer.visible = required_bool(value, "visible");
        layer.opacity = required_float(value, "opacity");
        const auto& transform = required(value, "transform");
        layer.transform = {required_float(transform, "x"), required_float(transform, "y"),
                           required_float(transform, "rotation_degrees"), required_float(transform, "scale_x"),
                           required_float(transform, "scale_y")};
        const auto& shape = required(value, "shape");
        layer.shape = {shape_kind(required_string(shape, "kind")), required_float(shape, "width"),
                       required_float(shape, "height"), decode_color(required(shape, "fill_rgba")),
                       stored_version >= 2 ? required_string(shape, "source") : std::string{}};
        document.layers.push_back(std::move(layer));
    }
    validate(document);
    return document;
}

} // namespace dingosdk::graphics_creator
