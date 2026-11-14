// Exercise the same pending command used by placement_physics.cpp, with the two native copies
// represented by independent values. Setter acceptance deliberately does not move either copy.
// This verifies scheduling and readback acknowledgement, not the game's native setter or network.
#include "Extension/Objects/placement_pose.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace policy = dingosdk::placement_pose;
using policy::Pending;
using policy::Pose;
using policy::Side;
using Millis = std::uint64_t;
unsigned checks{}, scenarios{};

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

Pose pose(float x, float y = 2.0f, float z = 3.0f) {
    return {1, 1, 1, 0, .1f, .2f, .3f, std::sqrt(.86f), x, y, z, 0};
}

// The harness holds observations separately from the real command policy. It does not implement
// a retry/acknowledgement rule: those decisions are all made by the production Pending helper.
struct Copies {
    struct Write { Side side; Pose target; Millis at; };
    Pose client = pose(12), server = pose(12), sampled = pose(12);
    std::optional<Pending> command;
    std::vector<Write> writes;

    void request(Pose wanted, Millis now) { command.emplace(wanted, now); }
    void cancel() { command.reset(); } // the owning lease drops this optional on edit/release

    void attempt(Side side, Millis now, bool setter_available = true) {
        if (!command || !command->retry_due(side, now)) return;
        if (setter_available) writes.push_back({side, command->target(), now});
        command->attempted(side, now, setter_available);
    }

    bool client_sample(Millis now) {
        sampled = client; // sample_pose/motion overwrites the observation, never the command
        return command && command->client_confirmed(sampled, now);
    }

    void server_sample(Millis now) {
        if (command) command->observe_server(server, now);
    }
};

void target_survives_client_server_ordering() {
    ++scenarios;
    const auto wanted = pose(1);
    // Several body/motion reads can precede the next server tick. The old implementation held
    // the request in the same field as these reads, so server_tick received the displaced pose.
    Copies copies;
    copies.request(wanted, 100);
    copies.attempt(Side::client, 100);
    for (Millis now : {101ULL, 106ULL, 116ULL, 133ULL}) {
        copies.client[8] += .25f;
        check(!copies.client_sample(now), "a displaced client sample acknowledged a requested target");
    }
    check(!policy::matches(copies.sampled, wanted), "fixture never overwrote the old mutable mirror field");
    copies.attempt(Side::server, 140);
    check(copies.writes.size() == 2 && copies.writes.back().side == Side::server &&
              policy::matches(copies.writes.back().target, wanted),
          "accepted target was lost to client sampling before server dispatch");
    check(policy::matches(copies.command->target(), wanted), "dispatch mutated the immutable requested target");
    check(!copies.client_sample(141), "successful setter invocation was mistaken for a later body acknowledgement");

    // Independently make the effects visible. Publication alone, same-timestamp observation,
    // and even a matching client before the server readback are insufficient acknowledgements.
    copies.client = wanted;
    check(!copies.client_sample(142), "matching client cleared pending before actual server observation");
    copies.server = wanted;
    copies.server_sample(140);
    check(!copies.client_sample(143), "same-timestamp server readback acknowledged its own write");
    copies.server_sample(150);
    check(!copies.client_sample(150), "client readback was not later than the server observation");
    check(copies.client_sample(151), "later real client and server observations did not acknowledge the target");
}

void early_client_match_does_not_skip_server() {
    ++scenarios;
    Copies copies;
    const auto wanted = pose(4);
    copies.request(wanted, 1000);
    copies.attempt(Side::client, 1000);
    copies.client = wanted;
    check(!copies.client_sample(1001), "client-only success removed the target before server publication");
    copies.attempt(Side::server, 1040);
    check(copies.writes.size() == 2 && policy::matches(copies.writes.back().target, wanted),
          "an early matching client caused the server to mirror a different target");
    copies.server_sample(1050); // old readable server pose, despite acceptance
    check(!copies.client_sample(1051), "accepted-but-not-applied server write was acknowledged");
    copies.server = wanted;
    copies.server_sample(1060);
    check(copies.client_sample(1061), "independent server readback did not unblock later client acknowledgement");
}

