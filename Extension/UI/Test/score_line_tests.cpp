// A plugin's own line in skate.'s score HUD (Extension/UI/score_line.h), apart from the game: one
// plugin's line at a time on a 500 ms lease, the yield to skate.'s own line, the take-out that is
// never dropped, the game's root picked over ours and copied only while idle, a new line's rows, the
// HUD's row pass over a recorded list (what is made, written and retired, and in which order), and
// what a line writes.
#include "Extension/UI/score_line.h"
#include "Extension/UI/own_ui_writes.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dingosdk::score_line;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Line sample_line() {
    Line line;
    line.tricks = {"First action", "Second action"};
    line.points = 550;
    line.line = 4200;
    line.multiplier = 2.4f;
    return line;
}

void a_challenge_begin_never_waits_for_an_activity_read() {
    ActivityCheck state;
    unsigned reads{};
    bool active{};
    const auto read = [&] { ++reads; return active; };
    check(!state.running(false, 0, read) && reads == 1, "the initial activity state was not read");
    check(!state.running(false, 100, read) && reads == 1, "an idle activity read was not cached");
    check(state.running(true, 101, read) && reads == 1, "a challenge begin waited behind an idle activity cache");
    check(!state.running(false, 102, read) && reads == 1, "an ended challenge replaced the cached activity state");
    active = true;
    check(state.running(false, 250, read) && reads == 2, "an expired activity state was not read");
    active = false;
    check(state.running(true, 300, read) && reads == 2, "a challenge called the native activity getter");
    check(state.running(false, 400, read) && reads == 2, "an activity's cached refusal was lost");
    check(!state.running(false, 500, read) && reads == 3, "the next activity read did not clear its ended refusal");
}

void one_plugin_shows_its_line_at_a_time() {
    LineOwner owner;
    check(owner.ask("sample-line", sample_line(), 0).empty(), "the first plugin's line was refused");
    check(owner.ask("other", sample_line(), 100) == "sample-line shows its line there", "a second plugin was not told whose line is up");
    check(owner.owner() == "sample-line" && owner.current(100) && owner.current(100)->tricks.size() == 2, "the first plugin's line was not kept");
    check(owner.ask("other", std::nullopt, 150).empty() && owner.current(150), "another plugin's nil cleared the line");
    // Once the first lets its lease run out, the second may show its own.
    check(owner.ask("other", sample_line(), 700).empty() && owner.owner() == "other", "a line past its lease still held the HUD");
}

void a_line_lasts_500_ms_after_its_last_call() {
    LineOwner owner;
    owner.ask("sample-line", sample_line(), 1000);
    check(owner.current(1400) != nullptr, "the line went before its lease ran out");
    owner.ask("sample-line", sample_line(), 1400);
    check(owner.current(1900) != nullptr, "setting the line again did not renew its lease");
    check(owner.current(1901) == nullptr && owner.owner().empty(), "the line stayed 500 ms after its last call");
    check(owner.current(1950) == nullptr, "a let-go line came back");
}

void nil_and_release_clear_the_line() {
    LineOwner owner;
    owner.ask("sample-line", sample_line(), 0);
    check(owner.ask("sample-line", std::nullopt, 10).empty() && !owner.current(10), "nil did not clear the plugin's line");
    owner.ask("sample-line", sample_line(), 20);
    owner.release("other");
    check(owner.current(20) != nullptr, "another plugin's release took the line");
    owner.release("sample-line");
    check(!owner.current(20) && owner.owner().empty(), "stopping the plugin did not take its line");
}

