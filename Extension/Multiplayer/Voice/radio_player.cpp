#include "radio_player.h"
#include "Extension/Multiplayer/Net/radio_frames.h"
#include <Windows.h>
#include <xaudio2.h>
#include <wrl/client.h>
#include <opus.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace dingosdk::multiplayer {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t frame_values = radio_frame_samples * radio_channels;
constexpr std::size_t prebuffer_frames = 15;  // 300 ms queued before playback starts
constexpr std::size_t max_queued_frames = 50; // 1 s: past this the server runs ahead of this device
constexpr std::uint32_t max_concealed = 5;    // lost frames the decoder fills in; a longer gap restarts it
constexpr std::size_t max_incoming = 64;      // batches waiting for the thread
struct Frame {
    std::array<opus_int16, frame_values> pcm{};
    std::atomic<bool> done{};
};
struct Stream final : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void *context) override {
        static_cast<Frame *>(context)->done.store(true, std::memory_order_release);
    }
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override { failed.store(true); }
    std::atomic<bool> failed{};
};
struct Decoder {
    OpusDecoder *state{};
    Decoder() {
        int error{};
        state = opus_decoder_create(radio_rate, radio_channels, &error);
        if (!state || error != OPUS_OK) throw std::runtime_error("Cannot start the radio decoder.");
    }
    ~Decoder() { opus_decoder_destroy(state); }
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    void reset() { opus_decoder_ctl(state, OPUS_RESET_STATE); }
    // An empty frame asks for concealment of a lost one.
    bool decode(std::span<const std::uint8_t> frame, Frame &out) {
        const auto count = opus_decode(state, frame.empty() ? nullptr : frame.data(), static_cast<opus_int32>(frame.size()),
                                       out.pcm.data(), static_cast<int>(radio_frame_samples), 0);
        return count == static_cast<int>(radio_frame_samples);
    }
};
// The device side: one engine, one stereo voice, the frames it still reads.
struct Output {
    Microsoft::WRL::ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice *master{};
    IXAudio2SourceVoice *voice{};
    Stream stream;
    std::deque<std::unique_ptr<Frame>> queued;
    bool started{};
    ~Output() { close(); }
    void close() {
        // DestroyVoice returns once the voice no longer reads any buffer.
        if (voice) { voice->DestroyVoice(); voice = nullptr; }
        queued.clear();
        started = false;
        if (master) { master->DestroyVoice(); master = nullptr; }
        engine.Reset();
        stream.failed.store(false);
    }
    void open() {
        if (voice) return;
        if (!engine && FAILED(XAudio2Create(&engine))) throw std::runtime_error("Cannot start radio playback.");
        if (!master && FAILED(engine->CreateMasteringVoice(&master, radio_channels, radio_rate, 0, nullptr, nullptr, AudioCategory_GameMedia)))
            throw std::runtime_error("No playback device is available for the radio.");
        WAVEFORMATEX format{WAVE_FORMAT_PCM, static_cast<WORD>(radio_channels), radio_rate,
                            radio_rate * radio_channels * 2, static_cast<WORD>(radio_channels * 2), 16, 0};
        if (FAILED(engine->CreateSourceVoice(&voice, &format, 0, 1.f, &stream)))
            throw std::runtime_error("Cannot create the radio's audio stream.");
    }
    // Back to silence and an empty buffer, keeping the device.
    void flush() {
        if (voice) { voice->DestroyVoice(); voice = nullptr; }
        queued.clear();
        started = false;
        open();
    }
    void collect() {
        while (!queued.empty() && queued.front()->done.load(std::memory_order_acquire)) queued.pop_front();
    }
    void submit(std::unique_ptr<Frame> frame) {
        if (queued.size() >= max_queued_frames) return;
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(sizeof(frame->pcm));
        buffer.pAudioData = reinterpret_cast<const BYTE *>(frame->pcm.data());
        buffer.pContext = frame.get();
        if (FAILED(voice->SubmitSourceBuffer(&buffer))) throw std::runtime_error("The radio's audio stream failed.");
        queued.push_back(std::move(frame));
    }
};
}
struct RadioPlayer::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    RadioModel state;
    std::uint64_t session{}, generation{};
    std::deque<std::vector<std::uint8_t>> incoming;
    std::jthread worker;
    ~Impl() {
        worker.request_stop(); wake.notify_all();
        if (worker.joinable()) worker.join();
    }
    void run(std::stop_token stop_token) noexcept {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Output output;
        std::unique_ptr<Decoder> decoder;
        std::uint32_t track{}, next{};
        std::uint64_t current_generation{};
        Clock::time_point last_batch{}, retry{};
        while (!stop_token.stop_requested()) {
            RadioSettings settings;
            std::deque<std::vector<std::uint8_t>> batches;
            std::uint64_t revision{};
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(20));
                settings = state.settings; revision = generation;
                batches.swap(incoming);
            }
            const auto now = Clock::now();
            bool playing{};
            std::string detail;
            try {
                if (revision != current_generation || !settings.enabled || output.stream.failed.load()) {
                    output.close();
                    track = 0;
                    current_generation = revision;
                }
                if (!settings.enabled) detail = "Server radio is muted.";
                else {
                    for (const auto &bytes : batches) {
                        const auto batch = decode_radio_batch(bytes);
                        if (!batch || now < retry) continue;
                        last_batch = now;
                        output.open();
                        if (!decoder) decoder = std::make_unique<Decoder>();
                        if (batch->track != track) {
                            output.flush();
                            decoder->reset();
                            track = batch->track;
                            next = batch->first;
                        }
                        for (std::size_t i = 0; i < batch->frames.size(); ++i) {
                            const auto index = batch->first + static_cast<std::uint32_t>(i);
                            if (index < next) continue; // already played, or a duplicate
                            if (index - next > max_concealed) decoder->reset();
                            else
                                for (; next < index; ++next) {
                                    auto lost = std::make_unique<Frame>();
                                    if (decoder->decode({}, *lost)) output.submit(std::move(lost));
                                }
                            next = index + 1;
                            auto frame = std::make_unique<Frame>();
                            if (decoder->decode(batch->frames[i], *frame)) output.submit(std::move(frame));
                        }
                    }
                    output.collect();
                    if (output.voice) {
                        output.voice->SetVolume(settings.volume);
                        // An underrun waits for the buffer to fill again instead of stuttering.
                        if (output.started && output.queued.empty()) {
                            output.voice->Stop();
                            output.started = false;
                        }
                        // A short tail (the stream ending) plays without waiting for a full buffer.
                        if (!output.started && !output.queued.empty() &&
                            (output.queued.size() >= prebuffer_frames || now - last_batch > std::chrono::milliseconds(200))) {
                            if (FAILED(output.voice->Start())) throw std::runtime_error("Cannot start the radio's audio stream.");
                            output.started = true;
                        }
                        if (!output.started && now - last_batch > std::chrono::seconds(2)) {
                            output.close();
                            track = 0;
                        }
                    }
                    playing = output.started;
                    detail = playing ? "Playing the server's radio." : output.voice ? "Buffering the server's radio..." :
                        "The server is not playing anything.";
                }
            } catch (const std::exception &error) {
                output.close(); track = 0;
                detail = error.what();
                retry = now + std::chrono::seconds(2);
            } catch (...) {
                output.close(); track = 0;
                detail = "Radio playback failed.";
                retry = now + std::chrono::seconds(2);
            }
            {
                std::lock_guard lock(mutex);
                if (generation == revision) { state.playing = playing; state.status = std::move(detail); }
            }
        }
        output.close();
        decoder.reset();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};
