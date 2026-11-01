#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/effects.h"
#include "Extension/Decals/native_decals.h"
#include "Extension/Skater/no_bail.h"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace decal_build = dingosdk::addr::decals;
std::array<std::byte, decal_build::manager_realm + sizeof(std::uintptr_t)> manager_storage{};
std::array<std::byte, decal_build::realm_update_queue + sizeof(std::uintptr_t)> realm_storage{}, replacement_realm{};
std::uintptr_t particle_service = 10, decal_manager = 20;
std::vector<std::uintptr_t> stopped, released;
std::vector<std::uint32_t> decals_released;
std::uintptr_t resolved_asset{};
unsigned descriptors{}, creations{}, decal_creations{};
void initialize_descriptor(void*, std::uintptr_t, const float*) { ++descriptors; }
void destroy_descriptor(void*) {}
void create_particle(std::uintptr_t, std::uintptr_t* handle, void*) { ++creations; *handle = 303; }
void stop_particle(std::uintptr_t* handle, bool immediate, float delay) {
    if (!immediate || delay != 0) throw std::runtime_error("reset must stop particles immediately");
    stopped.push_back(*handle);
}
void release_particle(std::uintptr_t* handle) { released.push_back(*handle); *handle = 0; }
std::uintptr_t native_function(std::uintptr_t, std::uintptr_t rva) {
    namespace fx = dingosdk::addr::effects;
    if (rva == fx::stop.rva) return reinterpret_cast<std::uintptr_t>(&stop_particle);
    if (rva == fx::release.rva) return reinterpret_cast<std::uintptr_t>(&release_particle);
    if (rva == fx::descriptor_init.rva) return reinterpret_cast<std::uintptr_t>(&initialize_descriptor);
    if (rva == fx::descriptor_destroy.rva) return reinterpret_cast<std::uintptr_t>(&destroy_descriptor);
    if (rva == fx::create.rva) return reinterpret_cast<std::uintptr_t>(&create_particle);
    throw std::runtime_error("unexpected native effect operation");
}
void release_decal(std::uintptr_t, dingosdk::decals::native::Handle& handle) {
    decals_released.push_back(handle.id); handle = {};
}
dingosdk::decals::native::Handle create_decal() {
    ++decal_creations;
    dingosdk::decals::native::Handle handle; handle.id = 404; return handle;
}
}
#define DINGOSDK_EFFECT_FUNCTION(base, rva) native_function(base, rva)
#define DINGOSDK_EFFECT_SERVICE(s) particle_service
#define DINGOSDK_EFFECT_DECAL_MANAGER(base) decal_manager
#define DINGOSDK_EFFECT_DECAL_CREATE(base, manager, asset, transform) create_decal()
#define DINGOSDK_EFFECT_DECAL_RELEASE(base, handle) release_decal(base, handle)
#define DINGOSDK_EFFECT_ASSET(s, name) resolved_asset
#include "Extension/Rendering/plugin_effects.cpp"

