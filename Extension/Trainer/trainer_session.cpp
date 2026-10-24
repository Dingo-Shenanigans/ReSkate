#include "trainer_session.h"
#include "trainer_classes.h"
#include "Engine/Game/Multiplayer/session_physics.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>

namespace dingosdk::trainer {
namespace {
// version, table id, six multipliers (in version 2 then Underflip speed and a byte: 1 for the under part only), count;
// then an index and a value per class field.
constexpr std::uint8_t version = 1, version_underflip = 2;
constexpr std::size_t header = 1 + 4 + 6 * 4 + 2, header_underflip = header + 4 + 1, row = 2 + 4;
static_assert(header_underflip + class_field_count * row <= max_physics_extras, "every class value must fit in a session's physics extras");
static_assert(class_field_count <= 0xffff);
constexpr float class_value_high = 1.0e6f; // what the trainer holds its own edits to

template <class T> void put(std::vector<std::uint8_t> &out, T value) {
    std::array<std::uint8_t, sizeof(T)> bytes;
    std::memcpy(bytes.data(), &value, sizeof(T));
    out.insert(out.end(), bytes.begin(), bytes.end());
}
template <class T> T take(std::span<const std::uint8_t> bytes, std::size_t &at) {
    T value;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    at += sizeof(T);
    return value;
}
} // namespace

std::uint32_t class_table_id() noexcept {
    // FNV-1a over what an index means: each class's key and each field's name and place.
    static const std::uint32_t id = [] {
        std::uint32_t hash = 2166136261u;
        const auto mix = [&](std::string_view text) {
            for (const char c : text) hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619u;
            hash = (hash ^ 0xffu) * 16777619u;
        };
        for (std::size_t c = 0; c < class_count; ++c) {
            mix(class_specs[c].key);
            for (std::size_t i = 0; i < class_specs[c].count; ++i) {
                const auto &field = class_fields[class_specs[c].first + i];
                mix(field.name);
                hash = (hash ^ field.offset) * 16777619u;
            }
        }
        return hash;
    }();
    return id;
}

std::vector<std::uint8_t> encode_session_extras(const SessionExtras &extras) {
    std::vector<std::uint8_t> out;
    if (extras.stock()) return out;
    const bool underflip = extras.underflip_speed != 1.0f || extras.underflip_split;
    out.reserve((underflip ? header_underflip : header) + extras.classes.size() * row);
    put(out, underflip ? version_underflip : version);
    put(out, class_table_id());
    for (const float value : {extras.flip_speed, extras.hippy_height, extras.nocomply_height, extras.boneless_height,
                              extras.offboard_height, extras.cruise})
        put(out, value);
    if (underflip) {
        put(out, extras.underflip_speed);
        put(out, static_cast<std::uint8_t>(extras.underflip_split ? 1 : 0));
    }
    auto classes = extras.classes;
    std::erase_if(classes, [](const auto &entry) { return entry.first >= class_field_count || !std::isfinite(entry.second); });
    std::ranges::stable_sort(classes, {}, [](const auto &entry) { return entry.first; });
    classes.erase(std::unique(classes.begin(), classes.end(), [](const auto &a, const auto &b) { return a.first == b.first; }), classes.end());
    put(out, static_cast<std::uint16_t>(classes.size()));
    for (const auto &[index, value] : classes) {
        put(out, index);
        put(out, value);
    }
    return out;
}

std::optional<SessionExtras> decode_session_extras(std::span<const std::uint8_t> bytes) {
    SessionExtras extras;
    if (bytes.empty()) return extras;
    const bool underflip = bytes[0] == version_underflip;
    const std::size_t head = underflip ? header_underflip : header;
    if (bytes.size() < head || bytes.size() > max_physics_extras || (bytes[0] != version && !underflip)) return std::nullopt;
    std::size_t at = 1;
    const auto table = take<std::uint32_t>(bytes, at);
    std::array<float, 6> numbers;
    for (auto &number : numbers) {
        number = take<float>(bytes, at);
        if (!std::isfinite(number)) return std::nullopt;
    }
    extras.flip_speed = std::clamp(numbers[0], flip_low, flip_high);
    extras.hippy_height = std::clamp(numbers[1], height_low, height_high);
    extras.nocomply_height = std::clamp(numbers[2], height_low, height_high);
    extras.boneless_height = std::clamp(numbers[3], height_low, height_high);
    extras.offboard_height = std::clamp(numbers[4], height_low, height_high);
    extras.cruise = numbers[5] > 0 && numbers[5] < cruise_high ? numbers[5] : 0.0f;
    if (underflip) {
        const auto value = take<float>(bytes, at);
        if (!std::isfinite(value)) return std::nullopt;
        extras.underflip_speed = std::clamp(value, flip_low, flip_high);
        const auto split = take<std::uint8_t>(bytes, at);
        if (split > 1) return std::nullopt;
        extras.underflip_split = split == 1;
    }
    const std::size_t count = take<std::uint16_t>(bytes, at);
    if (bytes.size() != head + count * row) return std::nullopt;
    const bool same_table = table == class_table_id();
    int previous = -1;
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = take<std::uint16_t>(bytes, at);
        const auto value = take<float>(bytes, at);
        // Another table's rows mean nothing here, but the encoding still has to be one.
        if (!std::isfinite(value) || static_cast<int>(index) <= previous) return std::nullopt;
        previous = index;
        if (!same_table) continue;
        if (index >= class_field_count) return std::nullopt;
        extras.classes.emplace_back(index, std::clamp(value, -class_value_high, class_value_high));
    }
    return extras;
}
} // namespace dingosdk::trainer
