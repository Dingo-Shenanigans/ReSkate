#include "trainer_presets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <string_view>
#include <utility>

namespace dingosdk::trainer {
bool value_used(std::uint16_t offset) {
    static constexpr std::uint16_t used[]{
#include "trainer_used.inc"
    };
    return std::binary_search(std::begin(used), std::end(used), offset);
}
// The game ignores these named values; the graphs on the right are what it reads. Linking them
// lets "ollie height" and "spin speed" be plain numbers instead of graph multipliers.
const std::vector<Link> &value_links() {
    static const std::vector<Link> links{
        {"physicsmode.jumpmaxheight", "physicsjump.maxheightvsspeed x"},
        {"physicsmode.jumpminheight", "physicsjump.minheightvsspeed x"},
        {"physicsairstates.maxspinspeed", "physicsbodyspin.propbodyspinvstime x"},
        {"physicsairstates.maxspinspeed", "physicsbodyspin.pbsvst_easy x"},
        {"physicsairstates.maxspinspeed", "physicsbodyspin.maxdeltavstime x"},
        {"physicsairstates.maxspinspeed", "physicsbodyspin.maxdeltavstimeeasy x"},
        // Pushing: the tuning's top pushing speed only gates whether a push may start. The speeds
        // pushes aim for are the push class's (trainer_classes.h), scaled here by the same ratio.
        {"physicspush.maxpushablespeed", "push.maxpushspeedlight"},
        {"physicspush.maxpushablespeed", "push.maxpushspeedmedium"},
        {"physicspush.maxpushablespeed", "push.maxpushspeedstrong"},
        {"physicspush.maxpushablespeed", "push.minmediumpushoverridespeed"},
    };
    return links;
}
// The two short lists of the Tune tab. Only values that do something: read by the game, linked
// above, or a field of one of the game's data-defined classes.
std::string_view essential_name(std::string_view key, int *rank, std::uint8_t *modes) {
    struct Name {
        std::string_view key, label;
        std::uint8_t modes;
    };
    constexpr std::uint8_t r = mode_realistic, f = mode_fun, both = mode_realistic | mode_fun;
    static constexpr Name names[]{
        {"physicsmode.jumpmaxheight", "Ollie height (max)", both},
        {"physicsmode.jumpminheight", "Ollie height (min, light pop)", both},
        {"physicsmode.grindjumpcommonmax", "Pop out of grinds (max)", both},
        {"physicsjump.jumpybonusmax", "Jump bonus (max)", f},
        {"physicspush.maxpushablespeed", "Push speed (9.25 = the game's; scales every push)", both},
        {"push.maxpushspeedlight", "Push: a tapped push settles at (m/s)", r},
        {"push.maxpushspeedmedium", "Push: a held push reaches (m/s)", r},
        {"push.maxpushspeedstrong", "Push: top speed (m/s)", r},
        {"physicsmode.autopushenabled", "Auto push (keeps a rolling skater going)", f},
        {"physicspush.maxspeedforautopush", "Auto push speed (m/s)", f},
        {"physicsreckoning.flipscalar", "Body flip speed", both},
        {"physicsreckoning.flipmaxspeed", "Body flip speed limit", both},
        {"physicsmode.perfectbodyflips", "Perfect body flips (exactly one rotation)", f},
        {"physicsairstates.maxspinspeed", "Body spin speed", both},
        {"physicsmode.maxautobodyspinspeed", "Auto body spin speed", both},
        {"physicsmode.easybodyspins", "Easy body spins", f},
        {"heldflip.straightflipcatchtime", "Flip catch time: kickflips and heelflips (s)", r},
        {"heldflip.shuvmincatchtime", "Flip catch time: shuvits (s)", r},
        {"heldflip.bigflipmincatchtime", "Flip catch time: big flips (s)", r},
        {"physicsmode.speedwobblestartspeed", "Speed wobble starts at (m/s)", both},
        {"physicsmode.grindlockdist", "Grind lock-on distance", both},
        {"physicsgrind.commonfrictionscalar", "Grind friction", both},
        {"physicsmode.makesurfacessmooth", "Smooth surfaces", f},
        {"onboard_powerslide.powerslide_forwardforcescalar_slide", "Powerslide: forward force", f},
        {"onboard_powerslide.frictionscalar_slide", "Powerslide: friction", both},
        {"physicsmode.wipeoutcheckforbadlanding", "Bail on bad landings", both},
        {"physicsmode.wipeout_groundxzacceleration", "Bail: sideways hit limit", both},
        {"wipeoutfallspeed.normalmaxspeed_ground", "Bail: fall speed limit on the ground (m/s)", r},
        {"wipeoutfallspeed.normalmaxspeed_grind", "Bail: fall speed limit in a grind (m/s)", r},
        {"jump.basejumpheight", "On foot: jump height (m)", both},
        {"jump.maxjumpheight", "On foot: highest jump (m)", both},
        {"jump.maxjumpvelocity", "On foot: jump speed limit (m/s)", f},
        {"jump.aircontrolforce", "On foot: steering in the air", f},
        {"locomotion.sprintspeed", "On foot: sprint speed (m/s)", both},
        {"locomotion.sprintspeedboost", "On foot: sprint boost speed (m/s)", f},
        {"flumping.flumptuckmaxrotationvelocity", "On foot: flip rotation speed", both},
        {"flumping.rollmaxvelocity", "On foot: roll speed (m/s)", f},
        {"flumping.rollvelocitymultiplier", "On foot: roll speed gain", f},
        {"wipeout.spreadeaglegravity", "Glide: gravity (-8; nearer 0 falls slower)", f},
        {"wipeout.spreadeagleairresistance", "Glide: air resistance", f},
        {"wipeout.spreadeagleaircontrolmax", "Glide: steering", f},
        {"wipeout.torpedoaircontrolmax", "Torpedo: steering (fast)", f},
        {"wipeout.defaultaircontrol", "Falling: steering", f},
        {"wipeout.defaultairresistance", "Falling: air resistance", f},
        {"wallrun.wallrunjumpupvelocityboost", "Wallrun: jump up boost", f},
        {"wallrun.wallrungravity", "Wallrun: gravity", f},
        {"physicstrucks.truckzposfront", "Front truck position (next respawn)", r},
        {"physicstrucks.truckzposback", "Back truck position (next respawn)", r},
    };
    const auto found = std::ranges::find(names, key, &Name::key);
    const bool known = found != std::end(names);
    if (rank) *rank = known ? static_cast<int>(found - std::begin(names)) + 1 : 0;
    if (modes) *modes = known ? found->modes : std::uint8_t{};
    return known ? found->label : std::string_view{};
}

PresetDial preset_dial(std::string_view name) {
    struct Dial {
        std::string_view name, title;
        std::uint8_t modes;
    };
    constexpr std::uint8_t r = mode_realistic, f = mode_fun, both = mode_realistic | mode_fun;
    static constexpr Dial dials[]{
        {"Super Ollie", "Ollie height", both},
        {"Fast", "Push speed", both},
        {"Fast Flips", "Body flip speed", both},
        {"Fast Spins", "Body spin speed", both},
        {"Hard To Bail", "Bail resistance (higher: harder to bail)", both},
        {"Sticky Grinds", "Grind lock-on", both},
        {"Slick Grinds", "Grind friction", both},
        {"Moon Jump", "On foot: jump height", both},
        {"Fast On Foot", "On foot: sprint speed", both},
        {"Fast Parkour Flips", "On foot: flip and roll speed", f},
        {"Super Glide", "Glide: gravity (lower falls slower)", f},
        {"Torpedo Boost", "Torpedo and falling: steering", f},
        {"Realistic", "", r},
        {"Hardcore", "", r},
        {"Mega Pop", "", 0},
        {"No Speed Wobble", "", f},
        {"Auto Push", "", f},
        {"Smooth Surfaces", "", f},
        {"Long Wheelbase", "", both},
    };
    const auto found = std::ranges::find(dials, name, &Dial::name);
    return found == std::end(dials) ? PresetDial{} : PresetDial{found->title, found->modes};
}

// Patterns are matched against the lower-case ids the game's own data gives its tuning
// (run `trainer dump` for the list), so a preset reaches every value a pattern names in
// whatever build is running and simply skips the ones that build lacks.
const std::vector<BuiltinPreset> &builtin_presets() {
    static const std::vector<BuiltinPreset> presets{
        // Rules name values the game was found to read (trainer_used.inc) or linked values
        // (value_links). Jump height comes from the PhysicsJump height graphs; body flips
        // from FlipScalar and FlipMaxSpeed unless PerfectBodyFlips forces exactly one rotation;
        // body spins from the PhysicsBodyspin graphs and MaxAutoBodySpinSpeed. The top pushing
        // speed is the trainer's own doing (set_push_top); the game skips its other push values.
        {"Super Ollie", "Huge ollies: about three times the height at any speed.",
         {{"physicsmode.jumpmaxheight", true, 3.0}, {"physicsmode.jumpminheight", true, 3.0},
          {"physicsjump.absoluteminheight", true, 2.0}, {"physicsjump.jumpybonusmax", true, 3.0},
          {"physicsmode.grindjump", true, 2.5}}},
        {"Fast Flips", "Front flips and back flips rotate three times as fast.",
         {{"physicsreckoning.flipscalar", true, 3.0}, {"physicsreckoning.flipmaxspeed", true, 3.0},
          {"physicsmode.perfectbodyflips", false, 0.0}}},
        {"Fast Spins", "Body spins rotate three times as fast.",
         {{"physicsairstates.maxspinspeed", true, 3.0}, {"physicsmode.maxautobodyspinspeed", true, 3.0}}},
        {"Realistic", "Lower pop, slower pushing and rotation, earlier speed wobble, easier to bail.",
         {{"physicsmode.jumpmaxheight", true, 0.75}, {"physicsmode.jumpminheight", true, 0.8},
          {"physicsmode.grindjump", true, 0.8}, {"physicspush.maxpushablespeed !camera", true, 0.75},
          {"physicsmode.speedwobblestartspeed", true, 0.7},
          {"physicsreckoning.flipscalar", true, 0.8}, {"physicsairstates.maxspinspeed", true, 0.75},
          {"physicsmode.maxautobodyspinspeed", true, 0.75}, {"physicsmode.grindlockdist", true, 0.7}, {"jump.basejumpheight", true, 0.85}, {"jump.maxjumpheight", true, 0.85},
          {"locomotion.sprintspeed", true, 0.85}, {"wipeoutfallspeed.", true, 0.8},
          {"physicswipeout.wipeout_ maxspeed", true, 0.8}, {"physicsmode.wipeout_ acceleration", true, 0.8}}},
        // Realistic with the game's help turned down, for players used to skating games without
        // it. Every value Realistic changes goes at least as far (Test/presets_tests.cpp). The
        // rest turn down what Sticky Grinds, Slick Grinds and Hard To Bail turn up, and raise
        // powerslide friction. Grinds still take a fast drop onto a rail: only the distance and
        // angle they lock on from shrink. The amounts are a starting point, not measured. Ollie
        // height is the first rule, so a dial of 0.75 is the preset (`trainer dial <x> hardcore`).
        {"Hardcore", "Realistic with less help: grinds only take a close, lined-up landing, grinds and powerslides lose speed, slower rotation, earlier bails.",
         {{"physicsmode.jumpmaxheight", true, 0.75}, {"physicsmode.jumpminheight", true, 0.8},
          {"physicsmode.grindjump", true, 0.8}, {"physicspush.maxpushablespeed !camera", true, 0.75},
          {"physicsmode.speedwobblestartspeed", true, 0.7},
          {"physicsreckoning.flipscalar", true, 0.7}, {"physicsairstates.maxspinspeed", true, 0.6},
          {"physicsmode.maxautobodyspinspeed", true, 0.5}, {"physicsmode.grindlockdist", true, 0.5}, {"jump.basejumpheight", true, 0.85}, {"jump.maxjumpheight", true, 0.85},
          {"locomotion.sprintspeed", true, 0.85}, {"wipeoutfallspeed.", true, 0.7},
          {"physicswipeout.wipeout_ maxspeed", true, 0.7}, {"physicsmode.wipeout_ acceleration", true, 0.7},
          {"physicstrajectory.grindlandingmaxangle", true, 0.6},
          {"physicsgrind.commonfrictionscalar", true, 1.3}, {"physicsgrind.curbfrictionscalar", true, 1.3},
          {"onboard_powerslide.frictionscalar_slide", true, 1.5},
          {"physicswipeout.wipeout_ force", true, 0.7}, {"physicswipeout.wipeout_ acceleration", true, 0.7},
          {"physicswipeout.wipeout_ maxdisp", true, 0.7}, {"physicswipeout.wipeout_ relativevel", true, 0.7},
          {"physicsmode.wipeoutcheckforbadlanding", false, 1.0}}},
        {"Mega Pop", "Ollies and grind pops go about twice as high.",
         {{"physicsmode.jumpmaxheight", true, 2.0}, {"physicsmode.jumpminheight", true, 1.6},
          {"physicsjump.jumpybonusmax", true, 2.0}, {"physicsmode.grindjump", true, 1.6}}},
        {"Fast", "Every push is 1.8 times as fast.",
         {{"physicspush.maxpushablespeed !camera", true, 1.8}, {"physicspush.maxspeedforautopush", true, 1.8},
          {"physicsmode.speedwobblestartspeed", true, 3.0}}},
        {"Moon Jump", "On foot: jumps about three times as high.",
         {{"jump.basejumpheight", true, 3.0}, {"jump.maxjumpheight", true, 3.0}, {"jump.maxjumpvelocity", true, 2.0}}},
        {"Fast On Foot", "On foot: sprint nearly twice as fast.",
         {{"locomotion.sprintspeed", true, 1.8}, {"locomotion.sprintspeedboost", true, 1.8}}},
        {"Fast Parkour Flips", "On foot: flips and rolls rotate and travel much faster.",
         {{"flumping.flumptuckmaxrotationvelocity", true, 2.5}, {"flumping.rollmaxvelocity", true, 2.0}, {"flumping.rollvelocitymultiplier", true, 1.5}}},
        {"Super Glide", "Spread-eagle falls slowly and steers hard.",
         {{"wipeout.spreadeaglegravity", true, 0.35}, {"wipeout.spreadeagleaircontrolmax", true, 2.0}, {"wipeout.spreadeagleaircontrolmin", true, 2.0}}},
        {"Torpedo Boost", "Torpedo steers and carries much harder, and so does a plain fall.",
         {{"wipeout.torpedoaircontrolmax", true, 2.5}, {"wipeout.defaultaircontrol", true, 2.0}}},
        {"No Speed Wobble", "The board stays steady at any speed.", {{"physicsmode.speedwobblestartspeed", true, 20.0}}},
        {"Auto Push", "Once rolling, the skater keeps gaining speed up to the auto push speed (8 m/s).", {{"physicsmode.autopushenabled", false, 1.0}}},
        {"Hard To Bail", "Much larger impacts are needed before a wipeout.",
         {{"physicswipeout.wipeout_ force", true, 3.0}, {"physicswipeout.wipeout_ acceleration", true, 3.0},
          {"physicswipeout.wipeout_ maxspeed", true, 2.5}, {"physicswipeout.wipeout_ maxdisp", true, 3.0}, {"physicswipeout.wipeout_ relativevel", true, 3.0},
          {"wipeout.on forcelimits", true, 3.0}, {"wipeout.runoutwipeoutforcelimits", true, 3.0},
          {"physicsmode.wipeout_ acceleration", true, 3.0}, {"physicsmode.wipeoutcheckforbadlanding", false, 0.0}}},
        {"Sticky Grinds", "Grinds lock on from further away and at higher speeds.",
         {{"physicsmode.grindlockdist", true, 2.0}, {"physicstrajectory.grindmaxspeedsqr", true, 4.0},
          {"physicstrajectory.grindmaxspeeddownontogrind", true, 2.0}, {"physicstrajectory.grindlandingmaxangle", true, 2.0}}},
        {"Slick Grinds", "Grinds and slides keep their speed.",
         {{"physicsgrind.commonfrictionscalar", true, 0.4}, {"physicsgrind.curbfrictionscalar", true, 0.4}}},
        {"Smooth Surfaces", "Rough ground rides like polished concrete.", {{"physicsmode.makesurfacessmooth", false, 1.0}}},
        {"Long Wheelbase", "Trucks 10 cm further out at both ends. Takes effect on the next respawn.",
         {{"physicstrucks.truckzposfront", false, 0.0143}, {"physicstrucks.truckzposback", false, 0.0143}}},
    };
    return presets;
}

namespace {
// Preset names compare as the console's commands do: ignoring ASCII case.
bool same_name(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}
} // namespace
bool reserved_preset_name(std::string_view name) {
    return same_name(name, "stock") || same_name(name, "map") ||
           std::ranges::any_of(builtin_presets(), [&](const BuiltinPreset &preset) { return same_name(name, preset.name); });
}
std::vector<std::pair<std::string, std::string>> rename_reserved_presets(UserPresets &user) {
    std::vector<std::string> hidden;
    for (const auto &entry : user)
        if (reserved_preset_name(entry.first)) hidden.push_back(entry.first);
    const auto taken = [&](std::string_view name) {
        return reserved_preset_name(name) || std::ranges::any_of(user, [&](const auto &entry) { return same_name(entry.first, name); });
    };
    std::vector<std::pair<std::string, std::string>> renamed;
    for (const auto &name : hidden) {
        // A reserved name is short, so the new one stays well inside max_preset_name.
        auto next = name + " (yours)";
        for (int n = 2; taken(next); ++n) next = name + " (yours " + std::to_string(n) + ")";
        auto node = user.extract(name);
        node.key() = next;
        user.insert(std::move(node));
        renamed.emplace_back(name, std::move(next));
    }
    return renamed;
}
} // namespace dingosdk::trainer
