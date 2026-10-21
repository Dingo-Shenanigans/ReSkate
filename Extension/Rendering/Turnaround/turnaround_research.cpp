#include "turnaround_research.h"
#include "turnaround_encode.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_skater.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/Customization/local_customization.h"
#include "Extension/Customization/local_customization_runtime.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

// Research aids for the board turnaround, run from the console (`turnaround ...`), with their
// answers in ReSkate.log. They find out what the turnaround needs from the game's Board screen:
// that its frames can be read back, which object is the board on show and what in it moves when
// the player spins it, and how the screen answers a right stick it did not get from a pad.
namespace dingosdk::turnaround::research {
namespace {
using logging::Channel;
using logging::Level;

std::uintptr_t game_base() noexcept { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

std::filesystem::path output_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) { path.resize(length); break; }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path() / L"Fit Checks";
}

std::string stamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// --- turnaround grab: the next presented frame, as the game drew it, saved as a PNG.
std::atomic<bool> grab_wanted{};
} // namespace

bool grab_requested() noexcept { return grab_wanted.load(); }

void take_grab(const overlay::GrabbedFrame& frame) noexcept {
    if (!grab_wanted.exchange(false)) return;
    if (!frame.data) {
        logging::log(Level::warning, Channel::graphics, "Turnaround grab: the frame could not be read back.");
        return;
    }
    try {
        Image image(frame.width, frame.height);
        for (std::uint32_t y = 0; y < frame.height; ++y)
            for (std::uint32_t x = 0; x < frame.width; ++x) {
                const auto* in = frame.data + std::size_t{y} * frame.pitch + std::size_t{x} * 4;
                auto* out = image.at(x, y);
                out[0] = frame.bgra ? in[2] : in[0];
                out[1] = in[1];
                out[2] = frame.bgra ? in[0] : in[2];
                out[3] = 255;
            }
        // Compressing a 4K frame takes a moment: not on the render thread.
        std::thread([image = std::move(image)] {
            try {
                const auto png = encode_png(image);
                const auto folder = output_folder();
                std::filesystem::create_directories(folder);
                const auto path = folder / ("grab-" + stamp() + ".png");
                std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
                logging::log(Level::info, Channel::graphics, "Turnaround grab: {}x{} saved to {}", image.width, image.height, path.string());
            } catch (const std::exception& error) {
                logging::log(Level::warning, Channel::graphics, "Turnaround grab: {}", error.what());
            }
        }).detach();
    } catch (const std::exception& error) {
        logging::log(Level::warning, Channel::graphics, "Turnaround grab: {}", error.what());
    }
}

void request_grab() noexcept { grab_wanted.store(true); }

