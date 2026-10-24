#include "plugin_sound.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Voice/native_voice.h"
#include "Extension/Skater/client_source_spawn.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <vector>

namespace dingosdk::plugin_sound {
namespace {
using Clock = std::chrono::steady_clock;
using multiplayer::native_voice_rate;
// Plugins' sounds in the game's sample provider ("RSKP"), apart from voices.
constexpr std::uint64_t source_tag = 0x52534b5000000000ULL;
constexpr std::size_t most_each = 8, most_all = 48;
// Made ahead for a sound just started, so its first blocks do not run dry; at most this much in one
// frame; how long a finished sound keeps its place for its last blocks to play.
constexpr double preroll_seconds = 0.06;
constexpr float longest_frame = 0.1f, tail_seconds = 0.4f;

struct Playing {
    Voice voice;
    double cursor{}, owed{preroll_seconds * native_voice_rate};
    float tail{};
    bool ended{};
    audio::MotorSynth synth{static_cast<int>(native_voice_rate)};
};
struct Player {
    multiplayer::NativeVoicePlayback playback{source_tag};
    std::map<std::uint64_t, Playing> voices;
    std::uint64_t next_id = 1;
    bool started{}, in_level{};
    Clock::time_point last{};
};
// Never destroyed: its sounds belong to the game, which is gone by then.
Player& player() {
    static auto* value = new Player;
    return *value;
}
float distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
}

std::uint64_t start(Voice voice) {
    auto& p = player();
    if (!p.in_level) return 0;
    std::size_t mine = 0;
    for (const auto& [id, playing] : p.voices) mine += playing.voice.owner == voice.owner && !playing.ended;
    if (mine >= most_each || p.voices.size() >= most_all) return 0;
    const auto id = p.next_id++;
    Playing playing;
    playing.voice = std::move(voice);
    p.voices.emplace(id, std::move(playing));
    return id;
}
Voice* find(std::uint64_t id, std::string_view owner) {
    auto& p = player();
    const auto found = p.voices.find(id);
    return found != p.voices.end() && found->second.voice.owner == owner && !found->second.ended ? &found->second.voice : nullptr;
}
void stop(std::uint64_t id, std::string_view owner) {
    auto& p = player();
    if (const auto found = p.voices.find(id); found != p.voices.end() && found->second.voice.owner == owner) found->second.ended = true;
}
void stop_all(std::string_view owner) {
    for (auto& [id, playing] : player().voices)
        if (playing.voice.owner == owner) playing.ended = true;
}

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto& p = player();
    try {
        const auto now = Clock::now();
        if (!p.started) {
            p.playback.reset(1);
            p.started = true;
            p.last = now;
        }
        const float elapsed = std::clamp(std::chrono::duration<float>(now - p.last).count(), 0.0f, longest_frame);
        p.last = now;
        p.in_level = ready;
        if (p.voices.empty()) return;
        std::array<float, 16> camera{};
        if (!ready || !read_local_camera_transform(base, client, camera)) {
            for (const auto& [id, playing] : p.voices) p.playback.remove(id, 1);
            p.voices.clear(); // out of a level: they end
            p.playback.process(base);
            return;
        }
        const std::array<float, 3> ear{camera[12], camera[13], camera[14]};
        std::optional<std::array<float, 3>> skater;
        if (std::any_of(p.voices.begin(), p.voices.end(), [](const auto& v) { return !v.second.voice.position; })) {
            const auto frame = multiplayer::capture_local(base, client, false);
            if (frame.ready) skater = std::array<float, 3>{frame.pose.root.position[0], frame.pose.root.position[1] + 0.3f, frame.pose.root.position[2]};
        }
        std::vector<std::int16_t> block;
        for (auto it = p.voices.begin(); it != p.voices.end();) {
            auto& [id, playing] = *it;
            auto& v = playing.voice;
            const auto at = v.position ? *v.position : skater ? *skater : ear;
            const float d = distance(ear, at), range = std::max(v.range, 1.0f);
            const float fall = std::clamp(1.0f - d / range, 0.0f, 1.0f);
            const float gain = std::clamp(v.volume, 0.0f, 1.0f) * fall * fall;
            p.playback.position(id, 1, at, playing.ended ? 0.0f : gain);
            if (playing.ended) {
                playing.tail += elapsed;
                if (playing.tail >= tail_seconds) {
                    p.playback.remove(id, 1);
                    it = p.voices.erase(it);
                    continue;
                }
                ++it;
                continue;
            }
            playing.owed += static_cast<double>(elapsed) * native_voice_rate;
            const auto count = static_cast<std::size_t>(std::min(std::floor(playing.owed), static_cast<double>(native_voice_rate) * longest_frame));
            playing.owed -= static_cast<double>(count);
            if (!count) {
                ++it;
                continue;
            }
            block.resize(count);
            if (!v.wave) {
                playing.synth.render(block.data(), static_cast<int>(count), v.throttle, v.speed, 1.0f, v.pitch);
            } else {
                // The wave at its pitch: read faster or slower, between its samples.
                const auto& samples = v.wave->samples;
                const double step = std::clamp(static_cast<double>(v.pitch), 0.25, 4.0);
                std::size_t made = 0;
                for (; made < count; ++made) {
                    if (playing.cursor >= static_cast<double>(samples.size() - 1)) {
                        if (!v.loop) break;
                        playing.cursor = std::fmod(playing.cursor, static_cast<double>(samples.size()));
                    }
                    const auto i = static_cast<std::size_t>(playing.cursor);
                    const auto f = static_cast<float>(playing.cursor - static_cast<double>(i));
                    const float a = samples[i], b = samples[std::min(i + 1, samples.size() - 1)];
                    block[made] = static_cast<std::int16_t>(a + (b - a) * f);
                    playing.cursor += step;
                }
                block.resize(made);
                if (made < count) playing.ended = true; // played out; its last block still plays
            }
            if (!block.empty()) p.playback.submit(id, 1, {reinterpret_cast<const std::uint8_t*>(block.data()), block.size() * sizeof(std::int16_t)});
            ++it;
        }
        p.playback.process(base);
    } catch (...) {
        p.voices.clear();
    }
}
}