void full_pose_and_quaternion_equivalence() {
    ++scenarios;
    auto wanted = pose(7);
    auto opposite = wanted;
    for (unsigned i = 4; i < 8; ++i) opposite[i] = -opposite[i];
    check(policy::matches(wanted, opposite), "q and -q represent the same full orientation");
    auto accepted_nonunit = wanted;
    for (unsigned i = 4; i < 8; ++i) accepted_nonunit[i] *= 1.02f;
    check(policy::matches(accepted_nonunit, opposite), "normalized readback rejected a valid near-unit API quaternion");

    Copies copies;
    copies.request(accepted_nonunit, 2000);
    copies.attempt(Side::client, 2000);
    copies.attempt(Side::server, 2020);
    copies.server = opposite;
    copies.server_sample(2021);
    copies.client = wanted;
    check(copies.client_sample(2022), "sign/normalization-equivalent full native readbacks never completed");

    auto wrong_rotation = wanted;
    wrong_rotation[4] = 0; wrong_rotation[5] = 0; wrong_rotation[6] = 0; wrong_rotation[7] = 1;
    check(!policy::matches(wanted, wrong_rotation), "matching position hid a different rotation");
    auto wrong_position = wanted;
    wrong_position[9] += .1f;
    check(!policy::matches(wanted, wrong_position), "matching rotation hid a displaced position");
    auto wrong_scale = wanted;
    wrong_scale[1] += .1f;
    check(!policy::matches(wanted, wrong_scale), "a changed native scale was ignored");
    // The host's accepted native pose domain constrains only the first scale axis to be positive.
    // Already accepted zero/negative secondary axes still need honest equality and finiteness checks.
    auto accepted_scale = wanted;
    accepted_scale[1] = 0; accepted_scale[2] = -1;
    check(policy::matches(accepted_scale, accepted_scale), "accepted secondary-axis scales could never acknowledge");
    auto scale_readback = accepted_scale;
    scale_readback[2] = -.5f;
    check(!policy::matches(accepted_scale, scale_readback), "different accepted secondary-axis scales matched");
    scale_readback = accepted_scale;
    scale_readback[1] = std::numeric_limits<float>::infinity();
    check(!policy::matches(accepted_scale, scale_readback), "nonfinite secondary-axis scale matched");
    accepted_scale[0] = .001f;
    check(!policy::matches(accepted_scale, accepted_scale), "invalid first-axis scale was acknowledged");
    auto invalid = wanted;
    invalid[8] = std::numeric_limits<float>::infinity();
    check(!policy::matches(wanted, invalid), "nonfinite native coordinates counted as an acknowledgement");
    invalid = wanted;
    for (unsigned i = 4; i < 8; ++i) invalid[i] = 0;
    check(!policy::matches(wanted, invalid), "a zero quaternion counted as an acknowledgement");

    // Native padding is not a coordinate. It can hold nonfinite bit patterns without invalidating
    // the scale/quaternion/position fields that placement_geometry::finite actually validates.
    auto padding = wanted;
    padding[3] = padding[11] = std::numeric_limits<float>::quiet_NaN();
    check(policy::matches(wanted, padding), "unused native padding was compared as a pose component");
}