void the_games_line_holds_a_plugins_down() {
    GameLine game;
    check(!game.running(0), "an idle game ran a line");
    game.observe({}, 0);
    check(!game.running(0), "idle values (Multiplier 0 at rest) ran a line");
    const GameSignals each[] = {{.action = true}, {.multiplier = 1.5f}, {.line_score = 22}, {.line_shown = 22}, {.item = true}};
    const char* names[] = {"ActionActive", "Multiplier 1.5", "LineScore 22", "LINE 22", "its score item"};
    double now = 1000;
    for (std::size_t i = 0; i < std::size(each); ++i, now += 10000) {
        game.observe(each[i], now);
        check(game.running(now) && game.why() == names[i], names[i]);
        game.observe({}, now + 16);
        check(game.running(now + 499), "the hold ended before 500 ms of idle");
        check(!game.running(now + 500), "the hold lasted past 500 ms of idle");
    }
    // Small values are idle: a multiplier, a score or a LINE number that rounds to nothing.
    GameLine small;
    small.observe({.multiplier = 0.005f, .line_score = 0.4f, .line_shown = 0.4f}, 0);
    check(!small.running(0), "values below the thresholds ran a line");
    // The game's item keeps it up while the values are idle (the LINE summary's last 0.3 s).
    GameLine item;
    for (double t = 0; t <= 300; t += 16) item.observe({.item = true}, t);
    check(item.running(700), "the game's item did not keep the line up");
}

void the_take_out_is_never_dropped() {
    ItemPresence presence;
    check(!presence.must_scan(), "nothing was in, yet a scan was asked for");
    presence.inserting();
    check(presence.must_scan(), "an insert did not ask for a scan");
    // A failed tick calls nothing: the scan is still due.
    check(presence.must_scan(), "a failed tick dropped the take-out");
    presence.scanned(true);
    check(presence.must_scan(), "a scan that found ours ended the take-out");
    presence.scanned(false);
    check(!presence.must_scan(), "a scan that found none of ours kept asking");
    presence.inserting();
    presence.registry_changed();
    check(!presence.must_scan(), "a new registry kept a scan of the old one's stack");
}

struct Model {
    std::uint64_t handle{};
};
struct Root {
    Model model;
};
void the_games_root_is_picked_over_ours() {
    const std::vector<Root> roots{{{0x177c10000}}, {{0x1000103270000}}};
    const auto* game = pick_game_root(roots, 0x177c10000);
    check(game && game->model.handle == 0x1000103270000, "our root was picked");
    check(pick_game_root(roots, 0)->model.handle == 0x177c10000, "with none of ours the first root was not picked");
    check(!pick_game_root(std::vector<Root>{{{0x177c10000}}}, 0x177c10000), "our root alone was taken for the game's");
}

void our_widget_writes_are_not_native_tricks() {
    dingosdk::ui::OwnUiEntries<3> own;
    check(!own.contains(0) && !own.contains(0x177c1), "an empty set claimed an entry");
    own.set({0x177c1, 0x100010327});
    check(own.contains(0x177c1) && own.contains(0x100010327), "an owned root or row was not excluded");
    check(!own.contains(0x200010327), "a different generation was excluded as ours");
    check(!own.contains(0) && !own.contains(0x456), "an empty slot or a native entry was excluded");
    own.set({0x456});
    check(own.contains(0x456) && !own.contains(0x177c1) && !own.contains(0x100010327), "replacing the set kept old ownership");
    own.set({1, 2, 3, 4});
    check(own.contains(3) && !own.contains(4), "the fixed capacity was not kept");
    own.set({});
    check(!own.contains(1) && !own.contains(2) && !own.contains(3), "releasing the models kept their entries");
}

void only_new_rows_come_in() {
    using Names = std::vector<std::string>;
    check(fresh_rows({}, {"First action"}) == 1, "a first row was not new");
    check(fresh_rows({"First action"}, {"Second action", "First action"}) == 1, "a trick added to the line renewed the older row");
    check(fresh_rows({"First action"}, {"First action", "First action"}) == 1, "a repeated trick was not one new row");
    check(fresh_rows({"C", "B", "A", "Z", "Y"}, {"D", "C", "B", "A", "Z"}) == 1, "the list at its cap did not keep the newest four");
    check(fresh_rows({"First action", "Second action"}, {"First action", "Second action"}) == 0, "the same rows were made again");
    check(fresh_rows({"First action"}, {"Third action", "Fourth action"}) == 2, "a new line kept an old row");
    check(fresh_rows({"First action"}, Names{}) == 0, "no rows were counted as new");
}

