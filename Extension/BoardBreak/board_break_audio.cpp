#include "board_break_audio.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <algorithm>
#include <array>
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
// One wave file, read whole: its format as the file gives it and its samples.
struct Sound {
    std::filesystem::path file;
    std::vector<std::uint8_t> format; // WAVEFORMATEX, or the larger WAVEFORMATEXTENSIBLE
    std::vector<std::uint8_t> samples;
};
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
struct Engine {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> queue;
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
            if (read_wave(file, sound)) read.push_back(std::move(sound));
            else logging::log(logging::Level::warning, logging::Channel::skater, "Board Break: {} is not a wave file it can play.",
                              file.filename().string());
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
        if (!e.started.exchange(true)) {
            e.worker = std::thread([&e] { run(e); });
            e.worker.detach();
        }
        std::lock_guard lock(e.mutex);
        if (e.queue.size() < 32) e.queue.push_back({kind, volume});
    } catch (...) { return; }
    e.wake.notify_one();
}
}
