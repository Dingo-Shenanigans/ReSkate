#include "Server/speed_check.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <algorithm>
#include <vector>

using dingosdk::server::SpeedCheck;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
// Poses at 30 Hz for `seconds` of real time: the sender's clock runs at `speed`, and each pose
// takes a base delay plus jitter (and, when `spikes`, a lag spike now and then) to arrive.
struct Run { bool flagged{}; double speed = 1.0; unsigned measured{}; };
Run run(double speed, double seconds, bool spikes, std::uint64_t start_sent = 5000000000ULL) {
    SpeedCheck check;
    std::mt19937 random(7);
    std::uniform_int_distribution<int> jitter(0, 80000);
    // Packets in send order, then delivered in arrival order; the server drops a pose older than the
    // newest it accepted, so only newer ones reach the check.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> packets; // arrived, sent
    for (double t = 0; t < seconds; t += 1.0 / 30) {
        const auto real = static_cast<std::uint64_t>(t * 1e6);
        const auto sent = start_sent + static_cast<std::uint64_t>(t * speed * 1e6);
        std::uint64_t delay = 40000 + static_cast<std::uint64_t>(jitter(random));
        if (spikes && static_cast<int>(t) % 7 == 3) delay += 900000; // a second of lag every seven
        packets.emplace_back(1000000 + real + delay, sent);
    }
    std::stable_sort(packets.begin(), packets.end());
    Run result;
    std::uint64_t newest{};
    for (const auto &[arrived, sent] : packets) {
        if (sent <= newest) continue;
        newest = sent;
        if (check.sample(sent, arrived)) {
            ++result.measured;
            result.speed = check.speed();
        }
        result.flagged |= check.flagged();
    }
    return result;
}
} // namespace

int main() {
    const auto normal = run(1.0, 120, false);
    check(!normal.flagged && normal.measured > 30 && std::abs(normal.speed - 1.0) < 0.01, "A real-time client was flagged");
    const auto laggy = run(1.0, 120, true);
    check(!laggy.flagged, "Lag spikes were taken for a speedhack");
    const auto fast = run(1.5, 40, true);
    check(fast.flagged && std::abs(fast.speed - 1.5) < 0.02, "A 1.5x speedhack was not caught");
    const auto slight = run(1.03, 120, false);
    check(!slight.flagged, "A 3% difference was flagged");
    // Under half as fast again is left alone; from there up it is caught.
    check(!run(1.1, 60, false).flagged && !run(1.3, 60, false).flagged, "A game under 1.5x speed was flagged");
    check(run(2.0, 60, false).flagged, "A 2x speedhack was not caught");
    const auto slow = run(0.5, 60, false);
    check(!slow.flagged, "A slowed-down client was flagged");
    // Loading (no poses for a while) starts the measurement over.
    SpeedCheck gaps;
    for (int i = 0; i < 30 * 15; ++i) gaps.sample(1000000 + i * 50000, 1000000 + i * 33333);
    gaps.sample(1000000 + 30 * 15 * 50000, 1000000 + 30 * 15 * 33333 + 5000000);
    check(!gaps.flagged() && gaps.speed() == 1.0, "A pause did not restart the measurement");
    std::cout << "Speed check: real time, lag, 1.5x and 2x speedhacks, 1.1x and 1.3x left alone, small drift, slow motion and pauses passed.\n";
}
