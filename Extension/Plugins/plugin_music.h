#pragma once
#include "plugin_stream_wave.h"
#include <xaudio2.h>
#include <memory>

namespace dingosdk::plugin_sound {
// The caller holds the mapped samples until the voice has been destroyed.
struct DestroySource {
    void operator()(IXAudio2SourceVoice* voice) const noexcept { voice->DestroyVoice(); }
};
using MusicSource = std::unique_ptr<IXAudio2SourceVoice, DestroySource>;
MusicSource play_music(const plugins::StreamWave& wave, bool loop, plugins::MusicLoop region, float volume, float pitch);
}
