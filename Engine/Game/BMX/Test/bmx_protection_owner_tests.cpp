#include "Engine/Game/Skater/protection_owner.h"
#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {
struct Owner {
    std::uintptr_t client{}, entity{}, core{}, context{}, selector{}, rig{};
    bool operator==(const Owner&) const = default;
};
int failures{};
void check(bool value, const char* message) {
    if (!value) { std::printf("FAIL: %s\n", message); ++failures; }
}
}
int main() {
    using dingosdk::skater::validated_protection_owner;
    const Owner published{1, 2, 3, 4, 5, 6};
    Owner live = published, result{};
    unsigned reads{};
    bool available = true;
    const auto resolve = [&](std::uintptr_t client, std::uintptr_t entity, Owner& output) noexcept {
        ++reads;
        if (!available || client != live.client || entity != live.entity) return false;
        output = live; return true;
    };
    check(validated_protection_owner(published, true, published.core, &Owner::core, resolve, &result) && result == published,
        "a live lease accepts the complete currently owned native chain");
    reads = 0;
    check(!validated_protection_owner(published, false, published.core, &Owner::core, resolve) && reads == 0,
        "expired/released lease does not inspect or protect native memory");
    check(!validated_protection_owner(published, true, 99, &Owner::core, resolve) && reads == 0,
        "another actor's hook object cannot use the local protection lease");
    available = false;
    check(!validated_protection_owner(published, true, published.core, &Owner::core, resolve),
        "loading/unavailable ownership cancels protection");
    available = true;
    for (auto member : {&Owner::client, &Owner::entity, &Owner::core, &Owner::context, &Owner::selector, &Owner::rig}) {
        live = published; ++(live.*member);
        check(!validated_protection_owner(published, true, published.core, &Owner::core, resolve),
            "recycled address or changed owner chain cannot inherit a lease");
    }
    live = published;
    check(validated_protection_owner(published, true, published.selector, &Owner::selector, resolve),
        "independent offboard mount guard validates its selector against the same ownership chain");
    // A released manual slot does not invalidate the independent BMX slot, and vice versa.
    const auto protected_by_either = [&](bool manual, bool bmx) {
        return validated_protection_owner(published, manual, published.rig, &Owner::rig, resolve) ||
            validated_protection_owner(published, bmx, published.rig, &Owner::rig, resolve);
    };
    check(protected_by_either(true, false) && protected_by_either(false, true) && !protected_by_either(false, false),
        "each protection owner releases independently without clearing the other");
    if (failures) std::printf("%d protection owner check(s) failed\n", failures);
    else std::printf("BMX protection owner checks passed\n");
    return failures ? 1 : 0;
}