void readbacks_must_be_fresh_and_follow_a_real_attempt() {
    ++scenarios;
    const auto wanted = pose(9);
    Pending pending(wanted, 3000);
    pending.attempted(Side::client, 3000, true);
    pending.attempted(Side::server, 3020, false);
    pending.observe_server(wanted, 3021);
    check(!pending.has_written(Side::server) && !pending.client_confirmed(wanted, 3022),
          "an unavailable server setter acquired acknowledgement authority");
    pending.attempted(Side::server, 3040, true);
    pending.observe_server(wanted, 3041);
    check(pending.client_confirmed(wanted, 3042), "fresh ordered observations did not acknowledge an issued command");
    check(!pending.client_confirmed(wanted, 3041 + policy::ack_fresh_ms + 1),
          "an old matching server sample remained an unlimited delivery acknowledgement");
    pending.observe_server(pose(25), 3070);
    check(!pending.client_confirmed(wanted, 3071), "a later mismatching server readback retained stale success");
    pending.observe_server(wanted, 3080);
    check(pending.client_confirmed(wanted, 3081), "later fresh matching server evidence could not recover");
    pending.attempted(Side::server, 3100, true);
    check(!pending.client_confirmed(wanted, 3101), "a newer server dispatch reused acknowledgement from an older dispatch");
    pending.observe_server(wanted, 3102);
    check(pending.client_confirmed(wanted, 3103), "new server dispatch did not obtain its own later readback");
    pending.server_unavailable();
    check(!pending.client_confirmed(wanted, 3104), "unavailable server readback retained an earlier delivery acknowledgement");
    pending.observe_server(wanted, 3105);
    check(!pending.client_confirmed(wanted, 3105), "recovery from missing server data reused the previous ordering barrier");
    check(pending.client_confirmed(wanted, 3106), "fresh ordered readbacks did not recover from missing server data");
}

void matching_ticks_in_one_clock_millisecond_do_not_starve() {
    ++scenarios;
    const auto wanted = pose(6);
    Pending pending(wanted, 3200);
    pending.attempted(Side::client, 3200, true);
    pending.attempted(Side::server, 3200, true);
    pending.observe_server(wanted, 3216);
    check(!pending.client_confirmed(wanted, 3216), "first same-millisecond observations acknowledged their own barrier");
    pending.observe_server(wanted, 3232);
    check(pending.client_confirmed(wanted, 3232),
          "repeated synchronized server/client ticks moved the acknowledgement barrier forever");
    pending.observe_server(pose(11), 3248);
    check(!pending.client_confirmed(wanted, 3248), "mismatching server evidence retained an earlier matching barrier");
    pending.observe_server(wanted, 3264);
    check(!pending.client_confirmed(wanted, 3264), "server recovery did not require a new later client observation");
    pending.observe_server(wanted, 3280);
    check(pending.client_confirmed(wanted, 3280), "recovered synchronized readbacks could never complete");
}

void bounded_retry_and_expiry() {
    ++scenarios;
    Copies copies;
    const auto wanted = pose(3);
    constexpr Millis accepted = 4000;
    copies.request(wanted, accepted);
    copies.attempt(Side::client, accepted, false);
    check(!copies.command->retry_due(Side::client, accepted + policy::retry_ms - 1),
          "a refused native attempt retried without the rate interval");
    check(copies.command->retry_due(Side::client, accepted + policy::retry_ms),
          "a refused native attempt prevented a later bounded retry");
    copies.attempt(Side::client, accepted + policy::retry_ms);
    check(copies.command->has_written(Side::client) && !copies.command->has_written(Side::server),
          "client/server native attempts were not tracked independently");

    // No native effect ever appears. Frequent sampling/attempts cannot slide the absolute deadline.
    bool falsely_confirmed = false;
    for (Millis now = accepted + policy::retry_ms + 1; now <= accepted + policy::lifetime_ms + 200; ++now) {
        copies.attempt(Side::client, now);
        copies.attempt(Side::server, now);
        copies.server_sample(now);
        falsely_confirmed = copies.client_sample(now) || falsely_confirmed;
    }
    check(!falsely_confirmed, "repeated accepted setters without effects falsely acknowledged a pose");
    bool ordered_and_bounded = true;
    std::array<std::optional<Millis>, 2> previous;
    for (const auto& write : copies.writes) {
        const auto side = write.side == Side::client ? 0U : 1U;
        if (write.at >= accepted + policy::lifetime_ms ||
            (previous[side] && write.at - *previous[side] < policy::retry_ms) ||
            !policy::matches(write.target, wanted)) ordered_and_bounded = false;
        previous[side] = write.at;
    }
    check(ordered_and_bounded && copies.writes.size() > 2, "retry writes escaped the interval, target or absolute deadline");
    check(copies.command->expired(accepted + policy::lifetime_ms) &&
              !copies.command->retry_due(Side::client, accepted + policy::lifetime_ms) &&
              !copies.command->retry_due(Side::server, accepted + policy::lifetime_ms),
          "expired commands could still dispatch native writes");
    copies.server = copies.client = wanted;
    copies.server_sample(accepted + policy::lifetime_ms + 1);
    check(!copies.client_sample(accepted + policy::lifetime_ms + 2),
          "a late coincidental target match revived an expired request");
}

