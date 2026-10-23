// Animation Studio's keyframes apart from the game: turning numbers, blending keys, what a profile
// and a clip file hold, and that edits never grow a pose.
#include "Extension/AnimationStudio/anim_core.h"
#include "Extension/AnimationStudio/skater_joint_limits.h"
#include <map>
#include "Extension/AnimationStudio/skater_mirror.h"
#include "Extension/AnimationStudio/anim_presets.h"
#include "Extension/AnimationStudio/anim_research.h"
#include "Extension/AnimationStudio/anim_runtime_rules.h"
#include "Extension/AnimationStudio/anim_diagnostics.h"
#include "Extension/AnimationStudio/anim_library.h"
#include "Extension/AnimationStudio/anim_playback.h"
#include "Extension/AnimationStudio/native_asset_names.h"
#include "Extension/AnimationStudio/native_trick_parts.h"
#include "Extension/AnimationStudio/marketplace.h"
#include "Extension/AnimationStudio/marketplace_listing.h"
#include "Extension/AnimationStudio/recording_rules.h"
#include <limits>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iostream>
#include <vector>
#include <stdexcept>

using namespace dingosdk::anim_studio;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct NamedNativeFixture { std::string asset, label, category, context, search, state; };
bool near(float a, float b, float within = 1e-3f) { return std::abs(a - b) < within; }
bool same_turn(const Quat& a, const Quat& b) {
    const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return d > 0.9999f;
}

// The MARKET's packages: their names on Thunderstore, their versions, and the README written and read back.
void market_package_words() {
    using namespace market;
    check(package_name("Steezy Tre's Pack!") == "Steezy_Tre_s_Pack" && package_name("***") == "Animation_Pack", "Package names are letters, digits and _");
    check(next_version("1.0.0", "") == "1.0.0" && next_version("1.0.0", "1.0.0") == "1.0.1" && next_version("2.3.4", "1.0.0") == "2.3.4" &&
              next_version("bad", "") == "1.0.0",
          "A published version is gone past, any other is kept");
    std::string about;
    std::vector<std::string> tricks;
    read_readme(readme("Street", "Line one.\nLine two.", {"Kickflip (mid pop)", "Heelflip (any pop, switch)"}, "Fourth"), about, tricks);
    check(about == "Line one.\nLine two." && tricks.size() == 2 && tricks[1] == "Heelflip (any pop, switch)", "The README's description and tricks read back");
    // A pack's file version and its release on Thunderstore are kept apart.
    PresetPack pack;
    pack.name = "Street";
    pack.version = "1.2.0";
    pack.description = "About it";
    const auto back = read_pack_json(pack_json(pack));
    check(back.version == "1.2.0" && back.description == "About it", "A pack keeps its release and description beside its file version");
    // The search tells a pack's version only by its icon.
    const std::string icon = "https://ccdn.thunderstore.io/live/repository/icons/Fourth_Studio-Reskate_Animator_Studio_Test-1.0.0.png";
    check(version_from_icon(icon, "Fourth_Studio-Reskate_Animator_Studio_Test") == "1.0.0", "A pack's version is read off its icon");
    check(version_from_icon(icon, "Fourth_Studio-Reskate_Animator").empty() && version_from_icon("", "A-B").empty() &&
              version_from_icon("https://x/A-B-1.0.0/../evil.png", "A-B").empty(),
          "An icon of another pack, or with no version in it, gives none");
    check(plain_name("Fourth_Studio") && !plain_name("a/b") && !plain_name("a-b") && !plain_name("") && plain_version("10.2.3") &&
              !plain_version("1.0.0/x") && !plain_version(".1"),
          "Only plain names and versions go into an address");
    // A page of Thunderstore's search as it came (2026-10-23), with one of someone else's and one with nothing in it.
    const std::string page = R"({"count":3,"next":"https://thunderstore.io/api/cyberstorm/listing/reskate/?page=2&q=Animator","previous":null,"results":[
        {"categories":[{"id":"2759","name":"Misc","slug":"misc"}],"community_identifier":"reskate",
         "description":"Animator Studio pack: Reskate. Animator Studio - Animation Pack Test","download_count":1,
         "icon_url":"https://ccdn.thunderstore.io/live/repository/icons/Fourth_Studio-Reskate_Animator_Studio_Test-1.0.0.png",
         "is_deprecated":false,"is_nsfw":false,"is_pinned":false,"last_updated":"2026-10-23T09:56:46.428418Z",
         "name":"Reskate_Animator_Studio_Test","namespace":"Fourth_Studio","rating_count":0,"size":1645622},
        {"description":"An animator studio pack of mine","download_count":5,"icon_url":null,"is_deprecated":false,"is_nsfw":false,
         "name":"Other","namespace":"Someone","rating_count":2},
        {"description":"Animator Studio pack: no icon","download_count":null,"icon_url":null,"is_deprecated":false,"is_nsfw":false,
         "name":"Bare","namespace":"Someone","rating_count":null,"last_updated":null}]})";
    std::string next;
    const auto found = read_search(page, next);
    check(found.size() == 2 && found[0].full_name == "Fourth_Studio-Reskate_Animator_Studio_Test" && found[0].version == "1.0.0" &&
              found[0].download_url == "https://thunderstore.io/package/download/Fourth_Studio/Reskate_Animator_Studio_Test/1.0.0/" &&
              found[0].description == "Reskate. Animator Studio - Animation Pack Test" && found[0].downloads == 1 && found[0].updated == "2026-10-23",
          "The search's studio packs are read, with their version off the icon");
    check(found[1].full_name == "Someone-Bare" && found[1].version.empty() && found[1].icon_url.empty(),
          "A pack with nothing but its name is kept, its version to be asked for");
    check(next == "https://thunderstore.io/api/cyberstorm/listing/reskate/?page=2&q=Animator", "The next page's address is read");
    check(found[0].created == "2026-10-23" && found[1].created.empty(), "When a pack came out is read");
    // An update has to go past what is up.
    check(version_after("1.0.1", "1.0.0") && version_after("1.0.10", "1.0.9") && version_after("2.0.0", "1.9.9") && !version_after("1.0.0", "1.0.0") &&
              !version_after("0.9.0", "1.0.0") && version_after("1.0.0", "") && !version_after("1.0", "") && !version_after("x", "1.0.0"),
          "Versions are compared number by number");
    check(day_number("2026-10-23") - day_number("2026-10-16") == 7 && day_number("2026-03-01") - day_number("2026-02-28") == 1 &&
              day_number("1970-01-01") == 0 && day_number("2026-10-23T09:56:44Z") == day_number("2026-10-23") && day_number("") == -1 &&
              day_number("2026-13-01") == -1,
          "Dates count in days");
    check(crew_member("fiendskateboards") != nullptr && crew_member("LeafBro") != nullptr && crew_member("Someone") == nullptr &&
              crew_member("leaf.bro") == nullptr,
          "THE CREW is found by team (not by name), in any case");
    check(builder_of("fourth_studio") != nullptr && builder_of("") == nullptr && builder_of("Fourth") == nullptr,
          "BUILT BY is found by their Thunderstore team, and only one with a team");
    pack.published = "1.0.0";
    pack.published_as = "Fourth_Studio-Street";
    check(read_pack_json(pack_json(pack)).published_as == "Fourth_Studio-Street", "A pack keeps the package it went up as");
}

// Packs: tricks switched off, a maker's lock, the playlist, and a trick's stance versions.
void packs_and_playlist() {
    PresetPack pack;
    pack.name = "Street";
    pack.slots["kickflip|mid"] = {"Kickflip a", "Kickflip b"};
    pack.slots["heelflip|mid"] = {"Heelflip a"};
    pack.off = {"Kickflip a"};
    pack.maker_key = "abc";
    pack.editable = true;
    const auto back = read_pack_json(pack_json(pack));
    check(back.off.contains("Kickflip a") && back.maker_key == "abc" && back.editable, "A pack keeps what is off, its maker and whether others may edit it");
    std::vector<Preset> presets{{"Kickflip a", "kickflip", "mid"}, {"Kickflip b", "kickflip", "mid"}, {"Heelflip a", "heelflip", "mid"}, {"Mine", "ollie", "mid"}};
    select_presets(presets, std::span<const PresetPack>(&pack, 1));
    check(!presets[0].active && !presets[0].included && presets[1].active && presets[2].active && !presets[3].included,
          "A trick switched off in a pack does not play from it; the next of its kind does");
    PresetPack other;
    other.name = "Park";
    other.slots["kickflip|mid"] = {"Kickflip c"};
    presets.push_back({"Kickflip c", "kickflip", "mid"});
    const std::array<PresetPack, 2> packs{pack, other};
    const std::vector<PlaylistEntry> playlist{{"Park", ""}, {"Street", ""}, {"", "Mine"}};
    select_playlist(presets, packs, playlist, PlaylistMode::fixed);
    check(presets[4].active && !presets[1].active && presets[2].active && presets[3].active && !presets[0].included,
          "Fixed: the first in the playlist plays; whole packs, single tricks and what is off as set");
    select_playlist(presets, packs, playlist, PlaylistMode::turns);
    check(presets[4].active && presets[1].active && presets[4].order < presets[1].order, "In turns: all of them play, in the playlist's order");
    check(stance_version("regular", false, false, false) == 0 && stance_version("regular", false, true, false) == 1 &&
              stance_version("regular", true, false, false) == 2 && stance_version("goofy", false, true, false) == 3,
          "A trick's stance version, against the skater's own stance");
    Animation locked;
    locked.maker_key = "k";
    locked.keep_leads_right = true;
    const auto read = from_json(to_json(locked));
    check(read.maker_key == "k" && !read.editable && read.keep_leads_right == std::optional<bool>(true), "A trick keeps its maker, its lock and the foot it leads with");
}

// Every spelling the game has for a trick is one trick: what it says in the air, its scoring data, its flick list.
void trick_names_match() {
    check(trick_key("nolliekickflip") == "n_kickflip" && trick_key("n_kickflip") == "n_kickflip" && trick_key("N_Kickflip") == "n_kickflip",
          "Nollie flips: scoring data, the air and the flick list agree");
    check(trick_key("nollie360inwardheelflip") == "n_360inwardheelflip" && trick_key("N_360InwardHeelflip") == "n_360inwardheelflip", "and the long ones");
    check(trick_key("nollie") == "nollie" && trick_key("Nollie") == "nollie" && trick_key("ollie") == "ollie", "Ollie and nollie stay themselves");
    check(trick_key("FrontFoot_Impossible") == "fsimpossible" && trick_key("ffimpossible") == "fsimpossible" && trick_key("bfimpossible") == "bsimpossible",
          "Impossibles by either name");
    check(trick_key("nolliebsimpossible") == "n_bsimpossible" && trick_key("N_BackFoot_Impossible") == "n_bsimpossible", "Nollie back foot impossible");
    check(trick_key("n_fspopshuvit") == "n_fspopshuvit" && trick_key("BSPopShuvit") == "popshuvit" && trick_key("KickFlip") == "kickflip", "Shove-its and case");
    check(trick_key("native:animation/dingo/c_proto_onb_grind_5050_rail_loop") == "native:animation/dingo/c_proto_onb_grind_5050_rail_loop",
          "Pieces keep their clip name");
}

// The game's pieces read as the tricks they are part of (TRICKS' grind, grab and manual cards).
void native_trick_cards() {
    const auto parts = [](const char* name) { return native_trick_parts(std::string("animation/dingo/") + name); };
    const auto shared = parts("c_proto_onb_grind_fs_boardslide_bs_lipslide_rail_loop");
    check(shared.size() == 2 && shared[0].trick == "Boardslide" && shared[0].side == "FS" && shared[1].trick == "Lipslide" && shared[1].side == "BS" &&
              shared[0].part == "LOOP" && shared[0].kind == "Slides",
          "A clip that is two slides goes on both cards, each the way it is done");
    const auto fakie = parts("c_proto_onb_grind_bf_fs_bs_5_0_rail_into");
    check(fakie.size() == 2 && fakie[0].trick == "5-0" && fakie[0].side == "FAKIE FS" && fakie[1].side == "FAKIE BS" && fakie[0].part == "INTO",
          "bf grinds are the fakie ones, and 5_0 is the 5-0");
    const auto out = parts("c_proto_onb_grind_fakie_fs_lipslide_bs_boardslide_rail_dark_popout_bs_inair");
    check(out.size() == 2 && out[0].part == "DARK OUT BS AIR" && out[0].side == "FAKIE FS", "Pop-outs keep their way out, air and darkslide");
    const auto melon = parts("c_proto_onb_inair_bs_grab_melon_mid_loop");
    check(melon.size() == 1 && melon[0].trick == "Melon" && melon[0].side == "BS GRAB" && melon[0].part == "HOLD MID" && melon[0].kind == "Grabs",
          "A grab's tweak is a card of its own, on the grab it is done from");
    const auto flip = parts("c_proto_inair_grab_stale_kick_fingerflip_to_inair");
    check(flip.size() == 1 && flip[0].trick == "Kick fingerflip" && flip[0].side == "STALEFISH" && flip[0].part == "TO AIR", "Fingerflips by style and grab");
    const auto manual = parts("c_proto_onb_manual_n_fakie_med_turn_bs");
    check(manual.size() == 1 && manual[0].trick == "Nose manual" && manual[0].side == "FAKIE" && manual[0].part == "TURN BS MID", "Manual poses and turns");
    const auto plant = parts("c_proto_onb_invert_fakie_bs_judoplant");
    check(plant.size() == 1 && plant[0].trick == "Judo plant" && plant[0].side == "FAKIE BS" && plant[0].kind == "Inverts", "Inverts");
    const auto dark = parts("c_proto_onb_trick_n_360flip_high_dark");
    check(dark.size() == 1 && dark[0].trick == "360 flip" && dark[0].side == "NOLLIE" && dark[0].part == "DARK CATCH HIGH", "Flip variations");
    check(parts("c_proto_onb_grind_darkslide_feet_addi").empty() && parts("c_proto_onb_idle_rolling_loop").empty() &&
              parts("c_proto_ofb_grind_5050_feet_rail_loop").empty() && parts("c_proto_onb_grind_fs_mystery_rail_loop").empty(),
          "Additive layers, off-board clips and names that do not read cleanly are left out");
    // A piece saved before pieces were named by their clip gets that name, so it plays in game.
    Animation piece;
    piece.source = {"native_asset", "animation/dingo/c_proto_onb_grind_5050_rail_loop", "", ""};
    check(from_json(to_json(piece)).trick == "native:animation/dingo/c_proto_onb_grind_5050_rail_loop", "A saved piece is known by its clip");
}

void native_names_and_search() {
    const auto flip = native_asset_name("animation/dingo/c_proto_onb_trick_n_360flip_high_dark", "ClipControllerAsset");
    check(flip.label == "Nollie 360 Flip High Dark", "Readable names preserve pop family, height and native variant");
    check(native_search_matches(flip.search, native_search_terms("HIGH tre-flip nollie")), "Readable aliases match all query words regardless of order/case/punctuation");
    check(native_search_matches(flip.search, native_search_terms("360flip haut")), "Compact engine spelling and height alias remain searchable");
    check(!native_search_matches(flip.search, native_search_terms("kickflip high")), "Unrelated tricks do not match");
    check(native_asset_name("animation/dingo/c_proto_onb_trick_n_nollie_mid").label == "Nollie Medium", "Nollie is not duplicated");
    check(native_asset_name("animation/dingo/c_proto_onb_trick_o_kickflip_low").label == "Kickflip Low", "Confirmed ollie-family marker is hidden without inventing stance");
    check(native_asset_name("animation/dingo/c_proto_onb_manual_t_fakie_high").label == "Manual T Fakie High", "Unknown codes remain visible rather than guessed");
    check(native_asset_name("animation/dingo/c_proto_ofbnb_walk_stop_90l_lfoot_fem").label == "Walk Stop 90 Left Left Foot Female", "Movement directions and physical feet retain their meaning");
    check(native_asset_name("animation/dingo/c_proto_onb_gesture_mysteryXYZ_07").label == "Gesture Mystery XYZ 07", "Unknown animation and variant identifiers are retained");
    const auto folder = [](const char* asset) { return native_asset_folder(asset, "ClipControllerAsset"); };
    check(folder("c_proto_onb_grind_fs_feeble_bs_smith_rail_into") == "Grinds/Feeble + Smith/Entries", "Shared grind clips are grouped by tricks instead of FS/BS developer prefixes");
    check(folder("c_proto_onb_grind_5050_rail_loop") == "Grinds/50-50/Loops", "Grind loops have a specific family/phase folder");
    check(folder("c_proto_onb_grind_fs_bs_5_0_rail_into") == "Grinds/5-0/Entries" && native_asset_name("c_proto_onb_grind_5_0_loop").label == "Grind 5-0 Loop", "Five-oh source tokens form one readable trick name and folder");
    check(folder("c_proto_onb_manual_t_fakie_high") == "Manuals/Other movements", "Unspecified phases are not invented");
    check(folder("c_proto_ofbnb_walk_loop_short") == "Off board/Walk/Loops", "Locomotion is separate from on-board tricks");
    check(folder("c_proto_onb_trick_n_360flip_high") == "Tricks/360 Flip/Variants", "Trick variants share a readable family folder");
    check(native_asset_folder("anything", "TransitionAsset") == "Game logic/Transitions", "Actual root types separate graph transitions from playable animations");
    check(native_search_terms("  --- \t ").empty(), "Punctuation-only input does not create misleading search terms");
    std::vector<NamedNativeFixture> variants{{"source-b", "Wave", "Gestures", "Off board"}, {"source-a", "Wave", "Gestures", "Off board"}, {"source-c", "Wave", "Gestures", "On board"}};
    native_disambiguate_names(variants);
    check(variants[0].label == "Wave / Off board / Variant 2" && variants[1].label == "Wave / Off board / Variant 1" && variants[2].label == "Wave / On board",
          "Equal names retain distinct source context and stable variants independent of input order");
    check(variants[0].asset == "source-b", "Human names never change the identity used to open an animation");
}

