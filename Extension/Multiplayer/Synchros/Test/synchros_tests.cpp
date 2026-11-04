#include "Extension/Multiplayer/Synchros/sharing_rules.h"
#include "Extension/Multiplayer/Synchros/share_code.h"
#include "Extension/Multiplayer/Synchros/synchros_wire.h"
#include "Extension/Multiplayer/Synchros/received.h"
#include "Engine/Resource/bundle_ref_table.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace dingosdk::multiplayer::synchros;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

constexpr std::uint64_t session = 0x1122334455667788ULL, me = 76561198000000001ULL, them = 76561198000000002ULL,
                        other = 76561198000000003ULL;

constexpr std::uint64_t group_member = 76561198000000004ULL;

void codes_look_right_and_read_back() {
    std::set<std::string> seen;
    for (const auto kind : {CodeKind::personal, CodeKind::group}) {
        for (int i = 0; i < 200; ++i) {
            const auto shown = make_code(kind);
            check(shown.size() == 18 && shown.starts_with(prefix_of(kind)) && shown[3] == '-' && shown[8] == '-' && shown[13] == '-',
                  "RSK-XXXX-XXXX-XXXX or RSG-...");
            const auto symbols = read_code(shown, kind);
            check(symbols && show_code(*symbols, kind) == shown, "a code reads back as itself");
            check(typed_kind(shown) == kind, "its prefix says its kind");
            seen.insert(shown);
        }
    }
    check(seen.size() == 400, "codes do not repeat");
}

void reading_is_forgiving_but_checked() {
    const auto shown = make_code(CodeKind::personal);
    const auto symbols = *read_code(shown, CodeKind::personal);
    std::string typed;
    for (const char c : symbols) typed += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    check(read_code(typed, CodeKind::personal) == symbols, "lower case, no dashes, no prefix");
    check(read_code("rsk " + typed.substr(0, 4) + " " + typed.substr(4), CodeKind::personal) == symbols, "spaces and a lower-case prefix");
    check(!read_code("RSG-" + symbols, CodeKind::personal), "a group prefix is not a personal code");
    check(read_code("RSG-" + symbols, CodeKind::group) == symbols, "the same characters read as a group code with its prefix");
    for (std::size_t at = 0; at < symbols.size(); ++at) {
        auto wrong = symbols;
        wrong[at] = wrong[at] == '7' ? '8' : '7';
        check(!read_code(wrong, CodeKind::personal), "a typo is not a code");
    }
    for (std::size_t at = 0; at + 1 < symbols.size(); ++at) {
        if (symbols[at] == symbols[at + 1]) continue;
        auto wrong = symbols;
        std::swap(wrong[at], wrong[at + 1]);
        check(!read_code(wrong, CodeKind::personal), "swapped characters are not a code");
    }
    check(!read_code(symbols.substr(0, 11), CodeKind::personal), "too short");
    check(!read_code(symbols + "0", CodeKind::personal), "too long");
    check(!read_code("RSK-U000-0000-0000", CodeKind::personal), "U is not in the alphabet");
    check(read_code("OOOO-OOOO-OOOO", CodeKind::personal) == std::string(12, '0'), "O reads as 0");
}

void proofs_bind_the_pair_session_and_kind() {
    const auto symbols = *read_code(make_code(CodeKind::personal), CodeKind::personal);
    const auto key = code_key(symbols, CodeKind::personal);
    const auto proof = code_proof(key, session, me, them);
    check(proof_matches(key, proof, session, me, them), "the right proof matches");
    check(!proof_matches(key, proof, session + 1, me, them), "not in another session");
    check(!proof_matches(key, proof, session, me, other), "not for another viewer");
    check(!proof_matches(key, proof, session, other, them), "not for another wearer");
    check(!proof_matches(code_key(symbols, CodeKind::group), proof, session, me, them), "a group key from the same characters differs");
}

void both_ends_opt_in_and_codes_are_the_only_way_in() {
    Settings wearer, viewer;
    SessionGrants grants;
    check(!may_send(wearer, grants, them), "sharing is off until turned on");
    wearer.share = true;
    check(!may_send(wearer, grants, them), "no code, no outfit, friend or not");
    viewer.show = true;
    check(!may_ask(viewer, me), "a viewer with no codes asks nobody");
    const auto shown = renew_code(wearer);
    check(enter_code(viewer, shown).empty(), "the code is entered");
    check(may_ask(viewer, me), "now it asks");
    viewer.show = false;
    check(!may_ask(viewer, me), "not with showing off");
}

