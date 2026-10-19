#include "Extension/Throwdowns/native_event_delivery.h"
#include <stdexcept>

namespace {
using namespace dingosdk::multiplayer;
constexpr std::uint32_t submit = 0xe85319b1, end_turn = 0xb2fc396c;
constexpr std::uint32_t local = 0x4, opponent = 0x301;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
auto submission = [](const auto& sent) { return sent.event == submit; };
}

void native_event_delivery_checks() {
    // Reproduce 19 Oct 20:04:34.954 -> 20:04:37.536 with the former bookkeeping:
    // original held submit still reserved sender 0, then injected local submit.
    NativeEventSendQueue old;
    old.note(submit, 0, 0);
    old.note(submit, local, 1);
    check(old.take(submission)->sender == 0, "Old held-submit failure did not reproduce");
    old.note(submit, opponent, 2569);
    check(old.first(submission)->sender == local, "Old quick handoff did not reproduce stale local attribution");

    NativeEventSendQueue fixed;
    // Use the same delivery decision and queue implementation as the runtime.
    // The observer still receives the original event; it records no sender slot.
    fixed.note(submit, 0, 0, !hold_native_skate_send(submit, true, false));
    fixed.note(submit, local, 1, !hold_native_skate_send(submit, true, true));
    check(fixed.first(submission)->sender == local, "Held original stole injected local sender");
    check(fixed.take(submission)->sender == local && !fixed.has_injected(), "Local handler left a phantom sender");
    fixed.note(submit, opponent, 2569, !hold_native_skate_send(submit, true, true));
    check(fixed.first(submission)->sender == opponent, "Opponent inherited local sender during quick handoff");
    check(fixed.take(submission)->sender == opponent && !fixed.first(submission), "Opponent handler left a phantom sender");

    // Many fast handoffs; no five-second expiry may hide a leaked entry.
    for (unsigned turn = 0; turn < 50; ++turn) {
        const auto sender = turn % 2 ? opponent : local;
        fixed.note(submit, 0, 2600 + turn, !hold_native_skate_send(submit, true, false));
        fixed.note(end_turn, 0, 2600 + turn, !hold_native_skate_send(end_turn, true, false));
        fixed.note(submit, sender, 2600 + turn, !hold_native_skate_send(submit, true, true));
        check(fixed.first(submission)->sender == sender, "Repeated fast turn attribution diverged");
        check(fixed.take(submission)->sender == sender && !fixed.has_injected(), "Repeated turn leaked sender");
    }
    check(!fixed.first([](const auto&) { return true; }), "Suppressed turn-end reserved a handler slot");

    // Preserve stock native sends and unrelated modes/events.
    check(!hold_native_skate_send(submit, false, false) &&
          !hold_native_skate_send(end_turn, false, false) &&
          !hold_native_skate_send(0x7fac81bb, true, false), "Non-detector native sends were held");
    fixed.note(submit, 0, 3000, !hold_native_skate_send(submit, false, false));
    fixed.note(submit, opponent, 3001);
    check(fixed.take(submission)->sender == 0 && fixed.take(submission)->sender == opponent,
          "Delivered stock sends lost native FIFO ordering");
    fixed.note(submit, local, 4000);
    fixed.note(submit, opponent, 9001);
    check(fixed.take(submission)->sender == opponent && !fixed.has_injected(), "Native five-second expiry changed");
    fixed.note(submit, local, 10000);
    check(!fixed.take([](const auto& sent) { return sent.event == end_turn; }), "Unrelated graph consumed submission");
    check(fixed.take(submission)->sender == local, "Unrelated graph changed submission sender");
}
