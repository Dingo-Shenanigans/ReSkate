#include "Extension/Multiplayer/Net/pose_batch.h"
#include "Extension/Multiplayer/Net/pose_codec.h"
#include "Extension/Multiplayer/Net/sound_codec.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <deque>
#include <iostream>
#include <map>
#include <random>
#include <unordered_map>

using namespace dingosdk::multiplayer;
using namespace dingosdk::multiplayer::pose_codec;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (ok) return;
    ++failures;
    std::cerr << "FAIL: " << what << '\n';
}
// A skater of the measured skeleton in some pose; `turn` moves it on a little or a lot.
Pose skeleton(std::mt19937 &random) {
    std::uniform_real_distribution<float> any(-1.f, 1.f);
    Pose pose;
    pose.skater.resize(finger_skeleton);
    pose.board.resize(17);
    const auto set = [&](Transform &t, float reach) {
        t.position = {any(random) * reach, any(random) * reach, any(random) * reach};
        t.rotation = {any(random), any(random), any(random), any(random)};
        float norm{};
        for (float v : t.rotation) norm += v * v;
        for (float &v : t.rotation) v /= std::sqrt(norm);
    };
    set(pose.root, 900.f);
    for (auto &t : pose.skater) set(t, 0.4f);
    for (auto &t : pose.board) set(t, 0.4f);
    pose.board[0].position = {512.25f, 31.5f, -640.125f};
    return pose;
}
void turn(Pose &pose, std::mt19937 &random, float amount, unsigned bones) {
    std::uniform_real_distribution<float> any(-amount, amount);
    std::uniform_int_distribution<std::size_t> which(0, pose.skater.size() - 1);
    for (unsigned i = 0; i < bones; ++i) {
        auto &t = pose.skater[which(random)];
        for (float &v : t.rotation) v += any(random);
        float norm{};
        for (float v : t.rotation) norm += v * v;
        for (float &v : t.rotation) v /= std::sqrt(norm);
    }
    pose.root.position[0] += any(random);
    pose.board[0].position[2] += any(random);
}

void exact_checks() {
    std::mt19937 random(5);
    auto pose = skeleton(random);
    // The awkward rotations: two components equal and as large as a smaller one can be, an exact
    // axis either way round, and all four equal.
    pose.skater[0].rotation = {.70710678f, .70710678f, 0, 0};
    pose.skater[1].rotation = {0, 0, 0, -1};
    pose.skater[2].rotation = {.5f, .5f, .5f, .5f};
    pose.skater[3].rotation = {-.5f, .5f, -.5f, .5f};
    pose.skater[4].scale = {4, 4, 4};
    const auto first = quantize(pose);
    check(first.bones.size() == 1 + finger_skeleton + 17, "A pose did not keep its bones");
    const auto whole_bytes = encode_whole(first);
    const auto back = decode(whole_bytes, nullptr);
    check(whole(whole_bytes) && back && *back == first, "A whole pose did not come back as it was sent");
    // Rounded and restored, a rotation is within a step and a position within a millimetre.
    const auto restored = restore(first);
    const float step = static_cast<float>(1U << rotation_shift) / rotation_scale;
    bool near = restored.skater.size() == pose.skater.size();
    for (std::size_t i = 0; near && i < pose.skater.size(); ++i) {
        const auto &a = pose.skater[i].rotation, &b = restored.skater[i].rotation;
        near = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) > 1.f - 3.f * step * step - 1e-4f;
        for (unsigned axis = 0; axis < 3; ++axis) near = near && std::abs(pose.skater[i].position[axis] - restored.skater[i].position[axis]) < 0.00051f;
    }
    check(near && std::abs(restored.board[0].position[0] - 512.25f) < 0.001f && restored.skater[4].scale == std::array<float, 3>{4, 4, 4},
          "A restored pose was not within a step of the original");
    // A chain of differences lands exactly on what rounding the pose directly gives: no drift.
    auto held_by_receiver = first;
    auto moving = pose;
    std::size_t bytes{};
    for (unsigned frame = 0; frame < 200; ++frame) {
        turn(moving, random, frame % 17 ? 0.01f : 0.6f, 70);
        if (frame == 60) moving.skater[9].scale = {1.5f, 1, 1};
        if (frame == 90) moving.skater[9].scale = {1, 1, 1};
        const auto now = quantize(moving);
        const auto delta = encode_delta(now, held_by_receiver);
        bytes += delta.size();
        const auto got = decode(delta, &held_by_receiver);
        if (!got || *got != now || whole(delta)) {
            check(false, "A difference did not rebuild the sender's pose exactly");
            break;
        }
        held_by_receiver = *got;
    }
    // Nothing changed: a handful of bytes.
    check(encode_delta(first, first).size() <= 6, "An unchanged pose was not nearly free");
    // Another skeleton cannot be a difference.
    auto other = first;
    other.bones.pop_back();
    --other.board;
    check(encode_delta(other, first).empty(), "A difference between two skeletons was offered");
    std::cout << "Pose codec: whole poses, exact chains of differences (" << bytes / 200 << " B a pose on the synthetic skater).\n";
}

