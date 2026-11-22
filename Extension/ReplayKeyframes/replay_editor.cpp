#include "replay_editor.h"
#include "replay_keyframes.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/replay_activity.h"
#include "Engine/Game/Build/20260929/replay_keyframes.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Rendering/replay_export.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace dingosdk::replay_editor {
namespace {
namespace md = multiplayer::menu_data;
namespace activity = addr::replay_activity;
namespace kf = addr::replay_keyframes;
using Address = std::uintptr_t;

Address peek_pointer(Address at) noexcept {
    Address value{};
    return memory::peek(at, value) ? value : 0;
}
bool matches(Address base, const game::build::Fingerprint& f) noexcept {
    std::array<unsigned char, 32> bytes{};
    return memory::peek(base + f.rva, bytes) && bytes == f.bytes;
}

// ---- The keyframe player's time (segment_eval, hooked) ------------------------------------------------------------------
// Each frame the replay's camera plays its keyframe segment at the replay's time, and that time goes as the keys' playback
// speed has it, slow motion and all, in exports too. Only with keyframes: with none, nothing is played.
using SegmentEval = bool (*)(Address camera, double time, Address segment, Address out);
std::atomic<SegmentEval> segment_original{};
std::atomic<int> played_hook{}; // 0 not tried, 1 hooked, 2 unavailable
std::atomic<double> played_time{};
std::atomic<std::uint64_t> played_at{};
std::atomic<int> played_streak{};
bool segment_eval(Address camera, double time, Address segment, Address out) {
    if (std::isfinite(time)) {
        // (One play a frame, each frame's on one job thread or another, one after the other.)
        played_streak.store(replay_keyframes::next_streak(played_streak.load(std::memory_order_relaxed), played_time.load(std::memory_order_relaxed), time),
                            std::memory_order_relaxed);
        played_time.store(time, std::memory_order_relaxed);
        played_at.store(GetTickCount64(), std::memory_order_release);
    }
    return segment_original.load(std::memory_order_acquire)(camera, time, segment, out);
}
void hook_player(Address base) noexcept {
    int idle = 0;
    if (!played_hook.compare_exchange_strong(idle, 2)) return;
    if (!matches(base, kf::segment_eval)) {
        logging::write(logging::Level::info, logging::Channel::graphics,
                       "Replay editor: the game's keyframe player is not the one known; exports are timed by their frames (slow motion not followed).");
        return;
    }
    auto* const target = reinterpret_cast<void*>(base + kf::segment_eval.rva);
    void* relay{};
    if (hook_prepare(target, reinterpret_cast<void*>(&segment_eval), &relay) != HookOk || !relay) return;
    segment_original.store(reinterpret_cast<SegmentEval>(relay), std::memory_order_release);
    HookBatch batch;
    if (const auto status = hook_enable(target); status != HookOk) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Replay editor: the keyframe player's hook did not go in ({}).",
                     hook_status_string(status));
        (void)hook_remove(target);
        return;
    }
    played_hook.store(1, std::memory_order_release);
}

// ---- What the client thread reads, and the export's timing ---------------------------------------------------------------
constexpr std::uint64_t search_every_ms = 2000;
struct Reader {
    std::mutex mutex;
    // The timeline's model (UI/Features/Social/Replay/Widgets/ReplayTimeline_Widget), kept as found.
    md::Handle timeline_handle{};
    Address timeline_type{};
    std::uint64_t next_search{}, next_read{};
    double length{};
    float trim_from{}, trim_to{1}, at{};
    bool timeline_read{};
    // The timeline as the editor shows it (kept while no video is exported), and the export timed by it.
    replay_keyframes::Timeline shown;
    std::uint64_t export_session{};
    replay_keyframes::Timeline exported;
    double export_clock{};
    std::uint64_t export_began{};
    bool export_played{};
    double export_held{};
    // Copies of the camera keys (copy_keys), by owner.
    struct Copy {
        std::uint64_t id{};
        struct Kept {
            std::int32_t session{};
            double time{};
            std::array<std::uint8_t, kf::value_bytes> value{};
        };
        std::vector<Kept> keys;
    };
    std::map<std::string, std::deque<Copy>, std::less<>> copies;
    std::uint64_t next_copy{1};
};
Reader& reader() {
    static auto* value = new Reader;
    return *value;
}

