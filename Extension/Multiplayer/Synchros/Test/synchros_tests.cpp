#include "Extension/Multiplayer/Synchros/sharing_rules.h"
#include "Extension/Multiplayer/Synchros/share_code.h"
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
} // namespace

int main() {
    try {
        codes_look_right_and_read_back();
        reading_is_forgiving_but_checked();
        proofs_bind_the_pair_and_session();
        both_ends_opt_in();
        a_code_pairs_a_stranger();
        blocking_wins();
    } catch (const std::exception& e) {
        std::cerr << "synchros: " << e.what() << '\n';
        return 1;
    }
    std::cout << "synchros: all passed\n";
    return 0;
}
