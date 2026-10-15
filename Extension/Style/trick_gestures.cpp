#include "trick_gestures.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/style.h"
#include "Engine/Core/Console/command_registry.h"
#include "Engine/Resource/ebx_fields.h"
#include "Engine/Vfs/game_bundles.h"
#include "Extension/Style/style_pose.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dingosdk::style {
namespace {
namespace fb = frostbite;
namespace ebx = frostbite::ebx;

// A field of the type the game's data has.
template <class T> const T &get(const ebx::Object &object, std::string_view name) {
    const auto what = "a gesture's " + std::string(name) + " is missing or of another type";
    return ebx::as<T>(ebx::field(object, name, what), what);
}
const ebx::Object &object_of(const ebx::Value &value) {
    const auto &object = ebx::as<std::shared_ptr<ebx::Object>>(value, "a gesture value is not an object");
    if (!object) throw std::runtime_error("a gesture value is empty");
    return *object;
}
XINPUT_GAMEPAD stick(float x, float y) {
    XINPUT_GAMEPAD pad{};
    pad.sThumbRX = stick_axis(x);
    pad.sThumbRY = stick_axis(y);
    return pad;
}
// `degrees` is clockwise from up.
std::array<float, 2> at_angle(float degrees, float magnitude) {
    const float radians = degrees / degrees_per_radian;
    return {std::sin(radians) * magnitude, std::cos(radians) * magnitude};
}
// The middle of the shorter way between two angles.
float middle(float from, float to) { return std::fmod(from + std::remainder(to - from, 360.0f) / 2 + 360.0f, 360.0f); }
// The first point is the crouch, at the stick's edge: a stick held between the middle and the edge is a manual.
std::vector<std::array<float, 2>> arc_points(const TrickGesture &gesture) {
    std::vector<std::array<float, 2>> points;
    for (const auto &arc : gesture.arcs) {
        // An arc near the middle is the stick let go.
        float magnitude = arc.high <= 0.25f ? 0.0f : (arc.low + arc.high) / 2;
        if (points.empty() && arc.high >= 1.0f) magnitude = 1.0f;
        points.push_back(at_angle(middle(arc.from, arc.to), magnitude));
    }
    return points;
}
// The trick: AnimationEnum points to an instance of this document whose Value is the FlipTrickType.
std::optional<std::uint8_t> trick_of(const ebx::Document &document, const ebx::Object &root) {
    const auto *found = root.find("AnimationEnum");
    const auto *pointer = found ? std::get_if<ebx::PointerReference>(&found->value.data) : nullptr;
    if (!pointer || pointer->kind != ebx::PointerKind::internal || pointer->index < 0 ||
        static_cast<std::size_t>(pointer->index) >= document.instances.size())
        return std::nullopt;
    const auto &instance = document.instances[static_cast<std::size_t>(pointer->index)];
    if (!instance.object) return std::nullopt;
    return flip_trick_type(get<std::int64_t>(*instance.object, "Value"));
}
} // namespace

std::optional<std::uint8_t> flip_trick_type(std::int64_t value) noexcept {
    if (value < 1 || value > 63) return std::nullopt;
    return static_cast<std::uint8_t>(value);
}
std::int16_t stick_axis(float value) noexcept {
    if (!std::isfinite(value)) return 0;
    return static_cast<std::int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767);
}
// The game reads the stick once a tick and takes the pop height and flip from the move's speed. The board lifts when the stick
// leaves the crouch and pops when the game knows the trick, so each arc gets no more ticks than the flick needs.
std::vector<overlay::PadStep> flick(const TrickGesture &gesture, std::uint32_t flick_ms) {
    constexpr std::uint32_t crouch = 18, release = 4, end = 36;
    const auto points = arc_points(gesture);
    if (points.empty()) return {};
    const auto moves = static_cast<std::uint32_t>(std::max<std::size_t>(points.size() - 1, 1));
    const std::uint32_t total = std::max(moves, to_ticks(std::min(flick_ms, max_flick_ms)));
    std::vector<overlay::PadStep> script{{0, stick(points[0][0], points[0][1])}};
    std::uint32_t tick = crouch;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto &a = points[i - 1], &b = points[i];
        // After the move out of the crouch, the stick turns 90 degrees at most in a tick: the game misses an arc after a longer turn.
        const float turn = std::abs(wrap_angle(std::atan2(b[0], b[1]) - std::atan2(a[0], a[1]))) * degrees_per_radian;
        const auto least = i > 1 && std::isfinite(turn) ? static_cast<std::uint32_t>(std::ceil(turn / 90.0f)) : 1u;
        const std::uint32_t share = std::max(total / moves + (i - 1 < total % moves ? 1 : 0), least);
        for (std::uint32_t t = 1; t <= share; ++t) {
            const float f = static_cast<float>(t) / static_cast<float>(share);
            script.push_back({tick + t - 1, stick(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)});
        }
        tick += share;
    }
    script.push_back({tick - 1 + release, stick(0, 0)});
    script.push_back({tick - 1 + end, stick(0, 0)});
    return script;
}

std::vector<TrickGesture> read_trick_gestures(const std::filesystem::path &game_root) {
    const vfs::GameData data(game_root);
    const auto bundle = data.read_bundle(data.read_toc(addr::style::gestures_toc), addr::style::gestures_bundle);
    if (!bundle) throw std::runtime_error("the game's level root bundle was not found");
    std::vector<TrickGesture> result;
    for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i) {
        const auto name = console::lower(bundle->manifest.ebx[i].name);
        // The ground tricks only: the others are out of a grind or an underflip.
        if (!name.starts_with(addr::style::gesture_prefix) || name.ends_with("_fromgrind") || name.ends_with("_underflip")) continue;
        const auto *payload = bundle->payload(fb::AssetKind::ebx, i);
        if (!payload) continue;
        const auto document = ebx::read_document(data.read(*payload));
        const auto *root = document.root();
        if (!root || !root->object || document.rootType != "DelMarGameplayGesture") continue;
        const auto trick = trick_of(document, *root->object);
        if (!trick) continue;
        const auto &gesture_data = *root->object;
        const auto decimal = [](const ebx::Object &object, std::string_view name) { return static_cast<float>(get<double>(object, name)); };
        TrickGesture gesture{*trick, get<bool>(gesture_data, "IsNollie"), decimal(gesture_data, "HoldPatternMinAngleDeg"),
                             decimal(gesture_data, "HoldPatternMaxAngleDeg"), decimal(gesture_data, "HoldPatternMinMagnitude"), {}};
        for (const auto &value : get<ebx::Value::Array>(gesture_data, "Patterns")) {
            const auto &pattern = object_of(value);
            if (get<std::int64_t>(pattern, "Mode") != 1 || !gesture.arcs.empty()) continue;
            for (const auto &arc : get<ebx::Value::Array>(pattern, "Arcs")) {
                const auto &a = object_of(arc);
                const auto turned = [&](std::string_view angle) { return std::fmod(decimal(a, angle) + 180.0f, 360.0f); };
                gesture.arcs.push_back({turned("StartAngle"), turned("EndAngle"), decimal(a, "MinMagnitude"), decimal(a, "MaxMagnitude"),
                                        decimal(a, "OverrideMaxTimeWaitingForArc")});
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