void a_personal_code_pairs_one_way() {
    Settings wearer, viewer;
    SessionGrants grants;
    wearer.share = viewer.show = true;
    const auto shown = renew_code(wearer);
    check(enter_code(viewer, shown).empty(), "the code is entered");
    check(!enter_code(viewer, shown).empty(), "not twice");
    check(!enter_code(wearer, shown).empty(), "not your own");
    check(!enter_code(viewer, "RSK-0000-0000-0001").empty(), "a typo is refused");
    check(!enter_code(viewer, make_code(CodeKind::group)).empty(), "a group code is pointed at groups");
    check(accept_proofs(wearer, grants, proofs_for(viewer, session, me, them), session, me, them), "the proof is accepted");
    check(may_send(wearer, grants, them), "paired");
    // One way: the wearer has no code of the viewer's, so it is not let in to see the viewer.
    Settings viewer_as_wearer = viewer;
    viewer_as_wearer.share = true;
    renew_code(viewer_as_wearer);
    SessionGrants viewer_grants;
    check(!accept_proofs(viewer_as_wearer, viewer_grants, proofs_for(wearer, session, them, me), session, them, me),
          "giving your code does not get you theirs");
    // A new code keeps those already paired, but the old code pairs nobody new.
    renew_code(wearer);
    check(may_send(wearer, grants, them), "still paired after a new code");
    Settings latecomer;
    latecomer.show = true;
    check(enter_code(latecomer, shown).empty(), "the old code can still be typed");
    check(!accept_proofs(wearer, grants, proofs_for(latecomer, session, me, other), session, me, other), "but it pairs nobody new");
}

void a_group_lets_everyone_in_it_see_each_other() {
    Settings a, b, c, outsider;
    for (auto* p : {&a, &b, &c, &outsider}) p->share = p->show = true;
    const auto code = create_group(a, "Sk8 Crew");
    check(a.groups.size() == 1 && a.groups[0].created && a.groups[0].name == "Sk8 Crew", "the group is made");
    check(join_group(b, code, "Crew").empty() && join_group(c, code, "").empty(), "two join it");
    check(!join_group(b, code, "again").empty(), "not twice");
    check(!join_group(b, make_code(CodeKind::personal), "").empty(), "a personal code is pointed at codes");
    check(!b.groups[0].created && !renew_group_code(b, 0).size(), "only the PC that made it offers a new group code");
    // Every pair of members, both ways.
    const std::uint64_t ids[3] = {me, them, group_member};
    Settings* members[3] = {&a, &b, &c};
    for (int w = 0; w < 3; ++w)
        for (int v = 0; v < 3; ++v) {
            if (w == v) continue;
            SessionGrants grants;
            check(may_ask(*members[v], ids[w]), "a member asks the others");
            check(accept_proofs(*members[w], grants, proofs_for(*members[v], session, ids[w], ids[v]), session, ids[w], ids[v]),
                  "a member's proof is accepted by every other member");
            check(may_send(*members[w], grants, ids[v]), "and gets the outfit");
            check(!members[w]->paired.contains(ids[v]), "a group lets in for the session, it does not pair");
        }
    SessionGrants grants;
    check(!accept_proofs(a, grants, proofs_for(outsider, session, me, other), session, me, other), "someone outside the group gets nothing");
    // A new group code: whoever keeps only the old one no longer matches the members who moved.
    const auto renewed = renew_group_code(a, 0);
    check(!renewed.empty(), "the maker makes a new group code");
    leave_group(b, 0);
    check(join_group(b, renewed, "Crew").empty(), "b moves to the new code");
    SessionGrants fresh;
    check(accept_proofs(a, fresh, proofs_for(b, session, me, them), session, me, them), "b still matches a");
    check(!accept_proofs(a, fresh, proofs_for(c, session, me, group_member), session, me, group_member), "c, on the old code, does not");
    // Grants are for one session.
    check(!may_send(a, SessionGrants{session + 1, {}}, them), "a group grant does not carry into another session");
    // Leaving.
    leave_group(a, 0);
    SessionGrants after;
    check(!accept_proofs(a, after, proofs_for(b, session, me, them), session, me, them), "after leaving, nobody gets in by it");
}

