#include "trainer_presets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <map>
#include <span>
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
        // On foot: the jump height is cut to the highest and lowest jump (measured 2026-10-13: base 3 m rose 1.32 m until the
        // highest jump was raised with it), so both move with it.
        {"jump.basejumpheight", "jump.maxjumpheight"},
        {"jump.basejumpheight", "jump.minjumpheight"},
    };
    return links;
}
// The named values of the Tune tab's sections. Only values that do something: read by the game, linked
// above, or a field of one of the game's data-defined classes.
std::string_view essential_name(std::string_view key, int *rank, std::uint8_t *topic, bool *trial) {
    struct Name {
        std::string_view key, label;
        std::uint8_t topic;
        bool trial{}; // named for players to try although the game was not seen reading it
    };
    constexpr std::uint8_t pop = topic_pop, speed = topic_speed, bails = topic_bails, grinds = topic_grinds, trickline = topic_trickline, transition = topic_transition,
                           pumping = topic_pumping, foot = topic_foot;
    static constexpr Name names[]{
        {"physicsmode.jumpmaxheight", "Ollie height (max)", pop},
        {"physicsmode.jumpminheight", "Ollie height (min, light pop)", pop},
        {"physicsmode.grindjumpcommonmax", "Pop out of grinds (max)", grinds},
        {"physicsmode.grindjumpcommonmin", "Pop out of grinds (min, a quick pop)", grinds},
        {"physicsmode.grindjumpboardslidemax", "Pop out of board slides (max)", grinds},
        {"physicsmode.grindjumpboardslidemin", "Pop out of board slides (min, a quick pop)", grinds},
        {"physicsmode.grindjumptipslidemax", "Pop out of nose and tail slides (max)", grinds},
        {"physicsmode.grindjumptipslidemin", "Pop out of nose and tail slides (min, a quick pop)", grinds},
        {"physicsmode.jumpminheightmanual", "Pop out of a manual (min)", pop, true},
        {"physicsmode.jumpdeltaminheightmanual", "Pop out of a manual: extra for a held pop", pop, true},
        {"physicsonboardjumptuning.jumpadjustvelocityboostlimitground", "Ollie: speed the stick can add (m/s)", pop},
        {"physicsonboardjumptuning.jumpadjustvelocityboostlimitgrind", "Pop out of a grind: speed the stick can add (m/s)", grinds},
        {"physicstrajectory.grindlandingvelscalar", "Grinds: speed kept landing on one", grinds},
        {"onboard_speedmodel.gravityacceleration", "On board: gravity (9.81 m/s2)", speed},
        {"onboard_speedmodel.maxgravityacceleration", "On board: gravity limit (m/s2)", speed},
        {"speedmodel.maxpositiveacceleration", "On board: fastest speeding up (m/s2)", speed},
        {"speedmodel.maxnegativeacceleration", "On board: fastest slowing down (m/s2)", speed},
        {"onboard_speedmodel.noinputtime", "On board: seconds of rolling before friction grows", speed},
        {"physicsairstates.mountrunlaunchz", "Get on board from a jog: forward speed (m/s)", speed, true},
        {"physicsairstates.mountrunlaunchy", "Get on board from a jog: hop (m/s)", speed, true},
        {"physicsairstates.mountsprintlaunchz", "Get on board from a sprint: forward speed (m/s)", speed, true},
        {"physicsairstates.mountsprintlaunchy", "Get on board from a sprint: hop (m/s)", speed, true},
        {"physicsairstates.minbodyfliptimevsupy x", "Body flips: air time needed (x)", pop, true},
        {"physicsairstates.bodyflipmingrabtimefraction", "Body flips: share of the air time held in a grab", pop, true},
        {"physicsfeet.maxlandingforce", "Landing: leg force limit", bails, true},
        {"physicsfeet.landingforcetime", "Landing: time the legs push back (s)", bails, true},
        {"physicsmode.wipeoutbadlandingscalar", "Bail on bad landings: strictness", bails},
        {"physicsfootplant.wipeoutmaxlandingspeeddown", "Bail: landing speed limit, downward (m/s)", bails},
        {"physicsfootplant.wipeoutmaxlandingspeedhoriz", "Bail: landing speed limit, sideways (m/s)", bails},
        {"onboard_powerslide.frictionscalar_revert", "Revert: friction", trickline},
        {"onboard_powerslide.frictionscalar_autorevert", "Auto revert: friction", trickline},
        {"onboard_powerslide.powerslide_forwardforcescalar_revert", "Revert: forward force", trickline},
        {"onboard_powerslide.powerslide_forwardforcescalar_autorevert", "Auto revert: forward force", trickline},
        {"physicsmode.pumpeffectfactor", "Pumping: strength", pumping},
        {"physicsmode.pumpmaxacceleration", "Pumping: fastest speed gain (m/s2)", pumping},
        {"physicsmode.unintentionalpumpscalar", "Pumping: speed gained without pumping", pumping},
        {"physicsjump.jumpybonusmax", "Jump bonus (max)", pop},
        {"physicspush.maxpushablespeed", "Push speed (9.25 = the game's; scales every push)", speed},
        {"push.maxpushspeedlight", "Push: a tapped push settles at (m/s)", speed},
        {"push.maxpushspeedmedium", "Push: a held push reaches (m/s)", speed},
        {"push.maxpushspeedstrong", "Push: top speed (m/s)", speed},
        {"physicsmode.autopushenabled", "Auto push (keeps a rolling skater going)", speed},
        {"physicspush.maxspeedforautopush", "Auto push speed (m/s)", speed},
        {"physicsreckoning.flipscalar", "Body flip speed", pop},
        {"physicsreckoning.flipmaxspeed", "Body flip speed limit", pop},
        {"physicsmode.perfectbodyflips", "Perfect body flips (exactly one rotation)", pop},
        {"physicsairstates.maxspinspeed", "Body spin speed", pop},
        {"physicsmode.maxautobodyspinspeed", "Auto body spin speed", pop},
        {"physicsmode.easybodyspins", "Easy body spins", pop},
        {"heldflip.straightflipcatchtime", "Flip catch time: kickflips and heelflips (s)", pop},
        {"heldflip.shuvmincatchtime", "Flip catch time: shuvits (s)", pop},
        {"heldflip.bigflipmincatchtime", "Flip catch time: big flips (s)", pop},
        {"physicsmode.speedwobblestartspeed", "Speed wobble starts at (m/s)", speed},
        {"physicsmode.grindlockdist", "Grind lock-on distance", grinds},
        {"physicsgrindsair.maxdistboardslide", "Grind lock-on: reach for board slides (m)", grinds},
        {"physicsgrindsair.maxdisttipslide", "Grind lock-on: reach for nose and tail slides (m)", grinds},
        {"physicsgrind.grind_tipslide_minangletoprimitive", "Nose and tail slides: least angle to the rail (deg)", grinds},
        {"physicsgrind.slowgrindexitspeed", "Grinds end below this speed (m/s; -1 never)", grinds, true},
        {"physicsgrind.copingexitstarttime", "Coping grinds may end after (s; -1 never)", transition, true},
        {"physicsgrind.curbexitstarttime", "Curb grinds may end after (s; -1 never)", grinds, true},
        {"physicsgrind.fairlysteepexitstarttime", "Steep grinds may end after (s; -1 never)", transition, true},
        {"physicsgrind.commonfrictionscalar", "Grind friction", grinds},
        {"physicsmode.makesurfacessmooth", "Smooth surfaces", speed},
        {"onboard_powerslide.powerslide_forwardforcescalar_slide", "Powerslide: forward force", speed},
        {"onboard_powerslide.frictionscalar_slide", "Powerslide: friction", speed},
        {"physicsmode.wipeoutcheckforbadlanding", "Bail on bad landings", bails},
        {"physicsmode.wipeout_groundxzacceleration", "Bail: sideways hit limit", bails},
        {"wipeoutfallspeed.normalmaxspeed_ground", "Bail: fall speed limit on the ground (m/s)", bails},
        {"wipeoutfallspeed.normalmaxspeed_grind", "Bail: fall speed limit in a grind (m/s)", grinds},
        {"jump.basejumpheight", "On foot: jump height (m)", foot},
        {"jump.maxjumpheight", "On foot: highest jump (m)", foot},
        {"locomotion.sprintspeed", "On foot: sprint speed (m/s)", foot},
        {"wallie.maxspeed", "Wallie: fastest speed it works at (m/s)", pop},
        {"wallie.heightboostpermetre", "Wallie: extra pop per metre of obstacle", pop},
        {"wallie.heightboostmax", "Wallie: most extra pop (m/s)", pop},
        {"wallie.boostminheight", "Wallie: lowest obstacle that adds pop (m)", pop},
        {"wallie.windowframes", "Wallie: how early the pop may come (frames)", pop},
        {"wallie.windowframestall", "Wallie: how early the pop may come, tall obstacle (frames)", pop},
        {"wallie.scanminheight", "Wallie: lowest obstacle it looks for (m)", pop},
        {"wallie.backjump", "Wallie: forward push (m/s)", pop},
        {"footflip.rollspeedmax", "On foot: flip speed early in a jump (rad/s)", foot},
        {"footflip.rollspeedmin", "On foot: flip speed late in a jump (rad/s)", foot},
        {"footspin.spinspeedmax", "On foot: spin speed in a tuck (rad/s)", foot},
        {"footspin.spinspeedfalloff", "On foot: spin speed lost while flipping (rad/s)", foot},
        {"wipeout.spreadeaglegravity", "Glide: gravity (-8; nearer 0 falls slower)", foot},
        {"wipeout.spreadeagleairresistance", "Glide: air resistance", foot},
        {"wipeout.spreadeagleaircontrolmax", "Glide: steering", foot},
        {"wipeout.torpedoaircontrolmax", "Torpedo: steering (fast)", foot},
        {"wipeout.defaultaircontrol", "Falling: steering", foot},
        {"wipeout.defaultairresistance", "Falling: air resistance", foot},
        {"wallrun.wallrunjumpupvelocityboost", "Wallrun: jump up boost", foot},
        {"wallrun.wallrungravity", "Wallrun: gravity", foot},
        {"physicstrucks.truckzposfront", "Front truck position (next respawn)", speed},
        {"physicstrucks.truckzposback", "Back truck position (next respawn)", speed},
        {"physicsgrind.nonpopexitangledeg", "Leaving a grind without a pop: angle (deg)", grinds},
        {"physicsgrind.grindreckoningsmoothnessonexit", "Leaving a grind: how slowly the body follows the board (0 to 1)", grinds},
        {"physicsgrind.grindreckoningsmoothnessdropin", "Dropping in off a grind: how slowly the body follows (0 to 1)", transition},
        {"physicshandplant.exitanglerangestart", "Handplant exit: directions tried from (deg off the coping)", transition},
        {"physicshandplant.exitanglerangeend", "Handplant exit: directions tried up to (deg; 90 = straight in)", transition},
        {"physicshandplant.minnormalvel", "Handplant exit: least speed into the ramp (m/s)", transition},
        {"physicshandplant.maxnormalvel", "Handplant exit: most speed into the ramp (m/s)", transition},
        {"physicshandplant.maxparallelvel", "Handplant exit: most speed kept along the coping (m/s)", transition},
        {"physicshandplant.maxverticalvel", "Handplant exit: most vertical speed (m/s)", transition},
        {"physicshandplant.minlipclearance", "Handplant: least height above the lip (m)", transition},
        {"physicshandplant.maxlipclearance", "Handplant: most height above the lip (m)", transition},
        {"physicshandplant.maxangleofapproach", "Handplant: widest approach to the lip (deg)", transition},
        {"physicshandplant.waitforanimtime", "Handplant: wait for the animation (s)", transition},
        {"physicsliptrick.liptrickcomapexheight", "Lip trick: body height above the lip (m)", transition},
        {"physicsliptrick.lipquerymaxlipdistance", "Lip trick: how far ahead a lip is looked for (m)", transition},
        {"physicsliptrick.lipquerymaxdirectionchangedegrees", "Lip trick: most turn to reach the lip (deg)", transition},
        {"physicsliptrick.launchgroundslopethreshold", "Lip trick: least slope to start from (deg)", transition},
        {"physicsliptrick.lipquerylipslopethreshold", "Lip trick: steepest lip that counts (deg)", transition},
        {"physicsliptrick.lipquerygroundslopethreshold", "Lip trick: least slope under the lip (deg)", transition},
        {"physicsliptrick.maxrotationpersecond", "Lip trick: fastest board rotation", transition},
        {"physicsrollin.maxrollintime", "Roll in: longest it may take (s)", transition},
        {"physicsrollin.skateboardrollinduration", "Roll in: time the board takes to tip in (s)", transition},
        {"physicsrollin.minlipbashdownwardspeed", "Roll in: downward speed that counts as hitting the lip (m/s)", transition},
        {"physicsrollin.undershootamount", "Roll in: how far short of the wall it aims", transition},
        {"physicsreckoning.vertjumpalignmaxgroundnormaly", "Vert pop: how steep a wall must be (0 = wall, 1 = flat)", transition},
        {"physicsreckoning.vertjumpalignmaxangle", "Vert pop: how far the stick tips it in or out", transition},
        {"physicsreckoning.vertjumpalignminjumpdiry", "Vert pop: how upward the pop must point", transition},
        {"physicsreckoning.vertjumpalignfactor", "Vert pop: strength of the straightening", transition},
        {"physicstrajectory.naturalairoffvertsscalar", "Rolling off a vert lip without a pop: speed kept out from the wall", transition},
        {"physicstrajectory.scoretransition", "Landing choice: weight of coming back into the transition", transition},
        {"physicstrajectory.mintimeafterapexfortransition", "Landing choice: soonest after the top of the air (s)", transition},
        {"physicstrajectory.scoregrind", "Landing choice: weight of an edge to grind", grinds},
        {"physicstrajectory.scoremiddlebonus", "Landing choice: bonus for the middle of the fan", transition},
        {"physicstrajectory.scorelandingdirection", "Landing choice: weight of facing the way you travel", transition},
        {"physicstrajectory.maxtrajectoryadjust", "In the air: most speed added to reach the landing (m/s)", transition},
        {"physicstrajectory.coneanglex", "Landing search: fan width up and down (deg)", transition},
        {"physicstrajectory.coneanglez", "Landing search: fan width side to side (deg)", transition},
        {"physicstrajectory.mintimefortrajectorytobevalid", "Shortest air that gets a planned landing (s)", transition},
        {"physicsairstates.naturalairmaxdist", "Rolling off an edge: longest gap still ridden over (m)", transition},
        {"physicsgrindsair.maxdist50_50", "Grind lock-on: reach for 50-50s (m)", grinds},
        {"physicsgrindsair.maxdist5_0", "Grind lock-on: reach for 5-0s and nose grinds (m)", grinds},
        {"physicstrajectory.grindlandingmaxangle", "Grind lock-on: widest angle onto the edge (deg)", grinds},
        {"physicspumping.pumpoutoftransitionscalar", "Pumping: push out of the top of a transition", pumping},
        {"physicsantipumping.maxdownwardangledeg", "Anti-pump: steepest downward push that costs speed (deg)", pumping},
    };
    const auto found = std::ranges::find(names, key, &Name::key);
    const bool known = found != std::end(names);
    if (rank) *rank = known ? static_cast<int>(found - std::begin(names)) + 1 : 0;
    if (topic) *topic = known ? found->topic : std::uint8_t{};
    if (trial) *trial = known && found->trial;
    return known ? found->label : std::string_view{};
}

