#pragma once
#include <climits>
#include <cstddef>

// The game's scripts read a number out of text with ParseInt, which is the C library's atoi:
// it stops at the first character that is not a digit. A Jam Session takes the score of the
// line just landed from the score HUD's own label (analysis/throwdowns-re/jam.md), so where
// that label groups its digits a 5,432 line is read as 5 and adds 5 to the player's total
// (a 1,250 one adds 1). This reads the same text and takes the groups as one number.
namespace dingosdk::throwdowns_text {
struct GroupedInt {
    int value{};
    bool grouped{}; // the text had digit groups, which atoi would have cut short
};

// atoi's reading of `text` (leading space, a sign, digits), going on through groups of
// exactly three digits set apart by one of , . ' a space, or a no-break, narrow no-break or
// thin space (UTF-8). "5,432" is 5432 and "1 234 567" is 1234567; "12.5", "5,43" and
// "1234,567" end where atoi ends them. Out of range saturates.
inline GroupedInt parse_grouped_int(const char *text) noexcept {
    if (!text) return {};
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    const auto digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) ++p;
    bool negative{};
    if (*p == '+' || *p == '-') negative = *p++ == '-';
    constexpr long long limit = static_cast<long long>(INT_MAX) + 1;
    long long value{};
    int first{};
    for (; digit(*p); ++p, ++first) value = value * 10 + (*p - '0') > limit ? limit : value * 10 + (*p - '0');
    GroupedInt result;
    // Only a number that starts with one to three digits is grouped.
    while (first >= 1 && first <= 3) {
        std::size_t separator{};
        if (*p == ',' || *p == '.' || *p == ' ' || *p == '\'') separator = 1;
        else if (p[0] == 0xC2 && p[1] == 0xA0) separator = 2;
        else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xAF || p[2] == 0x89)) separator = 3;
        if (!separator) break;
        const auto *group = p + separator;
        if (!digit(group[0]) || !digit(group[1]) || !digit(group[2]) || digit(group[3])) break;
        const auto thousands = (group[0] - '0') * 100 + (group[1] - '0') * 10 + (group[2] - '0');
        value = value * 1000 + thousands > limit ? limit : value * 1000 + thousands;
        p = group + 3;
        result.grouped = true;
    }
    if (negative) result.value = value >= limit ? INT_MIN : -static_cast<int>(value);
    else result.value = value >= limit ? INT_MAX : static_cast<int>(value);
    return result;
}
} // namespace dingosdk::throwdowns_text
