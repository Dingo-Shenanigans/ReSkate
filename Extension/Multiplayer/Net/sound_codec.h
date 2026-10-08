#pragma once
#include "Extension/Multiplayer/Remote/audio_state.h"
#include "pose_batch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// Skaters' sound between a dedicated server and its players: several players' samples in one
// message, each sample as what changed since the one before it.
//
// A sample is 58 numbers, 26 selectors and 43 switches, of which about twenty numbers change from
// one to the next and the rest seldom. Numbers are rounded to 1/1024 and sent as how far they
// moved; a switch or selector is sent only when one changed. Messages go reliably and in order
// (many samples are a pulse one sample long, which must not be lost), so each builds on the last
// one sent: the sender keeps what the receiver holds of each stream. A stream is sent whole when
// it starts and again every couple of seconds, which also puts right a receiver that missed its
// start.
namespace dingosdk::multiplayer::sound_codec {
inline constexpr std::array<std::uint8_t, 4> magic{'R', 'M', 'S', '1'};
inline constexpr std::size_t header_size = 12;
inline constexpr std::int64_t value_scale = 1024, value_limit = 1000000LL * value_scale;
// Ages are sent in steps of 256 us.
inline constexpr unsigned age_shift = 8;
inline constexpr std::uint32_t age_limit = 1000000U >> age_shift;
inline constexpr std::uint64_t whole_every_us = 2000000;

inline bool is_sound(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= header_size && std::equal(magic.begin(), magic.end(), bytes.begin());
}

struct QuantState {
    std::array<std::int32_t, audio_float_count> values{};
    std::array<std::uint32_t, audio_selector_count> selectors{};
    std::uint64_t flags{}; // a bit each
    bool operator==(const QuantState &) const = default;
};
// A number that is not nothing never rounds to nothing: the game may ask only whether it is.
inline QuantState quantize(const AudioState &state) noexcept {
    QuantState out;
    for (std::size_t i = 0; i < audio_float_count; ++i) {
        const double value = std::isfinite(state.values[i]) ? static_cast<double>(state.values[i]) : 0.0;
        auto rounded = std::llround(std::clamp(value * static_cast<double>(value_scale), -static_cast<double>(value_limit), static_cast<double>(value_limit)));
        if (!rounded && value != 0.0) rounded = value > 0 ? 1 : -1;
        out.values[i] = static_cast<std::int32_t>(rounded);
    }
    out.selectors = state.selectors;
    // (Never the first switch: a remote skater's sound is not the local player's mix.)
    for (std::size_t i = 1; i < audio_flag_count; ++i)
        if (state.flags[i]) out.flags |= std::uint64_t{1} << i;
    return out;
}
inline AudioState restore(const QuantState &state) noexcept {
    AudioState out;
    for (std::size_t i = 0; i < audio_float_count; ++i)
        out.values[i] = static_cast<float>(static_cast<double>(state.values[i]) / static_cast<double>(value_scale));
    out.selectors = state.selectors;
    for (std::size_t i = 0; i < audio_flag_count; ++i) out.flags[i] = (state.flags >> i) & 1;
    return out;
}

namespace detail {
using pose_batch::detail::put;
using pose_batch::detail::Reader;
using pose_batch::detail::unzigzag;
using pose_batch::detail::varint;
using pose_batch::detail::zigzag;
// Eight bytes say which numbers changed (a bit each) and, above those, whether the sample is an
// event, whether the switches follow and whether selectors do.
inline constexpr unsigned event_bit = 58, flags_bit = 59, selectors_bit = 60;
inline void put_sample(std::vector<std::uint8_t> &out, const QuantState &now, const QuantState &was, std::uint32_t age_us, bool event) {
    std::uint64_t changed{};
    std::uint32_t selectors{};
    for (std::size_t i = 0; i < audio_float_count; ++i)
        if (now.values[i] != was.values[i]) changed |= std::uint64_t{1} << i;
    for (std::size_t i = 0; i < audio_selector_count; ++i)
        if (now.selectors[i] != was.selectors[i]) selectors |= 1U << i;
    const bool flags = now.flags != was.flags;
    put(out, changed | (std::uint64_t{event} << event_bit) | (std::uint64_t{flags} << flags_bit) | (std::uint64_t{selectors != 0} << selectors_bit), 8);
    varint(out, std::min(age_us >> age_shift, age_limit));
    for (std::size_t i = 0; i < audio_float_count; ++i)
        if (changed & (std::uint64_t{1} << i)) varint(out, zigzag(std::int64_t{now.values[i]} - std::int64_t{was.values[i]}));
    if (flags) put(out, now.flags, 6);
    if (selectors) {
        put(out, selectors, 4);
        for (std::size_t i = 0; i < audio_selector_count; ++i)
            if (selectors & (1U << i)) put(out, now.selectors[i], 4);
    }
}
// Throws on bytes that are not a sample or that make an impossible one.
inline QuantState get_sample(Reader &r, const QuantState &was, std::uint32_t &age_us, bool &event) {
    const auto head = r.get(8);
    if (head >> (selectors_bit + 1)) throw std::runtime_error("Invalid sound sample");
    const auto age = r.varint();
    if (age > age_limit) throw std::runtime_error("Invalid sound sample age");
    age_us = static_cast<std::uint32_t>(age) << age_shift;
    event = (head >> event_bit) & 1;
    QuantState now = was;
    for (std::size_t i = 0; i < audio_float_count; ++i) {
        if (!(head & (std::uint64_t{1} << i))) continue;
        const auto step = unzigzag(r.varint());
        if (step > 2 * value_limit || step < -2 * value_limit) throw std::runtime_error("Invalid sound value");
        const auto value = std::int64_t{was.values[i]} + step;
        if (value > value_limit || value < -value_limit) throw std::runtime_error("Invalid sound value");
        now.values[i] = static_cast<std::int32_t>(value);
    }
    if ((head >> flags_bit) & 1) {
        now.flags = r.get(6);
        if (now.flags >> audio_flag_count || (now.flags & 1)) throw std::runtime_error("Invalid sound switches");
    }
    if ((head >> selectors_bit) & 1) {
        const auto selectors = r.get(4);
        if (!selectors || selectors >> audio_selector_count) throw std::runtime_error("Invalid sound selectors");
        for (std::size_t i = 0; i < audio_selector_count; ++i)
            if (selectors & (std::uint64_t{1} << i)) now.selectors[i] = static_cast<std::uint32_t>(r.get(4));
    }
    return now;
}
} // namespace detail

// The sending side of one connection: what its receiver holds of each player's sound. begin(),
// then add() for each player with samples, then the message goes reliably and sent() is told
// whether it went: what the receiver holds moves on only when it did.
class Sender {
  public:
    void begin(std::uint64_t world, std::uint64_t map) {
        message_.assign(magic.begin(), magic.end());
        detail::put(message_, world & 0xffffffff, 4);
        detail::put(message_, map & 0xffffffff, 4);
        staged_.clear();
    }
    void add(std::uint64_t source, std::uint64_t epoch, std::uint32_t sequence, std::uint64_t time_us, std::span<const AudioSample> samples, std::uint64_t now) {
        if (samples.empty() || samples.size() > max_audio_samples) return;
        Out out;
        if (const auto staged = staged_.find(source); staged != staged_.end()) out = staged->second;
        else if (const auto found = streams_.find(source); found != streams_.end()) out = found->second;
        if (!out.stream || out.epoch != epoch) {
            out = {};
            out.epoch = epoch;
            out.stream = next_stream_++;
            if (!next_stream_) next_stream_ = 1;
        }
        const bool whole = !out.started || sequence <= out.sequence || now - out.whole_at >= whole_every_us;
        detail::varint(message_, out.stream);
        message_.push_back(static_cast<std::uint8_t>((whole ? 1U : 0U) | (static_cast<unsigned>(samples.size()) << 1)));
        if (whole) {
            detail::put(message_, source, 8);
            detail::put(message_, epoch, 8);
            detail::put(message_, sequence, 4);
            detail::put(message_, time_us, 8);
            out.whole_at = now;
        } else {
            detail::varint(message_, sequence - out.sequence);
            detail::varint(message_, detail::zigzag(static_cast<std::int64_t>(time_us) - static_cast<std::int64_t>(out.time_us)));
        }
        QuantState was = whole ? QuantState{} : out.state;
        for (const auto &sample : samples) {
            const auto state = quantize(sample.state);
            detail::put_sample(message_, state, was, sample.age_us, sample.event);
            was = state;
        }
        out.state = was;
        out.sequence = sequence;
        out.time_us = time_us;
        out.started = true;
        staged_[source] = out;
    }
    bool pending() const noexcept { return !staged_.empty(); }
    std::span<const std::uint8_t> message() const noexcept { return message_; }
    void sent(bool went) {
        if (went)
            for (const auto &[source, out] : staged_) streams_[source] = out;
        staged_.clear();
        message_.clear();
    }
    // The player left: their stream is nobody's.
    void forget(std::uint64_t source) { streams_.erase(source); }
    // A new world: every stream starts over.
    void restart() {
        streams_.clear();
        staged_.clear();
        message_.clear();
    }