namespace dingosdk {
bool no_bail_skater(NoBailSkater&) noexcept { return false; }
}
namespace {
using namespace dingosdk::effects;
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
template<std::size_t N> std::uintptr_t address(const std::array<std::byte, N>& bytes) {
    return reinterpret_cast<std::uintptr_t>(bytes.data());
}
template<std::size_t N> void put(std::array<std::byte, N>& bytes, std::size_t at, std::uintptr_t value) {
    check(at + sizeof(value) <= bytes.size(), "fixture field outside allocation");
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}
void fixture() {
    auto& s = state();
    s.entries.clear(); s.assets.clear();
    put(manager_storage, decal_build::manager_realm, address(realm_storage));
    put(realm_storage, decal_build::realm_update_queue, address(realm_storage));
    put(replacement_realm, decal_build::realm_update_queue, address(replacement_realm));
    s.base = 0; s.service = particle_service = 10; s.manager = decal_manager = address(manager_storage);
    s.world = 30; s.entity = 40;
    s.valid = s.checked = s.allowed = true; s.suspended = false;
    s.thread = GetCurrentThreadId();
    check(service(s) == 0, "the fixture never resolves a real native service");
    check(asset(s, "effects/reskate/test/ebp_spark") == 0, "the fixture never resolves real game assets");
    Entry particle; particle.owner = "owner"; particle.particle = 101;
    Entry decal; decal.owner = "owner"; decal.decal.id = 202; decal.decal_realm = address(realm_storage);
    s.entries.emplace(1, particle); s.entries.emplace(2, decal);
    stopped.clear(); released.clear(); decals_released.clear();
    descriptors = creations = decal_creations = 0;
}
void creation_checks_the_actual_asset_owner() {
    namespace fx = dingosdk::addr::effects;
    fixture();
    state().entries.clear();
    state().serial = 10;
    std::uint32_t record = fx::particle_type_hash;
    const auto type = reinterpret_cast<std::uintptr_t>(&record);
    const auto type_pointer = reinterpret_cast<std::uintptr_t>(&type);
    std::array<std::byte, 32> blueprint{};
    std::memcpy(blueprint.data() + fx::asset_type, &type_pointer, sizeof(type_pointer));
    resolved_asset = reinterpret_cast<std::uintptr_t>(blueprint.data());
    dingosdk::plugins::EffectRequest request;
    request.asset = "effects/reskate/test/ebp_spark";
    request.transform = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for (const auto compartment : std::array<std::uint16_t, 9>{0, 1, 2, 3, 4, 6, 1507, 3006, 3007}) {
        std::memcpy(blueprint.data() + fx::asset_compartment, &compartment, sizeof(compartment));
        check(!create("owner", request), "a private particle name cannot authorize an asset outside Game");
    }
    request.asset = "world/materials/decal/reskate_test/stain_dv";
    request.decal = true;
    record = fx::decal_type_hash;
    check(!create("owner", request), "a private decal name cannot authorize an asset outside Game");
    check(descriptors == 0 && creations == 0 && state().entries.empty(), "refused assets never reach the native renderer");
    request.asset = "effects/reskate/test/ebp_spark";
    request.decal = false;
    record = fx::particle_type_hash;
    const auto compartment = fx::resident_compartment;
    std::memcpy(blueprint.data() + fx::asset_compartment, &compartment, sizeof(compartment));
    const auto id = create("owner", request);
    check(id && descriptors == 1 && creations == 1, "a particle owned by the level-wide compartment can be created");
    release("owner", id);
    check(state().entries.empty() && released == std::vector<std::uintptr_t>{303}, "the accepted particle is still released normally");
    request.asset = "world/materials/decal/reskate_test/stain_dv";
    request.decal = true;
    record = fx::decal_type_hash;
    const auto decal_id = create("owner", request);
    check(decal_id && decal_creations == 1, "a decal with a live realm can be created");
    release("owner", decal_id);
    check(decals_released == std::vector<std::uint32_t>{404}, "a new decal records its realm so normal release is allowed");
    const auto stale_id = create("owner", request);
    check(stale_id && decal_creations == 2, "another decal can be created in the original realm");
    put(manager_storage, decal_build::manager_realm, address(replacement_realm));
    release("owner", stale_id);
    check(state().entries.empty() && decals_released.size() == 1, "a new realm cannot receive a decal created in the old realm");
    put(manager_storage, decal_build::manager_realm, 0);
    check(!create("owner", request) && decal_creations == 2, "a decal cannot be created without an owning realm");
    resolved_asset = 0;
}
void world_replacement_releases_surviving_services() {
    fixture();
    const auto epoch = state().epoch;
    tick(0, 0, false, false, false); // The old world no longer resolves.
    check(stopped == std::vector<std::uintptr_t>{101} && released == stopped,
        "a lost world must stop and release particles in the global service");
    check(decals_released == std::vector<std::uint32_t>{202}, "a lost world releases surviving decal resources");
    check(state().entries.empty() && state().epoch != epoch, "world loss invalidates every public handle");
    fixture(); decal_manager = 21;
    tick(0, 0, false, false, false);
    check(released == std::vector<std::uintptr_t>{101} && decals_released.empty(),
        "a replaced decal manager must not leak particles or receive stale decal IDs");
    fixture(); particle_service = 11;
    tick(0, 0, false, false, false);
    check(released.empty() && decals_released == std::vector<std::uint32_t>{202},
        "a replaced particle service must not leak decals or receive stale particle references");
}
void world_teardown_skips_invalid_decal_realms() {
    for (const auto realm : {std::uintptr_t{}, std::uintptr_t{1}, address(replacement_realm)}) {
        fixture();
        const auto epoch = state().epoch;
        put(manager_storage, decal_build::manager_realm, realm);
        tick(0, 0, false, false, false);
        check(stopped == std::vector<std::uintptr_t>{101} && released == stopped,
            "a lost decal realm must not prevent cleanup in the surviving particle service");
        check(decals_released.empty(), "a cleared, unreadable or replaced realm must not receive stale decal IDs");
        check(state().entries.empty() && state().epoch != epoch, "realm loss invalidates every public handle");
    }
    fixture();
    put(realm_storage, decal_build::realm_update_queue, 0);
    tick(0, 0, false, false, false);
    check(released == std::vector<std::uintptr_t>{101} && decals_released.empty(),
        "a realm without its update queue cannot receive releases even when the manager survives");
}
void cleanup_checks_the_realm_before_the_next_tick() {
    fixture();
    put(manager_storage, decal_build::manager_realm, 0);
    release("owner", 2);
    clear("owner");
    check(state().entries.empty() && released == std::vector<std::uintptr_t>{101} && decals_released.empty(),
        "explicit cleanup refuses stale decal IDs before tick observes a world change");
    fixture();
    put(manager_storage, decal_build::manager_realm, 0);
    before_level_transition(24);
    check(state().entries.empty() && released == std::vector<std::uintptr_t>{101} && decals_released.empty(),
        "a late transition callback also refuses releases into a destroyed realm");
}
void transition_on_another_thread_finishes_cleanup() {
    fixture();
    std::thread transition([] { before_level_transition(24); });
    transition.join();
    check(state().entries.empty() && state().suspended, "transition suspends effects and empties ownership");
    check(stopped == std::vector<std::uintptr_t>{101} && released == stopped &&
        decals_released == std::vector<std::uint32_t>{202}, "pre-teardown releases on the transition thread");
    before_level_transition(24);
    check(released.size() == 1 && decals_released.size() == 1, "repeated teardown releases each reference once");
}
void ordinary_off_thread_cleanup_keeps_references_until_drained() {
    fixture();
    std::thread worker([] { release("owner", 1); clear("intruder"); clear("owner"); });
    worker.join();
    check(state().entries.size() == 2 && released.empty() && decals_released.empty(), "off-thread release retains native references");
    check(live("owner").empty(), "retired references no longer consume the VM's quota");
    tick(0, 0, false, false, false);
    check(released == std::vector<std::uintptr_t>{101} && decals_released == std::vector<std::uint32_t>{202},
        "the next tick drains retired references");
}
}
int main() {
    try {
        world_replacement_releases_surviving_services();
        world_teardown_skips_invalid_decal_realms();
        cleanup_checks_the_realm_before_the_next_tick();
        transition_on_another_thread_finishes_cleanup();
        ordinary_off_thread_cleanup_keeps_references_until_drained();
        creation_checks_the_actual_asset_owner();
        std::cout << "effect lifetime tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