void manual_and_scripted_recording_keep_distinct_rules() {
    const auto manual = evaluate_recorded_take(false, "kickflip", "heelflip", "low", 3, true, 0.7f);
    check(manual.rejection == TakeRejection::none && manual.pop == "high", "Manual takes accept the recognized trick and actual grade, ignoring stale batch targets");
    const auto fallback = evaluate_recorded_take(false, "kickflip", "", "", 0, true, 0.5f);
    check(fallback.rejection == TakeRejection::none && fallback.pop == "mid", "Manual recording still works without native grading hooks");
    check(evaluate_recorded_take(true, "heelflip", "varialheelflip", "high", 3, true, 0.7f).rejection == TakeRejection::wrong_trick,
          "Batch recording cannot save the wrong recognized trick");
    check(evaluate_recorded_take(true, "kickflip", "kickflip", "low", 3, true, 0.7f).rejection == TakeRejection::wrong_pop,
          "Batch recording keeps the game's actual requested pop grade");
    check(evaluate_recorded_take(true, "kickflip", "kickflip", "high", 0, true, 0.7f).rejection == TakeRejection::missing_grade,
          "Batch cannot substitute an air-time guess for missing native grading");
    check(evaluate_recorded_take(false, "kickflip", "", "", 4, true, 0.7f).pop == "high", "Unknown native enum values use the manual fallback");
    check(evaluate_recorded_take(false, "", "", "", 3, true, 0.7f).rejection == TakeRejection::missing_trick &&
          evaluate_recorded_take(false, "kickflip", "", "", 3, false, 0.7f).rejection == TakeRejection::missing_board,
          "Manual recording still rejects missing identity and board capture");
    check(pop_grade_since(100, 0.5) == 0 && pop_grade_since(5000, 0.5) == 4200,
          "Grade lookup uses a bounded recent time window without unsigned wraparound");
}

void euler_numbers_read_back() {
    for (const Vec3 angles : {Vec3{0, 0, 0}, Vec3{30, 0, 0}, Vec3{0, -45, 0}, Vec3{10, 20, 30}, Vec3{-120, 60, 170}, Vec3{5, -80, -90}}) {
        const auto back = to_euler(from_euler(angles));
        check(same_turn(from_euler(back), from_euler(angles)), "Degrees turned into a rotation and back give the same rotation");
    }
    const auto x = from_euler({90, 0, 0});
    const auto v = rotate(x, {0, 1, 0});
    check(near(v[0], 0) && near(v[1], 0) && near(v[2], 1), "90 degrees about X turns +Y onto +Z");
}

void slerp_meets_both_ends() {
    const auto a = from_euler({0, 0, 0}), b = from_euler({0, 90, 0});
    check(same_turn(slerp(a, b, 0), a) && same_turn(slerp(a, b, 1), b), "Blending starts and ends on the keys");
    check(same_turn(slerp(a, b, 0.5f), from_euler({0, 45, 0})), "and halfway is half the turn");
}

// Smooth keys never go past a key: a knee's keys like a user's kickflip (0 at the start bracket, still, a little, a
// lot, the most, back to 0 at the end bracket) never dip below 0 before the rise and never go over the most.
void smooth_keys_never_overshoot() {
    Animation animation;
    animation.clip.times = {0, 2.2f};
    const std::array<std::pair<float, float>, 6> keys{{{0.0f, 0.0f}, {0.63f, 0.0f}, {0.7f, 3.0f}, {0.93f, 48.0f}, {1.33f, 119.0f}, {2.12f, 0.0f}}};
    for (const auto& [t, degrees] : keys) set_key(animation, 342, t, {from_euler({0, 0, degrees}), {0, degrees / 1000, 0}});
    for (auto& k : animation.track(342)->keys) k.ease = Ease::smooth;
    const auto* track = animation.track(342);
    float lowest = 1e9f, highest = -1e9f, lowest_move = 1e9f, highest_move = -1e9f;
    for (int i = 0; i <= 2120; ++i) {
        const float t = static_cast<float>(i) / 1000;
        const auto e = evaluate(*track, t);
        const float z = to_euler(e.rotation)[2];
        lowest = std::min(lowest, z);
        highest = std::max(highest, z);
        lowest_move = std::min(lowest_move, e.offset[1]);
        highest_move = std::max(highest_move, e.offset[1]);
    }
    check(lowest > -0.01f && highest < 119.01f, "A smooth turn never goes past its keys: not below 0 before the rise, not over the most");
    check(lowest_move > -1e-5f && highest_move < 0.119f + 1e-5f, "and a smooth move neither");
    check(near(to_euler(evaluate(*track, 0.3f).rotation)[2], 0, 0.01f), "Off the start bracket it stays on 0 until the rise");
}
// A turn through keys keeps its speed at each key (no stop-and-go in playback).
void turn_keeps_its_speed_through_a_key() {
    Animation animation;
    animation.clip.times = {0, 2};
    set_key(animation, 343, 0.0f, {from_euler({0, 0, 0}), {}});
    set_key(animation, 343, 1.0f, {from_euler({90, 0, 0}), {}});
    set_key(animation, 343, 2.0f, {from_euler({180, 0, 0}), {}});
    const auto* track = animation.track(343);
    const auto angle = [&](float a, float b) {
        const auto qa = evaluate(*track, a).rotation, qb = evaluate(*track, b).rotation;
        return 2 * std::acos(std::min(1.0f, std::abs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3])));
    };
    const float speed_before = angle(0.95f, 1.0f) / 0.05f, speed_after = angle(1.0f, 1.05f) / 0.05f;
    check(speed_after > 0.5f * (angle(0.0f, 1.0f) / 1.0f) && speed_before > 0.5f * (angle(0.0f, 1.0f) / 1.0f),
          "A turn keeps its speed through a key");
    check(near(speed_before, speed_after, 0.2f * speed_after), "The speed on either side of a key is the same");
}

void animation_targets_keep_their_motion_and_hierarchy() {
    Clip clip;
    clip.joints = skeleton_joints;
    clip.times = {0, 0.1f, 0.2f};
    clip.frames.assign(skeleton_joints * 3, Joint{});
    for (std::size_t f = 0; f < 3; ++f) {
        clip.frames[f * skeleton_joints + 50].position = {0.1f * f, 1.0f * f, 0};
        clip.frames[f * skeleton_joints + 50].rotation = from_euler({10.0f * static_cast<float>(f), 0, 0});
    }
    for (std::size_t f = 0; f < 3; ++f) {
        clip.frames[f * skeleton_joints + 376].position = {0, 0.3f * f, 0};
        clip.frames[f * skeleton_joints + 390].position = {0, 0.4f * f, 0};
    }
    const auto decoded = clip_from_bytes(clip_bytes(clip));
    check(decoded.frames == clip.frames, "Serialization preserves authored attachment, ground and deck target motion exactly");
    check(parent(376) == 375 && parent(379) == 375 && parent(380) == 1 && parent(390) == 380 && parent(394) == 380,
          "Animation targets use the 395-joint Ant skeleton, not the 386-bone render skeleton");
    std::vector<Joint> pose(skeleton_joints);
    pose[1].position = {1, 2, 3}; pose[375].position = {0, 4, 0};
    pose[380].position = {0, 5, 0}; pose[380].rotation = from_euler({0, 90, 0});
    pose[376].position = {0, 0.3f, 0}; pose[390].position = {1, 0.4f, 0};
    const auto ground = world(pose, 376), deck = world(pose, 390);
    check(near(ground.position[1], 6.3f) && near(deck.position[1], 7.4f) && near(deck.position[2], 2),
          "Ground and deck effectors follow their actual parents and deck targets rotate with the board");
    clip.board.clear();
    for (std::size_t f = 0; f < clip.times.size(); ++f) {
        auto& mount = clip.frames[f * skeleton_joints + skateboard_root_joint];
        mount.position = {0.4f * f, 0.1f * f, 0};
        mount.rotation = from_euler({80.0f * f, 50.0f * f, 0});
        clip.board.push_back(mount);
    }
    std::vector<Joint> sampled(skeleton_joints);
    for (const float time : {0.0f, 0.05f, 0.11f, 0.2f}) {
        clip.sample(time, sampled); Joint mount;
        check(clip.sample_board(time, mount) && mount == sampled[skateboard_root_joint],
              "A native captured mount follows the same body sample through interpolation, without a separate board timestamp");
    }
}

void keys_hold_outside_and_blend_between() {
    Animation animation;
    animation.clip.times = {0, 1};
    set_key(animation, 343, 0.2f, {from_euler({0, 0, 0}), {0, 0, 0}});
    set_key(animation, 343, 0.8f, {from_euler({60, 0, 0}), {0, 0.3f, 0}});
    const auto* track = animation.track(343);
    check(track && track->keys.size() == 2, "Two keys on the left foot");
    check(same_turn(evaluate(*track, 0.0f).rotation, from_euler({0, 0, 0})), "Before the first key it holds");
    check(same_turn(evaluate(*track, 1.0f).rotation, from_euler({60, 0, 0})), "After the last key it holds");
    check(near(evaluate(*track, 0.5f).offset[1], 0.15f), "Halfway between the keys the move is halfway");
    set_key(animation, 343, 0.8f + 1.0f / 240, {from_euler({30, 0, 0}), {}});
    check(track->keys.size() == 2, "A key set on top of another replaces it");
    check(remove_key(animation, 343, 0.2f) && track->keys.size() == 1, "and a key can be taken off");
    auto& key = animation.track(343)->keys.front();
    key.ease = Ease::step;
    set_key(animation, 343, 0.9f, {from_euler({0, 0, 0}), {}});
    check(same_turn(evaluate(*animation.track(343), 0.85f).rotation, from_euler({30, 0, 0})), "A stepped key holds until the next one");
}

void keys_can_be_dragged_in_time() {
    Track track{7, {{0.1f}, {0.5f}, {0.9f}}};
    const auto index = move_key(track, 0, 0.7f, 1.0f);
    check(index == 1 && near(track.keys[1].time, 0.7f) && near(track.keys[0].time, 0.5f), "A key dragged past another moves past it");
    move_key(track, 2, 5.0f, 1.0f);
    check(near(track.keys.back().time, 1.0f), "A key stays on the clip");
}

void edits_are_on_top_of_the_pose_and_never_grow_it() {
    Animation animation;
    animation.clip.times = {0, 2};
    for (int i = 0; i < 1000; ++i)
        set_key(animation, editable_joints()[static_cast<std::size_t>(i) % editable_joints().size()].index, static_cast<float>(i / 24) / 50.0f,
                {from_euler({1, 0, 0}), {}});
    check(animation.key_count() == 1000, "A thousand keys are kept");
    std::vector<Joint> pose(skeleton_joints);
    pose[343].rotation = from_euler({0, 20, 0});
    apply_edits(animation, 0.5f, pose);
    check(pose.size() == skeleton_joints, "The pose keeps its 395 joints whatever is keyed");
    check(same_turn(pose[343].rotation, multiply(from_euler({0, 20, 0}), from_euler({1, 0, 0}))), "The edit turns the joint on from its own pose");
    check(same_turn(pose[200].rotation, Quat{0, 0, 0, 1}), "A joint without keys is untouched");
}

void joints_hang_from_their_parents() {
    std::vector<Joint> pose(skeleton_joints);
    pose[7].position = {0, 1, 0};        // the hips 1 m up
    pose[341].position = {0.1f, 0, 0};   // the left thigh beside them
    pose[342].position = {0, -0.5f, 0};  // the knee below the thigh
    pose[341].rotation = from_euler({0, 0, 90});
    const auto knee = world(pose, 342);
    // The thigh's quarter turn about Z swings the knee's -Y offset onto +X.
    check(near(knee.position[0], 0.6f) && near(knee.position[1], 1.0f), "Turning the thigh carries the knee");
    const auto hips = world(pose, 7);
    check(near(hips.position[1], 1.0f), "and the hips stay where they were");
}

void profiles_read_back_and_bad_ones_are_refused() {
    Animation animation;
    animation.name = "Kickflip 2";
    animation.trick = "Kickflip";
    animation.clip.times = {0, 1.5f};
    animation.speed = 0.5f;
    set_key(animation, 7, 0.4f, {from_euler({0, 10, 0}), {0, 0.1f, 0}});
    set_key(animation, 278, 1.0f, {from_euler({20, 0, 5}), {}}).ease = Ease::linear;
    const auto back = from_json(to_json(animation));
    check(back.name == "Kickflip 2" && back.trick == "Kickflip" && near(back.speed, 0.5f), "Name, trick and speed read back");
    check(back.key_count() == 2 && back.track(278) && back.track(278)->keys[0].ease == Ease::linear, "Keys read back");
    check(same_turn(back.track(7)->keys[0].rotation, from_euler({0, 10, 0})) && near(back.track(7)->keys[0].offset[1], 0.1f),
          "with their values");
    bool refused = false;
    try { (void)from_json(R"({"format":"fourth-animation-studio","version":1,"duration":1,"tracks":[{"joint":200,"keys":[]}]})"); }
    catch (const std::runtime_error&) { refused = true; }
    check(refused, "A track on a joint the editor does not edit is refused");
    refused = false;
    try { (void)from_json(R"({"format":"fourth-animation-studio","version":1,"duration":1,"tracks":[{"joint":7,"keys":[{"t":9,"r":[0,0,0,1],"p":[0,0,0]}]}]})"); }
    catch (const std::runtime_error&) { refused = true; }
    check(refused, "A key past the clip is refused");
    check(file_safe("  ../Kick:flip*  ") == "Kickflip", "File names keep only safe letters");
}

void clips_read_back_and_broken_ones_are_refused() {
    Clip clip;
    clip.joints = 3;
    clip.times = {0, 0.5f};
    clip.frames.resize(6);
    clip.frames[4].position = {1, 2, 3};
    clip.takeoff = 0.1f;
    clip.landing = 0.4f;
    const auto bytes = clip_bytes(clip);
    const auto back = clip_from_bytes(bytes);
    check(back.joints == 3 && back.times.size() == 2 && near(back.frames[4].position[2], 3) && near(back.landing, 0.4f), "A clip reads back");
    std::vector<Joint> pose(3);
    back.sample(0.25f, pose);
    check(near(pose[1].position[1], 1.0f), "and samples halfway between frames");
    auto cut = bytes;
    cut.pop_back();
    bool refused = false;
    try { (void)clip_from_bytes(cut); } catch (const std::runtime_error&) { refused = true; }
    check(refused, "A cut-short clip is refused");
}
} // namespace


