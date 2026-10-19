#include "board_break_audio.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Voice/native_voice.h"
#include <Windows.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

namespace dingosdk::board_break::audio {
namespace {
// One wave file, read whole: its format as the file gives it and its samples, and the same sound
// as the game's positioned sound takes it (16 kHz mono; empty when it could not be made).
struct Sound {
    std::filesystem::path file;
    std::vector<std::uint8_t> format; // WAVEFORMATEX, or the larger WAVEFORMATEXTENSIBLE
    std::vector<std::uint8_t> samples;
    std::vector<std::int16_t> voice;
};
// The sound at the game sound's rate, mixed down to one channel: each output sample is the mean of
// the input samples it spans (a plain low-pass, so a crack's highs do not fold back as noise).
std::vector<std::int16_t> to_voice(const Sound& sound) {
    WAVEFORMATEX format{};
    std::memcpy(&format, sound.format.data(), std::min(sound.format.size(), sizeof(format)));
    auto tag = format.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && sound.format.size() >= 26) std::memcpy(&tag, sound.format.data() + 24, 2); // its SubFormat
    const unsigned channels = format.nChannels, bits = format.wBitsPerSample, rate = format.nSamplesPerSec;
    const bool floats = tag == WAVE_FORMAT_IEEE_FLOAT;
    if (!channels || !rate || (tag != WAVE_FORMAT_PCM && !floats) || (floats && bits != 32) ||
        (!floats && bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return {};
    const std::size_t stride = channels * (bits / 8), frames = sound.samples.size() / stride;
    const auto at = [&](std::size_t frame) {
        float sum = 0.0f;
        for (unsigned c = 0; c < channels; ++c) {
            const auto* p = sound.samples.data() + frame * stride + c * (bits / 8);
            float v{};
            if (floats) std::memcpy(&v, p, 4);
            else if (bits == 8) v = (static_cast<float>(*p) - 128.0f) / 128.0f;
            else if (bits == 16) { std::int16_t s{}; std::memcpy(&s, p, 2); v = s / 32768.0f; }
            else if (bits == 24) v = static_cast<float>(static_cast<std::int32_t>((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) >> 8) / 8388608.0f;
            else { std::int32_t s{}; std::memcpy(&s, p, 4); v = static_cast<float>(s) / 2147483648.0f; }
            sum += v;
        }
        return sum / static_cast<float>(channels);
    };
    const double step = static_cast<double>(rate) / multiplayer::native_voice_rate;
    const auto count = static_cast<std::size_t>(static_cast<double>(frames) / step);
    std::vector<std::int16_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto from = static_cast<std::size_t>(static_cast<double>(i) * step);
        const auto to = std::min(frames, std::max(from + 1, static_cast<std::size_t>(static_cast<double>(i + 1) * step)));
        float sum = 0.0f;
        for (auto f = from; f < to; ++f) sum += at(f);
        const float v = std::isfinite(sum) ? sum / static_cast<float>(to - from) : 0.0f;
        out[i] = static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
    }
    return out;
}
// A RIFF WAVE file's "fmt " and "data" chunks; false for anything else.
bool read_wave(const std::filesystem::path& file, Sound& out) {
    std::ifstream in(file, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) return false;
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        std::uint32_t size{};
        std::memcpy(&size, bytes.data() + at + 4, 4);
        const auto body = at + 8;
        if (body + size > bytes.size()) size = static_cast<std::uint32_t>(bytes.size() - body);
        if (!std::memcmp(bytes.data() + at, "fmt ", 4) && size >= 16) out.format.assign(bytes.begin() + body, bytes.begin() + body + size);
        else if (!std::memcmp(bytes.data() + at, "data", 4)) out.samples.assign(bytes.begin() + body, bytes.begin() + body + size);
        at = body + size + (size & 1);
    }
    if (out.format.size() < 16 || out.samples.empty()) return false;
    std::uint16_t tag{};
    std::memcpy(&tag, out.format.data(), 2);
    if (tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_IEEE_FLOAT && tag != WAVE_FORMAT_EXTENSIBLE) return false;
    // XAudio2 reads a whole WAVEFORMATEX (cbSize included).
    if (out.format.size() < sizeof(WAVEFORMATEX)) out.format.resize(sizeof(WAVEFORMATEX), 0);
    out.file = file;
    return true;
}

struct Request {
    Kind kind{};
    float volume{};
};
// A sound the game's positioned sound is to play.
struct GameRequest {
    const Sound* sound{};
    float volume{};
};
long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Engine {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> queue;
    std::deque<GameRequest> game_queue;
    // When the game's sound last could play them (tick_game_sound): till then, and a moment after
    // it stops (a level left), the speakers do.
    std::atomic<long long> game_ms{};
    std::array<std::vector<Sound>, kinds> sounds;
    std::array<bool, kinds> loaded{};
    std::thread worker;
    std::atomic<bool> started{}, failed{};
    std::mt19937 random{std::random_device{}()};
};
Engine& engine() {
    static auto* value = new Engine;
    return *value;
}

void run(Engine& e) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IXAudio2* xaudio{};
    IXAudio2MasteringVoice* master{};
    if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xaudio->CreateMasteringVoice(&master))) {
        if (xaudio) xaudio->Release();
        xaudio = nullptr;
        e.failed.store(true, std::memory_order_release);
        logging::log(logging::Level::warning, logging::Channel::skater,
            "Board Break: no sound device for its own sounds; the cracks play one at a time.");
    }
    std::vector<IXAudio2SourceVoice*> voices;
    for (;;) {
        Request request;
        const Sound* sound{};
        {
            std::unique_lock lock(e.mutex);
            e.wake.wait(lock, [&] { return !e.queue.empty(); });
            request = e.queue.front();
            e.queue.pop_front();
            auto& list = e.sounds[static_cast<std::size_t>(request.kind)];
            if (list.empty()) continue;
            sound = &list[std::uniform_int_distribution<std::size_t>(0, list.size() - 1)(e.random)];
        }
        if (!xaudio) {
            // Without XAudio2: the cracks still sound, one at a time.
            if (request.kind <= Kind::heavy) (void)PlaySoundW(sound->file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
            continue;
        }
        // Voices whose sound has played out go; at most a couple of dozen at once.
        std::erase_if(voices, [](IXAudio2SourceVoice* voice) {
            XAUDIO2_VOICE_STATE state{};
            voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued) return false;
            voice->DestroyVoice();
            return true;
        });
        if (voices.size() >= 24) {
            voices.front()->DestroyVoice();
            voices.erase(voices.begin());
        }
        IXAudio2SourceVoice* voice{};
        if (FAILED(xaudio->CreateSourceVoice(&voice, reinterpret_cast<const WAVEFORMATEX*>(sound->format.data())))) continue;
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(sound->samples.size());
        buffer.pAudioData = sound->samples.data();
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        voice->SetVolume(std::clamp(request.volume, 0.0f, 1.0f));
        if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
            voice->DestroyVoice();
            continue;
        }
        voices.push_back(voice);
    }
    (void)com;
}
}