void row_identities_distinguish_equal_names() {
    const std::vector<std::string> same{"Repeated action", "Repeated action"};
    check(fresh_rows(same, same, {2, 1}, {2, 1}) == 0, "stable identities renewed rows");
    check(fresh_rows(same, same, {2, 1}, {3, 2}) == 1, "equal names hid a new identity");
    check(fresh_rows(same, same, {2, 1}, {4, 3}) == 2, "two new identities did not renew two rows");
    check(fresh_rows({"Old label"}, {"New label"}, {1}, {1}) == 1, "the identity hid a changed name");
    check(fresh_rows(same, same) == 0, "legacy name-only rows changed");
    check(fresh_rows(same, same, {2, 1}, {}) == 2, "omitting identities reused identified rows");
    check(fresh_rows(same, same, {}, {2, 1}) == 2, "new identities reused legacy rows");
    auto line = sample_line();
    line.trick_ids = {1, static_cast<std::uint64_t>(most_trick_id)};
    const auto look = look_of(line, 400);
    check(look.rows == std::vector<std::string>{"Second action", "First action"} &&
        look.trick_ids == std::vector<std::uint64_t>{static_cast<std::uint64_t>(most_trick_id), 1},
        "names and identities were not reversed together");
    line.trick_ids.clear();
    check(look_of(line, 400).trick_ids == std::vector<std::uint64_t>{0, 0}, "legacy identities were not absent");
}

// What the HUD's own row pass (update_rows) gives the game's list. A row here is only a number; the
// list records each write, which rows are alive, and whether one was retired while the list held it.
struct ListRow {
    std::string name;
    std::uint64_t trick_id{};
    unsigned serial{};
};
bool has(const std::vector<unsigned>& serials, unsigned serial) {
    return std::find(serials.begin(), serials.end(), serial) != serials.end();
}
struct FakeList {
    std::vector<ListRow> rows; // the HUD's cache, newest first
    std::uint64_t generation{};
    bool restart{};
    std::vector<unsigned> listed;              // the rows the list holds now
    std::vector<std::vector<unsigned>> writes; // each list write, in order
    std::vector<unsigned> alive, retired;
    std::size_t made{}, make_calls{};
    unsigned next_serial{};
    bool ready{true}, fail_write{};
    std::optional<std::size_t> fail_make_at; // the make call, counted from 0, that throws
    bool retired_while_listed{}, wrote_a_retired_row{};

    bool can_make() const { return ready; }
    ListRow make(const std::string& name, std::uint64_t trick_id) {
        const auto call = make_calls++;
        if (fail_make_at && *fail_make_at == call) throw std::runtime_error("a row was not made");
        ListRow row{name, trick_id, ++next_serial};
        alive.push_back(row.serial);
        ++made;
        return row;
    }
    void write(const std::vector<ListRow>& next) {
        if (fail_write) throw std::runtime_error("the list was not written");
        std::vector<unsigned> serials;
        for (const auto& row : next) {
            if (!has(alive, row.serial)) wrote_a_retired_row = true;
            serials.push_back(row.serial);
        }
        listed = serials;
        writes.push_back(std::move(serials));
    }
    void retire(ListRow& row) noexcept {
        if (has(listed, row.serial)) retired_while_listed = true;
        std::erase(alive, row.serial);
        retired.push_back(row.serial);
    }
    // As the HUD's clear_rows: the list emptied, then its rows destroyed.
    void clear() {
        listed.clear();
        for (auto& row : rows) retire(row);
        rows.clear();
        generation = 0;
    }
    RowsUpdate update(const Look& look, std::uint64_t owner_generation) {
        return update_rows(rows, generation, restart, look.rows, look.trick_ids, owner_generation, *this);
    }
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& row : rows) out.push_back(row.name);
        return out;
    }
    std::vector<std::uint64_t> ids() const {
        std::vector<std::uint64_t> out;
        for (const auto& row : rows) out.push_back(row.trick_id);
        return out;
    }
};

