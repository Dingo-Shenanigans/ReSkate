#include "Extension/Input/native_input_capture.h"
#include "Engine/Game/Input/input_lease_capture_policy.h"
#include "Extension/Input/input_publication.h"
#include "Extension/Input/pad_output_publication.h"
#include "Extension/Input/input_source_environment.h"
#include "Engine/Game/Input/plugin_input_policy.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
namespace source = dingosdk::native_input_source;
namespace lease = dingosdk::input_lease;
namespace capture = dingosdk::input_lease_capture;
namespace {
source::environment::Facts facts{1000, 1, true, true, true, true};
unsigned checks{}, writes{};
bool trusted_capture_active{};
lease::Pad offered_pad{};
enum class Write { full, refused, partial } write_mode{Write::full};
struct Buffer { std::uint32_t packet{}; lease::Pad pad{}; std::uint32_t tail{0xabcddcba}; };
void check(bool value, const char* why) {
    ++checks; if (!value) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
bool publish(void* output, std::uint32_t packet, const lease::Pad& pad) noexcept {
    check(source::publication_held(), "actual provider holds gate at the bounded write");
    ++writes; offered_pad = pad; auto& buffer = *static_cast<Buffer*>(output);
    if (write_mode == Write::refused) return false;
    buffer.packet = packet;
    if (write_mode == Write::partial) return false;
    buffer.pad = pad;
    return true;
}
Buffer finish(source::Poll& poll, lease::Pad raw = {}, lease::Pad returned = {},
    std::uint32_t result = 0, bool raw_known = true, bool returned_known = true,
    std::uint32_t selected_result = 0, std::uint32_t original_packet = 7,
    std::uint32_t returned_packet = 7, std::uint64_t revision = 0) {
    Buffer buffer{returned_packet, returned};
    ++facts.now_ms;
    const auto selected = source::finish_capture(poll, result, raw_known, original_packet, raw,
        selected_result, returned_known, returned_packet, returned,
        revision ? revision : source::publication_revision(), &buffer, &publish);
    check(selected == selected_result, "finalizer never invents transport success");
    check(buffer.tail == 0xabcddcba, "publication touches only bounded report destination");
    check(!source::publication_held(), "actual finalizer releases publication gate");
    return buffer;
}
Buffer report(lease::Pad raw = {}, lease::Pad returned = {}, std::uint32_t result = 0,
    bool known = true, std::uint32_t returned_packet = 7) {
    source::Poll poll; source::begin(true, 0, poll);
    return finish(poll, raw, returned, result, known, known, result, 7, returned_packet);
}
lease::Snapshot sample(lease::Handle handle, std::uint64_t owner = 1) {
    lease::Reason reason; lease::Snapshot snapshot;
    check(source::lease_read(owner, handle, snapshot, reason), "actual broker issues snapshot");
    return snapshot;
}
lease::Handle lease_only() {
    source::tick(1, 2, true, {true, false}); report();
    lease::Handle handle; lease::Reason reason;
    check(source::lease_request(1, 0, handle, reason), "passive owner admitted");
    report(); (void)sample(handle); return handle;
}
lease::Handle arm() {
    const auto handle = lease_only(); const auto snapshot = sample(handle);
    capture::Reason reason;
    check(source::capture_request(1, handle, snapshot, reason) && reason == capture::Reason::none,
        "capture needs actual issued accepted source");
    return handle;
}
capture::Receipt state(lease::Handle handle) {
    capture::Receipt receipt;
    check(source::capture_state(1, handle, receipt), "actual capture receipt present");
    check(receipt.key.owner == 1 && receipt.key.lease_sequence == handle.sequence && receipt.key.user == 0,
        "receipt retains exact owner and lease identity");
    return receipt;
}
lease::Handle start() {
    const auto handle = arm();
    check(state(handle).phase == capture::Phase::waiting_neutral, "request does not capture immediately");
    const auto before = writes; const auto buffer = report();
    const auto receipt = state(handle);
    check(receipt.phase == capture::Phase::capturing && receipt.debt && receipt.publications == 1 &&
        !receipt.neutral_releases && writes == before + 1 && buffer.pad == lease::Pad{},
        "only a real full report write starts capture and records debt");
    return handle;
}
void close(lease::Handle handle) {
    if (state(handle).phase != capture::Phase::complete) {
        check(source::capture_release(1, handle), "owner requests release once"); report();
    }
    check(source::capture_acknowledge(1, handle), "capture ACK after real release/drain");
    lease::Reason reason; lease::Receipt passive;
    check(source::lease_state(1, handle, passive, reason), "passive receipt remains separate");
    if (!lease::terminal(passive.status)) check(source::lease_release(1, handle, reason), "end separate passive reservation");
    check(source::lease_acknowledge(1, handle, reason), "passive ACK after capture reservation cleared");
}
void denial() {
    const auto handle = lease_only(); const auto snapshot = sample(handle);
    capture::Reason reason; capture::Receipt receipt;
    check(!source::capture_request(2, handle, snapshot, reason), "foreign owner cannot adopt issued stream");
    auto forged = snapshot; ++forged.observation_serial;
    check(!source::capture_request(1, handle, forged, reason), "forged scalar snapshot refused");
    forged = snapshot; forged.pad.buttons = 1;
    check(!source::capture_request(1, handle, forged, reason), "forged raw pad refused");
    facts.thread = 2;
    check(!source::capture_request(1, handle, snapshot, reason), "wrong native client thread refused");
    facts.thread = 1;
    source::Poll predating; source::begin(true, 0, predating);
    check(!source::capture_request(1, handle, snapshot, reason) && reason == capture::Reason::busy,
        "capture cannot ignore an already invoking original");
    (void)finish(predating);
    const auto fresh = sample(handle);
    check(source::capture_request(1, handle, fresh, reason), "capture can use new exact issued observation after drain");
    check(!source::capture_state(2, handle, receipt) && !source::capture_release(2, handle) &&
        !source::capture_acknowledge(2, handle), "all cleanup remains exact owner scoped");
    close(handle);
}
void cold_original() {
    source::Poll cold; source::begin(true, 0, cold); // before verified client tick
    const auto handle = lease_only(); const auto snapshot = sample(handle); capture::Reason reason;
    check(!source::capture_request(1, handle, snapshot, reason) && reason == capture::Reason::busy,
        "cold original is included in capture admission drain");
    (void)finish(cold);
    check(source::capture_request(1, handle, snapshot, reason), "drained cold original cannot seed but no longer blocks");
    close(handle);
}
void publication() {
    const auto handle = arm(); lease::Pad pressed{}; pressed.buttons = 0x1000;
    const auto before = writes; auto buffer = report(pressed, pressed);
    check(buffer.pad == pressed && writes == before && state(handle).phase == capture::Phase::waiting_neutral,
        "waiting does not consume a nonneutral source");
    report(); buffer = report(pressed, pressed);
    const auto issued = sample(handle);
    check(buffer.pad == lease::Pad{} && issued.pad == pressed && state(handle).debt,
        "actual captured game buffer differs from unmodified issued raw observation");
    const auto publications = state(handle).publications;
    source::Poll other; source::begin(true, 1, other);
    const auto unselected = finish(other, pressed, pressed);
    check(unselected.pad == pressed && state(handle).publications == publications,
        "a different XInput user is never suppressed or counted as this capture");
    close(handle);
}
void late_drain() {
    const auto handle = start(); check(source::capture_release(1, handle), "explicit release accepted");
    source::Poll older, newer; source::begin(true, 0, older); source::begin(true, 0, newer);
    (void)finish(newer);
    auto receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && receipt.polls_in_flight == 1 &&
        !receipt.neutral_releases,
        "newer publication cannot hand back while an older callback can still write");
    check(!source::capture_acknowledge(1, handle), "ACK refuses retained duty before exact callback drain");
    lease::Reason reason;
    check(!source::lease_acknowledge(1, handle, reason), "passive ACK also respects captured callback");
    lease::Pad pressed{}; pressed.buttons = 0x8000; const auto buffer = finish(older, pressed, pressed);
    receipt = state(handle);
    check(buffer.pad == lease::Pad{} && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.neutral_releases && !receipt.polls_in_flight,
        "old completion maintains retained duty but cannot supply a fresh handback witness");
    report(); check(state(handle).neutral_releases == 1, "new later original alone supplies confirmed raw handback");
    close(handle);
}
void interference() {
    const auto handle = start(); const auto identity = state(handle).key;
    source::Poll retained; source::begin(true, 0, retained);
    source::PublishedAtomic<bool> writer{false}; writer.store(true);
    facts.menu_closed = false; facts.interference_clear = false;
    const auto menu_packet = std::uint32_t{7} ^ 0x80000000u;
    (void)report({}, {}, 0, true, menu_packet);
    (void)finish(retained, {}, {}, 0, true, true, 0, 7, menu_packet);
    facts.now_ms += 1100; source::tick(1, 2, true, {true, false});
    (void)report({}, {}, 0, true, menu_packet);
    auto receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.neutral_releases && !receipt.polls_in_flight && !receipt.callbacks_unknown,
        "menu interference never fabricates completion; exact earlier callback has actually drained");
    lease::Reason reason; lease::Snapshot no_authority;
    check(!source::lease_read(1, handle, no_authority, reason), "menu revoked passive acquisition authority");
    writer.store(false); facts.menu_closed = true; facts.interference_clear = true;
    const auto before = writes; (void)report(); receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::complete && !receipt.debt &&
        receipt.neutral_releases == 1 && !receipt.polls_in_flight && !receipt.callbacks_unknown && writes == before + 1,
        "same selected stream plus ONE real clean neutral write and known drain complete retained menu cleanup");
    check(!source::lease_read(1, handle, no_authority, reason), "cleanup never reacquired passive input authority");
    close(handle);
}
void replacement() {
    const auto handle = start(); const auto identity = state(handle).key;
    check(source::capture_release(1, handle), "release before stream replacement");
    (void)report({}, {}, 1167, false);
    (void)report(); // reconnect creates another observed stream epoch
    auto receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::releasing && receipt.debt &&
        receipt.quarantined && !receipt.neutral_releases,
        "replacement stream neutral cannot prove release for old selected epoch");
    facts.now_ms += capture::Capture::maximum_release_ms;
    receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.quarantined && receipt.debt &&
        !receipt.neutral_releases && !source::capture_acknowledge(1, handle),
        "1s replacement boundary retains unknown old-stream duty and cannot authorize ACK");
    lease::Reason reason; lease::Receipt passive;
    check(source::lease_state(1, handle, passive, reason) && passive.capture_reserved && !passive.source_released &&
        !source::lease_acknowledge(1, handle, reason), "replacement cannot bypass the retained capture through passive ACK");
}
void context_release() {
    const auto handle = start(); const auto identity = state(handle).key;
    source::tick(1, 2, false);
    lease::Pad physical{}; physical.buttons = 0x1001; physical.left_x = 17000;
    const auto before = writes; const auto buffer = report(physical, physical);
    auto receipt = state(handle);
    check(buffer.pad == physical && writes == before && receipt.key == identity &&
        receipt.phase == capture::Phase::releasing && receipt.debt && !receipt.neutral_releases,
        "loading does not withhold physical controls or erase retained cleanup duty");
    lease::Snapshot no_authority; lease::Reason reason;
    check(!source::lease_read(1, handle, no_authority, reason), "context cleanup does not authorize a new input sample");
    source::tick(1, 2, true, {true, false});
    (void)report(); receipt = state(handle);
    check(receipt.phase == capture::Phase::complete && !receipt.debt && receipt.neutral_releases == 1,
        "later known-closed gameplay can confirm same-stream cleanup without restoring old authority");
    close(handle);
}
void replacement_publication() {
    dingosdk::input::PadOutput replacement{}; replacement.buttons = 0x1000;
    auto before = source::publication_revision();
    check(source::write_pad_output("replacement", 0, replacement, facts.now_ms).empty(),
        "actual replacement adapter admits its owner");
    check(source::publication_revision() > before && dingosdk::input::pad_outputs().active(facts.now_ms),
        "replacement publication advances the shared revision and vetoes passive availability");
    before = source::publication_revision();
    check(source::read_pad_output(0, facts.now_ms, true).has_value() && source::publication_revision() == before,
        "a nonmutating replacement read does not invalidate every raw poll");
    check(!source::write_pad_output("other", 0, replacement, facts.now_ms).empty() &&
        source::publication_revision() == before, "a refused owner cannot publish an interference transition");
    check(source::write_pad_output("other", 0, std::nullopt, facts.now_ms).empty() &&
        source::publication_revision() == before, "a foreign nil write does not release another replacement");
    check(source::write_pad_output("replacement", 0, std::nullopt, facts.now_ms).empty() &&
        source::publication_revision() > before && !dingosdk::input::pad_outputs().active(facts.now_ms),
        "explicit nil release advances the same publication fence");

    check(source::write_pad_output("replacement", 4, replacement, facts.now_ms).empty(),
        "the native-pad slot uses the same adapter");
    before = source::publication_revision();
    source::release_pad_output("other");
    check(source::publication_revision() == before, "foreign owner cleanup is not a replacement mutation");
    source::release_pad_output("replacement");
    check(source::publication_revision() > before && !dingosdk::input::pad_outputs().active(facts.now_ms),
        "owner close fences native-pad replacement release too");

    check(source::write_pad_output("replacement", 0, replacement, facts.now_ms).empty(), "expiry fixture publishes");
    before = source::publication_revision(); facts.now_ms += dingosdk::input::PadOutputs::lifetime_ms;
    check(!source::read_pad_output(0, facts.now_ms, true) && source::publication_revision() > before,
        "read-time expiry is a fenced mutation, not an invisible clear");
    check(source::write_pad_output("replacement", 0, replacement, facts.now_ms).empty() &&
        source::write_pad_output("replacement", 4, replacement, facts.now_ms).empty(), "menu-clear fixture publishes both transports");
    before = source::publication_revision();
    check(!source::read_pad_output(0, facts.now_ms, false) && source::publication_revision() > before &&
        !dingosdk::input::pad_outputs().active(facts.now_ms), "disallowed input clears every replacement behind the publication fence");

    check(source::write_pad_output("replacement", 0, replacement, facts.now_ms).empty(), "active-veto fixture publishes");
    source::tick(1, 2, true, {true, false}); report();
    lease::Handle denied; lease::Reason reason;
    check(!source::lease_request(1, 0, denied, reason) && reason == lease::Reason::interference,
        "an active replacement vetoes passive admission even when the staged report equals raw");
    source::release_pad_output("replacement");

    const auto handle = start(); const auto identity = state(handle).key;
    source::Poll poll; source::begin(true, 0, poll);
    const auto staged = source::publication_revision();
    // These are the two production filter reads. A replacement is published
    // after both snapshots but before the real finalizer acquires its gate.
    check(!source::read_pad_output(0, facts.now_ms, true) && !source::read_pad_output(0, facts.now_ms, true),
        "both staged filter reads precede the replacement");
    check(source::write_pad_output("replacement", 0, replacement, facts.now_ms).empty() &&
        source::publication_revision() > staged, "the intervening setter cannot share the old staged revision");
    lease::Pad pressed{}; pressed.buttons = replacement.buttons;
    const auto buffer = finish(poll, pressed, pressed, 0, true, true, 0, 7, 7, staged);
    const auto receipt = state(handle);
    check(buffer.pad == lease::Pad{} && receipt.key == identity && receipt.phase == capture::Phase::releasing &&
        receipt.debt && receipt.reason == capture::Reason::interference && !receipt.neutral_releases,
        "late replacement permits only retained neutral fallback, never continued capture or a false handback");
    lease::Snapshot unavailable;
    check(!source::lease_read(1, handle, unavailable, reason), "the stale candidate cannot issue a new passive observation");
    check(!source::capture_acknowledge(1, handle), "replacement interference cannot erase retained cleanup duty");
    source::release_pad_output("replacement"); report(); close(handle);
    check(!source::publication_held(), "all replacement paths return outside the publication gate");
}
void item_filters_aba() {
    // State's production assertion binds its actual member to this lightweight
    // type, without pulling rendering dependencies into the input fixture.
    using FilterFlag = source::PublishedAtomic<bool>;
    FilterFlag item_filters_visible{false};
    const auto handle = start(); const auto identity = state(handle).key;
    source::Poll poll; source::begin(true, 0, poll);
    const auto staged = source::publication_revision();
    item_filters_visible.store(true); item_filters_visible.store(false);
    check(!item_filters_visible.load() && source::publication_revision() > staged && !source::publication_held(),
        "item filters invalidate the staged raw candidate even when already closed");
    lease::Pad pressed{}; pressed.buttons = 0x1000;
    const auto buffer = finish(poll, pressed, pressed, 0, true, true, 0, 7, 7, staged);
    auto receipt = state(handle);
    check(buffer.pad == lease::Pad{} && receipt.key == identity && receipt.phase == capture::Phase::releasing &&
        receipt.debt && receipt.reason == capture::Reason::interference && !receipt.neutral_releases,
        "closed item filters cannot authorize continued capture or a false handback from the old candidate");
    lease::Reason reason; lease::Snapshot unavailable;
    check(!source::lease_read(1, handle, unavailable, reason) && !source::capture_acknowledge(1, handle),
        "item-filter interference retains source cleanup and refuses fresh samples or early ACK");
    report(); receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::complete && !receipt.debt &&
        receipt.neutral_releases == 1 && !receipt.polls_in_flight,
        "one later confirmed same-stream handback and exact callback drain discharge item-filter cleanup");
    close(handle);
}
void staged_aba() {
    const auto handle = start(); source::Poll poll; source::begin(true, 0, poll);
    const auto staged = source::publication_revision(); source::PublishedAtomic<bool> writer{false};
    writer.store(true); writer.store(false);
    lease::Pad pressed{}; pressed.buttons = 0x8000;
    const auto buffer = finish(poll, pressed, pressed, 0, true, true, 0, 7, 7, staged);
    const auto receipt = state(handle);
    check(buffer.pad == lease::Pad{} && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.neutral_releases, "stale staged write gets only owed neutral fallback, no completion");
    report(); close(handle);
    replacement_publication();
    item_filters_aba();
}
void failed_write() {
    const auto handle = arm(); write_mode = Write::partial;
    const auto buffer = report(); auto receipt = state(handle);
    check(buffer.packet != 7 && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.publications && !receipt.neutral_releases,
        "actual partial destination mutation retains unconfirmed debt even on first write");
    check(!source::capture_acknowledge(1, handle), "partial publication never ACKs");
    write_mode = Write::full; report(); receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && !receipt.neutral_releases,
        "unknown partial output first needs one confirmed translated publication");
    report(); receipt = state(handle);
    check(receipt.phase == capture::Phase::complete && !receipt.debt && receipt.neutral_releases == 1,
        "later confirmed raw handback resolves partial-write debt");
    close(handle);
}
void transport() {
    const auto handle = start(); const auto before = writes;
    report({}, {}, 1167, false);
    auto receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && writes == before,
        "failed original is not invented success or output");
    facts.now_ms += capture::Capture::maximum_release_ms;
    receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && receipt.quarantined &&
        !receipt.neutral_releases && !source::capture_acknowledge(1, handle),
        "lost-source 1s boundary retains unknown duty, never neutral consumption or ACK");
}
void raw_final() {
    const auto handle = start(); lease::Pad authored{}; authored.buttons = 0x8000;
    (void)report({}, authored); // first refusal establishes releasing duty
    const auto competing = report({}, authored);
    auto receipt = state(handle);
    check(competing.pad == authored && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.neutral_releases, "current independent candidate wins once held controls rest; no release is invented");
    (void)report({}, {}, 0, true, 8); receipt = state(handle);
    check(receipt.debt && !receipt.neutral_releases, "different raw/final packet cannot witness capture release");
    source::Poll unknown; source::begin(true, 0, unknown); const auto before = writes;
    (void)finish(unknown, {}, {}, 0, true, false);
    check(writes == before && state(handle).debt, "unknown final output is never confirmed");
    report(); close(handle);
}
void late_age() {
    const auto handle = start(); check(source::capture_release(1, handle), "release before aged callback");
    source::Poll slow; source::begin(true, 0, slow); facts.now_ms += 101;
    (void)finish(slow);
    const auto receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && !receipt.neutral_releases,
        "slow original/filter cannot become fresh merely at finalization");
    source::tick(1, 2, true, {true, false}); report(); close(handle);
}
void menu_admission() {
    lease::Handle handle; lease::Reason reason; capture::Reason capture_reason;
    source::tick(1, 2, true);
    (void)report();
    check(!source::native_menu_closed(facts.now_ms) && !source::lease_request(1, 0, handle, reason),
        "an unavailable native menu model cannot grant control authority");
    source::tick(1, 2, true, {true, true}); (void)report();
    check(!source::lease_request(1, 0, handle, reason), "a known open native menu cannot grant a lease");
    handle = lease_only(); const auto issued = sample(handle);
    source::tick(1, 2, true);
    check(!source::capture_request(1, handle, issued, capture_reason),
        "an issued sample cannot start capture after native menu state becomes unknown");
    check(source::lease_acknowledge(1, handle, reason), "unknown-state admission left only a terminal passive record");
    source::tick(1, 2, true, {true, false});
    check(source::native_menu_closed(facts.now_ms) && !source::native_menu_closed(facts.now_ms - 1),
        "legacy consumers cannot accept a menu observation from their future after clock regression");
    facts.now_ms += lease::maximum_age_ms;
    check(!source::native_menu_closed(facts.now_ms), "a stalled client cannot leave a permanent menu permit");
    (void)report();
    check(!source::lease_request(1, 0, handle, reason), "fresh raw input cannot renew a stale native menu observation");
    source::tick(1, 2, false, {true, false}); (void)report();
    check(!source::lease_request(1, 0, handle, reason), "loading cannot grant authority even with an old closed observation");
}
void menu_revocation(bool overlay_menu) {
    const auto handle = start(); const auto issued = sample(handle); const auto identity = state(handle).key;
    source::Poll pending; source::begin(true, 0, pending);
    const auto staged = source::publication_revision(); const auto before = writes;
    if (overlay_menu) facts.menu_closed = false;
    else source::tick(1, 2, true, {true, true});
    lease::Pad physical{}; physical.buttons = 0x1011; physical.left_trigger = 217;
    physical.left_x = 18000; physical.left_y = -14000; physical.right_x = 22000;
    lease::Reason reason; capture::Reason capture_reason;
    for (unsigned i = 0; i != 10; ++i) {
        check(!source::lease_renew(1, handle, issued, reason), "repeated malicious renewals cannot retain menu authority");
        check(!source::capture_request(1, handle, issued, capture_reason), "repeated capture requests cannot regain menu authority");
    }
    const auto buffer = finish(pending, physical, physical, 0, true, true, 0, 7, 7, staged);
    auto receipt = state(handle);
    check(buffer.pad == physical && buffer.packet == 7 && writes == before,
        "a menu gets physical sticks, buttons and packet without a capture write, including a staged old callback");
    check(receipt.key == identity && receipt.phase == capture::Phase::releasing && receipt.debt &&
        !receipt.neutral_releases && !receipt.polls_in_flight && !source::capture_acknowledge(1, handle),
        "bypassing menu input does not fabricate handback or discard cleanup debt");
    facts.menu_closed = true; source::tick(1, 2, true);
    const auto unknown = report(physical, physical);
    check(unknown.pad == physical && writes == before && state(handle).debt,
        "unknown native menu state continues passing physical input while retaining cleanup");
    source::tick(1, 2, true, {true, false});
    lease::Handle replacement;
    check(!source::lease_request(2, 0, replacement, reason), "old cleanup still reserves the stream after menu closure");
    (void)report(); receipt = state(handle);
    check(receipt.phase == capture::Phase::complete && !receipt.debt && receipt.neutral_releases == 1,
        "only later known-closed gameplay supplies an actual neutral handback");
    check(!source::lease_renew(1, handle, issued, reason), "menu closure does not resurrect the revoked lease");
    close(handle);
    (void)report();
    check(source::lease_request(2, 0, replacement, reason), "a new owner needs fresh admission after confirmed handback");
    check(source::lease_release(2, replacement, reason) && source::lease_acknowledge(2, replacement, reason),
        "fresh replacement owns its separate release duty");
}
void menu_button() {
    const auto handle = start(); lease::Pad physical{};
    physical.buttons = 0x1010; physical.left_x = 22000;
    const auto buffer = report(physical, physical);
    check(buffer.pad.buttons == dingosdk::input::menu_button && !buffer.pad.left_x && state(handle).debt,
        "physical Start survives active gameplay capture so the player can open settings");
    check(dingosdk::input::preserve_menu_button(0, 0xffff) == 0xffef,
        "a replacement cannot synthesize the protected menu button");
    check(dingosdk::input::preserve_menu_button(0x0010, 0) == 0x0010,
        "a replacement cannot erase the physical menu button");
    close(handle);
}
void menu_replacement() {
    source::tick(1, 2, true, {true, false});
    dingosdk::input::PadOutput replacement{}; replacement.left_x = -31000; replacement.buttons = 0xffff;
    check(source::write_pad_output("malicious", 0, replacement, facts.now_ms).empty(), "gameplay replacement fixture is published");
    source::tick(1, 2, true, {true, true});
    for (unsigned i = 0; i != 10; ++i) {
        check(source::write_pad_output("malicious", 0, replacement, facts.now_ms).empty(), "internal fixture retries publication");
        check(!source::read_pad_output(0, facts.now_ms, true) && !dingosdk::input::pad_outputs().active(facts.now_ms),
            "the trusted consumer discards every replacement while the native menu is open");
    }
    source::tick(1, 2, true);
    check(source::write_pad_output("malicious", 0, replacement, facts.now_ms).empty() &&
        !source::read_pad_output(0, facts.now_ms, true), "unknown native menu state also refuses replacement consumption");
    source::tick(1, 2, true, {true, false});
    check(!source::read_pad_output(0, facts.now_ms, true), "closing the menu cannot revive a discarded replacement");
}
void menu_staged_restore() {
    const auto handle = start();
    lease::Pad physical{}; physical.buttons = 0x1011; physical.left_x = 20000; physical.right_y = -17000;
    source::Poll staged; source::begin(true, 0, staged);
    const auto revision = source::publication_revision();
    source::tick(1, 2, true, {true, true});
    const auto before = writes;
    const auto restored = finish(staged, physical, {}, 0, true, true, 0, 7, 99, revision);
    check(restored.pad == physical && restored.packet == 7 && writes == before + 1 && state(handle).debt &&
        !state(handle).neutral_releases,
        "a menu race restores an already-filtered candidate with one bounded safety write, without claiming handback");
    source::Poll sdk; source::begin(true, 0, sdk);
    trusted_capture_active = true;
    const auto sdk_before = writes;
    const auto neutral = finish(sdk, physical, {}, 0, true, true, 0, 7, 99);
    check(neutral.pad == lease::Pad{} && neutral.packet == 99 && writes == sdk_before && state(handle).debt,
        "trusted overlay or prompt suppression is never undone by raw menu restoration");
    trusted_capture_active = false;
    source::tick(1, 2, true, {true, false}); report(); close(handle);
}
void renewal() {
    const auto handle = start(); const auto issued = sample(handle); facts.now_ms += 37;
    lease::Reason reason;
    check(source::lease_renew(1, handle, issued, reason), "exact fresh issued snapshot renews both source and capture");
    check(state(handle).expires_ms == facts.now_ms + 250 && state(handle).expires_ms > issued.observed_ms + 250,
        "capture follows actual now-plus250 passive renewal, not projection time");
    check(!source::lease_renew(1, handle, issued, reason), "one observation cannot renew twice");
    close(handle);
}
void owner_close() {
    const auto handle = start(); source::Poll old; source::begin(true, 0, old);
    source::lease_release_all(1); source::lease_release_all(1);
    auto receipt = state(handle);
    check(receipt.orphaned && receipt.debt && receipt.polls_in_flight == 1, "owner close retains exact pending original and owed write");
    report(); check(state(handle).phase == capture::Phase::releasing && state(handle).debt,
        "orphan raw handback waits for the old selected-stream callback");
    lease::Pad pressed{}; pressed.buttons = 1; check(finish(old, pressed, pressed).pad == lease::Pad{}, "old orphan original stays neutral");
    report();
    check(!source::capture_state(1, handle, receipt), "orphan reclaimed only after real release and callback drain");
    report(); lease::Handle next; lease::Reason reason;
    check(source::lease_request(2, 0, next, reason), "different owner gets only a fresh passive handle after cleanup");
    check(next.sequence != handle.sequence, "owner cleanup never resets source handle sequence");
    source::lease_release_all(2);
}
void held_tail() {
    const auto handle = start(); const auto identity = state(handle).key;
    check(source::capture_release(1, handle), "release owns the same key throughout physical hold");
    lease::Pad pressed{}; pressed.buttons = 0x1000; pressed.left_trigger = 255; pressed.left_x = 20000;
    (void)report(pressed, pressed); facts.now_ms += capture::Capture::maximum_release_ms;
    const auto before_stale = writes;
    const auto stale = report(pressed, pressed); auto receipt = state(handle);
    check(stale.pad == pressed && writes == before_stale,
        "an expired menu observation passes held physical controls without another cleanup write");
    check(receipt.key == identity && receipt.phase == capture::Phase::releasing && receipt.debt &&
        receipt.quarantined && !receipt.neutral_releases && !source::capture_acknowledge(1, handle),
        "1s keeps physically held capture tail reserved and refuses premature ACK");
    lease::Receipt passive; lease::Reason reason;
    check(source::lease_state(1, handle, passive, reason) && passive.capture_reserved && !passive.source_released &&
        !source::lease_acknowledge(1, handle, reason), "passive ACK cannot bypass tail work");
    // The host must observe the menu closed again before cleanup can own another report.
    source::tick(1, 2, true, {true, false});
    const auto before = receipt.publications; write_mode = Write::partial;
    (void)report(pressed, pressed); receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && receipt.quarantined &&
        receipt.reason == capture::Reason::publication_unconfirmed && receipt.publications == before &&
        !source::capture_acknowledge(1, handle), "pre-ACK partial tail write is charged and cannot silently clear debt");
    write_mode = Write::full; (void)report(pressed, pressed);
    check(state(handle).publications == before + 1, "confirmed tail write remains part of original capture ledger");
    (void)report(); receipt = state(handle);
    check(receipt.key == identity && receipt.phase == capture::Phase::complete && !receipt.debt &&
        !receipt.quarantined && receipt.neutral_releases == 1, "real physical rest plus raw handback finishes same key");
    close(handle);
}
void handback_collision() {
    const auto handle = start(); check(source::capture_release(1, handle), "request collision test release");
    Buffer confirmed;
    {
        // One actual older original prevents completion while the fresh report
        // establishes physical rest and a confirmed synthetic packet/neutral Pad.
        // Its destructor drains the ticket, but does not fabricate a publication.
        source::Poll blocker, fresh; source::begin(true, 0, blocker); source::begin(true, 0, fresh);
        confirmed = finish(fresh);
        check(state(handle).debt && !state(handle).neutral_releases, "callback fence retains the empty held set");
    }
    lease::Pad press{}; press.buttons = 0x4000;
    source::Poll collision; source::begin(true, 0, collision);
    const auto changed = finish(collision, press, press, 0, true, true, 0, confirmed.packet, confirmed.packet);
    auto receipt = state(handle);
    check(changed.packet != confirmed.packet && changed.pad == press && receipt.debt &&
        receipt.phase == capture::Phase::releasing && !receipt.neutral_releases &&
        !source::capture_acknowledge(1, handle), "equal-packet changed Pad needs translated publication before raw handback");
    source::Poll next; source::begin(true, 0, next);
    const auto handed = finish(next, press, press, 0, true, true, 0, confirmed.packet, confirmed.packet);
    receipt = state(handle);
    check(handed.packet == confirmed.packet && handed.pad == press && receipt.phase == capture::Phase::complete &&
        !receipt.debt && receipt.neutral_releases == 1, "non-colliding next raw report confirms handback without resetting sequence");
    close(handle);
}
void handback_partial() {
    const auto handle = start(); check(source::capture_release(1, handle), "request partial handback release");
    const auto before = writes; write_mode = Write::partial; (void)report();
    auto receipt = state(handle);
    check(writes == before + 1 && receipt.debt && receipt.quarantined &&
        receipt.phase == capture::Phase::releasing && !receipt.neutral_releases &&
        !source::capture_acknowledge(1, handle), "partial raw handback never stops translation or grants ACK");
    write_mode = Write::full; const auto translated = report();
    check(translated.packet != 7 && state(handle).debt, "unknown last output needs a complete translated bridge first");
    const auto raw = report(); receipt = state(handle);
    check(raw.packet == 7 && receipt.phase == capture::Phase::complete && !receipt.debt,
        "only a later confirmed same-stream raw handback ends output duty");
    close(handle);
}
void handback_race() {
    const auto handle = start(); check(source::capture_release(1, handle), "request callback-race release");
    source::Poll before, last; source::begin(true, 0, before); source::begin(true, 0, last);
    (void)finish(last); auto receipt = state(handle);
    check(receipt.phase == capture::Phase::releasing && receipt.debt && receipt.polls_in_flight == 1 &&
        !receipt.neutral_releases, "raw endpoint cannot retire the same stream while another original is live");
    lease::Pad press{}; press.buttons = 0x2000; write_mode = Write::partial;
    (void)finish(before, press, press); receipt = state(handle);
    check(receipt.debt && receipt.quarantined && !receipt.polls_in_flight && !receipt.neutral_releases &&
        !source::capture_acknowledge(1, handle), "late partial continuation stays charged to original unacknowledged key");
    write_mode = Write::full; report(); report(); close(handle);
}
void post_ack() {
    const auto handle = start(); const auto key = state(handle).key; close(handle);
    const auto before = writes; lease::Pad press{}; press.buttons = 0x8000; press.right_trigger = 200;
    for (std::uint32_t packet : {7u, 8u, 8u, 0xffffffffu, 0u}) {
        source::Poll call; source::begin(true, 0, call);
        const auto buffer = finish(call, press, press, 0, true, true, 0, packet, packet);
        check(buffer.packet == packet && buffer.pad == press && writes == before,
            "after ACK no capture-owned Pad OR packet-only write survives, including wrap/packet repeats");
    }
    source::PublishedAtomic<bool> later_writer{false}; later_writer.store(true); later_writer.store(false);
    (void)report(press, press);
    check(writes == before, "post-ACK publication interference cannot resurrect an old capture write");
    capture::Receipt absent;
    check(!source::capture_state(1, handle, absent) && !source::capture_release(1, handle) &&
        !source::capture_acknowledge(1, handle), "acknowledged key cannot be resurrected or reused");
    source::tick(1, 2, true, {true, false}); report(); lease::Handle next; lease::Reason reason;
    check(source::lease_request(2, 0, next, reason) && next.sequence != key.lease_sequence,
        "later owner only acquires a fresh monotonic handle");
    source::lease_release_all(2);
}
void packet_only_failure() {
    const auto handle = start(); lease::Pad independent{}; independent.buttons = 0x8000;
    (void)report({}, independent); // revoke authority; the next fresh original can establish physical rest
    const auto before = state(handle).publications;
    write_mode = Write::partial; const auto buffer = report({}, independent);
    auto receipt = state(handle);
    check(buffer.packet != 7 && offered_pad == independent && receipt.phase == capture::Phase::releasing &&
        receipt.debt && receipt.quarantined && receipt.reason == capture::Reason::publication_unconfirmed &&
        receipt.publications == before && !source::capture_acknowledge(1, handle), "packet-only partial publication is charged even without a suppression plan");
    write_mode = Write::full; report(); report(); close(handle);
}
void handback_equal_pad() {
    const auto handle = start(); const auto confirmed = report();
    check(source::capture_release(1, handle), "request identical Pad collision release");
    source::Poll call; source::begin(true, 0, call);
    const auto handed = finish(call, {}, {}, 0, true, true, 0, confirmed.packet, confirmed.packet);
    const auto receipt = state(handle);
    check(handed.packet == confirmed.packet && handed.pad == confirmed.pad &&
        receipt.phase == capture::Phase::complete && !receipt.debt && receipt.neutral_releases == 1,
        "equal raw/last packet with identical confirmed Pad is a safe actual handback");
    close(handle);
}
void replacement_no_write() {
    const auto handle = start(); check(source::capture_release(1, handle), "request old-stream release");
    (void)report({}, {}, 1167, false); const auto before = writes;
    lease::Pad press{}; press.buttons = 1;
    for (unsigned i = 0; i != 3; ++i) (void)report(press, press);
    facts.now_ms += capture::Capture::maximum_release_ms;
    const auto receipt = state(handle);
    check(writes == before && receipt.phase == capture::Phase::releasing && receipt.debt && receipt.quarantined &&
        !source::capture_acknowledge(1, handle), "replacement is neither normalized/suppressed nor old-stream handback proof");
}
void capacity() {
    const auto handle = start();
    {
        std::array<source::Poll, 129> calls;
        for (auto& call : calls) source::begin(true, 0, call);
        lease::Pad pressed{}; pressed.buttons = 1;
        check(finish(calls.back(), pressed, pressed).pad == lease::Pad{}, "unrecorded owned original gets only protected neutral fallback");
        const auto receipt = state(handle);
        check(receipt.callbacks_unknown && receipt.quarantined && receipt.debt && receipt.polls_in_flight == 128,
            "capacity failure preserves explicit unknown duty and bounded known count");
    }
    const auto receipt = state(handle);
    check(receipt.callbacks_unknown && receipt.debt && !receipt.polls_in_flight &&
        !source::capture_acknowledge(1, handle), "known callback drain cannot erase unknown capacity duty");
    lease::Receipt passive; lease::Reason reason;
    check(source::lease_state(1, handle, passive, reason) && passive.capture_reserved && !passive.source_released,
        "passive source release stays false under capture unknown");
    check(!source::lease_acknowledge(1, handle, reason), "passive ACK cannot bypass unknown capture");
}
}
namespace dingosdk::native_input_source::environment {
Facts sample() noexcept {
    auto result = facts;
    result.interference_clear = result.interference_clear && !input::pad_outputs().active(result.now_ms);
    return result;
}
bool trusted_capture() noexcept { return trusted_capture_active; }
bool preview_report_valid(std::uint64_t, std::uint64_t) noexcept { return false; }
}
int main(int argc, char** argv) {
    if (argc != 2) return 2; const std::string_view mode = argv[1];
    if (mode == "denial") denial(); else if (mode == "cold_original") cold_original();
    else if (mode == "publication") publication(); else if (mode == "late_drain") late_drain();
    else if (mode == "interference") interference(); else if (mode == "replacement") replacement();
    else if (mode == "context_release") context_release(); else if (mode == "staged_aba") staged_aba();
    else if (mode == "failed_write") failed_write(); else if (mode == "transport") transport();
    else if (mode == "raw_final") raw_final(); else if (mode == "late_age") late_age();
    else if (mode == "renewal") renewal(); else if (mode == "owner_close") owner_close();
    else if (mode == "capacity") capacity(); else if (mode == "held_tail") held_tail();
    else if (mode == "handback_collision") handback_collision(); else if (mode == "handback_partial") handback_partial();
    else if (mode == "handback_race") handback_race(); else if (mode == "post_ack") post_ack();
    else if (mode == "replacement_no_write") replacement_no_write();
    else if (mode == "packet_only_failure") packet_only_failure();
    else if (mode == "handback_equal_pad") handback_equal_pad();
    else if (mode == "menu_admission") menu_admission();
    else if (mode == "menu_revocation") menu_revocation(false);
    else if (mode == "overlay_menu") menu_revocation(true);
    else if (mode == "menu_button") menu_button();
    else if (mode == "menu_replacement") menu_replacement();
    else if (mode == "menu_staged_restore") menu_staged_restore(); else return 2;
    std::printf("PASS %s %u constructed provider checks; no native-consumption proof\n", argv[1], checks);
    return 0;
}
