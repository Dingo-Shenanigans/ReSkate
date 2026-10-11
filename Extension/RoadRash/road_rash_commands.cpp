#include "road_rash.h"
#include "Extension/Console/commands.h"

// Road Rash's console switches; the menu's (Mod Options > Player) send them too.
namespace dingosdk::console {
void register_road_rash_commands(Commands &registry) {
    const auto unavailable = "Road Rash is unavailable for this game build.";
    auto rash = variable("roadrash", "Bails leave marks on your skater that build up: dust, scrapes, bruises, wounds "
                         "(needs the Road Rash item pack)",
                         Group::gameplay, argument("0|1", Type::boolean));
    rash.inspect = [unavailable](const Model &) {
        return boolean_state(road_rash::available(), road_rash::enabled(), unavailable,
                             std::to_string(road_rash::bails()) + " bail(s) counted");
    };
    rash.run = [](const Model &, const Values &args, const Output &) { road_rash::set_enabled(std::get<bool>(args[0])); };
    registry.add(std::move(rash));

    auto blood = variable("roadrashblood", "Fresh blood on Road Rash's worst wounds; off, they are dry and scabbed",
                          Group::gameplay, argument("0|1", Type::boolean));
    blood.inspect = [unavailable](const Model &) { return boolean_state(road_rash::available(), road_rash::blood(), unavailable); };
    blood.run = [](const Model &, const Values &args, const Output &) { road_rash::set_blood(std::get<bool>(args[0])); };
    registry.add(std::move(blood));

    auto tattoos = variable("roadrashtattoos", "Road Rash's wounds take the place of your own tattoos and moles until you heal; "
                            "off, a limb that wears a tattoo gets no wounds",
                            Group::gameplay, argument("0|1", Type::boolean));
    tattoos.inspect = [unavailable](const Model &) {
        return boolean_state(road_rash::available(), road_rash::covers_tattoos(), unavailable);
    };
    tattoos.run = [](const Model &, const Values &args, const Output &) { road_rash::set_covers_tattoos(std::get<bool>(args[0])); };
    registry.add(std::move(tattoos));

    auto heal = action("roadrash heal", "Take Road Rash's marks off and start counting bails again", Group::gameplay);
    heal.run = [](const Model &, const Values &, const Output &out) {
        road_rash::heal();
        out("Healed.");
    };
    registry.add(std::move(heal));

    auto bails = action("roadrash bails", "Count as if you had bailed this often, to look at a stage (10, 14 and 20 are the steps)",
                        Group::gameplay, {argument("count", Type::unsigned_integer)});
    bails.run = [](const Model &, const Values &args, const Output &out) {
        const auto count = std::get<std::uint64_t>(args[0]);
        road_rash::set_bails(static_cast<unsigned>(count > 1000 ? 1000 : count));
        out("Counted.");
    };
    registry.add(std::move(bails));
}
} // namespace dingosdk::console