void replacements_and_cancellation_do_not_replay_old_targets() {
    ++scenarios;
    const auto old = pose(1), next = pose(8);
    Copies copies;
    copies.request(old, 6000);
    copies.attempt(Side::client, 6000);
    copies.attempt(Side::server, 6016);
    copies.server = old;
    copies.server_sample(6017);
    copies.request(next, 6020); // a later accepted pose replaces every receipt of the earlier one
    check(!copies.command->has_written(Side::client) && !copies.command->has_written(Side::server),
          "a new request inherited the old request's dispatch receipts");
    copies.client = copies.server = old;
    copies.server_sample(6021);
    check(!copies.client_sample(6022), "delayed old-request readback acknowledged its replacement");
    copies.attempt(Side::server, 6023);
    check(policy::matches(copies.writes.back().target, next), "server dispatched an obsolete target after replacement");
    copies.attempt(Side::client, 6024);
    copies.server = next;
    copies.server_sample(6025);
    copies.client = next;
    check(copies.client_sample(6026), "replacement request could not obtain its own ordered readbacks");

    // Editing/release/lifetime teardown remove this optional in the native integration. A late
    // observation from the cancelled lease cannot become authority for a subsequently accepted
    // command. Native cancellation call sites still need Windows/in-game integration checks.
    Copies interrupted;
    interrupted.request(old, 7000);
    interrupted.attempt(Side::client, 7000);
    interrupted.attempt(Side::server, 7016);
    interrupted.server = old;
    interrupted.server_sample(7017);
    interrupted.cancel();
    const auto before = interrupted.writes.size();
    interrupted.client = old;
    interrupted.server_sample(7020);
    interrupted.attempt(Side::server, 7030);
    check(!interrupted.client_sample(7031) && interrupted.writes.size() == before && !interrupted.command,
          "a cancelled lease replayed or acknowledged its pending target");
    interrupted.request(next, 7040);
    interrupted.server_sample(7041);
    interrupted.attempt(Side::client, 7042);
    check(!interrupted.client_sample(7043), "a new lease reused server acknowledgement from before cancellation");

    Copies first, second;
    first.request(old, 8000);
    second.request(next, 8000);
    first.cancel();
    second.attempt(Side::server, 8020);
    check(first.writes.empty() && second.writes.size() == 1 && policy::matches(second.writes.front().target, next),
          "cancelling one placement lost or retargeted another placement's independent command");
}
}

int main() {
    try {
        target_survives_client_server_ordering();
        early_client_match_does_not_skip_server();
        full_pose_and_quaternion_equivalence();
        readbacks_must_be_fresh_and_follow_a_real_attempt();
        matching_ticks_in_one_clock_millisecond_do_not_starve();
        bounded_retry_and_expiry();
        replacements_and_cancellation_do_not_replay_old_targets();
        std::cout << "placement pose: " << checks << " checks in " << scenarios
                  << " scheduling/readback scenarios passed (no native game execution)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "placement pose: " << error.what() << '\n';
        return 1;
    }
}