  private:
    struct Out {
        std::uint64_t epoch{}, time_us{}, whole_at{};
        std::uint32_t sequence{};
        std::uint16_t stream{};
        bool started{};
        QuantState state;
    };
    std::map<std::uint64_t, Out> streams_, staged_;
    std::vector<std::uint8_t> message_;
    std::uint16_t next_stream_ = 1;
};

// The receiving side of one stream: whose it is and the last sample of it.
struct In {
    std::uint64_t source{}, epoch{}, time_us{};
    std::uint32_t sequence{};
    QuantState state;
};
// One player's samples from a message, as the packet of sound it would have been.
struct Heard {
    std::uint64_t source{}, epoch{}, time_us{};
    std::uint32_t sequence{};
    std::vector<AudioSample> samples;
};
// What a message holds, or nothing when it is not one or is damaged. A message of another world
// holds nothing; samples of a stream this side was never told of are passed over. `streams` moves
// on with what is read.
template <typename Streams>
std::optional<std::vector<Heard>> read(Streams &streams, std::span<const std::uint8_t> bytes, std::uint64_t world, std::uint64_t map) noexcept {
    try {
        if (!is_sound(bytes)) return {};
        detail::Reader r{bytes, 4};
        std::vector<Heard> out;
        if (r.get(4) != (world & 0xffffffff) || r.get(4) != (map & 0xffffffff)) return out;
        while (r.at < bytes.size()) {
            const auto stream = r.varint();
            if (stream > 0xffff) return {};
            const auto head = r.get(1);
            const bool whole = head & 1;
            const auto count = static_cast<std::size_t>(head >> 1);
            if (!count || count > max_audio_samples) return {};
            In in;
            bool known = whole;
            if (whole) {
                in.source = r.get(8);
                in.epoch = r.get(8);
                in.sequence = static_cast<std::uint32_t>(r.get(4));
                in.time_us = r.get(8);
            } else {
                const auto ahead = r.varint();
                const auto time_step = detail::unzigzag(r.varint());
                if (!ahead || ahead > 0xffffff) return {};
                if (const auto found = streams.find(static_cast<std::uint16_t>(stream)); found != streams.end()) {
                    in = found->second;
                    known = true;
                }
                in.sequence += static_cast<std::uint32_t>(ahead);
                in.time_us = static_cast<std::uint64_t>(static_cast<std::int64_t>(in.time_us) + time_step);
            }
            Heard heard{in.source, in.epoch, in.time_us, in.sequence, {}};
            heard.samples.reserve(count);
            QuantState was = whole ? QuantState{} : in.state;
            std::uint32_t last_age = 1000000;
            for (std::size_t i = 0; i < count; ++i) {
                std::uint32_t age{};
                bool event{};
                was = detail::get_sample(r, was, age, event);
                if (age > last_age) return {};
                last_age = age;
                heard.samples.push_back({age, restore(was), event});
            }
            if (!known) continue;
            in.state = was;
            streams[static_cast<std::uint16_t>(stream)] = in;
            out.push_back(std::move(heard));
            if (out.size() > 512) return {};
        }
        return out;
    } catch (...) {
        return {};
    }
}
} // namespace dingosdk::multiplayer::sound_codec