// The replay editor's timeline: a root of schema 0xd71ee0a1 lists widgets (0x61742cb4), each holding (0x716496c8, then
// 0x25e4d6c8) a reference (0xecfbf5f3) to its model. The timeline's has the replay's length in seconds (0x3e6bd625, a
// double) and three shares of it (floats): 0xadb99775 the trim's start, 0xa2df9b3b its end, 0x50b950bb the playhead
// (`ui dump`, 2026-11-16). Under the UI model's lock. False when it could not be read.
bool read_timeline(const md::Context& context, Reader& r, std::uint64_t now) {
    constexpr std::uint32_t root_schema = 0xd71ee0a1, widgets = 0x61742cb4, slot = 0x716496c8, binding = 0x25e4d6c8, reference = 0xecfbf5f3;
    constexpr std::uint32_t length = 0x3e6bd625, trim_from = 0xadb99775, trim_to = 0xa2df9b3b, at = 0x50b950bb;
    if (!r.timeline_handle || context.type_of(r.timeline_handle) != r.timeline_type) {
        r.timeline_handle = 0;
        if (now < r.next_search) return false;
        r.next_search = now + search_every_ms;
        for (const auto& root : context.roots({root_schema})) {
            const auto list = context.field(root.model, widgets);
            unsigned count{}, stride{};
            (void)context.array(list, 64, count, stride);
            for (unsigned i = 0; i < std::min(count, 64u) && !r.timeline_handle; ++i) {
                try {
                    const auto inner = context.field(context.field(context.element(list, i), slot), binding);
                    const auto ref = context.member(inner.type, reference);
                    const auto found = md::read<md::Handle>(context.address(inner) + ref.offset);
                    const auto found_type = found ? context.type_of(found) : 0;
                    if (!found_type) continue;
                    // The timeline's model is the one with a length (member throws for the others).
                    if (md::size(context.member(found_type, length).type) != sizeof(double)) continue;
                    r.timeline_handle = found;
                    r.timeline_type = found_type;
                } catch (...) {
                    // Another widget: passed over.
                }
            }
            if (r.timeline_handle) break;
        }
        if (!r.timeline_handle) return false;
    }
    const auto at_model = context.address(md::Value{r.timeline_handle, r.timeline_type});
    const auto field = [&](std::uint32_t hash, std::size_t bytes) {
        const auto f = context.member(r.timeline_type, hash);
        md::require(md::size(f.type) == bytes, "The replay timeline's model differs.");
        return at_model + f.offset;
    };
    const double seconds = md::read<double>(field(length, sizeof(double)));
    const float from = md::read<float>(field(trim_from, sizeof(float))), to = md::read<float>(field(trim_to, sizeof(float))),
                where = md::read<float>(field(at, sizeof(float)));
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 3600 || !std::isfinite(from) || !std::isfinite(to) || !std::isfinite(where)) return false;
    r.length = seconds;
    r.trim_from = std::clamp(from, 0.0f, 1.0f);
    r.trim_to = std::max(std::clamp(to, 0.0f, 1.0f), r.trim_from);
    r.at = where;
    return true;
}
// The timeline at most every 100 ms (the UI model's lock is the game's). Client thread.
void sample_timeline(Address base, Reader& r) {
    const auto now = GetTickCount64();
    if (now < r.next_read) return;
    r.next_read = now + 100;
    try {
        const auto ui = peek_pointer(base + addr::engine::ui_manager);
        const auto manager = ui ? peek_pointer(ui + 0x140) : 0;
        if (!manager || !game::native_data().models.value) {
            r.timeline_read = false;
            return;
        }
        const md::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        r.timeline_read = read_timeline(context, r, now);
    } catch (...) {
        r.timeline_handle = 0;
        r.timeline_read = false;
    }
}
// The replay's chunks, as the game strings its footage on the clock (none when they cannot be read).
std::vector<Chunk> read_chunks(Address replay) {
    std::vector<Chunk> chunks;
    const auto first = replay ? peek_pointer(replay + activity::chunks_begin) : 0, last = replay ? peek_pointer(replay + activity::chunks_end) : 0;
    if (!first || last <= first || (last - first) % activity::chunk_size || (last - first) / activity::chunk_size > 256) return chunks;
    for (auto at = first; at < last; at += activity::chunk_size) {
        Chunk c;
        if (!memory::peek(at, c.clock_from) || !memory::peek(at + 8, c.clock_to) || !memory::peek(at + 0x10, c.from) || !memory::peek(at + 0x14, c.to) ||
            !std::isfinite(c.clock_from) || !std::isfinite(c.clock_to) || !std::isfinite(c.from) || !std::isfinite(c.to) || c.clock_to < c.clock_from ||
            c.to < c.from)
            return {};
        chunks.push_back(c);
    }
    return chunks;
}
Address editor_replay(Address base) {
    const auto client = peek_pointer(base + activity::client);
    return client ? peek_pointer(client + activity::manager) : 0;
}

