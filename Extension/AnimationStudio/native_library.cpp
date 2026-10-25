#include "native_library.h"
#include "Engine/Game/Build/20260929/skater_skeleton.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace dingosdk::anim_studio::native_library {
namespace {
namespace fb = dingosdk::frostbite;
namespace ebx = fb::ebx;

void require(bool test, const char* message) {
    if (!test) throw Unsupported(message);
}
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// ---- little-endian and big-endian reads out of a resource, bounds checked ----
struct Reader {
    std::span<const std::byte> blob;
    template<class T> T le(std::size_t at) const {
        require(at + sizeof(T) <= blob.size(), "Read past the end of a native resource");
        T value{};
        std::memcpy(&value, blob.data() + at, sizeof(T));
        return value;
    }
    std::uint16_t be16(std::size_t at) const {
        require(at + 2 <= blob.size(), "Read past the end of a native resource");
        return static_cast<std::uint16_t>((std::to_integer<unsigned>(blob[at]) << 8) | std::to_integer<unsigned>(blob[at + 1]));
    }
    std::uint8_t u8(std::size_t at) const {
        require(at < blob.size(), "Read past the end of a native resource");
        return std::to_integer<std::uint8_t>(blob[at]);
    }
};
// A track's key times: one byte each (time codec 0) or big-endian u16 (time codec 1), strictly rising.
std::vector<double> key_times(const Reader& r, std::size_t at, std::uint32_t n, std::uint32_t time_codec, const char* what) {
    std::vector<double> t(n);
    for (std::uint32_t i = 0; i < n; ++i) t[i] = time_codec == 0 ? r.u8(at + i) : r.be16(at + 2 * std::size_t{i});
    for (std::uint32_t i = 1; i < n; ++i) require(t[i] > t[i - 1], what);
    return t;
}
bool ends_with(std::string_view text, std::string_view tail) { return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail; }
} // namespace

// ---- decoding ---------------------------------------------------------------------------------------
std::vector<std::pair<std::string, QuaternionTrack>> decode_quaternions(std::span<const std::byte> blob, const std::vector<std::string>& channels) {
    const Reader r{blob};
    require(blob.size() >= 96, "Truncated quaternion resource");
    const auto nq = r.le<std::uint32_t>(0);
    const auto offset = r.le<std::uint64_t>(4);
    const auto nv = r.le<std::uint32_t>(12), nf = r.le<std::uint32_t>(24);
    const auto codec = r.le<std::uint32_t>(36), time_codec = r.le<std::uint32_t>(48);
    const auto size = r.le<std::uint32_t>(56);
    const auto data = r.le<std::uint64_t>(60);
    require(channels.size() == std::size_t{nq} + nv + nf && (codec == 1 || codec == 2) && time_codec <= 1, "Unsupported quaternion codec/inventory");
    require(80 <= offset && offset + 12ull * nq <= data && data + size <= blob.size() - 16, "Invalid quaternion descriptor bounds");
    std::vector<std::pair<std::string, QuaternionTrack>> out;
    std::set<std::string> seen;
    const double root2 = std::numbers::sqrt2;
    for (std::uint32_t i = 0; i < nq; ++i) {
        const auto& name = channels[i];
        require(ends_with(name, ".q") && seen.insert(name).second, "Invalid quaternion binding");
        const std::size_t at = offset + 12ull * i;
        const auto n = r.le<std::uint32_t>(at), times = r.le<std::uint32_t>(at + 4), values = r.le<std::uint32_t>(at + 8);
        require(n >= 1 && n <= 20000, "Invalid quaternion key count");
        QuaternionTrack track;
        std::vector<std::array<double, 3>> v(n);
        std::vector<int> missing(n);
        const auto unpack = [&](std::uint64_t packed, std::size_t k) {
            missing[k] = static_cast<int>(packed & 3);
            constexpr std::array<std::pair<int, int>, 3> layout{{{43, 21}, {22, 21}, {2, 20}}};
            for (std::size_t c = 0; c < 3; ++c) {
                const auto [shift, bits] = layout[c];
                const auto raw = (packed >> shift) & ((std::uint64_t{1} << bits) - 1);
                v[k][c] = (static_cast<double>(raw) / static_cast<double>(std::uint64_t{1} << (bits - 1)) - 1) / root2;
            }
        };
        if (n == 1) {
            unpack(r.le<std::uint64_t>(at + 4), 0);
            track.frames = {0.0};
        } else {
            const std::size_t width = time_codec == 0 ? 1 : 2;
            require(times + width * n <= size && values + std::size_t{codec == 1 ? 6u : 8u} * n <= size, "Quaternion keys outside resource");
            track.frames = key_times(r, data + times, n, time_codec, "Unordered quaternion keys");
            for (std::uint32_t k = 0; k < n; ++k) {
                if (codec == 1) {
                    const std::size_t base = data + values + 6ull * k;
                    std::array<std::uint32_t, 3> u{r.be16(base), r.be16(base + 2), r.be16(base + 4)};
                    missing[k] = static_cast<int>((u[0] & 1) * 2 + (u[1] & 1));
                    u[0] &= ~1u;
                    u[1] &= ~1u;
                    for (std::size_t c = 0; c < 3; ++c) v[k][c] = (static_cast<double>(u[c]) - 32768) / (32768 * root2);
                } else unpack(r.le<std::uint64_t>(data + values + 8ull * k), k);
            }
        }
        track.values.resize(n);
        for (std::uint32_t k = 0; k < n; ++k) {
            const double square = v[k][0] * v[k][0] + v[k][1] * v[k][1] + v[k][2] * v[k][2];
            require(square <= 1 + 1e-6, "Invalid packed quaternion");
            auto& q = track.values[k];
            std::size_t c = 0;
            for (int slot = 0; slot < 4; ++slot) {
                if (slot == missing[k]) q[static_cast<std::size_t>(slot)] = std::sqrt(std::max(0.0, 1 - square));
                else q[static_cast<std::size_t>(slot)] = v[k][c++];
            }
        }
        out.emplace_back(name, std::move(track));
    }
    return out;
}

