#include "style_file.h"
#include "Engine/Core/Json/json.h"
#include <stdexcept>

namespace dingosdk::style {
namespace {
constexpr int format = 2;
void read_joints(const Json &joints, Target target, Rotations &result) {
    if (!joints.is_object()) return;
    for (const auto &[joint, angles] : joints.items()) {
        const auto known = std::ranges::find(editable_joints, std::string_view(joint));
        if (known == editable_joints.end() || !angles.is_array() || angles.size() != 3) continue;
        std::array<float, 3> degrees{};
        for (std::size_t i = 0; i < 3; ++i) {
            if (!angles.at(i).is_number()) throw std::runtime_error("A style angle is not a number.");
            const auto value = angles.at(i).get<double>();
            degrees[i] = std::isfinite(value) ? std::clamp(static_cast<float>(value), -max_degrees, max_degrees) : 0.0f;
        }
        if (degrees != std::array<float, 3>{}) result[{target, std::string(*known)}] = degrees;
    }
}
const Json *group(const Json &root, std::string_view name) {
    if (!root.contains(name)) return nullptr;
    const auto &targets = root.at(name);
    if (!targets.is_object()) throw std::runtime_error("The style's " + std::string(name) + " are not an object.");
    return &targets;
}
void read_keys(const Json &keys, Target target, Style &result) {
    if (!keys.is_array()) return;
    std::vector<float> times;
    for (std::size_t i = 0; i < keys.size() && times.size() < max_keys; ++i) {
        const auto &key = keys.at(i);
        if (!key.is_object() || !key.contains("time") || !key.at("time").is_number()) continue;
        const auto time = key.at("time").get<double>();
        if (!std::isfinite(time)) continue;
        target.key = static_cast<std::uint8_t>(times.size());
        times.push_back(std::clamp(static_cast<float>(time), 0.0f, trick_end));
        if (key.contains("joints")) read_joints(key.at("joints"), target, result.rotations);
    }
    result.times[target.id] = std::move(times);
}
} // namespace

std::string encode_style(const Style &style) {
    auto root = Json::object();
    root["format"] = format;
    root["skeleton_joints"] = 395;
    root["tricks"] = Json::object();
    root["states"] = Json::object();
    const auto keys_of = [&](std::uint8_t trick) -> Json & {
        auto &keys = root["tricks"][flip_trick_names[trick]];
        if (keys.is_null()) {
            keys = Json::array();
            for (const auto time : style.keys(trick)) {
                auto key = Json::object();
                key["time"] = time;
                key["joints"] = Json::object();
                keys.push_back(std::move(key));
            }
        }
        return keys;
    };
    for (const auto &[trick, times] : style.times) (void)keys_of(trick);
    for (const auto &[key, degrees] : style.rotations) {
        const auto angles = Json::array({degrees[0], degrees[1], degrees[2]});
        if (!key.first.trick) root["states"][family_names[key.first.id]][key.second] = angles;
        else if (auto &keys = keys_of(key.first.id); key.first.key < keys.size()) keys[key.first.key]["joints"][key.second] = angles;
    }
    return root.dump(2) + "\n";
}

Style decode_style(std::string_view text) {
    if (text.size() > maximum_style_bytes) throw std::runtime_error("The style file is too large.");
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    const auto root = Json::parse(text, JsonLimits{maximum_style_bytes, 8, 16384});
    if (!root.is_object() || root.value("format", 0) != format) throw std::runtime_error("This is not a style this version reads.");
    Style result;
    if (const auto *tricks = group(root, "tricks"))
        for (const auto &[name, keys] : tricks->items()) {
            auto target = parse_target(name);
            if (!target || !target->trick) continue;
            read_keys(keys, *target, result);
        }
    if (const auto *states = group(root, "states"))
        for (const auto &[name, joints] : states->items())
            if (const auto target = parse_target(name); target && !target->trick) read_joints(joints, *target, result.rotations);
    return result;
}
} // namespace dingosdk::style
