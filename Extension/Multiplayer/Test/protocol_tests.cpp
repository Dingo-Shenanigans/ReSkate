#include "Extension/Multiplayer/Steam/steam_server_browser.h"
#include "Server/steam_server.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Multiplayer/Net/wire_codec.h"
#include "Extension/Throwdowns/throwdown_wire.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace dingosdk::multiplayer;
namespace {
void check(bool value, const char *reason) {
    if (!value)
        throw std::runtime_error(reason);
}
Packet frame(std::uint32_t seq, float x) {
    Packet p;
    p.session = 73;
    p.epoch = 91;
    p.map = 42;
    p.sequence = seq;
    p.pose.root.position[0] = x;
    p.pose.skater.resize(2);
    p.pose.board.resize(1);
    return p;
}
void tick_rates_codec() {
    for (const unsigned rate : {20U, 30U, 60U, 120U, 10U, 5U}) {
        auto p = frame(1, 5);
        p.pose_interval_us = 1000000U / rate;
        for (const bool packed : {false, true}) {
            auto bytes = encode(p, packed);
            const auto decoded = decode(bytes);
            check(decoded && decoded->pose_interval_us == p.pose_interval_us, "Pose TPS lost wire precision");
            bytes[packet_header_size + 4] = 0;
            bytes[packet_header_size + 5] = 0;
            check(!decode(bytes), "Zero pose TPS accepted");
        }
        if (dingosdk::valid_multiplayer_tps(rate)) {
            p.kind = PacketKind::roster; p.tps = rate;
            p.members = {{76561198000000001ULL, p.epoch, "Host"}};
            const auto decoded = decode(encode(p));
            check(decoded && decoded->tps == rate, "Roster lost host TPS");
        }
    }
}
void compressed_codec() {
    auto p = frame(1, 125);
    p.time_us = 1000000;
    p.pose.skater.resize(395);
    p.pose.board.resize(17);
    for (std::size_t i = 0; i < p.pose.skater.size(); ++i) {
        auto &t = p.pose.skater[i];
        t.position = {static_cast<float>(i % 7) * .01f, static_cast<float>(i % 11) * .02f, .1f};
        t.rotation = {0, .70710678f, 0, .70710678f};
    }
    const auto raw = encode(p), wire = encode_wire(p);
    check(wire.size() < raw.size() / 2, "Repeating rig data failed to compress");
    const auto decoded = decode_wire(wire);
    check(decoded && decoded->pose.skater.size() == p.pose.skater.size(), "Packed rig lost bones");
    for (std::size_t i = 0; i < p.pose.skater.size(); ++i) {
        const auto &a = p.pose.skater[i], &b = decoded->pose.skater[i];
        for (unsigned j = 0; j < 3; ++j)
            check(std::abs(a.position[j] - b.position[j]) <= .00051f, "Packed bone position error exceeds half a millimetre");
        float dot{};
        for (unsigned j = 0; j < 4; ++j) dot += a.rotation[j] * b.rotation[j];
        check(std::abs(dot) > .99999f, "Packed rotation error exceeds bound");
        check(a.scale == b.scale, "Packed scale changed");
    }
    check(wire_original_size(wire) == encode(p, true).size(), "Compression telemetry size differs");
    for (std::size_t n = 0; n < wire.size(); ++n)
        check(!decode_wire(std::span(wire).first(n)), "Truncated compressed block accepted");
    auto bad = wire;
    bad.push_back(0);
    check(!decode_wire(bad), "Compressed trailing data accepted");
    bad = wire;
    for (int i = 4; i < 8; ++i)
        bad[i] = 255;
    check(!decode_wire(bad), "Decompression allocation limit ignored");
    std::mt19937 rng(827);
    for (unsigned i = 0; i < 8000; ++i) {
        bad = wire;
        bad[rng() % bad.size()] = static_cast<std::uint8_t>(rng());
        if (auto packet = decode_wire(bad))
            check(decode(encode(*packet)).has_value(), "Compressed mutation bypassed protocol validation");
    }
    p.kind = PacketKind::audio;
    p.audio.resize(32);
    for (std::size_t i = 0; i < p.audio.size(); ++i) {
        p.audio[i].age_us = static_cast<std::uint32_t>((31 - i) * 1000);
        p.audio[i].state.values[3] = static_cast<float>(i) * .3f;
        p.audio[i].state.flags[2] = i % 2;
    }
    check(encode(*decode_wire(encode_wire(p))) == encode(p), "Audio compression lost event edges");
    auto maximum = frame(1, 0);
    maximum.pose.skater.resize(max_skater_bones);
    maximum.pose.board.resize(max_board_bones);
    check(decode_wire(encode_wire(maximum)).has_value(), "Maximum rig cannot pass wire codec");
    auto hello = frame(1, 0);
    hello.kind = PacketKind::hello;
    check(encode_wire(hello) == encode(hello), "Small greeting should stay uncompressed");
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i)
        check(decode_wire(wire).has_value(), "Repeated block decode failed");
    std::cout << "Synthetic 395-bone wire fixture: " << raw.size() << " -> " << wire.size()
              << " bytes; 2000 decodes in "
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count()
              << " ms.\n";
}
void sender_timeline() {
    PoseBuffer buffer;
    auto a = frame(1, 0);
    a.time_us = 1000000;
    auto b = frame(2, 10);
    b.time_us = 1100000;
    auto c = frame(3, 20);
    c.time_us = 1200000;
    check(buffer.push(a, 1000000) && buffer.push(b, 1200000) && buffer.push(c, 1233333),
          "Timestamped jitter fixture rejected");
    const auto pose = buffer.sample(1250000);
    check(pose && std::abs(pose->root.position[0] - 15.f) < .1f,
          "Arrival jitter stretched animation instead of following sender time");
    buffer.clear();
    b.pose.root.position[0] = 1;
    b.time_us = 1033333;
    c.pose.root.position[0] = 2;
    c.time_us = 1066666;
    check(buffer.push(a, 1000000) && buffer.push(b, 1500000) && buffer.push(c, 1500000),
          "Bursty delivery fixture rejected");
    check(buffer.sample(1500000)->root.position[0] == 2, "Old burst replayed as slow motion");
    auto backwards = frame(4, 3);
    backwards.time_us = 1000000;
    check(!buffer.push(backwards, 1600000), "Sender clock rewind was accepted");
    backwards.time_us = UINT64_MAX;
    check(!buffer.push(backwards, 1600000), "Oversized sender timestamp was accepted");
    buffer.clear();
    a.time_us = 5000000000000ULL;
    b.time_us = a.time_us + 100000;
    b.pose.root.position[0] = 10;
    check(buffer.push(a, 1000000) && buffer.push(b, 1100000) &&
              std::abs(buffer.sample(1150000)->root.position[0] - 5) < .01f,
          "Independent PC clock offsets changed playback speed");
}
void codec() {
    auto p = frame(1, 12);
    p.pose.skater[1].rotation = {0, .70710678f, 0, .70710678f};
    const auto bytes = encode(p);
    const auto decoded = decode(bytes);
    check(decoded && decoded->pose.root == p.pose.root && decoded->pose.skater == p.pose.skater &&
              decoded->pose.board == p.pose.board,
          "Pose packet loses fields");
    for (std::size_t n = 0; n < bytes.size(); ++n)
        check(!decode(std::span(bytes).first(n)), "Truncated packet accepted");
    auto extra = bytes;
    extra.push_back(0);
    check(!decode(extra), "Trailing bytes accepted");
    auto corrupt = bytes;
    corrupt[4] = 99;
    check(!decode(corrupt), "Unknown protocol accepted");
    corrupt = bytes;
    corrupt[4] = 1;
    check(!decode(corrupt), "Pre-skateboard protocol accepted");
    corrupt = bytes;
    corrupt[6] = 99;
    check(!decode(corrupt), "Unknown packet type accepted");
    corrupt = bytes;
    corrupt[packet_header_size + 0] = 255;
    corrupt[packet_header_size + 1] = 255;
    check(!decode(corrupt), "Oversize bone count accepted");
    corrupt = bytes;
    corrupt[packet_header_size + 6] = 0;
    corrupt[packet_header_size + 7] = 0;
    corrupt[packet_header_size + 8] = 0xc0;
    corrupt[packet_header_size + 9] = 0x7f;
    check(!decode(corrupt), "NaN coordinate accepted");
    corrupt = bytes;
    for (std::size_t i = packet_header_size + 18; i < packet_header_size + 34; ++i)
        corrupt[i] = 0;
    check(!decode(corrupt), "Zero quaternion accepted");
    p = frame(2, 0);
    p.pose.skater.resize(max_skater_bones);
    p.pose.board.resize(max_board_bones);
    check(encode(p).size() <= max_packet && decode(encode(p)).has_value(),
          "Maximum supported pose exceeds packet bound");
    Packet hello;
    hello.kind = PacketKind::hello;
    hello.session = 1;
    hello.epoch = 2;
    hello.build.fill(0xa5);
    check(decode(encode(hello))->build == hello.build, "Build identity does not round trip");
    check(!newer_sequence(1, 1) && !newer_sequence(0, 1) && newer_sequence(0, UINT32_MAX),
          "Sequence wrap ordering failed");
    check(map_hash("Levels\\Park") == map_hash("levels/park"), "Map normalization failed");
}
void playback() {
    PoseBuffer buffer;
    check(buffer.push(frame(1, 0), 1000000), "First frame refused");
    check(buffer.push(frame(2, 10), 1200000), "Second frame refused");
    auto sample = buffer.sample(1200000);
    check(sample && std::abs(sample->root.position[0] - 5) < .001f, "Interpolation is not delayed by 100 ms");
    check(!buffer.push(frame(1, 100), 1300000), "Reordered frame accepted");
    check(!buffer.sample(2300000), "Stale avatar never expires");
    check(buffer.push(frame(3, 100), 1400000), "Teleport refused");
    check(buffer.sample(1400000)->root.position[0] == 100, "Teleport interpolates across the map");
    auto p = frame(4, 200);
    p.epoch = 92;
    check(buffer.push(p, 1500000) && buffer.size() == 1, "Epoch does not reset playback");
    Transform a, b;
    b.rotation = {0, 0, 0, -1};
    check(valid_transform(interpolate(a, b, .5f)), "Opposite quaternion signs collapse");
    for (unsigned i = 0; i < 80; ++i)
        buffer.push(frame(i + 10, 1), 1600000 + i * 1000);
    check(buffer.size() <= 64, "Playback buffer grows without bound");
    buffer.clear();
    check(!buffer.sample(2000000), "Clear leaves an avatar");
}
void skateboard_playback() {
    auto first = frame(1, 10);
    first.pose.board.resize(17);
    first.pose.board[0].position = {11, 2, 30};
    first.pose.board[1].position = {0, .2f, 0};
    first.pose.skater.resize(4);
    first.pose.skater[1].position = {10, 3, 30};
    first.pose.board[2].position = {11, 1.9f, 30};
    auto second = first;
    second.sequence = 2;
    second.pose.root.position[0] += 4;
    second.pose.board[0].position[0] += 4;
    second.pose.skater[1].position[0] += 4;
    second.pose.board[2].position[0] += 4;
    second.pose.board[4].rotation = {0, 0, 1, 0};
    PoseBuffer buffer;
    check(buffer.push(*decode(encode(first)), 1000000) && buffer.push(*decode(encode(second)), 1200000),
          "Skateboard packets rejected");
    auto pose = buffer.sample(1200000);
    check(pose && pose->board.size() == 17 && std::abs(pose->board[0].position[0] - 13) < .001f &&
              std::abs(pose->board[4].rotation[2] - .70710678f) < .001f,
          "Board root and flip pose do not interpolate together");
    const auto bones = pose->board;
    const auto skater_bones = pose->skater;
    offset_pose(*pose, {5, 0, 0});
    check(pose->root.position[0] == 17 && pose->board[0].position[0] == 18 &&
              pose->skater[1].position[0] == 17 && pose->board[2].position[0] == 18 &&
              pose->board[1] == bones[1] && pose->board[4] == bones[4] &&
              pose->skater[0] == skater_bones[0] && pose->skater[3] == skater_bones[3],
          "Local echo moved child bones or left a rig's world anchor behind");
    offset_pose(*pose, {-5, 2, -3});
    check(pose->skater[1].position == std::array<float, 3>{12, 5, 27} &&
              pose->board[2].position == std::array<float, 3>{13, 3.9f, 27} &&
              pose->board[2].rotation == bones[2].rotation,
          "World anchor displacement changed relative pose or rotation");
    Pose root_only;
    offset_pose(root_only, {5, 0, 0});
    check(root_only.root.position[0] == 5 && root_only.skater.empty() && root_only.board.empty(),
          "Root-only echo required unavailable rig anchors");
    second.sequence = 3;
    second.pose.board.clear();
    check(buffer.push(second, 1300000) && buffer.sample(1300000)->board.empty(),
          "Unavailable skateboard retained stale pose data");
}
void greetings() {
    auto hello = frame(1, 0);
    hello.kind = PacketKind::hello;
    hello.build.fill(0xa5);
    check(greeting_error(hello, 73, 42, hello.build).empty(), "Compatible hello rejected");
    const auto received = decode(encode(hello));
    check(received.has_value(), "Hello decoding failed");
    auto welcome = *received;
    welcome.kind = PacketKind::welcome;
    welcome.epoch = 456;
    const auto peer = decode(encode(welcome));
    check(peer && greeting_error(*peer, 73, 42, hello.build).empty(), "Compatible welcome rejected");
    check(!greeting_error(hello, 74, 42, hello.build).empty(), "Wrong join code accepted");
    check(!greeting_error(hello, 73, 43, hello.build).empty(), "Wrong map accepted");
    check(!greeting_error(hello, 73, 0, hello.build).empty(), "Unloaded map accepted");
    auto different = hello.build;
    different[0] ^= 1;
    check(!greeting_error(hello, 73, 42, different).empty(), "Different build accepted");
    check(greeting_error(hello, 73, 42, hello.build, hello.epoch).empty(), "Repeated hello rejected");
    check(!greeting_error(hello, 73, 42, hello.build, 92).empty(), "Changed peer epoch accepted");
    hello.kind = PacketKind::pose;
    check(!greeting_error(hello, 73, 42, hello.build).empty(), "Pose accepted as handshake");
}
void matrices_and_invites() {
    for (unsigned i = 0; i < 360; i += 7) {
        Transform t;
        const float angle = static_cast<float>(i) * .0174532925f;
        t.rotation = {0, std::sin(angle * .5f), 0, std::cos(angle * .5f)};
        t.position = {12, 30, -50};
        const auto matrix = to_matrix(t);
        const auto roundtrip = to_matrix(from_matrix(matrix));
        for (unsigned j = 0; j < 16; ++j)
            check(std::abs(matrix[j] - roundtrip[j]) < .0001f, "Matrix convention mismatch");
    }
    const Invite invite{76561198000000001ull, 0x123456789abcdef0ull};
    const auto parsed = parse_invite(format_invite(invite));
    check(parsed && parsed->steam_id == invite.steam_id && parsed->secret == invite.secret,
          "Join code does not round trip");
    check(!parse_invite("0-123456789abcdef0") && !parse_invite("76561198000000001-0000000000000000") &&
              !parse_invite("76561198000000001-123456789abcdef0 extra") &&
              !parse_invite("76561198000000001-x23456789abcdef0"),
          "Malformed invite accepted");
}
void malformed_input() {
    std::mt19937 random(42);
    for (unsigned i = 0; i < 20000; ++i) {
        std::vector<std::uint8_t> bytes(random() % 1024);
        for (auto &b : bytes)
            b = static_cast<std::uint8_t>(random());
        (void)decode(bytes);
    }
    auto original = encode(frame(7, 12));
    for (unsigned i = 0; i < 4000; ++i) {
        auto bytes = original;
        bytes[random() % bytes.size()] = static_cast<std::uint8_t>(random());
        const auto p = decode(bytes);
        if (p && p->kind == PacketKind::pose)
            check(valid_pose(p->pose), "Decoder returned unsafe pose");
    }
}
void cosmetics_codec() {
    auto p = frame(0xfffffffeU, 0);
    p.kind = PacketKind::cosmetics;
    p.appearance = {{skater_recipe_key,
                     2,
                     {0, 0x3f000000, 0xffffffff},
                     {{11, "Own_Shirt", {}}, {12, "", {0, 0x3f800000, 5}}}},
                    {board_recipe_key, 1, {0x3f800000}, {{13, "Own_Deck", {7}}}}};
    const auto bytes = encode(p);
    const auto decoded = decode(bytes);
    check(decoded && decoded->appearance == p.appearance && !decoded->appearance.hide_tag && !decoded->appearance.hide_items,
          "Cosmetic fields or opaque parameter bits were lost");
    // A player's choices to go without their backend tag, or its animated items, travel with
    // their outfit, each on its own.
    for (const auto &[tag, items] : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
        auto hidden = p;
        hidden.appearance.hide_tag = tag, hidden.appearance.hide_items = items;
        const auto told = decode(encode(hidden));
        check(told && told->appearance.hide_tag == tag && told->appearance.hide_items == items &&
                  told->appearance == hidden.appearance && !(told->appearance == p.appearance),
              "A player's choice to hide their tag or their items was lost");
    }
    // So does how they have each marked cosmetic animate.
    auto styled = p;
    styled.appearance.marks[0] = {MarkMode::gradient, {1, 2, 3}, {250, 251, 252}, 2};
    styled.appearance.marks[4] = {MarkMode::off, {}, {}, 1};
    styled.appearance.marks.back() = {MarkMode::solid, {9, 8, 7}, {}, 0};
    const auto kept = decode(encode(styled));
    check(kept && kept->appearance == styled.appearance && kept->appearance.marks[0].to[2] == 252 &&
              kept->appearance.marks.back().mode == MarkMode::solid && kept->appearance.marks.back().from[0] == 9 &&
              kept->appearance.marks[1] == MarkStyle{} && !(kept->appearance == p.appearance),
          "A player's cosmetic styles were lost");
    check(!valid_mark_style({static_cast<MarkMode>(4), {}, {}, 0}) && !valid_mark_style({MarkMode::standard, {}, {}, 3}),
          "A cosmetic style no menu can make was accepted");
    for (std::size_t n = 0; n < bytes.size(); ++n)
        check(!decode(std::span(bytes).first(n)), "Truncated cosmetics accepted");
    auto corrupt = bytes;
    corrupt.push_back(0);
    check(!decode(corrupt), "Cosmetic trailing bytes accepted");
    corrupt = bytes;
    corrupt[4] = 2;
    check(!decode(corrupt), "Old cosmetic-free protocol accepted");
    corrupt = bytes;
    corrupt[packet_header_size + 8] = 255;
    corrupt[packet_header_size + 9] = 255;
    check(!decode(corrupt), "Oversized cosmetic scalar allocation accepted");
    auto reject = [&](const Appearance &a) {
        auto bad = p;
        bad.appearance = a;
        try {
            (void)encode(bad);
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    auto bad = p.appearance;
    bad.skater.items[1].slot = 11;
    check(reject(bad), "Duplicate cosmetic slots accepted");
    bad = p.appearance;
    bad.board.key = skater_recipe_key;
    check(reject(bad), "Wrong cosmetic template accepted");
    bad = p.appearance;
    bad.skater.items[0].asset = std::string("Own_\0bad", 8);
    check(reject(bad), "Embedded null in native asset name accepted");
    bad = p.appearance;
    bad.skater.items[0].parameters.resize(max_cosmetic_parameters + 1);
    check(reject(bad), "Oversized cosmetic parameters accepted");
    bad = p.appearance;
    bad.skater.items[0].asset.resize(max_cosmetic_asset + 1, 'a');
    check(reject(bad), "Oversized cosmetic asset name accepted");
    bad = p.appearance;
    bad.skater.items.clear();
    for (std::uint32_t i = 1; i <= max_cosmetic_slots; ++i)
        bad.skater.items.push_back(
            {i, std::string(max_cosmetic_asset, 'x'), std::vector<std::uint32_t>(max_cosmetic_parameters)});
    check(reject(bad), "Aggregate cosmetic packet budget not enforced");
    AppearanceBuffer state;
    PoseBuffer poses;
    check(poses.push(frame(9, 1), 1000) && state.push(p), "Pose sequence must not gate reliable cosmetics");
    check(!state.push(p), "Duplicate cosmetics accepted");
    p.sequence = 0;
    p.appearance.board.items[0].asset = "Own_AnotherDeck";
    check(state.push(p) && state.value() == p.appearance, "Appearance sequence wrap/update failed");
    p.sequence = 0xffffffff;
    check(!state.push(p), "Reordered appearance overwrote current outfit");
    p.epoch++;
    check(!state.push(p), "Wrong epoch overwrote current outfit");
    state.clear();
    check(!state.value() && state.push(p), "Reconnect did not reset cosmetic state");
    std::mt19937 random(81);
    for (unsigned i = 0; i < 4000; ++i) {
        auto mutated = bytes;
        mutated[random() % mutated.size()] = static_cast<std::uint8_t>(random());
        const auto out = decode(mutated);
        if (out && out->kind == PacketKind::cosmetics)
            check(valid_appearance(out->appearance), "Decoder returned invalid cosmetics");
    }
}
void sound_codec_and_timing() {
    auto p = frame(7, 0);
    p.kind = PacketKind::audio;
    AudioState idle, hit;
    idle.values[3] = 12.5f;
    idle.selectors[0] = 0xfedcba98;
    hit = idle;
    hit.flags[2] = 1;
    p.audio = {{20000, idle}, {10000, hit}, {0, idle}};
    const auto bytes = encode(p);
    const auto out = decode(bytes);
    check(out && out->audio.size() == 3 && out->audio[1].state == hit && out->audio[0].age_us == 20000,
          "Sound packet lost parameters or short action edge");
    for (std::size_t n = 0; n < bytes.size(); ++n)
        check(!decode(std::span(bytes).first(n)), "Truncated sound packet accepted");
    auto bad = bytes;
    bad.push_back(0);
    check(!decode(bad), "Trailing sound bytes accepted");
    bad = bytes;
    bad[packet_header_size + 0] = 255;
    bad[packet_header_size + 1] = 255;
    check(!decode(bad), "Unbounded sound sample count accepted");
    bad = bytes;
    bad[4] = 3;
    check(!decode(bad), "Cosmetics-only protocol accepted for sound");
    AudioBuffer buffer;
    check(buffer.push(p, 1000000), "Sound buffer refused first batch");
    check(!buffer.sample(1079999), "Sound played before pose delay");
    check(buffer.sample(1080000) == idle, "First delayed sound frame missing");
    check(buffer.sample(1090000) == hit, "Short contact/action edge skipped");
    check(buffer.sample(1100000) == idle, "Contact/action edge did not reset");
    check(!buffer.push(p, 1100000), "Duplicate sound packet replayed");
    ++p.epoch;
    ++p.sequence;
    check(!buffer.push(p, 1100000), "Wrong sound epoch accepted");
    buffer.clear();
    check(!buffer.sample(1100000) && buffer.push(p, 1100000), "Sound reconnect retained old state");
    check(!buffer.sample(2100001), "Stale sound could continue rolling indefinitely");
    buffer.clear();
    p.audio = {{20000, idle}, {10000, idle}, {0, idle}};
    p.audio.back().state.values[3] = 15;
    check(buffer.push(p, 1000000) && buffer.sample(1100000) == p.audio.back().state && buffer.size() == 0,
          "Continuous sound frames must coalesce for lower-frame-rate playback");
    buffer.clear();
    check(buffer.push(p, 1000000) && !buffer.sample(1500000), "Old sound backlog replayed after stall");
    auto reject = [&](Packet x) {
        try {
            (void)encode(x);
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    auto invalid = p;
    invalid.audio[1].age_us = 30000;
    check(reject(invalid), "Backwards sound timestamps accepted");
    invalid = p;
    invalid.audio[0].age_us = 1000001;
    check(reject(invalid), "Unbounded sound sample age accepted");
    invalid = p;
    invalid.audio[0].state.values[0] = std::numeric_limits<float>::quiet_NaN();
    check(reject(invalid), "NaN sound parameter accepted");
    invalid = p;
    invalid.audio[0].state.flags[0] = 1;
    check(reject(invalid), "Peer can select local-player audio mix");
    invalid = p;
    invalid.audio[0].state.flags[5] = 2;
    check(reject(invalid), "Invalid native sound boolean accepted");
    invalid = p;
    invalid.audio.resize(max_audio_samples + 1);
    check(reject(invalid), "Too many sound frames accepted");
    p.audio.assign(max_audio_samples, AudioSample{0, idle});
    check(encode(p).size() <= max_packet && decode(encode(p)).has_value(),
          "Largest sound batch exceeds transport limit");
    std::mt19937 random(109);
    for (unsigned i = 0; i < 4000; ++i) {
        auto mutated = bytes;
        mutated[random() % mutated.size()] = static_cast<std::uint8_t>(random());
        const auto result = decode(mutated);
        if (result && result->kind == PacketKind::audio)
            check(valid_audio_batch(result->audio), "Invalid decoded sound state");
    }
}
void object_codec() {
    using dingosdk::NetworkObject;
    const auto reject = [](const Packet &packet) {
        try { return !decode(encode(packet)); } catch (const std::invalid_argument &) { return true; }
    };
    ObjectState sender, receiver;
    std::vector<NetworkObject> objects;
    for (unsigned i = 0; i < max_owned_objects; ++i)
        objects.push_back({i + 1ULL, "own_bk_" + std::string(249, 'x'),
                           {static_cast<float>(i), 2, 3}, {0, 0, 0, 1}, i == 7 ? 2.5f : 1.0f});
    sender.replace(objects);
    const auto chunks = sender.updates(0);
    check(chunks.size() == 16, "Largest owned park was not split into bounded messages");
    Packet p; p.kind = PacketKind::objects; p.session = 9; p.epoch = 10; p.map = 11; p.source = 76561198000000001ULL;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        p.objects = chunks[i];
        const auto bytes = encode(p);
        check(bytes.size() <= max_packet, "Maximum object chunk exceeded the transport bound");
        const auto decoded = decode(bytes);
        check(decoded.has_value(), "Object packet failed to decode");
        const auto accepted = receiver.receive(decoded->objects);
        check(i + 1 == chunks.size() ? accepted == ObjectState::Result::applied : accepted == ObjectState::Result::pending,
              "Object snapshot was applied before all parts arrived");
        if (i + 1 < chunks.size()) check(receiver.objects().empty(), "Partial park mutated visible state");
    }
    check(receiver.layout() == objects && sender.updates(sender.revision()).empty(), "Object round-trip lost data or resent unchanged state");
    const auto old = sender.revision();
    objects.erase(objects.begin()); objects[0].position[1] = 50; objects[0].scale = 3.25f; sender.replace(objects);
    const auto delta = sender.updates(old);
    check(delta.size() == 1 && delta[0].removed.size() == 1 && delta[0].objects.size() == 1 &&
              receiver.receive(delta[0]) == ObjectState::Result::applied && receiver.layout() == objects,
          "Changed-only move/delete did not preserve other objects");
    check(receiver.receive(chunks[0]) == ObjectState::Result::ignored && receiver.layout() == objects,
          "Stale snapshot resurrected a deleted object");
    auto invalid = delta[0]; invalid.revision += 3; invalid.base = 999;
    check(receiver.receive(invalid) == ObjectState::Result::invalid, "Invalid delta base was accepted");
    p.objects = chunks[0]; p.objects.objects[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    check(reject(p), "NaN object pose was encoded");
    p.objects = chunks[0]; p.objects.objects[0].scale = 0;
    check(reject(p), "Invalid object scale was encoded");
    p.objects = chunks[0]; p.objects.objects[0].item = "../../asset";
    check(reject(p), "Non-Build-Kit path was encoded as an object");
    p.objects = chunks[0]; p.objects.objects[1].id = p.objects.objects[0].id;
    check(reject(p), "Duplicate object identity was accepted");
    p.objects = chunks[0]; p.objects.parts = 33;
    check(reject(p), "Unbounded object snapshot part count was accepted");
    p.objects = chunks[0];
    const auto raw = encode(p);
    for (std::size_t length = 0; length < raw.size(); length += 17)
        check(!decode(std::span(raw).first(length)), "Truncated object update decoded successfully");
    Packet roster; roster.kind = PacketKind::roster; roster.session = 9; roster.epoch = 10; roster.map = 11;
    roster.capacity = 32;
    for (unsigned i = 0; i < 32; ++i) roster.members.push_back({76561198000000001ULL + i, 10 + i, "Skater"});
    check(decode(encode(roster))->members.size() == 32, "A 32-player roster failed to round-trip");
    roster.members.push_back({76561198000000033ULL, 50, "Extra"});
    check(reject(roster), "A 33rd roster member was accepted");
}
void chat_codec() {
    const auto reject = [](const Packet &packet) {
        try { static_cast<void>(encode(packet)); return false; } catch (const std::invalid_argument &) { return true; }
    };
    Packet p;
    p.kind = PacketKind::chat; p.session = 9; p.epoch = 10; p.map = 11; p.source = 76561198000000001ULL;
    p.text = "kickflip the gap \xF0\x9F\x9B\xB9";
    const auto decoded = decode(encode(p));
    check(decoded && decoded->kind == PacketKind::chat && decoded->text == p.text && decoded->source == p.source,
          "Chat message failed to round-trip");
    const auto wire = decode_wire(encode_wire(p));
    check(wire && wire->text == p.text, "Chat message failed the wire codec");
    for (const char *bad : {"", "   ", "line\nbreak", "bell\x07", "\xC0\xAF", "\xED\xA0\x80", "c1\xC2\x85"}) {
        p.text = bad;
        check(reject(p), "Invalid chat text was encoded");
    }
    p.text = std::string(dingosdk::multiplayer_chat_max_bytes, 'a');
    check(!reject(p), "A full-length chat message was refused");
    p.text += 'a';
    check(reject(p), "An overlong chat message was encoded");
    p.text = "hello";
    auto bytes = encode(p);
    bytes.back() = 0x07;
    check(!decode(bytes), "A chat control character decoded");
    p.source = 0;
    check(reject(p), "Chat without a sender was encoded");
    check(clean_chat_text("  hi\tthere \n") == "hi there", "Chat cleaning kept blanks or controls");
    check(clean_chat_text(std::string("a\xFF") + "b") == "ab", "Chat cleaning kept broken UTF-8");
    check(clean_chat_text(std::string(199, 'a') + "\xC3\xA9").size() == 199, "Chat cleaning cut a character in half");
    check(clean_chat_text("\x01\x02").empty(), "Chat cleaning left only controls");
    check(valid_chat_text(clean_chat_text("  typed \xE2\x80\x94 text  ")), "Cleaned chat text is not valid");
}
// Linked throwdowns: the packet carries one opaque message; the message codec is strict.
void throwdown_codec() {
    const auto reject = [](const Packet &packet) {
        try { static_cast<void>(encode(packet)); return false; } catch (const std::invalid_argument &) { return true; }
    };
    constexpr std::uint64_t leader = 76561198000000001ULL, guest = 76561198000000002ULL;
    ThrowdownMessage offer;
    offer.kind = ThrowdownMessage::Kind::offer; offer.leader = leader; offer.id = 0x7c8bf0b2;
    offer.series = "JamSession";
    offer.placement.assign(300, 7); offer.settings.assign(200, 9);
    offer.order = {guest};
    const auto message = encode_throwdown(offer);
    check(decode_throwdown(message) == offer, "Throwdown offer failed to round-trip");
    Packet p;
    p.kind = PacketKind::throwdown; p.session = 9; p.epoch = 10; p.map = 11; p.source = leader;
    p.throwdown = message;
    const auto decoded = decode(encode(p));
    check(decoded && decoded->kind == PacketKind::throwdown && decoded->throwdown == message && decoded->source == leader,
          "Throwdown packet failed to round-trip");
    const auto wire = decode_wire(encode_wire(p));
    check(wire && wire->throwdown == message, "Throwdown packet failed the wire codec");
    p.throwdown.clear();
    check(reject(p), "An empty throwdown packet was encoded");
    p.throwdown.assign(max_throwdown_message + 1, 1);
    check(reject(p), "An overlong throwdown packet was encoded");
    p.throwdown = message; p.source = 0;
    check(reject(p), "A throwdown packet without a sender was encoded");

    for (const auto kind : {ThrowdownMessage::Kind::close, ThrowdownMessage::Kind::join, ThrowdownMessage::Kind::leave}) {
        ThrowdownMessage m; m.kind = kind; m.leader = leader; m.id = 5;
        check(decode_throwdown(encode_throwdown(m)) == m, "Throwdown control message failed to round-trip");
    }
    // Coop challenges.
    ThrowdownMessage challenge;
    challenge.kind = ThrowdownMessage::Kind::challenge_start; challenge.leader = leader; challenge.id = 3;
    challenge.series = "OTS"; challenge.challenge = "Plot-014-OTS-03"; challenge.add = true; challenge.order = {leader, guest};
    check(decode_throwdown(encode_throwdown(challenge)) == challenge, "Challenge start failed to round-trip");
    auto no_leader = challenge; no_leader.order = {guest, leader};
    check(!valid_throwdown(no_leader), "A challenge start not led by its leader was valid");
    auto alone = challenge; alone.order = {leader};
    check(!valid_throwdown(alone), "A challenge start with nobody invited was valid");
    auto spaced = challenge; spaced.challenge = "Plot 014";
    check(!valid_throwdown(spaced), "A challenge id with a space was valid");
    ThrowdownMessage finished;
    finished.kind = ThrowdownMessage::Kind::challenge_attempt; finished.leader = leader; finished.id = 3;
    finished.criteria.assign(4 * challenge_criteria_size, 0x41); finished.indexes.assign(16, 1);
    check(decode_throwdown(encode_throwdown(finished)) == finished, "Challenge attempt failed to round-trip");
    auto ragged = finished; ragged.criteria.pop_back();
    check(!valid_throwdown(ragged), "A challenge attempt with a partial criteria entry was valid");
    for (const auto kind : {ThrowdownMessage::Kind::challenge_optout, ThrowdownMessage::Kind::challenge_slam,
                            ThrowdownMessage::Kind::challenge_leave}) {
        ThrowdownMessage m; m.kind = kind; m.leader = leader; m.id = 3; m.value = kind == ThrowdownMessage::Kind::challenge_slam ? 7 : 0;
        check(decode_throwdown(encode_throwdown(m)) == m, "Challenge control message failed to round-trip");
    }
    // Party beacons.
    ThrowdownMessage beacon;
    beacon.kind = ThrowdownMessage::Kind::beacon; beacon.leader = leader; beacon.id = 2; beacon.add = true;
    for (unsigned i = 0; i < beacon.location.size(); ++i) beacon.location[i] = 0.5f * static_cast<float>(i);
    check(decode_throwdown(encode_throwdown(beacon)) == beacon, "Beacon failed to round-trip");
    auto nowhere = beacon; nowhere.location[13] = std::numeric_limits<float>::infinity();
    check(!valid_throwdown(nowhere), "A beacon at infinity was valid");
    ThrowdownMessage lifted;
    lifted.kind = ThrowdownMessage::Kind::beacon; lifted.leader = leader; lifted.id = 3;
    check(decode_throwdown(encode_throwdown(lifted)) == lifted, "Beacon removal failed to round-trip");
    ThrowdownMessage start; start.kind = ThrowdownMessage::Kind::start; start.leader = leader; start.id = 5;
    start.order = {leader, guest};
    check(decode_throwdown(encode_throwdown(start)) == start, "Throwdown start failed to round-trip");
    ThrowdownMessage score; score.kind = ThrowdownMessage::Kind::score; score.leader = leader; score.id = 5; score.value = -3;
    check(decode_throwdown(encode_throwdown(score)) == score, "Throwdown score failed to round-trip");
    ThrowdownMessage row; row.kind = ThrowdownMessage::Kind::row; row.leader = leader; row.id = 5;
    row.board = 1; row.add = true; row.value = 250;
    check(decode_throwdown(encode_throwdown(row)) == row, "Throwdown leaderboard row failed to round-trip");
    ThrowdownMessage turn_end; turn_end.kind = ThrowdownMessage::Kind::turn_end; turn_end.leader = leader; turn_end.id = 5;
    turn_end.value = 2;
    check(decode_throwdown(encode_throwdown(turn_end)) == turn_end, "Throwdown turn end failed to round-trip");
    ThrowdownMessage attempt; attempt.kind = ThrowdownMessage::Kind::attempt; attempt.leader = leader; attempt.id = 5;
    attempt.value = 3; attempt.add = true;
    for (std::size_t i = 0; i < attempt.trick.size(); ++i) attempt.trick[i] = static_cast<std::uint8_t>(i * 7);
    check(decode_throwdown(encode_throwdown(attempt)) == attempt, "Throwdown S.K.A.T.E. attempt failed to round-trip");
    auto no_turn = attempt; no_turn.value = 0;
    try { static_cast<void>(encode_throwdown(no_turn)); check(false, "An attempt without a turn was encoded"); }
    catch (const std::invalid_argument &) {}
    auto flag = encode_throwdown(row);
    flag[flag.size() - 5] = 2;
    check(!decode_throwdown(flag), "A leaderboard row with an invalid add flag decoded");

    const auto invalid = [](ThrowdownMessage m) {
        try { static_cast<void>(encode_throwdown(m)); return false; } catch (const std::invalid_argument &) { return true; }
    };
    auto bad = offer; bad.series = "Jam Session"; check(invalid(bad), "A throwdown mode with a space was encoded");
    bad = offer; bad.series.clear(); check(invalid(bad), "A throwdown offer without a mode was encoded");
    bad = offer; bad.placement.clear(); check(invalid(bad), "A throwdown offer without a placement was encoded");
    bad = offer; bad.settings.assign(max_throwdown_params + 1, 0); check(invalid(bad), "Oversized throwdown settings were encoded");
    bad = offer; bad.id = 0; check(invalid(bad), "A throwdown without an id was encoded");
    bad = offer; bad.leader = 42; check(invalid(bad), "A throwdown leader that is not a Steam user was encoded");
    bad = offer; bad.order = {7}; check(invalid(bad), "A throwdown member that is not a Steam user was encoded");
    bad = start; bad.order.clear(); check(invalid(bad), "A throwdown start without players was encoded");
    bad = start; bad.order.assign(max_throwdown_order + 1, guest); check(invalid(bad), "Too many throwdown players were encoded");
    bad = row; bad.board = max_throwdown_boards; check(invalid(bad), "A leaderboard index past the limit was encoded");

    // Every truncation, every trailing byte and every single-byte mutation either
    // decodes to a valid message or is refused; none may throw or read past the end.
    for (std::size_t i = 0; i < message.size(); ++i)
        check(!decode_throwdown(std::span(message.data(), i)), "A truncated throwdown message decoded");
    auto longer = message; longer.push_back(0);
    check(!decode_throwdown(longer), "A throwdown message with trailing bytes decoded");
    std::mt19937 random(7);
    for (unsigned i = 0; i < 20000; ++i) {
        auto mutated = i % 2 ? message : encode_throwdown(start);
        mutated[random() % mutated.size()] = static_cast<std::uint8_t>(random());
        if (const auto m = decode_throwdown(mutated)) check(valid_throwdown(*m), "A mutated throwdown decoded invalid");
    }
}
} // namespace
// Dedicated servers: a game-server host in the roster, player names in hellos,
// admin requests, server invites and the browser's tag format.
void dedicated_server_codec() {
    const auto reject = [](const Packet &packet) {
        try { static_cast<void>(encode(packet)); return false; } catch (const std::invalid_argument &) { return true; }
    };
    constexpr std::uint64_t server = 0x0140C976E39F681FULL, player = 76561198000000001ULL, other = 76561198000000002ULL;
    check(game_server_steam_id(server) && !individual_steam_id(server), "Anonymous game server ID not recognised");
    check(individual_steam_id(player) && !game_server_steam_id(player), "Player ID taken for a game server");

    Packet roster;
    roster.kind = PacketKind::roster; roster.session = 9; roster.epoch = 10; roster.map = 11; roster.source = server;
    roster.members = {{server, 10, "My server"}, {player, 20, "Skater", true}, {other, 30, "Other"}};
    roster.voice_range = 450;
    roster.guest_noclip = false;
    Packet teleport;
    teleport.kind = PacketKind::teleport; teleport.session = 9; teleport.epoch = 10; teleport.map = 11; teleport.source = server;
    teleport.teleport = {612.5f, 199.25f, -1075.75f};
    check(decode(encode(teleport)) && decode(encode(teleport))->teleport == teleport.teleport, "Teleport position lost");
    teleport.teleport[1] = std::numeric_limits<float>::infinity();
    check(reject(teleport), "A teleport to infinity was encoded");
    // The party-collision flag rides in the top bit of the pose's rate field.
    Packet solid;
    solid.kind = PacketKind::pose; solid.session = 9; solid.epoch = 10; solid.map = 11; solid.source = player;
    solid.pose_interval_us = 1000000 / 60;
    for (const bool compact : {false, true})
        for (const bool on : {false, true}) {
            solid.player_collision = on;
            const auto decoded_pose = decode(encode(solid, compact));
            check(decoded_pose && decoded_pose->player_collision == on &&
                      decoded_pose->pose_interval_us == solid.pose_interval_us,
                  "Party collision flag or pose rate lost");
        }
    const auto decoded = decode(encode(roster));
    check(decoded && decoded->members == roster.members && decoded->voice_range == 450.f,
          "Server roster, admin flags or voice range lost");
    check(decoded && !decoded->guest_noclip && decoded->guest_no_bail && decoded->guest_boosts,
          "Guest noclip / No Bail / boost permissions lost");
    // Parties ride in the roster: each has one leader and at least two members; a server is in none.
    auto party = roster;
    party.members[1].party = party.members[2].party = 7;
    party.members[1].party_leader = party.members[1].party_open = true;
    party.members[2].speeding = true;
    party.members[1].scoring = true;
    const auto partied = decode(encode(party));
    check(partied && partied->members == party.members,
          "Party membership, leader, openness, the speed flag or the scoring flag lost");
    party.members[2].speeding = false;
    party.members[1].scoring = false;
    auto two_leaders = party;
    two_leaders.members[2].party_leader = true;
    check(reject(two_leaders), "A party with two leaders was encoded");
    auto alone = party;
    alone.members[2].party = 8;
    check(reject(alone), "A one-player party was encoded");
    auto open_member = party;
    open_member.members[2].party_open = true;
    check(reject(open_member), "A party opened by a non-leader was encoded");
    auto server_party = party;
    server_party.members[0].party = 7;
    check(reject(server_party), "A dedicated server was put in a party");
    Packet party_request;
    party_request.kind = PacketKind::party; party_request.session = 9; party_request.epoch = 20; party_request.map = 11; party_request.source = player;
    party_request.party_action = PartyAction::invite; party_request.party_player = other;
    const auto asked = decode(encode(party_request));
    check(asked && asked->party_action == PartyAction::invite && asked->party_player == other, "Party party_request lost");
    party_request.party_action = PartyAction::leave;
    check(reject(party_request), "A party leave naming a player was encoded");
    party_request.party_player = 0;
    check(decode(encode(party_request)) && decode(encode(party_request))->party_action == PartyAction::leave, "Party leave lost");
    party_request.party_action = static_cast<PartyAction>(40);
    check(reject(party_request), "An unknown party action was encoded");
    auto voting = roster;
    voting.server_votes = server_vote_map | server_vote_time;
    check(decode(encode(voting)) && decode(encode(voting))->server_votes == (server_vote_map | server_vote_time),
          "Server votes lost");
    auto no_boosts = roster;
    no_boosts.guest_boosts = false;
    check(decode(encode(no_boosts)) && !decode(encode(no_boosts))->guest_boosts, "Guest boosts permission lost");
    check(decoded && decoded->enforce_tuning, "Physics tuning enforcement lost");
    auto own_tuning = voting;
    own_tuning.enforce_tuning = false;
    check(decode(encode(own_tuning)) && !decode(encode(own_tuning))->enforce_tuning &&
              decode(encode(own_tuning))->server_votes == voting.server_votes,
          "Physics tuning choice lost or mixed with the server votes");
    Packet scoring;
    scoring.kind = PacketKind::scoring; scoring.session = 9; scoring.epoch = 10; scoring.map = 11; scoring.source = player;
    check(decode(encode(scoring)) && decode(encode(scoring))->scoring == 0 && decode(encode(scoring))->text.empty(),
          "The game's own scoring report lost");
    scoring.scoring = 0x8000'0000'1234'5678ULL;
    scoring.text = "BigPoints, OtherMod";
    check(decode(encode(scoring)) && decode(encode(scoring))->scoring == scoring.scoring &&
              decode(encode(scoring))->text == scoring.text,
          "Scoring fingerprint or mod names lost");
    scoring.text = std::string(max_admin_text + 1, 'a');
    check(reject(scoring), "Oversized scoring mod names encoded");
    scoring.text = "bad\nname";
    check(reject(scoring), "Scoring mod names with a control character encoded");
    Packet tuning;
    tuning.kind = PacketKind::physics_tuning; tuning.session = 9; tuning.epoch = 10; tuning.map = 11; tuning.source = player;
    check(decode(encode(tuning)) && decode(encode(tuning))->tuning.empty(), "Empty physics tuning (the game's own) lost");
    tuning.tuning = {1, 0, 2, 0, 0x20, 0, 4, 0, 1, 2, 3, 4};
    check(decode(encode(tuning)) && decode(encode(tuning))->tuning == tuning.tuning, "Physics tuning bytes lost");
    tuning.tuning.assign(max_physics_tuning + 1, 0);
    check(reject(tuning), "Oversized physics tuning encoded");
    Packet extras;
    extras.kind = PacketKind::physics_extras; extras.session = 9; extras.epoch = 10; extras.map = 11; extras.source = player;
    check(decode(encode(extras)) && decode(encode(extras))->kind == PacketKind::physics_extras && decode(encode(extras))->extras.empty(),
          "Empty physics extras (the game's own) lost");
    extras.extras = {1, 0xde, 0xad, 0xbe, 0xef, 0, 0, 0x80, 0x3f};
    check(decode(encode(extras)) && decode(encode(extras))->extras == extras.extras, "Physics extras bytes lost");
    extras.extras.assign(dingosdk::max_physics_extras, 7);
    check(decode(encode(extras)) && decode(encode(extras))->extras.size() == dingosdk::max_physics_extras, "The largest physics extras lost");
    extras.extras.assign(dingosdk::max_physics_extras + 1, 0);
    check(reject(extras), "Oversized physics extras encoded");
    extras.extras = {1};
    extras.source = 0;
    check(reject(extras), "Physics extras from nobody encoded");
    auto moved = roster;
    moved.members = {{player, 20, "Skater"}, {server, 10, "My server"}};
    check(reject(moved), "A game server accepted as a guest");
    auto crowned = roster;
    crowned.members[0].admin = true;
    check(reject(crowned), "A host marked as an admin");
    auto far = roster;
    far.voice_range = 5;
    check(reject(far), "Invalid voice range encoded");

    Packet hello;
    hello.kind = PacketKind::hello; hello.session = 9; hello.epoch = 20; hello.map = 11; hello.source = player;
    hello.text = "Skater — one";
    const auto greeting = decode(encode(hello));
    check(greeting && greeting->text == hello.text, "Hello name lost");
    hello.text = std::string(max_member_name + 1, 'a');
    check(reject(hello), "Overlong hello name encoded");
    hello.text.clear();
    check(decode(encode(hello)) && decode(encode(hello))->text.empty(), "Nameless hello refused");

    Packet admin;
    admin.kind = PacketKind::admin; admin.session = 9; admin.epoch = 20; admin.map = 11; admin.source = player;
    admin.text = "map Levels/Game/DingoLevel_Root/DingoLevel_Root|Levels/Game/BAM_LevelRoot/BAM_LevelRoot";
    const auto request = decode_wire(encode_wire(admin));
    check(request && request->kind == PacketKind::admin && request->text == admin.text, "Admin request lost");
    admin.text = "bad\nline";
    check(reject(admin), "Admin control character encoded");
    admin.text = std::string(max_admin_text + 1, 'a');
    check(reject(admin), "Overlong admin request encoded");

    Packet bans;
    bans.kind = PacketKind::bans; bans.session = 9; bans.epoch = 10; bans.map = 11; bans.source = server;
    bans.bans = {{player, "Griefer", 1790000000}, {other, "", 0}};
    bans.ban_total = 300;
    const auto list = decode_wire(encode_wire(bans));
    check(list && list->kind == PacketKind::bans && list->ban_total == 300 && list->bans.size() == 2 &&
              list->bans[0].id == player && list->bans[0].name == "Griefer" && list->bans[0].added == 1790000000 &&
              list->bans[1].id == other && list->bans[1].name.empty(), "Server ban list lost");
    auto overfull = bans;
    overfull.ban_total = 1;
    check(reject(overfull), "Ban list longer than its total encoded");
    auto spoofed = bans;
    spoofed.bans[0].id = server;
    check(reject(spoofed), "A game server ID encoded as a banned player");

    Packet maps;
    maps.kind = PacketKind::maps; maps.session = 9; maps.epoch = 10; maps.map = 11; maps.source = server;
    maps.maps = {"Levels/Game/BAM_LevelRoot/BAM_LevelRoot", "Levels/Custom/bbcity/bbcity"};
    const auto map_list = decode_wire(encode_wire(maps));
    check(map_list && map_list->kind == PacketKind::maps && map_list->maps == maps.maps, "Server map list lost");
    auto pooled = maps;
    pooled.map_pool = {1, 0};
    pooled.map_rotation = 20;
    const auto pool_list = decode_wire(encode_wire(pooled));
    check(pool_list && pool_list->map_pool == pooled.map_pool && pool_list->map_rotation == 20, "Server map pool or rotation lost");
    Packet changed_map;
    changed_map.kind = PacketKind::world_state; changed_map.session = 9; changed_map.epoch = 10; changed_map.source = server; changed_map.world = 2;
    changed_map.destination = "Levels/Game/DingoLevel_Root/DingoLevel_Root|Levels/Game/dingolevel_reskate_momentumpark/x";
    changed_map.map = map_hash(changed_map.destination);
    changed_map.map_label = "Momentum Park";
    const auto arrived = decode_wire(encode_wire(changed_map));
    check(arrived && arrived->map_label == "Momentum Park" && arrived->destination == changed_map.destination,
          "The map's name lost from a map change");
    auto unnamed = changed_map;
    unnamed.map_label.assign(max_member_name + 1, 'a');
    check(reject(unnamed), "An overlong map name encoded");
    auto stray = maps;
    stray.map_pool = {2};
    check(reject(stray), "A map pool entry past the map list encoded");
    auto twice = maps;
    twice.map_pool = {0, 0};
    check(reject(twice), "A map listed twice in the pool encoded");
    auto endless = maps;
    endless.map_rotation = dingosdk::max_map_rotation + 1;
    check(reject(endless), "An overlong map rotation encoded");
    auto piped = maps;
    piped.maps[0] = "Levels/Game/DingoLevel_Root/DingoLevel_Root|Levels/Game/BAM_LevelRoot/BAM_LevelRoot";
    check(reject(piped), "A destination encoded as a server map");
    auto too_many = maps;
    too_many.maps.assign(max_server_maps + 1, "Levels/Game/BAM_LevelRoot/BAM_LevelRoot");
    check(reject(too_many), "Overlong server map list encoded");

    const auto invite = parse_invite(format_invite({server, 0x1234}));
    check(invite && invite->steam_id == server && invite->secret == 0x1234, "Server invite refused");

    dingosdk::server::Advertisement ad{"Big, friendly server", "San Vansterdam", 3, 16, true, true, 0xABCDEF0123456789ULL};
    const auto tags = dingosdk::server::server_tags(ad);
    check(tags.size() < 128, "Server tags exceed Steam's limit");
    const auto row = read_server_tags(tags, server);
    check(row && row->dedicated && row->name == "Big  friendly server" && row->map == ad.map && row->players == 3 &&
              row->capacity == 16 && row->password_required && row->code == format_invite({server, ad.secret}),
          "Server tags did not round trip");
    // A whole last character is kept, and one the byte limit cuts through is dropped whole.
    const auto accented = read_server_tags(dingosdk::server::server_tags({"Skate Caf\xC3\xA9", "Caf\xC3\xA9", 1, 8, false, true, 1}), server);
    check(accented && accented->name == "Skate Caf\xC3\xA9" && accented->map == "Caf\xC3\xA9",
          "A server name lost its non-ASCII last letter");
    for (std::size_t lead = 0; lead < 3; ++lead) {
        std::string euros(lead, 'a');
        for (int i = 0; i < 50; ++i) euros += "\xE2\x82\xAC";
        const auto cut = dingosdk::server::server_tags({euros, "Caf\xC3\xA9", 1, 8, false, true, 1});
        const auto name = cut.substr(cut.find(",n") + 2);
        check(cut.size() < 128 && name.size() > lead && (name.size() - lead) % 3 == 0, "A server name was cut inside a character");
    }
    check(!read_server_tags(tags, player), "A player's ID read as a server");
    check(!read_server_tags("reskate,v1,k1,c4", server), "An incompatible server version listed");
}
int main() {
    try {
        codec();
        tick_rates_codec();
        compressed_codec();
        sender_timeline();
        greetings();
        playback();
        skateboard_playback();
        matrices_and_invites();
        malformed_input();
        cosmetics_codec();
        sound_codec_and_timing();
        object_codec();
        chat_codec();
        throwdown_codec();
        dedicated_server_codec();
        std::cout << "Multiplayer protocol: all checks passed (40,000 malformed/mutated inputs).\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