// One plain sentence per named value. Kept beside the names so the two stay in step.
std::string_view essential_help(std::string_view key) {
    struct Help {
        std::string_view key, text;
    };
    static constexpr Help help[]{
        {"physicsmode.jumpmaxheight", "How high the skater's body rises on a full, held ollie, in metres. 1.575 is the game's own; Skate 3 used 1.71."},
        {"physicsmode.jumpminheight", "How high a quick tap of an ollie goes, in metres. Bring it close to the max and every ollie is the same height."},
        {"physicsmode.grindjumpcommonmax", "The highest pop out of a truck grind (50-50, 5-0, nosegrind...), in metres."},
        {"physicsmode.grindjumpcommonmin", "The lowest pop out of a truck grind: what a quick flick gives. A pop is never lower than this."},
        {"physicsmode.grindjumpboardslidemax", "The highest pop out of a boardslide or lipslide, in metres."},
        {"physicsmode.grindjumpboardslidemin", "The lowest pop out of a boardslide or lipslide: what a quick flick gives."},
        {"physicsmode.grindjumptipslidemax", "The highest pop out of a noseslide or tailslide, in metres."},
        {"physicsmode.grindjumptipslidemin", "The lowest pop out of a noseslide or tailslide: what a quick flick gives."},
        {"physicsmode.jumpminheightmanual", "The pop height out of a manual, in metres. Not confirmed to do anything yet: try it and tell us."},
        {"physicsonboardjumptuning.jumpadjustvelocityboostlimitground", "Pushing the left stick as you pop adds speed that way; this is the most it adds, in m/s."},
        {"physicsonboardjumptuning.jumpadjustvelocityboostlimitgrind", "The same stick boost when popping out of a grind. It is lower than on the ground, which is why a pop out of a grind can feel slower."},
        {"physicspush.maxpushablespeed", "The speed pushing can take you to, in m/s (1 m/s = 3.6 km/h). Every kind of push scales with it."},
        {"onboard_speedmodel.gravityacceleration", "Named as the gravity the board's speed model uses, in m/s2. Measured with a scripted controller: x 0.5 and x 2 changed neither an ollie's air time nor the speed gained rolling down a gentle slope, so its dial was removed."},
        {"onboard_speedmodel.maxgravityacceleration", "The most gravity may speed the board up down a slope, in m/s2. Skate 3 capped this at 7."},
        {"physicsairstates.mountrunlaunchz", "How fast you roll away after pressing Y to get on the board while jogging, in m/s. Not confirmed to do anything yet."},
        {"physicsairstates.mountsprintlaunchz", "How fast you roll away after pressing Y to get on the board while sprinting, in m/s. Not confirmed to do anything yet."},
        {"physicsairstates.minbodyfliptimevsupy x", "A body flip needs this much air time. x 0.5 halves it, so flips start from lower pops. Not confirmed to do anything yet."},
        {"physicsreckoning.flipscalar", "How fast a front flip or back flip rotates. 1 is the game's own; Skate 3 used 1.25."},
        {"physicsreckoning.flipmaxspeed", "The fastest a body flip may rotate. Skate 3 used 5, half of this game's 10."},
        {"physicsairstates.maxspinspeed", "How fast the skater's body spins in the air, in degrees per second at full stick."},
        {"physicsmode.speedwobblestartspeed", "The speed where the board starts to wobble, in m/s. Higher: stable for longer."},
        {"physicsmode.grindlockdist", "How far from a rail or ledge the board still snaps onto it, in metres. Skate 3: 0.9 normal, 0.15 hardcore."},
        {"physicsgrindsair.maxdistboardslide", "How far away a board slide still locks on, in metres. Skate 3 used 0.75: you had to be closer."},
        {"physicsgrindsair.maxdisttipslide", "How far away a nose or tail slide still locks on, in metres. Skate 3 used 0.7."},
        {"physicsgrind.grind_tipslide_minangletoprimitive", "How far the board must be turned to the rail before a grind is a nose or tail slide. The older value was 25."},
        {"physicsgrind.slowgrindexitspeed", "A grind slower than this ends by itself. -1 (skate.) never does; the older value was 0.15 m/s. Not confirmed to do anything yet."},
        {"physicsgrind.copingexitstarttime", "Seconds before the game may end a grind on coping. -1 (skate.) never; the older value was 0.1. Not confirmed to do anything yet."},
        {"physicsgrind.curbexitstarttime", "Seconds before the game may end a grind on a curb. -1 (skate.) never; the older value was 2. Not confirmed to do anything yet."},
        {"physicsgrind.fairlysteepexitstarttime", "Seconds before the game may end a grind on a steep rail. -1 (skate.) never; the older value was 0.8. Not confirmed to do anything yet."},
        {"physicsgrind.commonfrictionscalar", "How quickly a grind loses speed. Lower slides further."},
        {"onboard_powerslide.frictionscalar_slide", "How hard a powerslide brakes. Higher stops sooner."},
        {"onboard_powerslide.frictionscalar_revert", "How much speed a revert costs. Higher loses more; 0 loses none."},
        {"onboard_powerslide.frictionscalar_autorevert", "How much speed the automatic revert after a spin landing costs."},
        {"onboard_powerslide.powerslide_forwardforcescalar_revert", "A push forward during a revert. Higher gains speed out of reverts."},
        {"physicsmode.pumpeffectfactor", "How much speed pumping a transition gives. 13 is the game's own; Skate 3 used 18."},
        {"physicsmode.pumpmaxacceleration", "The fastest pumping may speed you up. 13 is the game's own; Skate 3 used 10."},
        {"physicsmode.wipeoutbadlandingscalar", "How strict the game is about landing crooked. The switch above turns the check off altogether."},
        {"physicsfootplant.wipeoutmaxlandingspeeddown", "Land falling faster than this (m/s) and you bail. Skate 3 used 10."},
        {"physicsfootplant.wipeoutmaxlandingspeedhoriz", "Land with more sideways speed than this (m/s) and you bail. Skate 3 used 11."},
        {"physicsmode.wipeout_groundxzacceleration", "How hard a sideways hit has to be to knock you off. Higher: harder to bail."},
        {"jump.basejumpheight", "How high a jump on foot goes, in metres."},
        {"wallie.maxspeed", "A wallie only goes below this speed. 11 is the game's own; raise it to wallie at full speed."},
        {"wallie.heightboostpermetre", "How much upward speed the wallie adds for each metre of the kerb, ledge or wall it climbs. 1.3 is the game's own."},
        {"wallie.heightboostmax", "The most upward speed a wallie may add, however tall the obstacle. 2.6 is the game's own."},
        {"wallie.boostminheight", "Obstacles lower than this give a plain pop with no extra height. 0.3 is the game's own."},
        {"wallie.windowframes", "How many frames (60 a second) before the board reaches a kerb, step or ledge the pop may come and still be a wallie. 16 is the game's own; at 120 a pop well before the steps still wallies."},
        {"wallie.windowframestall", "The same window for obstacles over 0.6 m. 18 is the game's own."},
        {"wallie.scanminheight", "Anything lower than this is not looked at as something to wallie. 0.225 is the game's own; at 0.6 the steps of a building front no longer wallie."},
        {"wallie.backjump", "Speed the wallie adds along the way you ride as it climbs. -0.2 is the game's own (a slight check); at 3 the skater went over at 5.7 m/s against 4.2, at -6 it stalled to 1.7."},
        {"footflip.rollspeedmax", "Jump, hold a tuck (LT or RT) and push the left stick forward or back to flip. This is the fastest the flip may turn right after the jump. 12 is the game's own."},
        {"footflip.rollspeedmin", "The fastest a flip may turn when it starts late in the jump: the limit drops from the early speed to this. 9 is the game's own."},
        {"footspin.spinspeedmax", "In a tuck, the left stick sideways spins the body. This is the fastest it may spin. 8 is the game's own."},
        {"footspin.spinspeedfalloff", "How much of the spin limit goes over the first second of a flip. 2 is the game's own."},
        {"locomotion.sprintspeed", "Sprinting speed on foot, in m/s."},
        {"wipeout.spreadeaglegravity", "Gravity while gliding spread-eagle. -8 is the game's own; nearer 0 falls slower."},
        {"physicshandplant.exitanglerangestart", "A handplant looks for a wall to come down on in a fan of directions. This is where the fan starts: 0 is along the coping."},
        {"physicshandplant.exitanglerangeend", "Where the fan of exit directions ends: 90 is straight down the ramp. Skate 3 stopped at 69."},
        {"physicshandplant.minnormalvel", "Skate 3 left a plant at 0.9 m/s or more."},
        {"physicshandplant.maxparallelvel", "Skate 3 kept up to 2 m/s."},
        {"physicshandplant.maxlipclearance", "Skate 3's plant topped out 0.76 m above the lip."},
        {"physicsreckoning.vertjumpalignmaxgroundnormaly", "A pop off a wall this steep or steeper is straightened to go up the wall. 0.4 is the game's own; Skate 3 used 0.2, so only near-vert walls counted."},
        {"physicsreckoning.vertjumpalignmaxangle", "Stick neutral a vert pop comes back into the ramp; pushed up it goes out over the deck. This is how far it tips."},
        {"physicstrajectory.scoretransition", "In the air the game scores the landings it could reach. This is what coming back down the wall you left is worth."},
        {"physicstrajectory.scoregrind", "What landing on an edge is worth against landing on ground. At a million, an edge in reach always wins."},
        {"physicsmode.jumpdeltaminheightmanual", "How much higher a held pop out of a manual goes than a quick one, in metres. Measured dead: the manual pop height slider is the lever that works."},
        {"physicstrajectory.grindlandingvelscalar", "How much of your speed is kept when you land on a rail or ledge. 1 keeps it all; lower slows you as you lock on."},
        {"speedmodel.maxpositiveacceleration", "The fastest the board may gain speed on its own (downhill, pumping), in m/s per second. Higher lets you pick up speed quicker."},
        {"speedmodel.maxnegativeacceleration", "The fastest the board may lose speed (friction, uphill), in m/s per second. Lower and you coast further."},
        {"onboard_speedmodel.noinputtime", "How many seconds you can roll without touching the stick before the game starts slowing you down more."},
        {"physicsairstates.mountrunlaunchy", "The little hop up when you jump onto the board from a jog, in m/s. Higher hops higher."},
        {"physicsairstates.mountsprintlaunchy", "The hop up when you jump onto the board from a sprint, in m/s. Higher hops higher."},
        {"physicsairstates.bodyflipmingrabtimefraction", "How much of the air time a body flip has to be held in a grab before it counts, as a share (0.5 = half). Lower makes body flips easier to start."},
        {"physicsfeet.maxlandingforce", "The most force the legs take on a landing before it counts as too hard. Higher lets bigger drops land; not measured yet."},
        {"physicsfeet.landingforcetime", "How long the legs push back after a landing, in seconds. Not measured yet."},
        {"onboard_powerslide.powerslide_forwardforcescalar_autorevert", "How hard an auto revert (the board snapping straight after a spin) pushes you forward. Higher keeps more speed out of reverts."},
        {"physicsmode.unintentionalpumpscalar", "Speed the game gives you on a transition when you are not pumping on purpose. 0 gives none; 1 is the game's own."},
        {"physicsjump.jumpybonusmax", "The most extra pop the game adds to a jump (its \"jumpy\" bonus). Higher raises the top of your pops."},
        {"push.maxpushspeedlight", "The speed a single tapped push settles at, in m/s. Higher makes every tap faster."},
        {"push.maxpushspeedmedium", "The speed a held push reaches, in m/s."},
        {"push.maxpushspeedstrong", "The fastest pushing alone gets you, in m/s. Hills and pumping can still go faster."},
        {"physicsmode.autopushenabled", "Keeps a rolling skater pushing by itself up to the auto push speed while you are not braking."},
        {"physicspush.maxspeedforautopush", "The speed auto push keeps you at, in m/s."},
        {"physicsmode.perfectbodyflips", "Body flips always finish exactly one turn, so they land upright."},
        {"physicsmode.maxautobodyspinspeed", "How fast the body spins on its own in the air, in degrees per second. Higher spins faster."},
        {"physicsmode.easybodyspins", "Body spins (180s, 360s) are easier to start and to finish."},
        {"heldflip.straightflipcatchtime", "The longest a kickflip or heelflip may take before the game hurries it round to the catch, in seconds. Longer lets slow flips stay slow."},
        {"heldflip.shuvmincatchtime", "The shortest time a shuvit takes to the catch, in seconds."},
        {"heldflip.bigflipmincatchtime", "The shortest time a big flip (360 flips and the like) takes to the catch, in seconds."},
        {"physicsmode.makesurfacessmooth", "Rough ground (cobbles, cracks, bricks) rides like smooth concrete: no shaking, no speed lost to it."},
        {"onboard_powerslide.powerslide_forwardforcescalar_slide", "How hard a powerslide keeps pushing you forward. Higher keeps more speed through the slide."},
        {"physicsmode.wipeoutcheckforbadlanding", "When on, landing badly (crooked, on the tail, sideways) bails you. Off, the game forgives it."},
        {"wipeoutfallspeed.normalmaxspeed_ground", "How fast you can come down onto the ground before you bail, in m/s. Higher lets you land bigger drops."},
        {"wipeoutfallspeed.normalmaxspeed_grind", "How fast you can come down onto a rail or ledge before you bail, in m/s."},
        {"jump.maxjumpheight", "The highest a jump on foot can go, in metres. The on-foot jump height row is held under it."},
        {"wipeout.spreadeagleairresistance", "Air drag while gliding spread-eagle in a fall. Higher slows the fall more."},
        {"wipeout.spreadeagleaircontrolmax", "How much you can steer while gliding spread-eagle."},
        {"wipeout.torpedoaircontrolmax", "How much you can steer in a torpedo dive at speed."},
        {"wipeout.defaultaircontrol", "How much you can steer while falling off the board."},
        {"wipeout.defaultairresistance", "Air drag while falling off the board. Higher slows the fall."},
        {"wallrun.wallrunjumpupvelocityboost", "Extra upward speed when you jump off a wallrun, in m/s."},
        {"wallrun.wallrungravity", "How strongly you are pulled down during a wallrun. Lower runs further along the wall."},
        {"physicstrucks.truckzposfront", "Where the front truck sits along the board, in metres from the middle. Takes hold at the next respawn."},
        {"physicstrucks.truckzposback", "Where the back truck sits along the board, in metres from the middle. Takes hold at the next respawn."},
        {"physicsgrind.nonpopexitangledeg", "The angle you roll off a grind at when you leave it without popping, in degrees."},
        {"physicsgrind.grindreckoningsmoothnessonexit", "How slowly the body follows the board when you leave a grind, from 0 to 1. Higher is smoother and slower."},
        {"physicsgrind.grindreckoningsmoothnessdropin", "How slowly the body follows the board when you drop in off a grind, from 0 to 1."},
        {"physicshandplant.maxnormalvel", "The most speed you can carry into the ramp coming out of a handplant, in m/s."},
        {"physicshandplant.maxverticalvel", "The most up or down speed coming out of a handplant, in m/s."},
        {"physicshandplant.minlipclearance", "How high above the lip you must be for a handplant, in metres. Lower lets you plant from smaller airs."},
        {"physicshandplant.maxangleofapproach", "How far off straight at the lip you can be and still handplant, in degrees."},
        {"physicshandplant.waitforanimtime", "How long a handplant waits for its animation before it can end, in seconds."},
        {"physicsliptrick.liptrickcomapexheight", "How high the body rises above the lip in a lip trick, in metres."},
        {"physicsliptrick.lipquerymaxlipdistance", "How far ahead the game looks for a lip to do a lip trick on, in metres."},
        {"physicsliptrick.lipquerymaxdirectionchangedegrees", "How far you may turn to reach a lip for a lip trick, in degrees."},
        {"physicsliptrick.launchgroundslopethreshold", "How steep the ground must be to start a lip trick from it, in degrees."},
        {"physicsliptrick.lipquerylipslopethreshold", "The steepest lip that still counts for a lip trick, in degrees."},
        {"physicsliptrick.lipquerygroundslopethreshold", "How steep the ground under the lip must be, in degrees."},
        {"physicsliptrick.maxrotationpersecond", "The fastest the board turns in a lip trick, in degrees per second."},
        {"physicsrollin.maxrollintime", "The longest a roll in (dropping in from the lip) may take, in seconds."},
        {"physicsrollin.skateboardrollinduration", "How long the board takes to tip over the lip as you roll in, in seconds."},
        {"physicsrollin.minlipbashdownwardspeed", "How fast you must come down onto the lip for it to count as hitting it, in m/s."},
        {"physicsrollin.undershootamount", "How far short of the wall a roll in aims, so you do not slam into the transition."},
        {"physicsreckoning.vertjumpalignminjumpdiry", "How much a pop on a ramp must point upward before the game straightens it up the wall (0 flat, 1 straight up)."},
        {"physicsreckoning.vertjumpalignfactor", "How strongly the game straightens a pop on a ramp up along the wall. 0 leaves your pop as it is."},
        {"physicstrajectory.naturalairoffvertsscalar", "How much speed away from the wall you keep when you roll off a vert lip without popping. Higher throws you out further."},
        {"physicstrajectory.mintimeafterapexfortransition", "How soon after the top of your air the game may aim you back at a ramp, in seconds."},
        {"physicstrajectory.scoremiddlebonus", "When the game picks where you land, the bonus a landing straight ahead (the middle of its search) gets."},
        {"physicstrajectory.scorelandingdirection", "When the game picks where you land, how much it prefers landing facing the way you travel."},
        {"physicstrajectory.maxtrajectoryadjust", "The most speed the game adds in the air to bring you onto the landing it picked, in m/s. 0 lets every air fly as you popped it."},
        {"physicstrajectory.coneanglex", "How far up and down the game looks for a landing while you are in the air, in degrees."},
        {"physicstrajectory.coneanglez", "How far to the sides the game looks for a landing while you are in the air, in degrees."},
        {"physicstrajectory.mintimefortrajectorytobevalid", "The shortest air the game plans a landing for, in seconds. Shorter airs land wherever they come down."},
        {"physicsairstates.naturalairmaxdist", "The widest gap you can roll over off an edge and keep riding, in metres."},
        {"physicsgrindsair.maxdist50_50", "How far from a rail or ledge a 50-50 still locks on, in metres. Higher makes grinds easier to catch."},
        {"physicsgrindsair.maxdist5_0", "How far from a rail or ledge a 5-0 or nose grind still locks on, in metres."},
        {"physicstrajectory.grindlandingmaxangle", "The widest angle you can come onto an edge at and still lock into a grind, in degrees."},
        {"physicspumping.pumpoutoftransitionscalar", "How much speed pumping gives you as you come out of the top of a transition."},
        {"physicsantipumping.maxdownwardangledeg", "The steepest downward push the game counts as pumping the wrong way (which costs speed), in degrees."},
    };
    const auto found = std::ranges::find(help, key, &Help::key);
    return found == std::end(help) ? std::string_view{} : found->text;
}