// Goofy: the mirror pairs the joints on the two sides, is its own inverse, and moves a right key to the left.
// A mirror has to be a mirror: wherever a joint was, its opposite number has to end up at the reflection
// of that spot, in the world. Reflecting each bone in its own parent frame - which is what was done -
// passes a does-it-come-back test and still fails this one as soon as the rest pose is not symmetrical,
// and that is what tore the skater up on screen.
void mirroring_puts_each_joint_where_its_reflection_belongs() {
    std::vector<Joint> pose(skeleton_joints);
    for (std::size_t j = 0; j < pose.size(); ++j) {
        // Deliberately lopsided: no two bones share a rest orientation and none is axis aligned.
        pose[j].rotation = from_euler({17.0f + static_cast<float>(j % 11) * 7, -23.0f + static_cast<float>(j % 5) * 13,
                                       41.0f - static_cast<float>(j % 7) * 9});
        pose[j].position = {0.03f + 0.001f * static_cast<float>(j % 13), 0.11f, -0.02f + 0.002f * static_cast<float>(j % 9)};
    }
    std::vector<Joint> mirrored(pose.size());
    mirror_pose_frame(pose, mirrored, false);
    std::size_t checked = 0;
    for (std::uint16_t j = 0; j < skeleton_joints; ++j) {
        const auto there = world(pose, j).position;
        const auto here = world(mirrored, mirror_joint[j]).position;
        check(near(here[0], there[0], 2e-3f) && near(here[1], there[1], 2e-3f) && near(here[2], -there[2], 2e-3f),
              "Every joint's opposite number lands on its reflection");
        ++checked;
    }
    check(checked == skeleton_joints, "Every joint was checked");
    // Reflecting every bone in its own parent frame comes to the same thing, as long as every joint is
    // done: the frames cancel along the chain. What it cannot do is mirror part of a skeleton - a joint
    // whose parent was left alone is then reflected in a frame that was not - and that is what tore the
    // skater up, not the arithmetic.
    {
        std::vector<Joint> whole(pose.size());
        for (std::uint16_t j = 0; j < skeleton_joints; ++j) whole[mirror_joint[j]] = mirror_pose(pose[j]);
        for (std::uint16_t j = 0; j < skeleton_joints; ++j) {
            const auto there = world(pose, j).position, here = world(whole, mirror_joint[j]).position;
            check(near(here[0], there[0], 2e-3f) && near(here[1], there[1], 2e-3f) && near(here[2], -there[2], 2e-3f),
                  "Mirroring every joint at once comes to the same");
        }
    }
    // Mirroring the body alone keeps everything that is not the body exactly as it was, and still puts the
    // body where its reflection belongs - because the locals are worked out again against the parents as
    // they ended up, rather than assumed.
    {
        std::vector<Joint> body(pose.size());
        mirror_pose_frame(pose, body, true);
        for (std::uint16_t j = 0; j < skeleton_joints; ++j) {
            if (gameplay_body_joint(j) || gameplay_body_joint(mirror_joint[j])) continue;
            // Held where it stood, in the world. Its parent-local numbers may change to keep it there,
            // which is the point: a prop or an attachment under a mirrored bone must not be dragged along.
            const auto before = world(pose, j).position, after = world(body, j).position;
            check(near(after[0], before[0], 2e-3f) && near(after[1], before[1], 2e-3f) && near(after[2], before[2], 2e-3f),
                  "What is not the body stays where it was");
        }
        check(gameplay_body_joint(343) && gameplay_body_joint(10), "The feet are body joints");
        // Reflected about where he stands - the placement - not about the root, which can be anywhere.
        const auto at = world(pose, placement_joint);
        const auto in_placement = [&](const Vec3& p) {
            return rotate(conjugate(at.rotation), {p[0] - at.position[0], p[1] - at.position[1], p[2] - at.position[2]});
        };
        const auto left = in_placement(world(pose, 343).position), right = in_placement(world(body, 10).position);
        check(near(right[0], left[0], 3e-3f) && near(right[1], left[1], 3e-3f) && near(right[2], -left[2], 3e-3f),
              "and the left foot's reflection is where the right one is");
    }
    // The root far from him and turned, as it is in the game: the body is mirrored where it stands, so the
    // hips do not move away from the placement and the feet stay as high above it as they were.
    {
        auto far = pose;
        far[0].position = {37.0f, -4.0f, 12.0f};
        far[0].rotation = from_euler({0.0f, 73.0f, 0.0f});
        std::vector<Joint> body(far.size());
        mirror_pose_frame(far, body, true);
        const auto at = world(far, placement_joint);
        const auto from = [&](const std::vector<Joint>& p, std::uint16_t j) {
            const auto w = world(p, j).position;
            return rotate(conjugate(at.rotation), {w[0] - at.position[0], w[1] - at.position[1], w[2] - at.position[2]});
        };
        const auto hips_before = from(far, hips_joint), hips_after = from(body, hips_joint);
        check(near(hips_after[0], hips_before[0], 3e-3f) && near(hips_after[1], hips_before[1], 3e-3f) &&
                  near(hips_after[2], -hips_before[2], 3e-3f),
              "A far-off root does not throw the mirrored body off its spot");
        const auto foot_before = from(far, 343), foot_after = from(body, 10);
        check(near(foot_after[1], foot_before[1], 3e-3f), "and the feet stay on the board");
    }
    // And the bones keep their lengths, so nothing is stretched on the way.
    for (std::uint16_t j = 1; j < skeleton_joints; ++j) {
        const auto up = parent(j);
        if (up < 0) continue;
        const auto span = [](const Vec3& a, const Vec3& b) {
            return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        };
        const auto before = span(world(pose, j).position, world(pose, static_cast<std::uint16_t>(up)).position);
        const auto after = span(world(mirrored, mirror_joint[j]).position,
                                world(mirrored, mirror_joint[static_cast<std::uint16_t>(up)]).position);
        check(std::abs(before - after) < 2e-3f, "Bones keep their lengths through a mirror");
    }
}
void mirror_pairs_and_comes_back() {
    for (std::uint16_t joint = 0; joint < skeleton_joints; ++joint) {
        check(mirror_joint[mirror_joint[joint]] == joint, "Mirroring twice gives the same joint");
    }
    check(mirror_joint[7] == 7 && mirror_joint[42] == 42, "The hips and the spine sit on the middle line");
    check(mirror_joint[8] != 8 && mirror_joint[mirror_joint[8]] == 8, "The right thigh pairs with the left");
    Joint right;
    right.rotation = from_euler({20, 30, 40});
    right.position = {0.3f, 0.1f, -0.2f};
    const auto back = mirror_pose(mirror_pose(right));
    check(same_turn(back.rotation, right.rotation) && near(back.position[0], right.position[0]), "A pose mirrored twice is the pose");
    Animation animation;
    animation.clip.joints = skeleton_joints;
    animation.clip.times = {0.0f};
    animation.clip.frames.assign(skeleton_joints, Joint{});
    animation.clip.frames[8].position = {0, 0, 0.4f};
    const auto goofy = mirror_animation(animation);
    check(goofy.clip.frames[mirror_joint[8]].position[2] == -0.4f, "A right thigh's place goes to the left, on the other side");
    set_key(animation, 8, 0.5f, {from_euler({10, 0, 0}), {0, 0, 0.2f}});
    const auto goofy_key = mirror_animation(animation);
    const auto* track = goofy_key.track(mirror_joint[8]);
    check(track && near(track->keys[0].offset[2], -0.2f), "A key on the right moves to the left, mirrored");
    // The game's plane holds the board's long axis: a mirrored board is not turned end for end.
    Joint board;
    board.rotation = axis_angle({0, 1, 0}, 3.14159265f / 2); // its z along the skater's x, as the game's clips have it
    board.position = {0.08f, -0.94f, 0.03f};
    const auto turned = mirror_board_pose(board);
    const auto along = rotate(turned.rotation, Vec3{0, 0, 1}), along_before = rotate(board.rotation, Vec3{0, 0, 1});
    check(near(along[0], along_before[0], 1e-4f) && near(along[2], along_before[2], 1e-4f), "A mirrored board keeps its nose where it was");
    check(near(turned.position[2], -0.03f) && near(turned.position[0], 0.08f), "and only its offset across reflects");
    // A board key works across the board, so that is what a mirror reflects.
    check(near(mirror_board_vector({0.1f, 0.2f, 0.3f})[0], -0.1f) && near(mirror_board_vector({0.1f, 0.2f, 0.3f})[2], 0.3f),
          "A board key's sideways push is reflected, its push along the board is not");
}


