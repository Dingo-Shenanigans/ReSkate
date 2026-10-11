#include "trick_gestures.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

namespace dingosdk::style {
namespace {
namespace fb = frostbite;
namespace ebx = frostbite::ebx;
constexpr std::string_view toc_name = "Win32/levels/game/bam_levelroot/bam_levelroot";
constexpr std::string_view bundle_name = "win32/levels/game/bam_levelroot/bam_levelroot";
constexpr std::string_view prefix = "gameplay/input/gestures/fliptricks/gesture_fliptrick_";

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
const ebx::Value *field(const ebx::Object &object, std::string_view name) {
    const auto *found = object.find(name);
    return found ? &found->value : nullptr;
}
double number(const ebx::Object &object, std::string_view name) {
    const auto *value = field(object, name);
    if (!value) throw std::runtime_error("a gesture has no " + std::string(name));
    if (const auto *d = std::get_if<double>(&value->data)) return *d;
    if (const auto *i = std::get_if<std::int64_t>(&value->data)) return static_cast<double>(*i);
    if (const auto *u = std::get_if<std::uint64_t>(&value->data)) return static_cast<double>(*u);
    if (const auto *b = std::get_if<bool>(&value->data)) return *b ? 1 : 0;
    throw std::runtime_error("a gesture's " + std::string(name) + " is not a number");
}
const ebx::Value::Array &array(const ebx::Object &object, std::string_view name) {
    const auto *value = field(object, name);
    const auto *values = value ? std::get_if<ebx::Value::Array>(&value->data) : nullptr;
    if (!values) throw std::runtime_error("a gesture has no " + std::string(name));
    return *values;
}
const ebx::Object &object_of(const ebx::Value &value) {
    const auto *object = std::get_if<std::shared_ptr<ebx::Object>>(&value.data);
    if (!object || !*object) throw std::runtime_error("a gesture value is not an object");
    return **object;
}
// The trick: AnimationEnum points to an instance of this document whose Value is the FlipTrickType.
std::optional<std::uint8_t> trick_of(const ebx::Document &document, const ebx::Object &root) {
    const auto *value = field(root, "AnimationEnum");
    const auto *pointer = value ? std::get_if<ebx::PointerReference>(&value->data) : nullptr;
    if (!pointer || pointer->kind != ebx::PointerKind::internal || pointer->index < 0 ||
        static_cast<std::size_t>(pointer->index) >= document.instances.size())
        return std::nullopt;
    const auto &instance = document.instances[static_cast<std::size_t>(pointer->index)];
    if (!instance.object) return std::nullopt;
    const auto trick = number(*instance.object, "Value");
    if (trick < 1 || trick > 63) return std::nullopt;
    return static_cast<std::uint8_t>(trick);
}
} // namespace

std::vector<TrickGesture> read_trick_gestures(const std::filesystem::path &game_root) {
    const vfs::GameData data(game_root);
    const auto bundle = data.read_bundle(data.read_toc(std::string(toc_name) + ".toc"), bundle_name);
    if (!bundle) throw std::runtime_error("the game's level root bundle was not found");
    std::vector<TrickGesture> result;
    for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i) {
        const auto name = lower(bundle->manifest.ebx[i].name);
        // The ground tricks only: the others are out of a grind or an underflip.
        if (!name.starts_with(prefix) || name.ends_with("_fromgrind") || name.ends_with("_underflip")) continue;
        const auto *payload = bundle->payload(fb::AssetKind::ebx, i);
        if (!payload) continue;
        const auto document = ebx::read_document(data.read(*payload));
        const auto *root = document.root();
        if (!root || !root->object || document.rootType != "DelMarGameplayGesture") continue;
        const auto trick = trick_of(document, *root->object);
        if (!trick) continue;
        TrickGesture gesture{*trick, number(*root->object, "IsNollie") != 0, static_cast<float>(number(*root->object, "HoldPatternMinAngleDeg")),
                             static_cast<float>(number(*root->object, "HoldPatternMaxAngleDeg")),
                             static_cast<float>(number(*root->object, "HoldPatternMinMagnitude")), {}};
        // An arc pattern lists its arcs in time order, turned half a circle from the stick's axes.
        for (const auto &value : array(*root->object, "Patterns")) {
            const auto &pattern = object_of(value);
            if (number(pattern, "Mode") != 1 || !gesture.arcs.empty()) continue;
            for (const auto &arc : array(pattern, "Arcs")) {
                const auto &a = object_of(arc);
                const auto turned = [&](std::string_view angle) { return std::fmod(static_cast<float>(number(a, angle)) + 180.0f, 360.0f); };
                gesture.arcs.push_back({turned("StartAngle"), turned("EndAngle"), static_cast<float>(number(a, "MinMagnitude")),
                                        static_cast<float>(number(a, "MaxMagnitude")), static_cast<float>(number(a, "OverrideMaxTimeWaitingForArc"))});
            }
        }
        if (!gesture.arcs.empty() && std::ranges::none_of(result, [&](const TrickGesture &known) { return known.trick == gesture.trick; }))
            result.push_back(std::move(gesture));
    }
    if (result.empty()) throw std::runtime_error("the game's data has no flip trick gestures");
    std::ranges::sort(result, {}, &TrickGesture::trick);
    return result;
}
} // namespace dingosdk::style
