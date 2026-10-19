#include "Extension/Multiplayer/Net/map_fetch_policy.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace policy = dingosdk::multiplayer::map_fetch_policy;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        require(policy::second_us == 1000000, "microsecond units");
        require(policy::session_us == 1800 * policy::second_us, "30 minute session");
        require(dingosdk::multiplayer::map_fetch_limit_us == policy::session_us,
            "host protocol and local downloader share the session budget");
        require(policy::confirmation_us == 120 * policy::second_us, "2 minute confirmation");
        require(policy::installed_load_us == 30 * policy::second_us, "30 second installed-map load");
        require(!policy::expired(0, 0) && !policy::expired(0, std::numeric_limits<std::uint64_t>::max()),
            "zero means no deadline");
        require(!policy::host_expired(0, policy::session_us), "host zero means no fetch");
        require(policy::remaining(0, 100) == 0, "zero remaining saturates");

        // Arbitrary process clock epochs must not affect durations or host/client agreement.
        for (const auto origin : std::array<std::uint64_t, 3>{1, 500000000000ull, 800000000000000ull}) {
            for (const auto duration : std::array{
                     policy::session_us, policy::confirmation_us, policy::installed_load_us}) {
                const auto deadline = origin + duration;
                require(!policy::expired(deadline, deadline - 1), "before boundary");
                require(policy::expired(deadline, deadline), "exact boundary");
                require(policy::expired(deadline, deadline + 1), "after boundary");
                require(policy::remaining(deadline, deadline - 1) == 1, "last microsecond");
                require(policy::remaining(deadline, deadline) == 0, "exact remaining");
                require(policy::remaining(deadline, deadline + 1) == 0, "remaining never underflows");
            }
            const auto overall = origin + policy::session_us;
            require(!policy::host_expired(origin, origin - 1), "host rejects earlier clock");
            require(!policy::host_expired(origin, overall - 1), "host before boundary");
            require(policy::host_expired(origin, overall), "host exact boundary");
            require(policy::host_expired(origin, overall + 1), "host after boundary");
            require(policy::host_expired(origin, overall) == policy::expired(overall, overall),
                "host and downloader share duration");
        }
        constexpr auto host_clock = 90000000000000ull;
        constexpr auto client_clock = 700000000000ull;
        for (const auto elapsed : std::array{
                 policy::session_us - 1, policy::session_us, policy::session_us + 1}) {
            require(policy::host_expired(host_clock, host_clock + elapsed) ==
                    policy::expired(client_clock + policy::session_us, client_clock + elapsed),
                "independent host/client epochs yield the same expiration");
        }
        std::cout << "Map fetch policy boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