Vec3 sub3_test(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float length3_test(const Vec3& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

// Dragging a foot: the leg bends so the foot reaches the drag, the bones keep their lengths, and a drag to
// where the foot already is changes nothing. Works on the skater's own joints (Hips 7, RightUpLeg 8,
// RightLeg 9, RightFoot 10).
void dragged_foot_reaches_the_drag() {
    std::vector<Joint> native(skeleton_joints);
    native[8].position = {0.1f, -0.05f, 0};
    native[9].position = {0, -0.4f, 0};
    native[10].position = {0, -0.4f, 0};
    const auto before_thigh = world(native, 8).position, before_shin = world(native, 9).position, before_foot = world(native, 10).position;
    const float shin = length3_test(sub3_test(world(native, 10).position, world(native, 9).position));
    const Vec3 target = {before_foot[0] + 0.1f, before_foot[1] + 0.25f, before_foot[2] + 0.12f};
    Animation animation;
    animation.clip.joints = skeleton_joints;
    drag_joint(animation, native, 0.5f, 10, target);
    std::vector<Joint> posed(native);
    apply_edits(animation, 0.5f, posed);
    const auto foot = world(posed, 10).position;
    check(length3_test(sub3_test(foot, target)) < 2e-3f, "The dragged foot reaches the drag");
    const auto thigh = world(posed, 8).position, knee = world(posed, 9).position;
    check(near(length3_test(sub3_test(knee, thigh)), length3_test(sub3_test(before_shin, before_thigh)), 2e-3f), "The thigh keeps its length");
    check(near(length3_test(sub3_test(foot, knee)), shin, 2e-3f), "The shin keeps its length");
    check(near(thigh[0], before_thigh[0], 1e-4f) && near(thigh[1], before_thigh[1], 1e-4f), "The hip end of the chain stays where it is");
    Animation still;
    still.clip.joints = skeleton_joints;
    drag_joint(still, native, 0.5f, 10, before_foot);
    std::vector<Joint> rest(native);
    apply_edits(still, 0.5f, rest);
    check(length3_test(sub3_test(world(rest, 10).position, before_foot)) < 2e-3f, "A drag to where the foot is holds it there");
}

// Smooth keys: a key that says nothing goes, the rest ease smoothly and are marked, and the brackets stay.
// Smoothing chosen keys (KEYS, a double-click, the chosen-keys strip): only those keys change, each is
// drawn towards its neighbours and shows round; the first and last only show round.
void smoothing_chosen_keys() {
    Track track;
    track.joint = 342;
    for (const auto& [t, y] : std::vector<std::pair<float, float>>{{0.0f, 0.0f}, {0.5f, 0.3f}, {1.0f, 0.0f}, {1.5f, 0.3f}, {2.0f, 0.0f}}) {
        Key key;
        key.time = t;
        key.offset = {0, y, 0};
        track.keys.push_back(key);
    }
    const std::array<float, 2> chosen{0.5f, 2.0f};
    const auto changed = smooth_keys_at(track, chosen, -1.0f, -1.0f);
    check(changed > 0, "Smoothing chosen keys changes them");
    check(track.keys[1].offset[1] < 0.3f - 1e-3f && track.keys[1].smooth, "A chosen key is drawn towards its neighbours and shows round");
    check(near(track.keys[3].offset[1], 0.3f) && !track.keys[3].smooth, "A key that was not chosen is left alone");
    check(near(track.keys[4].offset[1], 0.0f) && track.keys[4].smooth, "The last key has no neighbour after it: it only shows round");
    const auto again = smooth_keys_at(track, std::array<float, 1>{4.0f}, -1.0f, -1.0f);
    check(again == 0, "A time with no key changes nothing");
}
// The timeline's channel rows: one number of a key changes, the others stay.
void one_channel_changes_alone() {
    Offset edit{from_euler({20, -35, 50}), {0.1f, 0.2f, 0.3f}};
    for (int c = 0; c < 6; ++c) {
        const auto before = to_euler(edit.rotation);
        const auto changed = with_channel(edit, c, c < 3 ? 12.5f : -0.4f);
        check(std::abs(channel_value(changed, c) - (c < 3 ? 12.5f : -0.4f)) < 1e-2f, "A channel takes the value it is given");
        if (c < 3) {
            const auto after = to_euler(changed.rotation);
            for (int o = 0; o < 3; ++o)
                if (o != c) check(std::abs(after[static_cast<std::size_t>(o)] - before[static_cast<std::size_t>(o)]) < 1e-2f, "The other angles stay as they were");
            check(changed.offset == edit.offset, "Changing a turn leaves the move alone");
        } else {
            for (int o = 0; o < 3; ++o)
                if (o != c - 3) check(near(changed.offset[static_cast<std::size_t>(o)], edit.offset[static_cast<std::size_t>(o)]), "The other moves stay as they were");
            check(changed.rotation == edit.rotation, "Changing a move leaves the turn alone");
        }
    }
}
void smoothing_tidies_a_track() {
    Track track;
    track.joint = 343;
    const auto key_at = [&](float t, float y, Ease e = Ease::smooth) {
        Key k; k.time = t; k.offset = {0, y, 0}; k.ease = e; track.keys.push_back(k);
    };
    key_at(0.0f, 0.0f);          // the start bracket
    key_at(0.5f, 0.5f);
    key_at(1.0f, 1.0f, Ease::step); // straight between its neighbours: it says nothing
    key_at(1.5f, 1.5f);
    key_at(2.0f, 2.0f);          // the end bracket
    const auto before = track.keys.size();
    const auto changed = smooth_track(track, 0.0f, 2.0f);
    check(changed > 0, "Smoothing reports what it changed");
    check(track.keys.size() < before, "A key that says nothing is dropped");
    check(track.keys.front().time == 0.0f && track.keys.back().time == 2.0f, "The bracket keys stay where they are");
    check(near(track.keys.front().offset[1], 0.0f) && near(track.keys.back().offset[1], 2.0f), "and keep their values");
    for (const auto& k : track.keys) {
        check(k.ease == Ease::smooth, "Every key eases smoothly afterwards");
        check(k.smooth, "and is marked as smoothed");
    }
}

// Solo: turning a joint on its own. The joints hanging off it are turned back by as much, so they keep
// the way they face. This is the sum the studio does in set_offset, on the arm and its elbow and hand.
void solo_turn_holds_the_joints_below() {
    std::vector<Joint> native(skeleton_joints);
    for (std::size_t j = 0; j < native.size(); ++j) {
        native[j].rotation = from_euler({3.0f * static_cast<float>(j % 7) - 9, 5.0f * static_cast<float>(j % 5) - 10, 7.0f * static_cast<float>(j % 3)});
        native[j].position = {0.1f, 0.05f, -0.02f};
    }
    constexpr std::uint16_t arm = 47, elbow = 48, hand = 49;
    Animation edits;
    edits.end = -1;
    // The elbow and the hand already carry keys of their own: solo must build on those, not wipe them.
    const auto key_of = [&](std::uint16_t joint, const Vec3& degrees) {
        Key k;
        k.time = 0.5f;
        k.rotation = from_euler(degrees);
        Track track;
        track.joint = joint;
        track.keys.push_back(k);
        edits.tracks.push_back(track);
    };
    key_of(elbow, {12, -8, 4});
    key_of(hand, {-5, 15, 2});
    std::vector<Joint> before(native);
    apply_edits(edits, 0.5f, before);
    const auto faced_elbow = world(before, elbow).rotation, faced_hand = world(before, hand).rotation;

    // The arm is turned, as a drag would.
    Key turn;
    turn.time = 0.5f;
    turn.rotation = from_euler({0, 40, 0});
    Track arm_track;
    arm_track.joint = arm;
    arm_track.keys.push_back(turn);
    edits.tracks.push_back(arm_track);
    std::vector<Joint> swung(native);
    apply_edits(edits, 0.5f, swung);
    check(!same_turn(world(swung, elbow).rotation, faced_elbow), "Without solo the joints below swing with their parent");

    // Solo puts them back: a joint's place is the chain above it times its own key, so multiplying its key
    // by conj(where it faces now) * (where it faced) holds it. Only the joint hanging directly off the one
    // being turned is touched - the hand rides on the elbow, so holding the elbow holds the hand with it.
    check(parent(elbow) == arm && parent(hand) == elbow, "The arm, its elbow and its hand hang off one another");
    const auto back = normalised(multiply(conjugate(world(swung, elbow).rotation), faced_elbow));
    for (auto& track : edits.tracks)
        if (track.joint == elbow) track.keys.front().rotation = normalised(multiply(track.keys.front().rotation, back));
    std::vector<Joint> held(native);
    apply_edits(edits, 0.5f, held);
    check(same_turn(world(held, elbow).rotation, faced_elbow), "Solo holds the way the joint below the turn faces");
    check(same_turn(world(held, hand).rotation, faced_hand), "and the one below that, which rides on the held joint");
    check(same_turn(world(held, arm).rotation, world(swung, arm).rotation), "while the joint being turned keeps its turn");
    // Bones are rigid: the joints below still travel with the one that turned. Only their facing is held.
    check(length3_test(sub3_test(world(held, hand).position, world(before, hand).position)) > 1e-3f,
          "The joints below still travel with their parent: solo holds the facing, not the place");
}

// Several packs on at once (the PACKS tab): each plays what it has, and two with the same trick take turns.
void several_packs_play_together() {
    std::vector<Preset> presets{{"Kick A", "kickflip", ""}, {"Kick B", "kickflip", ""}, {"Heel", "heelflip", ""}, {"Ollie", "ollie", ""}};
    PresetPack one, two;
    one.name = "One";
    one.slots["kickflip|"] = {"Kick A"};
    one.slots["heelflip|"] = {"Heel"};
    two.name = "Two";
    two.slots["kickflip|"] = {"Kick B"};
    select_presets(presets, std::vector<PresetPack>{one, two});
    check(presets[0].active && presets[1].active, "Two packs on with a kickflip each: both kickflips play, taking turns");
    check(presets[2].active && !presets[3].active && !presets[3].included, "A trick in no pack that is on stays the game's");
    select_presets(presets, std::vector<PresetPack>{two});
    check(!presets[0].active && presets[1].active && !presets[2].active, "Switching a pack off takes its tricks out");
    select_presets(presets, std::vector<PresetPack>{});
    check(presets[0].active && presets[2].active && presets[3].active, "No pack on: every saved trick plays");
    // A trick whose pop was changed after it went in is still in the pack.
    std::vector<Preset> moved{{"Kick A", "kickflip", "high"}};
    select_presets(moved, std::vector<PresetPack>{one});
    check(moved[0].active && one.has("Kick A") && !one.has("Kick B"), "A pack keeps a trick whose pop changed");
    one.made_by = "22sh";
    const auto back = read_pack_json(pack_json(one));
    check(back.made_by == "22sh" && back.members().size() == 2, "A pack says who made it after it is shared");
}
void packs_on_together_and_their_rules() {
    std::vector<Preset> presets{{"Renz kick", "kickflip", "mid"}, {"Steezy kick", "kickflip", "mid"}, {"Heel", "heelflip", "low"}};
    PresetPack renz, steezy;
    renz.name = "Renz";
    renz.slots["kickflip|mid"] = {"Renz kick"};
    renz.slots["heelflip|low"] = {"Heel"};
    steezy.name = "Steezy";
    steezy.slots["kickflip|mid"] = {"Steezy kick"};
    steezy.slots["heelflip|low"] = {"Heel"}; // the same animation in both: not a choice
    const std::vector<PresetPack> both{renz, steezy};
    auto conflicts = pack_conflicts(presets, both);
    check(conflicts.size() == 1 && conflicts[0].slot == "kickflip|mid" && conflicts[0].packs.size() == 2 && conflicts[0].choice.empty(),
          "Two packs on with a kickflip of their own each: one duplicate, found; one animation in both is not one");
    select_presets(presets, both, nullptr);
    check(presets[0].active && presets[1].active && presets[2].active, "Left as it is, both kickflips play (at random)");
    ConflictChoices choices{{"kickflip|mid", "Steezy"}};
    select_presets(presets, both, &choices);
    check(!presets[0].active && presets[1].active && presets[2].active, "Chosen: only Steezy's kickflip plays");
    check(pack_conflicts(presets, both, choices)[0].choice == "Steezy", "and the choice reads back");
    // Pack rules: a pop for the whole pack.
    std::vector<Preset> one{{"Low kick", "kickflip", "low"}};
    PresetPack ruled;
    ruled.slots["kickflip|low"] = {"Low kick"};
    ruled.pop_rule = "any";
    select_presets(one, std::vector<PresetPack>{ruled}, nullptr);
    check(one[0].active && one[0].pack_pop && one[0].pack_pop->empty(), "A pack set to any pop plays its low kickflip on every pop");
    ruled.pop_rule = "high";
    select_presets(one, std::vector<PresetPack>{ruled}, nullptr);
    check(one[0].pack_pop && *one[0].pack_pop == "high", "and one set to high plays it on high pops only");
    check(read_pack_json(pack_json(ruled)).pop_rule == "high", "The pack's rules come back from its file");
}
void pack_slots_are_fixed_or_alternate() {
    std::vector<Preset> presets{{"Kick A", "kickflip", "mid"}, {"Kick B", "kickflip", "mid"}, {"Kick C", "kickflip", "mid"},
                                {"Heel A", "heelflip", "mid"}, {"Heel B", "heelflip", "mid"}};
    PresetPack pack;
    pack.name = "Steezy";
    pack.slots["kickflip|mid"] = {"Kick B", "Kick A", "Kick C"};
    pack.slots["heelflip|mid"] = {"Heel A", "Heel B"};
    select_presets(presets, std::vector<PresetPack>{pack});
    check(presets[1].active && !presets[0].active && !presets[2].active && presets[3].active && !presets[4].active,
          "Fixed (as packs were): the first of each trick plays");
    pack.slot_alternate["kickflip|mid"] = true;
    pack.unlinked.insert("Kick C");
    select_presets(presets, std::vector<PresetPack>{pack});
    check(presets[0].active && presets[1].active && !presets[2].active && presets[2].included,
          "Alternate: the kickflips linked into the cycle take turns, the unlinked one does not play");
    check(presets[3].active && !presets[4].active, "and the heelflips, Fixed on their own, still play their first");
    pack.unlinked = {"Kick A", "Kick B", "Kick C"};
    select_presets(presets, std::vector<PresetPack>{pack});
    check(presets[1].active && !presets[0].active && !presets[2].active, "With none linked, the first plays, as Fixed");
    pack.unlinked = {"Kick C"};
    const auto back = read_pack_json(pack_json(pack));
    check(back.alternates("kickflip|mid") && !back.alternates("heelflip|mid") && back.unlinked.contains("Kick C") && back.unlinked.size() == 1 &&
              back.slots.at("kickflip|mid").front() == "Kick B",
          "Fixed or Alternate per trick, the cycle and the order come back from the file");
    {
        // A variant saved leading with the other foot is a stance slot of its own: unlinked, it still never plays.
        std::vector<Preset> stances{{"Shuv", "popshuvit", "mid"}, {"Shuv 2", "popshuvit", "mid"}};
        stances[1].stance = "goofy";
        PresetPack cycle;
        cycle.slots["popshuvit|mid"] = {"Shuv", "Shuv 2"};
        cycle.slot_alternate["popshuvit|mid"] = true;
        cycle.unlinked = {"Shuv 2"};
        select_presets(stances, std::vector<PresetPack>{cycle});
        check(stances[0].active && !stances[1].active, "An unlinked variant in another stance does not play while one is linked");
    }
    PresetPack old;
    old.alternate = true;
    old.slots["kickflip|mid"] = {"Kick A", "Kick B"};
    check(read_pack_json(pack_json(old)).alternates("kickflip|mid"), "A pack from before, Alternate for the whole pack, still alternates");
}
void packs_select_variants_without_filesystem_order() {
    std::vector<Preset> presets{{"Z", "Kickflip", "mid"}, {"A", "kickflip", "mid"}, {"Any", "kickflip", ""}};
    select_presets(presets, std::nullopt);
    check(!presets[0].active && presets[1].active && presets[2].active, "Default choice is deterministic and separate for each pop");
    PresetPack pack;
    pack.name = "Test";
    pack.slots["kickflip|mid"] = {"Deleted", "Z", "A"};
    select_presets(presets, pack);
    check(presets[0].active && !presets[1].active && !presets[2].active, "Fixed pack skips missing variants and respects its chosen order");
    check(presets[0].included && presets[1].included && !presets[2].included, "Pack membership is distinct from the fixed choice");
    pack.alternate = true;
    select_presets(presets, read_pack_json(pack_json(pack)));
    check(presets[0].active && presets[1].active && !presets[2].active, "Alternate pack enables all included variants after round trip");
    pack.slots.clear();
    select_presets(presets, pack);
    check(!presets[0].active && !presets[1].active && !presets[2].active, "An empty pack leaves tricks native");
    bool refused = false;
    try { read_pack_json(dingosdk::Json::parse(R"({"format":"animator-preset-pack","version":1,"slots":{"ollie|":[42]}})")); }
    catch (const std::exception&) { refused = true; }
    check(refused, "Invalid pack members are refused");
    std::vector<Preset> stances{
        {"Normal A", "ollie", "", false, false, "goofy", false},
        {"Switch A", "ollie", "", false, false, "goofy", true},
        {"Switch Z", "ollie", "", false, false, "goofy", true},
        {"Regular", "ollie", "", false, false, "regular", false}};
    select_presets(stances, std::nullopt);
    check(stances[0].active && stances[1].active && !stances[2].active && stances[3].active,
          "Default selection retains a separate playable recording per base/switch stance");
    pack.alternate = false;
    pack.slots["ollie|"] = {"Normal A", "Switch Z", "Switch A", "Regular"};
    select_presets(stances, pack);
    check(stances[0].active && !stances[1].active && stances[2].active && stances[3].active,
          "Fixed packs choose their first included compatible variant for each stance");
}
void research_keeps_short_transitions_and_is_bounded() {
    ResearchBuffer buffer;
    ResearchSample sample;
    sample.physics = 200;
    buffer.append(sample);
    sample.seconds = 0.001;
    buffer.append(sample);
    check(buffer.samples.size() == 1, "Unchanged state is sampled periodically");
    sample.physics = 403;
    buffer.append(sample);
    sample.seconds = 0.002;
    sample.physics = 401;
    buffer.append(sample);
    check(buffer.samples.size() == 3, "Rail transitions between periodic samples are retained");
    sample.seconds = 0.003;
    sample.physics.reset();
    buffer.append(sample);
    check(!buffer.samples.back().physics, "Unavailable state is preserved, never guessed as ground");
    sample.seconds = 61;
    check(!buffer.append(sample) && buffer.samples.size() == 4, "Trace ends after its bounded duration");
    buffer.samples.clear();
    for (std::size_t i = 0; i < ResearchBuffer::capacity + 4; ++i) {
        sample.seconds = static_cast<double>(i) / 1000.0;
        sample.physics = static_cast<std::uint32_t>(i);
        buffer.append(sample);
    }
    check(buffer.samples.size() == ResearchBuffer::capacity, "Trace memory stays bounded even with a state change each tick");
}
void merged_library_and_timing() {
    check(episode_selection_changed("", "ollie") && episode_selection_changed("ollie", "kickflip"),
          "A first recognized trick and a late flip can select a preset");
    check(!episode_selection_changed("kickflip", "kickflip") && !episode_selection_changed("kickflip", ""),
          "Pop refinement and a blank name cannot restart the same trick's source clock");
    check(recorded_clip_identity("n_heelflip__high.clip") == std::pair{std::string("n_heelflip"), std::string("high")},
          "Recorded filenames preserve the game's underscored trick ID");
    for (const auto* file : {"../ollie__high.clip", "a\\ollie__high.clip", "a:ollie__high.clip", "ollie__any.clip", "__high.clip"})
        check(!recorded_clip_identity(file), "Recorded clips reject traversal and ambiguous identities");
    GameTrick low{"Flip", "Kickflip", "kickflip", "low", "native-low.clip", "asset-low"};
    auto high = low; high.pop = "high"; high.file = "native-high.clip"; high.asset = "asset-high";
    GameTrick grind{"Grind", "50-50 entry", "", "", "grind.clip", "grind-asset", true};
    auto take = high; take.file = "kickflip__high.clip"; take.asset.clear(); take.stance = "goofy"; take.switch_stance = true;
    const auto library = merge_game_library({low, high, grind}, {take});
    check(library.size() == 3, "A batch recording replaces only its matching native trick/pop slot");
    check(library[0].recorded && library[0].file == take.file && library[0].asset == "asset-high" &&
          library[0].stance == "goofy" && library[0].switch_stance.value_or(false),
          "High pop is shown first and retains native provenance plus recorded stance");
    check(!library[1].recorded && library[1].file == low.file && library[2].preview_only,
          "Other pop heights and grind previews stay available after Record all");
    check(playback_pop_rank("high", "high") == 0 && playback_pop_rank("high", "") == 1 &&
          playback_pop_rank("high", "low") < 0 && playback_pop_rank("mid", "low") < 0,
          "Playback accepts exact or explicitly generic pops, never a different source height");
    check(episode_selection_changed("360flip", "360flip", "low", "high") &&
          !episode_selection_changed("360flip", "360flip", "low", "low") &&
          !episode_selection_changed("360flip", "360flip", "", "high"),
          "A late high-pop grade invalidates a low binding without cycling a generic or matching binding");
    // A limb can be given back to the game on its own: a weight each, and the joints below a shoulder or
    // a thigh go with their limb, fingers included.
    check(limb_group(277) == 1 && limb_group(278) == 1 && limb_group(292) == 1, "The left arm, its hand and its fingers are one limb");
    check(limb_group(48) == 2 && limb_group(49) == 2 && limb_group(62) == 2, "and so are the right");
    check(limb_group(342) == 3 && limb_group(343) == 3, "The left leg and its foot");
    check(limb_group(9) == 4 && limb_group(10) == 4, "and the right");
    check(limb_group(7) == 0 && limb_group(44) == 0 && limb_group(103) == 0, "The hips, the chest and the head are not a limb");
    {
        Animation layered;
        Track t{10, {}, false, false};
        Key k; k.time = 0; k.offset = {0, 0.2f, 0}; t.keys.push_back(k);
        layered.tracks.push_back(t);
        t.joint = 278; layered.tracks.push_back(t);
        std::vector<Joint> pose(skeleton_joints);
        LimbWeights limbs;
        limbs.limb[4] = 0; // the right leg has been taken by the game
        apply_authored_edit_layer(layered, 0, pose, 1.0f, &limbs);
        check(near(pose[10].position[1], 0), "A limb given back takes none of the authored move");
        check(near(pose[278].position[1], 0.2f), "while the others keep all of it");
    }
    // Which foot leads, and what that means for a trick recorded in one stance being done in another.
    check(leads_right(true, false) && !leads_right(false, false), "Goofy leads with the right foot, regular with the left");
    check(leads_right(false, true) && !leads_right(true, true), "Riding switch turns that round");
    // The game tells us one thing outright - whether the left foot leads - and switch is worked out from it
    // and the stance setting. The stance cancels out of the lead foot, so the lead foot comes out right even
    // when the setting is wrong or was never set: a goofy skater read as regular is read as regular riding
    // switch, which leads with the same foot. That is why matching by lead foot holds where matching by
    // stance label does not, and why a recording's stance and switch must come from the same setting.
    for (const bool left : {false, true})
        for (const bool goofy : {false, true})
            check(leads_right(goofy, riding_switch(left, goofy)) == !left, "The lead foot is the game's bit, whatever the stance setting says");
    // Read the old way - the bit taken for a switch flag - a goofy skater came out right by luck and a
    // regular one the wrong way round, every time. Recordings made like that need fix_regular_skater_tags.
    check(leads_right(true, false) == !false && leads_right(false, true) != !true, "The bit read as a switch flag is wrong for regular skaters");
    GameplayClipClock clock;
    check(near(clock.advance_air(10, 0.3f, 1, 2), 0.3f), "Newly selected presets begin at takeoff");
    check(near(clock.advance_air(10.1, 0.3f, 1, 2), 0.4f), "Air source time advances incrementally");
    check(near(clock.advance_air(10.1, 0.3f, 2, 2), 0.4f), "Changing pop rate cannot move the playhead on the same callback");
    check(near(clock.advance_air(10.35, 0.3f, 2, 2), 0.9f), "A long callback integrates its elapsed time at the updated rate");
    check(near(clock.advance_air(10, 0.3f, 2, 2), 0.9f), "Backwards callbacks cannot rewind air playback");
    check(near(clock.advance_air(12, 0.3f, 2, 2), 2) && near(clock.air_rate, 0), "The landing marker holds until contact");
    // Moving a bracket carries that bracket's own key and nothing else: the keys between the brackets are
    // left exactly where they are, so the track keeps its whole frames instead of being stretched onto
    // fractions of one. (This replaces the window retiming, which stretched every key.)
    Animation animation; animation.start = 1; animation.end = 3;
    Track track{7, {}, false, false};
    track.keys = {{0, {}, {}, Ease::smooth}, {1, {}, {}, Ease::smooth}, {2, {}, {}, Ease::smooth}, {3, {}, {}, Ease::smooth}};
    animation.tracks.push_back(track); track.joint = 8; track.locked = true; animation.tracks.push_back(track);
    check(move_window_edge(animation, 1, 0.5f, 4), "A bracket accepts a place inside the window");
    check(near(animation.start, 0.5f), "and the window starts there");
    check(animation.tracks[0].keys.size() == 4 && near(animation.tracks[0].keys[1].time, 0.5f),
          "The key that was on the bracket goes with it");
    check(near(animation.tracks[0].keys[2].time, 2) && near(animation.tracks[0].keys[3].time, 3),
          "and the keys between the brackets do not move");
    check(near(animation.tracks[1].keys[1].time, 1), "A locked track is left alone");
    check(!move_window_edge(animation, 1, 3.5f, 4) && near(animation.start, 0.5f), "A bracket cannot be put past its other half");
    check(!move_window_edge(animation, 2, 0.2f, 4) && near(animation.end, 3), "and that holds for the end as well");
    check(move_window_edge(animation, 1, 0.0f, 4) && animation.tracks[0].keys.size() == 3 && near(animation.tracks[0].keys[0].time, 0),
          "Taken back onto a key that is already there, the two become one");
    // Smoothing puts keys back on whole frames, which is where an older stretched animation is mended.
    Track drifted{9, {}, false, false};
    drifted.keys = {{0, {}, {}, Ease::smooth}, {0.337f, {}, {0, 0.4f, 0}, Ease::step}, {0.681f, {}, {0, 0.1f, 0}, Ease::linear}, {1, {}, {}, Ease::smooth}};
    smooth_track(drifted, 0, 1);
    for (const auto& k : drifted.keys)
        check(near(k.time, on_a_frame(k.time), 1e-5f), "Every key sits on a whole frame after smoothing");
}
void preview_trajectory_uses_the_whole_clip() {
    Clip clip;
    clip.joints = 2; clip.times = {0, 0.25f, 0.5f, 0.75f, 1};
    clip.frames.resize(clip.times.size() * clip.joints); clip.takeoff = 0; clip.landing = 1;
    for (std::size_t i = 0; i < clip.times.size(); ++i)
        clip.frames[i * clip.joints + placement_joint].position[1] = 10 + clip.times[i];
    check(!clip_has_vertical_trajectory(clip), "A rising/sloped linear trajectory is not a jump relative to its endpoints");
    clip.frames[2 * clip.joints + placement_joint].position[1] += 0.3f;
    check(clip_has_vertical_trajectory(clip),
          "A captured jump uses its trajectory for the whole clip, including early/late frames below the old 12 cm switch threshold");
    clip.frames[2 * clip.joints + placement_joint].position[1] -= 0.3f;
    check(!clip_has_vertical_trajectory(clip), "An in-place clip can use the preview arc consistently");
    clip.landing = clip.takeoff;
    check(!clip_has_vertical_trajectory(clip), "Preview-only or invalid phase markers cannot divide by a zero airborne span");
}
void timing_and_stance_edge_cases() {
    check(!gameplay_body_joint(50) && !gameplay_body_joint(283), "Gameplay does not replace native hand attachment transforms with recorded rest positions");
    check(!gameplay_body_joint(placement_joint) && !gameplay_body_joint(board_joint), "Gameplay body ownership excludes placement and board");
    for (const auto& joint : editable_joints())
        if (joint.index != board_joint) check(gameplay_body_joint(joint.index), "Every editable body joint retains gameplay ownership");
    for (std::uint16_t j = 0; j < skeleton_joints; ++j)
        check(gameplay_body_joint(j) == gameplay_body_joint(mirror_joint[j]), "Gameplay ownership is symmetric across left and right");
    GameplayClipClock clock;
    clock.observe_air(10, 0.94f, 0.46f);
    const auto first_landing = clock.land(10.0163584, 1, 1.6675f, 1.01725f);
    check(first_landing > 0.94f && first_landing < 0.95f,
          "Landing continues the previous source time instead of the measured 77 ms contact-marker jump");
    const auto later = clock.land(10.1, 1, 1.6675f, 1.1f);
    check(near(later, 0.94f + 0.08f * (0.46f + 1) / 2 + 0.02f),
          "Landing integrates its smooth rate ramp exactly even across a long callback interval");
    check(near(clock.land(10.1, 1, 1.6675f, 1.1f), later), "A duplicate callback cannot advance the landing clock");
    check(near(clock.land(9, 1, 1.6675f, 1.1f), later), "A backwards timestamp cannot rewind landing playback");
    check(near(clock.land(12, 1, 1.6675f, 1.1f), 1.6675f), "Landing clock stays inside its clip");
    clock.reset(); clock.observe_air(20, 0.98f, 0);
    check(clock.land(20.016, 1, 2, 1.1f) < 0.981f, "Held airborne pose starts landing without a source-time snap");
    Track uneven{7, {}, false, false};
    for (const float t : {0.0f, 0.25f, 2.0f, 3.0f})
        uneven.keys.push_back({t, from_euler({t * 60, 0, 0}), {t, 0, 0}, Ease::smooth});
    constexpr float h = 0.002f;
    const auto left = evaluate(uneven, 0.25f - h), centre = evaluate(uneven, 0.25f), right = evaluate(uneven, 0.25f + h);
    check(near((centre.offset[0] - left.offset[0]) / h, (right.offset[0] - centre.offset[0]) / h, 0.03f),
          "Uneven key spacing must not introduce a position velocity jump");
    check(near((to_euler(centre.rotation)[0] - to_euler(left.rotation)[0]) / h,
               (to_euler(right.rotation)[0] - to_euler(centre.rotation)[0]) / h, 2.0f),
          "Uneven key spacing must not introduce an angular velocity jump");
    for (const double hz : {30.0, 60.0, 144.0}) {
        ContactWindow contact;
        check(!contact.observe(10, true), "A single contact sample is not a confirmed landing");
        bool confirmed = false;
        for (int i = 1; i < 20; ++i)
            if (contact.observe(10 + i / hz, true)) { confirmed = true; break; }
        check(confirmed && contact.since && *contact.since == 10, "Landing marker preserves first contact independent of sample frequency");
        contact.observe(11, false);
        check(!contact.since && !contact.observe(11.001, true), "A brief contact interrupted by air cannot accumulate into a false landing");
    }
    check(!riding_switch(true, false) && riding_switch(false, false), "A regular skater whose left foot leads is not switch");
    check(riding_switch(true, true) && !riding_switch(false, true), "A goofy skater whose left foot leads is switch");
    check(near(advance_time(0, 0.25f, 1, 2, false), 0.25f), "A 250 ms render stall must not discard 150 ms of playback");
    check(near(advance_time(0.8f, 0.7f, 2, 1, true), 0.2f), "Looping handles a frame spanning multiple loops");
    check(near(advance_time(0.8f, 0.7f, 2, 1, false), 1), "Non-looping playback ends at the last frame");
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    check(near(advance_time(0.4f, nan, 1, 1, false), 0.4f), "Nonfinite elapsed time cannot corrupt the playhead");
    check(!fresh_sample(10, 9.99, 9.99), "A render pose cannot be recorded again with a new animation timestamp");
    check(!fresh_sample(10, 10.01, 9.99) && fresh_sample(10, 9.995, 9.99), "Render capture rejects future and accepts fresh timestamps");
    const auto a = variation_name(std::string(48, 'a'), 2), b = variation_name(std::string(48, 'a'), 3);
    check(a.size() <= 48 && file_safe(a) == a && file_safe(a) != file_safe(b), "Long variation names retain their unique suffix after filename sanitization");
    Animation animation;
    animation.stance = "goofy";
    animation.switch_stance = true;
    auto back = from_json(to_json(animation));
    check(back.switch_stance && *back.switch_stance, "Switch metadata survives profile serialization");
    animation.switch_stance = false;
    back = from_json(to_json(animation));
    check(back.switch_stance && !*back.switch_stance, "Normal stance is distinct from missing legacy metadata");
    const auto mirror = mirror_animation(animation);
    check(mirror.stance == "regular" && mirror.switch_stance == animation.switch_stance, "Mirrored animation metadata describes the opposite base stance");
    Clip clip;
    clip.joints = 1;
    clip.times = {0, 1};
    clip.frames.resize(2);
    clip.board.resize(2);
    std::vector<Joint> pose(1);
    pose[0].position = {17, 0, 0};
    clip.sample(nan, pose);
    check(pose[0].position[0] == 17 && !clip.sample_board(nan, pose[0]), "NaN sampling cannot index past a clip's final frame");
    clip.frames.clear();
    clip.sample(0.5f, pose);
    check(pose[0].position[0] == 17, "Incomplete in-memory clips never read nonexistent poses");
    bool refused = false;
    try { (void)clip_bytes(clip); } catch (const std::runtime_error&) { refused = true; }
    check(refused, "Incomplete poses cannot be written as a valid-looking clip");
    Animation shifted;
    shifted.clip.times = {0, 1};
    set_key(shifted, 7, 0.2f, {}).offset = {0.1f, 0, 0};
    set_key(shifted, 7, 0.8f, {});
    set_key(shifted, 7, 1.0f, {});
    set_key(shifted, 49, 0.3f, {});
    shifted.track(49)->locked = true;
    shift_keys(shifted, 0.5f, 1.0f);
    const auto restored = from_json(to_json(shifted));
    check(restored.track(7)->keys.size() == 2 && near(restored.track(7)->keys.back().time, 1),
          "Moving a start bracket keeps saturated keys unique and the profile reloadable");
    check(near(restored.track(49)->keys[0].time, 0.3f), "Bracket shifts preserve locked tracks");
    auto track = *restored.track(7);
    move_key(track, 0, 1, 1);
    check(track.keys.size() == 1 && near(track.keys[0].offset[0], 0.1f), "Dropping a key on another frame replaces its pose instead of introducing a duplicate timestamp");
    Animation window;
    window.start = 0.2f;
    window.end = 0.8f;
    set_key(window, 7, 0, {from_euler({0, 30, 0}), {0.2f, 0, 0}});
    std::vector<Joint> body(skeleton_joints);
    apply_edits(window, 0.1f, body);
    check(near(body[7].position[0], 0) && animation_window_weight(window, 0.1f) == 0,
          "A bracket excludes both joint edits and in-game replacement outside its window");
    check(near(animation_window_weight(window, 0.24f), 0.5f) && near(animation_window_weight(window, 0.5f), 1) &&
          near(animation_window_weight(window, 0.76f), 0.5f), "Custom playback blends continuously at explicit bracket edges");
    for (const auto text : {
        R"({"format":"fourth-animation-studio","version":1,"duration":1,"start":-1})",
        R"({"format":"fourth-animation-studio","version":1,"duration":1,"start":0.8,"end":0.2})",
        R"({"format":"fourth-animation-studio","version":1,"duration":1,"switch_stance":"yes"})"}) {
        refused = false;
        try { (void)from_json(text); } catch (const std::exception&) { refused = true; }
        check(refused, "Malformed timeline and stance metadata is rejected before reaching playback");
    }
    {
        // A clip 1.9167 s long (115 frames at 60): its end bracket used to round onto 1.9333 (58 frames at 30), past it.
        const auto rounded = from_json(R"({"format":"fourth-animation-studio","version":1,"duration":1.9167,"start":0.5,"end":1.9333})");
        check(near(rounded.end, 1.9167f), "A bracket rounded a frame past the clip is put back on it, not refused");
        Animation edge;
        edge.start = 0.5f;
        check(move_window_edge(edge, 2, 1.9167f, 1.9167f) && edge.end <= 1.9167f, "The end bracket never goes past the clip");
    }
}
void retargeted_edits_preserve_native_rig_and_convert_joint_frames() {
    std::vector<Joint> source(skeleton_joints), native(skeleton_joints);
    source[placement_joint].rotation = from_euler({0, 37, 0});
    native[placement_joint].rotation = from_euler({0, -125, 0});
    source[hips_joint].rotation = from_euler({12, 35, -8});
    native[hips_joint].rotation = from_euler({-10, -25, 17});
    source[8].rotation = from_euler({67, 11, -20});
    native[341].rotation = from_euler({-32, 48, 91});
    for (std::size_t i = 2; i < native.size(); ++i) {
        native[i].position = {0.013f * static_cast<float>(i % 5), 0.03f, -0.02f};
        native[i].scale = {1.1f, 1.1f, 1.1f};
    }
    Animation animation;
    animation.tracks.push_back({8, {{0, {0, 0, 0, 1}, {}, Ease::linear}}});
    auto target = native;
    retarget_edits(animation, 0, source, target, true);
    for (std::size_t i = 0; i < target.size(); ++i) {
        check(target[i].position == native[i].position && target[i].scale == native[i].scale,
              "Identity retarget never imports recorded bone positions or scale");
        for (int k = 0; k < 4; ++k) check(near(target[i].rotation[k], native[i].rotation[k]), "Identity edit leaves native switch pose unchanged");
    }
    const auto edit = from_euler({23, -11, 8});
    animation.tracks[0].keys[0].rotation = edit;
    retarget_edits(animation, 0, source, target, true);
    const auto sf = multiply(conjugate(world(source, placement_joint).rotation), world(source, 8).rotation);
    const auto tf = multiply(conjugate(world(native, placement_joint).rotation), world(native, 341).rotation);
    const auto local_delta = multiply(conjugate(native[341].rotation), target[341].rotation);
    const auto actual = normalised(multiply(multiply(tf, local_delta), conjugate(tf)));
    const auto expected = normalised(mirror_rotation(multiply(multiply(sf, edit), conjugate(sf))));
    float dot = 0;
    for (int k = 0; k < 4; ++k) dot += actual[k] * expected[k];
    check(std::abs(dot) > 0.99999f, "Edit rotates in the reflected placement frame even with asymmetric local bone bases");
    for (std::size_t i = 0; i < target.size(); ++i) {
        check(target[i].position == native[i].position && target[i].scale == native[i].scale && finite(target[i]),
              "Rotational retarget preserves all native local bone lengths and scales");
        if (i != 341) check(target[i].rotation == native[i].rotation, "Only the paired editable joint is changed");
    }
    animation.tracks[0].keys[0].offset = {0.03f, -0.1f, 0.02f};
    target = native;
    retarget_edits(animation, 0, source, target, true);
    const auto src_parent = world(source, hips_joint);
    const auto dst_parent = world(native, hips_joint);
    const auto source_frame = multiply(conjugate(world(source, placement_joint).rotation), src_parent.rotation);
    const auto target_frame = multiply(conjugate(world(native, placement_joint).rotation), dst_parent.rotation);
    const auto wanted_offset = mirror_vector(rotate(source_frame, animation.tracks[0].keys[0].offset));
    Vec3 local_offset{};
    for (int k = 0; k < 3; ++k) local_offset[k] = (target[341].position[k] - native[341].position[k]) * dst_parent.scale[0];
    const auto actual_offset = rotate(target_frame, local_offset);
    for (int k = 0; k < 3; ++k) check(near(actual_offset[k], wanted_offset[k]), "Offsets convert through parent frames and compensate target parent scale");
    animation.tracks[0].hidden = true;
    target = native;
    retarget_edits(animation, 0, source, target, true);
    check(target[341].rotation == native[341].rotation, "Hidden edits stay hidden in switch");
}
void switch_edits_follow_the_native_body_facing() {
    std::vector<Joint> source(skeleton_joints);
    source[placement_joint].rotation = from_euler({0, 37, 0});
    source[hips_joint].rotation = from_euler({12, 35, -8});
    source[341].rotation = from_euler({23, -17, 9});
    source[341].position = {0.1f, -0.2f, 0.03f};
    const auto rotation = from_euler({19, 11, -7});
    const Vec3 offset{0.03f, 0.02f, -0.01f};
    Animation animation;
    animation.tracks.push_back({341, {{0, rotation, offset, Ease::linear}}});
    for (const bool mirror : {false, true}) for (const bool reverse : {false, true}) {
        auto native = source;
        if (mirror) for (std::size_t j = 0; j < source.size(); ++j) native[mirror_joint[j]] = mirror_pose(source[j]);
        native[placement_joint].rotation = from_euler({0, -125, 0});
        if (reverse) native[hips_joint].rotation = multiply(Quat{0, 1, 0, 0}, native[hips_joint].rotation);
        auto target = native;
        retarget_edits(animation, 0, source, target, mirror, reverse);
        const auto joint = mirror ? mirror_joint[341] : 341;
        const auto expected_rotation = normalised(multiply(native[joint].rotation, mirror ? mirror_rotation(rotation) : rotation));
        const auto expected_offset = mirror ? mirror_vector(offset) : offset;
        float dot = 0;
        for (int k = 0; k < 4; ++k) dot += expected_rotation[k] * target[joint].rotation[k];
        check(std::abs(dot) > 0.99999f, "A native body half turn preserves anatomical rotation direction independently of limb mirroring");
        for (int k = 0; k < 3; ++k)
            check(near(target[joint].position[k] - native[joint].position[k], expected_offset[k]),
                  "Switch conversion keeps authored parent-local offset signs in the turned native body");
        for (std::size_t j = 0; j < native.size(); ++j) {
            check(target[j].scale == native[j].scale, "Facing conversion preserves native bone scales");
            if (j != joint) check(target[j] == native[j], "Facing conversion changes only the authored limb, never the native skeleton");
        }
    }
}
void diagnostic_history_is_bounded() {
    DiagnosticBuffer history;
    for (int i = 0; i < 5000; ++i) {
        DiagnosticSample sample; sample.at = i / 240.0; sample.sequence = i;
        // Deliberately submit poses at callback frequency: the history enforces its own cap.
        sample.native.resize(1); sample.desired.resize(1); sample.render_native.resize(1); sample.render_edited.resize(1);
        history.append(std::move(sample));
    }
    check(history.samples.size() <= DiagnosticBuffer::capacity &&
          history.samples.back().at - history.samples.front().at <= DiagnosticBuffer::duration,
          "Diagnostic callback history is bounded by both count and duration");
    std::size_t poses = 0;
    for (const auto& sample : history.samples) {
        if (!sample.native.empty()) ++poses;
        else check(sample.render_edited.empty(), "Post-edit diagnostic poses obey the same sampling/memory bound as native poses");
    }
    check(poses <= 151, "Diagnostic poses cannot grow past the 10 Hz time window even at 240 callbacks per second");
    const auto size = history.samples.size();
    DiagnosticSample bad; bad.at = std::numeric_limits<double>::quiet_NaN(); history.append(std::move(bad));
    check(history.samples.size() == size, "Invalid diagnostic timestamps cannot corrupt the ring");
    DiagnosticSample reset; reset.at = 0; history.append(std::move(reset));
    check(history.samples.size() == 1, "Clock reset starts a fresh diagnostic window");
    check(diagnostic_csv("a,\"b\"") == "\"a,\"\"b\"\"\"", "Diagnostic CSV quotes names correctly");
}
void authored_gameplay_matches_f8_limbs_and_source_key_times() {
    Animation animation;
    animation.stance = "regular"; animation.switch_stance = false;
    const auto edit = from_euler({23, -11, 8});
    const Vec3 offset{0.03f, -0.02f, 0.01f};
    animation.tracks.push_back({341, {{0, edit, offset, Ease::linear}}});
    animation.tracks.push_back({board_joint, {{0, edit, {10, 10, 10}, Ease::linear}}});
    for (const bool goofy : {false, true}) for (const bool switched : {false, true}) {
        std::vector<Joint> native(skeleton_joints);
        native[placement_joint].rotation = from_euler({0, goofy ? -125.0f : 37.0f, 0});
        native[hips_joint].rotation = from_euler({12, switched ? 180.0f : 0.0f, -8});
        native[341].rotation = from_euler({67, 11, -20});
        native[8].rotation = from_euler({-32, 48, 91});
        native[341].position = {0.1f, -0.2f, 0.03f};
        auto expected = native; apply_edits(animation, 0, expected);
        auto actual = native; apply_authored_edit_layer(animation, 0, actual, 1);
        check(actual[341] == expected[341], "Gameplay uses the exact F8 local edit on the physical left leg in every stance");
        for (std::size_t j = 0; j < native.size(); ++j)
            if (j != 341) check(actual[j] == native[j], "An authored left-leg track never swaps into the right leg or imports a source pose");
        actual = native; apply_authored_edit_layer(animation, 0, actual, 0);
        check(actual == native, "A zero-weight anatomical edit is a strict no-op");
        actual = native; apply_authored_edit_layer(animation, 0, actual, 0.5f);
        for (int k = 0; k < 3; ++k)
            check(near(actual[341].position[k], native[341].position[k] + offset[k] * 0.5f), "Authored offsets blend without changing local signs");
    }
    PlaybackEpisode episode;
    episode.observe(10, motion_state(100)); episode.observe(10.1, motion_state(200)); episode.select(10.2);
    const auto first = episode.sample(10.2, 0.2f, 0.75f, 2, 1, 0.3f, true);
    check(near(first.time, 0.3f), "Late trick recognition catches up from takeoff instead of restarting the edit timeline");
    const auto short_air = episode.sample(10.3, 0.2f, 0.75f, 2, 1, 0.3f, true);
    const auto long_air = episode.sample(10.3, 0.2f, 0.75f, 2, 1, 1.8f, true);
    check(near(short_air.time, 0.4f) && near(short_air.time, long_air.time), "Source key seconds are independent of ballistic airtime estimates");
    check(near(episode.sample(10.3, 0.2f, 0.75f, 2, 0.25f, 0.3f, true).time, 0.4f) &&
          near(episode.sample(10.3, 0.2f, 0.75f, 2, 4, 0.3f, true).time, 0.4f),
          "Quarter-speed and accelerated studio previews cannot retime gameplay edit keys");
    episode.select(10.31);
    check(near(episode.sample(10.32, 0.2f, 0.75f, 2, 1, 1, true).time, 0.42f), "Refining the named trick cannot reset source key time");
    episode.observe(10.6, motion_state(100)); episode.observe(10.64, motion_state(100));
    const auto reception = episode.sample(11.4, 0.2f, 0.75f, 2, 1, 1, true);
    check(near(reception.time, 1.5f) && reception.weight == 1 && !reception.finished,
          "Keys past one second remain active through source reception instead of fading at landing plus 600 ms");
    const auto finished = episode.sample(12, 0.2f, 0.75f, 2, 1, 1, true);
    check(finished.time == 2 && finished.weight == 0 && finished.finished, "Source playback ends and releases at clip duration without looping");
}
// A trick fitted to the game's air: the flip at its own pace, the landing on the game's touchdown.
void fitting_the_air_keeps_the_flip() {
    const float recorded = 0.63f;
    check(near(fitted_air_time(0.2f, recorded, 1.0f), 0.2f), "A longer air leaves the trick at its own pace");
    check(near(fitted_air_time(0.5f, recorded, 1.0f), 0.5f), "and is never slowed down to fit (it came in late)");
    check(near(fitted_air_time(5.0f, recorded, 1.0f), recorded - 1.0f / 60, 1e-3f), "Still in the air: it comes to just before its landing");
    {
        const float a = fitted_air_time(0.58f, recorded, 1.0f), b = fitted_air_time(0.62f, recorded, 1.0f), c = fitted_air_time(0.7f, recorded, 1.0f);
        check(a < b && b < c && c < recorded - 1.0f / 60, "and keeps moving towards it, never standing still");
    }
    check(near(fitted_air_time(0.2f, recorded, 0.5f), 0.252f), "A shorter air plays the air a little faster");
    check(near(fitted_air_time(0.2f, recorded, 0.1f), 0.26f), "but never more than 1.3 times as fast");
    check(near(landed_time(0, 1.3f, 1.25f), 1.25f) && near(landed_time(0.08f, 1.3f, 1.25f), 1.38f),
          "At touchdown a recording a little behind catches up to its landing over 80 ms");
    check(near(landed_time(0.16f, 1.3f, 1.1f), 1.46f) && landed_time(0.08f, 1.3f, 1.1f) < 1.3f,
          "and one far behind over longer, never more than about 2.25 times its pace");
    check(near(landed_time(0.5f, 1.3f, 1.3f), 1.8f), "and from its landing on plays at its own pace");
    PlaybackEpisode episode;
    episode.observe(10, MotionState::riding);
    episode.observe(10.1, MotionState::air);
    episode.select(10.15);
    const auto early = episode.sample(10.3, 0.7f, 1.33f, 2, 1, 1.0f, true, true);
    check(near(early.time, 0.9f), "In game the flip plays at its own pace in a long air");
    const auto before = episode.sample(10.55, 0.7f, 1.33f, 2, 1, 1.0f, true, true);
    check(near(before.time, 1.15f), "An air expected long is not slowed down");
    // The board touches at 10.56; the game is sure two frames later. The landing starts at the touch.
    episode.observe(10.56, MotionState::riding);
    const auto touch = episode.sample(10.57, 0.7f, 1.33f, 2, 1, 1.0f, true, true);
    check(episode.phase == PlaybackPhase::air && touch.time > before.time && touch.time < 1.33f,
          "At the touch the landing starts, before the game is sure of it");
    float last = touch.time;
    bool steady = true;
    for (int i = 1; i <= 12; ++i) {
        const double at = 10.57 + i / 60.0;
        episode.observe(at, MotionState::riding);
        const auto s = episode.sample(at, 0.7f, 1.33f, 2, 1, 1.0f, true, true);
        steady = steady && s.time >= last && s.time - last < 0.06f; // no frame jumps more than about 3 frames' worth
        last = s.time;
    }
    check(episode.phase == PlaybackPhase::landing && steady && last > 1.33f, "and runs on into the landing without a jump");
}
void native_clock_follows_the_game() {
    // A trick clip at 60 fps: every body joint turning its own way (pop at 0.2 s, landing at 0.7 s), and
    // another pop of the same trick, the same moves over a longer air (pop 0.25 s, landing 0.85 s).
    const auto angle = [](std::size_t j, float t) { return 0.6f * std::sin(2.1f * t + 0.37f * static_cast<float>(j)) + 0.3f * std::sin(5.3f * t + static_cast<float>(j)); };
    const auto turn = [&](std::size_t j, float t, float size = 1.0f) {
        const float a = size * angle(j, t) / 2;
        return j % 2 ? Quat{0, 0, std::sin(a), std::cos(a)} : Quat{std::sin(a), 0, 0, std::cos(a)};
    };
    // `warp` takes the clip's own time to the first clip's; `size` makes its moves bigger (a higher pop).
    const auto make = [&](float up, float down, float size, auto warp) {
        auto clip = std::make_shared<Clip>();
        clip->joints = skeleton_joints;
        for (int f = 0; f < 90; ++f) {
            const float t = static_cast<float>(f) / 60;
            clip->times.push_back(t);
            for (std::size_t j = 0; j < skeleton_joints; ++j) {
                Joint joint;
                if (j >= 2 && j < 375) joint.rotation = turn(j, warp(t), size);
                clip->frames.push_back(joint);
            }
        }
        clip->takeoff = up;
        clip->landing = down;
        return clip;
    };
    const auto own = make(0.2f, 0.7f, 1.0f, [](float t) { return t; });
    const auto higher = make(0.25f, 0.85f, 1.35f, [](float t) { return t <= 0.25f ? t - 0.05f : t >= 0.85f ? t - 0.15f : 0.2f + (t - 0.25f) * 0.5f / 0.6f; });
    const auto models = build_native_models({own, higher}, 0);
    // The game's pose: one of the clips at `t`, as it is or mirrored.
    const auto pose_of = [&](const Clip& clip, float t, bool mirrored) {
        std::vector<Joint> pose(skeleton_joints);
        clip.sample(t, pose);
        if (!mirrored) return pose;
        std::vector<Joint> out(skeleton_joints);
        for (std::size_t j = 0; j < skeleton_joints; ++j) {
            out[mirror_joint[j]] = pose[j];
            out[mirror_joint[j]].rotation = mirror_rotation(pose[j].rotation);
        }
        return out;
    };
    {
        // The game plays the edit's own clip a little slower than it was made, from 0.03 s after its pop.
        NativeClock clock;
        clock.start(models);
        float worst = 0;
        bool locked = true;
        for (int i = 0; i < 30; ++i) {
            const float game = 0.23f + 0.93f * static_cast<float>(i) / 60;
            const auto r = clock.update(pose_of(*own, game, false), 1.0f / 60);
            locked = locked && r.locked && !r.lost && r.clip == 0 && !r.mirrored;
            if (i >= 3) worst = std::max(worst, std::abs(r.time - game));
        }
        check(locked && worst < 0.006f, "The clock locks onto the game's clip and follows its time to within a third of a frame");
        // The game goes into another animation: its pose leaves the clip.
        std::vector<Joint> other(skeleton_joints);
        for (std::size_t j = 2; j < 375; ++j) other[j].rotation = turn(j + 7, 3.0f);
        NativeClock::Reading r;
        for (int i = 0; i < 4; ++i) r = clock.update(other, 1.0f / 60);
        check(r.lost && r.confidence < 0.5f, "and lets go when the game's pose leaves it, fading");
    }
    {
        // The game plays the higher pop, mirrored (the other stance): read across onto the edit's own clip.
        NativeClock clock;
        clock.start(models);
        float worst = 0;
        bool right = true;
        for (int i = 0; i < 30; ++i) {
            const float game = 0.27f + static_cast<float>(i) / 60;
            const auto r = clock.update(pose_of(*higher, game, true), 1.0f / 60);
            const float expected = 0.2f + (game - 0.25f) * 0.5f / 0.6f;
            right = right && r.locked && r.clip == 1 && r.mirrored;
            if (i >= 3) worst = std::max(worst, std::abs(r.time - expected));
        }
        check(right && worst < 0.01f, "Another pop of the trick, the other way round, is found and read across through the marks");
    }
    {
        NativeClock clock;
        clock.start(models);
        std::vector<Joint> riding(skeleton_joints);
        const auto r = clock.update(riding, 1.0f / 60);
        check(!r.locked, "A pose that is not the trick is not followed");
    }
    {
        // A recording of the trick (the edit's own clip) that drifts off the game's own clip towards the landing,
        // as the game's corrections in it grow: the clock moves onto the game's clip, without a jump in time.
        auto drifting = std::make_shared<Clip>(*own);
        for (std::size_t f = 0; f < drifting->times.size(); ++f) {
            const float drift = std::max(0.0f, drifting->times[f] - 0.35f) * 30 * 3.14159265f / 180;
            for (std::size_t j = 2; j < 375; ++j) {
                auto& q = drifting->frames[f * skeleton_joints + j].rotation;
                q = normalised(multiply(q, Quat{0, std::sin(drift / 2), 0, std::cos(drift / 2)}));
            }
        }
        const auto both = build_native_models({drifting, own}, 0);
        NativeClock clock;
        clock.start(both);
        bool moved = false, smooth = true;
        float last = -1;
        int on = -1;
        for (int i = 0; i < 40; ++i) {
            const float game = 0.23f + static_cast<float>(i) / 60;
            const auto r = clock.update(pose_of(*own, game, false), 1.0f / 60);
            if (i == 0) on = r.clip;
            moved = moved || r.switched;
            if (last >= 0) smooth = smooth && r.time >= last && r.time - last < 0.03f;
            last = r.time;
        }
        check(on == 0 && moved && smooth && std::abs(last - (0.23f + 39.0f / 60)) < 0.01f,
              "A recording drifting off the game's clip hands over to the game's clip, in time and without a jump");
    }
}
void native_clips_pop_where_the_game_does() {
    // A library clip at 60 fps: the board flat, then from frame 8 (0.133 s) its tail goes down 8 degrees a
    // frame. Its height dips as the tail strikes (0.15 s), deepest as it lands (frame 38, 0.633 s), and a
    // little as he rolls away (0.8 s).
    const auto make = [](float takeoff, float landing) {
        Clip clip;
        clip.joints = 1;
        const auto dip = [](float t, float at, float depth) { return -depth * std::exp(-((t - at) / 0.03f) * ((t - at) / 0.03f)); };
        for (int f = 0; f < 60; ++f) {
            const float t = static_cast<float>(f) / 60;
            clip.times.push_back(t);
            const float turn = f >= 8 ? static_cast<float>(f - 7) * 8 * 3.14159265f / 180 : 0;
            Joint board;
            board.rotation = {std::sin(turn / 2), 0, 0, std::cos(turn / 2)};
            board.position = {0, dip(t, 0.15f, 0.15f) + dip(t, 38.0f / 60, 0.15f) + dip(t, 0.8f, 0.05f), 0};
            clip.board.push_back(board);
        }
        clip.takeoff = takeoff;
        clip.landing = landing;
        return clip;
    };
    auto late = make(0.283f, 0.6f); // the library's pop shuv-it low: its pop a later dip
    check(place_native_marks(late) && near(late.takeoff, 8.0f / 60 + 0.05f) && near(late.landing, 0.6f),
          "A late pop is put just after the board tips, where the game leaves the ground; a landing near enough stays");
    auto good = make(0.183f, 0.617f);
    check(!place_native_marks(good) && near(good.takeoff, 0.183f) && near(good.landing, 0.617f), "Marks already there are left alone");
    auto early = make(0.1f, 0.617f);
    check(!place_native_marks(early) && near(early.takeoff, 0.1f), "and a pop earlier than that too: only late ones are the library's mistake");
    auto spinning = make(0.183f, 0.433f); // FS 360 pop shuv-it low: landed with the board still turning
    check(place_native_marks(spinning) && near(spinning.landing, 38.0f / 60), "A landing marked in the air goes to where the board comes down");
    auto both = make(0.417f, 0.867f); // nollie 360 pop shuv-it low: both marks a dip late
    check(place_native_marks(both) && near(both.takeoff, 8.0f / 60 + 0.05f) && near(both.landing, 38.0f / 60),
          "and one looked for after a late pop is found again from the right one");
    auto boardless = make(0.283f, 0.6f);
    boardless.board.clear();
    check(!place_native_marks(boardless) && near(boardless.takeoff, 0.283f), "A clip without its board keeps its mark");
    auto piece = make(-1, -1);
    check(!place_native_marks(piece) && piece.takeoff < 0, "and a piece with no pop gets none");
}
void native_playback_contracts() {
    EditTransition transition;
    std::array<Offset, 1> a{}, b{}; a[0].offset[0] = 0.4f; b[0].offset[0] = -0.4f;
    check(near(transition.evaluate(1, 1, 1, 10, a)[0].offset[0], 0), "The first edit layer fades from native motion");
    check(near(transition.evaluate(1.151, 1, 1, 10, a)[0].offset[0], 0.4f), "An established edit layer reaches its target");
    check(near(transition.evaluate(1.17, 1, 1, 11, b)[0].offset[0], 0.4f),
          "Ollie-to-flip selection starts from the previous edit delta instead of resetting the visible pose");
    check(near(transition.evaluate(1.245, 1, 1, 11, b)[0].offset[0], 0, 0.001f), "A changed preset blends smoothly between deltas");
    check(near(transition.evaluate(1.25, 2, 1, 11, b)[0].offset[0], 0), "Respawn cannot carry a previous entity's edit layer");
    PlaybackEpisode episode;
    // Real failed hardflip trace: riding -> five crouch samples -> pop -> air.
    // Production classification must retain ground entry through state 103.
    PlaybackEpisode crouched;
    crouched.observe(2.20, motion_state(100));
    for (int i = 0; i < 5; ++i) crouched.observe(2.2158 + i / 60.0, motion_state(103));
    check(crouched.observe(2.2998, motion_state(200)) && crouched.entry == EntryKind::ground,
          "Native 100/103/200 takeoff retains ground entry so a saved ground preset can be selected");
    const auto crouch_episode = crouched.id;
    check(!crouched.observe(2.3156, motion_state(201)) && crouched.id == crouch_episode && crouched.selection_open(2.3338),
          "200/201 remains one selectable airborne episode after the crouch");
    check(motion_state(std::nullopt) == MotionState::unknown && motion_state(102) == MotionState::unknown &&
          motion_state(300) == MotionState::offboard && motion_state(504) == MotionState::offboard &&
          motion_state(401) == MotionState::rail,
          "Recognizing grounded crouch does not guess other states or confuse bails and rails");
    check(!episode.observe(0, MotionState::riding) && episode.observe(0.1, MotionState::air), "Riding to air starts a new observed episode");
    check(episode.entry == EntryKind::ground, "Ground takeoff carries a ground binding");
    episode.select(0.1);
    (void)episode.sample(0.1, 0.2f, 1, 2, 1, 1);
    const auto air = episode.sample(0.2, 0.2f, 1, 2, 1, 1);
    const auto original_id = episode.id;
    episode.observe(0.21, MotionState::rail);
    const auto release = episode.sample(0.25, 0.2f, 1, 2, 1, 1);
    check(episode.phase == PlaybackPhase::rail_release && near(release.time, air.time) && release.weight < air.weight,
          "Rail entry releases to native constraints instead of advancing/holding a fake airborne grind");
    check(episode.sample(0.3, 0.2f, 1, 2, 1, 1).finished, "Rail release is bounded");
    episode.observe(0.31, MotionState::rail);
    check(episode.observe(0.4, MotionState::air) && episode.entry == EntryKind::rail && episode.id > original_id,
          "A grind pop-out is a fresh rail-entry episode");
    check(episode.sample(0.4, 0.2f, 1, 2, 1, 1).time < 0, "A new pop-out cannot inherit the entry preset or source clock");
    episode.observe(0.5, MotionState::unknown);
    check(episode.phase == PlaybackPhase::idle, "Unavailable native state cancels pending edits");
    episode.observe(1, MotionState::riding); episode.observe(1.1, MotionState::air); episode.select(1.1);
    (void)episode.sample(1.15, 0.2f, 1, 2, 1, 1);
    episode.observe(1.2, MotionState::riding); episode.observe(1.21, MotionState::air);
    check(episode.phase == PlaybackPhase::air, "A brief contact does not become a landing");
    episode.observe(1.3, MotionState::riding); episode.observe(1.334, MotionState::riding);
    check(episode.phase == PlaybackPhase::landing && episode.landing == 1.3, "Landing confirms elapsed contact while retaining first-contact time");
    episode.observe(1.35, MotionState::offboard);
    check(episode.phase == PlaybackPhase::idle && episode.clock.time < 0, "Bails and off-board states release the episode");

    PresentationLatch<int> cohort;
    auto first = std::make_shared<const int>(1), second = std::make_shared<const int>(2);
    check(*cohort.get(10, 1, first) == 1 && *cohort.get(10, 1, second) == 1,
          "Body and board in one presentation pin the same schedule despite a producer update");
    check(*cohort.get(11, 1, second) == 2 && !cohort.get(11, 2, {}), "The next presentation advances; invalidation releases an already pinned schedule");
    {
    struct Preview { Joint foot, board; };
    auto preview_before = std::make_shared<Preview>(), preview_after = std::make_shared<Preview>();
    preview_before->foot.position = {0, 0.1f, 0};
    preview_after->foot.position = {0, 0.8f, 0}; preview_after->board.position = {0, 0.7f, 0};
    PresentationLatch<Preview> preview;
    const auto body = preview.get(1, 0, preview_before);
    const auto board_sample = preview.get(1, 0, preview_after);
    check(body == board_sample && near(body->foot.position[1] - board_sample->board.position[1], 0.1f),
          "Preview body and board retain contact when preview_before producer publishes between their callbacks");
    const auto board_first = preview.get(2, 0, preview_after);
    check(board_first == preview.get(2, 0, preview_before) && near(board_first->foot.position[1] - board_first->board.position[1], 0.1f),
          "Preview sampling is coherent when board renders first and on repeated publications");
    check(!preview.get(2, 1, {}), "Closing or invalidating preview releases its current presentation");
    PresentationLatch<Preview> no_sample;
    check(!no_sample.get(3, 0, {}) && !no_sample.get(3, 0, preview_before),
          "A missing first preview sample cannot publish only one half later in the same presentation");
    }

    std::vector<Joint> native(skeleton_joints), source(skeleton_joints);
    native[7].position = {1, 2, 3}; native[7].scale = {1.2f, 0.8f, 1};
    source[7].position = {90, 80, 70}; source[7].rotation = from_euler({90, 30, 80});
    Animation keys; keys.tracks.push_back({7, {{0, {}, {}, Ease::linear}}, false, false});
    auto fresh = native;
    apply_native_edit_layer(keys, 0, source, fresh, false, 1);
    check(fresh == native, "An untouched source preset preserves the complete native pose exactly, regardless of its recorded coordinates");
    Joint board; board.position = {80, 60, 40}; board.rotation = from_euler({0, 70, 0});
    keys.tracks.push_back({board_joint, {{0, {}, {}, Ease::linear}}, false, false});
    check(board_edited(board, keys, 0, from_euler({0, 90, 0})) == board, "Neutral board keys preserve native physics exactly");
    keys.tracks[1].keys[0].offset = {0.2f, 0, 0};
    const auto board_edit = board_edited(board, keys, 0, from_euler({0, 90, 0}));
    check(near(board_edit.position[2], board.position[2] - 0.2f) && board_edit.rotation == board.rotation && board_edit.scale == board.scale,
          "A board offset uses its explicit edit frame and does not import source coordinates or modify rotation/scale");
    keys.tracks[0].keys[0].offset = {0.2f, 0, 0};
    apply_native_edit_layer(keys, 0, source, fresh, false, 0.5f);
    check(fresh[7].rotation == native[7].rotation, "A translation-only edit leaves native joint rotation exactly unchanged");
    check(fresh[7].scale == native[7].scale && fresh[placement_joint] == native[placement_joint] && fresh[50] == native[50],
          "Authored edits retain native scale, placement and attachment ownership");
    EditPublicationCache cache;
    cache.commit(1, 10, 100, native, fresh);
    auto repeated = fresh; cache.restore(1, 10, 100, repeated);
    check(repeated == native, "Repeated publication unwraps our prior writes before reapplying keys");
    apply_native_edit_layer(keys, 0, source, repeated, false, 0.5f);
    check(repeated == fresh, "Network capture of an already edited render pose applies the keys exactly once");
    const auto deltas = pose_edit_deltas(native, fresh);
    auto moved_native = native; moved_native[7].position[1] += 20;
    apply_pose_deltas(deltas, moved_native);
    check(near(moved_native[7].position[1], fresh[7].position[1] + 20),
          "Blended deltas follow a moving native pose without teleporting to an old absolute skeleton");
    auto changed_native = fresh; changed_native[7].position[1] += 1;
    const auto fresh_y = changed_native[7].position[1];
    cache.restore(1, 10, 100, changed_native);
    check(changed_native[7].position[1] == fresh_y && changed_native[7].position[0] == native[7].position[0],
          "A fresh Y correction survives while our exact previous X edit is unwrapped to avoid accumulation");
    repeated = fresh; cache.restore(1, 11, 100, repeated);
    check(repeated == fresh, "A new animation callback cannot reuse an earlier update's baseline");
    repeated = fresh; cache.restore(2, 10, 100, repeated);
    check(repeated == fresh, "Entity replacement invalidates repeated-publication recovery");

    Animation metadata; metadata.source = {"native_asset", "animation/dingo/test", "rig", "skater-placement"}; metadata.entry = "rail";
    metadata.source.position_codec = "native-piec-v1-extent";
    const auto back = from_json(to_json(metadata));
    check(back.source.kind == metadata.source.kind && back.source.asset == metadata.source.asset &&
          back.source.position_codec == metadata.source.position_codec && back.entry == "rail",
          "Source provenance and entry bindings survive save/load without becoming rendered captures");
    std::vector<Preset> presets{{"Ground", "ollie", "", false, false, "regular", false, "ground"},
                                {"Rail", "ollie", "", false, false, "regular", false, "rail"}};
    select_presets(presets, std::nullopt);
    check(presets[0].active && presets[1].active, "Fixed selection preserves separate ground and rail-exit presets");
    {
        // PLAY: with no pack on only the tricks switched to PLAY play; beside packs that are on they play where no pack's
        // trick of that trick, pop and stance does.
        std::vector<Preset> mine{{"Flip A", "kickflip", "mid", false, false, "regular", false, "ground"},
                                 {"Flip B", "kickflip", "mid", false, false, "regular", false, "ground"},
                                 {"Heel", "heelflip", "mid", false, false, "regular", false, "ground"}};
        const std::set<std::string> playing{"Flip A", "Heel"};
        select_presets(mine, std::span<const PresetPack>{}, nullptr, &playing);
        check(mine[0].active && !mine[1].active && mine[2].active, "With no pack on, only the tricks switched to PLAY play");
        PresetPack pack;
        pack.name = "Pack";
        pack.slots[preset_slot("kickflip", "mid")] = {"Flip B"};
        select_presets(mine, std::span<const PresetPack>(&pack, 1), nullptr, &playing);
        check(!mine[0].active && mine[1].active && mine[2].active, "Beside a pack that is on, a trick on PLAY plays where the pack has none of it");
        pack.off.insert("Flip B");
        select_presets(mine, std::span<const PresetPack>(&pack, 1), nullptr, &playing);
        check(mine[0].active && !mine[1].active, "and a trick switched off in its pack does not play from it");
    }
}
// The game's own clips are recognised by the pose: a piece is found when the body moves through it in
// step, at the right time, the other way round too - and not when he only stands in a pose like it.
void pieces_are_recognised_by_the_pose() {
    const auto make = [](float fa, float fb, float spread) {
        Clip clip;
        clip.joints = skeleton_joints;
        for (int f = 0; f <= 60; ++f) clip.times.push_back(static_cast<float>(f) / 30.0f);
        clip.frames.resize(clip.times.size() * clip.joints);
        for (std::size_t f = 0; f < clip.times.size(); ++f)
            for (std::uint16_t j = 2; j < 60; ++j) {
                const float t = clip.times[f];
                clip.frames[f * clip.joints + j].rotation =
                    from_euler({70.0f * std::sin(fa * t + spread * j), 45.0f * std::cos(fb * t + 0.11f * j), 10.0f});
            }
        return clip;
    };
    const auto piece = make(9.0f, 7.0f, 0.37f), other = make(5.0f, 12.0f, 1.3f);
    {
        // A grind is told by the pose he holds: the saved loop it is, which way round, and where in it.
        const std::array<const Clip*, 2> loops{&other, &piece};
        std::vector<Joint> held(piece.frame(30).begin(), piece.frame(30).end());
        auto fits = held_pose_fits(loops, held);
        check(fits.size() == 2 && fits[0].piece == 1 && !fits[0].mirrored && fits[0].score < 1e-4f && std::abs(fits[0].time - 1.0f) < 1e-4f &&
                  fits[1].score > fits[0].score,
              "A held grind pose is told by its saved loop, the frame it holds, and the other loop is further");
        const auto mirrored = mirror_clip(piece);
        held.assign(mirrored.frame(12).begin(), mirrored.frame(12).end());
        fits = held_pose_fits(loops, held);
        check(!fits.empty() && fits[0].piece == 1 && fits[0].mirrored && fits[0].score < 1e-4f, "and held the other way round, as mirrored");
    }
    PieceTracker tracker;
    tracker.set_pieces({&piece});
    check(tracker.looking_for() == 2, "A moving piece is looked for as it is and mirrored");
    std::vector<Joint> pose(skeleton_joints);
    // Standing in its first pose for a second: never taken for it.
    double at = 0;
    bool fired = false;
    for (int i = 0; i < 60; ++i, at += 1.0 / 60) {
        for (std::size_t j = 0; j < pose.size(); ++j) pose[j] = piece.frame(0)[j];
        fired |= tracker.update(at, pose).has_value();
    }
    check(!fired, "Holding a pose like the piece's is not playing it");
    // Then playing it, at 60 frames a second.
    std::optional<PieceMatch> found;
    double found_at = -1, played_from = at;
    for (; at - played_from < 1.8; at += 1.0 / 60) {
        piece.sample(static_cast<float>(at - played_from), pose);
        const auto m = tracker.update(at, pose);
        if (m && !found) found_at = at;
        if (m) found = m;
    }
    check(found.has_value() && found_at - played_from < 0.45, "Playing the piece is found within half a second");
    check(found && !found->mirrored && std::abs(found->time - static_cast<float>(at - 1.0 / 60 - played_from)) < 0.08f,
          "at the time it has got to");
    // Something else moving is not it.
    tracker.reset();
    fired = false;
    for (double t = 0; t < 1.8; t += 1.0 / 60, at += 1.0 / 60) {
        other.sample(static_cast<float>(t), pose);
        fired |= tracker.update(at, pose).has_value();
    }
    check(!fired, "Another motion is not taken for the piece");
    // Played the other way round, it is found mirrored.
    tracker.reset();
    found.reset();
    std::vector<Joint> straight(skeleton_joints);
    for (double t = 0; t < 1.8; t += 1.0 / 60, at += 1.0 / 60) {
        piece.sample(static_cast<float>(t), straight);
        for (std::uint16_t j = 0; j < skeleton_joints; ++j) pose[mirror_joint[j]] = mirror_pose(straight[j]);
        if (const auto m = tracker.update(at, pose)) found = m;
    }
    check(found && found->mirrored, "Played the other way round, it is found mirrored");
}
// Matching the studio: a joint that is really bent takes the pose it was made on with the bend on it, a
// joint whose key only holds the pose stays the game's, and the bend alone is added when matching is off.
float angle_between(const Quat& a, const Quat& b) {
    const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2 * std::acos(std::clamp(d, 0.0f, 1.0f));
}
void bent_joints_match_the_studio() {
    std::vector<Joint> native(skeleton_joints), studio(skeleton_joints);
    native[9].rotation = from_euler({10, 0, 0});
    studio[9].rotation = from_euler({70, 0, 0});
    native[342].rotation = from_euler({5, 0, 0});
    studio[342].rotation = from_euler({60, 0, 0});
    Animation animation;
    set_key(animation, 9, 0.0f, {from_euler({0, 0, 40}), {}});
    set_key(animation, 342, 0.0f, {from_euler({0, 0, 1}), {}});
    auto matched = native;
    apply_authored_edit_layer(animation, 0, matched, 1.0f, nullptr, studio, 1.0f);
    check(same_turn(matched[9].rotation, normalised(multiply(studio[9].rotation, from_euler({0, 0, 40})))),
          "A bent knee takes the studio's pose with the bend on it");
    check(angle_between(matched[342].rotation, normalised(multiply(native[342].rotation, from_euler({0, 0, 1})))) < 0.01f,
          "A key that barely bends leaves the game's pose");
    auto added = native;
    apply_authored_edit_layer(animation, 0, added, 1.0f, nullptr, studio, 0.0f);
    check(same_turn(added[9].rotation, normalised(multiply(native[9].rotation, from_euler({0, 0, 40})))),
          "With matching off the bend is added to the game's pose");
    // Whole body: a joint with no key at all follows the studio's pose too.
    native[343].rotation = from_euler({0, 30, 0});
    studio[343].rotation = from_euler({0, -20, 0});
    auto whole = native;
    apply_authored_edit_layer(animation, 0, whole, 1.0f, nullptr, studio, 1.0f, 1.0f);
    check(same_turn(whole[343].rotation, studio[343].rotation), "With the whole body matched an unkeyed foot follows the recording");
    check(same_turn(whole[9].rotation, normalised(multiply(studio[9].rotation, from_euler({0, 0, 40})))), "and the keys still go on top");
    // and the hips sit where they do in the recording: how high he is over the board.
    native[7].position = {0, 0.9f, 0};
    studio[7].position = {0.05f, 0.7f, -0.02f};
    auto placed = native;
    apply_authored_edit_layer(animation, 0, placed, 1.0f, nullptr, studio, 1.0f, 1.0f);
    check(near(placed[7].position[1], 0.7f) && near(placed[7].position[0], 0.05f), "With the whole body matched the hips sit where the recording has them");
    // Mirrored playback reads the studio's pose mirrored, so the bend lands on the joint it now drives.
    Clip clip;
    clip.joints = skeleton_joints;
    clip.times = {0.0f};
    clip.frames.assign(skeleton_joints, Joint{});
    clip.frames[9].rotation = from_euler({70, 0, 0});
    const auto mirrored = studio_pose(clip, 0, true);
    check(same_turn(mirrored[mirror_joint[9]].rotation, mirror_rotation(clip.frames[9].rotation)), "and the studio's pose is mirrored with the keys");
}
// Joint limits: the game's own range for each joint. The average pose is left alone, limiting twice is the
// same as once, and a knee is a hinge - it cannot be twisted round or bent the wrong way.
void joints_stay_in_their_range() {
    check(has_joint_limit(9) && has_joint_limit(342) && has_joint_limit(48) && !has_joint_limit(7), "Knees and elbows are limited, the hips are not");
    for (const auto& l : joint_limits) {
        const Quat ref{l.reference[0], l.reference[1], l.reference[2], l.reference[3]};
        check(same_turn(limit_rotation(l.joint, ref), ref), "A joint's average pose is inside its limits");
        for (int i = 0; i < 40; ++i) {
            const auto q = from_euler({static_cast<float>((i * 37) % 360 - 180), static_cast<float>((i * 53) % 180 - 90),
                                       static_cast<float>((i * 71) % 360 - 180)});
            const auto once = limit_rotation(l.joint, q);
            check(angle_between(limit_rotation(l.joint, once), once) < 0.02f, "Limiting twice is the same as once");
        }
    }
    const auto& knee = *std::find_if(joint_limits.begin(), joint_limits.end(), [](const JointLimit& l) { return l.joint == 9; });
    const Quat ref{knee.reference[0], knee.reference[1], knee.reference[2], knee.reference[3]};
    const Vec3 axis{knee.axis[0], knee.axis[1], knee.axis[2]};
    const auto twisted = normalised(multiply(ref, axis_angle(axis, 1.2f)));
    check(angle_between(limit_rotation(9, twisted), ref) < 0.2f, "A knee twisted round is turned back");
}
// The lead foot comes from the clip: right foot on the skater's +X side is the game's own goofy layout, and
// mirroring the clip the game's way turns it into the left foot leading.
void lead_foot_is_read_from_the_clip() {
    Clip clip;
    clip.joints = skeleton_joints;
    clip.times = {0.0f};
    clip.frames.assign(skeleton_joints, Joint{});
    // Feet hung straight off the placement, the right one on +X, the left on -X.
    clip.frames[10].position = {0.15f, -0.8f, 0.05f};
    clip.frames[343].position = {-0.2f, -0.8f, 0.07f};
    for (std::uint16_t j = 0; j < skeleton_joints; ++j) {
        if (j == 10 || j == 343 || j == placement_joint) continue;
        clip.frames[j].position = {};
    }
    // Make the feet children of the placement for the test by giving their chain no offsets of its own.
    const auto right = clip_leads_right(clip);
    check(right.has_value() && *right, "Right foot on +X: the clip leads with the right foot");
    const auto mirrored = mirror_body_clip(clip);
    const auto left = clip_leads_right(mirrored);
    check(left.has_value() && !*left, "Mirrored the game's way it leads with the left");
    clip.frames[343].position = {0.1f, -0.8f, 0.07f};
    check(!clip_leads_right(clip).has_value(), "Feet together cannot tell");
}
// Every trigger the game gives at takeoff, through the one rule that picks the recording: regular, goofy,
// switch, fakie and switch fakie, in front-foot and exact-stance modes.
void every_stance_finds_its_recording() {
    const auto pick = [](std::vector<StanceCandidate> c, LiveStance live, bool front_foot) { return choose_stance_variants(c, live, front_foot); };
    const StanceCandidate normal_left{false, false, 0}, normal_right{true, false, 0}, fakie_left{false, true, 0}, fakie_right{true, true, 0};
    const std::vector<StanceCandidate> all{normal_left, normal_right, fakie_left, fakie_right};
    // With a recording for every case, each live stance gets its own one, unmirrored, in both modes.
    struct Case { LiveStance live; std::size_t expected; const char* what; };
    for (const auto& c : {Case{{false, false}, 0, "regular normal"}, Case{{true, false}, 1, "regular switch / goofy normal"},
                          Case{{false, true}, 2, "regular fakie"}, Case{{true, true}, 3, "switch fakie / goofy fakie"}})
        for (const bool front : {true, false}) {
            const auto got = pick(all, c.live, front);
            check(got.size() == 1 && got[0] == c.expected && !plays_mirrored(all[got[0]], c.live), c.what);
        }
    // Only a regular normal recording: front foot plays it everywhere (mirrored when the right foot leads),
    // exact stance only riding the way it was made.
    const std::vector<StanceCandidate> one{normal_left};
    check(pick(one, {true, false}, true).size() == 1 && plays_mirrored(one[0], {true, false}), "Front foot: switch plays it mirrored");
    check(pick(one, {false, true}, true).size() == 1 && !plays_mirrored(one[0], {false, true}), "Front foot: fakie plays it, never mirrored");
    check(pick(one, {true, false}, false).empty(), "Exact stance: not for the other foot");
    check(pick(one, {false, true}, false).empty(), "Exact stance: not for fakie when it was made rolling forward");
    // Fakie matters more than the foot: the game's fakie upper body is in a fakie recording, a mirror is exact.
    const std::vector<StanceCandidate> mixed{normal_left, fakie_right};
    const auto fakie_pick = pick(mixed, {false, true}, true);
    check(fakie_pick.size() == 1 && fakie_pick[0] == 1 && plays_mirrored(mixed[1], {false, true}), "Rolling fakie takes the fakie recording, mirrored");
    // The pop the game graded comes first.
    const std::vector<StanceCandidate> pops{{false, false, 1}, {true, false, 0}};
    const auto pop_pick = pick(pops, {false, false}, true);
    check(pop_pick.size() == 1 && pop_pick[0] == 1, "The graded pop wins over the closer stance");
    check(pick({{false, false, -1}}, {false, false}, true).empty(), "A recording for another pop never plays");
    // A recording from before fakie was kept counts as rolling forward.
    const std::vector<StanceCandidate> old{{false, std::nullopt, 0}};
    check(pick(old, {false, false}, false).size() == 1 && pick(old, {false, true}, false).empty(), "Old recordings count as rolling forward");
    // Equally good recordings all come back, so variants cycle between them.
    check(pick({normal_left, normal_left, normal_right}, {false, false}, true).size() == 2, "Equally good recordings cycle");
}
// A fixed pack keeps one recording per trick, pop and stance - and a fakie one is its own, so both play.
void fakie_recordings_have_their_own_slot() {
    std::vector<Preset> presets{{"Kick", "kickflip", "mid", false, false, "regular", false, "ground"},
                                {"Kick fakie", "kickflip", "mid", false, false, "regular", false, "ground"},
                                {"Kick 2", "kickflip", "mid", false, false, "regular", false, "ground"}};
    presets[1].fakie = true;
    select_presets(presets, std::nullopt);
    check(presets[1].active, "The fakie recording plays beside the normal one");
    check(presets[0].active != presets[2].active, "and two normal ones still share a slot");
}
int main(int argc, char** argv) {
    try {
        timing_and_stance_edge_cases();
        merged_library_and_timing();
        preview_trajectory_uses_the_whole_clip();
        native_playback_contracts();
        fitting_the_air_keeps_the_flip();
        native_clips_pop_where_the_game_does();
        native_clock_follows_the_game();
        diagnostic_history_is_bounded();
        retargeted_edits_preserve_native_rig_and_convert_joint_frames();
        switch_edits_follow_the_native_body_facing();
        authored_gameplay_matches_f8_limbs_and_source_key_times();
        packs_select_variants_without_filesystem_order();
    several_packs_play_together();
    pack_slots_are_fixed_or_alternate();
    packs_on_together_and_their_rules();
        research_keeps_short_transitions_and_is_bounded();
        euler_numbers_read_back();
        native_names_and_search();
        native_trick_cards();
        trick_names_match();
        packs_and_playlist();
        market_package_words();
        manual_and_scripted_recording_keep_distinct_rules();
        mirror_pairs_and_comes_back();
        pieces_are_recognised_by_the_pose();
        bent_joints_match_the_studio();
        joints_stay_in_their_range();
        lead_foot_is_read_from_the_clip();
        every_stance_finds_its_recording();
        fakie_recordings_have_their_own_slot();
        mirroring_puts_each_joint_where_its_reflection_belongs();
        dragged_foot_reaches_the_drag();
        turn_keeps_its_speed_through_a_key();
        smooth_keys_never_overshoot();
        smoothing_tidies_a_track();
        smoothing_chosen_keys();
        one_channel_changes_alone();
        solo_turn_holds_the_joints_below();
        animation_targets_keep_their_motion_and_hierarchy();
        slerp_meets_both_ends();
        keys_hold_outside_and_blend_between();
        keys_can_be_dragged_in_time();
        edits_are_on_top_of_the_pose_and_never_grow_it();
        joints_hang_from_their_parents();
        profiles_read_back_and_bad_ones_are_refused();
        clips_read_back_and_broken_ones_are_refused();
        if (argc == 3 && std::string_view(argv[1]) == "--diagnostic-export") {
            DiagnosticBuffer history;
            for (int i = 0; i < 3; ++i) {
                DiagnosticSample sample; sample.at = 100 + i * 0.1; sample.sequence = i; sample.physics = 200;
                sample.trick = "test,\"trick\""; sample.preset = "synthetic fixture";
                sample.live_switch = true; sample.recorded_switch = false; sample.retarget = sample.mirror = true;
                sample.native.resize(skeleton_joints); sample.desired = sample.native;
                sample.desired[7].position[0] = 0.1f;
                sample.desired[7].rotation = from_euler({30, 0, 0});
                sample.native_board = Joint{}; sample.desired_board = Joint{};
                sample.render_edited = sample.desired; sample.render_edited_at = sample.at;
                sample.render_edited_board = sample.desired_board; sample.render_edited_board_at = sample.at;
                sample.source = sample.native; sample.source_board = Joint{};
                sample.board_rig.resize(16); sample.render_board_at = sample.board_rig_at = sample.at;
                sample.observed_phase = "air"; sample.entry = "ground"; sample.source_kind = "native_asset";
                sample.episode = 1; sample.render_edit_sequence = sample.board_edit_sequence = i + 1;
                sample.render_presentation = sample.board_presentation = i + 10;
                history.append(std::move(sample));
            }
            auto summary = write_diagnostic_history(history, argv[2]);
            check(summary.at("samples").get<std::size_t>() == 3 &&
                  near(summary.at("max_local_position_delta").get<float>(), 0.1f) &&
                  near(summary.at("max_local_rotation_delta_degrees").get<float>(), 30.0f, 0.01f),
                  "Diagnostic text export measures known pose differences");
            summary["synthetic_fixture"] = true;
            const auto contents = [&](const char* file) {
                std::ifstream in(std::filesystem::path(argv[2]) / file);
                return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            };
            check(contents("poses.csv").find(",source_sample,7,") != std::string::npos &&
                  contents("board.csv").find(",source_sample,900,") != std::string::npos &&
                  contents("board-rig.csv").find(",render_before_override,15,") != std::string::npos,
                  "Preview diagnostics export source poses and all sixteen physical board bones");
            std::ofstream out(std::filesystem::path(argv[2]) / "summary.json"); out << summary.dump(2);
            std::cout << "Synthetic diagnostic text export passed\n";
        }
        if (argc == 3 && std::string_view(argv[1]) == "--animation-skeleton") {
            std::ifstream in(argv[2]);
            const auto document = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            const auto& joints = document.at("instances")[0].at("object").at("fields").at("Joints");
            check(joints.size() == skeleton_joints, "Native animation skeleton has the supported joint count");
            for (std::size_t i = 0; i < joints.size(); ++i)
                check(parent(static_cast<std::uint16_t>(i)) == joints[i].at("fields").at("Parent").get<int>(),
                      "Every studio parent must match the actual native AntSkeletonAsset");
            std::cout << "Native animation hierarchy: all 395 parents match\n";
        }
        if (argc == 5 && std::string_view(argv[1]) == "--mirror-saved") {
            // A saved animation mirrored the game's way (clip and keys), as the studio does for the other foot.
            std::ifstream json_in(argv[2], std::ios::binary);
            auto animation = from_json(std::string((std::istreambuf_iterator<char>(json_in)), std::istreambuf_iterator<char>()));
            std::filesystem::path clip_path(argv[2]);
            clip_path.replace_extension(".clip");
            std::ifstream clip_in(clip_path, std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(clip_in)), std::istreambuf_iterator<char>());
            const auto clip = clip_from_bytes(bytes);
            auto mirrored = mirror_animation(animation);
            mirrored.name = argv[4];
            mirrored.upgraded.clear();
            const auto mirrored_clip = mirror_clip(clip);
            mirrored.clip.times = {0, mirrored_clip.duration()};
            mirrored.clip.takeoff = mirrored_clip.takeoff;
            mirrored.clip.landing = mirrored_clip.landing;
            const std::filesystem::path out = std::filesystem::path(argv[3]) / argv[4];
            std::filesystem::create_directories(out);
            std::ofstream(out / (std::string(argv[4]) + ".json"), std::ios::binary) << to_json(mirrored);
            const auto clip_out = clip_bytes(mirrored_clip);
            std::ofstream(out / (std::string(argv[4]) + ".clip"), std::ios::binary).write(reinterpret_cast<const char*>(clip_out.data()), static_cast<std::streamsize>(clip_out.size()));
            const auto lead = clip_leads_right(mirrored_clip);
            std::cout << "mirrored; leads " << (lead ? (*lead ? "right" : "left") : "unclear") << "\n";
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--roundtrip") {
            std::ifstream in(argv[2], std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto animation = from_json(text);
            std::cout << to_json(animation);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--zip-test") {
            const std::string clip(70000, '\x01');
            const auto zip = stored_zip({{"presets/Steezy.json", "{\"name\": \"Steezy\"}"}, {"Kick/Kick.json", "{}"}, {"Kick/Kick.clip", clip}, {"README.txt", "hi"}});
            std::ofstream out(argv[2], std::ios::binary);
            out.write(zip.data(), static_cast<std::streamsize>(zip.size()));
            std::cout << "crc of 'hello': " << std::hex << crc32_of("hello") << "\n";
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--limits-check") {
            // How often the game's own clips would be limited, per joint (should be about 1% or less).
            const std::filesystem::path folder(argv[2]);
            std::map<std::uint16_t, std::pair<std::size_t, std::size_t>> counts;
            std::size_t clips = 0;
            for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                if (entry.path().extension() != ".clip" || ++clips > 600) continue;
                std::ifstream binary(entry.path(), std::ios::binary);
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(binary)), std::istreambuf_iterator<char>());
                Clip clip;
                try { clip = clip_from_bytes(bytes); } catch (...) { continue; }
                if (clip.joints != skeleton_joints) continue;
                for (std::size_t f = 0; f < clip.times.size(); f += 3)
                    for (const auto& l : joint_limits) {
                        const auto q = clip.frame(f)[l.joint].rotation;
                        auto& c = counts[l.joint];
                        ++c.second;
                        if (angle_between(limit_rotation(l.joint, q), q) > 0.035f) ++c.first;
                    }
            }
            for (const auto& [joint, c] : counts)
                std::cout << joint << ": " << (100.0 * static_cast<double>(c.first) / static_cast<double>(std::max<std::size_t>(c.second, 1))) << "% limited\n";
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--stance-check") {
            // Where his feet and the board are, in the placement's frame, at the clip's last frame, as it is
            // and mirrored the way the studio mirrors it. For finding why a mirrored skater is off his board.
            std::ifstream binary(argv[2], std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(binary)), std::istreambuf_iterator<char>());
            const auto clip = clip_from_bytes(bytes);
            std::vector<Joint> pose(clip.joints);
            clip.sample(clip.duration(), pose);
            Joint board;
            const bool has_board = clip.sample_board(clip.duration(), board);
            std::cout << "hips chain:";
            for (int at = hips_joint; at >= 0; at = parent(static_cast<std::uint16_t>(at))) std::cout << ' ' << at;
            std::cout << "\nboard: " << (has_board ? "yes" : "no") << '\n';
            const auto show = [&](const char* what, const std::vector<Joint>& p, const Joint& b) {
                const auto placed = world(p, placement_joint);
                const auto in_placement = [&](const Vec3& w) {
                    return rotate(conjugate(placed.rotation), Vec3{w[0] - placed.position[0], w[1] - placed.position[1], w[2] - placed.position[2]});
                };
                const auto v = [](const Vec3& x) {
                    char t[64];
                    std::snprintf(t, sizeof(t), "(%6.3f %6.3f %6.3f)", x[0], x[1], x[2]);
                    return std::string(t);
                };
                std::cout << what << "\n  left foot " << v(in_placement(world(p, 343).position)) << "  right foot "
                          << v(in_placement(world(p, 10).position)) << "\n  hips " << v(in_placement(world(p, hips_joint).position))
                          << "  head " << v(in_placement(world(p, 42).position)) << "\n  board " << v(b.position) << "  board x "
                          << v(rotate(b.rotation, Vec3{1, 0, 0})) << " y " << v(rotate(b.rotation, Vec3{0, 1, 0})) << " z "
                          << v(rotate(b.rotation, Vec3{0, 0, 1})) << '\n';
                // Each foot in the board's own frame: along it, across it, above it.
                for (const std::uint16_t foot : {std::uint16_t{343}, std::uint16_t{10}}) {
                    const auto f = in_placement(world(p, foot).position);
                    const auto local = rotate(conjugate(b.rotation), Vec3{f[0] - b.position[0], f[1] - b.position[1], f[2] - b.position[2]});
                    std::cout << "  " << (foot == 343 ? "left" : "right") << " foot in board frame " << v(local) << '\n';
                }
            };
            show("as it is", pose, board);
            std::vector<Joint> mirrored(pose.size());
            mirror_pose_frame(pose, mirrored, true);
            show("mirrored (body in placement frame, board mirror_board_pose)", mirrored, mirror_board_pose(board));
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--native-catalog") {
            const std::filesystem::path folder(argv[2]);
            std::ifstream in(folder / "catalog.json", std::ios::binary);
            const auto root = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            check(root.is_array() && root.size() > 0, "Exported native catalog must be nonempty");
            for (const auto& entry : root) {
                const auto name = entry.value("file", "");
                std::ifstream binary(folder / name, std::ios::binary);
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(binary)), std::istreambuf_iterator<char>());
                const auto clip = clip_from_bytes(bytes);
                check(clip.joints == skeleton_joints && !clip.empty(), "Native library clip must load on the skater rig");
                if (entry.value("preview_only", false))
                    check(clip.takeoff == -1 && clip.landing == -1, "Preview-only grind clips must not invent airborne markers");
                for (const auto& joint : clip.frames) check(finite(joint), "Native catalog clip has a broken joint");
            }
            std::cout << "Native catalog compatibility: " << root.size() << " clips passed\n";
        }
        if (argc == 3 && std::string_view(argv[1]) == "--native-vector-coherence") {
            const std::filesystem::path folder(argv[2]);
            std::ifstream in(folder / "catalog.json");
            const auto root = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}));
            float maximum = 0;
            std::string worst;
            for (const auto& entry : root) {
                std::ifstream file(folder / entry.value("file", ""), std::ios::binary);
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
                const auto clip = clip_from_bytes(bytes);
                for (std::size_t f = 0; f < clip.times.size(); ++f) {
                    const auto pose = clip.frame(f);
                    for (const auto pair : {std::pair{343, 376}, std::pair{10, 377}, std::pair{343, 390}, std::pair{10, 391}}) {
                        const auto foot = world(pose, static_cast<std::uint16_t>(pair.first));
                        const auto target = world(pose, static_cast<std::uint16_t>(pair.second));
                        float square = 0;
                        for (std::size_t axis = 0; axis < 3; ++axis) square += std::pow(foot.position[axis]-target.position[axis], 2.0f);
                        const float distance = std::sqrt(square);
                        if (distance > maximum) { maximum = distance; worst = entry.value("file", ""); }
                    }
                }
            }
            std::cout << "Native FK/ankle-target maximum separation: " << maximum << " m in " << worst << '\n';
        }
        if (argc == 3 && std::string_view(argv[1]) == "--native-index") {
            std::ifstream in(argv[2], std::ios::binary);
            const auto root = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), dingosdk::JsonLimits{32 * 1024 * 1024, 48, 2 * 1024 * 1024});
            check(root.is_array() && root.size() > 0, "Native index must fit production JSON parser limits");
            for (const auto& entry : root)
                check((entry.value("asset", "").starts_with("animation/dingo/") || entry.value("asset", "").starts_with("_af/")) && !entry.value("category", "").empty(), "Indexed native asset requires category and stable source path");
            std::cout << "Native index compatibility: " << root.size() << " assets passed\n";
        }
        if (argc == 4 && std::string_view(argv[1]) == "--native-labels") {
            std::ifstream in(argv[2], std::ios::binary);
            const auto root = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}), dingosdk::JsonLimits{32 * 1024 * 1024, 48, 2 * 1024 * 1024});
            check(root.is_array() && root.size() > 0, "Readable-name validation requires the actual native index");
            auto report = dingosdk::Json::array();
            std::vector<NamedNativeFixture> names;
            for (const auto& entry : root) {
                const auto asset = entry.value("asset", ""), type = entry.value("root_type", "");
                const auto name = native_asset_name(asset, type, entry.value("category", ""));
                const auto folder = native_asset_folder(asset, type, entry.value("category", ""));
                check(!name.label.empty() && !folder.empty() && !folder.starts_with('/') && !folder.ends_with('/') && folder.find("//") == std::string::npos,
                      "Every native asset needs a readable label and a valid folder");
                check(native_search_matches(name.search, native_search_terms(asset)), "Exact original names remain searchable");
                names.push_back({asset, name.label, folder, name.context, name.search, entry.value("state", "")});
            }
            native_disambiguate_names(names);
            for (const auto& name : names) {
                dingosdk::Json row = dingosdk::Json::object();
                row["asset"] = name.asset; row["label"] = name.label; row["category"] = name.category;
                row["context"] = name.context; row["state"] = name.state;
                report.push_back(std::move(row));
            }
            std::ofstream out(argv[3], std::ios::binary); out << report.dump(2); out.close();
            check(out.good(), "Readable inventory report must be written successfully");
            std::cout << "Native readable names and folders: " << root.size() << " assets passed\n";
        }
        if (argc == 3 && std::string_view(argv[1]) == "--native-documents") {
            std::size_t count = 0;
            for (const auto& directory : {"native-documents", "native-resources"}) {
                for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(argv[2]) / directory)) {
                    if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                    std::ifstream in(entry.path(), std::ios::binary);
                    const auto document = dingosdk::Json::parse(std::string((std::istreambuf_iterator<char>(in)), {}), dingosdk::JsonLimits{16 * 1024 * 1024, 64, 2 * 1024 * 1024});
                    check(document.is_object() && document.contains("asset") && document.at("asset").is_string() &&
                          document.contains("root_type") && document.at("root_type").is_string(), "Native inspector documents must fit the production reader and retain source identity/type");
                    ++count;
                }
            }
            check(count > 0, "Native inspector requires actual documents");
            std::cout << "Native inspector compatibility: " << count << " documents passed\n";
        }
        if (argc == 2) {
            std::size_t files = 0;
            for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                std::ifstream json(entry.path(), std::ios::binary);
                const auto animation = from_json(std::string((std::istreambuf_iterator<char>(json)), std::istreambuf_iterator<char>()));
                auto path = entry.path(); path.replace_extension(".clip");
                std::ifstream binary(path, std::ios::binary);
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(binary)), std::istreambuf_iterator<char>());
                const auto clip = clip_from_bytes(bytes);
                for (const auto& t : animation.tracks) for (const auto& k : t.keys)
                    check(k.time <= clip.duration() + 0.01f, "Saved user key is outside the actual clip");
                std::vector<Joint> sampled(clip.joints);
                for (float time = 0; time <= clip.duration(); time += 1.0f / 60) {
                    clip.sample(time, sampled);
                    apply_edits(animation, time, sampled);
                    for (const auto& joint : sampled) check(finite(joint), "Saved user animation produces a nonfinite joint");
                    if (clip.joints == skeleton_joints) {
                        std::vector<Joint> source(clip.joints), native(clip.joints);
                        clip.sample(time, source);
                        for (auto& joint : native) joint.rotation = from_euler({11, -29, 31});
                        retarget_edits(animation, time, source, native, true);
                        for (const auto& joint : native) check(finite(joint), "Saved user edits produce finite retargeted switch joints");
                    }
                }
                ++files;
            }
            std::cout << "Saved animation compatibility: " << files << " files passed\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "anim_core: all passed\n";
    return 0;
}
