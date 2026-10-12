// No game access, so tests link it.
#include "commands.h"

namespace dingosdk::console {
Argument argument(std::string name, Type type, bool optional) {
    Argument result;
    result.name = std::move(name);
    result.type = type;
    result.optional = optional;
    return result;
}
Entry action(std::string name, std::string description, Group group, std::vector<Argument> args) {
    Entry entry;
    entry.name = std::move(name);
    entry.description = std::move(description);
    entry.group = group;
    entry.arguments = std::move(args);
    return entry;
}
Entry variable(std::string name, std::string description, Group group, Argument arg) {
    auto entry = action(std::move(name), std::move(description), group, {std::move(arg)});
    entry.kind = Kind::variable;
    return entry;
}
State boolean_state(bool available, bool value, std::string reason, std::string detail) {
    return {available, available ? std::optional<std::string>(value ? "1" : "0") : std::nullopt, std::move(reason),
            std::move(detail), false};
}
} // namespace dingosdk::console
