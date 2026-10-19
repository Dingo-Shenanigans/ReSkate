#include "drone_sound.h"
#include "native_prop.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Voice/native_voice.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/drone_audio.h"
#include "Extension/Skater/drone_beacon.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
using Clock = std::chrono::steady_clock;
// How far away a drone is heard: a 5-inch at full power carries well past a skate spot.
constexpr float hearing_distance = 120.0f;
// Other players' drones a little under the pilot's own motor at the same volume setting.
constexpr float remote_level = 0.3f;
constexpr float own_level = 1.0f;
// Full volume only this close; from there it falls off with distance (about -6 dB each time the
// distance doubles), faded to nothing over the last quarter of the hearing distance.
constexpr float full_volume_distance = 1.5f;
float drone_gain(const std::array<float, 3> &ear, const std::array<float, 3> &at) {
    float squared = 0.0f;
    for (std::size_t i = 0; i < 3; ++i) squared += (ear[i] - at[i]) * (ear[i] - at[i]);
    const float distance = std::sqrt(squared);
    if (!std::isfinite(distance) || distance >= hearing_distance) return 0.0f;
    float gain = full_volume_distance / std::max(distance, full_volume_distance);
    const float fade_start = hearing_distance * 0.75f;
    if (distance > fade_start) gain *= 1.0f - (distance - fade_start) / (hearing_distance - fade_start);
    return gain;
}
constexpr std::uint64_t drone_source_tag = 0x52534b4400000000ULL; // "RSKD"
// The dronemodeltest prop's sound (players are keyed by their nonzero ids).
constexpr std::uint64_t test_drone = 0;
// The pilot's own drone, played here too so it goes through the game's sound like everything else.
constexpr std::uint64_t own_drone = ~0ULL;
// Sound made ahead of time for a drone just heard, so the first blocks do not run dry.
constexpr double preroll_seconds = 0.06;
// At most this much sound made in one frame (a long hitch is not caught up).
constexpr float longest_frame = 0.1f;

struct Voice {
    drone::audio::MotorSynth synth{static_cast<int>(native_voice_rate)};
    double owed{}; // samples due and not yet made (the fraction carries to the next frame)
};
struct Sounds {
    NativeVoicePlayback playback{drone_source_tag};
    std::map<std::uint64_t, Voice> voices;
    bool started{}, announced{};
    Clock::time_point last{}, test_started{};
    std::mutex status_mutex;
    std::string status = "No other player's drone is in the air.";
};
// Never destroyed: its sounds belong to the game, which is gone by then.
Sounds &sounds() {
    static auto *s = new Sounds;
    return *s;
}
void set_status(Sounds &s, std::string text) {
    std::lock_guard lock(s.status_mutex);
    s.status = std::move(text);
}
struct Source {
    std::uint64_t id;
    std::array<float, 3> position;
    float throttle, speed;
    float pitch = 1.0f;
    float gain = -1.0f; // set: this loud wherever it is (the pilot's own); else by its distance
};
} // namespace

void tick_drone_sounds(std::uintptr_t base, std::uintptr_t client, bool ready, bool show) noexcept {
    auto &s = sounds();
    try {
        const auto now = Clock::now();
        if (!s.started) {
            s.playback.reset(1);
            s.started = true;
            s.last = now;
        }
        const float elapsed = std::clamp(std::chrono::duration<float>(now - s.last).count(), 0.0f, longest_frame);
        s.last = now;
        const float volume = drone::knobs().value[drone::kVolume].load(std::memory_order_relaxed);
        std::vector<Source> sources;
        std::array<float, 16> camera{};
        if (ready && volume > 0.001f && read_local_camera_transform(base, client, camera)) {
            // The pilot's own drone: the view is its camera, so it sits just under and ahead of it.
            if (drone::flying() && drone::telemetry().armed.load(std::memory_order_relaxed)) {
                const auto &t = drone::telemetry();
                const std::array<float, 3> at{camera[12] - camera[8] * 0.2f, camera[13] - camera[9] * 0.2f - 0.1f,
                                              camera[14] - camera[10] * 0.2f};
                sources.push_back({own_drone, at, t.throttle.load(std::memory_order_relaxed), t.speed.load(std::memory_order_relaxed),
                                   drone::knobs().value[drone::kMotorPitch].load(std::memory_order_relaxed), own_level});
            }
            if (show)
                for (const auto &drone : drone::beacon::snapshot())
                    sources.push_back({drone.id, drone.position, drone.throttle, drone.speed});
            if (const auto at = prop_test_position()) {
                // Going round its circle with the throttle rising and falling, as a drone would.
                if (!s.voices.contains(test_drone)) s.test_started = now;
                const float t = std::chrono::duration<float>(now - s.test_started).count();
                sources.push_back({test_drone, *at, 0.35f + 0.25f * std::sin(t * 1.3f), 3.1f});
            }
        }
        const std::array<float, 3> ear{camera[12], camera[13], camera[14]};
        std::erase_if(s.voices, [&](const auto &item) {
            return std::none_of(sources.begin(), sources.end(), [&](const Source &x) { return x.id == item.first; });
        });
        std::vector<std::int16_t> samples;
        for (const auto &source : sources) {
            const float gain = volume * (source.gain >= 0.0f ? source.gain : remote_level * drone_gain(ear, source.position));
            if (!(gain > 0.0f)) {
                s.voices.erase(source.id);
                continue; // its sound is released once it has had no position for a moment
            }
            auto [found, added] = s.voices.try_emplace(source.id);
            auto &voice = found->second;
            if (added) voice.owed = preroll_seconds * native_voice_rate;
            voice.owed += static_cast<double>(elapsed) * native_voice_rate;
            const auto count = static_cast<int>(std::min(std::floor(voice.owed), static_cast<double>(native_voice_rate) * longest_frame));
            voice.owed = std::max(0.0, voice.owed - count);
            s.playback.position(source.id, 1, source.position, gain);
            if (count <= 0) continue;
            samples.resize(static_cast<std::size_t>(count));
            voice.synth.render(samples.data(), count, source.throttle, source.speed, 1.0f, source.pitch);
            s.playback.submit(source.id, 1, {reinterpret_cast<const std::uint8_t *>(samples.data()), samples.size() * 2});
        }
        s.playback.process(base);
        // The game's sound has the pilot's motor: the drone's own Windows output stays quiet.
        if (s.playback.available() && s.voices.contains(own_drone))
            drone::audio::game_sound_ms().store(drone::now_ms(), std::memory_order_relaxed);
        if (!s.voices.empty() && !s.announced && s.playback.available()) {
            s.announced = true;
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Other players' drones are heard through the game's positioned voice sound.");
        }
        const auto others = s.voices.size() - (s.voices.contains(own_drone) ? 1 : 0);
        if (others == 0)
            set_status(s, "No other player's drone is in the air within earshot.");
        else if (!s.playback.available())
            set_status(s, std::format("{} drone(s) within earshot, but the game's positioned voice sound is not ready, so they are silent.",
                                      others));
        else
            set_status(s, std::format("{} drone(s) heard.", others));
    } catch (const std::exception &e) {
        s.voices.clear();
        set_status(s, std::string("Drone sounds stopped: ") + e.what());
    }
}

std::string drone_sounds_status() {
    auto &s = sounds();
    std::lock_guard lock(s.status_mutex);
    return s.status;
}
} // namespace dingosdk::multiplayer