void finger_checks() {
    std::mt19937 random(9);
    auto pose = skeleton(random);
    const auto start = quantize(pose);
    // Held: everything but the fingers follows; the receiver's fingers stay as they were.
    auto receiver = start;
    QuantPose truth = start;
    for (unsigned frame = 0; frame < 40; ++frame) {
        turn(pose, random, 0.2f, 200);
        truth = quantize(pose);
        const auto got = decode(encode_delta(truth, receiver, Fingers::held), &receiver);
        if (!got) return check(false, "A pose with its fingers held did not decode");
        receiver = *got;
    }
    bool body{true}, fingers_kept{true}, fingers_moved{};
    for (std::size_t bone = 0; bone < truth.bones.size(); ++bone) {
        if (finger_bone(truth, bone)) {
            fingers_kept = fingers_kept && receiver.bones[bone] == start.bones[bone];
            fingers_moved = fingers_moved || !(truth.bones[bone] == start.bones[bone]);
        } else {
            body = body && receiver.bones[bone] == truth.bones[bone];
        }
    }
    check(body && fingers_kept && fingers_moved, "Held fingers moved, or the rest of the pose did not follow");
    // Whole: the receiver's stale fingers are replaced, whatever its reference holds.
    turn(pose, random, 0.2f, 200);
    truth = quantize(pose);
    const auto synced = decode(encode_delta(truth, receiver, Fingers::whole), &receiver);
    check(synced && *synced == truth, "Fingers sent whole did not bring the receiver back to the sender's pose");
    // And held fingers are what makes the pose small.
    turn(pose, random, 0.02f, 395);
    const auto next = quantize(pose);
    check(encode_delta(next, truth, Fingers::held).size() < encode_delta(next, truth).size(), "Holding the fingers did not make a pose smaller");
    QuantPose small;
    small.skater = 12;
    small.board = 1;
    small.bones.resize(14);
    check(!finger_bone(small, 60), "Another skeleton was given fingers");
    std::cout << "Pose codec: fingers held, and sent whole again.\n";
}

void hostile_checks() {
    // Whatever arrives, decoding answers or declines: it never reads past the bytes or builds an
    // impossible pose.
    std::mt19937 random(21);
    auto pose = skeleton(random);
    const auto reference = quantize(pose);
    turn(pose, random, 0.3f, 120);
    const auto good = encode_delta(quantize(pose), reference);
    const auto whole_bytes = encode_whole(reference);
    unsigned answered{};
    for (const auto &source : {good, whole_bytes}) {
        for (std::size_t cut = 0; cut < source.size(); cut += std::max<std::size_t>(1, source.size() / 97)) {
            const std::vector<std::uint8_t> part(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(cut));
            answered += decode(part, &reference).has_value();
        }
        for (unsigned round = 0; round < 400; ++round) {
            auto damaged = source;
            for (unsigned flips = 0; flips < 1 + round % 5; ++flips) damaged[random() % damaged.size()] ^= static_cast<std::uint8_t>(1U << (random() % 8));
            if (const auto got = decode(damaged, &reference)) {
                ++answered;
                bool sane = got->bones.size() == std::size_t{1} + got->skater + got->board;
                for (const auto &bone : got->bones) {
                    sane = sane && bone.largest < 4;
                    for (const auto component : bone.rotation) sane = sane && std::abs(component) <= (32767 >> rotation_shift);
                    for (const auto mm : bone.position) sane = sane && std::abs(std::int64_t{mm}) <= position_limit;
                }
                if (!sane) return check(false, "Damaged bytes decoded to an impossible pose");
            }
        }
    }
    std::vector<std::uint8_t> noise(300);
    for (unsigned round = 0; round < 2000; ++round) {
        for (auto &byte : noise) byte = static_cast<std::uint8_t>(random());
        (void)decode(noise, round % 2 ? &reference : nullptr);
    }
    check(!decode({}, &reference) && !decode(good, nullptr), "Empty bytes or a difference without its reference decoded");
    std::cout << "Pose codec: cut, damaged and random bytes handled (" << answered << " still decoded, all to possible poses).\n";
}