void load(Kind kind, const std::vector<std::filesystem::path>& files) noexcept {
    auto& e = engine();
    std::vector<Sound> read;
    for (const auto& file : files) {
        try {
            Sound sound;
            if (read_wave(file, sound)) {
                sound.voice = to_voice(sound);
                read.push_back(std::move(sound));
            }
            else logging::log(logging::Level::warning, logging::Channel::skater, "Board Break: {} is not a wave file it can play.",
                              path_utf8(file.filename()));
        } catch (...) {}
    }
    std::lock_guard lock(e.mutex);
    const auto at = static_cast<std::size_t>(kind);
    if (e.loaded[at]) return;
    e.loaded[at] = true;
    e.sounds[at] = std::move(read);
}
bool has(Kind kind) noexcept {
    auto& e = engine();
    std::lock_guard lock(e.mutex);
    return !e.sounds[static_cast<std::size_t>(kind)].empty();
}
void play(Kind kind, float volume) noexcept {
    auto& e = engine();
    try {
        // In a level: the game's own sound plays it, at the skater (tick_game_sound).
        if (now_ms() - e.game_ms.load(std::memory_order_relaxed) < 400) {
            std::lock_guard lock(e.mutex);
            auto& list = e.sounds[static_cast<std::size_t>(kind)];
            if (list.empty()) return;
            const auto& sound = list[std::uniform_int_distribution<std::size_t>(0, list.size() - 1)(e.random)];
            if (!sound.voice.empty()) {
                if (e.game_queue.size() < 32) e.game_queue.push_back({&sound, volume});
                return;
            }
        }
        if (!e.started.exchange(true)) {
            e.worker = std::thread([&e] { run(e); });
            e.worker.detach();
        }
        std::lock_guard lock(e.mutex);
        if (e.queue.size() < 32) e.queue.push_back({kind, volume});
    } catch (...) { return; }
    e.wake.notify_one();
}
namespace {
using Clock = std::chrono::steady_clock;
// Board Break's sounds in the game's sample provider ("RSKB"), apart from voices and drones.
constexpr std::uint64_t source_tag = 0x52534b4200000000ULL;
// Their loudness through the game's sound, against the speakers' (1: the same samples).
constexpr float game_level = 1.0f;
// Sound made ahead for a sound just started, so its first blocks do not run dry; at most this
// much in one frame; and how long a finished sound keeps its place for its last blocks to play.
constexpr double preroll_seconds = 0.06;
constexpr float longest_frame = 0.1f;
constexpr float tail_seconds = 0.4f;
struct Stream {
    const Sound* sound{};
    float gain{};
    std::size_t cursor{};
    double owed{};
    float tail{};
};
struct Player {
    multiplayer::NativeVoicePlayback playback{source_tag};
    std::map<std::uint64_t, Stream> streams;
    std::uint64_t next_id = 1;
    bool started{};
    Clock::time_point last{};
};
// Never destroyed: its sounds belong to the game, which is gone by then.
Player& player() {
    static auto* value = new Player;
    return *value;
}
}