// A constructed UI drain reads only the latest request and runs the HUD's own row pass over a
// FakeList. An unavailable registry calls nothing, leaving the last successful row cache and its
// owner generation intact, as the HUD does.
struct RowDrain {
    FakeList list;
    std::vector<std::string> names;
    std::vector<std::uint64_t> ids;
    std::uint64_t& generation = list.generation;
    bool& restart = list.restart;
    std::size_t& made = list.made;
    LineStart start;
    std::size_t drain(LineOwner& owner, double now, bool available = true, bool row_update_reached = true) {
        if (!available) return 0;
        const auto* line = owner.current(now);
        if (!line) {
            start.out();
            return 0;
        }
        if (start.shown(line->closed)) {
            // A line that starts again empties the HUD's list (clear_rows), whatever its rows carry.
            restart = true;
            list.clear();
        }
        const auto before = list.made;
        if (row_update_reached) list.update(look_of(*line, 400), owner.generation());
        names = list.names();
        ids = list.ids();
        return list.made - before;
    }
};

void append_identified_row(Line& line, std::uint64_t id) {
    line.tricks.push_back("Repeated action");
    line.trick_ids.push_back(id);
    if (line.tricks.size() > most_tricks) {
        line.tricks.erase(line.tricks.begin());
        line.trick_ids.erase(line.trick_ids.begin());
    }
}

void seven_equal_names_each_make_one_row() {
    LineOwner owner;
    RowDrain shown;
    Line line;
    for (std::uint64_t id = 1; id <= 7; ++id) {
        append_identified_row(line, id);
        const auto now = static_cast<double>(id) * 16;
        check(owner.ask("sample-line", line, now).empty(), "an identified row was refused");
        check(shown.drain(owner, now) == 1, "an equal name did not make exactly one new row");
        check(shown.drain(owner, now + 1) == 0, "an unchanged snapshot duplicated its row");
    }
    check(shown.made == 7 && shown.ids == std::vector<std::uint64_t>{7, 6, 5, 4, 3},
        "the latest five identities did not retain seven distinct additions");
}

void missed_ui_drains_keep_each_retained_identity() {
    LineOwner owner;
    RowDrain shown;
    Line line;
    for (std::uint64_t id = 1; id <= 5; ++id) {
        append_identified_row(line, id);
        owner.ask("sample-line", line, static_cast<double>(id) * 16);
        check(shown.drain(owner, static_cast<double>(id) * 16) == 1, "initial rows differ");
    }
    const auto before = shown.ids;
    append_identified_row(line, 6);
    owner.ask("sample-line", line, 96);
    check(shown.drain(owner, 96, false) == 0 && shown.ids == before && shown.made == 5,
        "an unavailable registry consumed a pending snapshot");
    append_identified_row(line, 7);
    owner.ask("sample-line", line, 112);
    check(shown.drain(owner, 112, false) == 0 && shown.ids == before, "another failed drain changed its cache");
    // No four-row trim request is needed: the latest five still contain both unseen identities. Both
    // come in at once, into a list that was full, so it is written one row short of full.
    check(shown.drain(owner, 128) == 2 && shown.made == 7 && shown.ids == std::vector<std::uint64_t>{7, 6, 5, 4},
        "coalesced equal names lost or duplicated retained additions");
    check(shown.drain(owner, 144) == 0 && shown.made == 7, "idle recovery duplicated rows");
}