PresetDial preset_dial(std::string_view name) {
    struct Dial {
        std::string_view name, title;
        std::uint8_t topic;
    };
    static constexpr Dial dials[]{
        {"Super Ollie", "Ollie height", topic_pop},
        {"Grind Pop", "Pop out of grinds and slides", topic_grinds},
        {"Fast", "Push speed", topic_speed},
        {"Easy Body Flips", "Body flips: air time needed (untested)", topic_pop},
        {"Soft Landings", "Landing speed you can take", topic_bails},
        {"Revert Friction", "Revert friction (higher loses more speed)", topic_trickline},
        {"Fast Flips", "Body flip speed", topic_pop},
        {"Fast Spins", "Body spin speed", topic_pop},
        {"Hard To Bail", "Bail resistance (higher: harder to bail)", topic_bails},
        {"Sticky Grinds", "Grind lock-on", topic_grinds},
        {"Slick Grinds", "Grind friction", topic_grinds},
        {"Moon Jump", "On foot: jump height", topic_foot},
        {"Fast On Foot", "On foot: sprint speed", topic_foot},
        {"Fast Foot Flips", "On foot: flip speed", topic_foot},
        {"Big Wallies", "Wallie: extra pop", topic_pop},
        {"Wallie At Speed", "Wallie: fastest speed it works at", topic_pop},
        {"Easy Wallies", "Wallie: how early the pop may come", topic_pop},
        {"Fast Foot Spins", "On foot: spin speed in a tuck", topic_foot},
        {"Super Glide", "Glide: gravity (lower falls slower)", topic_foot},
        {"Torpedo Boost", "Torpedo and falling: steering", topic_foot},
        {"Realistic", "", topic_game},
        {"Hardcore", "", topic_game},
        {"Mega Pop", "", topic_pop},
        {"No Speed Wobble", "", topic_speed},
        {"Auto Push", "", topic_speed},
        {"Smooth Surfaces", "", topic_speed},
        {"Long Wheelbase", "", topic_speed},
        {"Skate 3", "", topic_game},
        {"Skate 3 Hardcore", "", topic_game},
        {"Skate 3 Easy", "", topic_game},
    };
    const auto found = std::ranges::find(dials, name, &Dial::name);
    return found == std::end(dials) ? PresetDial{} : PresetDial{found->title, found->topic};
}

