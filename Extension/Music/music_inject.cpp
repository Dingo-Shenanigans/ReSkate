#include "music_inject.h"
#include "app_audio_capture.h"
#include "spotify_device.h"
#include "external_audio.h"
#include "youtube_music.h"
#include "external_player.h"
#include "spotify_station.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/audio_core.h"
#include "Engine/Game/Build/addresses.h"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>

namespace dingosdk::music_inject {
namespace {
namespace ac = addr::audio_core;
using Process = std::uint64_t (*)(std::uintptr_t player, std::uintptr_t mixer);
using Command = void (*)(std::uintptr_t player, int command, std::uintptr_t arguments);
using Exit = void (WINAPI*)(UINT code);
Process process_original{};
Command command_original{};
Exit exit_original{};
std::atomic<bool> installed{}, hooked{};
std::atomic<int> which{1};
std::atomic<float> loudness{1.0f};
// The sound file of the station's own song: the one a player is asked for just after the game
// begins that song (the asking is a moment after, on the same thread or the next frame).
std::atomic<std::uint64_t> expect_until{};
char station_file[120]{};
std::atomic<std::uint64_t> last_replaced{}; // GetTickCount64 of the last block replaced
std::atomic<std::uint64_t> replaced_blocks{}, replaced_frames{};

// The game's players of sound files seen so far, for the log and the console: what each was
// last asked to play and what it produces. Written by the game's threads, read by the client
// thread; a name read while it is being written is a muddle for one line and no more.
struct Player {
    std::atomic<std::uintptr_t> address{};
    char name[120]{};
    std::atomic<std::uint32_t> requests{}, said{};
    std::atomic<std::uint64_t> blocks{}, replaced{};
    std::atomic<std::uint32_t> channels{}, rate{}, seconds{};
};
std::array<Player, 96> players{};

Player* player_of(std::uintptr_t address) noexcept {
    for (auto& player : players) {
        auto held = player.address.load(std::memory_order_relaxed);
        if (held == address) return &player;
        if (!held && player.address.compare_exchange_strong(held, address, std::memory_order_relaxed)) return &player;
        if (held == address) return &player;
    }
    return nullptr;
}

template <class T> T at(std::uintptr_t address) noexcept {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

// A block the player has just produced into `buffer` is written over with what Spotify is
// playing. The audio thread: no locks, no allocation.
bool replace(std::uint64_t reader, std::uintptr_t buffer, std::uint32_t frames, std::uint32_t channels, float rate) noexcept {
    __try {
        const auto data = at<std::uintptr_t>(buffer + ac::buffer_data);
        const bool together = at<std::uint8_t>(buffer + ac::buffer_together) != 0;
        const auto stride = at<std::uint16_t>(buffer + ac::buffer_stride);
        if (!data || (together && stride < frames)) return false;
        const auto channel = [&](std::uint32_t index) {
            return together ? reinterpret_cast<float*>(data + std::size_t{stride} * index * 4) : at<float*>(data + std::size_t{index} * 8);
        };
        float* const left = channel(0);
        float* const right = channel(1);
        if (!left || !right) return false;
        app_audio::read(reader, left, right, frames, rate);
        if (const float gain = loudness.load(std::memory_order_relaxed); gain != 1.0f)
            for (std::uint32_t i = 0; i < frames; ++i) {
                left[i] *= gain;
                right[i] *= gain;
            }
        for (std::uint32_t i = 2; i < channels; ++i)
            if (float* const other = channel(i)) std::memset(other, 0, std::size_t{frames} * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint64_t process_hook(std::uintptr_t player, std::uintptr_t mixer) {
    // What it is playing, read before it moves on to its next file.
    std::uint32_t length{};
    float voice_rate{};
    std::uintptr_t buffer{};
    std::uint64_t reader{};
    __try {
        const auto voice = player + at<std::uint16_t>(player + ac::player_voices) + std::size_t{at<std::uint8_t>(player + ac::player_voice)} * ac::voice_size;
        if (const auto state = at<std::uint8_t>(voice + ac::voice_state); state != 0 && state != 5) {
            length = at<std::uint32_t>(voice + ac::voice_frames);
            voice_rate = at<float>(voice + ac::voice_rate);
        }
        buffer = at<std::uintptr_t>(mixer + ac::mixer_write);
        // The game may run one player for more than one mix, and a player has more than one
        // file going while it fades from one to the next: each of those reads for itself.
        reader = (player * 0x9e3779b97f4a7c15ull) ^ (mixer * 0xc2b2ae3d27d4eb4full) ^ at<std::uint8_t>(player + ac::player_voice);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        buffer = 0;
    }
    const auto result = process_original(player, mixer);
    if (!result || !buffer) return result;
    __try {
        const auto frames = at<std::uint32_t>(mixer + ac::mixer_frames);
        const auto channels = std::uint32_t{at<std::uint8_t>(mixer + ac::mixer_channels)};
        const auto rate = at<float>(mixer + ac::mixer_rate);
        if (!frames || frames > 0x4000 || at<std::uintptr_t>(mixer + ac::mixer_written) != buffer) return result;
        const std::uint32_t seconds = voice_rate > 1000.0f ? static_cast<std::uint32_t>(static_cast<float>(length) / voice_rate) : 0;
        auto* const seen = player_of(player);
        if (seen) {
            seen->blocks.fetch_add(1, std::memory_order_relaxed);
            seen->channels.store(channels, std::memory_order_relaxed);
            seen->rate.store(static_cast<std::uint32_t>(rate), std::memory_order_relaxed);
            if (seconds) seen->seconds.store(seconds, std::memory_order_relaxed);
        }
        const int mode = which.load(std::memory_order_relaxed);
        if (!mode || channels < 2 || rate < 8000.0f || !spotify_station::active()) return result;
        // (A song's length is kept from its last block that had one: the blocks of its fade
        // out are of a file it has already left.)
        // (A song's length is kept from its last block that had one: the blocks of its fade
        // out are of a file it has already left.)
        // The station's own song is replaced whatever its length (one a station was given was
        // half a minute long, and was heard in the place of what the station plays, 2026-10-12).
        const bool stations_own = seen && station_file[0] && std::strncmp(seen->name, station_file, sizeof(station_file)) == 0;
        if (mode == 1 && !stations_own && (seen ? seen->seconds.load(std::memory_order_relaxed) : seconds) < 60) return result;
        if (mode == 3 && !stations_own) return result;
        if (replace(reader, buffer, frames, channels, rate)) {
            last_replaced.store(GetTickCount64(), std::memory_order_relaxed);
            replaced_blocks.fetch_add(1, std::memory_order_relaxed);
            replaced_frames.fetch_add(frames, std::memory_order_relaxed);
            if (seen) seen->replaced.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

void command_hook(std::uintptr_t player, int command, std::uintptr_t arguments) {
    if (command == ac::command_play && arguments) {
        __try {
            if (auto* const seen = player_of(player)) {
                const auto name = at<const char*>(arguments + ac::command_play_name);
                std::size_t length{};
                if (name)
                    while (length + 1 < sizeof(seen->name) && name[length]) {
                        seen->name[length] = name[length];
                        ++length;
                    }
                seen->name[length] = 0;
                if (const auto until = expect_until.load(std::memory_order_relaxed); until && GetTickCount64() < until && length) {
                    expect_until.store(0, std::memory_order_relaxed);
                    std::memcpy(station_file, seen->name, sizeof(station_file));
                }
                seen->seconds.store(0, std::memory_order_relaxed);
                seen->requests.fetch_add(1, std::memory_order_relaxed);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    command_original(player, command, arguments);
}

// The game is ending: Spotify's own output is put back first.
//
// Not when it is the Spotify library built into this DLL that asks for the end (librespot ends
// the whole program in a dozen places: on an account that is not Premium, on a sound it cannot
// play): that took the game down with it at every start for a player whose account was not
// Premium, 2026-10-12. Only the thread that asked is ended, and the device is left stopped.
// The library is known by the call coming from this DLL, which itself never ends the program.
void WINAPI exit_hook(UINT code) {
    HMODULE caller{}, own{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(_ReturnAddress()), &caller) &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&exit_hook), &own) && caller == own) {
        spotify_device::library_gave_up();
        ExitThread(code);
    }
    app_audio::stop();
    exit_original(code);
}

bool matches(std::uintptr_t base, const game::build::Fingerprint& contract) noexcept {
    std::array<unsigned char, 32> bytes{};
    return memory::peek(base + contract.rva, bytes) && bytes == contract.bytes;
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return hooked.load();
    if (!base || !matches(base, ac::stream_player_process) || !matches(base, ac::stream_player_command)) {
        try {
            logging::log(logging::Level::warning, logging::Channel::music, "Spotify through the game: the game's sound file player is not the one this was written for.");
        } catch (...) {}
        return false;
    }
    const auto process = reinterpret_cast<void*>(base + ac::stream_player_process.rva), command = reinterpret_cast<void*>(base + ac::stream_player_command.rva);
    if (hook_prepare(command, reinterpret_cast<void*>(&command_hook), reinterpret_cast<void**>(&command_original)) != HookOk || hook_enable(command) != HookOk) {
        command_original = nullptr;
        return false;
    }
    if (hook_prepare(process, reinterpret_cast<void*>(&process_hook), reinterpret_cast<void**>(&process_original)) != HookOk || hook_enable(process) != HookOk) {
        process_original = nullptr;
        return false;
    }
    if (const auto exit = reinterpret_cast<void*>(&ExitProcess);
        hook_prepare(exit, reinterpret_cast<void*>(&exit_hook), reinterpret_cast<void**>(&exit_original)) != HookOk || hook_enable(exit) != HookOk)
        exit_original = nullptr;
    hooked.store(true);
    // (Spotify left turned down by a game that ended without putting it back.)
    app_audio::restore_source(L"Spotify.exe");
    return true;
}

void station_song_begins() noexcept { expect_until.store(GetTickCount64() + 1500, std::memory_order_relaxed); }
int mode() noexcept { return which.load(std::memory_order_relaxed); }
void set_mode(int mode) noexcept { which.store(std::clamp(mode, 0, 3), std::memory_order_relaxed); }
float gain() noexcept { return loudness.load(std::memory_order_relaxed); }
void set_gain(float gain) noexcept { loudness.store(std::clamp(gain, 0.0f, 16.0f), std::memory_order_relaxed); }

void tick() noexcept {
    if (!hooked.load(std::memory_order_relaxed)) return;
    static bool started{};
    static std::uint64_t looked{};
    static unsigned said{};
    const auto now = GetTickCount64();
    const bool on = spotify_station::active() && which.load(std::memory_order_relaxed) != 0;
    // The sound is the game's own Spotify device's when an account is on it (spotify_device.h),
    // and what the Spotify app on this computer plays otherwise.
    // (And the picked application's, external_audio.h, when its station is the one that is on.)
    // (And YouTube Music's, youtube_music.h, which fills the buffer itself as the device does.)
    const bool others = external_audio::on(), tube = youtube_music::on();
    const bool device = !others && !tube && spotify_device::connected();
    app_audio::set_external(device || tube, tube ? youtube_music::rate() : 44100);
    if (on && !device && !tube) {
        const auto name = others ? external_audio::app() : std::string("Spotify.exe");
        if (!name.empty()) {
            started = true;
            app_audio::target(std::wstring(name.begin(), name.end()).c_str());
        }
    }
    // Spotify's own output is down only while its sound is in fact going through the game. When
    // the game stops asking for the song's sound (the player paused the game's music), Spotify
    // is paused, and kept down for the moment that takes; it plays again when the game does.
    if (started || device || tube) {
        static bool flowing{}, paused{};
        const auto since = now - last_replaced.load(std::memory_order_relaxed);
        const bool flows = on && since < 700;
        if (flows != flowing) {
            flowing = flows;
            if (!flows && on) {
                external_player::send(external_player::Command::pause);
                paused = true;
            } else if (flows && paused) {
                external_player::send(external_player::Command::play);
                paused = false;
            }
            if (!on) paused = false;
        }
        if (started) app_audio::mute_source(on && !device && !tube && since < 2500);
    }
    if (now - looked < 1000) return;
    // How fast the game is taking the sound, against the clock: more than it can play (it is
    // catching up after standing still, as when the player comes back to its window, and the
    // music is heard fast for a moment: reported 2026-10-12) or less. Said when it is not as it
    // should be, with whether the game's window is the one in front.
    {
        static std::uint64_t counted{}, counted_starved{};
        static unsigned told{};
        const auto frames = replaced_frames.load(std::memory_order_relaxed);
        const auto capture = app_audio::state();
        const double seconds = static_cast<double>(now - looked) / 1000.0;
        if (on && looked && seconds < 3.0 && capture.readers && told < 80) {
            const double pace = static_cast<double>(frames - counted) / (48000.0 * seconds * capture.readers);
            DWORD front{};
            GetWindowThreadProcessId(GetForegroundWindow(), &front);
            const bool focused = front == GetCurrentProcessId();
            static bool was_focused{true};
            if (pace > 1.12 || (pace < 0.88 && pace > 0.01) || focused != was_focused) {
                ++told;
                try {
                    logging::log(logging::Level::info, logging::Channel::music, "Pace: the game took the sound at {:.2f} of the clock's speed over {:.1f} s ({} reader(s), {} frame(s) it wanted were not there); its window is {}.", pace,
                        seconds, capture.readers, capture.starved - counted_starved, focused ? "in front" : "not in front");
                } catch (...) {}
            }
            was_focused = focused;
        }
        counted = frames;
        counted_starved = capture.starved;
    }
    looked = now;
    // How the capture of an application's sound is going, into the log as it changes (a player
    // who hears nothing has this to send): whether it runs, why not, and whether any sound has
    // come of it.
    if (on && started && !device && !tube) {
        static std::string said_capture;
        const auto capture = app_audio::state();
        const auto name = others ? external_audio::app() : std::string("Spotify.exe");
        auto text = std::format("{}: {}{}; {}; its own output {}", name, capture.capturing ? "being captured" : "not being captured",
            capture.problem.empty() ? std::string() : " (" + capture.problem + ")", capture.level > 0.0005f ? "sound is coming of it" : "no sound has come of it yet",
            capture.muted ? "turned down" : "as it is");
        if (text != said_capture) {
            said_capture = text;
            try {
                logging::log(capture.capturing ? logging::Level::debug : logging::Level::warning, logging::Channel::music, "Sound through the game: {}.", text);
            } catch (...) {}
        }
    }
    // (What the game's sound files are, into the log while this is being worked out.)
    for (std::size_t i = 0; i < players.size() && said < 400; ++i) {
        auto& player = players[i];
        const auto requests = player.requests.load(std::memory_order_relaxed);
        if (!player.address.load(std::memory_order_relaxed) || requests == player.said.load(std::memory_order_relaxed) || !player.seconds.load(std::memory_order_relaxed)) continue;
        player.said.store(requests, std::memory_order_relaxed);
        ++said;
        try {
            char name[sizeof(player.name)];
            std::memcpy(name, player.name, sizeof(name));
            name[sizeof(name) - 1] = 0;
            logging::log(logging::Level::debug, logging::Channel::music, "Sound file player {}: \"{}\", {} channel(s) at {} Hz, {} s long{}.", i, name,
                player.channels.load(std::memory_order_relaxed), player.rate.load(std::memory_order_relaxed), player.seconds.load(std::memory_order_relaxed),
                spotify_station::active() ? " (the Spotify station is on)" : "");
        } catch (...) {}
    }
}

std::string status() {
    if (!hooked.load()) return "Spotify through the game: not available (the game's sound file player was not recognised).";
    const auto capture = app_audio::state();
    auto text = std::format("Spotify through the game: mode {}, gain {:.2f}; {} block(s) replaced ({} s); capture {}{}, {} reader(s), level {:.3f}, {} frame(s) short; Spotify's own output {}.",
        which.load(), loudness.load(), replaced_blocks.load(), replaced_frames.load() / 48000,
        capture.capturing ? "running" : "not running", capture.problem.empty() ? std::string() : " (" + capture.problem + ")", capture.readers, capture.level, capture.starved,
        capture.muted ? "turned down" : "as it is");
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto& player = players[i];
        if (!player.address.load() || !player.replaced.load()) continue;
        char name[sizeof(player.name)];
        std::memcpy(name, player.name, sizeof(name));
        name[sizeof(name) - 1] = 0;
        text += std::format("\n  replaced: player {} \"{}\", {} channel(s), {} s, {} block(s)", i, name, player.channels.load(), player.seconds.load(), player.replaced.load());
    }
    return text;
}
} // namespace dingosdk::music_inject