// The server's sender and a game's receiver, joined by a link that delays, loses and reorders:
// the same code a dedicated server and a game run (pose_batch.h). Whatever the link does, every
// pose the receiver rebuilds must be the sender's exactly, no stream may stop for good, and whole
// poses must stay rare.
struct LinkResult {
    std::size_t sent{}, rebuilt{}, wrong{}, wholes{}, messages{}, bytes{}, stalled{};
};
LinkResult run_link(unsigned seed, double loss, unsigned delay_least, unsigned delay_most, std::size_t kept_poses) {
    using namespace pose_batch;
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> chance(0, 1);
    constexpr std::size_t sources = 6;
    constexpr unsigned ticks = 800; // 40 s at 20 a second
    struct Source {
        Pose pose;
        std::uint32_t sequence{};
        std::deque<Stream::Held> kept;
        unsigned every{1}; // sent every this many ticks
    };
    std::array<Source, sources> world;
    for (std::size_t i = 0; i < sources; ++i) {
        world[i].pose = skeleton(random);
        world[i].every = i == 3 ? 2 : i == 4 ? 4 : i == 5 ? 20 : 1;
    }
    std::map<std::pair<std::size_t, std::uint32_t>, QuantPose> truth;
    Sender sender;
    std::unordered_map<std::uint16_t, Stream> streams;
    Ack ack;
    bool ack_due{};
    struct Flying {
        unsigned arrives{};
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Flying> to_receiver, to_sender;
    const auto delay = [&] { return delay_least + static_cast<unsigned>(random() % (delay_most - delay_least + 1)); };
    LinkResult result;
    std::array<unsigned, sources> last_rebuilt{};
    // Fingers held and let go in turns, on every other player. A pose's number is its tick.
    const auto held = [](std::size_t source, unsigned tick) { return source % 2 == 1 && (tick / 100) % 4 == 1; };
    for (unsigned tick = 1; tick <= ticks; ++tick) {
        const std::uint64_t now = std::uint64_t{tick} * 50000;
        for (auto it = to_sender.begin(); it != to_sender.end();) {
            if (it->arrives > tick) {
                ++it;
                continue;
            }
            if (const auto got = Ack::read(it->bytes)) sender.ack(*got);
            it = to_sender.erase(it);
        }
        // The server's pass: every player moves, and those due go out.
        const auto emit = [&](std::span<const std::uint8_t> message, bool reliable) {
            ++result.messages;
            result.bytes += message.size();
            if (reliable || chance(random) >= loss) to_receiver.push_back({tick + delay(), {message.begin(), message.end()}});
            return true;
        };
        sender.begin(7, 42);
        for (std::size_t i = 0; i < sources; ++i) {
            auto &source = world[i];
            turn(source.pose, random, tick % 37 ? 0.02f : 0.5f, 90);
            ++source.sequence;
            source.kept.push_back({source.sequence, now, quantize(source.pose)});
            while (source.kept.size() > kept_poses) source.kept.pop_front();
            truth[{i, source.sequence}] = source.kept.back().pose;
            if (tick % source.every) continue;
            const auto find = [&](std::uint32_t sequence) -> std::optional<KeptView> {
                for (const auto &pose : source.kept)
                    if (pose.sequence == sequence) return KeptView{pose.sequence, pose.time_us, &pose.pose};
                return {};
            };
            const auto added = sender.add(1000 + i, 5, *find(source.sequence), find, held(i, tick), false, Rate::full, now, emit);
            result.wholes += added.did == Sender::Did::whole;
            result.sent += added.did != Sender::Did::nothing;
        }
        sender.flush(emit);
        // What reached the game this tick, in whatever order.
        std::shuffle(to_receiver.begin(), to_receiver.end(), random);
        for (auto it = to_receiver.begin(); it != to_receiver.end();) {
            if (it->arrives > tick) {
                ++it;
                continue;
            }
            if (const auto batch = read(it->bytes); batch && batch->world == 7 && batch->map == 42) {
                bool complete = true;
                for (const auto &entry : batch->entries) {
                    const auto got = rebuild(streams, entry);
                    if (!got) {
                        complete = false;
                        continue;
                    }
                    const auto who = static_cast<std::size_t>(got->source - 1000);
                    const auto &real = truth[{who, got->sequence}];
                    bool same = got->pose.bones.size() == real.bones.size() && got->epoch == 5;
                    // A pose sent with its fingers held has the receiver's old ones; every other is exact.
                    const bool fingers = !held(who, got->sequence);
                    for (std::size_t bone = 0; same && bone < real.bones.size(); ++bone)
                        if (fingers || !finger_bone(real, bone)) same = got->pose.bones[bone] == real.bones[bone];
                    result.wrong += !same;
                    ++result.rebuilt;
                    last_rebuilt[who] = tick;
                }
                if (complete) {
                    ack.note(batch->number);
                    ack_due = true;
                }
            }
            it = to_receiver.erase(it);
        }
        if (ack_due) {
            const auto bytes = ack.bytes();
            if (chance(random) >= loss) to_sender.push_back({tick + delay(), {bytes.begin(), bytes.end()}});
            ack_due = false;
        }
    }
    // Nobody left behind: each stream's last pose came within a few seconds of the end.
    for (std::size_t i = 0; i < sources; ++i) result.stalled += last_rebuilt[i] + 80 + world[i].every < ticks;
    return result;
}
void link_checks() {
    struct Case {
        const char *name;
        double loss;
        unsigned least, most;
        std::size_t kept, most_wholes;
    };
    for (const auto &c : {Case{"a clean link", 0.0, 1, 1, 300, 6}, Case{"a 400 ms link losing 5%", 0.05, 7, 9, 300, 12},
                          Case{"a link losing 30% and reordering", 0.30, 1, 6, 300, 60}, Case{"poses kept only briefly", 0.02, 2, 4, 12, 80}}) {
        const auto r = run_link(31, c.loss, c.least, c.most, c.kept);
        std::cout << "  " << c.name << ": " << r.sent << " poses sent, " << r.rebuilt << " rebuilt, " << r.wrong << " wrong, " << r.wholes << " whole, "
                  << r.bytes / std::max<std::size_t>(1, r.sent) << " B a pose in " << r.messages / 40 << " messages a second\n";
        check(r.wrong == 0, "A pose was rebuilt differently from what the server sent");
        check(r.stalled == 0, "A stream stopped for good");
        check(static_cast<double>(r.rebuilt) > static_cast<double>(r.sent) * (1 - c.loss) * 0.85, "Too few of the poses sent were rebuilt");
        check(r.wholes <= c.most_wholes, "Too many whole poses were needed");
    }
    // Acks: the newest and the 64 before it, in any order.
    pose_batch::Ack ack;
    for (const std::uint32_t number : {5U, 3U, 9U, 8U, 70U, 200U, 199U, 136U}) ack.note(number);
    check(ack.holds(200) && ack.holds(199) && ack.holds(136) && !ack.holds(135) && !ack.holds(70) && !ack.holds(198), "Acks did not keep the last 65 messages");
    const auto bytes = ack.bytes();
    const auto back = pose_batch::Ack::read(bytes);
    check(back && back->newest == 200 && back->holds(199) && back->holds(136) && !back->holds(137), "An ack did not survive being sent");
    // Noise under the right first four bytes is declined or rebuilds nothing; it never throws.
    std::vector<std::uint8_t> junk(96);
    std::copy(pose_batch::batch_magic.begin(), pose_batch::batch_magic.end(), junk.begin());
    std::mt19937 random(77);
    for (unsigned round = 0; round < 5000; ++round) {
        for (std::size_t i = 4; i < junk.size(); ++i) junk[i] = static_cast<std::uint8_t>(round % 3 ? random() : random() % 4);
        if (const auto batch = pose_batch::read(junk)) {
            std::unordered_map<std::uint16_t, pose_batch::Stream> streams;
            for (const auto &entry : batch->entries) (void)pose_batch::rebuild(streams, entry);
        }
    }
    std::cout << "Pose batches: delay, loss, reordering, short memory and noise handled.\n";
}

// Sizes on real play: a pose-dump recording (mp pose-dump), each record a time, a length and a
// compact packet. Not a pass or fail: prints what each choice costs, and checks every difference
// rebuilds the pose it was made from.
int bench(const char *path) {
    std::ifstream in(path, std::ios::binary);
    char magic[6]{};
    if (!in.read(magic, 6) || std::memcmp(magic, "RSPD1\n", 6) != 0) {
        std::cerr << "Not a pose-dump recording: " << path << '\n';
        return 2;
    }
    std::vector<QuantPose> poses;
    for (;;) {
        std::uint64_t time{};
        std::uint32_t length{};
        if (!in.read(reinterpret_cast<char *>(&time), 8) || !in.read(reinterpret_cast<char *>(&length), 4) || length > 1 << 20) break;
        std::vector<std::uint8_t> raw(length);
        if (!in.read(reinterpret_cast<char *>(raw.data()), length)) break;
        if (const auto packet = dingosdk::multiplayer::decode(raw); packet && packet->kind == PacketKind::pose) poses.push_back(quantize(packet->pose));
    }
    if (poses.size() < 100) {
        std::cerr << "Too few poses in " << path << '\n';
        return 2;
    }
    std::cout << poses.size() << " recorded poses, " << poses[0].bones.size() << " bones; rotations to "
              << static_cast<double>(1U << rotation_shift) * 0.00247 << " degrees\n";
    std::cout << "whole pose: " << encode_whole(poses[poses.size() / 2]).size() << " B\n";
    std::cout << "reference age   all bones   fingers held\n";
    bool exact = true;
    for (const std::size_t age : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{6}, std::size_t{12}, std::size_t{30}, std::size_t{150}, std::size_t{600}}) {
        std::size_t all{}, held{}, count{};
        for (std::size_t frame = age; frame < poses.size(); ++frame) {
            const auto delta = encode_delta(poses[frame], poses[frame - age]);
            all += delta.size();
            held += encode_delta(poses[frame], poses[frame - age], Fingers::held).size();
            ++count;
            if (frame % 13 == 0) {
                const auto got = decode(delta, &poses[frame - age]);
                exact = exact && got && *got == poses[frame];
            }
        }
        std::cout << "  " << age * 33 << " ms" << std::string(age * 33 < 100 ? 9 : age * 33 < 1000 ? 8 : age * 33 < 10000 ? 7 : 6, ' ') << all / count
                  << " B       " << held / count << " B\n";
    }
    const auto started = std::chrono::steady_clock::now();
    std::size_t made{};
    for (unsigned round = 0; round < 20; ++round)
        for (std::size_t frame = 2; frame < poses.size(); ++frame) made += encode_delta(poses[frame], poses[frame - 2]).size() > 0;
    const auto encode_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count() / static_cast<double>(made);
    std::cout << "one difference takes " << encode_us << " us to build; every one checked rebuilt its pose: " << (exact ? "yes" : "NO") << '\n';
    return exact ? 0 : 1;
}
// Skater sound (sound_codec.h): a chain of messages rebuilds every sample as the sender rounded
// it, a message that did not go leaves the chain where it was, and noise is declined.
AudioSample sound_sample(std::mt19937 &random, AudioState &state, std::uint32_t age) {
    std::uniform_real_distribution<float> any(-1.f, 1.f);
    for (std::size_t i = 0; i < 20; ++i) state.values[i] += any(random) * 0.2f;
    if (random() % 9 == 0) state.values[20 + random() % 38] = any(random) * 90.f;
    if (random() % 7 == 0) state.flags[1 + random() % 42] ^= 1;
    if (random() % 11 == 0) state.selectors[random() % 26] = static_cast<std::uint32_t>(random());
    return {age, state, random() % 3 == 0};
}
void sound_checks() {
    using namespace sound_codec;
    std::mt19937 random(41);
    Sender sender;
    std::unordered_map<std::uint16_t, In> streams;
    std::array<AudioState, 3> states{};
    states[1].values[7] = 0.0001f; // not nothing: must not arrive as nothing
    std::size_t bytes{}, messages{}, heard_samples{};
    bool exact = true;
    for (unsigned tick = 1; tick <= 600; ++tick) {
        const std::uint64_t now = std::uint64_t{tick} * 33333;
        sender.begin(7, 42);
        std::array<std::vector<AudioSample>, 3> made;
        for (std::size_t who = 0; who < 3; ++who) {
            if (who == 2 && (tick / 50) % 2) continue; // out of earshot for a while
            made[who] = {sound_sample(random, states[who], 24000), sound_sample(random, states[who], 9000)};
            sender.add(500 + who, 9, tick, now, made[who], now);
        }
        const bool went = tick % 13 != 0; // some messages are refused by the connection
        std::vector<std::uint8_t> message(sender.message().begin(), sender.message().end());
        sender.sent(went);
        if (!went) continue;
        bytes += message.size();
        ++messages;
        const auto heard = read(streams, message, 7, 42);
        if (!heard) {
            exact = false;
            break;
        }
        for (const auto &one : *heard) {
            const auto who = static_cast<std::size_t>(one.source - 500);
            exact = exact && who < 3 && one.epoch == 9 && one.sequence == tick && one.time_us == now && one.samples.size() == made[who].size() &&
                    valid_audio_batch(one.samples);
            for (std::size_t i = 0; exact && i < one.samples.size(); ++i) {
                exact = one.samples[i].state == restore(quantize(made[who][i].state)) && one.samples[i].event == made[who][i].event &&
                        one.samples[i].age_us == (made[who][i].age_us >> age_shift << age_shift);
                ++heard_samples;
            }
        }
    }
    check(exact && heard_samples > 2500, "Sound was not rebuilt as it was sent");
    check(restore(quantize(states[1])).values[7] != 0.f, "A small sound value arrived as nothing");
    // Rounding again changes nothing: what a server relays is what it was sent.
    const auto once = quantize(states[0]);
    check(quantize(restore(once)) == once, "Sound did not survive being rounded twice");
    // Another world's message holds nothing; a stream never started is passed over, not guessed.
    sender.begin(7, 42);
    const std::vector<AudioSample> one{sound_sample(random, states[0], 5000)};
    sender.add(500, 9, 700, 1, one, 600 * 33333 + 1);
    std::vector<std::uint8_t> message(sender.message().begin(), sender.message().end());
    sender.sent(true);
    std::unordered_map<std::uint16_t, In> fresh;
    const auto other_world = read(streams, message, 8, 42);
    const auto unknown = read(fresh, message, 7, 42);
    check(other_world && other_world->empty() && unknown && unknown->empty() && fresh.empty(), "Sound for another world or an unknown stream was played");
    for (unsigned round = 0; round < 4000; ++round) {
        auto damaged = message;
        if (round % 2) damaged.resize(random() % damaged.size());
        else damaged[random() % damaged.size()] ^= static_cast<std::uint8_t>(1U << (random() % 8));
        std::unordered_map<std::uint16_t, In> copy = streams;
        if (const auto got = read(copy, damaged, 7, 42))
            for (const auto &entry : *got)
                for (const auto &sample : entry.samples)
                    if (!valid_audio(sample.state)) return check(false, "Damaged sound decoded to an impossible sample");
    }
    std::cout << "Sound codec: chains, refused messages, other worlds and damage handled (" << bytes / messages / 3 << " B a player a message on synthetic sound).\n";
}
// Sizes on real play: the sound in a pose-dump recording, sent as a game would send it.
int sound_bench(const char *path) {
    using namespace sound_codec;
    std::ifstream in(path, std::ios::binary);
    char magic_bytes[6]{};
    if (!in.read(magic_bytes, 6)) return 2;
    Sender sender;
    std::unordered_map<std::uint16_t, In> streams;
    std::size_t messages{}, old_bytes{}, new_bytes{}, samples{};
    bool exact = true;
    std::uint64_t first{}, last{};
    for (;;) {
        std::uint64_t time{};
        std::uint32_t length{};
        if (!in.read(reinterpret_cast<char *>(&time), 8) || !in.read(reinterpret_cast<char *>(&length), 4) || length > 1 << 20) break;
        std::vector<std::uint8_t> raw(length);
        if (!in.read(reinterpret_cast<char *>(raw.data()), length)) break;
        const auto packet = dingosdk::multiplayer::decode(raw);
        if (!packet || packet->kind != PacketKind::audio) continue;
        if (!first) first = packet->time_us;
        last = packet->time_us;
        sender.begin(1, 2);
        sender.add(packet->source, packet->epoch, packet->sequence, packet->time_us, packet->audio, packet->time_us);
        const std::vector<std::uint8_t> message(sender.message().begin(), sender.message().end());
        sender.sent(true);
        const auto heard = read(streams, message, 1, 2);
        exact = exact && heard && heard->size() == 1 && heard->front().samples.size() == packet->audio.size();
        for (std::size_t i = 0; exact && i < packet->audio.size(); ++i) exact = heard->front().samples[i].state == restore(quantize(packet->audio[i].state));
        ++messages;
        samples += packet->audio.size();
        old_bytes += length;
        new_bytes += message.size();
    }
    if (!messages) return 0;
    const double seconds = static_cast<double>(last - first) / 1e6;
    std::cout << messages << " sound messages, " << samples << " samples in " << seconds << " s\n"
              << "  old format, before the wire's own compression: " << old_bytes / messages << " B a message, " << static_cast<double>(old_bytes) / seconds / 1024 << " KB/s\n"
              << "  new format: " << new_bytes / messages << " B a message, " << static_cast<double>(new_bytes) / seconds / 1024 << " KB/s; every sample rebuilt as rounded: "
              << (exact ? "yes" : "NO") << '\n';
    return exact ? 0 : 1;
}

