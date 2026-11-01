#pragma once
#include "Engine/Game/World/body_query.h"
#include <cstdint>
#include <string>

// Native rigid bodies a plugin VM instance owns (reskate.world.physics), and bounded ray
// batches beside one of them. Each body is a set of capsules in the local client's physics
// world, driven only by measured force, torque and impulse commands produced on the game's
// own body synchronizer. owner is the plugin VM instance, never a plugin name.
namespace dingosdk::owned_bodies {
// Client thread only. Tick grants no plugin ownership and never creates bodies;
// a previously requested producer setup may install generic hooks.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
// Explicit process-lifetime producer preparation, never body creation. Cold
// calls queue only deferred setup and refuse; a later explicit call can report
// a freshly captured local client/clock. No owner, definition or slot is kept.
std::string prepare() noexcept;
// Cold create requests only process-lifetime producer setup on a later eligible
// tick and refuses before allocation. No definition/body request is queued;
// callers must make a separate explicit create after readiness is available.
std::string create(std::uint64_t owner, const owned_body::Definition&, owned_body::Handle&) noexcept;
std::string read(std::uint64_t owner, const owned_body::Handle&, owned_body::Sample&) noexcept;
body_query::Result probe_batch(std::uint64_t owner, const owned_body::Handle&, const body_query::Request&, unsigned allowance) noexcept;
std::string submit(std::uint64_t owner, const owned_body::Handle&, const owned_body::Command&, std::uint64_t& sequence) noexcept;
std::string receipt(std::uint64_t owner, const owned_body::Handle&, std::uint64_t sequence, owned_body::Receipt&) noexcept;
std::string acknowledge(std::uint64_t owner, const owned_body::Handle&, std::uint64_t sequence) noexcept;
std::string release(std::uint64_t owner, const owned_body::Handle&) noexcept;
void release_all(std::uint64_t owner) noexcept;
} // namespace dingosdk::owned_bodies
