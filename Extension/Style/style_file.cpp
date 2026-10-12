#include "style_file.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dingosdk::style {
namespace {
constexpr int format = 1;
void check_format(const Json &root) {
    const auto *found = root.is_object() && root.contains("format") ? &root.at("format") : nullptr;
    const double value = found && found->is_number() ? found->get<double>() : 0;
    if (value == format) return;
    if (value > format && value == std::floor(value)) throw NewerStyle();
    throw std::runtime_error("This is not a style this version reads.");
}
void read_joints(const Json &joints, Target target, Rotations &result) {
    if (!joints.is_object()) return;
    for (const auto &[joint, angles] : joints.items()) {
        const auto known = editable_joint(joint);
        if (!known || !angles.is_array() || angles.size() != 3) continue;
        std::array<float, 3> degrees{};
        for (std::size_t i = 0; i < 3; ++i) {
            if (!angles.at(i).is_number()) throw std::runtime_error("A style angle is not a number.");
            const auto value = angles.at(i).get<double>();
            degrees[i] = std::isfinite(value) ? std::clamp(static_cast<float>(value), -max_degrees, max_degrees) : 0.0f;
        }
        if (degrees != std::array<float, 3>{}) result[{target, std::string(editable_joints[*known])}] = degrees;
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
        times.push_back(free_key_time(times, std::clamp(static_cast<float>(time), 0.0f, trick_end)));
        if (key.contains("joints")) read_joints(key.at("joints"), target, result.rotations);
        if (key.contains("blend_out_ms") && key.at("blend_out_ms").is_number())
            if (const auto ms = key.at("blend_out_ms").get<double>(); std::isfinite(ms) && ms > 0)
                result.blend_outs[target] = std::clamp(static_cast<float>(ms), min_blend_out_ms, max_blend_out_ms);
    }
    result.times[target.id] = std::move(times);
}
} // namespace

std::string encode_style(const Style &style) {
    auto root = Json::object();
    root["format"] = format;
    root["tricks"] = Json::object();
    root["states"] = Json::object();
    // Each trick's keyframes in time order, and each keyframe's place in time by ID.
    std::map<std::uint8_t, std::vector<std::size_t>> places;
    for (const auto &[trick, times] : style.times) {
        auto &keys = root["tricks"][flip_trick_names[trick]];
        keys = Json::array();
        auto sorted = times;
        std::ranges::sort(sorted);
        for (const auto time : sorted) {
            auto key = Json::object();
            key["time"] = time;
            key["joints"] = Json::object();
            keys.push_back(std::move(key));
        }
        auto &place = places[trick];
        for (std::size_t key = 0; key < times.size(); ++key) place.push_back(key_place(times, key));
    }
    const auto saved_key = [&](const Target &target) -> Json * {
        const auto found = places.find(target.id);
        if (!target.trick || found == places.end() || target.key >= found->second.size()) return nullptr;
        return &root["tricks"][flip_trick_names[target.id]][found->second[target.key]];
    };
    for (const auto &[target, ms] : style.blend_outs)
        if (auto *key = saved_key(target)) (*key)["blend_out_ms"] = ms;
    for (const auto &[key, degrees] : style.rotations) {
        const auto angles = Json::array({degrees[0], degrees[1], degrees[2]});
        if (!key.target.trick) root["states"][family_names[key.target.id]][key.joint] = angles;
        else if (auto *saved = saved_key(key.target)) (*saved)["joints"][key.joint] = angles;
    }
    return root.dump(2) + "\n";
}

Style decode_style(std::string_view text) {
    if (text.size() > maximum_style_bytes) throw std::runtime_error("The style file is too large.");
    const auto root = Json::parse(text, JsonLimits{maximum_style_bytes, 8, 16384});
    check_format(root);
    Style result;
    TrickSet read; // a name in another case is the same trick, and the first one counts
    if (const auto *tricks = group(root, "tricks"))
        for (const auto &[name, keys] : tricks->items()) {
            auto target = parse_target(name);
            if (!target || !target->trick || read.test(target->id)) continue;
            read.set(target->id);
            read_keys(keys, *target, result);
        }
    if (const auto *states = group(root, "states"))
        for (const auto &[name, joints] : states->items())
            if (const auto target = parse_target(name); target && !target->trick) read_joints(joints, *target, result.rotations);
    return result;
}
} // namespace dingosdk::style