void blocking_wins() {
    Settings wearer, viewer;
    SessionGrants grants;
    wearer.share = viewer.show = true;
    const auto code = create_group(wearer, "g");
    join_group(viewer, code, "g");
    check(accept_proofs(wearer, grants, proofs_for(viewer, session, me, them), session, me, them), "in the same group");
    block(wearer, grants, them);
    check(!may_send(wearer, grants, them), "a blocked group member gets nothing");
    check(!accept_proofs(wearer, grants, proofs_for(viewer, session, me, them), session, me, them), "and cannot prove their way back");
    block(viewer, grants, me);
    check(!may_ask(viewer, me), "a blocked player is not asked");
}

File sample_file(std::uint8_t seed, FileKind kind = FileKind::ebx) {
    File f;
    f.id.fill(seed);
    f.size = 1000u + seed;
    f.kind = kind;
    f.name = kind == FileKind::ebx || kind == FileKind::resource ? "characters/customization/own_x_ap" : "";
    f.bundle = kind == FileKind::toc_chunk ? "" : "win32/characters/skateboard/deck_x_ap_cas_main_bundlereftable";
    f.superbundle = "win32/items";
    f.original_size = 5000;
    f.resource_type = 0x6bde20ba;
    f.resource_id = 0x1122334455667788ULL;
    f.resource_meta = {1, 2, 3};
    f.chunk_guid.fill(static_cast<std::uint8_t>(seed + 1));
    f.logical_offset = 4;
    f.logical_size = 8;
    f.sha1.fill(static_cast<std::uint8_t>(seed + 2));
    if (kind == FileKind::chunk) f.chunk_record = {0x82, 0x01, 0x00};
    return f;
}

template<class T> T round_trip(const T& message) {
    const auto bytes = encode(Message{message});
    const auto back = decode(bytes);
    check(back.has_value(), "a message decodes");
    const auto* typed = std::get_if<T>(&*back);
    check(typed != nullptr, "as the kind it was");
    return *typed;
}

void every_message_round_trips() {
    Settings holder;
    enter_code(holder, make_code(CodeKind::personal));
    create_group(holder, "g");
    const auto proofs = proofs_for(holder, session, me, them);
    Hello hello{session, proofs.personal, proofs.group};
    const auto h = round_trip(hello);
    check(h.session == session && h.personal == hello.personal && h.group == hello.group && h.group.size() == 1, "hello");
    check(round_trip(Welcome{true}).allowed && !round_trip(Welcome{false}).allowed, "welcome");
    check(round_trip(Want{{"items/board_bottomart/own_x", "items/cust_tops/own_y"}}).items.size() == 2, "want");
    Contents contents{{ItemContents{"items/board_bottomart/own_x", Withheld::none, {sample_file(1), sample_file(2, FileKind::resource),
                                                                                 sample_file(3, FileKind::chunk), sample_file(4, FileKind::toc_chunk)},
                                    {TableRow{"characters/customization/configs/cas_main_bundlereftable", "characters/x/own_x_ap", 812}}},
                       ItemContents{"items/cust_tops/own_store_copy", Withheld::store_item, {}, {}}}};
    const auto c = round_trip(contents);
    check(c.items.size() == 2 && c.items[0].files == contents.items[0].files && c.items[0].rows == contents.items[0].rows && c.items[1].withheld == Withheld::store_item, "contents");
    Fetch fetch{{sample_file(1).id, sample_file(2).id}};
    check(round_trip(fetch).files == fetch.files, "fetch");
    Piece piece{sample_file(1).id, 0, 3, {7, 8, 9}};
    const auto p = round_trip(piece);
    check(p.bytes == piece.bytes && p.total == 3, "piece");
    check(round_trip(Refuse{"Sharing is off."}).reason == "Sharing is off.", "refuse");
}

