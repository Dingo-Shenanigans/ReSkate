#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Multiplayer/Net/delta_codec.h"
#include "Extension/Multiplayer/Session/room.h"
#include "Engine/Game/Multiplayer/voice_settings.h"
#include "Engine/Game/Input/voice_input.h"
#include "Extension/Multiplayer/Voice/native_voice_pcm.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dingosdk;
using namespace dingosdk::multiplayer;
namespace {
void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
Packet voice_packet(std::size_t size) {
    Packet packet;
    packet.kind = PacketKind::voice;
    packet.source = 11; packet.epoch = 22; packet.session = 33; packet.map = 44;
    packet.world = 3; packet.sequence = 8; packet.time_us = 10000;
    packet.voice.bytes.resize(size);
    for (std::size_t i = 0; i < size; ++i) packet.voice.bytes[i] = static_cast<std::uint8_t>(i * 17);
    return packet;
}
void rejected(const Packet &packet) {
    try { (void)encode(packet); } catch (const std::invalid_argument &) { return; }
    throw std::runtime_error("Invalid voice was encoded");
}
}
int main() try {
    {
        NativeVoicePcm pcm;
        std::array<std::int16_t, 500> source;
        for (std::size_t n = 0; n < source.size(); ++n) source[n] = static_cast<std::int16_t>(static_cast<int>(n) * 50 - 12000);
        const auto bytes = std::span(reinterpret_cast<const std::uint8_t *>(source.data()), sizeof(source));
        check(!pcm.append(bytes.first(3)), "Odd PCM byte count accepted");
        check(pcm.append(bytes.first(240)) && pcm.append(bytes.subspan(240)), "Split PCM packet rejected");
        check(pcm.samples() == source.size(), "Native framing added silence between packets");
        NativeVoiceBlock block;
        check(pcm.pop(block, 1.f) && std::equal(block.begin(), block.end(), source.begin()), "Native sample ordering changed");
        check(!pcm.pop(block, 1.f), "Partial native block submitted before completion");
        pcm.flush();
        check(pcm.pop(block, 1.f) && std::equal(source.begin() + 320, source.end(), block.begin()), "Native tail samples lost");
        check(std::all_of(block.begin() + 180, block.end(), [](auto v) { return v == 0; }), "Native tail not zero padded");
        std::vector<std::uint8_t> full(native_voice_rate);
        check(pcm.append(full) && !pcm.append(bytes.first(2)), "Native voice queue is unbounded");
        check(pcm.pop(block, 1.f), "Full native queue could not drain");
        pcm = {};
        const std::array<std::int16_t, 4> peaks{20000,-20000,32767,-32768};
        check(pcm.append({reinterpret_cast<const std::uint8_t *>(peaks.data()), sizeof(peaks)}), "Gain input rejected");
        pcm.flush();
        check(pcm.pop(block, 10.f) && block[0] == 32767 && block[1] == -32768, "Native gain wrapped PCM peaks");
        check(pcm.samples() == 0, "Native PCM remained after drain");
    }
    for (const auto size : {std::size_t{1}, std::size_t{1024}, max_voice_bytes}) {
        auto packet = voice_packet(size);
        packet.voice.gain = 4.5f;
        packet.voice.policy_revision = 9;
        const auto bytes = encode(packet);
        const auto decoded = decode(bytes);
        check(decoded && decoded->kind == PacketKind::voice && decoded->voice.bytes == packet.voice.bytes &&
            decoded->voice.distance == packet.voice.distance && decoded->world == packet.world &&
            decoded->source == packet.source && decoded->epoch == packet.epoch &&
            decoded->voice.gain == packet.voice.gain && decoded->voice.policy_revision == 9, "Voice packet round trip failed");
        for (std::size_t length = 0; length < bytes.size(); ++length)
            check(!decode(std::span(bytes).first(length)), "Truncated voice packet accepted");
        auto extra = bytes; extra.push_back(0);
        check(!decode(extra), "Trailing voice data accepted");
    }
    auto packet = voice_packet(128);
    DeltaSender sender;
    DeltaReceiver receiver;
    auto first = sender.prepare(packet);
    check(!first.establishes_baseline(), "Voice depends on a reliable pose baseline");
    sender.sent(packet, std::move(first));
    ++packet.sequence;
    const auto next = sender.prepare(packet);
    bool missing{};
    const auto received = receiver.receive(next.bytes, missing, packet.world);
    check(received && !missing && received->voice.bytes == packet.voice.bytes, "Lost voice chunk prevents subsequent playback");
    const Member member{packet.source, packet.epoch, {}};
    check(routed_source(packet, member, packet.source, true, 55) &&
          routed_source(packet, member, 55, false, 55) &&
          !routed_source(packet, member, 66, false, 55), "Voice sender routing permits impersonation");
    ReceiveBudget link_budget;
    for (int i = 0; i < 2 * 120 + 50 + 20; ++i)
        check(link_budget.accept(1000000, 32), "120 TPS plus voice trips the link budget");
    packet.voice.distance = 0;
    check(decode(encode(packet))->voice.distance == 0.f, "Session-wide voice was rejected");
    packet.voice.distance = 300.f;
    check(decode(encode(packet))->voice.distance == 300.f, "A long hearing distance was rejected");
    for (const float radius : {-1.f, 1.f, max_hearing_distance + 1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        packet.voice.distance = radius; rejected(packet);
    }
    rejected(voice_packet(0)); rejected(voice_packet(max_voice_bytes + 1));
    packet = voice_packet(5); packet.source = 0; rejected(packet);
    packet = voice_packet(5); packet.map = 0; rejected(packet);
    packet = voice_packet(5); packet.world = 0; rejected(packet);
    auto invalid_radius = encode(voice_packet(5));
    invalid_radius[64] = 0; invalid_radius[65] = 0; invalid_radius[66] = 0x80; invalid_radius[67] = 0x7f;
    check(!decode(invalid_radius), "Non-finite received radius accepted");
    auto invalid_size = encode(voice_packet(5));
    invalid_size[76] = 255; invalid_size[77] = 255;
    check(!decode(invalid_size), "Invalid received voice length accepted");
    for (const float gain : {-1.f, 10.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        packet = voice_packet(5); packet.voice.gain = gain; rejected(packet);
    }
    packet = voice_packet(5); packet.voice.policy_revision = 0; rejected(packet);
    auto invalid_gain = encode(voice_packet(5));
    invalid_gain[68] = 0; invalid_gain[69] = 0; invalid_gain[70] = 0x80; invalid_gain[71] = 0x7f;
    check(!decode(invalid_gain), "Non-finite received microphone gain accepted");
    packet = voice_packet(5);
    VoicePolicy policy;
    check(policy.accepts(packet.voice), "Initial policy rejected current voice");
    policy.allowed = false; ++policy.revision;
    check(!policy.accepts(packet.voice), "Host disable accepted voice");
    packet.voice.policy_revision = policy.revision;
    check(!policy.accepts(packet.voice), "Matching revision bypassed host disable");
    policy.allowed = true; ++policy.revision;
    check(!policy.accepts(packet.voice), "Re-enabling voice replayed stale audio");
    packet.voice.policy_revision = policy.revision;
    check(policy.accepts(packet.voice), "Re-enabled voice rejected fresh audio");
    packet.kind = PacketKind::roster;
    packet.members = {{76561198000000001ULL, 22, "Host"}, {76561198000000002ULL, 66, "Guest"}};
    for (const bool allowed : {false, true}) {
        packet.voice_policy = {allowed, 34};
        const auto encoded = encode(packet);
        const auto decoded = decode(encoded);
        check(decoded && decoded->voice_policy == packet.voice_policy, "Host voice policy lost in roster");
        check(decoded && decoded->voice_range == packet.voice_range, "Host voice range lost in roster");
        // The roster ends: voice allowed (1), policy revision (4), voice range (2), guest noclip / No Bail (1),
        // then what a dedicated server adds, here none: polls (1), custom votes (1), announcement (4).
        const auto end = encoded.size() - 6;
        for (std::size_t i = end - 8; i < encoded.size(); ++i)
            check(!decode(std::span(encoded).first(i)), "Truncated voice policy accepted");
        auto invalid = encoded; invalid[end - 8] = 2;
        check(!decode(invalid), "Invalid host voice toggle accepted");
        invalid = encoded; invalid[end - 1] = 128;
        check(!decode(invalid), "Invalid guest noclip / No Bail byte accepted");
        invalid = encoded;
        for (std::size_t i = end - 7; i < end - 3; ++i) invalid[i] = 0;
        check(!decode(invalid), "Zero host voice revision accepted");
        invalid = encoded; invalid[end - 3] = 10; invalid[end - 2] = 0;
        check(!decode(invalid), "Voice range below the minimum accepted");
    }
    const std::array<float, 3> origin{};
    auto stereo = voice_stereo(origin, {1, 0, 0}, {-10, 0, 0});
    check(stereo[0] == 1.f && stereo[1] == 0.f, "Player on camera left is not in left ear");
    stereo = voice_stereo(origin, {1, 0, 0}, {10, 0, 0});
    check(stereo[0] == 0.f && stereo[1] == 1.f, "Player on camera right is not in right ear");
    stereo = voice_stereo(origin, {-1, 0, 0}, {10, 0, 0});
    check(stereo[0] == 1.f && stereo[1] == 0.f, "Voice did not follow camera rotation");
    stereo = voice_stereo(origin, {}, {10, 0, 0});
    check(stereo[0] == stereo[1], "Missing camera should center voice instead of using body heading");
    stereo = voice_stereo(origin, {1, 0, 0}, {0, 0, -10});
    check(stereo[0] == stereo[1], "Player in front is not centered");
    stereo = voice_stereo(origin, {1, 0, 0}, {std::numeric_limits<float>::quiet_NaN(), 0, 0});
    check(std::isfinite(stereo[0]) && stereo[0] == stereo[1], "Invalid spatial audio matrix");
    check(voice_gain(origin, origin, 35) == 1.f, "Nearby voice is attenuated");
    check(voice_gain(origin, {35, 0, 0}, 35) == 0.f, "Voice outside the radius is audible");
    check(voice_gain(origin, {200, 0, 0}, 0) == 1.f, "Session-wide voice is distance limited");
    check(voice_gain(origin, {17.5f, 0, 0}, 35) > .25f, "Voice fades out too early");
    const auto edge = voice_gain(origin, {34, 0, 0}, 35);
    check(edge > 0.f && edge < .05f, "Voice does not trail off near the edge");
    float previous = 1.f;
    for (int i = 0; i <= 100; ++i) {
        const auto gain = voice_gain(origin, {static_cast<float>(i), 0, 0}, 35);
        check(gain >= 0 && gain <= previous, "Voice attenuation is not monotonic");
        previous = gain;
    }
    check(voice_gain(origin, {std::numeric_limits<float>::quiet_NaN(), 0, 0}, 35) == 0.f,
          "Non-finite player position is audible");
    VoiceBudget budget;
    for (int i = 0; i < 80; ++i) check(budget.accept(1000000, 32), "Normal voice cadence rejected");
    check(!budget.accept(1000000, 32) && budget.accept(2000000, 32), "Voice rate limit failed");
    budget = {};
    for (int i = 0; i < 2; ++i) check(budget.accept(1000000, max_voice_bytes), "Voice byte budget rejected early");
    check(!budget.accept(1000000, 1), "Voice byte budget exceeded");
    VoiceSettings settings;
    check(settings.valid() && !settings.enabled && !settings.open_mic && settings.push_to_talk == 'V', "Unsafe microphone default");
    settings.volume = settings.microphone = max_voice_volume;
    settings.controller_combo = 0x300;
    check(settings.valid(), "Supported gain or controller combo rejected");
    VoicePushToTalk ptt;
    ControllerInput pad{true, 0, 1};
    check(!ptt.update(settings, true, pad, false), "Held key transmitted before release");
    check(!ptt.update(settings, false, pad, false), "Released key transmitted");
    check(ptt.update(settings, true, pad, false) && ptt.update(settings, true, pad, false), "Keyboard PTT does not stay active while held");
    check(!ptt.update(settings, true, pad, true) && !ptt.update(settings, true, pad, false), "Menu/focus gate failed to disarm keyboard");
    check(!ptt.update(settings, false, pad, false), "Idle controller transmitted");
    pad.buttons = 0x100;
    check(!ptt.update(settings, false, pad, false), "Partial controller combo transmitted");
    pad.buttons = 0x300;
    check(ptt.update(settings, false, pad, false) && ptt.update(settings, false, pad, false), "Controller PTT does not stay active while held");
    pad.device = 2;
    check(!ptt.update(settings, false, pad, false), "Controller reconnect transmitted held combo");
    pad.buttons = 0;
    check(!ptt.update(settings, false, pad, false), "Released controller transmitted");
    pad.buttons = 0x300;
    check(ptt.update(settings, false, pad, false), "Reconnected controller could not re-arm");
    settings.controller_combo = 0x100;
    check(!ptt.update(settings, false, pad, false), "Controller rebind transmitted immediately");
    settings.push_to_talk = 'B';
    check(!ptt.update(settings, true, {}, false), "Keyboard rebind transmitted immediately");
    settings.push_to_talk = 0;
    check(settings.valid(), "Cleared keyboard binding was rejected");
    settings.controller_combo = 0x40000;
    check(!settings.valid(), "Invalid controller bits accepted");
    settings.controller_combo = 0;
    settings.microphone = 11;
    check(!settings.valid(), "Unsupported microphone volume accepted");
    settings.microphone = 1;
    settings.volume = std::numeric_limits<float>::quiet_NaN();
    check(!settings.valid(), "Non-finite voice volume accepted");
    std::cout << "Voice codec, host policy, gain limits, PTT release gates and proximity checks passed.\n";
} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