void owner_and_closed_boundaries_survive_a_missed_drain() {
    LineOwner owner;
    RowDrain shown;
    Line line;
    append_identified_row(line, 1);
    owner.ask("sample-line", line, 0);
    check(shown.drain(owner, 0) == 1, "initial owner row differs");
    const auto first_generation = owner.generation();
    // A reload releases its owner, then a new instance can start its own row IDs at 1 before the UI runs.
    owner.release("sample-line");
    owner.ask("sample-line", line, 16);
    check(owner.generation() != first_generation && shown.drain(owner, 16) == 1,
        "a restarted owner reused the old instance's cached identity");
    owner.ask("sample-line", std::nullopt, 32);
    owner.ask("sample-line", line, 32);
    check(shown.drain(owner, 32) == 1, "an unseen clear lost the next line's first row");
    line.closed = true;
    owner.ask("sample-line", line, 48);
    line.closed = false;
    owner.ask("sample-line", line, 64);
    check(shown.drain(owner, 64) == 1, "an unseen closed line lost the next line's first row");
    const auto previous_generation = owner.generation();
    check(!owner.ask("other", line, 65).empty() && owner.generation() == previous_generation,
        "a refused owner changed the row generation");
    owner.ask("other", line, 600);
    check(shown.drain(owner, 600) == 1 && shown.made == 5, "an expired owner kept the next owner's row identity");
}

void a_deferred_row_update_keeps_its_pending_generation() {
    LineOwner owner;
    RowDrain shown;
    Line line;
    append_identified_row(line, 1);
    owner.ask("sample-line", line, 0);
    shown.drain(owner, 0);
    const auto old_generation = shown.generation;
    owner.release("sample-line");
    owner.ask("sample-line", line, 16);
    // The outer UI publication can stop before the row update. This is not a thrown row-array write:
    // that separate native safety path disables names until the registry is reset.
    check(shown.drain(owner, 16, true, false) == 0 && shown.generation == old_generation && shown.made == 1,
        "a deferred row update acknowledged the restarted owner");
    check(shown.drain(owner, 32) == 1 && shown.generation == owner.generation(), "a later update lost the pending owner row");
    // A visible line restarts after it yielded; no row update has consumed this restart yet.
    shown.start.out();
    check(shown.drain(owner, 48, true, false) == 0 && shown.restart, "a deferred update consumed a visible-line restart");
    check(shown.drain(owner, 64) == 1 && !shown.restart && shown.made == 3, "recovery reused a row from before the restart");
}

// A caller that sends names alone keeps the rows that still match by name when no UI drain saw its
// lease lapse, its clear, its release or its closed line, as it did before rows had identities. The
// same boundaries start every row again for a caller that sends identities, and a boundary the drain
// did see starts the rows again for both.
void name_only_rows_survive_unseen_owner_boundaries() {
    using Boundary = void (*)(LineOwner&, Line&);
    const Boundary boundaries[] = {
        [](LineOwner& owner, Line& line) { owner.ask("sample-line", line, 600); }, // past its 500 ms lease
        [](LineOwner& owner, Line& line) {
            owner.ask("sample-line", std::nullopt, 16);
            owner.ask("sample-line", line, 16);
        },
        [](LineOwner& owner, Line& line) {
            owner.release("sample-line");
            owner.ask("sample-line", line, 16);
        },
        [](LineOwner& owner, Line& line) {
            line.closed = true;
            owner.ask("sample-line", line, 16);
            line.closed = false;
            owner.ask("sample-line", line, 32);
        },
    };
    for (const auto boundary : boundaries) {
        for (const bool identified : {false, true}) {
            LineOwner owner;
            auto line = sample_line();
            if (identified) line.trick_ids = {1, 2};
            check(owner.ask("sample-line", line, 0).empty(), "the first line was refused");
            const auto before = look_of(line, 400);
            const auto generation = owner.generation();
            boundary(owner, line); // no drain and no current() between: the UI saw none of it
            const auto after = look_of(line, 400);
            check(owner.generation() != generation, "an owner boundary did not start a new generation");
            const auto fresh = fresh_rows(before.rows, after.rows, before.trick_ids, after.trick_ids, generation, owner.generation());
            check(fresh == (identified ? 2u : 0u), identified ? "identified rows were kept across an owner boundary"
                                                             : "rows sent by name alone were remade across an unseen owner boundary");
        }
    }
    // One more name on a name-only line after an unseen lapse is one new row, not three.
    check(fresh_rows({"Second action", "First action"}, {"Third action", "Second action", "First action"}, {}, {}, 1, 2) == 1,
        "a name added across an unseen owner boundary renewed the older rows");
    // A drain that saw the line go starts its rows again, with or without identities.
    for (const bool identified : {false, true}) {
        LineOwner owner;
        RowDrain shown;
        auto line = sample_line();
        if (identified) line.trick_ids = {1, 2};
        owner.ask("sample-line", line, 0);
        check(shown.drain(owner, 0) == 2, "the first rows differ");
        check(shown.drain(owner, 600) == 0, "a lapsed line made rows");
        owner.ask("sample-line", line, 616);
        check(shown.drain(owner, 616) == 2 && shown.made == 4, "a lapse the drain saw kept the old line's rows");
        check(shown.drain(owner, 632) == 0 && shown.made == 4, "an unchanged line after a restart made rows again");
    }
}

