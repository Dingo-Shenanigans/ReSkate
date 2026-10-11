#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace dingosdk {

// EA's online services, as Skate.exe names them. ea.com holds the Blaze redirectors, accounts and
// gateway, telemetry (pin-*, freeform-river), error reports (collector.errors), remote
// configuration (gcs), experimentation, leaderboards, social and ad tech; tnt-ea.com holds push
// notifications and real-time messaging. Not listed on purpose: dingo-dev-assets.akamaized.net,
// the CDN the fast-travel artwork comes from (Engine/Game/World/location_travel.h). It only serves
// images, and the travel menu would lose its icons without it.
inline constexpr std::array<std::string_view, 2> ea_service_domains{"ea.com", "tnt-ea.com"};

// Whether a host name is one of ea_service_domains or a name under one, ignoring ASCII case and a
// trailing root dot. Address literals never match: Skate.exe reaches EA by name only.
template<class Char>
constexpr bool ea_service_host(std::basic_string_view<Char> host) noexcept {
    using Unit = std::make_unsigned_t<Char>;
    if (!host.empty() && host.back() == Char('.')) host.remove_suffix(1);
    for (const auto domain : ea_service_domains) {
        if (host.size() < domain.size()) continue;
        const auto start = host.size() - domain.size();
        if (start != 0 && host[start - 1] != Char('.')) continue;
        bool same = true;
        for (std::size_t i = 0; same && i < domain.size(); ++i) {
            auto unit = static_cast<Unit>(host[start + i]);
            if (unit >= Unit('A') && unit <= Unit('Z')) unit = static_cast<Unit>(unit + Unit('a' - 'A'));
            same = unit == static_cast<Unit>(domain[i]);
        }
        if (same) return true;
    }
    return false;
}

// Makes every name lookup inside Skate.exe for an EA service fail as if the name did not exist, so
// the game cannot reach EA's servers: no error reports, telemetry or remote configuration. Always on.
bool start_ea_service_block(std::string& error) noexcept;

} // namespace dingosdk
