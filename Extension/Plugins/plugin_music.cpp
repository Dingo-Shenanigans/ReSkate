#include "plugin_music.h"
#include <wrl/client.h>

namespace dingosdk::plugin_sound {
namespace {
struct Output {
    HRESULT apartment{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    Microsoft::WRL::ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice* master{};

    bool ready() {
        if (master) return true;
        if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return false;
        if (FAILED(XAudio2Create(engine.ReleaseAndGetAddressOf()))) return false;
        return SUCCEEDED(engine->CreateMasteringVoice(&master));
    }
    ~Output() {
        if (master) master->DestroyVoice();
        engine.Reset();
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
};
}

MusicSource play_music(const plugins::StreamWave& wave, bool loop, plugins::MusicLoop region, float volume, float pitch) {
    thread_local Output output;
    if (!output.ready()) return {};
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(wave.channels);
    format.nSamplesPerSec = wave.rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(wave.channels * 2);
    format.nAvgBytesPerSec = wave.rate * format.nBlockAlign;
    IXAudio2SourceVoice* raw{};
    if (FAILED(output.engine->CreateSourceVoice(&raw, &format, 0, 4.0f))) return {};
    MusicSource source(raw);
    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.pAudioData = wave.pcm.data();
    buffer.AudioBytes = static_cast<UINT32>(wave.pcm.size());
    buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
    buffer.LoopBegin = region.first;
    buffer.LoopLength = region.frames;
    if (FAILED(source->SetVolume(volume)) || FAILED(source->SetFrequencyRatio(pitch)) ||
        FAILED(source->SubmitSourceBuffer(&buffer)) || FAILED(source->Start())) return {};
    return source;
}
}