// The sound in a recording, laid flat for study: for each sample its message's number and size,
// its age, whether it is an event, and its values, selectors and flags.
int sound_out(const char *path, const char *to) {
    std::ifstream in(path, std::ios::binary);
    char magic[6]{};
    if (!in.read(magic, 6)) return 2;
    std::ofstream out(to, std::ios::binary);
    std::uint32_t message{};
    for (;;) {
        std::uint64_t time{};
        std::uint32_t length{};
        if (!in.read(reinterpret_cast<char *>(&time), 8) || !in.read(reinterpret_cast<char *>(&length), 4) || length > 1 << 20) break;
        std::vector<std::uint8_t> raw(length);
        if (!in.read(reinterpret_cast<char *>(raw.data()), length)) break;
        const auto packet = dingosdk::multiplayer::decode(raw);
        if (!packet || packet->kind != PacketKind::audio) continue;
        for (const auto &sample : packet->audio) {
            const std::uint32_t head[4]{message, length, sample.age_us, sample.event ? 1U : 0U};
            out.write(reinterpret_cast<const char *>(head), sizeof head);
            out.write(reinterpret_cast<const char *>(&time), 8);
            out.write(reinterpret_cast<const char *>(sample.state.values.data()), sizeof sample.state.values);
            out.write(reinterpret_cast<const char *>(sample.state.selectors.data()), sizeof sample.state.selectors);
            out.write(reinterpret_cast<const char *>(sample.state.flags.data()), sizeof sample.state.flags);
        }
        ++message;
    }
    std::cout << message << " sound messages\n";
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 3) return sound_out(argv[1], argv[2]);
    if (argc == 2) {
        const auto poses = bench(argv[1]);
        const auto sound = sound_bench(argv[1]);
        return poses ? poses : sound;
    }
    exact_checks();
    finger_checks();
    hostile_checks();
    link_checks();
    sound_checks();
    if (failures) return 1;
    std::cout << "Pose codec tests passed.\n";
    return 0;
}