std::vector<std::pair<std::string, VectorTrack>> decode_vectors(std::span<const std::byte> blob, const std::vector<std::string>& channels) {
    const Reader r{blob};
    require(blob.size() >= 96, "Truncated PIEC resource");
    const auto nq = r.le<std::uint32_t>(0);
    const auto count = r.le<std::uint32_t>(12);
    const auto offset = r.le<std::uint64_t>(16);
    const auto nf = r.le<std::uint32_t>(24);
    require(channels.size() == std::size_t{nq} + count + nf, "Native channel inventory mismatch");
    const auto time_codec = r.le<std::uint32_t>(48);
    require(r.le<std::uint32_t>(40) == 1 && time_codec <= 1, "Unsupported vector/time codec");
    const auto size = r.le<std::uint32_t>(56);
    const auto data = r.le<std::uint64_t>(60);
    require(80 <= offset && offset <= data && offset + 48ull * count <= data && data + size <= blob.size() - 16, "Invalid PIEC descriptor/data bounds");
    std::vector<std::pair<std::string, VectorTrack>> out;
    std::set<std::string> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& name = channels[std::size_t{nq} + i];
        require((ends_with(name, ".t") || ends_with(name, ".s")) && seen.insert(name).second, "Invalid vector binding");
        const std::size_t pos = offset + 48ull * i;
        const auto n = r.le<std::uint32_t>(pos), times = r.le<std::uint32_t>(pos + 4), values = r.le<std::uint32_t>(pos + 8);
        std::array<double, 3> minimum{}, extent{};
        for (std::size_t c = 0; c < 3; ++c) {
            minimum[c] = r.le<float>(pos + 16 + 4 * c);
            extent[c] = r.le<float>(pos + 32 + 4 * c);
        }
        const bool finite = std::all_of(minimum.begin(), minimum.end(), [](double x) { return std::isfinite(x); }) &&
                            std::all_of(extent.begin(), extent.end(), [](double x) { return std::isfinite(x); });
        VectorTrack track;
        if (n == 2) {
            // Two keys kept in the descriptor itself: times in place of the offsets, the values in min and extent.
            require(times < values && finite, "Unmeasured short vector encoding; refusing to guess");
            track.frames = {static_cast<double>(times), static_cast<double>(values)};
            track.values = {minimum, extent};
        } else {
            require(n >= 3 && n <= 20000, "Invalid vector key count");
            const std::size_t width = time_codec == 0 ? 1 : 2;
            require(times + width * n <= size && values + 6ull * n <= size, "Vector keys outside resource");
            track.frames = key_times(r, data + times, n, time_codec, "Unordered vector keys");
            require(finite && std::all_of(extent.begin(), extent.end(), [](double x) { return x >= 0; }), "Invalid vector quantization bounds");
            track.values.resize(n);
            for (std::uint32_t k = 0; k < n; ++k)
                for (std::size_t c = 0; c < 3; ++c)
                    track.values[k][c] = minimum[c] + (static_cast<double>(r.be16(data + values + 6ull * k + 2 * c)) / 65535) * extent[c];
        }
        out.emplace_back(name, std::move(track));
    }
    return out;
}

namespace {
// numpy's interp: linear between keys, held at either end.
double interp(double x, const std::vector<double>& xs, const std::vector<std::array<double, 3>>& ys, std::size_t axis) {
    if (x <= xs.front()) return ys.front()[axis];
    if (x >= xs.back()) return ys.back()[axis];
    const auto hi = static_cast<std::size_t>(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin());
    const auto lo = hi - 1;
    const double u = (x - xs[lo]) / (xs[hi] - xs[lo]);
    return ys[lo][axis] + u * (ys[hi][axis] - ys[lo][axis]);
}
// native_piecewise.sample_quaternions: slerp between the keys either side, the shorter way, normalised.
std::array<double, 4> sample(const QuaternionTrack& track, double frame) {
    const auto& t = track.frames;
    const auto n = static_cast<std::ptrdiff_t>(t.size());
    std::ptrdiff_t lo = static_cast<std::ptrdiff_t>(std::upper_bound(t.begin(), t.end(), frame) - t.begin()) - 1;
    lo = std::clamp<std::ptrdiff_t>(lo, 0, n - 1);
    const std::ptrdiff_t hi = std::min(lo + 1, n - 1);
    const double span = t[static_cast<std::size_t>(hi)] - t[static_cast<std::size_t>(lo)];
    const double u = std::clamp(span != 0 ? (frame - t[static_cast<std::size_t>(lo)]) / span : 0.0, 0.0, 1.0);
    const auto a = track.values[static_cast<std::size_t>(lo)];
    auto b = track.values[static_cast<std::size_t>(hi)];
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0) for (auto& c : b) c = -c;
    const double angle = std::acos(std::clamp(std::abs(dot), 0.0, 1.0)), sine = std::sin(angle);
    const double wa = sine > 1e-8 ? std::sin((1 - u) * angle) / sine : 1 - u;
    const double wb = sine > 1e-8 ? std::sin(u * angle) / sine : u;
    std::array<double, 4> q{};
    for (std::size_t c = 0; c < 4; ++c) q[c] = a[c] * wa + b[c] * wb;
    const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (auto& c : q) c /= length;
    return q;
}
} // namespace

Decoded decode_pose(std::span<const std::byte> blob, const std::vector<std::string>& channels, const std::vector<Joint10>& defaults,
                    const std::vector<std::string>& names, std::int64_t end_frame) {
    require(end_frame >= 0 && end_frame < 20000, "Unsupported native frame range");
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < names.size(); ++i) index.emplace(names[i], i);
    const auto ticks = static_cast<std::size_t>(end_frame) + 1;
    Decoded out;
    out.frames.resize(ticks * names.size());
    for (std::size_t f = 0; f < ticks; ++f) std::copy(defaults.begin(), defaults.end(), out.frames.begin() + static_cast<std::ptrdiff_t>(f * names.size()));
    const auto quaternions = decode_quaternions(blob, channels);
    const auto vectors = decode_vectors(blob, channels);
    std::size_t mapped = 0;
    for (const auto& [name, track] : quaternions) {
        const auto found = index.find(name.substr(0, name.size() - 2));
        if (found == index.end()) continue;
        for (std::size_t f = 0; f < ticks; ++f) {
            const auto q = sample(track, static_cast<double>(f));
            auto& joint = out.frames[f * names.size() + found->second];
            for (std::size_t c = 0; c < 4; ++c) joint[3 + c] = static_cast<float>(q[c]);
        }
        ++mapped;
    }
    for (const auto& [name, track] : vectors) {
        const auto found = index.find(name.substr(0, name.size() - 2));
        if (found == index.end()) continue;
        const std::size_t start = ends_with(name, ".t") ? 7 : 0;
        for (std::size_t f = 0; f < ticks; ++f) {
            auto& joint = out.frames[f * names.size() + found->second];
            for (std::size_t c = 0; c < 3; ++c) joint[start + c] = static_cast<float>(interp(static_cast<double>(f), track.frames, track.values, c));
        }
        ++mapped;
    }
    require(mapped > 0, "No mapped skater pose channels");
    for (const auto& joint : out.frames) {
        require(std::all_of(joint.begin(), joint.end(), [](float x) { return std::isfinite(x); }) && joint[0] > 0 && joint[1] > 0 && joint[2] > 0,
                "Invalid decoded pose");
    }
    out.times.resize(ticks);
    for (std::size_t f = 0; f < ticks; ++f) out.times[f] = static_cast<float>(static_cast<double>(f) / 60);
    out.board = std::any_of(channels.begin(), channels.end(), [](const std::string& c) { return c.starts_with("Skateboard_Root."); });
    return out;
}