// Skate 3's own tuning, for the values this game still has under the same class and field name
// and that differ: this game's tuning grew out of Skate 3's. Hardcore and Easy are Skate 3's
// difficulty settings on top of the same set.
namespace {
struct Skate3Value {
    std::string_view id;
    double value;
};
#include "trainer_skate3.inc"
std::vector<PresetRule> skate3_rules(std::span<const Skate3Value> difficulty) {
    std::map<std::string_view, double> values;
    for (const auto &row : skate3_normal) values[row.id] = row.value;
    for (const auto &row : difficulty) values[row.id] = row.value;
    std::vector<PresetRule> rules;
    for (const auto &[id, value] : values) rules.push_back({id, false, value, false, true});
    return rules;
}
} // namespace

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
        {"Grind Pop", "Pops out of grinds, board slides and nose and tail slides go twice as high, quick pops included.",
         {{"physicsmode.grindjump", true, 2.0}}},
        {"Easy Body Flips", "Front flips and back flips start from much lower pops.",
         {{"physicsairstates.minbodyfliptimevsupy", true, 0.3, true}, {"physicsairstates.bodyflipmingrabtimefraction", true, 0.3}}},
        {"Soft Landings", "Land from bigger drops and with more sideways speed before a bail.",
         {{"physicsfootplant.wipeoutmaxlandingspeed", true, 1.5}, {"physicswipeout.maxspeedlandingonboard", true, 1.5}}},
        {"Revert Friction", "Reverts and auto reverts scrub three times the speed, as in the older games.",
         {{"onboard_powerslide.frictionscalar_revert", true, 3.0}, {"onboard_powerslide.frictionscalar_autorevert", true, 3.0}}},
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
         {{"jump.basejumpheight", true, 3.0}, {"jump.maxjumpheight", true, 3.0}}},
        {"Fast On Foot", "On foot: sprint nearly twice as fast.",
         {{"locomotion.sprintspeed", true, 1.8}}},
        {"Fast Foot Flips", "On foot: flips in a tuck turn twice as fast.",
         {{"footflip.rollspeedmax", true, 2.0}, {"footflip.rollspeedmin", true, 2.0}}},
        {"Big Wallies", "Wallies pop twice as high off the obstacle.",
         {{"wallie.heightboostpermetre", true, 2.0}, {"wallie.heightboostmax", true, 2.0}}},
        {"Wallie At Speed", "Wallies work at any speed, not only below 11 m/s.",
         {{"wallie.maxspeed", true, 10.0}}},
        {"Easy Wallies", "The pop may come twice as early and still count as a wallie.",
         {{"wallie.windowframes", true, 2.0}, {"wallie.windowframestall", true, 2.0}, {"wallie.windowframesstraight", true, 2.0}}},
        {"Fast Foot Spins", "On foot: spins in a tuck turn twice as fast.",
         {{"footspin.spinspeedmax", true, 2.0}, {"footspin.spinspeedfalloff", true, 2.0}}},
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
          {"physicstrajectory.grindmaxspeeddownontogrind", true, 2.0}, {"physicstrajectory.grindlandingmaxangle", true, 2.0},
          // The reach from the air: without these a dial at 0 still pulls the board onto an edge in flight.
          {"physicsgrindsair.maxdist50_50", true, 2.0}, {"physicsgrindsair.maxdist5_0", true, 2.0},
          {"physicsgrindsair.maxdisttipslide", true, 2.0}, {"physicsgrindsair.maxdistboardslide", true, 2.0},
          {"physicstrajectory.scoregrind", true, 2.0}}},
        {"Slick Grinds", "Grinds and slides keep their speed.",
         {{"physicsgrind.commonfrictionscalar", true, 0.4}, {"physicsgrind.curbfrictionscalar", true, 0.4}}},
        {"Smooth Surfaces", "Rough ground rides like polished concrete.", {{"physicsmode.makesurfacessmooth", false, 1.0}}},
        {"Long Wheelbase", "Trucks 10 cm further out at both ends. Takes effect on the next respawn.",
         {{"physicstrucks.truckzposfront", false, 0.0143}, {"physicstrucks.truckzposback", false, 0.0143}}},
        {"Skate 3", "Skate 3's own numbers for everything this game still shares with it: pop, grind pops, pushing, pumping, "
                    "steering, manuals, body spins and flips, bails, and grinds as they were before skate. Normal difficulty.",
         skate3_rules({})},
        {"Skate 3 Hardcore", "Skate 3 on Hardcore: lower pops, weaker pushes, tight grind lock-on, slow auto spins.",
         skate3_rules(skate3_hardcore)},
        {"Skate 3 Easy", "Skate 3 on Easy: full-height pops, strong pushes, generous grind lock-on.", skate3_rules(skate3_easy)},
        // The tuning part of "Transition feels like Skate 3" (`trainer transition feel skate3`): Skate 3's timers for
        // the lean that ends a coping grind (off, -1, in skate.) and its handplant exit fan.
        // Not its pumping: with the game's legacy pumping forced on nothing pumped at all (owner, 2026-10-11).
        // Not its vert pop and landing numbers (VertJumpAlignMaxGroundNormalY 0.2, NaturalAirMaxDist 0.12, ScoreMiddleBonus
        // 40, MinTimeForTrajectoryToBeValid 0.25): with them and the air grind hold on, 6 of 6 popped airs landed 3 to 4 m
        // out on the deck (0 of them aimed back at the wall), against 5 of 5 back within half a metre on the game's own
        // (owner's log, 2026-10-11). The pop's rule itself is already Skate 3's in skate.: same formula, same 0.08 / 0.25 / 500.
        {"Transition: Skate 3", "Skate 3's numbers for coping grinds and handplants.",
         {{"physicsgrind.copingexitstarttime", false, 0.1, false, true}, {"physicsgrind.fairlysteepexitstarttime", false, 0.8, false, true},
          {"physicshandplant.exitanglerangeend", false, 69.0, false, true}}},
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