void malformed_messages_are_refused() {
    auto bytes = encode(Message{Want{{"items/x"}}});
    check(!decode(std::span(bytes).first(bytes.size() - 1)), "truncated");
    auto longer = bytes; longer.push_back(0);
    check(!decode(longer), "trailing bytes");
    auto version = bytes; version[1] = 2;
    check(!decode(version), "another version");
    auto kind = bytes; kind[0] = 99;
    check(!decode(kind), "an unknown kind");
    check(!decode(encode(Message{Want{}})), "a want of nothing");
    check(!decode(encode(Message{Hello{0, {}, {}}})), "a hello without a session");
    // A name with a control character is not a name the game has.
    check(!decode(encode(Message{Want{{std::string("items/x\n")}}})), "control characters");
    // Too many items.
    Want many;
    for (std::size_t i = 0; i <= max_items; ++i) many.items.push_back("items/x" + std::to_string(i));
    check(!decode(encode(Message{many})), "over max_items");
    // A withheld item that still carries files.
    Contents liar{{ItemContents{"items/x", Withheld::store_item, {sample_file(1)}, {}}}};
    check(!decode(encode(Message{liar})), "withheld with files");
    Contents rows_liar{{ItemContents{"items/x", Withheld::store_item, {}, {TableRow{"t_bundlereftable", "p_ap", 1}}}}};
    check(!decode(encode(Message{rows_liar})), "withheld with table rows");
    // A file that says it is bigger than a file may be, or empty.
    auto big = sample_file(1); big.size = max_file + 1;
    check(!decode(encode(Message{Contents{{ItemContents{"items/x", Withheld::none, {big}, {}}}}})), "a file over max_file");
    auto empty = sample_file(1); empty.size = 0;
    check(!decode(encode(Message{Contents{{ItemContents{"items/x", Withheld::none, {empty}, {}}}}})), "an empty file");
    // A bundled asset without its bundle.
    auto loose = sample_file(1); loose.bundle.clear();
    check(!decode(encode(Message{Contents{{ItemContents{"items/x", Withheld::none, {loose}, {}}}}})), "an asset with no bundle");
    // Only a chunk has a chunk-metadata record.
    auto recorded = sample_file(1); recorded.chunk_record = {1, 2};
    check(!decode(encode(Message{Contents{{ItemContents{"items/x", Withheld::none, {recorded}, {}}}}})), "a record on an ebx");
    auto long_record = sample_file(1, FileKind::chunk); long_record.chunk_record.assign(max_chunk_record + 1, 1);
    check(!decode(encode(Message{Contents{{ItemContents{"items/x", Withheld::none, {long_record}, {}}}}})), "a record over max_chunk_record");
    // A whole outfit over max_outfit.
    Contents huge;
    ItemContents item{"items/x", Withheld::none, {}, {}};
    for (int i = 0; i < 3; ++i) { auto f = sample_file(static_cast<std::uint8_t>(i)); f.size = max_file; item.files.push_back(f); }
    huge.items.push_back(item);
    check(!decode(encode(Message{huge})), "over max_outfit");
    // Pieces must lie inside their file.
    check(!decode(encode(Message{Piece{sample_file(1).id, 2, 3, {1, 2}}})), "a piece past the end");
    check(!decode(encode(Message{Piece{sample_file(1).id, 4, 3, {1}}})), "an offset past the end");
    check(!decode(encode(Message{Piece{sample_file(1).id, 0, 3, {}}})), "an empty piece");
    std::vector<std::uint8_t> too_much(max_piece + 1, 1);
    check(!decode(encode(Message{Piece{sample_file(1).id, 0, max_file, too_much}})), "a piece over max_piece");
}

void a_message_never_exceeds_steams_limit() {
    Contents full;
    for (std::size_t i = 0; i < max_items; ++i) {
        ItemContents item{"items/board_bottomart/own_" + std::to_string(i), Withheld::none, {}, {}};
        for (int k = 0; k < 100; ++k) item.files.push_back(sample_file(static_cast<std::uint8_t>(k)));
        full.items.push_back(item);
    }
    bool threw = false;
    try { (void)encode(Message{full}); } catch (const std::length_error&) { threw = true; }
    check(threw, "a Contents over 512 KB is refused at encode, so the sender splits it");
}