namespace {
// --- turnaround boards: every skateboard entity in memory, and what in it changes while the
// player spins the board on show.

// One region of memory copied out, stopping at a fault (the region can go away while it is read).
bool copy_region(const void* from, void* to, std::size_t size) noexcept {
    __try {
        std::memcpy(to, from, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every 8-byte-aligned place in the process's private read/write memory holding `value`.
std::vector<std::uintptr_t> find_qword(std::uint64_t value, std::size_t most) {
    std::vector<std::uintptr_t> found;
    std::vector<std::uint8_t> buffer(1 << 20);
    MEMORY_BASIC_INFORMATION region{};
    for (std::uintptr_t at = 0x10000; at < memory::highest_user_address && found.size() < most;) {
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region))) break;
        const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress), end = start + region.RegionSize;
        const bool readable = region.State == MEM_COMMIT && region.Type == MEM_PRIVATE &&
                              (region.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(region.Protect & PAGE_GUARD);
        if (readable)
            for (auto chunk = start; chunk < end && found.size() < most; chunk += buffer.size()) {
                const auto size = static_cast<std::size_t>(std::min<std::uintptr_t>(buffer.size(), end - chunk));
                if (!copy_region(reinterpret_cast<const void*>(chunk), buffer.data(), size)) continue;
                for (std::size_t i = 0; i + 8 <= size; i += 8) {
                    std::uint64_t word;
                    std::memcpy(&word, buffer.data() + i, 8);
                    if (word == value) found.push_back(chunk + i);
                }
            }
        if (end <= at) break;
        at = end;
    }
    return found;
}

std::uintptr_t pointer_at(std::uintptr_t address) noexcept {
    std::uintptr_t value{};
    return memory::read(address, value) ? value : 0;
}

constexpr std::size_t watched_bytes = 0x200;
struct Watched {
    std::string name;
    std::uintptr_t address{};
    std::array<float, watched_bytes / 4> first{}, low{}, high{};
    std::array<unsigned, watched_bytes / 4> changes{};
};

std::string floats(const std::array<float, watched_bytes / 4>& values, std::size_t from, std::size_t count) {
    std::string text;
    for (std::size_t i = from; i < from + count && i < values.size(); ++i) text += std::format(" {:.4g}", values[i]);
    return text;
}

std::atomic<bool> boards_running{};

void watch_boards(double seconds) {
    try {
        const auto base = game_base();
        const auto entity_vtable = base + addr::engine::board_entity_vtable;
        auto entities = find_qword(entity_vtable, 64);
        logging::log(Level::info, Channel::customization, "Turnaround boards: {} skateboard entit{} in memory.",
                     entities.size(), entities.size() == 1 ? "y" : "ies");
        std::vector<Watched> watched;
        for (const auto entity : entities) {
            const auto holder = pointer_at(entity + 0xf0), parent = pointer_at(entity + 0x40);
            const auto transform = parent ? pointer_at(parent + 0x18) : 0;
            const auto collection = pointer_at(entity + 0x70);
            const bool holder_ok = holder && pointer_at(holder) == base + addr::engine::board_holder_vtable;
            const bool transform_ok = transform && pointer_at(transform) == base + addr::native_skater::blueprint_transform_vtable;
            logging::log(Level::info, Channel::customization,
                         "  board {:#x}: holder {:#x}{}, parent {:#x}, blueprint transform {:#x}{}, collection {:#x}{}",
                         entity, holder, holder_ok ? "" : " (not a board holder)", parent, transform,
                         transform_ok ? "" : " (unexpected type)", collection, pointer_at(collection) == entity ? "" : " (not its own)");
            const auto add = [&](std::string name, std::uintptr_t address) {
                if (!address) return;
                Watched w{std::move(name), address};
                if (!memory::read_bytes(address, w.first.data(), watched_bytes)) return;
                w.low = w.high = w.first;
                watched.push_back(std::move(w));
            };
            add(std::format("board {:#x}", entity), entity);
            if (transform_ok) add(std::format("board {:#x} blueprint transform", entity), transform);
            if (holder_ok) add(std::format("board {:#x} holder", entity), holder);
            if (collection && pointer_at(collection) == entity) add(std::format("board {:#x} collection", entity), collection);
        }
        logging::log(Level::info, Channel::customization, "Turnaround boards: spin the board on show now; watching for {:.0f} s.", seconds);
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        std::array<float, watched_bytes / 4> now{}, previous{};
        std::map<std::size_t, std::array<float, watched_bytes / 4>> last;
        for (std::size_t i = 0; i < watched.size(); ++i) last[i] = watched[i].first;
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            for (std::size_t i = 0; i < watched.size(); ++i) {
                auto& w = watched[i];
                if (!memory::read_bytes(w.address, now.data(), watched_bytes)) continue;
                previous = last[i];
                for (std::size_t k = 0; k < now.size(); ++k) {
                    if (!std::isfinite(now[k])) continue;
                    if (now[k] != previous[k]) ++w.changes[k];
                    w.low[k] = std::min(w.low[k], now[k]);
                    w.high[k] = std::max(w.high[k], now[k]);
                }
                last[i] = now;
            }
        }
        // What moved: each float that changed, how often and over what range; then the object's
        // first 0x100 bytes as floats, for the layout.
        for (std::size_t i = 0; i < watched.size(); ++i) {
            const auto& w = watched[i];
            std::string moved;
            for (std::size_t k = 0; k < w.changes.size(); ++k)
                if (w.changes[k])
                    moved += std::format("\n    +{:#05x}: {} change(s), {:.4g} .. {:.4g} (first {:.4g})", k * 4, w.changes[k], w.low[k], w.high[k], w.first[k]);
            logging::log(Level::info, Channel::customization, "  {} {:#x}:{}", w.name, w.address, moved.empty() ? " nothing changed" : moved);
            std::string dump;
            for (std::size_t row = 0; row < 16; ++row) dump += std::format("\n    +{:#05x}:{}", row * 16, floats(w.first, row * 4, 4));
            logging::log(Level::debug, Channel::customization, "  {} as floats:{}", w.name, dump);
        }
        logging::log(Level::info, Channel::customization, "Turnaround boards: done.");
    } catch (const std::exception& error) {
        logging::log(Level::warning, Channel::customization, "Turnaround boards: {}", error.what());
    } catch (...) {}
    boards_running.store(false);
}

