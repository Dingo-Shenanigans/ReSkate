#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <span>

namespace dingosdk::multiplayer {
// The rate ReSkate gives the game's voice sound its samples at: the game's own is 16 kHz, and
// native_voice.cpp patches the sound to take 48 kHz (it is not used unless that patch is in).
// The game still takes them 320 at a time: 6.67 ms a block at this rate.
inline constexpr unsigned native_voice_rate = 48000;
inline constexpr std::size_t native_voice_samples = 320;
using NativeVoiceBlock = std::array<std::int16_t, native_voice_samples>;

class NativeVoicePcm {
public:
    bool append(std::span<const std::uint8_t> bytes) {
        if (bytes.size() % 2 || samples() + bytes.size() / 2 > native_voice_rate / 2) return false;
        for (std::size_t n = 0; n < bytes.size(); n += 2) {
            std::memcpy(&partial_[used_++], bytes.data() + n, 2);
            if (used_ == partial_.size()) flush();
        }
        return true;
    }
    void flush() {
        if (!used_) return;
        std::fill(partial_.begin() + used_, partial_.end(), std::int16_t{});
        blocks_.push_back(partial_);
        used_ = 0;
    }
    bool pop(NativeVoiceBlock &block, float gain) {
        if (blocks_.empty()) return false;
        block = blocks_.front();
        blocks_.pop_front();
        gain = std::isfinite(gain) ? std::clamp(gain, 0.f, 1000.f) : 0.f;
        for (auto &sample : block)
            sample = static_cast<std::int16_t>(std::clamp(static_cast<float>(sample) * gain, -32768.f, 32767.f));
        return true;
    }
    std::size_t samples() const { return blocks_.size() * native_voice_samples + used_; }
private:
    std::deque<NativeVoiceBlock> blocks_;
    NativeVoiceBlock partial_{};
    std::size_t used_{};
};
}
