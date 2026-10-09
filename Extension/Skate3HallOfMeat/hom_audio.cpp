#include "hom_audio.h"
#include "hom_art.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace dingosdk::skate3_hom {
namespace {
namespace fs = std::filesystem;

struct Sound {
    std::vector<BYTE> pcm; // 16-bit mono
    WAVEFORMATEX format{};
    float seconds{};
    int set{};
};

struct Player {
    std::mutex mutex;
    bool loaded{}, ready{};
    std::chrono::steady_clock::time_point retry{}; // no output device yet: try again then
    IXAudio2 *engine{};
    IXAudio2MasteringVoice *master{};
    std::vector<Sound> sounds;
    std::vector<IXAudio2SourceVoice *> voices;
    std::mt19937 rng{20100511u};
    std::chrono::steady_clock::time_point last{};
};
Player &player() {
    static auto *p = new Player; // never destroyed: XAudio2 must not be torn down at process exit
    return *p;
}

fs::path sounds_directory() {
    const auto root = overlay::hom_directory();
    return root.empty() ? fs::path{} : root / L"sounds";
}

bool load_wav(const fs::path &path, Sound &out) {
    std::ifstream file(path, std::ios::binary);
    std::vector<BYTE> d((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    std::size_t o = 12;
    bool have_format = false;
    while (o + 8 <= d.size()) {
        std::uint32_t size{};
        std::memcpy(&size, d.data() + o + 4, 4);
        if (o + 8 + size > d.size()) break;
        if (!std::memcmp(d.data() + o, "fmt ", 4) && size >= 16) {
            std::memcpy(&out.format, d.data() + o + 8, 16);
            out.format.cbSize = 0;
            have_format = out.format.wFormatTag == WAVE_FORMAT_PCM && out.format.wBitsPerSample == 16;
        } else if (!std::memcmp(d.data() + o, "data", 4)) {
            out.pcm.assign(d.begin() + static_cast<std::ptrdiff_t>(o + 8), d.begin() + static_cast<std::ptrdiff_t>(o + 8 + size));
        }
        o += 8 + size + (size & 1);
    }
    if (!have_format || out.pcm.empty()) return false;
    out.seconds = static_cast<float>(out.pcm.size()) / static_cast<float>(out.format.nAvgBytesPerSec);
    return true;
}

bool start(Player &p) {
    const auto dir = sounds_directory();
    if (!p.loaded) {
        p.loaded = true;
        std::error_code error;
        for (const auto &entry : fs::directory_iterator(dir, error)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("HOM_Set_", 0) != 0 || entry.path().extension() != ".wav") continue;
            Sound sound;
            sound.set = name.size() > 8 ? name[8] - '0' : 0;
            if (load_wav(entry.path(), sound) && sound.seconds > 0.04f) p.sounds.push_back(std::move(sound));
        }
        if (p.sounds.empty()) logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: no bone sounds in {}.", dir.string());
    }
    if (p.sounds.empty()) return false;
    HRESULT result = S_OK;
    if (!p.engine) {
        // XAudio2 2.9 ships with Windows 10 and later; loaded here so nothing else depends on it.
        const auto library = LoadLibraryW(L"XAudio2_9.dll");
        using Create = HRESULT(WINAPI *)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
        const auto create = library ? reinterpret_cast<Create>(GetProcAddress(library, "XAudio2Create")) : nullptr;
        // XAudio2 wants COM on the calling thread; the game's own mode stands if it set one.
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        result = create ? create(&p.engine, 0, XAUDIO2_DEFAULT_PROCESSOR) : E_NOINTERFACE;
        if (FAILED(result)) p.engine = nullptr;
    }
    // Fails with no sound output device (0x80070490): tried again a little later.
    if (p.engine) result = p.engine->CreateMasteringVoice(&p.master);
    if (FAILED(result)) {
        p.retry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: no sound output ({:#x}), bone sounds wait.",
                     static_cast<unsigned>(result));
        return false;
    }
    logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: {} bone sounds loaded.", p.sounds.size());
    return true;
}
} // namespace

void play_bone_sound(float strength, bool broken) noexcept {
    try {
        auto &p = player();
        std::lock_guard lock(p.mutex);
        if (!p.ready && std::chrono::steady_clock::now() >= p.retry) p.ready = start(p);
        if (!p.ready) return;
        // Skate 3 spaces its HoM sounds out (aud_general.hom priorities): never a pile of them at once.
        const auto now = std::chrono::steady_clock::now();
        if (now - p.last < std::chrono::milliseconds(broken ? 40 : 90)) return;
        p.last = now;
        // Finished voices go.
        std::erase_if(p.voices, [](IXAudio2SourceVoice *v) {
            XAUDIO2_VOICE_STATE state{};
            v->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued) return false;
            v->DestroyVoice();
            return true;
        });
        if (p.voices.size() >= 8) return;
        // A break gets one of the long crunches, lesser damage a short knock.
        std::vector<const Sound *> pool;
        for (const auto &s : p.sounds)
            if (broken ? s.seconds >= 0.25f : s.seconds < 0.25f) pool.push_back(&s);
        if (pool.empty()) for (const auto &s : p.sounds) pool.push_back(&s);
        const Sound &sound = *pool[std::uniform_int_distribution<std::size_t>(0, pool.size() - 1)(p.rng)];
        IXAudio2SourceVoice *voice{};
        if (FAILED(p.engine->CreateSourceVoice(&voice, &sound.format))) return;
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(sound.pcm.size());
        buffer.pAudioData = sound.pcm.data();
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        voice->SetVolume(std::clamp(broken ? 1.0f : 0.35f + 0.5f * strength, 0.0f, 1.0f));
        if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
            voice->DestroyVoice();
            return;
        }
        p.voices.push_back(voice);
    } catch (...) {
    }
}
} // namespace dingosdk::skate3_hom
