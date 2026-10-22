#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dingosdk::bmx::startup_detail {
inline constexpr std::uint32_t event_hash = 3835844145U, force_field_hash = 2101345324U;
// Native GUID byte order: the first three canonical GUID components are little endian.
inline constexpr std::array<std::uint8_t, 16> event_guid{
    0xc0,0x29,0xa4,0x42,0x5c,0xfd,0xda,0x62,0xbf,0x8d,0x20,0x00,0x0e,0xe6,0x49,0x68};
inline bool asset_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        if (fold(left[i]) != fold(right[i])) return false;
    }
    return true;
}
inline bool recovery_world(std::string_view root, std::string_view detached) noexcept {
    return asset_equal(root, "levels/game/dingolevel_root/dingolevel_root") &&
        (asset_equal(detached, "levels/game/bam_levelroot/bam_levelroot") ||
            asset_equal(detached, "levels/game/dingolevel_mpr/dingolevel_mpr"));
}
template<class Read> bool event_type_contract(std::uintptr_t object, std::uintptr_t bool_type, Read&& read) noexcept {
    std::uintptr_t record{}, fields{}, field_type{}, array{}, array_record{}, element{};
    std::uint32_t hash{}, field_hash{};
    std::uint16_t flags{}, size{}, count{}, offset{};
    std::array<std::uint8_t, 16> guid{};
    const auto get = [&read]<class T>(std::uintptr_t address, T& value) noexcept {
        return address >= 0x10000 && address <= 0x00007fffffffffffULL - sizeof(T) && read(address, &value, sizeof(T));
    };
    const bool valid = get(object, record) && get(record, hash) && hash == event_hash &&
        get(record + 4, flags) && ((flags >> 5) & 31) == 2 && get(record + 6, size) && size == 1 &&
        get(record + 8, guid) && guid == event_guid && get(record + 0x2a, count) && count == 1 &&
        get(record + 0x60, fields) && get(fields, field_hash) && field_hash == force_field_hash &&
        get(fields + 8, offset) && offset == 0 && get(fields + 0x10, field_type) && field_type == bool_type &&
        get(record + 0x20, array) && get(array, array_record) && get(array_record + 0x30, element) && element == object;
    if (!valid) return false;
    std::uintptr_t after_record{}, after_fields{}, after_field_type{};
    std::uint16_t after_size{}, after_count{}, after_offset{};
    std::array<std::uint8_t, 16> after_guid{};
    return get(object, after_record) && after_record == record && get(record, hash) && hash == event_hash &&
        get(record + 6, after_size) && after_size == 1 && get(record + 8, after_guid) && after_guid == event_guid &&
        get(record + 0x2a, after_count) && after_count == 1 && get(record + 0x60, after_fields) && after_fields == fields &&
        get(fields, field_hash) && field_hash == force_field_hash && get(fields + 8, after_offset) && after_offset == 0 &&
        get(fields + 0x10, after_field_type) && after_field_type == bool_type;
}
struct WorldKey {
    std::uintptr_t client{}, context{}, root{}, player{};
    std::uint64_t generation{};
    bool operator==(const WorldKey&) const = default;
};
class AttemptGate {
public:
    bool consume(const WorldKey& key) noexcept {
        if (!key.client || !key.context || !key.root || !key.player || exhausted_) return false;
        for (std::size_t i = 0; i < count_; ++i) if (attempted_[i] == key) return false;
        if (count_ == attempted_.size()) { exhausted_ = true; return false; }
        attempted_[count_++] = key;
        return true;
    }
private:
    std::array<WorldKey, 32> attempted_{};
    std::size_t count_{};
    bool exhausted_{};
};
}
