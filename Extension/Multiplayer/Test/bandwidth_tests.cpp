#include "Extension/Multiplayer/Remote/audio_capture.h"
#include "Extension/Multiplayer/Net/delta_codec.h"
#include "Extension/Multiplayer/Net/pose_delta.h"
#include "Extension/Multiplayer/Session/room.h"
#include "Extension/Multiplayer/Net/block_codec.h"
#include <bit>
#include <iostream>
#include <random>

using namespace dingosdk::multiplayer;
namespace {
void check(bool value, const char *why) { if (!value) throw std::runtime_error(why); }
Packet fixture() {
    Packet p;
    p.session = 73; p.epoch = 91; p.map = 42; p.source = 200;
    p.sequence = 1; p.time_us = 1000000;
    p.pose.skater.resize(395); p.pose.board.resize(17);
    for (std::size_t i = 0; i < p.pose.skater.size(); ++i) {
        auto &t = p.pose.skater[i];
        t.position = {static_cast<float>(i) * .004f, .11f, .07f};
        t.rotation = {.1f, .2f, .3f, .9f};
    }
    return p;
}
void pose_checks() {
    auto p = fixture();
    const auto reference = encode(p, true);
    std::mt19937 random(871);
    DeltaSender tx; DeltaReceiver rx;
    bool missing{};
    auto base = tx.prepare(p);
    check(rx.receive(base.bytes, missing).has_value(), "Initial reliable reference rejected");
    tx.sent(p, std::move(base));
    unsigned sparse{};
    for (unsigned frame = 0; frame < 100; ++frame) {
        p.pose_interval_us = dingosdk::multiplayer_pose_interval(dingosdk::multiplayer_tick_rates[(frame / 25) % 4]);
        ++p.sequence; p.time_us += 50000;
        p.pose.root.position[0] += .03f;
        for (unsigned edit = 0; edit < 8; ++edit) {
            auto &t = p.pose.skater[random() % p.pose.skater.size()];
            t.position[random() % 3] = frame % 2 ? 40.23f : .12f;
            t.rotation = frame % 2 ? std::array<float, 4>{.9f, .1f, .2f, .3f} : std::array<float, 4>{.1f, .2f, .3f, .9f};
            t.scale = frame % 3 ? std::array<float, 3>{1, 1, 1} : std::array<float, 3>{1.1f, .9f, 1.2f};
        }
        p.pose.board[0].position[1] += .025f;
        const auto raw = encode(p, true), patch = pose_delta::encode(raw, reference);
        check(pose_delta::decode(patch, reference, raw.size()) == raw, "Sparse pose changed packed bytes");
        if (frame == 0) {
            for (std::size_t n = 0; n < patch.size(); ++n) {
                bool rejected{};
                try { (void)pose_delta::decode(std::span(patch).first(n), reference, raw.size()); }
                catch (...) { rejected = true; }
                check(rejected, "Truncated pose patch accepted");
            }
            auto extra = patch; extra.push_back(0);
            bool rejected{};
            try { (void)pose_delta::decode(extra, reference, raw.size()); } catch (...) { rejected = true; }
            check(rejected, "Trailing pose patch bytes accepted");
        }
        auto update = tx.prepare(p);
        sparse += update.bytes.size() > 4 && update.bytes[2] == 'S';
        if (frame % 4 || update.establishes_baseline()) {
            const auto decoded = rx.receive(update.bytes, missing);
            check(decoded && encode(*decoded) == encode(*decode(raw)), "Loss changed skater/board transforms");
        }
        tx.sent(p, std::move(update));
    }
    check(sparse > 0, "Sparse field candidate never selected");
    // Reference changes must survive missing frames, roster changes and travel.
    p.pose.board.clear(); ++p.sequence; p.time_us += 50000;
    auto changed = tx.prepare(p);
    check(rx.receive(changed.bytes, missing)->pose.board.empty(), "Board disappearance lost");
    tx.sent(p, std::move(changed));
    ++p.world; ++p.sequence; p.time_us += 50000;
    auto world = tx.prepare(p);
    check(world.establishes_baseline(), "New world reused an old reference");
    DeltaReceiver late;
    check(late.receive(world.bytes, missing, p.world).has_value(), "New world reference rejected");
    tx.sent(p, std::move(world));
    ++p.sequence; p.time_us += 50000;
    auto next = tx.prepare(p);
    check(late.receive(next.bytes, missing, p.world).has_value(), "New world delta rejected");
    std::cout << "Sparse poses: exact packed bytes, width/scale/rotation changes, board removal, loss, travel and malformed data passed.\n";
}
void audio_checks() {
    AudioCaptureBuffer capture;
    AudioState state;
    for (unsigned i = 0; i < 12; ++i) {
        state.values[1] = static_cast<float>(i);
        if (i == 4) state.flags[2] = 1;
        if (i == 5) state.flags[2] = 0;
        if (i == 7) state.selectors[3] = 9;
        capture.push(state, 1000000 + i * 4000);
    }
    auto p = fixture(); p.kind = PacketKind::audio; p.time_us = 1050000;
    p.audio = capture.drain(p.time_us);
    check(p.audio.size() == 7, "Continuous samples not coalesced around action edges");
    check(std::count_if(p.audio.begin(), p.audio.end(), [](const auto &v) { return v.event; }) == 4,
          "Capture lost a contact or selector edge");
    p.audio.back().state.values[57] = -0.f;
    const auto raw = encode(p), wire = encode_wire(p);
    const auto decoded = decode_wire(wire);
    check(decoded && encode(*decoded) == raw, "Sparse sound changed fields, ages or event bits");
    for (std::size_t n = 0; n < raw.size(); ++n)
        check(!decode(std::span(raw).first(n)), "Truncated sparse audio accepted");
    AudioBuffer playback;
    Packet continuous = p; continuous.sequence = 3; continuous.time_us = 1100000;
    continuous.audio = {{0, state, false}};
    check(playback.push(p, 2050000), "First sound batch rejected");
    check(playback.push(continuous, 2100000), "Continuous sound rejected");
    while (playback.size()) (void)playback.sample(2200000);
    Packet late = continuous; late.sequence = 2; late.time_us = 1075000;
    late.audio[0].event = true; late.audio[0].state.flags[2] = 1;
    check(playback.push(late, 2210000), "Reliable event rejected behind continuous state");
    check(playback.sample(2210000)->flags[2] == 1, "Late contact edge not played");
    check(playback.sample(2211000) == state, "Late edge rewound continuous sound");
    check(!playback.push(late, 2211000), "Relay copy replayed an event");
    check(!playback.sample(3210001), "Disconnected sound did not stop");
    capture.push(state, 1100000);
    check(capture.drain(1100000).empty(), "Unchanged audio was not suppressed");
    capture.push(state, 1350000);
    check(!capture.drain(1350000).empty(), "Idle audio heartbeat missing");
    capture.clear();
    capture.push(state, 1400000);
    state.values[20] = 8.f; capture.push(state, 1401000);
    state.values[20] = 0.f; capture.push(state, 1402000);
    const auto impulse = capture.drain(1450000);
    check(impulse.size() == 3 && impulse[1].event && impulse[1].state.values[20] == 8.f,
          "Unknown scalar impulse was treated as continuous motion");
    check(raw.size() < packet_header_size + 2 + p.audio.size() * 383, "Sparse audio failed to shrink fixture");
    std::cout << "Audio: continuous coalescing, contact/selector edges, exact values, idle heartbeat, reordering, deduplication and disconnect passed.\n";
}
void coarse_checks() {
    // Coarser rotations still encode and decode as any pose, land within a step of the
    // original, and make a bone that barely turned the same bytes as before.
    std::mt19937 random(11);
    std::uniform_real_distribution<float> any(-1.f, 1.f);
    auto p = fixture();
    p.kind = PacketKind::pose;
    for (auto &t : p.pose.skater) {
        t.rotation = {any(random), any(random), any(random), any(random)};
        float norm{};
        for (float v : t.rotation) norm += v * v;
        for (float &v : t.rotation) v /= std::sqrt(norm);
    }
    // The awkward ones: two components equal and as large as a smaller one can be, and an exact axis.
    p.pose.skater[0].rotation = {.70710678f, .70710678f, 0, 0};
    p.pose.skater[1].rotation = {0, 0, 0, -1};
    p.pose.skater[2].rotation = {.5f, .5f, .5f, .5f};
    for (const unsigned bits : {4U, 6U, 7U, 11U}) {
        auto coarse = p;
        coarsen_rotations(coarse.pose, bits);
        const auto decoded = decode(encode(coarse, true));
        check(decoded && decoded->pose.skater.size() == p.pose.skater.size(), "A coarsened pose did not decode");
        const float step = static_cast<float>(1U << bits) / 46339.5358f;
        for (std::size_t i = 0; decoded && i < p.pose.skater.size(); ++i) {
            const auto &a = p.pose.skater[i].rotation, &b = decoded->pose.skater[i].rotation;
            const float same = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
            // Each of three components is off by at most half a step.
            check(same > 1.f - 2.f * step * step - 1e-4f, "A coarsened rotation moved by more than its step");
        }
        // A turn of a fortieth of a step is lost: the same bytes, so a difference leaves the bone out.
        auto nudged = p;
        for (auto &t : nudged.pose.skater) {
            t.rotation[0] += step * .025f;
            float norm{};
            for (float v : t.rotation) norm += v * v;
            for (float &v : t.rotation) v /= std::sqrt(norm);
        }
        coarsen_rotations(nudged.pose, bits);
        const auto first = encode(coarse, true), second = encode(nudged, true);
        std::size_t different{};
        for (std::size_t i = 0; i < std::min(first.size(), second.size()); ++i) different += first[i] != second[i];
        check(first.size() == second.size() && different < first.size() / 20, "A turn far below the step still changed the pose's bytes");
    }
    // A resized bone is held to the limit both ways, and at 1 is not resized at all.
    auto big = fixture();
    big.pose.skater[5].scale = {4, 4, 4};
    big.pose.skater[6].scale = {.1f, 1, 1};
    auto held = big.pose, plain = big.pose, free = big.pose;
    limit_bone_scale(held, 1.5f);
    limit_bone_scale(plain, 1.f);
    limit_bone_scale(free, 0.f);
    check(held.skater[5].scale == std::array<float, 3>{1.5f, 1.5f, 1.5f} && std::abs(held.skater[6].scale[0] - 1.f / 1.5f) < 1e-5f &&
              plain.skater[5].scale == std::array<float, 3>{1, 1, 1} && plain.skater[6].scale == std::array<float, 3>{1, 1, 1} &&
              free.skater[5].scale == std::array<float, 3>{4, 4, 4},
          "A bone's scale was not held to the limit");
    std::cout << "Coarse rotations: valid poses, within a step, small turns unchanged.\n";
}
void rate_checks() {
    check(pose_interval(61*61, 50000) == 100000 && pose_interval(56*56, 100000) == 100000 &&
          pose_interval(49*49, 100000) == 50000, "Near-rate hysteresis failed");
    check(pose_interval(171*171, 100000) == 200000 && pose_interval(160*160, 200000) == 200000 &&
          pose_interval(149*149, 200000) == 100000, "Far-rate hysteresis failed");
    {
        // A crowd: no limit while everyone fits at the full rate, then the nearest at full,
        // the next at half, the rest at low, within the budget.
        std::vector<float> few(20), crowd(49), packed(127);
        for (std::size_t i = 0; i < packed.size(); ++i) {
            const auto d = static_cast<float>((packed.size() - i) * (packed.size() - i)); // farthest first
            packed[i] = d;
            if (i < crowd.size()) crowd[i] = d;
            if (i < few.size()) few[i] = d;
        }
        const auto none = crowd_limits(few, 30);
        // The default leaves twenty players in one place at the full rate, each to each.
        std::vector<float> twenty(crowd.begin(), crowd.begin() + 19);
        check(crowd_limits(twenty, 30).half == none.half && dingosdk::valid_crowd_budget(0) &&
              dingosdk::valid_crowd_budget(dingosdk::crowd_pose_budget) && !dingosdk::valid_crowd_budget(50),
              "The default crowd budget slowed twenty players, or a bad one was valid");
        check(crowd_interval(33333, 1e9f, none) == 33333, "A small group was limited");
        const auto count = [](std::span<const float> all, const CrowdLimits &limits, unsigned tps) {
            unsigned sent{};
            for (const auto d : all) sent += 1000000U / crowd_interval(dingosdk::multiplayer_pose_interval(tps), d, limits);
            return sent;
        };
        for (const unsigned tps : {20U, 30U, 60U, 120U}) {
            const auto limits = crowd_limits(crowd, tps, 600);
            const auto sent = count(crowd, limits, tps);
            // Within the budget, or at the least every crowd is sent: the nearest few at the
            // full and half rates, which at a high TPS is more than the budget by itself.
            const unsigned least = crowd_always_full * (1000000U / dingosdk::multiplayer_pose_interval(tps)) + crowd_always_half * 10 +
                                   (static_cast<unsigned>(crowd.size()) - crowd_always_full - crowd_always_half) * 5;
            check(sent <= std::max(602U, least + 2) && sent > 450, "A crowd was not sent within the budget");
            check(crowd_interval(dingosdk::multiplayer_pose_interval(tps), crowd.front(), limits) == dingosdk::multiplayer_pose_interval(tps) &&
                  crowd_interval(dingosdk::multiplayer_pose_interval(tps), crowd.back(), limits) == 200000,
                  "A crowd's nearest and farthest were not sent at the full and low rates");
            check(limits.half < limits.low, "A crowd had no half-rate ring");
        }
        // Too many for the budget even at the low rate: the nearest few are still sent at the
        // full and half rates, and only the rest at the low rate.
        const auto floor = crowd_limits(packed, 30, 600);
        check(count(packed, floor, 30) == crowd_always_full * 30 + crowd_always_half * 10 + (packed.size() - crowd_always_full - crowd_always_half) * 5,
              "An over-full crowd did not keep its nearest at the full and half rates");
        // Distance still slows what the crowd limit would send faster.
        check(crowd_interval(200000, 0, none) == 200000 && crowd_interval(100000, 0, crowd_limits(crowd, 30, 600)) == 100000,
              "A crowd limit sped a far player up");
    }
    for (auto interval : {8333U, 16666U, 33333U, 50000U, 100000U, 200000U}) {
        PoseBuffer buffer;
        auto p = fixture(); p.pose_interval_us = interval;
        for (unsigned i = 0; i < 20; ++i) {
            p.sequence = i + 1; p.time_us = 1000000 + i * interval;
            p.pose.root.position[0] = static_cast<float>(i * interval) / 1000000.f;
            p.pose.board[0] = p.pose.root;
            check(buffer.push(p, p.time_us + 7000000), "Rate fixture rejected");
        }
        const auto sampled = buffer.sample(p.time_us + 7000000 + interval / 2);
        const auto expected = static_cast<float>(19 * interval + interval/2 - std::max(100000U, interval + 50000)) / 1000000.f;
        check(sampled && std::abs(sampled->root.position[0] - expected) < .0001f &&
              sampled->root == sampled->board[0], "Rate changed animation speed or board alignment");
    }
    std::cout << "Distance rates: hysteresis and 20/10/5 TPS sender-time interpolation with aligned boards passed.\n";
}
void block_checks() {
    std::vector<std::uint8_t> raw(6000), compressed(6500), restored(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint8_t>((i / 97) ^ (i % 13));
    const auto encoded = compress_block(raw);
    check(encoded.codec == BlockCodec::lz4, "Client hot-path encoder did not select LZ4");
    check(decompress_block(encoded.bytes, restored, encoded.codec) && raw == restored,
          "Client hot-path LZ4 block changed bytes");
    for (const auto codec : {BlockCodec::lz4, BlockCodec::zstd}) {
        const auto size = codec == BlockCodec::zstd ? ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 1)
            : static_cast<std::size_t>(LZ4_compress_default(reinterpret_cast<const char *>(raw.data()),
                reinterpret_cast<char *>(compressed.data()), static_cast<int>(raw.size()), static_cast<int>(compressed.size())));
        check(size && size < compressed.size(), "Compression fixture failed");
        const auto bytes = std::span(compressed).first(size);
        check(decompress_block(bytes, restored, codec) && raw == restored, "Block codec changed bytes");
        for (std::size_t n = 0; n < size; ++n)
            check(!decompress_block(bytes.first(n), restored, codec), "Truncated compressed block accepted");
        check(!decompress_block(bytes, std::span(restored).first(raw.size() - 1), codec), "Block output bound ignored");
        auto trailing = std::vector<std::uint8_t>(bytes.begin(), bytes.end()); trailing.push_back(0);
        check(!decompress_block(trailing, restored, codec), "Trailing compressed bytes accepted");
    }
    std::cout << "LZ4/Zstd blocks: exact output, truncation, bounded decompression and trailing data checks passed.\n";
}
}
int main() {
    try { pose_checks(); audio_checks(); coarse_checks(); rate_checks(); block_checks(); }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