// --- turnaround stick: the right stick pushed for a while, on top of what the pad says.
std::atomic<std::int16_t> stick_x{}, stick_y{};
std::atomic<std::uint64_t> stick_until{};
std::atomic<unsigned> stick_reads{};
void stick_filter(unsigned, overlay::GamepadValues& pad) noexcept {
    if (GetTickCount64() >= stick_until.load()) return;
    pad.right_x = stick_x.load();
    pad.right_y = stick_y.load();
    stick_reads.fetch_add(1);
}
} // namespace

std::string request_boards(double seconds) {
    seconds = std::clamp(seconds, 1.0, 30.0);
    if (boards_running.exchange(true)) return "Already watching; the answer goes to the log.";
    std::thread(watch_boards, seconds).detach();
    return std::format("Looking for skateboards in memory, then watching them for {:.0f} s: spin the board now. The answer goes to the log.", seconds);
}

std::filesystem::path folder() { return output_folder(); }

std::string request_parts() {
    // The game's skater presets as it holds them now (the loadout manager the saves read, see
    // local_customization_runtime.cpp): every item in them, its asset and its title.
    try {
        const auto base = game_base();
        std::uintptr_t manager{}, begin{}, end{};
        if (!memory::read(base + addr::engine::loadout_manager, manager) || !manager || !memory::read(manager + 0x18, begin) ||
            !memory::read(manager + 0x20, end) || begin > end || (end - begin) % 0x48 || (end - begin) / 0x48 > 10)
            return "The game's loadouts are not readable right now.";
        for (std::uintptr_t record = begin, index = 0; record < end; record += 0x48, ++index) {
            profile::CosmeticLoadout loadout;
            std::vector<profile_runtime::CosmeticNativeRecipe> native;
            if (!profile_runtime::read_cosmetic_recipes(reinterpret_cast<const void*>(record), loadout, native)) {
                logging::log(Level::info, Channel::customization, "Turnaround parts: loadout {} ({:#x}) unreadable.", index, record);
                continue;
            }
            std::string text;
            for (std::size_t r = 0; r < loadout.recipes.size(); ++r) {
                const auto& recipe = loadout.recipes[r];
                text += std::format("\n  recipe {} (template {:#x}):", r, recipe.template_key);
                for (const auto& item : recipe.items) {
                    const auto title = profile::item_display_metadata(item.asset, Json::object()).value("title", "");
                    text += std::format("\n    slot {:#010x} {} \"{}\"", item.slot, item.asset, title);
                }
            }
            logging::log(Level::info, Channel::customization, "Turnaround parts: loadout {} ({:#x}):{}", index, record, text);
        }
        return "The game's loadouts are in the log.";
    } catch (const std::exception& error) {
        return std::string("Turnaround parts: ") + error.what();
    }
}

std::string request_stick(double x, double y, double seconds) {
    seconds = std::clamp(seconds, 0.1, 30.0);
    const auto axis = [](double v) { return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * 32767)); };
    stick_x.store(axis(x));
    stick_y.store(axis(y));
    stick_reads.store(0);
    stick_until.store(GetTickCount64() + static_cast<std::uint64_t>(seconds * 1000));
    DingoSDKOverlaySetGamepadFilter(&stick_filter);
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds + 0.2));
        DingoSDKOverlaySetGamepadFilter(nullptr);
        logging::log(Level::info, Channel::input, "Turnaround stick: the game read the pushed stick {} time(s).", stick_reads.load());
    }).detach();
    return std::format("Right stick at ({:.2f}, {:.2f}) for {:.1f} s. Watch whether the board turns, and how fast; the count of reads goes to the log.", x, y, seconds);
}
} // namespace dingosdk::turnaround::research