std::vector<std::byte> encode_clip(const Decoded& clip, float takeoff, float landing) {
    const std::size_t n = static_cast<std::size_t>(joint_count), frames = clip.times.size();
    if (frames == 0 || clip.frames.size() != frames * n) throw std::runtime_error("A clip's poses do not fill its frames");
    std::vector<std::uint8_t> moves(n);
    for (std::size_t j = 0; j < n; ++j)
        for (std::size_t f = 1; f < frames && !moves[j]; ++f) moves[j] = clip.frames[f * n + j] != clip.frames[j];
    std::vector<std::byte> out;
    const auto put = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::byte*>(data);
        out.insert(out.end(), bytes, bytes + size);
    };
    put("FAS3", 4);
    const auto joints = static_cast<std::uint32_t>(n), count = static_cast<std::uint32_t>(frames);
    put(&joints, 4);
    put(&count, 4);
    put(&takeoff, 4);
    put(&landing, 4);
    const std::uint8_t board = clip.board ? 1 : 0;
    put(&board, 1);
    put(clip.times.data(), frames * 4);
    put(moves.data(), n);
    for (std::size_t j = 0; j < n; ++j)
        if (!moves[j]) put(clip.frames[j].data(), 40);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t j = 0; j < n; ++j)
            if (moves[j]) put(clip.frames[f * n + j].data(), 40);
        if (clip.board) put(clip.frames[f * n + static_cast<std::size_t>(board_joint_index)].data(), 40);
    }
    return out;
}

std::pair<float, float> trick_marks(const Decoded& clip) {
    const std::size_t n = static_cast<std::size_t>(joint_count), frames = clip.times.size();
    const auto board = [&](std::size_t f) -> const Joint10& { return clip.frames[f * n + static_cast<std::size_t>(board_joint_index)]; };
    const float duration = clip.times.back() - clip.times.front();
    // anim_core.cpp place_native_marks, as the studio runs it when a game clip loads.
    const auto place = [&](float takeoff, float landing) {
        if (frames < 8) return std::pair{takeoff, landing};
        const auto& rest = board(0);
        for (std::size_t i = 1; i < frames; ++i) {
            const auto& q = board(i);
            const float d = std::abs(rest[3] * q[3] + rest[4] * q[4] + rest[5] * q[5] + rest[6] * q[6]);
            if (!std::isfinite(d) || 2 * std::acos(std::clamp(d, 0.0f, 1.0f)) * 57.2957795f <= 6.0f) continue;
            const float pop = clip.times[i] + 0.05f;
            if (pop < takeoff - 0.04f && pop < duration - 0.3f) takeoff = pop;
            break;
        }
        std::vector<float> smooth(frames, 0.0f);
        for (std::size_t i = 2; i + 2 < frames; ++i) {
            float sum = 0;
            for (std::size_t j = i - 2; j <= i + 2; ++j) sum += board(j)[8];
            smooth[i] = sum / 5;
        }
        std::optional<std::size_t> deepest;
        for (std::size_t i = 3; i + 3 < frames; ++i) {
            if (clip.times[i] < takeoff + 0.25f - 1e-3f || clip.times[i] > takeoff + 0.85f + 1e-3f) continue;
            if (!(smooth[i] <= smooth[i - 1] && smooth[i] < smooth[i + 1])) continue;
            if (!deepest || smooth[i] < smooth[*deepest]) deepest = i;
        }
        if (deepest && std::isfinite(smooth[*deepest]) && (landing < 0 || std::abs(clip.times[*deepest] - landing) > 0.05f)) landing = clip.times[*deepest];
        return std::pair{takeoff, landing};
    };
    auto marks = place(duration, -1.0f);
    if (marks.first < duration - 1e-6f) return marks;
    // The board never tips: make_trick_library.py's first low of its height (rounded: flat stretches stay flat).
    std::vector<double> y(frames), smooth(frames);
    for (std::size_t f = 0; f < frames; ++f) y[f] = std::round(static_cast<double>(board(f)[8]) * 1e5) / 1e5;
    for (std::size_t i = 0; i < frames; ++i) {
        double sum = 0;
        for (int k = -2; k <= 2; ++k) {
            const auto at = static_cast<std::ptrdiff_t>(i) + k;
            if (at >= 0 && at < static_cast<std::ptrdiff_t>(frames)) sum += y[static_cast<std::size_t>(at)];
        }
        smooth[i] = sum / 5;
    }
    std::size_t takeoff = 0;
    for (std::size_t i = 2; i + 2 < frames; ++i)
        if (smooth[i] <= smooth[i - 1] && smooth[i] < smooth[i + 1]) {
            takeoff = i;
            break;
        }
    return place(clip.times[takeoff], -1.0f);
}

// ---- where an asset goes ---------------------------------------------------------------------------
namespace {
bool has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }
std::pair<std::string, std::string> classify(const std::string& name) {
    const auto n = lower(name);
    const auto any = [&](std::initializer_list<std::string_view> parts) {
        return std::any_of(parts.begin(), parts.end(), [&](std::string_view p) { return has(n, p); });
    };
    if (any({"bool.", "float.", "enum.", "enumgs.", "int."})) return {"Graph/Parameters", "parameter candidate"};
    if (any({"transition", "_to_"})) return {"Graph/Transitions", "transition candidate"};
    if (any({".node", ".conduit"})) return {"Graph/States", "state candidate"};
    if (any({"superlayer", "layer"})) return {"Graph/Layers", "layer candidate"};
    const std::string kind = n.starts_with("c_") ? "clip candidate" : "controller or other asset";
    if (has(n, "skatepedia")) return {has(n, "grind") ? "Skatepedia/Grinds" : "Skatepedia/Other", kind};
    if (any({"grind", "slide"})) {
        const std::string phase = any({"lockin", "lock_in", "entry", "_into"}) ? "Lock-in" : has(n, "loop") ? "Loops"
                                  : any({"exit", "popout", "pop_out"})            ? "Exits"
                                                                                  : "Other";
        return {"On board/Grinds/" + phase, kind};
    }
    if (any({"trick", "ollie", "flip"})) return {has(n, "_n_") || has(n, "nollie") ? "On board/Tricks/Nollie" : "On board/Tricks/Other", kind};
    if (has(n, "manual")) return {"On board/Manuals", kind};
    if (any({"ofb", "offboard", "off_board"})) return {"Off board/Movements", kind};
    if (any({"push", "brake", "pump", "carv", "cruise", "roll", "ride"})) return {"On board/Locomotion", kind};
    if (any({"idle", "fidget", "look", "corrective", "adjust"})) return {"Small movements/Idle and corrections", kind};
    if (any({"bail", "wipeout", "stumble", "fall"})) return {"Bails/Recovery", kind};
    if (any({"emote", "gesture"})) return {"Gestures/Emotes", kind};
    return {std::string("Other/") + (kind == "clip candidate" ? "Clip candidates" : "Unclassified"), kind};
}
} // namespace
std::string category(const std::string& asset, const std::string& root_type) {
    if (asset.starts_with("_af/")) return "Rig/Channel bindings";
    const std::string name = asset.starts_with("animation/dingo/") ? asset.substr(16) : asset;
    const auto base = classify(name).first;
    if (root_type == "ClipControllerAsset" && has(name, "grind_")) {
        auto family = name.substr(name.find("grind_") + 6);
        family = family.substr(0, family.find('_'));
        const std::string phase = has(name, "_into") || has(name, "_lockin") ? "Entry" : has(name, "_loop") ? "Loop"
                                  : has(name, "_popout") || has(name, "_exit")                         ? "Exit"
                                                                                                         : "Other";
        return "On board/Grinds/" + family + "/" + phase;
    }
    if (root_type == "ClipControllerAsset" && has(name, "manual")) {
        const std::string phase = has(name, "_into") ? "Entry" : has(name, "_loop") ? "Loop" : has(name, "_exit") || has(name, "_popout") ? "Exit" : "Other";
        return "On board/Manuals/" + phase;
    }
    if (root_type == "BoolAsset" || root_type == "FloatAsset" || root_type == "Vector3Asset" || root_type == "IntAsset" || root_type == "GameStateEnumerationAsset") {
        auto short_type = root_type;
        if (ends_with(short_type, "Asset")) short_type.resize(short_type.size() - 5);
        return "Graph/Parameters/" + short_type;
    }
    if (root_type == "TransitionAsset") return "Graph/Transitions";
    const auto tail = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
    if (root_type == "ClipControllerAsset" && (has(asset, "/gameplay_facial/") || tail.starts_with("f_"))) return "Face/" + base;
    return base;
}