// The sixth and seventh of seven equal names, where the list is full. skate.'s widget does not draw
// a list of five replaced by another of five, so the list is never written at the count it has: the
// seven writes hold 1, 2, 3, 4, 5, 4 and 5 rows, as the launch that kept the names on screen wrote
// them. Each write is the whole list, the new row first and then the rows it keeps, which are the
// rows they were. A row that fell off is retired only after the write that dropped it.
void a_full_list_changes_its_count_with_each_new_row() {
    LineOwner owner;
    FakeList list;
    Line line;
    const std::size_t counts[] = {1, 2, 3, 4, 5, 4, 5};
    for (unsigned id = 1; id <= 7; ++id) {
        append_identified_row(line, id);
        const auto now = static_cast<double>(id) * 16;
        check(owner.ask("sample-line", line, now).empty(), "an identified row was refused");
        const auto before = list.listed;
        check(list.update(look_of(*owner.current(now), 400), owner.generation()) == RowsUpdate::written, "a new row did not write the list");
        const auto count = counts[id - 1];
        check(list.writes.size() == id && list.listed.size() == count, "a new row did not write the list once, at the count it should have");
        check(list.listed.size() != before.size(), "a list was replaced at the count it already had");
        check(list.listed.front() == id, "the new row is not first in the list");
        for (std::size_t index = 1; index < count; ++index)
            check(list.listed[index] == before[index - 1], "a row the list keeps was remade or moved");
        auto alive = list.alive, listed = list.listed;
        std::sort(alive.begin(), alive.end());
        std::sort(listed.begin(), listed.end());
        check(alive == listed, "the rows alive are not the rows the list holds");
        check(!list.retired_while_listed && !list.wrote_a_retired_row, "a row was retired before the write that dropped it");
        // The same line again, with the list full or one short of it, is left as it is.
        check(list.update(look_of(*owner.current(now + 1), 400), owner.generation()) == RowsUpdate::unchanged && list.writes.size() == id,
            "an unchanged line wrote the list again");
    }
    check(list.retired == std::vector<unsigned>{2, 1}, "the rows retired are not the two that fell off");
    check(list.made == 7 && list.ids() == std::vector<std::uint64_t>{7, 6, 5, 4, 3}, "seven equal names did not leave the newest five rows");
}

// Names alone fill the list the same way: the sixth different name is written as four rows.
void a_full_list_of_names_alone_changes_its_count_too() {
    FakeList list;
    Look look;
    look.rows = {"E", "D", "C", "B", "A"};
    check(list.update(look, 1) == RowsUpdate::written && list.listed.size() == 5, "five names did not fill the list");
    look.rows = {"F", "E", "D", "C", "B"};
    check(list.update(look, 1) == RowsUpdate::written && list.names() == std::vector<std::string>{"F", "E", "D", "C"} && list.made == 6,
        "a sixth name was written at the count the list already had");
    // A lease that lapsed unseen changes the generation and nothing else for names alone.
    check(list.update(look, 2) == RowsUpdate::unchanged && list.writes.size() == 2 && list.generation == 2, "the short full list was written again");
    look.rows = {"G", "F", "E", "D", "C"};
    check(list.update(look, 2) == RowsUpdate::written && list.names() == look.rows && list.made == 7 && !list.retired_while_listed,
        "the seventh name did not bring the list back to five rows");
}