void tick_game_sound(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto& e = engine();
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
        {
            std::lock_guard lock(e.mutex);
            for (const auto& request : e.game_queue)
                if (ready && p.streams.size() < 24) {
                    Stream stream;
                    stream.sound = request.sound;
                    stream.gain = std::clamp(request.volume, 0.0f, 1.0f) * game_level;
                    stream.owed = preroll_seconds * multiplayer::native_voice_rate;
                    p.streams.emplace(p.next_id++, stream);
                }
            e.game_queue.clear();
        }
        std::array<float, 3> at{};
        bool placed{};
        if (ready && !p.streams.empty()) {
            const auto frame = multiplayer::capture_local(base, client, false);
            if (frame.ready) {
                at = {frame.pose.root.position[0], frame.pose.root.position[1] + 0.3f, frame.pose.root.position[2]};
                placed = true;
            }
        }
        if (!placed) p.streams.clear(); // (their sounds are let go once they have no place for a moment)
        std::vector<std::int16_t> block;
        for (auto it = p.streams.begin(); it != p.streams.end();) {
            auto& stream = it->second;
            const auto& samples = stream.sound->voice;
            p.playback.position(it->first, 1, at, stream.gain);
            if (stream.cursor >= samples.size()) {
                stream.tail += elapsed;
                if (stream.tail >= tail_seconds) { it = p.streams.erase(it); continue; }
                ++it;
                continue;
            }
            stream.owed += static_cast<double>(elapsed) * multiplayer::native_voice_rate;
            const auto count = std::min({static_cast<std::size_t>(stream.owed), samples.size() - stream.cursor,
                                         static_cast<std::size_t>(multiplayer::native_voice_rate * longest_frame)});
            stream.owed -= static_cast<double>(count);
            if (count) {
                const auto* data = reinterpret_cast<const std::uint8_t*>(samples.data() + stream.cursor);
                if (p.playback.submit(it->first, 1, {data, count * sizeof(std::int16_t)})) stream.cursor += count;
            }
            ++it;
        }
        p.playback.process(base);
        if (ready && p.playback.available()) e.game_ms.store(now_ms(), std::memory_order_relaxed);
    } catch (...) {
        p.streams.clear();
    }
}
}