// ---- the game's documents ----------------------------------------------------------------------------
namespace {
std::string type_name(const ebx::Document& doc, std::int32_t descriptor) {
    return descriptor >= 0 && static_cast<std::size_t>(descriptor) < doc.types.size() ? doc.types[static_cast<std::size_t>(descriptor)].name : "unknown";
}
// The probe's JSON for a value (Extension/AnimationStudio/Tools/anim_graph_probe.cpp), for the inspector.
Json value_json(const ebx::Value& value, const ebx::Document& doc, unsigned depth = 0) {
    if (depth > 64) throw std::runtime_error("EBX object nesting exceeds inspection limit");
    return std::visit([&](const auto& held) -> Json {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) return nullptr;
        else if constexpr (std::is_same_v<T, ebx::PointerReference>) {
            if (held.kind == ebx::PointerKind::null) return nullptr;
            if (held.index < 0) throw std::runtime_error("Negative EBX pointer index");
            const auto i = static_cast<std::size_t>(held.index);
            if (held.kind == ebx::PointerKind::internal) {
                if (i >= doc.instances.size()) throw std::runtime_error("Internal EBX pointer outside instance table");
                const auto& target = doc.instances[i];
                return {{"kind", "internal"}, {"instance", held.index}, {"type", target.object ? type_name(doc, target.object->descriptor) : "unknown"}};
            }
            if (i >= doc.imports.size()) throw std::runtime_error("External EBX pointer outside import table");
            const auto& target = doc.imports[i];
            return {{"kind", "external"}, {"partition", target.fileGuid.string()}, {"instance", target.classGuid.string()}, {"asset", nullptr}};
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ebx::Object>>) {
            if (!held) return nullptr;
            Json fields = Json::object();
            for (const auto& field : held->fields) fields[field.name] = value_json(field.value, doc, depth + 1);
            return {{"type", type_name(doc, held->descriptor)}, {"fields", std::move(fields)}};
        } else if constexpr (std::is_same_v<T, ebx::Value::Array>) {
            Json items = Json::array();
            for (const auto& item : held) items.push_back(value_json(item, doc, depth + 1));
            return items;
        } else if constexpr (std::is_same_v<T, fb::Guid>) return held.string();
        else if constexpr (std::is_same_v<T, fb::Sha1>) {
            std::string hex;
            constexpr char digits[] = "0123456789abcdef";
            for (const auto byte : held.bytes) {
                const auto v = std::to_integer<unsigned>(byte);
                hex += digits[v >> 4];
                hex += digits[v & 15];
            }
            return hex;
        } else if constexpr (std::is_same_v<T, ebx::ResourceReference>) return {{"resource_id", held.id}};
        else if constexpr (std::is_same_v<T, ebx::TypeReference>) return {{"type_reference", held.descriptor}, {"encoded", held.encoded}};
        else if constexpr (std::is_same_v<T, ebx::BoxedReference>) return {{"boxed_type", held.encodedType}, {"offset", held.dataOffset}};
        else if constexpr (std::is_same_v<T, double>) return std::isfinite(held) ? Json(held) : Json{{"nonfinite", true}};
        else return held;
    }, value.data);
}
Json document_json(const std::string& name, const ebx::Document& doc) {
    Json instances = Json::array();
    for (std::size_t i = 0; i < doc.instances.size(); ++i) {
        const auto& instance = doc.instances[i];
        const ebx::Value wrapped{instance.object, {}};
        instances.push_back({{"index", i}, {"guid", instance.instanceGuid.string()}, {"object", value_json(wrapped, doc)}});
    }
    return {{"asset", name}, {"partition", doc.fileGuid.string()}, {"root_type", doc.rootType}, {"instances", std::move(instances)}};
}

// Field access straight on a document's objects.
const ebx::Value* field(const ebx::Object& object, std::string_view name) {
    const auto* found = object.find(name);
    return found ? &found->value : nullptr;
}
std::optional<std::int64_t> as_int(const ebx::Value* value) {
    if (!value) return std::nullopt;
    if (const auto* i = std::get_if<std::int64_t>(&value->data)) return *i;
    if (const auto* u = std::get_if<std::uint64_t>(&value->data)) return static_cast<std::int64_t>(*u);
    return std::nullopt;
}
std::uint64_t as_u64(const ebx::Value& value) {
    if (const auto* u = std::get_if<std::uint64_t>(&value.data)) return *u;
    if (const auto* i = std::get_if<std::int64_t>(&value.data)) return static_cast<std::uint64_t>(*i);
    throw Unsupported("Expected a whole number in a native document");
}
std::optional<double> as_number(const ebx::Value* value) {
    if (!value) return std::nullopt;
    if (const auto* d = std::get_if<double>(&value->data)) return *d;
    if (const auto i = as_int(value)) return static_cast<double>(*i);
    return std::nullopt;
}
bool as_bool(const ebx::Value* value) {
    if (!value) return false;
    if (const auto* b = std::get_if<bool>(&value->data)) return *b;
    return false;
}
const ebx::Value::Array& as_array(const ebx::Value* value, const char* what) {
    if (value)
        if (const auto* array = std::get_if<ebx::Value::Array>(&value->data)) return *array;
    throw Unsupported(what);
}
const ebx::Object* as_object(const ebx::Value& value) {
    if (const auto* object = std::get_if<std::shared_ptr<ebx::Object>>(&value.data)) return object->get();
    return nullptr;
}
// An external pointer's partition (lower case), or "".
std::string partition_of(const ebx::Document& doc, const ebx::Value* value) {
    if (!value) return {};
    const auto* pointer = std::get_if<ebx::PointerReference>(&value->data);
    if (!pointer || pointer->kind != ebx::PointerKind::external || pointer->index < 0 || static_cast<std::size_t>(pointer->index) >= doc.imports.size())
        return {};
    return lower(doc.imports[static_cast<std::size_t>(pointer->index)].fileGuid.string());
}
const ebx::Object* root_object(const ebx::Document& doc) {
    return doc.instances.empty() || !doc.instances.front().object ? nullptr : doc.instances.front().object.get();
}

