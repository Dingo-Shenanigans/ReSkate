#include "Extension/Console/commands.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/World/world_entities.h"
namespace dingosdk::console {
void register_object_commands(Commands &registry) {
    // Test only: a per-body physics setter on plugins' world objects (world_entities.h test_bodies).
    auto body_test = action("objbody", "Test: run a physics body setter on plugins' world objects", Group::objects, {argument("setter")});
    body_test.run = [](const Model &, const Values &args, const Output &out) {
        world_entities::test_bodies(std::get<std::string>(args[0]));
        out("Queued; the log says which bodies it ran on.");
    };
    registry.add(std::move(body_test));
    auto enabled = variable("objects enabled", "Persist placed objects between sessions", Group::objects,
                            argument("0|1", Type::boolean));
    enabled.aliases = {"objectpersistence"};
    enabled.inspect = [](const Model &m) {
        return boolean_state(m.object_persistence.available, m.object_persistence.enabled,
                             "Object persistence is unavailable.");
    };
    enabled.run = [](const Model &, const Values &args, const Output &out) {
        out(set_local_object_persistence(std::get<bool>(args[0])) ? local_profile_object_persistence().status
                                                                  : "error: Object persistence could not be saved.");
    };
    registry.add(std::move(enabled));
    auto map = argument("map");
    map.complete = [](const Model &m, auto) {
        return m.object_persistence.map.empty() ? std::vector<std::string>{}
                                                : std::vector<std::string>{m.object_persistence.map};
    };
    auto clear = action("objects clear", "Clear placed objects from the specified map", Group::objects, {map});
    clear.inspect = [](const Model &m) {
        return State{m.object_persistence.available && m.object_persistence.can_clear && !m.object_persistence.busy,
                     {},
                     "Object clearing is unavailable or busy.",
                     {},
                     false};
    };
    clear.run = [](const Model &, const Values &args, const Output &out) {
        out(clear_local_persisted_objects(std::get<std::string>(args[0])) ? local_profile_object_persistence().status
                                                                          : "error: Objects could not be cleared.");
    };
    registry.add(std::move(clear));
    auto row = argument("row_token", Type::unsigned_integer);
    row.minimum = 1;
    row.complete = [](const Model &m, auto args) {
        std::vector<std::string> result;
        if (!args.empty() && equal(args[0], m.object_persistence.map))
            for (const auto &item : m.object_persistence.rows)
                result.push_back(std::to_string(item.token));
        return result;
    };
    for (const auto &operation : {"delete", "tp"}) {
        auto entry = action("objects " + std::string(operation),
                            equal(operation, "delete") ? "Delete one placed object using its current row token"
                                                       : "Teleport to a placed object using its current row token",
                            Group::objects, {map, row});
        entry.inspect = [](const Model &m) {
            return State{m.object_persistence.available && !m.object_persistence.busy,
                         {},
                         "Object controls are unavailable or busy.",
                         {},
                         false};
        };
        entry.run = [remove = equal(operation, "delete")](const Model &, const Values &args, const Output &out) {
            const auto &map = std::get<std::string>(args[0]);
            const auto token = std::get<std::uint64_t>(args[1]);
            const bool ok =
                remove ? delete_local_placed_object(map, token) : teleport_to_local_placed_object(map, token);
            out(ok ? local_profile_object_persistence().status
                   : "error: Object action unavailable or row token is stale.");
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