// A list write that fails leaves the list, the rows and their cache as they were: only the row made
// for that write goes, and the next pass makes it again.
void a_failed_list_write_keeps_the_rows_it_had() {
    LineOwner owner;
    FakeList list;
    Line line;
    for (unsigned id = 1; id <= 5; ++id) {
        append_identified_row(line, id);
        owner.ask("sample-line", line, id * 16.0);
        list.update(look_of(*owner.current(id * 16.0), 400), owner.generation());
    }
    const auto listed = list.listed;
    const auto generation = list.generation;
    append_identified_row(line, 6);
    owner.ask("sample-line", line, 96);
    const auto look = look_of(*owner.current(96), 400);
    list.fail_write = true;
    bool threw{};
    try {
        list.update(look, owner.generation());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "a failed list write was not passed on");
    check(list.listed == listed && list.ids() == std::vector<std::uint64_t>{5, 4, 3, 2, 1} && list.generation == generation,
        "a failed list write changed the rows or their cache");
    check(list.retired == std::vector<unsigned>{6} && list.alive.size() == 5, "a failed list write did not retire exactly the row made for it");
    list.fail_write = false;
    check(list.update(look, owner.generation()) == RowsUpdate::written && list.ids() == std::vector<std::uint64_t>{6, 5, 4, 3},
        "the pass after a failed write did not bring the list up to date");
    check(list.retired == std::vector<unsigned>{6, 2, 1} && !list.retired_while_listed, "the pass after a failed write retired the wrong rows");
}