std::string sha256_hex(std::string_view text) {
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()),
                   digest.data(), static_cast<ULONG>(digest.size())) != 0)
        throw std::runtime_error("SHA-256 is not available");
    std::string hex;
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}
void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    auto temp = path;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw WriteFailed("could not write " + path.filename().string());
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) throw WriteFailed("could not write " + path.filename().string() + ": " + error.message());
}
void write_text(const std::filesystem::path& path, const std::string& text) {
    write_file(path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

// The game's own tricks: the clip family, the game's name for the trick, the name shown, the kind (make_trick_library.py).
struct TrickFamily {
    const char *family, *trick, *label, *category;
};
// The game's name for each trick is the one it says in the air (current_trick), seen in recordings made in game:
// "n_kickflip", not the scoring data's "nolliekickflip". Nollie impossibles: the nollie clip is the front foot one
// (n_impossible), the back foot one has its own clips (n_backfoot_impossible). trick_key matches every spelling.
constexpr std::array<TrickFamily, 34> trick_families{{
    {"o_ollie", "ollie", "Ollie", "Flip tricks"},
    {"o_kickflip", "kickflip", "Kickflip", "Flip tricks"},
    {"o_heelflip", "heelflip", "Heelflip", "Flip tricks"},
    {"o_popshuv_bs", "popshuvit", "Pop Shuv-it", "Flip tricks"},
    {"o_popshuv_fs", "fspopshuvit", "FS Pop Shuv-it", "Flip tricks"},
    {"o_360popshuv_bs", "360popshuvit", "360 Pop Shuv-it", "Flip tricks"},
    {"o_360popshuv_fs", "fs360popshuvit", "FS 360 Pop Shuv-it", "Flip tricks"},
    {"o_varialflip", "varialkickflip", "Varial Kickflip", "Flip tricks"},
    {"o_varialheel", "varialheelflip", "Varial Heelflip", "Flip tricks"},
    {"o_360flip", "360flip", "360 Flip", "Flip tricks"},
    {"o_hardflip", "hardflip", "Hardflip", "Flip tricks"},
    {"o_inwardheel", "inwardheelflip", "Inward Heelflip", "Flip tricks"},
    {"o_360inwardheel", "360inwardheelflip", "360 Inward Heelflip", "Flip tricks"},
    {"o_360hardflip", "360hardflip", "360 Hardflip", "Flip tricks"},
    {"o_laserflip", "laserflip", "Laser Flip", "Flip tricks"},
    {"o_impossible", "bsimpossible", "BF Impossible", "Flip tricks"},
    {"o_frontfoot_impossible", "fsimpossible", "FF Impossible", "Flip tricks"},
    {"n_nollie", "nollie", "Nollie", "Nollie tricks"},
    {"n_kickflip", "n_kickflip", "Nollie Kickflip", "Nollie tricks"},
    {"n_heelflip", "n_heelflip", "Nollie Heelflip", "Nollie tricks"},
    {"n_popshuv_bs", "n_popshuvit", "Nollie Pop Shuv-it", "Nollie tricks"},
    {"n_popshuv_fs", "n_fspopshuvit", "Nollie FS Pop Shuv-it", "Nollie tricks"},
    {"n_360popshuv_bs", "n_360popshuvit", "Nollie 360 Pop Shuv-it", "Nollie tricks"},
    {"n_360popshuv_fs", "n_fs360popshuvit", "Nollie FS 360 Pop Shuv-it", "Nollie tricks"},
    {"n_varialflip", "n_varialkickflip", "Nollie Varial Kickflip", "Nollie tricks"},
    {"n_varialheel", "n_varialheelflip", "Nollie Varial Heelflip", "Nollie tricks"},
    {"n_360flip", "n_360flip", "Nollie 360 Flip", "Nollie tricks"},
    {"n_hardflip", "n_hardflip", "Nollie Hardflip", "Nollie tricks"},
    {"n_inwardheel", "n_inwardheelflip", "Nollie Inward Heelflip", "Nollie tricks"},
    {"n_360inwardheel", "n_360inwardheelflip", "Nollie 360 Inward Heelflip", "Nollie tricks"},
    {"n_360hardflip", "n_360hardflip", "Nollie 360 Hardflip", "Nollie tricks"},
    {"n_laserflip", "n_laserflip", "Nollie Laser Flip", "Nollie tricks"},
    {"n_impossible", "n_fsimpossible", "Nollie FF Impossible", "Nollie tricks"},
    {"n_backfoot_impossible", "n_bsimpossible", "Nollie BF Impossible", "Nollie tricks"},
}};

// One asset of the inventory: where it is and what it is.
struct Row {
    std::string asset, partition, root_type, toc, bundle, state, detail;
    fb::BundleFileInfo payload;
};
// What decoding a clip controller needs from its document, taken while the document is read.
struct ClipSource {
    std::string asset;
    bool timing_ok{}, looping{};
    std::size_t eclipse_count{};
    std::string rig, channel_map;
    std::uint64_t resource{};
    std::optional<std::int64_t> end_frame;
    std::int64_t trim_offset{};
    std::optional<std::int64_t> trimmed_duration;
};

// The skeleton's joint names and parents (animbase_default_skeleton).
void read_skeleton(const ebx::Document& doc, std::vector<std::string>& names, std::vector<int>& parents) {
    const auto* root = root_object(doc);
    require(root != nullptr, "The native skeleton has no root");
    std::vector<char> table;
    for (const auto& v : as_array(field(*root, "NameTable"), "The native skeleton has no name table")) table.push_back(static_cast<char>(as_u64(v)));
    for (const auto& joint : as_array(field(*root, "Joints"), "The native skeleton has no joints")) {
        const auto* object = as_object(joint);
        require(object != nullptr, "A native skeleton joint is not an object");
        const auto name_at = as_int(field(*object, "Name"));
        const auto parent = as_int(field(*object, "Parent"));
        require(name_at && parent && *name_at >= 0 && static_cast<std::size_t>(*name_at) < table.size(), "A native skeleton joint is unreadable");
        const char* start = table.data() + *name_at;
        const char* limit = table.data() + table.size();
        names.emplace_back(start, std::find(start, limit, '\0'));
        parents.push_back(static_cast<int>(*parent));
    }
    require(names.size() == static_cast<std::size_t>(joint_count), "Unsupported preview skeleton: all 395 native joints are required");
}
} // namespace

Json document_from_game(const std::filesystem::path& game_root, const std::string& toc_name, const std::string& bundle_name, const std::string& asset) {
    const vfs::GameData data(game_root);
    const auto toc = data.read_toc(toc_name);
    const auto bundle = data.read_bundle(toc, bundle_name);
    if (!bundle) throw std::runtime_error("The bundle this asset was found in is not in the game any more: rebuild the trick library.");
    for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i) {
        if (lower(bundle->manifest.ebx[i].name) != asset) continue;
        const auto* payload = bundle->payload(fb::AssetKind::ebx, i);
        if (!payload) break;
        return document_json(asset, ebx::read_document(data.read(*payload)));
    }
    throw std::runtime_error("This asset is not in the game any more: rebuild the trick library.");
}