// ---- The camera's keyframes ----------------------------------------------------------------------------------------------
using AddFramingKey = bool (*)(void* manager, int session, double time, int camera, int target, float fov, float tilt, float roll, float x_offset,
                               float y_offset, float speed);
using RebuildSession = void (*)(void* manager, int session);
using KeyTime = double (*)(void* manager, double time);
std::atomic<int> keys_known{}; // 0 not checked, 1 known, 2 not this build's
bool keyframes_known(Address base) {
    if (const int k = keys_known.load(std::memory_order_acquire)) return k == 1;
    // (The rebuild is ReSkate's own keyframe smoothing's to hook, which checked it first: its first bytes are then a jump.)
    const bool ok = matches(base, kf::add_framing_key) && matches(base, kf::fill_framing_key) && matches(base, kf::key_time) &&
                    (replay_keyframes::available() || matches(base, kf::session_rebuild));
    keys_known.store(ok ? 1 : 2, std::memory_order_release);
    if (!ok)
        logging::write(logging::Level::info, logging::Channel::graphics,
                       "Replay editor: the game's keyframe functions are not the ones known; plugins cannot copy and put back camera keyframes.");
    return ok;
}
// The next node of an EASTL map (right at +0, left at +8, parent at +16), as the game steps through one.
Address map_next(Address node) {
    if (const auto right = peek_pointer(node)) {
        node = right;
        for (int guard = 0; guard < 64; ++guard) {
            const auto left = peek_pointer(node + 8);
            if (!left) break;
            node = left;
        }
        return node;
    }
    auto parent = peek_pointer(node + 16);
    for (int guard = 0; parent && node == peek_pointer(parent) && guard < 64; ++guard) {
        node = parent;
        parent = peek_pointer(parent + 16);
    }
    if (peek_pointer(node) != parent) node = parent;
    return node;
}
// Every framing key of every session, as the manager has them now: visit(session, node of the key). False when it has no
// session (the editor is not up, or closing) or the walk went wrong.
template <class Visit>
bool walk_keys(Address manager, Visit visit) {
    const auto anchor = manager + kf::manager_sessions;
    std::uint64_t sessions{};
    if (!memory::peek(anchor + kf::map_size_after_anchor, sessions) || !sessions || sessions > 64) return false;
    int n = 0;
    for (auto node = peek_pointer(anchor + 8); node && node != anchor && n < 64; node = map_next(node), ++n) {
        std::int32_t session{};
        if (!memory::peek(node + kf::session_id, session)) return false;
        const auto keys = node + kf::session_framing_keys;
        int k = 0;
        for (auto at = peek_pointer(keys + 8); at && at != keys && k < 512; at = map_next(at), ++k)
            if (!visit(session, at)) return false;
        if (k >= 512) return false;
    }
    return n < 64;
}
// The node of a session's framing key nearest a time (within a frame or so), or 0.
Address key_node(Address manager, std::int32_t session, double time) {
    Address best{};
    double gap = 1.0 / 40.0;
    (void)walk_keys(manager, [&](std::int32_t s, Address at) {
        double at_time{};
        if (s == session && memory::peek(at + kf::framing_key_time, at_time) && std::abs(at_time - time) < gap) {
            gap = std::abs(at_time - time);
            best = at;
        }
        return true;
    });
    return best;
}
bool poke_bytes(Address address, const void* source, std::size_t size) noexcept {
    if (!source || !size || address < 0x10000 || address > memory::highest_user_address - size) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_add(Address function, Address manager, std::int32_t session, double time, std::int32_t camera, float fov, float speed) noexcept {
    __try {
        (void)reinterpret_cast<AddFramingKey>(function)(reinterpret_cast<void*>(manager), session, time, camera, 0, fov, 0, 0, 0, 0, speed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_key_time(Address function, Address manager, double time, double& out) noexcept {
    __try {
        out = reinterpret_cast<KeyTime>(function)(reinterpret_cast<void*>(manager), time);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_rebuild(Address function, Address manager, std::int32_t session) noexcept {
    __try {
        reinterpret_cast<RebuildSession>(function)(reinterpret_cast<void*>(manager), session);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
Address keyframe_manager(Address base) { return base && keyframes_known(base) ? peek_pointer(base + kf::manager) : 0; }
} // namespace

std::optional<State> state(Address base) noexcept {
    try {
        if (!base || !multiplayer::replay_editor_open(base)) return std::nullopt;
        hook_player(base);
        auto& r = reader();
        std::lock_guard lock(r.mutex);
        State s;
        std::uint8_t lane{};
        double playhead{};
        if (memory::peek(base + activity::playing, lane) && lane && memory::peek(base + activity::playhead, playhead) && std::isfinite(playhead))
            s.playhead = playhead;
        const auto replay = editor_replay(base);
        double from{}, to{};
        if (replay && memory::peek(replay + activity::replay_from, from) && memory::peek(replay + activity::replay_to, to) && std::isfinite(from) &&
            std::isfinite(to) && to > from)
            s.from = from, s.to = to;
        sample_timeline(base, r);
        s.timeline.chunks = read_chunks(replay);
        if (r.timeline_read && lane) {
            s.timeline.known = true;
            s.timeline.length = r.length;
            s.timeline.start = s.playhead - static_cast<double>(r.at) * r.length;
            s.timeline.trim_from = r.trim_from;
            s.timeline.trim_to = r.trim_to;
            s.at = r.at;
            // Trimmed, the bar is the trimmed part (Advanced Edit's tabs, the export's) or the whole replay (its Trim tab):
            // whichever puts the playhead where the timeline has it. The trim and the chunks are shares of the whole.
            if (r.trim_to - r.trim_from > 1e-4f && (r.trim_from > 1e-4f || r.trim_to < 1.0f - 1e-4f) && !s.timeline.chunks.empty()) {
                replay_keyframes::Timeline whole = s.timeline;
                whole.shown_from = 0, whole.shown_to = 1;
                const float share = replay_keyframes::share_of(whole, s.playhead);
                if (share >= 0) {
                    const float as_trimmed = (share - r.trim_from) / (r.trim_to - r.trim_from);
                    if (std::abs(as_trimmed - r.at) < std::abs(share - r.at)) s.timeline.shown_from = r.trim_from, s.timeline.shown_to = r.trim_to;
                }
            }
        }
        s.ui_hidden = multiplayer::sample_game_ui_state(base).ui_hidden;
        const auto now = GetTickCount64();
        const auto played = played_at.load(std::memory_order_acquire);
        s.keys_playing = played_hook.load(std::memory_order_acquire) == 1 && played && now - played < 250;
        // The export, from the frames its encoder takes.
        const auto running = replay_export::running();
        s.time = s.playhead;
        if (running.running) {
            s.exporting = true;
            s.frame = running.frames;
            s.fps = running.fps > 1.0f ? running.fps : 60.0f;
            if (running.session != r.export_session) {
                // A new export: the timeline as the editor showed it, and the clock where it stood. While a video is
                // exported the clock stands where the editor's playhead was when it began, wherever that was; the export
                // runs from the trim's start.
                r.export_session = running.session;
                r.exported = r.shown;
                r.export_clock = s.playhead;
                r.export_began = running.began_ms;
                r.export_played = false;
            }
            // As the game plays the camera's keyframes in this export, once its plays run on (not the export's odd first one),
            // whatever the clock says; held through a stall once taken. Without keyframes, by its frames from the trim's start.
            if (s.keys_playing && played >= r.export_began && played_streak.load(std::memory_order_relaxed) >= 2) {
                r.export_played = true;
                r.export_held = played_time.load(std::memory_order_relaxed);
            }
            s.time = r.export_played ? r.export_held
                                     : replay_keyframes::time_by_frames(r.exported, r.export_clock, static_cast<double>(running.frames) / s.fps);
        } else if (s.timeline.known) {
            r.shown = s.timeline;
        }
        return s;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Key> keys(Address base) noexcept {
    std::vector<Key> out;
    try {
        if (!base || !multiplayer::replay_editor_open(base)) return out;
        const auto manager = keyframe_manager(base);
        if (!manager) return out;
        const bool ok = walk_keys(manager, [&](std::int32_t session, Address at) {
            Key key;
            key.session = session;
            std::array<std::uint8_t, kf::value_bytes> value{};
            if (!memory::peek(at + kf::framing_key_time, key.time) || !std::isfinite(key.time) ||
                !memory::peek_bytes(at + kf::framing_key_value, value.data(), value.size()))
                return false;
            std::memcpy(&key.camera, value.data() + kf::value_camera, sizeof(key.camera));
            std::memcpy(&key.fov, value.data() + kf::value_fov, sizeof(key.fov));
            std::memcpy(&key.speed, value.data() + kf::value_speed, sizeof(key.speed));
            out.push_back(key);
            return out.size() < 4096;
        });
        if (!ok) out.clear();
        std::sort(out.begin(), out.end(), [](const Key& a, const Key& b) { return a.session != b.session ? a.session < b.session : a.time < b.time; });
    } catch (...) {
        out.clear();
    }
    return out;
}

std::uint64_t copy_keys(Address base, std::string_view owner) noexcept {
    try {
        if (!base || !multiplayer::replay_editor_open(base)) return 0;
        const auto manager = keyframe_manager(base);
        if (!manager) return 0;
        Reader::Copy copy;
        const bool ok = walk_keys(manager, [&](std::int32_t session, Address at) {
            Reader::Copy::Kept kept;
            kept.session = session;
            if (!memory::peek(at + kf::framing_key_time, kept.time) || !std::isfinite(kept.time) ||
                !memory::peek_bytes(at + kf::framing_key_value, kept.value.data(), kept.value.size()))
                return false;
            copy.keys.push_back(kept);
            return copy.keys.size() < 4096;
        });
        if (!ok || copy.keys.empty()) return 0;
        auto& r = reader();
        std::lock_guard lock(r.mutex);
        copy.id = r.next_copy++;
        auto& kept = r.copies[std::string(owner)];
        if (kept.size() >= 8) kept.pop_front();
        kept.push_back(std::move(copy));
        return kept.back().id;
    } catch (...) {
        return 0;
    }
}

std::variant<std::size_t, std::string> put_keys(Address base, std::string_view owner, std::uint64_t id) noexcept {
    try {
        if (!base || !multiplayer::replay_editor_open(base)) return std::string("not in the replay editor");
        const auto manager = keyframe_manager(base);
        if (!manager) return std::string("this game build's keyframes are not the ones known");
        std::vector<Reader::Copy::Kept> kept;
        {
            auto& r = reader();
            std::lock_guard lock(r.mutex);
            const auto found = r.copies.find(owner);
            if (found != r.copies.end())
                for (const auto& copy : found->second)
                    if (copy.id == id) kept = copy.keys;
        }
        if (kept.empty()) return std::string("no such copy of this plugin's");
        // Only into sessions with no keys of their own.
        std::set<std::int32_t> keyed;
        if (!walk_keys(manager, [&](std::int32_t session, Address) {
                keyed.insert(session);
                return true;
            }))
            return std::string("the editor's keyframes are not up yet");
        // What the editor shows: the replay's footage, within what a key can be put at now (its trim).
        double from{}, to{}, first{}, last{};
        const auto replay = editor_replay(base);
        const auto at_time = base + kf::key_time.rva;
        if (!replay || !memory::peek(replay + activity::replay_from, from) || !memory::peek(replay + activity::replay_to, to) ||
            !call_key_time(at_time, manager, -1.0e7, first) || !call_key_time(at_time, manager, 1.0e7, last) || !std::isfinite(first) ||
            !std::isfinite(last) || last - first < 0.25)
            return std::string("the editor's range is not up yet");
        from = std::max(from, first);
        to = std::min(to, last);
        const auto add = base + kf::add_framing_key.rva, rebuild = base + kf::session_rebuild.rva;
        std::set<std::int32_t> sessions;
        std::size_t put{};
        for (const auto& key : kept) {
            if (keyed.contains(key.session)) continue;
            double lands{};
            if (key.time < from - 1e-3 || key.time > to + 1e-3 || !call_key_time(at_time, manager, key.time, lands) || std::abs(lands - key.time) > 1.0 / 40.0)
                continue;
            std::int32_t camera{};
            float fov{}, speed{};
            std::memcpy(&camera, key.value.data() + kf::value_camera, sizeof(camera));
            std::memcpy(&fov, key.value.data() + kf::value_fov, sizeof(fov));
            std::memcpy(&speed, key.value.data() + kf::value_speed, sizeof(speed));
            if (camera < 0 || camera > 16) camera = 0;
            if (!std::isfinite(fov) || fov < 1 || fov > 179) fov = 65;
            if (!std::isfinite(speed) || speed <= 0 || speed > 100) speed = 1;
            const auto node = call_add(add, manager, key.session, key.time, camera, fov, speed) ? key_node(manager, key.session, key.time) : 0;
            double node_time{};
            if (!node || !memory::peek(node + kf::framing_key_time, node_time)) continue;
            auto value = key.value;
            std::memcpy(value.data() + kf::value_time, &node_time, sizeof(node_time));
            if (!poke_bytes(node + kf::framing_key_value, value.data(), value.size())) continue;
            sessions.insert(key.session);
            ++put;
        }
        for (const auto session : sessions) (void)call_rebuild(rebuild, manager, session);
        if (!put) return std::string("none of its keys are on footage the replay still has, in sessions with no keys");
        return put;
    } catch (...) {
        return std::string("they could not be put back");
    }
}

void forget(std::string_view owner) noexcept {
    try {
        auto& r = reader();
        std::lock_guard lock(r.mutex);
        if (const auto found = r.copies.find(owner); found != r.copies.end()) r.copies.erase(found);
    } catch (...) {}
}

std::optional<Skater> skater(Address base) noexcept {
    try {
        if (!base || !multiplayer::replay_editor_open(base)) return std::nullopt;
        const auto manager = keyframe_manager(base);
        if (!manager) return std::nullopt;
        std::array<float, 4> body{}, upper{}, board{};
        if (!memory::peek(manager + kf::manager_skater_body, body) || !memory::peek(manager + kf::manager_skater_upper, upper) ||
            !memory::peek(manager + kf::manager_skater_board, board))
            return std::nullopt;
        for (const auto* v : {&body, &upper, &board})
            for (std::size_t i = 0; i < 3; ++i)
                if (!std::isfinite((*v)[i]) || std::abs((*v)[i]) > 1e6f) return std::nullopt;
        if (body[0] == 0 && body[1] == 0 && body[2] == 0) return std::nullopt;
        return Skater{{body[0], body[1], body[2]}, {upper[0], upper[1], upper[2]}, {board[0], board[1], board[2]}};
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace dingosdk::replay_editor
