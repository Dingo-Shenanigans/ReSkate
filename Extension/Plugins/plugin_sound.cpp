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
// (8 and 48 until 2.1.4: a vehicle's engine alone is five or six loops, which left a plugin
// nothing for anyone else's.)
constexpr std::size_t most_each = 128, most_all = 256;
// How far ahead of the game's playing a sound is kept (the game plays what it is given as it
// comes: with less ahead than a frame takes, it runs dry and the sound breaks up); how long a
// finished sound keeps its place for its last blocks to play.
// The lead follows the frames: two and a half of the longest frame of late, so a game running
// fast hears a sound start (and change) sooner than it did with the fixed 60 ms, and one running
// slow, or stalling now and then, still does not run dry.
constexpr double least_lead = 0.035, most_lead = 0.12, frames_of_lead = 2.5;
constexpr float longest_frame = 0.1f, tail_seconds = 0.4f;

struct Playing {
    Voice voice;
    double cursor{};
    // Seconds of the sound the game holds beyond what it has played, as far as can be told: what
    // was made, less the time gone by. Each frame makes it up to the lead again, however long the
    // frame took. (It was made by the frame's length, capped: after one long frame the sound was
    // behind for good, each frame's samples played the moment they came with a gap after them,
    // which is heard as a crackle until the sound is started again.)
    double ahead{};
    float gain{};      // the gain its last samples were made at: the next ramp from it
    float tail{};
    bool ended{};
    bool started{};    // its first samples have been made
    audio::MotorSynth synth{static_cast<int>(native_voice_rate)};
};
struct Player {
    multiplayer::NativeVoicePlayback playback{source_tag};
    std::map<std::uint64_t, Playing> voices;
    std::uint64_t next_id = 1;
    bool started{}, in_level{};
    Clock::time_point last{};
    double frame = 1.0 / 60.0; // the longest frame of late, forgotten slowly (half in about 3 s at 60 a second)
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
        const double passed = std::clamp(std::chrono::duration<double>(now - p.last).count(), 0.0, 5.0);
        const float elapsed = std::clamp(static_cast<float>(passed), 0.0f, longest_frame);
        p.last = now;
        // One stall is not how the game runs: only up to 50 ms of a frame counts toward the lead.
        p.frame = std::max(std::min(passed, 0.05), p.frame * 0.996);
        const double lead_seconds = std::clamp(p.frame * frames_of_lead, least_lead, most_lead);
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
        if (std::any_of(p.voices.begin(), p.voices.end(), [](const auto& v) { return !v.second.voice.position && !v.second.voice.listener; })) {
            const auto frame = multiplayer::capture_local(base, client, false);
            if (frame.ready) skater = std::array<float, 3>{frame.pose.root.position[0], frame.pose.root.position[1] + 0.3f, frame.pose.root.position[2]};
        }
        std::vector<std::int16_t> block;
        for (auto it = p.voices.begin(); it != p.voices.end();) {
            auto& [id, playing] = *it;
            auto& v = playing.voice;
            const auto at = v.listener ? ear : v.position ? *v.position : skater ? *skater : ear;
            const float d = distance(ear, at), range = std::max(v.range, 1.0f);
            const float fall = std::clamp(1.0f - d / range, 0.0f, 1.0f);
            const float gain = std::clamp(v.volume, 0.0f, 1.0f) * fall * fall;
            // The gain is in the samples (ramped from the last frame's to this one's, so a change
            // of volume or distance is not a step every frame): the game plays them as they are.
            // A sound that is silent and was silent holds no game sound at all.
            const bool silent = gain <= 0.0f && playing.gain <= 0.0f;
            p.playback.position(id, 1, at, playing.ended || !silent ? 1.0f : 0.0f);
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
            // A sound's first frame: none of it has played, so no time has gone by for it, and a
            // recording starts at its own volume. (It was treated as run dry and faded in: the
            // frame's length was skipped from its start and the rest came up from silence over the
            // first block, which took the crack off every gunshot: its first 17 ms at 60 frames a
            // second. The engine sound still comes up from silence: it has no start to keep.)
            if (!playing.started) {
                playing.started = true;
                if (v.wave) playing.gain = gain;
            } else {
                playing.ahead -= passed;
            }
            const double step_rate = v.wave ? std::clamp(static_cast<double>(v.pitch), 0.25, 4.0) * native_voice_rate : 0.0;
            if (silent) {
                // Nothing is held for it: it goes on in time, unheard, and starts with a lead again.
                playing.ahead = 0.0;
                if (v.wave && !v.wave->samples.empty()) {
                    playing.cursor += passed * step_rate;
                    if (playing.cursor >= static_cast<double>(v.wave->samples.size() - 1)) {
                        if (v.loop) playing.cursor = std::fmod(playing.cursor, static_cast<double>(v.wave->samples.size()));
                        else playing.ended = true;
                    }
                }
                ++it;
                continue;
            }
            // Run dry (a frame longer than the lead): the game played nothing for that long, and
            // the sound goes on from where it would be now, a lead ahead again. That time is gone,
            // not owed: made up, it would sit in the game's queue as a delay.
            if (playing.ahead < 0.0) {
                if (v.wave) playing.cursor += -playing.ahead * step_rate;
                playing.ahead = 0.0;
            }
            const double need = lead_seconds - playing.ahead;
            const auto count = need > 0.0 ? static_cast<std::size_t>(need * native_voice_rate) : std::size_t{};
            if (!count) {
                ++it;
                continue;
            }
            playing.ahead += static_cast<double>(count) / native_voice_rate;
            block.resize(count);
            if (!v.wave) {
                playing.synth.render(block.data(), static_cast<int>(count), v.throttle, v.speed, 1.0f, v.pitch);
            } else {
                // The wave at its pitch: read faster or slower, between its samples.
                const auto& samples = v.wave->samples;
                const double step = std::clamp(static_cast<double>(v.pitch), 0.25, 4.0);
                std::size_t made = 0;
                for (; made < count; ++made) {
                    if (samples.empty()) break;
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
            if (!block.empty()) {
                const float from = playing.gain, to = gain, steps = static_cast<float>(block.size());
                for (std::size_t n = 0; n < block.size(); ++n) {
                    const float g = from + (to - from) * (static_cast<float>(n + 1) / steps);
                    block[n] = static_cast<std::int16_t>(std::clamp(static_cast<float>(block[n]) * g, -32768.0f, 32767.0f));
                }
                p.playback.submit(id, 1, {reinterpret_cast<const std::uint8_t*>(block.data()), block.size() * sizeof(std::int16_t)});
            }
            playing.gain = gain;
            ++it;
        }
        p.playback.process(base);
    } catch (...) {
        p.voices.clear();
    }
}
}
