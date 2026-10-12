#include "command_registry.h"
#include <array>
namespace dingosdk::console {
std::vector<CompletionToken> completion_tokens(std::string_view input, int cursor) {
    const int size = static_cast<int>(input.size());
    const int limit = std::clamp(cursor, 0, size);
    std::vector<CompletionToken> tokens;
    int position = 0;
    const auto separator = [](char c) { return c == ' ' || c == '\t'; };
    while (position <= limit) {
        while (position < limit && separator(input[position]))
            ++position;
        CompletionToken token;
        token.begin = position;
        char quote = 0;
        bool escaped = false;
        // Scan the whole token, but decode only its prefix before the cursor.
        // This matches execution's escapes and single/double/concatenated quotes.
        for (; position < size; ++position) {
            const char c = input[position];
            if (!escaped && !quote && separator(c))
                break;
            const bool before_cursor = position < limit;
            if (escaped) {
                if (before_cursor)
                    token.text += c;
                escaped = false;
            } else if (c == '\\')
                escaped = true;
            else if (quote) {
                if (c == quote)
                    quote = 0;
                else if (before_cursor)
                    token.text += c;
            } else if (c == '\'' || c == '"') {
                quote = c;
                token.quoted = true;
            } else if (before_cursor)
                token.text += c;
        }
        token.end = position;
        tokens.push_back(std::move(token));
        if (position >= limit)
            break;
        ++position;
    }
    return tokens;
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto &c : result)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return result;
}
bool equal(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && ascii_case_insensitive_starts_with(a, b);
}
const char *group_name(Group group) noexcept {
    constexpr std::array names{"Console",     "Movement", "Gameplay", "World",
                               "Graphics",    "Progression", "Objects", "Engine"};
    const auto i = static_cast<std::size_t>(group);
    return i < names.size() ? names[i] : "Console";
}
std::string value_text(const Value &value) {
    return std::visit(
        [](const auto &v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "1" : "0";
            else {
                std::array<char, 64> text{};
                const auto result = std::to_chars(text.data(), text.data() + text.size(), v);
                return result.ec == std::errc{} ? std::string(text.data(), result.ptr) : "?";
            }
        },
        value);
}
std::optional<Value> parse_value(Type type, std::string_view text) {
    if (type == Type::text)
        return std::string(text);
    if (type == Type::boolean) {
        if (text == "1" || equal(text, "true") || equal(text, "on"))
            return true;
        if (text == "0" || equal(text, "false") || equal(text, "off"))
            return false;
        return std::nullopt;
    }
    const auto parse = [&]<class T>() -> std::optional<Value> {
        T value{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            return std::nullopt;
        if constexpr (std::is_floating_point_v<T>)
            if (!std::isfinite(value))
                return std::nullopt;
        return value;
    };
    switch (type) {
    case Type::integer:
        return parse.operator()<std::int64_t>();
    case Type::unsigned_integer:
        return parse.operator()<std::uint64_t>();
    case Type::number:
        return parse.operator()<double>();
    default:
        return std::nullopt;
    }
}
} // namespace dingosdk::console
