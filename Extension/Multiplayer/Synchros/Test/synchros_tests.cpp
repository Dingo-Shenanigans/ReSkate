#include "Extension/Multiplayer/Synchros/sharing_rules.h"
#include "Extension/Multiplayer/Synchros/share_code.h"
#include "Extension/Multiplayer/Synchros/synchros_wire.h"
#include "Engine/Resource/bundle_ref_table.h"
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace dingosdk::multiplayer::synchros;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

constexpr std::uint64_t session = 0x1122334455667788ULL, me = 76561198000000001ULL, them = 76561198000000002ULL,
                        other = 76561198000000003ULL;

void codes_look_right_and_read_back() {
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const auto shown = make_code();
        check(shown.size() == 18 && shown.starts_with("RSK-") && shown[8] == '-' && shown[13] == '-', "RSK-XXXX-XXXX-XXXX");
        const auto symbols = read_code(shown);
        check(symbols && show_code(*symbols) == shown, "a code reads back as itself");
        seen.insert(shown);
    }
    check(seen.size() == 200, "codes do not repeat");
}

void reading_is_forgiving_but_checked() {
    const auto shown = make_code();
    const auto symbols = *read_code(shown);
    std::string typed;
    for (const char c : symbols) typed += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    check(read_code(typed) == symbols, "lower case, no dashes, no prefix");
    check(read_code("rsk " + typed.substr(0, 4) + " " + typed.substr(4)) == symbols, "spaces and a lower-case prefix");
    // One changed character, anywhere, is caught.
    for (std::size_t at = 0; at < symbols.size(); ++at) {
        auto wrong = symbols;
        wrong[at] = wrong[at] == '7' ? '8' : '7';
        check(!read_code(wrong), "a typo is not a code");
    }
    // Two neighbours swapped, where they differ, is caught.
    for (std::size_t at = 0; at + 1 < symbols.size(); ++at) {
        if (symbols[at] == symbols[at + 1]) continue;
        auto wrong = symbols;
        std::swap(wrong[at], wrong[at + 1]);
        check(!read_code(wrong), "swapped characters are not a code");
    }
    check(!read_code(symbols.substr(0, 11)), "too short");
    check(!read_code(symbols + "0"), "too long");
    check(!read_code("RSK-U000-0000-0000"), "U is not in the alphabet");
    // O for 0 and I or L for 1.
    std::string zeros = "00000000000";
    std::string with_check = zeros + "0";
    check(read_code(with_check).has_value(), "all zeros is a code (its check is 0)");
    check(read_code("OOOO-OOOO-OOOO") == with_check, "O reads as 0");
}

void proofs_bind_the_pair_and_session() {
    const auto symbols = *read_code(make_code());
    const auto key = code_key(symbols);
    const auto proof = code_proof(key, session, me, them);
    check(proof_matches(key, proof, session, me, them), "the right proof matches");
    check(!proof_matches(key, proof, session + 1, me, them), "not in another session");
    check(!proof_matches(key, proof, session, me, other), "not for another viewer");
    check(!proof_matches(key, proof, session, other, them), "not for another wearer");
    check(!proof_matches(code_key(*read_code(make_code())), proof, session, me, them), "not under another code");
}

void both_ends_opt_in() {
    Settings wearer, viewer;
    const Peer friend_peer{them, true}, stranger{other, false};
    check(!may_send(wearer, friend_peer), "sharing is off until turned on");
    check(!may_ask(viewer, friend_peer), "showing is off until turned on");
    wearer.share = true;
    viewer.show = true;
    check(may_send(wearer, friend_peer), "a friend gets it");
    check(!may_send(wearer, stranger), "a stranger does not");
    check(may_ask(viewer, friend_peer), "a friend is asked");
    check(!may_ask(viewer, stranger), "a stranger is not asked without a code");
    wearer.share_with_friends = false;
    check(!may_send(wearer, friend_peer), "friends can be turned off");
}

void a_code_pairs_a_stranger() {
    Settings wearer, viewer;
    wearer.share = viewer.show = true;
    const auto shown = renew_code(wearer);
    check(enter_code(viewer, shown).empty(), "the code is entered");
    check(!enter_code(viewer, shown).empty(), "not twice");
    check(!enter_code(wearer, shown).empty(), "not your own");
    check(!enter_code(viewer, "RSK-0000-0000-0001").empty(), "a typo is refused");
    check(may_ask(viewer, {me, false}), "the viewer asks the wearer it has a code for");
    const auto proofs = proofs_for(viewer, session, me, them);
    check(!may_send(wearer, {them, false}), "not before the proof");
    check(accept_proof(wearer, proofs, session, me, them), "the proof is accepted");
    check(may_send(wearer, {them, false}), "paired");
    // A proof made for someone else's request is not accepted for this one.
    Settings other_wearer = wearer;
    other_wearer.paired.clear();
    check(!accept_proof(other_wearer, proofs_for(viewer, session, me, other), session, me, them), "a proof for another pair fails");
    // A new code keeps those already paired, but the old code pairs nobody new.
    renew_code(wearer);
    check(may_send(wearer, {them, false}), "still paired after a new code");
    Settings latecomer;
    latecomer.show = true;
    check(enter_code(latecomer, shown).empty(), "the old code can still be typed");
    check(!accept_proof(wearer, proofs_for(latecomer, session, me, other), session, me, other), "but it pairs nobody new");
}

void blocking_wins() {
    Settings wearer;
    wearer.share = true;
    renew_code(wearer);
    wearer.paired.insert(them);
    block(wearer, them);
    check(!may_send(wearer, {them, true}), "a blocked friend gets nothing");
    check(!wearer.paired.contains(them), "and is unpaired");
    Settings viewer;
    viewer.show = true;
    block(viewer, me);
    check(!may_ask(viewer, {me, true}), "a blocked player is not asked");
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
    Hello hello{session, {code_proof(code_key(*read_code(make_code())), session, me, them)}};
    const auto h = round_trip(hello);
    check(h.session == session && h.proofs == hello.proofs, "hello");
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
    check(!decode(encode(Message{Hello{0, {}}})), "a hello without a session");
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

int main() {
    try {
        codes_look_right_and_read_back();
        reading_is_forgiving_but_checked();
        proofs_bind_the_pair_and_session();
        both_ends_opt_in();
        a_code_pairs_a_stranger();
        blocking_wins();
        every_message_round_trips();
        malformed_messages_are_refused();
        a_message_never_exceeds_steams_limit();
        received_presets_go_into_the_table();
    } catch (const std::exception& e) {
        std::cerr << "synchros: " << e.what() << '\n';
        return 1;
    }
    std::cout << "synchros: all passed\n";
    return 0;
}
