#include "Extension/Plugins/plugin_effects.h"
#include "Extension/Rendering/effect_surface.h"
#include "Extension/Rendering/effect_asset.h"
#include "Engine/Game/Settings/transient_count_lease.h"
#include "Engine/Game/Abi/native_data.h"
#include "Extension/Skater/skater_physics.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/skater_body.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace dingosdk {
bool no_bail_available() noexcept { return true; }
bool no_bail_skater(NoBailSkater&) noexcept { return false; }
}
namespace {
using namespace dingosdk;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class T> void put(std::vector<std::byte>& bytes, std::size_t at, const T& value) {
    check(at + sizeof(T) <= bytes.size(), "fixture field outside allocation");
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}
std::uintptr_t address(const std::vector<std::byte>& bytes) { return reinterpret_cast<std::uintptr_t>(bytes.data()); }

void effects_refuse_the_wrong_native_asset_type() {
    // Identity words read from the supported executable and confirmed on the
    // loaded private assets. Computing these with native_name_hash hid the bug.
    std::uint32_t record = 0xf258c6f9;
    const auto type = reinterpret_cast<std::uintptr_t>(&record);
    std::vector<std::byte> asset(32);
    put(asset, addr::effects::asset_type, reinterpret_cast<std::uintptr_t>(&type));
    check(effects::asset_type_matches(address(asset), false), "particle blueprints are accepted");
    check(!effects::asset_type_matches(address(asset), true), "a particle is never treated as a decal");
    record = 0x5de4ae3c;
    check(effects::asset_type_matches(address(asset), true), "decal templates are accepted");
    record = game::native_name_hash("TextureAsset");
    check(!effects::asset_type_matches(address(asset), true) && !effects::asset_type_matches(address(asset), false),
        "a private name cannot make a texture into an effect");
    record = game::native_name_hash("EffectBlueprint");
    check(!effects::asset_type_matches(address(asset), false), "a native function-name hash is not an asset type identity");
    put(asset, addr::effects::asset_type, std::uintptr_t{});
    check(!effects::asset_type_matches(address(asset), false) && !effects::asset_type_matches(0, true),
        "missing reflection fails closed");
}

void effects_only_resolve_the_level_wide_compartment() {
    effects::AssetLookup lookup;
    std::map<std::uint16_t, std::uintptr_t> loaded;
    loaded[7] = 1; // A valid private name in a streamed sublevel.
    unsigned calls{};
    const auto find = [&](std::uint16_t domain) -> std::uintptr_t {
        check(domain == addr::effects::resident_compartment, "lookup never searches streamed compartments");
        ++calls;
        const auto it = loaded.find(domain);
        return it == loaded.end() ? 0 : it->second;
    };
    check(lookup.resolve(0, find) == 0 && calls == 1, "a streamed blueprint is refused even when its name is declared resident");
    loaded[addr::effects::resident_compartment] = 2;
    calls = 0;
    check(lookup.resolve(1999, find) == 0 && calls == 0, "missing assets keep the bounded lookup retry");
    check(lookup.resolve(2000, find) == 2, "the level-wide copy is selected over a same-named streamed copy");
    calls = 0;
    check(lookup.resolve(2001, find) == 2 && calls == 1, "a previously found asset is looked up again before every use");
    loaded[addr::effects::resident_compartment] = 3;
    check(lookup.resolve(2002, find) == 3, "a replacement never revives the old pointer");
    loaded.erase(addr::effects::resident_compartment);
    check(lookup.resolve(2003, find) == 0, "losing the resident copy does not fall back to a streamed copy");
    calls = 0;
    check(lookup.resolve(4002, find) == 0 && calls == 0, "a lost resident copy is retried at a bounded rate");
    loaded[addr::effects::resident_compartment] = 4;
    check(lookup.resolve(4003, find) == 4, "a newly loaded resident copy becomes available again");
}

void geometry_is_from_the_local_physics_step() {
    namespace b = addr::skater_body;
    namespace s = addr::skater_state;
    std::vector<std::byte> rig(0x5000), physics(0x2000), bodies(26 * 0x130), contacts(0x2000), core(0x500), context(0x2000);
    constexpr std::uintptr_t base = 0x140000000;
    put(rig, s::rig_physics_offset, address(physics));
    put(physics, 0, base + s::rig_physics_vtable);
    put(physics, s::physics_bodies_offset, address(bodies));
    put(physics, b::contact_struct_offset, address(contacts));
    put(bodies, 0, s::rig_body_count);
    put(context, s::physics_state_offset, std::uint32_t{300});
    const plugins::EffectMatrix transform{1,0,0,0,0,1,0,0,0,0,1,0,2,.15f,4,1};
    const game::Vec3 velocity{5,-12,2};
    for (std::size_t i = 1; i < skater_body::count; ++i) {
        put(bodies, i*s::physics_body_size+s::body_owner_offset, address(physics));
        put(bodies, i*s::physics_body_size+b::body_transform_offset, transform);
        put(bodies, i*s::physics_body_size+s::body_velocity_offset, velocity);
    }
    put(contacts, b::bone_touching_offset+1, std::uint8_t{1});
    const auto record = b::bone_records_offset+b::bone_record_size;
    put(contacts, record+b::bone_peak_offset, 7.0f);
    put(contacts, record+b::bone_tracked_peak_offset, 12.0f);
    put(contacts, record+b::bone_normal_offset, std::array<float,4>{1,0,0,0});
    put(contacts, record+b::bone_normal_offset+b::bone_tracked_offset, std::array<float,4>{0,1,0,0});
    put(contacts, record+b::bone_point_offset, std::array<float,4>{2,0,4,0});
    put(contacts, record+b::bone_hit_vehicle_offset, std::uint8_t{1});
    NoBailSkater skater{base, 0, address(core), address(context), address(rig)};
    auto sample = skater_physics::read_step(skater, .05f, true);
    const auto& c = sample.body.bodies[1];
    check(c.touching && c.impact == 12 && c.hit.vehicle, "the largest contact and receiver kind survive");
    check(c.normal == game::Vec3{0,1,0} && c.point == game::Vec3{2,0,4}, "normal comes from the same tracked peak, point from the verified field");
    check(sample.poses[1] && sample.poses[1]->transform == transform && sample.poses[1]->velocity == velocity, "poses and velocities are copied");
    check(sample.poses[23].has_value() && !sample.body.bodies[23].touching, "airborne body poses remain available for attached emitters");
    check(sample.state == 300u, "the transition into a bail retains its selector state");
    put(bodies, s::physics_body_size+s::body_owner_offset, std::uintptr_t{});
    put(contacts, record+b::bone_normal_offset+b::bone_tracked_offset, std::array<float,4>{std::numeric_limits<float>::quiet_NaN(),0,0,0});
    sample = skater_physics::read_step(skater, .05f, true);
    check(!sample.poses[1] && sample.poses[23], "a broken owner chain is never published as a body pose");
    check(sample.body.bodies[1].touching && !sample.body.bodies[1].normal, "unknown contact geometry keeps the raw touching evidence");
    put(bodies, 0, std::uint32_t{24});
    sample = skater_physics::read_step(skater, .05f, true);
    check(!sample.poses[23], "a different physics layout fails closed");
}
void projections_follow_receivers() {
    using namespace effects;
    Matrix pose{};
    check(surface::pose({0,.70710678f,0,.70710678f},{10,2,3,0},pose), "a receiver's quaternion makes a rigid pose");
    const Matrix decal{.3f,0,0,0,0,.08f,0,0,0,0,.7f,0,11,2,4,1};
    const auto local = surface::local(pose,decal);
    const auto original = surface::world(pose,local);
    for (std::size_t i = 0; i < 16; ++i) check(std::abs(original[i]-decal[i])<.0001f, "projection round trip preserves its shape");
    pose[12]+=5;
    const auto moved = surface::world(pose,local);
    check(std::abs(moved[12]-16)<.0001f && surface::changed(original,moved), "the stain follows a moved vehicle");
    check(!surface::pose({0,0,0,0},{0,0,0,0},pose), "invalid receiver rotations are refused");
    check(plugins::valid_effect_matrix(decal), "a thin projector is permitted");
    auto invalid=decal; invalid[12]=100001;
    check(!plugins::valid_effect_matrix(invalid), "effects cannot leave the bounded world");
    invalid=decal; invalid[5]=0;
    check(!plugins::valid_effect_matrix(invalid), "zero depth is refused");
}
void draw_allowance_restores_only_its_own_setting() {
    CountSettingSample value{1,2,3,120};
    unsigned writes{};
    const auto read=[&]() -> std::optional<CountSettingSample> { return value; };
    const auto write=[&](const CountSettingSample& expected, std::uint32_t next) {
        if (value!=expected) return false;
        ++writes;
        value.value=next; return true;
    };
    TransientCountLease lease;
    check(lease.update(1,read,write) && value.value==121, "one decal adds exactly one slot");
    const auto first_write = writes;
    check(lease.update(1,read,write) && writes==first_write, "an unchanged count never resizes the render buffers");
    check(lease.update(7,read,write) && value.value==127, "growing demand adds to the original count, not the raised value");
    check(lease.update(2,read,write) && value.value==122, "shrinking demand releases unused allowance");
    check(lease.update(2048,read,write) && value.value==2168, "decal allowance is additive");
    check(lease.restore(read,write) && value.value==120, "disabling restores the renderer's original budget");
    check(lease.update(2048,read,write), "the allowance can be reacquired");
    value.value=4000;
    check(!lease.update(2048,read,write), "an external settings edit relinquishes ownership");
    check(lease.restore(read,write) && value.value==4000, "cleanup does not overwrite the player's later setting");
    check(!lease.update(4097,read,write), "the temporary allowance is bounded");
    value.value=65000;
    check(lease.update(500,read,write) && value.value==65500, "an allowance near the limit fits");
    check(lease.update(400,read,write) && value.value==65400, "shrinking near the limit uses the original baseline");
    value.manager=4;
    check(!lease.update(1,read,write), "a replaced settings manager relinquishes ownership");
    check(lease.restore(read,write) && value.value==65400, "a replacement is never restored using the previous manager's baseline");
    value.value=65000;
    check(!lease.update(4096,read,write), "an allowance cannot overflow the native count limit");
    value.value=200;
    check(lease.update(1,read,write) && value.value==201, "a refused allowance never captures a stale baseline");
}
}
int main() {
    try {
        effects_refuse_the_wrong_native_asset_type(); effects_only_resolve_the_level_wide_compartment();
        geometry_is_from_the_local_physics_step();
        projections_follow_receivers(); draw_allowance_restores_only_its_own_setting();
        std::cout << "effect geometry tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
