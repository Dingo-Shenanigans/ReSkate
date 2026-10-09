#pragma once
#include "Engine/Game/Multiplayer/voice_settings.h"
#include <cstdint>
#include <memory>
#include <span>

namespace dingosdk::multiplayer {
// A dedicated server's radio (PacketKind::radio): its Opus batches decoded on a thread of their
// own and played on an XAudio2 voice of their own, stereo 48 kHz, after ~300 ms of buffering.
// A new track restarts the decoder; a short gap is concealed, a long one or an underrun waits
// for the buffer to fill again (silence, never a stale frame). Nothing arriving for a while
// releases the audio device.
class RadioPlayer {
public:
    RadioPlayer();
    ~RadioPlayer();
    RadioPlayer(const RadioPlayer &) = delete;
    RadioPlayer &operator=(const RadioPlayer &) = delete;
    void configure(RadioSettings);
    RadioModel model() const;
    // One batch as a decoded packet carries it (Packet::radio, already checked by decode()).
    void receive(std::uint64_t session, std::span<const std::uint8_t> batch);
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