Report build(const std::filesystem::path& game_root, const std::filesystem::path& output, const Progress& progress, const std::string& stamp) {
    namespace skeleton = dingosdk::game::build::v20260929::skater_skeleton;
    Report report;
    const auto tell = [&](std::string_view step, std::size_t done, std::size_t total) {
        if (progress && !progress(step, done, total)) throw Stopped{};
    };
    std::filesystem::create_directories(output);
    std::filesystem::create_directories(output / L"native-resources");
    const vfs::GameData data(game_root);

    // ---- 1. every animation/dingo document, the first of each name, in the probe's order ----
    std::vector<std::string> tocs{"Win32/levels/game/bam_levelroot/bam_levelroot.toc", "Win32/globals.toc", "Win32/levels/game/dingolevel_root/dingolevel_root.toc"};
    {
        std::set<std::string> known;
        for (const auto& name : tocs) known.insert(lower(name));
        std::vector<std::string> extra;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(game_root / L"Data" / L"Win32", error), end; !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error) || lower(it->path().extension().string()) != ".toc") continue;
            const auto relative = std::filesystem::relative(it->path(), game_root / L"Data").generic_string();
            if (known.insert(lower(relative)).second) extra.push_back(relative);
        }
        std::sort(extra.begin(), extra.end());
        tocs.insert(tocs.end(), extra.begin(), extra.end());
    }
    std::vector<Row> rows;
    std::unordered_map<std::string, std::size_t> by_name, by_partition;
    std::vector<ClipSource> clips;
    std::set<std::string> channel_maps, channel_prefixes;
    std::set<std::uint64_t> resources;
    std::size_t scanned = 0;
    const auto scan = [&](const vfs::GameBundle& bundle, const std::string& toc_name, const std::string& bundle_name) {
        for (std::size_t i = 0; i < bundle.manifest.ebx.size(); ++i) {
            const auto name = lower(bundle.manifest.ebx[i].name);
            if (!name.starts_with("animation/dingo/") || by_name.contains(name)) continue;
            Row row{name, {}, {}, toc_name, bundle_name, "metadata", "Typed native authoring data; live binding not established", {}};
            try {
                const auto* payload = bundle.payload(fb::AssetKind::ebx, i);
                if (!payload) throw std::runtime_error("EBX payload is missing");
                row.payload = *payload;
                const auto doc = ebx::read_document(data.read(*payload));
                row.partition = lower(doc.fileGuid.string());
                row.root_type = doc.rootType;
                ClipSource clip;
                clip.asset = name;
                for (const auto& instance : doc.instances) {
                    if (!instance.object) continue;
                    const auto type = type_name(doc, instance.object->descriptor);
                    if (!ends_with(type, "AnimationAsset")) continue;
                    if (const auto map = partition_of(doc, field(*instance.object, "ChannelToDofAsset")); map.size() == 36) {
                        channel_maps.insert(map);
                        channel_prefixes.insert("_af/" + map.substr(0, 18) + "/");
                    }
                    if (const auto* resource = field(*instance.object, "Resource"))
                        if (const auto* id = std::get_if<ebx::ResourceReference>(&resource->data)) resources.insert(id->id);
                    if (type == "EclipseAnimationAsset") {
                        ++clip.eclipse_count;
                        clip.channel_map = partition_of(doc, field(*instance.object, "ChannelToDofAsset"));
                        if (const auto* resource = field(*instance.object, "Resource"))
                            if (const auto* id = std::get_if<ebx::ResourceReference>(&resource->data)) clip.resource = id->id;
                        clip.end_frame = as_int(field(*instance.object, "EndFrame"));
                        clip.trim_offset = as_int(field(*instance.object, "TrimOffset")).value_or(0);
                        clip.trimmed_duration = as_int(field(*instance.object, "TrimmedDuration"));
                    }
                }
                if (doc.rootType == "ClipControllerAsset") {
                    if (const auto* root = root_object(doc)) {
                        clip.timing_ok = as_number(field(*root, "FPS")) == 60.0 && as_number(field(*root, "TickOffset")).value_or(0) == 0;
                        clip.looping = as_bool(field(*root, "IsLooping"));
                        clip.rig = partition_of(doc, field(*root, "RigRef"));
                    }
                    clips.push_back(std::move(clip));
                }
            } catch (const std::exception& e) {
                row.state = "error";
                row.detail = std::string(e.what()).substr(0, 400);
            }
            by_name.emplace(name, rows.size());
            if (!row.partition.empty()) by_partition.emplace(row.partition, rows.size());
            rows.push_back(std::move(row));
            if (++scanned % 250 == 0) tell("Reading the game's animation", scanned, 19500);
        }
    };
    const auto scan_toc = [&](const std::string& toc_name, bool skeleton_first) {
        const auto toc = data.read_toc(toc_name);
        if (skeleton_first)
            if (const auto bundle = data.read_bundle(toc, skeleton::skeleton_bundle)) scan(*bundle, toc_name, std::string(skeleton::skeleton_bundle));
        for (const auto& entry : toc.bundles) {
            if (skeleton_first && entry.name == skeleton::skeleton_bundle) continue;
            if (const auto bundle = data.read_bundle(toc, entry.name)) scan(*bundle, toc_name, entry.name);
        }
    };
    for (std::size_t t = 0; t < tocs.size(); ++t) {
        try {
            scan_toc(tocs[t], t == 0);
        } catch (const Stopped&) {
            throw;
        } catch (const std::exception& e) {
            if (t < 3) throw std::runtime_error("The game's " + tocs[t] + " could not be read: " + e.what());
        }
    }
    report.assets = rows.size();

    // ---- 2. the channel maps (_af/) and the resources the clips need ----
    std::unordered_map<std::string, std::vector<std::int64_t>> channel_ids; // map partition -> its DOF ids
    std::unordered_map<std::uint64_t, std::pair<fb::BundleFileInfo, std::uint64_t>> resource_at; // id -> where, and its size
    for (std::size_t t = 0; t < tocs.size(); ++t) {
        tell("Finding the clips' keys", t, tocs.size());
        try {
            const auto toc = data.read_toc(tocs[t]);
            for (const auto& entry : toc.bundles) {
                const auto bundle = data.read_bundle(toc, entry.name);
                if (!bundle) continue;
                for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i) {
                    const auto name = lower(bundle->manifest.ebx[i].name);
                    if (!name.starts_with("_af/") || name.size() < 23 || !channel_prefixes.contains(name.substr(0, 23))) continue;
                    try {
                        const auto* payload = bundle->payload(fb::AssetKind::ebx, i);
                        if (!payload) continue;
                        const auto doc = ebx::read_document(data.read(*payload));
                        const auto guid = lower(doc.fileGuid.string());
                        if (!channel_maps.contains(guid) || channel_ids.contains(guid)) continue;
                        std::vector<std::int64_t> ids;
                        if (const auto* root = root_object(doc))
                            for (const auto& v : as_array(field(*root, "ChannelToDofIds"), "A channel map has no DOF ids")) ids.push_back(static_cast<std::int64_t>(as_u64(v)));
                        channel_ids.emplace(guid, std::move(ids));
                        if (!by_name.contains(name)) {
                            by_name.emplace(name, rows.size());
                            by_partition.emplace(guid, rows.size());
                            rows.push_back({name, guid, doc.rootType, tocs[t], entry.name, "metadata", "Typed native authoring data; live binding not established", *payload});
                        }
                    } catch (const std::exception&) {}
                }
                for (std::size_t i = 0; i < bundle->manifest.resources.size(); ++i) {
                    const auto& asset = bundle->manifest.resources[i];
                    if (!resources.contains(asset.resourceId) || resource_at.contains(asset.resourceId)) continue;
                    if (const auto* payload = bundle->payload(fb::AssetKind::resource, i)) resource_at.emplace(asset.resourceId, std::pair{*payload, asset.originalSize});
                }
            }
        } catch (const std::exception&) {}
    }

    // ---- 3. the skeleton, the rig's DOF names and its default pose ----
    const auto doc_of = [&](const std::string& partition) {
        const auto found = by_partition.find(partition);
        require(found != by_partition.end(), "A native document the rig needs is missing");
        return ebx::read_document(data.read(rows[found->second].payload));
    };
    const auto named = [&](const char* asset) -> const Row& {
        const auto found = by_name.find(asset);
        if (found == by_name.end()) throw std::runtime_error(std::string("The game's ") + asset + " was not found");
        return rows[found->second];
    };
    std::vector<std::string> names;
    std::vector<int> parents;
    read_skeleton(ebx::read_document(data.read(named("animation/dingo/animbase_default_skeleton").payload)), names, parents);
    const auto& rig_row = named("animation/dingo/animbase_default_rig");
    const auto rig = ebx::read_document(data.read(rig_row.payload));
    std::unordered_map<std::int64_t, std::string> dofs;
    {
        const auto* root = root_object(rig);
        require(root != nullptr, "The native rig has no root");
        std::vector<std::int64_t> ids, starts;
        for (const auto& v : as_array(field(*root, "DofIds"), "The native rig has no DOF ids")) ids.push_back(static_cast<std::int64_t>(as_u64(v)));
        for (const auto& v : as_array(field(*root, "DofSetIdIndices"), "The native rig has no DOF ranges")) starts.push_back(static_cast<std::int64_t>(as_u64(v)));
        const auto& sets = as_array(field(*root, "DofSets"), "The native rig has no DOF sets");
        require(starts.size() == sets.size(), "Rig DOF range mismatch");
        for (std::size_t s = 0; s < sets.size(); ++s) {
            const auto set = doc_of(partition_of(rig, &sets[s]));
            const auto* set_root = root_object(set);
            require(set_root != nullptr, "A native DOF set has no root");
            std::vector<std::byte> blob;
            for (const auto& word : as_array(field(*set_root, "LayoutData"), "A native DOF set has no layout")) {
                const auto w = as_u64(word);
                for (int b = 0; b < 8; ++b) blob.push_back(static_cast<std::byte>((w >> (8 * b)) & 0xff));
            }
            const Reader r{blob};
            const auto text = r.le<std::uint32_t>(16), size = r.le<std::uint32_t>(20);
            const auto end = s + 1 < starts.size() ? starts[s + 1] : static_cast<std::int64_t>(ids.size());
            const auto count = end - starts[s];
            require(count >= 0 && text == 32 + 32 * count && std::uint64_t{text} + size <= blob.size(), "Unsupported cached DOF layout");
            for (std::int64_t j = 0; j < count; ++j) {
                const auto at = r.le<std::uint32_t>(32 + static_cast<std::size_t>(j) * 32 + 12);
                require(at < size, "DOF name outside layout");
                const auto* start = reinterpret_cast<const char*>(blob.data()) + text + at;
                const auto* limit = reinterpret_cast<const char*>(blob.data()) + text + size;
                const auto* stop = std::find(start, limit, '\0');
                require(stop != limit, "DOF name outside layout");
                const auto dof = ids[static_cast<std::size_t>(starts[s] + j)];
                const std::string name(start, stop);
                const auto [found, fresh] = dofs.emplace(dof, name);
                require(fresh || found->second == name, "Conflicting cached DOF identities");
            }
        }
    }
    std::vector<Joint10> defaults(names.size(), Joint10{});
    {
        const auto* root = root_object(rig);
        const auto* pose_field = field(*root, "DefaultPoseAsset");
        require(pose_field != nullptr, "Unsupported default-pose reference");
        const auto* ref = std::get_if<ebx::PointerReference>(&pose_field->data);
        require(ref && ref->kind == ebx::PointerKind::internal && ref->index >= 0 && static_cast<std::size_t>(ref->index) < rig.instances.size(),
                "Unsupported default-pose reference");
        const auto* pose = rig.instances[static_cast<std::size_t>(ref->index)].object.get();
        require(pose != nullptr, "Unsupported default-pose reference");
        std::unordered_map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < names.size(); ++i) index.emplace(names[i], i);
        std::set<std::string> seen;
        for (const auto& [prefix, axes] : {std::pair{std::string("Vector3"), std::string("xyz")}, std::pair{std::string("Vector4"), std::string("xyzw")}}) {
            const auto& dof_ids = as_array(field(*pose, prefix + "DofIds"), "Default pose inventory mismatch");
            const auto& values = as_array(field(*pose, prefix + "Values"), "Default pose inventory mismatch");
            require(dof_ids.size() == values.size(), "Default pose inventory mismatch");
            for (std::size_t k = 0; k < dof_ids.size(); ++k) {
                const auto found_dof = dofs.find(static_cast<std::int64_t>(as_u64(dof_ids[k])));
                const std::string channel = found_dof == dofs.end() ? std::string{} : found_dof->second;
                if (channel.size() < 2) continue;
                const auto bone = channel.substr(0, channel.size() - 2), suffix = channel.substr(channel.size() - 2);
                const auto joint = index.find(bone);
                if (joint == index.end() || (suffix != ".t" && suffix != ".q" && suffix != ".s")) continue;
                const std::size_t start = suffix == ".s" ? 0 : suffix == ".q" ? 3 : 7;
                require((start == 3) == (axes.size() == 4), "Default pose DOF type mismatch");
                require(seen.insert(channel).second, "Duplicate default pose channel");
                const auto* value = as_object(values[k]);
                require(value != nullptr, "Default pose value is not an object");
                for (std::size_t a = 0; a < axes.size(); ++a) {
                    const auto number = as_number(field(*value, std::string(1, axes[a])));
                    require(number.has_value(), "Default pose value is missing an axis");
                    defaults[joint->second][start + a] = static_cast<float>(*number);
                }
            }
        }
        for (const auto& name : names)
            for (const char* suffix : {".s", ".q", ".t"}) require(seen.contains(name + suffix), "Incomplete native default pose");
        for (const auto& joint : defaults)
            require(std::all_of(joint.begin(), joint.end(), [](float x) { return std::isfinite(x); }) && joint[0] > 0 && joint[1] > 0 && joint[2] > 0,
                    "Invalid native default pose");
    }

    // ---- 4. every clip, decoded and written ----
    std::unordered_map<std::string, const TrickFamily*> trick_assets;
    std::unordered_map<std::string, std::string> trick_pop;
    for (const auto& family : trick_families)
        for (const char* pop : {"low", "mid", "high"}) {
            const auto asset = std::string("animation/dingo/c_proto_onb_trick_") + family.family + "_" + pop;
            trick_assets.emplace(asset, &family);
            trick_pop.emplace(asset, pop);
        }
    std::sort(clips.begin(), clips.end(), [](const ClipSource& a, const ClipSource& b) {
        const auto busy = [](const std::string& s) { return has(s, "grind") || has(s, "manual") || has(s, "grab") || has(s, "trick"); };
        return std::pair{!busy(a.asset), a.asset} < std::pair{!busy(b.asset), b.asset};
    });
    Json catalog = Json::array();
    std::vector<std::tuple<std::string, std::string, std::string, Json>> entries; // sort key, then the entry
    for (std::size_t c = 0; c < clips.size(); ++c) {
        if (c % 20 == 0) tell("Decoding the clips", c, clips.size());
        const auto& source = clips[c];
        auto& row = rows[by_name.at(source.asset)];
        if (row.state == "error") continue;
        try {
            require(source.timing_ok, "Unsupported native source timing");
            require(source.eclipse_count == 1, "Unsupported source backend: expected one EclipseAnimationAsset");
            const auto map = channel_ids.find(source.channel_map);
            require(map != channel_ids.end(), "A clip's channel map is missing");
            std::vector<std::string> channels;
            for (const auto id : map->second) {
                const auto found = dofs.find(id);
                channels.push_back(found != dofs.end() ? found->second : "unknown:" + std::to_string(id));
            }
            const auto where = resource_at.find(source.resource);
            require(where != resource_at.end(), "A clip's keys are missing");
            const auto blob = data.read(where->second.first);
            require(blob.size() >= 40 && (Reader{blob}.le<std::uint32_t>(36) == 1 || Reader{blob}.le<std::uint32_t>(36) == 2), "Unsupported native quaternion codec");
            require(source.rig == rig_row.partition, "Unsupported preview rig");
            require(source.end_frame.has_value(), "Unsupported native frame range");
            require(source.trim_offset == 0 && source.trimmed_duration.value_or(*source.end_frame) == *source.end_frame, "Unsupported trimmed native clip");
            const auto decoded = decode_pose(blob, channels, defaults, names, *source.end_frame);
            const auto trick = trick_assets.find(source.asset);
            Json entry;
            float takeoff = -1, landing = -1;
            std::string file, label, entry_category, pop;
            if (trick != trick_assets.end()) {
                std::tie(takeoff, landing) = trick_marks(decoded);
                pop = trick_pop.at(source.asset);
                file = std::string(trick->second->trick) + "_" + pop + ".clip";
                label = trick->second->label;
                entry_category = trick->second->category;
                entry = {{"category", entry_category}, {"label", label}, {"trick", trick->second->trick}, {"pop", pop}, {"file", file},
                         {"asset", source.asset}, {"position_codec", "native-piec-v1-extent"}, {"native_looping", source.looping}};
                ++report.tricks;
            } else {
                file = "native-" + sha256_hex(source.asset).substr(0, 24) + ".clip";
                label = source.asset.substr(source.asset.rfind('/') + 1);
                entry_category = category(source.asset, "ClipControllerAsset");
                entry = {{"category", entry_category}, {"label", label}, {"trick", "native:" + source.asset}, {"pop", ""}, {"file", file},
                         {"asset", source.asset}, {"preview_only", true}, {"position_codec", "native-piec-v1-extent"}, {"native_looping", source.looping}};
            }
            const auto bytes = encode_clip(decoded, takeoff, landing);
            write_file(output / std::filesystem::path(file), bytes);
            // The resource's details, for the inspector (the bytes themselves stay in the game).
            {
                Json names_json = Json::array();
                for (std::size_t i = 0; i < map->second.size(); ++i) names_json.push_back({{"id", map->second[i]}, {"name", channels[i]}});
                const Reader r{blob};
                const Json details{{"asset", source.asset + " [resource " + std::to_string(source.resource) + "]"}, {"root_type", "EclipseAnimationAsset"},
                                   {"resource_id", source.resource}, {"bytes", blob.size()}, {"channel_names", std::move(names_json)},
                                   {"piecewise_header", Json{{"quaternion_codec", r.le<std::uint32_t>(36)}, {"vector_codec", r.le<std::uint32_t>(40)},
                                                             {"scalar_codec", r.le<std::uint32_t>(44)}, {"time_codec", r.le<std::uint32_t>(48)}}},
                                   {"scope", "Encoded native source bytes stay in the game; these are their details"}};
                write_text(output / L"native-resources" / (std::to_string(source.resource) + ".json"), details.dump());
            }
            row.state = "decoded";
            row.detail = std::to_string(decoded.times.size()) + " poses at 60 Hz; " + (decoded.board ? "body and board" : "body only") + "; no live phase binding";
            entries.emplace_back(entry_category, label, pop, std::move(entry));
            ++report.clips;
        } catch (const Stopped&) {
            throw;
        } catch (const WriteFailed&) {
            throw;
        } catch (const Unsupported& e) {
            row.state = "unsupported";
            row.detail = std::string(e.what()).substr(0, 400);
            ++report.unsupported;
        } catch (const std::exception& e) {
            row.state = "error";
            row.detail = std::string(e.what()).substr(0, 400);
            if (report.first_error.empty()) report.first_error = source.asset + ": " + e.what();
            ++report.errors;
        }
    }
    tell("Writing the catalog", clips.size(), clips.size());

    // ---- 5. the catalogs ----
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return std::tie(std::get<0>(a), std::get<1>(a), std::get<2>(a)) < std::tie(std::get<0>(b), std::get<1>(b), std::get<2>(b));
    });
    for (auto& entry : entries) catalog.push_back(std::move(std::get<3>(entry)));
    write_text(output / L"catalog.json", catalog.dump());
    std::vector<std::string> categories(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) categories[i] = category(rows[i].asset, rows[i].root_type);
    std::vector<std::size_t> order(rows.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return std::tie(categories[a], rows[a].asset) < std::tie(categories[b], rows[b].asset); });
    Json native = Json::array();
    for (const auto i : order) {
        const auto& row = rows[i];
        native.push_back({{"asset", row.asset}, {"category", categories[i]}, {"kind", row.root_type.empty() ? "Unverified name" : row.root_type},
                          {"root_type", row.root_type}, {"state", row.state}, {"detail", row.detail}, {"partition", row.partition},
                          {"toc", row.toc}, {"bundle", row.bundle}});
    }
    write_text(output / L"native-catalog.json", native.dump());
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char date[32]{};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local);
    write_text(output / L"build-info.json",
               Json{{"builder", builder_version}, {"built", date}, {"stamp", stamp}, {"assets", report.assets}, {"clips", report.clips}, {"tricks", report.tricks},
                    {"unsupported", report.unsupported}, {"errors", report.errors}}
                   .dump(2));
    return report;
}
} // namespace dingosdk::anim_studio::native_library
