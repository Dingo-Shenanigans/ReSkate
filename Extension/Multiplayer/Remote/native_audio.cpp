#include "native_audio.h"
#include "native_audio_layout.h"
#include "native_audio_binding.h"
#include "audio_capture.h"
#include "native_pose_layout.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_audio.h"
#include <Windows.h>
#include "Extension/Multiplayer/Session/monotonic_clock.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <memory>
#include <format>
#include <cstring>

namespace dingosdk::multiplayer {
namespace {
namespace sound = addr::native_audio;
using Address = std::uintptr_t;
bool readable(Address p, void *out, std::size_t size) {
    if (p < 0x10000 || size >= memory::highest_user_address || p > memory::highest_user_address - size)
        return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void *>(p), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> T get(Address p, std::size_t off = 0) {
    return native_pose_detail::value<T>(readable, native_pose_detail::add(p, off), "skater audio");
}
Address ptr(Address p, std::size_t off = 0) { return get<Address>(p, off); }
void require(bool valid, const char *why) { native_pose_detail::require(valid, why); }
struct State {
    Address base{};
    DWORD client_thread{};
    std::atomic<Address> local_component{}, local_entity{};
    std::atomic<std::uint64_t> captured{};
    std::uint64_t next_local_audit{};
    void (*submit)(Address, const NativeAudioFrame *){};
    bool attempted{}, installed{};
    std::mutex mutex;
    std::unique_ptr<NativeAudioFrame> accumulated = std::make_unique<NativeAudioFrame>();
    // The local skater's updates as the game gave them, merged: sent whole when the surface sound goes.
    std::unique_ptr<NativeAudioFrame> as_given = std::make_unique<NativeAudioFrame>();
    std::atomic<std::uint32_t> surface_sound{}, surface_fields{~0U};
    std::atomic<bool> surface_resend{}; // the whole state goes once (the surface sound changed)
    int surface_resend_in{};            // local updates until it goes whole (0: not waiting); under mutex
    unsigned surface_updates{};         // local updates with a surface sound, for its regular resend; under mutex
    AudioCaptureBuffer queue;
    std::string capture_issue;
    // Each slot's remote sound component, contiguous so submit_hook (every skater's
    // sound update) scans a few cache lines up to `bound`, the highest slot that
    // has ever stored one plus one. The bound grows before the component is stored.
    std::array<std::atomic<Address>, max_remote_players> components{};
    std::atomic<std::size_t> bound{};
};
State &state() {
    static auto *s = new State;
    return *s;
}
struct RemoteAudio {
    Address manager{};
    std::atomic<Address> remote_entity{};
    std::atomic<std::uint64_t> played{};
    bool playing{};
    std::uint16_t last_handle{}; // Diagnostic only; owned by the native component.
    std::string playback_issue;
    // Owners of the last full binding resolve (read_native_audio_binding walks both
    // actors' component collections). Updates between the 1 Hz resolves re-check
    // these with a few reads and read only the handle again. Client thread.
    Address source{}, binding_local{}, binding_context{};
    std::uint64_t binding_actor{}; // remote_skater_generation() of the actor resolved
    ULONGLONG next_binding{};
};
RemoteAudio &remote() {
    static auto *r = new PeerStorage<RemoteAudio>;
    return r->current();
}
std::atomic<Address> &remote_component() { return state().components[peer_slot]; }
void set_remote_component(Address component) noexcept {
    auto &s = state();
    if (component) {
        const auto needed = peer_slot + 1;
        for (auto current = s.bound.load(std::memory_order_acquire);
             current < needed && !s.bound.compare_exchange_weak(current, needed, std::memory_order_acq_rel);) {
        }
    }
    remote_component().store(component, std::memory_order_release);
}
// Whether a field is one the surface sound is for: one of its fields, naming a surface.
bool surface_field(const State &s, std::size_t field, std::uint32_t word) {
    return word && (s.surface_fields.load(std::memory_order_acquire) & (1U << field));
}
// The surface sound put into a frame's fields it is for (their own flags kept).
void put_surface_sound(State &s, NativeAudioFrame &frame, std::uint32_t sound_word) {
    for (std::size_t field = 0; field < audio_material_offsets.size(); ++field) {
        const auto word = audio_value<std::uint32_t>(frame, audio_material_offsets[field]);
        if (surface_field(s, field, word)) audio_put(frame, audio_material_offsets[field], (word & 0x3fU) | (sound_word & ~0x3fU));
    }
}
// Whether a merged state still has another surface in a field the surface sound is for.
bool surface_stale(State &s, const NativeAudioFrame &frame, std::uint32_t sound_word) {
    for (std::size_t field = 0; field < audio_material_offsets.size(); ++field) {
        const auto word = audio_value<std::uint32_t>(frame, audio_material_offsets[field]);
        if (surface_field(s, field, word) && (word & ~0x3fU) != (sound_word & ~0x3fU)) return true;
    }
    return false;
}
void submit_hook(Address handle, const NativeAudioFrame *frame) {
    auto &s = state();
    // The visual-only actor submits default inputs after animation evaluation.
    // Suppress those updates so they cannot overwrite our buffered peer inputs.
    // Handle creation, description changes and destruction still run natively.
    const auto error_before = GetLastError();
    bool suppress{};
    const auto component = handle > 0x74 ? handle - 0x74 : 0;
    const auto bound = component ? std::min(s.bound.load(std::memory_order_acquire), max_remote_players)
                                 : std::size_t{};
    // A stale slot can still hold a destroyed actor's component address: keep
    // looking until one slot's actor owns it.
    for (std::size_t slot = 0; slot < bound && !suppress; ++slot) {
        if (s.components[slot].load(std::memory_order_acquire) != component)
            continue;
        const PeerScope scope(slot);
        try {
            const auto actor = remote().remote_entity.load();
            if (actor && actor == remote_skater_entity() &&
                ptr(component) == s.base + addr::engine::skater_audio_component_vtable &&
                ptr(ptr(component, 0x18)) == actor && !ptr(actor, 0xf8)) {
                suppress = true;
            }
        } catch (...) {
        }
    }
    SetLastError(error_before);
    if (suppress)
        return;
    const auto c = s.local_component.load(std::memory_order_acquire);
    if (!c || handle != c + 0x74) {
        s.submit(handle, frame); // Every other skater's sound goes on unchanged.
        return;
    }
    // The local skater's: read once, its surface sound put in when one is set, and passed on.
    NativeAudioFrame snapshot;
    const bool read = readable(reinterpret_cast<Address>(frame), snapshot.bytes.data(), 0x218);
    const NativeAudioFrame given = snapshot;
    const auto sound_word = s.surface_sound.load(std::memory_order_acquire);
    if (read && sound_word) put_surface_sound(s, snapshot, sound_word);
    // The game sends a field only when it changes, so the component keeps what it was last sent: a
    // surface sent before the surface sound was set (or while the skater was not followed, after a
    // teleport or a reload) stayed until the skater reached another surface, and when the surface
    // sound went, its surface stayed. Then the whole state goes once, every field marked, as a remote
    // skater's does (audio_frame): the merged state with the surface sound in, or as the game gave it.
    NativeAudioFrame full;
    bool send_full = false;
    std::array<std::uint32_t, 4> masks{};
    const auto copy = reinterpret_cast<void (*)(NativeAudioFrame *, const NativeAudioFrame *, const void *)>(
        s.base + sound::frame_copy);
    if (read) {
        masks = {audio_value<std::uint32_t>(snapshot, 0x164), audio_value<std::uint32_t>(snapshot, 0x130),
                 audio_value<std::uint32_t>(snapshot, 0x124), audio_value<std::uint32_t>(snapshot, 0xc0)};
        if (!(masks[3] & ~0x07ffffffU)) {
            std::lock_guard lock(s.mutex);
            bool resend = s.surface_resend.exchange(false, std::memory_order_acq_rel);
            if (s.surface_resend_in > 0 && --s.surface_resend_in == 0) resend = true;
            // And every 30 updates (about half a second) while a surface sound is set: the game can put
            // its own state back without telling (a reset to a marker went back to the default
            // surface's sound, concrete's, and stayed there; 2026-11-11), and a field it does not
            // change is never sent again.
            if (sound_word && ++s.surface_updates % 30 == 0) resend = true;
            if (sound_word) {
                full = *s.accumulated;
                copy(&full, &snapshot, masks.data());
                send_full = resend || surface_stale(s, full, sound_word);
                if (send_full) put_surface_sound(s, full, sound_word);
            } else if (resend) {
                full = *s.as_given;
                copy(&full, &given, masks.data());
                send_full = true;
            }
            if (send_full) {
                for (const auto off : {0x164, 0x130, 0x124})
                    audio_put(full, off, UINT32_MAX);
                audio_put(full, 0xc0, std::uint32_t{0x07ffffff});
            }
        }
    }
    if (send_full) s.submit(handle, &full);
    else if (read && sound_word) s.submit(handle, &snapshot);
    else s.submit(handle, frame); // Original local audio is forwarded unchanged.
    const auto error = GetLastError();
    std::lock_guard lock(s.mutex);
    try {
        if (c != s.local_component.load() || ptr(c) != s.base + addr::engine::skater_audio_component_vtable ||
            ptr(ptr(c, 0x18)) != s.local_entity.load() || get<std::uint16_t>(handle) < 2)
            throw std::runtime_error("Local sound capture ownership changed.");
        require(read, "Native sound update is unreadable.");
        require(!(masks[3] & ~0x07ffffffU), "Native sound update fields differ.");
        // Pure native field copy merges partial updates into owned storage.
        // It contains only primitive fields and does not allocate or call out.
        const NativeAudioFrame &sent = send_full ? full : snapshot;
        const std::array<std::uint32_t, 4> sent_masks{
            audio_value<std::uint32_t>(sent, 0x164), audio_value<std::uint32_t>(sent, 0x130),
            audio_value<std::uint32_t>(sent, 0x124), audio_value<std::uint32_t>(sent, 0xc0)};
        copy(s.accumulated.get(), &sent, sent_masks.data());
        copy(s.as_given.get(), &given, masks.data());
        s.queue.push(audio_state(*s.accumulated), now_us());
        ++s.captured;
        s.capture_issue.clear();
    } catch (const std::exception &e) {
        s.capture_issue = e.what();
    }
    SetLastError(error);
}
template <std::size_t N>
void fingerprint(Address base, Address rva, const std::array<std::uint8_t, N> &bytes) {
    std::array<std::uint8_t, 32> actual{};
    require(bytes.size() <= actual.size() && readable(base + rva, actual.data(), bytes.size()) &&
                std::equal(bytes.begin(), bytes.end(), actual.begin()),
            "Native skater sound function differs.");
}
void install(Address base) {
    auto &s = state();
    require(!s.attempted, "Sound hook installation requires restarting ReSkate.");
    fingerprint(base, sound::submit, sound::submit_prefix);
    fingerprint(base, sound::frame_copy, sound::frame_copy_prefix);
    fingerprint(base, sound::frame_reset, sound::frame_reset_prefix);
    fingerprint(base, sound::stop_sound, sound::stop_sound_prefix);
    s.attempted = true;
    s.base = base;
    s.client_thread = GetCurrentThreadId();
    void *original{};
    auto *target = reinterpret_cast<void *>(base + sound::submit);
    require(hook_prepare(target, reinterpret_cast<void *>(&submit_hook), &original) == HookOk,
            "Cannot prepare skater sound capture.");
    s.submit = reinterpret_cast<decltype(s.submit)>(original);
    require(hook_enable(target) == HookOk, "Cannot enable skater sound capture.");
    s.installed = true;
}
// The remote actor's sound binding for this update. A full resolve walks both
// actors' component collections; it runs at 1 Hz, or when the owners found last
// time no longer check out. Between, the handle is the only field read again.
NativeAudioBinding resolve_binding(Address base, const NativeFrame &local, Address actor) {
    auto &r = remote();
    const auto now = GetTickCount64();
    if (const auto component = remote_component().load(std::memory_order_acquire);
        component && now < r.next_binding && r.binding_actor == remote_skater_generation() &&
        r.remote_entity.load() == actor && r.binding_local == local.entity &&
        r.binding_context == local.context && r.source &&
        ptr(component) == base + addr::engine::skater_audio_component_vtable &&
        ptr(ptr(component, 0x18)) == actor && ptr(base, addr::engine::audio_manager) == r.manager) {
        NativeAudioBinding out{component, r.manager, get<std::uint16_t>(component, 0x74), r.source};
        // The same handle rules as read_native_audio_binding.
        require(!out.handle || (out.handle >= 2 && out.handle != get<std::uint16_t>(r.source, 0x74) &&
                                get<std::uint8_t>(component, 0x76) == 0),
                "Remote sound handle ownership differs.");
        return out;
    }
    const auto out = read_native_audio_binding(readable, base, local.entity, actor, local.context);
    r.binding_actor = remote_skater_generation();
    r.source = out.source;
    r.binding_local = local.entity;
    r.binding_context = local.context;
    r.next_binding = now + 1000;
    return out;
}
} // namespace
void prepare_remote_audio(Address base) noexcept {
    try {
        if (!state().attempted) install(base);
    } catch (...) {}
}
void prepare_audio_capture(Address base, const NativeFrame &local) noexcept {
    auto &s = state();
    try {
        if (!s.installed)
            install(base);
        require(base == s.base && GetCurrentThreadId() == s.client_thread,
                "Skater sound must update on the client thread.");
        const auto now = now_us();
        if (s.local_entity.load(std::memory_order_acquire) == local.entity &&
            s.local_component.load(std::memory_order_acquire) && now < s.next_local_audit)
            return;
        const auto component =
            read_native_component(readable, local.entity, base + addr::engine::skater_audio_component_vtable);
        require(ptr(local.entity) == base + addr::engine::skater_entity_vtable && ptr(local.entity, 0xf8),
                "Local sound actor ownership differs.");
        if (component != s.local_component.load() || local.entity != s.local_entity.load()) {
            s.local_component.store(0, std::memory_order_release);
            std::lock_guard lock(s.mutex);
            s.queue.clear();
            *s.accumulated = {};
            reinterpret_cast<void (*)(void *)>(base + sound::frame_reset)(s.accumulated.get());
            *s.as_given = {};
            reinterpret_cast<void (*)(void *)>(base + sound::frame_reset)(s.as_given.get());
            // A new component, or the same one after frames went by unfollowed: with a surface sound,
            // sent whole once, after 30 updates have filled in what is merged here.
            s.surface_resend_in = s.surface_sound.load(std::memory_order_acquire) ? 30 : 0;
            s.local_entity.store(local.entity);
            s.local_component.store(component, std::memory_order_release);
            s.capture_issue.clear();
        }
        s.next_local_audit = now + 1000000;
    } catch (const std::exception &e) {
        std::lock_guard lock(s.mutex);
        if (s.capture_issue != e.what())
            logging::log(logging::Level::warning, logging::Channel::runtime, "Skater sound: the local skater's sound is not followed: {}", e.what());
        s.capture_issue = e.what();
    }
}
void set_surface_sound(std::uint32_t word, std::uint32_t fields) noexcept {
    auto &s = state();
    s.surface_sound.store(0, std::memory_order_release);
    s.surface_fields.store(fields, std::memory_order_release);
    s.surface_sound.store(word, std::memory_order_release);
    s.surface_resend.store(true, std::memory_order_release);
}
std::vector<AudioSample> drain_audio_capture(std::uint64_t now) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.queue.drain(now);
}
void stop_remote_audio() noexcept {
    auto &s = state();
    remote().next_binding = 0; // resolve in full next time, whatever was playing
    if (!remote().playing)
        return;
    remote().playing = false;
    try {
        const auto actor = remote().remote_entity.load();
        if (s.installed && GetCurrentThreadId() == s.client_thread && actor &&
            actor == remote_skater_entity()) {
            const auto binding =
                read_native_audio_binding(readable, s.base, s.local_entity.load(), actor, ptr(actor, 0x20));
            if (binding.component == remote_component().load() &&
                binding.manager == remote().manager && binding.handle) {
                // Use the component's native stop operation, including clearing
                // its handle. Never release a cached ID that could be recycled.
                reinterpret_cast<void (*)(Address)>(s.base + sound::stop_sound)(binding.component + 0x74);
                logging::log(logging::Level::debug, logging::Channel::runtime,
                             "Multiplayer: native peer sound stopped; actor={:#x}, handle={}.", actor,
                             binding.handle);
            }
        }
    } catch (...) { // Entity/manager teardown may already have destroyed the voice.
    }
    remote().last_handle = 0;
    remote().manager = 0;
    remote().next_binding = 0;
    // Keep suppressing this actor's default submissions while sound is stale.
    // A subsequent native update may create an idle handle, but it receives no
    // locomotion inputs until fresh peer data arrives. Entity destruction owns it.
}
void reset_audio() noexcept {
    auto &s = state();
    each_peer([&] {
        stop_remote_audio();
        set_remote_component(0);
        remote().remote_entity.store(0);
        remote().next_binding = 0;
        remote().playback_issue.clear();
    });
    s.local_component.store(0, std::memory_order_release);
    s.local_entity.store(0);
    s.next_local_audit = 0;
    std::lock_guard lock(s.mutex);
    s.queue.clear();
    s.capture_issue.clear();
    remote().playback_issue.clear();
}
void update_remote_audio(Address base, const NativeFrame &local, const Pose &pose,
                         const AudioState &audio) noexcept {
    auto &s = state();
    try {
        require(s.installed && base == s.base && GetCurrentThreadId() == s.client_thread,
                "Native sound playback is unavailable.");
        const auto actor = remote_skater_entity();
        const auto binding = resolve_binding(base, local, actor);
        if (remote().remote_entity.load() != actor || remote_component().load() != binding.component ||
            remote().manager != binding.manager) {
            stop_remote_audio();
            set_remote_component(0);
            remote().remote_entity.store(actor);
            set_remote_component(binding.component);
            remote().manager = binding.manager;
        }
        if (!binding.handle) {
            remote().playing = false;
            std::lock_guard lock(s.mutex);
            remote().playback_issue = "Waiting for the remote actor's native sound handle.";
            return;
        }
        const auto frame = audio_frame(audio, pose);
        // Call the original submit directly. The hook only suppresses the
        // engine's visual-only defaults; all sound synthesis stays native.
        s.submit(binding.component + 0x74, &frame);
        remote().playing = true;
        if (remote().last_handle != binding.handle) {
            remote().last_handle = binding.handle;
            logging::log(logging::Level::debug, logging::Channel::runtime,
                         "Multiplayer: native peer sound bound; actor={:#x}, component={:#x}, handle={}, "
                         "manager={:#x}.",
                         actor, binding.component, binding.handle, binding.manager);
        }
        ++remote().played;
        // Only this thread writes the issue; the lock is for status readers.
        if (!remote().playback_issue.empty()) {
            std::lock_guard lock(s.mutex);
            remote().playback_issue.clear();
        }
    } catch (const std::exception &e) {
        stop_remote_audio();
        remote().next_binding = 0; // resolve in full next time
        std::lock_guard lock(s.mutex);
        remote().playback_issue = e.what();
    }
}
std::string native_audio_status() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.capture_issue.empty())
        return "Sound: " + s.capture_issue;
    if (!remote().playback_issue.empty())
        return "Sound: " + remote().playback_issue;
    if (!s.captured.load())
        return "Sound: waiting for native skater audio updates.";
    return remote().playing ? "Sound: native spatial skater and skateboard playback active."
                            : "Sound: captured; waiting for peer playback.";
}
std::uint64_t captured_audio_frames() noexcept { return state().captured.load(); }
std::uint64_t played_audio_frames() noexcept { return remote().played.load(); }
} // namespace dingosdk::multiplayer