// The smallest table the game's layout allows (bundle_ref_table.cpp): two bundles (index 0 the
// sentinel), one preset of the base's own, a pool padded so the table is 256 bytes or more.
struct TableBytes { std::vector<std::byte> data, meta; };
constexpr std::string_view base_preset = "base/existing_ap";
TableBytes empty_table() {
    constexpr std::size_t pool = 128, bundles = 224, lookups = 240, payload = 256;
    std::vector<std::byte> d(payload + 20);
    auto p32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(d.data() + at, &v, 4); };
    auto p64 = [&](std::size_t at, std::uint64_t v) { std::memcpy(d.data() + at, &v, 8); };
    p64(0, pool); p64(8, lookups); p64(16, pool); p64(24, bundles); p64(32, pool);
    p32(72, 1); p32(76, 2); p32(92, 1);
    // Path descriptors, inline: parent none, then the text. A bundle "b", and the base's preset.
    p32(pool, 0xffffffffu);
    d[pool + 4] = std::byte{'b'};
    const std::uint32_t bundle_token = 0u | 0x800000u | (1u << 24);
    p32(pool + 16, 0xffffffffu);
    std::memcpy(d.data() + pool + 20, base_preset.data(), base_preset.size());
    const std::uint32_t preset_token = 16u | 0x800000u | (static_cast<std::uint32_t>(base_preset.size()) << 24);
    p32(bundles, 0xffffffffu); p32(bundles + 4, 0);  // the sentinel
    p32(bundles + 8, bundle_token); p32(bundles + 12, 0);
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : base_preset) h = h * 0x100000001B3ULL ^ static_cast<unsigned char>(c);
    p64(lookups, h); p32(lookups + 8, 1); p32(lookups + 12, preset_token);
    for (std::uint32_t i = 0; i < 5; ++i) p32(payload + i * 4, i * 8);
    std::vector<std::byte> meta(16);
    std::uint32_t size = payload, trailer = 20;
    std::memcpy(meta.data(), &size, 4);
    std::memcpy(meta.data() + 4, &trailer, 4);
    return {d, meta};
}

void received_presets_go_into_the_table() {
    namespace br = dingosdk::frostbite::bundle_ref;
    const auto base = empty_table();
    const br::Table table{base.data, base.meta};
    check(br::presets(table).size() == 1 && br::presets(table).at(std::string(base_preset)) == 1, "the base table reads, with its one preset");
    const std::vector<br::Preset> rows{{"characters/customization/reskate/textureedits/abc_ap", 1},
                                       {"characters/customization/own_nabi/nabi_ap", 1}};
    const auto merged = br::with_presets(table, rows);
    check(merged.added == 2 && !merged.conflicts, "both presets go in");
    const br::Table after{merged.resource, merged.resourceMeta};
    const auto held = br::presets(after);
    check(held.size() == 3 && held.at(rows[0].path) == 1 && held.at(rows[1].path) == 1, "and read back where they were put");
    const auto again = br::with_presets(after, rows);
    check(again.added == 0 && !again.conflicts && again.resource == merged.resource, "the same presets again change nothing");
    const std::vector<br::Preset> moved{{rows[0].path, 0}};
    check(br::with_presets(after, moved).conflicts == 1, "a preset the table has in another bundle is a conflict, not a move");
    bool threw = false;
    try { (void)br::with_presets(table, std::vector<br::Preset>{{"x/y_ap", 9}}); } catch (const std::exception&) { threw = true; }
    check(threw, "a bundle index past the list is refused");
    // What merge makes of a mod's copy is the same as adding its presets.
    const br::Table copy{merged.resource, merged.resourceMeta};
    const auto via_merge = br::merge(table, std::span(&copy, 1));
    check(via_merge.resource == merged.resource, "with_presets and merge agree");
}
} // namespace

std::vector<std::uint8_t> made_bytes(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::uint8_t>(seed + i * 7);
    return out;
}

File file_of(const std::vector<std::uint8_t>& bytes) {
    File f = sample_file(1);
    f.id = content_id(bytes);
    f.size = static_cast<std::uint32_t>(bytes.size());
    return f;
}

std::vector<Piece> pieces_of(const std::vector<std::uint8_t>& bytes) {
    std::vector<Piece> out;
    for (std::size_t at = 0; at < bytes.size(); at += max_piece) {
        const auto end = std::min(bytes.size(), at + max_piece);
        out.push_back({content_id(bytes), static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(bytes.size()),
                       {bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(end)}});
    }
    return out;
}

