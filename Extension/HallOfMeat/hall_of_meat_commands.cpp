#include "hall_of_meat.h"
#include "Extension/Console/commands.h"

// The `hallofmeat` console switch; the menu's (Custom Stuff > Player) sends it too.
namespace dingosdk::console {
void register_hall_of_meat_commands(Commands &registry) {
    auto meat = variable("hallofmeat", "Show the bones a bail hurt, bruised yellow and broken red, and its Meat score",
                         Group::gameplay, argument("0|1", Type::boolean));
    meat.inspect = [](const Model &) {
        return boolean_state(hall_of_meat::available(), hall_of_meat::enabled(), "Hall of Meat is unavailable for this game build.");
    };
    meat.run = [](const Model &, const Values &args, const Output &) { hall_of_meat::set_enabled(std::get<bool>(args[0])); };
    registry.add(std::move(meat));
}
} // namespace dingosdk::console