RadioPlayer::RadioPlayer() : impl_(std::make_unique<Impl>()) {}
RadioPlayer::~RadioPlayer() = default;
void RadioPlayer::configure(RadioSettings settings) {
    if (!settings.valid()) return;
    std::lock_guard lock(impl_->mutex);
    if (!settings.enabled) impl_->incoming.clear();
    impl_->state.settings = settings;
    impl_->wake.notify_all();
}
RadioModel RadioPlayer::model() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->state;
}
void RadioPlayer::receive(std::uint64_t session, std::span<const std::uint8_t> batch) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->state.settings.enabled) return;
    if (session != impl_->session) {
        impl_->session = session;
        ++impl_->generation;
        impl_->incoming.clear();
    }
    if (impl_->incoming.size() >= max_incoming) return;
    impl_->incoming.emplace_back(batch.begin(), batch.end());
    if (!impl_->worker.joinable())
        impl_->worker = std::jthread([p = impl_.get()](std::stop_token stop) { p->run(stop); });
    impl_->wake.notify_all();
}
void RadioPlayer::reset() {
    std::lock_guard lock(impl_->mutex);
    impl_->session = 0;
    ++impl_->generation;
    impl_->incoming.clear();
    impl_->state.playing = false;
    impl_->state.status = "No server radio.";
    impl_->wake.notify_all();
}
}