// A row that cannot be made stops the pass: the rows it had made go, and nothing is written.
void a_row_that_cannot_be_made_writes_nothing() {
    FakeList list;
    Look look;
    look.rows = {"Second action", "First action"};
    look.trick_ids = {2, 1};
    list.fail_make_at = 1;
    bool threw{};
    try {
        list.update(look, 1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw && list.writes.empty() && list.rows.empty(), "a pass that could not make a row wrote the list");
    check(list.alive.empty() && list.retired == std::vector<unsigned>{1}, "a row made before the one that failed was left alive");
    list.fail_make_at.reset();
    check(list.update(look, 1) == RowsUpdate::written && list.listed.size() == 2, "the rows were not made once they could be");
}

// Until the game has made a row of its own, none of ours can be made: the pass waits without a write
// and without advancing its cache, so the rows come in once they can. A list that only loses its
// oldest row needs no new one, and is written while none can be made.
void rows_wait_until_one_can_be_made() {
    FakeList list;
    list.ready = false;
    Look look;
    look.rows = {"Second action", "First action"};
    check(list.update(look, 3) == RowsUpdate::waiting && list.writes.empty() && !list.made && !list.generation, "a pass that cannot make a row did not wait");
    list.ready = true;
    check(list.update(look, 3) == RowsUpdate::written && list.generation == 3 && list.names() == look.rows, "the waiting rows never came in");
    list.ready = false;
    look.rows = {"Second action"};
    check(list.update(look, 3) == RowsUpdate::written && list.listed == std::vector<unsigned>{1} && list.retired == std::vector<unsigned>{2},
        "a list that lost its oldest row was not written without it");
}

// Names alone are not written again across an owner generation; the same names with identities are.
void unchanged_names_are_not_written_again() {
    FakeList list;
    Look look;
    look.rows = {"Second action", "First action"};
    look.trick_ids = {0, 0};
    list.update(look, 1);
    check(list.update(look, 2) == RowsUpdate::unchanged && list.writes.size() == 1 && list.made == 2 && list.generation == 2,
        "rows sent by name alone were written again across an owner generation");
    look.trick_ids = {2, 1};
    check(list.update(look, 3) == RowsUpdate::written && list.made == 4 && list.retired.size() == 2 && !list.retired_while_listed,
        "identified rows did not start again under a new owner generation");
}

// Ours is a copy of the game's root, so it is made only while nothing of the game's line is in it
// (after a level load GameLine has seen nothing, so this is what holds the first copy back).
void the_games_root_is_copied_only_while_idle() {
    check(root_busy({}).empty(), "an idle root was taken for a busy one");
    check(root_busy({.hud_active = true}) == "HudWidgetActive", "HudWidgetActive on did not hold the copy");
    check(!root_busy({.line_timer = 0.0001f}).empty(), "time left on the game's line did not hold the copy");
    check(!root_busy({.trick_rows = 1}).empty() && !root_busy({.second_rows = 2}).empty(), "rows in the game's lists did not hold the copy");
    check(!root_busy({.item = true}).empty(), "the game's score item in the stack did not hold the copy");
    check(root_busy({.hud_active = true, .trick_rows = 3}) == "HudWidgetActive, rows in its trick list", "each thing on was not named");
}

// A line that opens after none, or after a closed one, starts its rows from an empty list.
void a_new_line_starts_its_rows_again() {
    LineStart start;
    check(start.shown(false), "the first line did not start its rows");
    check(!start.shown(false) && !start.shown(false), "a line going on started its rows again");
    check(!start.shown(true), "closing a line started its rows");
    check(!start.shown(true), "a closed line held started its rows");
    check(start.shown(false), "a line opening after a closed one kept the old line's rows");
    start.out();
    check(start.shown(false), "a line after the last was taken out kept its rows");
    start.out();
    check(!start.shown(true), "a closed line after none started rows it does not show");
    check(start.shown(false), "and the line opening after it did not start them");
}

void a_line_looks_like_the_games() {
    auto line = sample_line();
    auto look = look_of(line, 400);
    check(look.hud_active && look.ring_on && look.line_on, "a running line did not show the ring and LINE");
    check(look.points == "550" && look.multiplier_text == "2.4" && look.line_score == 4200, "a line's numbers differ");
    check(look.fill == 1.f, "the ring's fill was not capped at full");
    check(look.rows == std::vector<std::string>{"Second action", "First action"}, "the rows are not newest first");
    line.points = 100;
    line.multiplier = 2;
    check(look_of(line, 400).fill == 0.25f && look_of(line, 400).multiplier_text == "2", "the fill or a whole multiplier differs");
    check(multiplier_text(1.5f) == "1.5" && multiplier_text(1.04f) == "1" && multiplier_text(99) == "99", "the ring's number differs");
    line.line = 0;
    check(!look_of(line, 400).line_on, "LINE showed for a line with nothing banked");
    line.line = 937;
    line.closed = true;
    look = look_of(line, 400);
    check(!look.hud_active && !look.ring_on && !look.line_on && look.line_score == 937, "a closed line is not the game's LINE summary");
    line.tricks = {"1", "2", "3", "4", "5", "6", "7"};
    check(look_of(line, 400).rows == std::vector<std::string>{"7", "6", "5", "4", "3"}, "more rows than the list keeps");
}
} // namespace

int main() {
    try {
        a_challenge_begin_never_waits_for_an_activity_read();
        one_plugin_shows_its_line_at_a_time();
        a_line_lasts_500_ms_after_its_last_call();
        nil_and_release_clear_the_line();
        the_games_line_holds_a_plugins_down();
        the_take_out_is_never_dropped();
        the_games_root_is_picked_over_ours();
        our_widget_writes_are_not_native_tricks();
        only_new_rows_come_in();
        row_identities_distinguish_equal_names();
        seven_equal_names_each_make_one_row();
        missed_ui_drains_keep_each_retained_identity();
        owner_and_closed_boundaries_survive_a_missed_drain();
        a_deferred_row_update_keeps_its_pending_generation();
        name_only_rows_survive_unseen_owner_boundaries();
        a_full_list_changes_its_count_with_each_new_row();
        a_full_list_of_names_alone_changes_its_count_too();
        a_failed_list_write_keeps_the_rows_it_had();
        a_row_that_cannot_be_made_writes_nothing();
        rows_wait_until_one_can_be_made();
        unchanged_names_are_not_written_again();
        the_games_root_is_copied_only_while_idle();
        a_new_line_starts_its_rows_again();
        a_line_looks_like_the_games();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "score line tests passed\n";
    return 0;
}
