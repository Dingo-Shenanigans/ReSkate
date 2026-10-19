#include "native_voice.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/native_voice.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>

namespace dingosdk::multiplayer {
namespace {
using Address = std::uintptr_t;
using Clock = std::chrono::steady_clock;
namespace voice = addr::native_voice;
template<class T> T read(Address address) noexcept {
    __try { return *reinterpret_cast<const T *>(address); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
}
template<std::size_t N> bool matches(Address address, const std::array<unsigned char, N> &bytes) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void *>(address), bytes.data(), N) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool supported(Address base) noexcept {
    return matches(base + voice::sound_create, voice::sound_create_prefix) &&
        matches(base + voice::sound_destroy, voice::sound_destroy_prefix) &&
        matches(base + voice::sound_set_input, voice::sound_set_input_prefix) &&
        matches(base + voice::samples_submit, voice::samples_submit_prefix) &&
        matches(base + voice::samples_remove, voice::samples_remove_prefix);
}
bool create_sound(Address base, Address asset, Address *handle) noexcept {
    __try {
        reinterpret_cast<Address *(*)(Address *, const Address *)>(base + voice::sound_create)(handle, &asset);
        return *handle != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool set_input(Address base, Address *handle, Address type, const void *input) noexcept {
    __try {
        const auto manager = read<Address>(base + voice::sound_manager);
        if (!manager || !*handle || !reinterpret_cast<bool (*)(Address, Address *, Address)>(
            read<Address>(read<Address>(manager) + 0x60))(manager, handle, type)) return false;
        reinterpret_cast<void (*)(Address *, Address, unsigned, const void *)>(base + voice::sound_set_input)(handle, type, 0, input);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool destroy_sound(Address base, Address *handle) noexcept {
    __try {
        reinterpret_cast<void (*)(Address *)>(base + voice::sound_destroy)(handle);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool remove_samples(Address base, Address provider, std::uint64_t id) noexcept {
    __try {
        reinterpret_cast<void (*)(Address, std::uint64_t)>(base + voice::samples_remove)(provider, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool submit_samples(Address base, Address provider, std::uint64_t id, const NativeVoiceBlock &block) noexcept {
    __try {
        reinterpret_cast<void (*)(Address, const std::int16_t *, int, std::uint64_t)>(base + voice::samples_submit)(
            provider, block.data(), static_cast<int>(block.size()), id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
struct NativeVoicePlayback::Impl {
    struct Request {
        std::uint64_t serial{};
        std::array<float, 3> position{};
        float gain{};
        NativeVoicePcm pcm;
        Clock::time_point received{}, updated{};
    };
    struct Sound {
        std::uint64_t serial{}, source{};
        Address handle{};
        Clock::time_point queued_until{};
    };
    std::mutex mutex;
    std::map<std::uint64_t, Request> requests;
    std::map<std::uint64_t, Sound> sounds;
    std::uint64_t generation{}, serial{}, source_tag{};
    std::atomic<bool> ready{};
    Address base{}, manager{}, provider{}, asset{};
    DWORD thread{};
    bool checked{}, valid{}, failed{}, logged{};
    bool release(Sound &sound) {
        bool ok = true;
        if (sound.handle && manager && read<Address>(base + voice::sound_manager) == manager)
            ok = destroy_sound(base, &sound.handle);
        if (provider && read<Address>(base + voice::samples_provider) == provider)
            ok = remove_samples(base, provider, sound.source) && ok;
        sound = {};
        return ok;
    }
    void clear() {
        for (auto &[id, sound] : sounds) if (!release(sound)) failed = true;
        sounds.clear();
    }
    void process(Address image) {
        if (!checked) {
            base = image;
            thread = GetCurrentThreadId();
            valid = supported(base);
            checked = true;
        }
        if (!valid || base != image || thread != GetCurrentThreadId()) { ready = false; return; }
        std::lock_guard lock(mutex);
        const auto next_manager = read<Address>(base + voice::sound_manager);
        const auto next_provider = read<Address>(base + voice::samples_provider);
        const auto spatializer = read<Address>(base + voice::spatializer);
        const auto next_asset = spatializer ? read<Address>(spatializer + 0x228) : 0;
        const bool loaded = next_manager && next_provider && next_asset &&
            read<Address>(next_manager) == base + voice::sound_manager_vtable &&
            read<Address>(next_asset) == base + voice::voice_asset_vtable &&
            read<std::uint64_t>(next_asset - 16) == 0x0649ef62b6644d8aULL &&
            read<std::uint64_t>(next_asset - 8) == 0x5f435637e4220e96ULL;
        if (manager != next_manager || provider != next_provider || asset != next_asset || !loaded || failed) {
            clear();
            manager = next_manager; provider = next_provider; asset = next_asset;
        }
        ready = loaded && !failed;
        if (!ready) { requests.clear(); return; }
        const auto now = Clock::now();
        std::erase_if(requests, [&](const auto &item) { return now - item.second.updated > std::chrono::milliseconds(300); });
        for (auto i = sounds.begin(); i != sounds.end();) {
            const auto request = requests.find(i->first);
            if (request == requests.end() || request->second.serial != i->second.serial || request->second.gain <= 0.f) {
                if (!release(i->second)) failed = true;
                i = sounds.erase(i);
            } else ++i;
        }
        for (auto &[id, request] : requests) {
            if (request.gain <= 0.f) { request.pcm = {}; continue; }
            if (now - request.received > std::chrono::milliseconds(250)) request.pcm = {};
            if (now - request.received > std::chrono::milliseconds(40)) request.pcm.flush();
            auto found = sounds.find(id);
            if (found == sounds.end()) {
                if (!request.pcm.samples()) continue;
                Sound sound;
                sound.serial = request.serial;
                sound.source = source_tag | request.serial;
                if (!create_sound(base, asset, &sound.handle)) throw std::runtime_error("Native voice sound creation failed");
                found = sounds.emplace(id, sound).first;
                if (!set_input(base, &found->second.handle, base + voice::source_input_type, &found->second.source))
                    throw std::runtime_error("Native voice source binding failed");
            }
            auto &sound = found->second;
            alignas(16) std::array<float, 16> transform{1,0,0,0, 0,1,0,0, 0,0,1,0,
                request.position[0],request.position[1],request.position[2],1};
            if (!set_input(base, &sound.handle, base + voice::transform_input_type, transform.data()))
                throw std::runtime_error("Native voice positioning failed");
            NativeVoiceBlock block;
            while (request.pcm.pop(block, request.gain)) {
                if (sound.queued_until > now + std::chrono::milliseconds(240)) continue;
                if (!submit_samples(base, provider, sound.source, block))
                    throw std::runtime_error("Native voice sample submission failed");
                sound.queued_until = std::max(now, sound.queued_until) + std::chrono::milliseconds(20);
                if (!logged) {
                    logged = true;
                    logging::log(logging::Level::info, logging::Channel::runtime,
                        "Voice playback uses Frostbite's spatialized VOIP sound (16 kHz PCM, native obstruction/reverb traits).");
                }
            }
        }
        if (failed) throw std::runtime_error("Native voice cleanup failed");
    }
};
NativeVoicePlayback::NativeVoicePlayback(std::uint64_t source_tag) : impl_(std::make_unique<Impl>()) {
    impl_->source_tag = source_tag & 0xffffffff00000000ULL;
}
NativeVoicePlayback::~NativeVoicePlayback() = default;
bool NativeVoicePlayback::available() const noexcept { return impl_->ready.load(); }
void NativeVoicePlayback::reset(std::uint64_t generation) {
    std::lock_guard lock(impl_->mutex);
    impl_->generation = generation;
    impl_->requests.clear();
}
void NativeVoicePlayback::remove(std::uint64_t id, std::uint64_t generation) {
    std::lock_guard lock(impl_->mutex);
    if (generation == impl_->generation) impl_->requests.erase(id);
}
void NativeVoicePlayback::position(std::uint64_t id, std::uint64_t generation, const std::array<float, 3> &position, float gain) {
    if (!std::all_of(position.begin(), position.end(), [](float v) { return std::isfinite(v); }) || !std::isfinite(gain)) return;
    std::lock_guard lock(impl_->mutex);
    if (generation != impl_->generation || !available()) return;
    auto found = impl_->requests.find(id);
    if (found == impl_->requests.end()) {
        if (impl_->requests.size() >= max_remote_players || impl_->serial == 0xffffffff) return;
        found = impl_->requests.try_emplace(id).first;
        found->second.serial = ++impl_->serial;
    } else if (found->second.gain <= 0.f && gain > 0.f) {
        if (impl_->serial == 0xffffffff) return;
        found->second.serial = ++impl_->serial;
    }
    found->second.position = position;
    found->second.gain = std::clamp(gain, 0.f, 1000.f);
    found->second.updated = Clock::now();
}
bool NativeVoicePlayback::submit(std::uint64_t id, std::uint64_t generation, std::span<const std::uint8_t> pcm) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->requests.find(id);
    if (generation != impl_->generation || !available() || found == impl_->requests.end()) return false;
    if (!found->second.pcm.append(pcm)) return false;
    found->second.received = Clock::now();
    return true;
}
void NativeVoicePlayback::process(std::uintptr_t base) noexcept {
    try { impl_->process(base); }
    catch (const std::exception &e) {
        impl_->ready = false;
        impl_->failed = true;
        std::lock_guard lock(impl_->mutex);
        impl_->clear();
        impl_->requests.clear();
        logging::log(logging::Level::warning, logging::Channel::runtime, "{}; using fallback voice playback.", e.what());
    }
}
}
