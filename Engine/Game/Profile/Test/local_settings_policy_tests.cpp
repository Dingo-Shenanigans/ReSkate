#include "Engine/Game/Profile/local_settings_policy.h"
#include "Engine/Game/Build/20260929/local_native_settings.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace p = dingosdk::local_settings_policy;
namespace pins = dingosdk::game::build::v20260929::local_native_settings;
namespace {
unsigned checks{}, groups{};
void check(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(why);
}
p::StartupRequest selected_request() { return {L"1", true, true, true, true}; }
void activate(p::Authority& authority) {
    check(authority.begin(selected_request()) && authority.publish(true), "selected installation did not become active");
}
struct Errors {
    std::uint32_t value{9};
    auto get() { return [this]() noexcept { return value; }; }
    auto set() { return [this](std::uint32_t next) noexcept { value = next; }; }
};
void approved_callers_use_the_actual_pins_and_exact_returns() {
    ++groups;
    constexpr std::uintptr_t image = 0x10000000;
    constexpr auto maximum = (std::numeric_limits<std::uintptr_t>::max)();
    for (const auto& call : pins::calls) {
        const auto caller = image + call.instruction + call.bytes.size();
        check(p::approved_caller(image, caller, pins::calls), "an approved pinned return address was refused");
        check(!p::approved_caller(image, caller - 1, pins::calls) && !p::approved_caller(image, caller + 1, pins::calls),
            "an adjacent instruction was admitted as an approved caller");
        check(!p::approved_caller(0, call.instruction + call.bytes.size(), pins::calls), "a baseless native caller was admitted");
        const auto wrapped_instruction = maximum - call.instruction + 1;
        check(!p::approved_caller(wrapped_instruction, wrapped_instruction + call.instruction + call.bytes.size(), pins::calls),
            "an overflowing image-plus-instruction matched a wrapped caller");
        const auto wrapped_return = maximum - call.instruction - 2;
        check(!p::approved_caller(wrapped_return, wrapped_return + call.instruction + call.bytes.size(), pins::calls),
            "an overflowing return address matched an approved caller");
    }
    check(!p::approved_caller(image, image + 0x1234, pins::calls) && !p::approved_caller(image, 0, pins::calls),
        "an unknown or null caller was admitted");
    const std::array<pins::Call, 0> absent{};
    check(!p::approved_caller(image, image + 5, absent), "an empty approved table admitted a caller");
    struct EmptyCall { std::uintptr_t instruction; std::array<std::uint8_t, 0> bytes; };
    const std::array<EmptyCall, 1> empty_instruction{{{10, {}}}};
    check(!p::approved_caller(image, image + 10, empty_instruction), "an unverified zero-byte instruction admitted a caller");
}
void startup_request_guards() {
    ++groups;
    const std::array<std::wstring_view, 7> flags{L"", L"0", L"true", L"01", L"1 ", L" 1", std::wstring_view(L"1\0", 2)};
    for (const auto flag : flags) {
        auto request = selected_request(); request.opt_in = flag;
        p::Authority authority;
        check(!p::selected(request) && !authority.begin(request) && authority.phase() == p::Phase::failed,
            "an inexact opt-in selected the native settings adapter");
        check(!authority.begin(selected_request()) && !authority.publish(true), "failed initialization rearmed after a flag change");
    }
    for (unsigned fault = 0; fault < 4; ++fault) {
        auto request = selected_request();
        if (fault == 0) request.global_offline_requested = false;
        if (fault == 1) request.offline_active = false;
        if (fault == 2) request.redirect_checked = false;
        if (fault == 3) request.development_root_selected = false;
        p::Authority authority;
        check(!authority.begin(request) && authority.phase() == p::Phase::failed,
            "missing EA-offline request, active boot, checked redirect or development selection admitted installation");
    }
    p::Authority selected;
    check(selected.phase() == p::Phase::inactive && p::selected(selected_request()), "fresh authority or complete request changed");
    check(selected.begin(selected_request()) && selected.phase() == p::Phase::installing && !selected.active(),
        "initialization published authority before hook verification");
}
void authority_publication_and_revocation() {
    ++groups;
    p::Authority uninstalled;
    check(!uninstalled.publish(true) && !uninstalled.active(), "uninstalled hooks published authority");
    p::Authority failed;
    check(failed.begin(selected_request()) && !failed.publish(false) && failed.phase() == p::Phase::failed,
        "an unverified hook pair published authority");
    check(!failed.begin(selected_request()) && !failed.publish(true), "failed hook installation rearmed");
    p::Authority interrupted;
    check(interrupted.begin(selected_request()), "installation fixture refused");
    interrupted.revoke();
    check(!interrupted.publish(true) && interrupted.phase() == p::Phase::revoked,
        "publication defeated an installation-time revocation");
    p::Authority current; activate(current);
    check(current.check(true), "current launch selection was refused");
    check(!current.begin(selected_request()) && !current.publish(true) && current.active(),
        "duplicate initialization replaced the active installation");
    check(!current.check(false) && current.phase() == p::Phase::revoked, "selection drift did not revoke authority");
    check(!current.check(true) && !current.begin(selected_request()) && !current.publish(true),
        "restoring a previous selection revived revoked authority");
    current.fail();
    check(current.phase() == p::Phase::revoked, "a later failure erased the revocation history");
    p::Authority setup_failure;
    check(setup_failure.begin(selected_request()), "setup failure fixture refused");
    setup_failure.fail();
    check(setup_failure.phase() == p::Phase::failed && !setup_failure.publish(true), "setup failure was not terminal");
}
void native_gate_preservation() {
    ++groups;
    for (unsigned phase = 0; phase < 5; ++phase) {
        p::Authority authority;
        if (phase == 1) check(authority.begin(selected_request()), "installing gate fixture refused");
        if (phase == 2) activate(authority);
        if (phase == 3) authority.fail();
        if (phase == 4) authority.revoke();
        Errors error; unsigned originals{}, qualifications{};
        const bool result = p::gate(authority, [&] {
            ++originals; check(error.value == 9, "native predicate lost its incoming error");
            error.value = 71; return true;
        }, [&]() -> bool { ++qualifications; throw std::runtime_error("admission must not run"); }, error.get(), error.set());
        check(result && originals == 1 && qualifications == 0 && error.value == 71,
            "native true was changed, repeated or subjected to adaptation reads");
    }
}
void false_gate_requires_current_qualification() {
    ++groups;
    // These qualify the shared composition only. Actual query/user snapshot
    // readers have separate native-layout tests; no synthetic ownership join exists here.
    for (unsigned outcome = 0; outcome < 4; ++outcome) {
        p::Authority authority; activate(authority);
        Errors error; unsigned originals{}, qualifications{};
        const bool result = p::gate(authority, [&] { ++originals; error.value = 72; return false; }, [&] {
            ++qualifications; error.value = 901;
            if (outcome == 2) throw std::runtime_error("constructed snapshot failure");
            if (outcome == 3) authority.revoke();
            return outcome != 1;
        }, error.get(), error.set());
        check(result == (outcome == 0) && originals == 1 && qualifications == 1 && error.value == 72,
            "false predicate ignored refusal, exception, closing revocation or native error preservation");
    }
    for (unsigned phase = 0; phase < 4; ++phase) {
        p::Authority authority;
        if (phase == 1) check(authority.begin(selected_request()), "installing refusal fixture refused");
        if (phase == 2) authority.fail();
        if (phase == 3) authority.revoke();
        Errors error; unsigned originals{}, qualifications{};
        check(!p::gate(authority, [&] { ++originals; error.value = 73; return false; },
            [&] { ++qualifications; return true; }, error.get(), error.set()) &&
            originals == 1 && qualifications == 0 && error.value == 73,
            "inactive installation inspected adaptation witnesses or changed native false");
    }
}
void storage_result_and_error_contract() {
    ++groups;
    for (const auto native_result : std::array<std::uint32_t, 3>{0, pins::invalid_path_result, 0xffffffffU}) {
        p::Authority authority; activate(authority);
        Errors error; unsigned originals{}, validations{};
        const auto result = p::configure_storage(authority, [&] {
            ++originals;
            check(error.value == 9 && validations == 1, "native configuration saw validation errors or ran before validation");
            error.value = 74; return native_result;
        }, [&] { ++validations; error.value = 902; return true; }, error.get(), error.set(), pins::invalid_path_result);
        check(result == native_result && originals == 1 && validations == 1 && error.value == 74,
            "accepted native configuration changed its exact result/error or ran repeatedly");
    }
    for (unsigned refusal = 0; refusal < 3; ++refusal) {
        p::Authority authority; activate(authority);
        Errors error; unsigned originals{}, validations{};
        const auto result = p::configure_storage(authority, [&] { ++originals; return 0U; }, [&] {
            ++validations; error.value = 903;
            if (refusal == 1) throw std::runtime_error("constructed path failure");
            if (refusal == 2) { authority.revoke(); return true; }
            return false;
        }, error.get(), error.set(), pins::invalid_path_result);
        check(result == pins::invalid_path_result && originals == 0 && validations == 1 && error.value == 9,
            "rejected path/configuration/revoked selection entered native I/O or changed the incoming error");
    }
    for (unsigned phase = 0; phase < 4; ++phase) {
        p::Authority authority;
        if (phase == 1) check(authority.begin(selected_request()), "installing storage fixture refused");
        if (phase == 2) authority.fail();
        if (phase == 3) authority.revoke();
        Errors error; unsigned originals{}, validations{};
        check(p::configure_storage(authority, [&] { ++originals; return 0U; }, [&] { ++validations; return true; },
            error.get(), error.set(), pins::invalid_path_result) == pins::invalid_path_result &&
            originals == 0 && validations == 0 && error.value == 9,
            "unpublished storage hook entered native configuration or filesystem validation");
    }
}
void native_exceptions_are_not_adaptation_success() {
    ++groups;
    p::Authority authority; activate(authority);
    Errors error; unsigned originals{}, qualifications{}; bool propagated{};
    try {
        (void)p::gate(authority, [&]() -> bool { ++originals; error.value = 75; throw std::runtime_error("native predicate"); },
            [&] { ++qualifications; return true; }, error.get(), error.set());
    } catch (const std::runtime_error&) { propagated = true; }
    check(propagated && originals == 1 && qualifications == 0 && error.value == 75,
        "native predicate exception became an adapted result or lost native error state");
    error.value = 9; originals = qualifications = 0; propagated = false;
    try {
        (void)p::configure_storage(authority, [&]() -> std::uint32_t {
            ++originals; check(error.value == 9, "native throwing configuration lost incoming error");
            error.value = 76; throw std::runtime_error("native configuration");
        }, [&] { ++qualifications; error.value = 904; return true; }, error.get(), error.set(), pins::invalid_path_result);
    } catch (const std::runtime_error&) { propagated = true; }
    check(propagated && originals == 1 && qualifications == 1 && error.value == 76,
        "native configuration exception became success or lost its own error state");
}

struct HookPair {
    std::array<unsigned, 2> prepares{}, enables{}, removes{};
    std::vector<unsigned> order;
    unsigned commits{};
    unsigned refuse_prepare{2}, unusable_relay{2}, refuse_enable{2}, refuse_remove{2}, throw_prepare{2}, throw_remove{2};
    bool refuse_commit{};
    p::Authority* authority{};
    bool published_early{};
    void entered(unsigned event) {
        order.push_back(event);
        if (authority && authority->active()) published_early = true;
    }
    p::InstallResult install() {
        return p::install_pair([&](unsigned index) -> p::PrepareResult {
            ++prepares[index]; entered(10 + index);
            if (index == throw_prepare) throw std::runtime_error("prepare ownership unknown");
            const bool prepared = index != refuse_prepare;
            return {prepared, prepared && index != unusable_relay};
        }, [&](unsigned index) {
            ++enables[index]; entered(20 + index); return index != refuse_enable;
        }, [&] { ++commits; entered(30); return !refuse_commit; }, [&](unsigned index) {
            ++removes[index]; entered(40 + index);
            if (index == throw_remove) throw std::runtime_error("remove");
            return index != refuse_remove;
        });
    }
};
void paired_hooks_commit_before_publication() {
    ++groups;
    p::Authority authority; check(authority.begin(selected_request()), "hook pair fixture refused");
    HookPair pair; pair.authority = &authority;
    const auto result = pair.install();
    check(result.verified() && result.prepared == 2 && result.cleanup_complete && !pair.published_early &&
        pair.order == std::vector<unsigned>{10, 11, 20, 21, 30} && pair.removes == std::array<unsigned, 2>{},
        "hook pair published before both relays, queued enables and explicit commit, or removed successful hooks");
    check(!authority.active() && authority.publish(result.verified()) && authority.active(),
        "only verified paired installation may publish authority");
}
void paired_hook_failures_keep_ownership_and_cleanup_truthful() {
    ++groups;
    for (unsigned index = 0; index < 2; ++index) {
        HookPair pair; pair.refuse_prepare = index;
        const auto result = pair.install();
        check(!result.verified() && result.failure == p::InstallFailure::prepare && result.cleanup_complete &&
            pair.removes[index] == 0 && pair.removes[0] == index && pair.enables == std::array<unsigned, 2>{} && !pair.commits,
            "refused prepare removed an unowned target or continued to enable/commit");
        HookPair unusable; unusable.unusable_relay = index;
        const auto relay = unusable.install();
        check(!relay.verified() && relay.failure == p::InstallFailure::prepare && relay.cleanup_complete &&
            unusable.removes[0] == 1 && unusable.removes[index] == 1 && unusable.removes[1] == index && !unusable.commits,
            "acquired target with an unusable relay escaped exactly-once rollback");
        HookPair enabling; enabling.refuse_enable = index;
        const auto queued = enabling.install();
        check(!queued.verified() && queued.failure == p::InstallFailure::enable && queued.cleanup_complete &&
            enabling.removes == std::array<unsigned, 2>{1, 1} && !enabling.commits &&
            enabling.enables[0] == 1 && enabling.enables[1] == index &&
            enabling.order[enabling.order.size() - 2] == 41 && enabling.order.back() == 40,
            "partial queued enable committed or failed to remove both owned targets in reverse order");
    }
    HookPair committing; committing.refuse_commit = true;
    const auto commit = committing.install();
    check(!commit.verified() && commit.failure == p::InstallFailure::commit && commit.cleanup_complete &&
        committing.commits == 1 && committing.removes == std::array<unsigned, 2>{1, 1},
        "failed explicit commit did not roll back both prepared targets exactly once");
    for (unsigned failure = 0; failure < 2; ++failure) {
        HookPair cleanup; cleanup.refuse_commit = true;
        if (failure == 0) cleanup.refuse_remove = 1;
        else cleanup.throw_remove = 1;
        const auto result = cleanup.install();
        check(!result.verified() && result.failure == p::InstallFailure::commit && !result.cleanup_complete &&
            cleanup.removes == std::array<unsigned, 2>{1, 1},
            "failed cleanup lost the original cause, claimed clean removal or skipped another owned target");
        p::Authority authority; check(authority.begin(selected_request()), "failed cleanup authority fixture refused");
        check(!authority.publish(result.verified()) && authority.phase() == p::Phase::failed,
            "unknown partial installation published adaptation authority");
    }
    HookPair throwing; throwing.throw_prepare = 1;
    const auto unknown = throwing.install();
    check(!unknown.verified() && unknown.failure == p::InstallFailure::prepare && !unknown.cleanup_complete &&
        throwing.removes == std::array<unsigned, 2>{1, 0},
        "throwing prepare invented an ownership receipt or forgot earlier acquired cleanup");
}
}
int main() {
    try {
        approved_callers_use_the_actual_pins_and_exact_returns(); startup_request_guards();
        authority_publication_and_revocation(); native_gate_preservation();
        false_gate_requires_current_qualification(); storage_result_and_error_contract();
        native_exceptions_are_not_adaptation_success(); paired_hooks_commit_before_publication();
        paired_hook_failures_keep_ownership_and_cleanup_truthful();
        std::cout << "PASS " << groups << " groups " << checks << " checks; portable policy and constructed native boundaries only\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
