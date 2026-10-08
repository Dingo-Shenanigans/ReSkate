#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dingosdk {
// Host policy for who may place, move or delete objects in a session. Wire
// values 0/1 match the older on/off editor byte (off = host only).
enum class ObjectPlacement : std::uint8_t { host_only = 0, everyone = 1, nobody = 2 };
inline constexpr bool valid_object_placement(std::uint64_t value) noexcept { return value <= 2; }
inline constexpr bool object_placement_allowed(ObjectPlacement policy, bool hosting) noexcept {
    return policy == ObjectPlacement::everyone || (policy == ObjectPlacement::host_only && hosting);
}
inline constexpr std::string_view object_placement_name(ObjectPlacement policy) noexcept {
    return policy == ObjectPlacement::everyone ? "Everyone" : policy == ObjectPlacement::host_only ? "Host only" : "Nobody";
}
// Accepts everyone|host|nobody, on/off as everyone/host, and next to cycle.
// Native widgets retain their activation callback, so `next` resolves against
// live state, never a value captured when the widget opened.
inline std::optional<ObjectPlacement> parse_object_placement(std::string_view argument,
                                                             ObjectPlacement current) noexcept {
    if (argument == "everyone" || argument == "on") return ObjectPlacement::everyone;
    if (argument == "host" || argument == "off") return ObjectPlacement::host_only;
    if (argument == "nobody") return ObjectPlacement::nobody;
    if (argument == "next")
        return current == ObjectPlacement::everyone ? ObjectPlacement::host_only
             : current == ObjectPlacement::host_only ? ObjectPlacement::nobody : ObjectPlacement::everyone;
    return {};
}
// How many objects each player may have placed in a session, set by the host or the server:
// 0 is no limit beyond the protocol's own (max_object_limit). The host and a server's admins
// are not held to it. A player over it keeps what everyone already sees of theirs; only the
// objects past the limit are left out.
inline constexpr unsigned max_object_limit = 1024;
// What a server or a host starts with, until they choose otherwise.
inline constexpr unsigned default_object_limit = 100;
inline constexpr bool valid_object_limit(std::uint64_t value) noexcept { return value <= max_object_limit; }
// Accepts a number from 1 to max_object_limit, and off|none|0 for no limit.
inline std::optional<unsigned> parse_object_limit(std::string_view argument) noexcept {
    if (argument == "off" || argument == "none") return 0U;
    unsigned value{};
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (argument.empty() || parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !valid_object_limit(value))
        return {};
    return value;
}
} // namespace dingosdk