void sha1_is_sha1() {
    const std::string abc = "abc";
    const auto id = content_id({reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()});
    check(hex(id) == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of abc");
}

void a_file_arrives_in_order_and_whole() {
    const auto bytes = made_bytes(max_piece * 2 + 100, 3);
    const auto pieces = pieces_of(bytes);
    {
        Arriving arriving(file_of(bytes));
        check(arriving.take(pieces[0]) == Arriving::Took::more, "first piece");
        check(arriving.take(pieces[1]) == Arriving::Took::more, "second piece");
        check(arriving.take(pieces[2]) == Arriving::Took::whole, "last piece");
        check(arriving.release() == bytes, "the bytes sent");
    }
    {
        Arriving arriving(file_of(bytes));
        check(arriving.take(pieces[1]) == Arriving::Took::refused, "out of order");
    }
    {
        Arriving arriving(file_of(bytes));
        (void)arriving.take(pieces[0]);
        check(arriving.take(pieces[0]) == Arriving::Took::refused, "the same piece twice");
    }
    {
        auto bad = pieces;
        bad[2].bytes[0] ^= 1;
        Arriving arriving(file_of(bytes));
        (void)arriving.take(bad[0]);
        (void)arriving.take(bad[1]);
        check(arriving.take(bad[2]) == Arriving::Took::refused, "bytes that do not hash to the id");
    }
    {
        auto short_first = pieces[0];
        short_first.bytes.resize(100);
        Arriving arriving(file_of(bytes));
        check(arriving.take(short_first) == Arriving::Took::refused, "only the last piece is short");
    }
    {
        auto other_total = pieces[0];
        other_total.total += 1;
        Arriving arriving(file_of(bytes));
        check(arriving.take(other_total) == Arriving::Took::refused, "another total");
    }
}

void the_cache_keeps_files_by_content_and_wearer() {
    const auto root = std::filesystem::temp_directory_path() / "synchros-cache-test";
    std::filesystem::remove_all(root);
    Cache cache(root, 1u << 20);
    const auto a = made_bytes(300000, 1), b = made_bytes(300000, 2), c = made_bytes(300000, 3);
    check(!cache.put(content_id(a), b), "bytes must hash to their id");
    check(cache.put(content_id(a), a) && cache.put(content_id(b), b) && cache.put(content_id(c), c), "kept");
    check(cache.has(content_id(a)) && cache.read(content_id(b)) == b, "read back");

    ItemContents worn{"own_x", Withheld::none, {file_of(a), file_of(b)}, {}};
    ItemContents theirs{"own_y", Withheld::none, {file_of(b), file_of(c)}, {}};
    cache.remember(them, {worn});
    cache.remember(other, {theirs});
    const auto back = cache.listing(them);
    check(back.size() == 1 && back[0].item == "own_x" && back[0].files.size() == 2 && back[0].files[0].id == content_id(a),
          "a wearer's listing reads back");
    check(cache.wearers() == std::vector<std::uint64_t>{them, other}, "both wearers");
    check(cache.missing(them).empty(), "nothing missing");

    cache.forget(them);
    check(!cache.has(content_id(a)), "forgetting a wearer drops their files");
    check(cache.has(content_id(b)) && cache.has(content_id(c)), "but not files another wearer's items need");
    check(cache.missing(other).empty(), "the other wearer still has everything");

    const auto d = made_bytes(600000, 4);
    check(cache.put(content_id(d), d), "kept");
    check(cache.size() > cache.limit(), "over the limit");
    cache.trim({other});
    check(cache.has(content_id(b)) && cache.has(content_id(c)), "trimming keeps what players in the session wear");
    check(!cache.has(content_id(d)) && cache.size() <= cache.limit(), "and drops the rest to fit");

    cache.empty();
    check(cache.size() == 0 && cache.wearers().empty(), "emptied");
    std::filesystem::remove_all(root);
}

int main() {
    try {
        codes_look_right_and_read_back();
        reading_is_forgiving_but_checked();
        proofs_bind_the_pair_session_and_kind();
        both_ends_opt_in_and_codes_are_the_only_way_in();
        a_personal_code_pairs_one_way();
        a_group_lets_everyone_in_it_see_each_other();
        blocking_wins();
        every_message_round_trips();
        malformed_messages_are_refused();
        a_message_never_exceeds_steams_limit();
        received_presets_go_into_the_table();
        sha1_is_sha1();
        a_file_arrives_in_order_and_whole();
        the_cache_keeps_files_by_content_and_wearer();
    } catch (const std::exception& e) {
        std::cerr << "synchros: " << e.what() << '\n';
        return 1;
    }
    std::cout << "synchros: all passed\n";
    return 0;
}
