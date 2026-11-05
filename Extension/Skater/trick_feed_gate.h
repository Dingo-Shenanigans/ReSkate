#pragma once
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Which writes of the trick list's fields are a trick's (trick_feed.cpp). Other HUD models write
// text to the same field numbers: with in-game music on, the song's title is a string at the
// name's field (0x0002) of another model, and read as a trick name (reported 2026-11-03: about
// one name in ten on a server's first day). So a write is told apart by the UI model record it
// belongs to. A record counts as a trick list's once it writes something only a trick entry
// writes (a side, a landing, a stance or a modifier, each checked against the game's own ids). A
// name, spin or board spin from a record not known yet waits for that, and is left out after
// `hold_ms`. Pure, so it tests.
namespace dingosdk::tricks::feed {
template <class Note> class RecordGate {
public:
    static constexpr std::uint64_t hold_ms = 2000;
    static constexpr std::size_t most_held = 64, most_records = 64;

    // A note from `record` (0: unknown, taken as it is) at `now_ms`; `sure` when its kind is one
    // only a trick entry writes. What may go out now goes into `out`, in order.
    void add(const Note& note, std::uintptr_t record, bool sure, std::uint64_t now_ms, std::vector<Note>& out) {
        expire(now_ms);
        if (!record) {
            out.push_back(note);
            return;
        }
        if (sure) {
            if (!records_.contains(record)) {
                if (records_.size() >= most_records) records_.clear(); // (a new level's models)
                records_.insert(record);
                // What waited for this record goes first, as it was written.
                for (auto it = held_.begin(); it != held_.end();)
                    if (it->record == record) {
                        out.push_back(it->note);
                        it = held_.erase(it);
                    } else ++it;
            }
            out.push_back(note);
        } else if (records_.contains(record)) {
            out.push_back(note);
        } else if (held_.size() < most_held) {
            held_.push_back({note, record, now_ms});
        } else {
            left_out_.push_back(note);
        }
    }
    // Notes that waited too long: never a trick's.
    void expire(std::uint64_t now_ms) {
        for (auto it = held_.begin(); it != held_.end();)
            if (now_ms - it->at >= hold_ms) {
                left_out_.push_back(it->note);
                it = held_.erase(it);
            } else ++it;
    }
    // What was left out since the last call (for the log).
    std::vector<Note> take_left_out() { return std::exchange(left_out_, {}); }
    void clear() {
        records_.clear();
        held_.clear();
        left_out_.clear();
    }

private:
    struct Held {
        Note note;
        std::uintptr_t record{};
        std::uint64_t at{};
    };
    std::set<std::uintptr_t> records_;
    std::vector<Held> held_;
    std::vector<Note> left_out_;
};
} // namespace dingosdk::tricks::feed
