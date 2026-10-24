#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace dingosdk {
// Chat flood control, the same for the sender, the host, a dedicated server and every
// receiver: a few lines at once, then one every 1.5 s, and never the same line twice within
// 15 s. Receivers allow one line of slack for lines that arrive bunched up. An answer to the
// running vote or poll may repeat the line before it (`answer`): the same /2 is a new answer
// in the next poll, and the vote itself counts one answer per player. The pace still holds.
struct ChatRate {
    static constexpr double burst = 4, interval_us = 1500000;
    static constexpr std::uint64_t repeat_us = 15000000;
    enum class Verdict { accepted, too_fast, repeated };
    double tokens = burst;
    std::uint64_t refilled{}, previous_at{};
    std::string previous;
    Verdict accept(std::uint64_t now, std::string_view text, double slack = 0, bool answer = false) {
        const double limit = burst + slack;
        if (!refilled || now < refilled) {
            tokens = limit;
        } else {
            tokens = std::min(limit, tokens + static_cast<double>(now - refilled) / interval_us);
        }
        refilled = now;
        if (!answer && !previous.empty() && text == previous && now >= previous_at && now - previous_at < repeat_us)
            return Verdict::repeated;
        if (tokens < 1) return Verdict::too_fast;
        tokens -= 1;
        previous.assign(text);
        previous_at = now;
        return Verdict::accepted;
    }
};
} // namespace dingosdk
