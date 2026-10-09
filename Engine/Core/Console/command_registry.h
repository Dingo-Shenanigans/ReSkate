#pragma once

#include "console_core.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <type_traits>

namespace dingosdk::console {
enum class Group {
    console,
    movement,
    gameplay,
    world,
    graphics,
    progression,
    objects,
    engine
};
enum class Kind {
    action,
    variable
};
enum class Execution {
    local,
    game_thread
};
enum class Type {
    text,
    boolean,
    integer,
    unsigned_integer,
    number
};
using Value = std::variant<std::string, bool, std::int64_t, std::uint64_t, double>;
using Values = std::vector<Value>;
struct Output {
    std::function<void(const std::string &)> write;
    std::function<void()> clear;
    std::function<void()> history;
    void operator()(const std::string &text) const {
        if (write)
            write(text);
    }
};
struct State {
    bool available = true;
    std::optional<std::string> value;
    std::string reason;
    std::string detail;
    bool overridden = false;
};
std::string lower(std::string_view text);
bool equal(std::string_view a, std::string_view b);
const char *group_name(Group group) noexcept;
std::string value_text(const Value &value);
std::optional<Value> parse_value(Type type, std::string_view text);

struct CompletionToken {
    std::string text;
    int begin = 0, end = 0;
    bool quoted = false;
};
std::vector<CompletionToken> completion_tokens(std::string_view input, int cursor);

struct Suggestion {
    std::string text, name, description, usage;
    Group group = Group::console;
    Kind kind = Kind::action;
    State state;
    bool argument = false;
};

template <class Context> class Registry {
  public:
    struct Argument {
        std::string name;
        Type type = Type::text;
        bool optional = false, rest = false;
        std::vector<std::string> choices;
        std::optional<double> minimum, maximum;
        std::function<std::vector<std::string>(const Context &, std::span<const std::string>)> complete;
    };
    struct Entry {
        std::string name, description;
        Group group = Group::console;
        Kind kind = Kind::action;
        Execution execution = Execution::game_thread;
        bool echo_input = true;
        std::vector<Argument> arguments;
        std::vector<std::string> aliases;
        std::function<State(const Context &)> inspect;
        std::function<void(const Context &, const Values &, const Output &)> run;
        std::function<void(const Context &, const Output &)> reset;
    };
    Registry() = default;
    Registry(const Registry &) = delete;
    Registry &operator=(const Registry &) = delete;

    void add(Entry entry) {
        const auto parsed = parse_console_command(entry.name);
        if (!parsed || parsed.arguments.empty())
            throw std::logic_error("Invalid command name");
        bool optional = false;
        for (std::size_t i = 0; i < entry.arguments.size(); ++i) {
            const auto &arg = entry.arguments[i];
            if (arg.name.empty() || (optional && !arg.optional) ||
                (arg.rest && (i + 1 != entry.arguments.size() || arg.type != Type::text)))
                throw std::logic_error("Invalid command argument schema: " + entry.name);
            optional = arg.optional;
        }
        if (entry.kind == Kind::variable && (entry.arguments.size() != 1 || !entry.inspect))
            throw std::logic_error("Variables require one typed argument and a state reader");
        std::vector<std::string> names{entry.name};
        names.insert(names.end(), entry.aliases.begin(), entry.aliases.end());
        std::map<std::string, bool> unique;
        for (const auto &name : names) {
            const auto key = lower(name);
            if (!parse_console_command(name) || name.empty() || lookup_.contains(key) ||
                !unique.emplace(key, true).second)
                throw std::logic_error("Duplicate/invalid command: " + name);
        }
        const auto index = entries_.size();
        entries_.push_back(std::move(entry));
        for (const auto &name : names)
            lookup_.emplace(lower(name), index);
    }
    const std::vector<Entry> &entries() const noexcept {
        return entries_;
    }
    const Entry *find(std::string_view name) const {
        const auto it = lookup_.find(lower(name));
        return it == lookup_.end() ? nullptr : &entries_[it->second];
    }
    static std::string usage(const Entry &entry) {
        std::string result = entry.name;
        for (const auto &arg : entry.arguments) {
            const bool optional = arg.optional || entry.kind == Kind::variable;
            result += optional ? " [" : " <";
            result += arg.name;
            if (arg.rest)
                result += "...";
            result += optional ? "]" : ">";
        }
        return result;
    }
    State state(const Entry &entry, const Context &context) const {
        return entry.inspect ? entry.inspect(context) : State{};
    }
    std::string describe(const Entry &entry, const Context &context) const {
        const auto current = state(entry, context);
        std::string result = entry.name;
        if (current.value)
            result += " [" + *current.value + "]";
        if (!entry.description.empty())
            result += " - " + entry.description;
        if (!current.available)
            result += " | unavailable: " + current.reason;
        if (!current.detail.empty())
            result += " | " + current.detail;
        return result;
    }
    bool echoes(std::span<const std::string> words) const {
        const auto [entry, consumed] = resolve(words);
        return !entry || entry->echo_input || consumed != words.size();
    }
    bool local(std::span<const std::string> words, const Context &context) const {
        const auto [entry, consumed] = resolve(words);
        return !entry || entry->execution == Execution::local ||
               (entry->kind == Kind::variable && consumed == words.size()) || !state(*entry, context).available;
    }
    bool execute(std::string_view text, const Context &context, const Output &output) const {
        const auto parsed = parse_console_command(text);
        if (!parsed || parsed.arguments.empty()) {
            output("error: Invalid command text; check quoting and length.");
            return false;
        }
        return execute(parsed.arguments, context, output);
    }
    bool execute(std::span<const std::string> words, const Context &context, const Output &output) const {
        const auto [entry, consumed] = resolve(words);
        if (!entry) {
            output("error: Unknown command. Use help or Tab completion.");
            return false;
        }
        const auto current = state(*entry, context);
        if (entry->kind == Kind::variable && consumed == words.size()) {
            output(describe(*entry, context));
            return true;
        }
        if (!current.available) {
            output("error: " + entry->name + " is unavailable: " + current.reason);
            return false;
        }
        const auto args = words.subspan(consumed);
        Values values;
        std::size_t index = 0;
        for (const auto &arg : entry->arguments) {
            if (index == args.size()) {
                if (arg.optional)
                    break;
                output("error: Missing " + arg.name + ". Usage: " + usage(*entry));
                return false;
            }
            std::string token = args[index++];
            if (arg.rest)
                while (index < args.size()) {
                    token += ' ';
                    token += args[index++];
                }
            // "toggle" on an on/off switch: the other of what it is now.
            if (arg.type == Type::boolean && equal(token, "toggle")) token = current.value && *current.value == "1" ? "0" : "1";
            auto value = parse_value(arg.type, token);
            if (!value) {
                output("error: Invalid " + arg.name + ": '" + token + "'. Usage: " + usage(*entry));
                return false;
            }
            if (!arg.choices.empty()) {
                const auto match = std::find_if(arg.choices.begin(), arg.choices.end(), [&](const auto &choice) {
                    if (arg.type == Type::text)
                        return equal(choice, token);
                    const auto parsed_choice = parse_value(arg.type, choice);
                    return parsed_choice && *parsed_choice == *value;
                });
                if (match == arg.choices.end()) {
                    std::string choices;
                    for (const auto &choice : arg.choices) {
                        if (!choices.empty())
                            choices += ", ";
                        choices += choice;
                    }
                    output("error: " + arg.name + " must be one of: " + choices);
                    return false;
                }
                value = parse_value(arg.type, *match);
            }
            if (arg.minimum || arg.maximum) {
                const auto numeric = std::visit(
                    [](const auto &v) -> double {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_arithmetic_v<T>)
                            return static_cast<double>(v);
                        else
                            return 0;
                    },
                    *value);
                if ((arg.minimum && numeric < *arg.minimum) || (arg.maximum && numeric > *arg.maximum)) {
                    output("error: " + arg.name + " is outside the allowed range. Use help " + entry->name);
                    return false;
                }
            }
            values.push_back(std::move(*value));
        }
        if (index != args.size()) {
            output("error: Too many arguments. Usage: " + usage(*entry));
            return false;
        }
        if (!entry->run) {
            output("error: " + entry->name + " is read-only.");
            return false;
        }
        entry->run(context, values, output);
        return true;
    }
    // words includes the active, possibly empty token; completion only reads Context.
    std::vector<Suggestion> complete(std::span<const std::string> words, const Context &context) const {
        std::vector<Suggestion> result;
        std::set<std::string> emitted;
        if (words.empty())
            return result;
        const auto prefix = words.back();
        const auto prior = words.first(words.size() - 1);
        auto add = [&](std::string text, const Entry &entry, bool argument) {
            if (!ascii_case_insensitive_starts_with(text, prefix))
                return;
            if (!emitted.insert(lower(text)).second)
                return;
            result.push_back({std::move(text), entry.name, entry.description, usage(entry), entry.group, entry.kind,
                              state(entry, context), argument});
        };
        for (const auto &item : entries_) {
            std::vector<std::string> routes{item.name};
            // Multiword aliases also supply the menu command routes.
            for (const auto &alias : item.aliases)
                if (alias.find(' ') != std::string::npos)
                    routes.push_back(alias);
            for (const auto &route : routes) {
                const auto path = parse_console_command(route).arguments;
                if (prior.size() >= path.size())
                    continue;
                bool matches = true;
                for (std::size_t i = 0; i < prior.size(); ++i)
                    matches = matches && equal(prior[i], path[i]);
                if (!matches)
                    continue;
                const auto before = result.size();
                add(path[prior.size()], item, false);
                if (result.size() == before)
                    continue;
                auto &suggestion = result.back();
                if (prior.size() + 1 < path.size()) {
                    suggestion.name = suggestion.text;
                    suggestion.description = "Browse " + suggestion.text + " commands";
                    suggestion.usage = route;
                    suggestion.kind = Kind::action;
                    suggestion.state = {};
                } else
                    suggestion.name = route;
            }
        }
        const auto [entry, consumed] = resolve(prior);
        if (entry) {
            const auto arg_index = prior.size() - consumed;
            if (arg_index < entry->arguments.size()) {
                const auto &arg = entry->arguments[arg_index];
                auto candidates = arg.complete ? arg.complete(context, prior.subspan(consumed)) : arg.choices;
                if (candidates.empty() && arg.type == Type::boolean)
                    candidates = {"0", "1"};
                for (const auto &candidate : candidates)
                    add(candidate, *entry, true);
            }
        }
        std::sort(result.begin(), result.end(),
                  [](const auto &a, const auto &b) { return lower(a.text) < lower(b.text); });
        if (result.size() > console_max_autocomplete_matches)
            result.resize(console_max_autocomplete_matches);
        return result;
    }

  private:
    std::pair<const Entry *, std::size_t> resolve(std::span<const std::string> words) const {
        const Entry *entry = nullptr;
        std::size_t consumed = 0;
        std::string path;
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (i)
                path += ' ';
            path += words[i];
            if (const auto *found = find(path)) {
                entry = found;
                consumed = i + 1;
            }
        }
        return {entry, consumed};
    }
    std::vector<Entry> entries_;
    std::map<std::string, std::size_t> lookup_;
};
} // namespace dingosdk::console
