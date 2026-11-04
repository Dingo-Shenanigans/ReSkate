// Re-activating a parked owned body (create with reuse_own_parked) over the actual Service.
// Every native setter here is a recording stand-in that also makes its effect on the constructed
// arrays: shape masks and owners, the dynamic bit, the velocity and pose rings, pending force and
// torque, the mass and inertia arrays, and a new body's or capsule's starting values. So the
// actual park, pool and witness checks pass or fail on what was written, and the order of writes
// is the call log. The stand-ins log to reuse_native, never to test_facts::mutations. None of this
// proves what the retail functions do with a body that was used before.
struct ReuseNative {
    std::uintptr_t state{}, world{};
    std::vector<std::string> calls;
    std::string fail_on;                           // the one call that faults, as a crashing setter would
    std::function<void(const std::string&)> after; // runs after each recorded call, before any fault
    std::uint32_t bodies{}, shapes{};              // body and shape indices already in use
};
ReuseNative reuse_native;
// Records the call and says whether it should fault. Nothing here outlives the return, so the
// caller raises the fault with no C++ object left to unwind.
bool reuse_call(const char* kind, std::uint32_t index, std::int64_t value = -1) {
    auto call = std::string(kind) + ' ' + std::to_string(index);
    if (value >= 0) call += ' ' + std::to_string(value);
    reuse_native.calls.push_back(call);
    if (reuse_native.after) reuse_native.after(call);
    if (reuse_native.fail_on.empty() || call != reuse_native.fail_on) return false;
    reuse_native.fail_on.clear();
    return true;
}
void reuse_fault(bool fault) { if (fault) RaiseException(0xE0B0D1E5, 0, 0, nullptr); }
std::uintptr_t reuse_array(std::uintptr_t offset) {
    std::uintptr_t at{};
    return peek(reuse_native.state + offset, at) ? at : 0;
}
template<class T> void reuse_put(std::uintptr_t offset, std::uint32_t index, const T& value) {
    if (const auto at = reuse_array(offset)) put(at + std::uintptr_t{index} * sizeof(T), value);
}
void reuse_ring(std::uintptr_t offset, std::uint32_t index, const V4& value) {
    Ring ring{};
    if (peek(reuse_native.state + offset, ring) && ring.values) put(ring.values + std::uintptr_t{index} * sizeof(V4), value);
}
V4 reuse_v4(const float* v) { return {v[0], v[1], v[2], v[3]}; }
void reuse_motion(NativeHandle* h, int type) {
    std::uint32_t bits{};
    if (const auto dynamics = reuse_array(r::dynamic_bits); dynamics && peek(dynamics, bits))
        put(dynamics, type == 3 ? bits | (1U << h->index) : bits & ~(1U << h->index));
    reuse_fault(reuse_call("motion", h->index, type));
}
void reuse_inverse_mass(NativeHandle* h, float value) {
    reuse_put(r::inverse_mass_array, h->index, value);
    reuse_fault(reuse_call("inverse_mass", h->index));
}
void reuse_clear(NativeHandle* h) { reuse_fault(reuse_call("clear", h->index)); }
void reuse_wake(NativeHandle* h) { reuse_fault(reuse_call("wake", h->index)); }
void reuse_categories(NativeHandle* h, const std::uint32_t* value) {
    reuse_put(r::shape_categories, h->index, *value);
    reuse_fault(reuse_call("categories", h->index, *value));
}
void reuse_collides(NativeHandle* h, const std::uint32_t* value) {
    reuse_put(r::shape_collides, h->index, *value);
    reuse_fault(reuse_call("collides", h->index, *value));
}
void reuse_owner(NativeHandle* h, const NativeHandle* value) {
    reuse_put(pb::body_owner_offset, h->index, value->index);
    reuse_fault(reuse_call("owner", h->index));
}
void reuse_com(NativeHandle* h, const float* v) { reuse_put(r::local_com, h->index, reuse_v4(v)); reuse_fault(reuse_call("com", h->index)); }
void reuse_inertia(NativeHandle* h, const float* v) {
    reuse_put(r::inertia_ratio_array, h->index, reuse_v4(v));
    reuse_fault(reuse_call("inertia", h->index));
}
void reuse_transform(NativeHandle* h, const float* pose) {
    // Pose is rotation XYZW then position; the rotation ring holds W first.
    reuse_ring(r::position_ring, h->index, V4{pose[4], pose[5], pose[6], 0});
    reuse_ring(r::rotation_ring, h->index, V4{pose[3], pose[0], pose[1], pose[2]});
    reuse_fault(reuse_call("transform", h->index));
}
void reuse_linear(NativeHandle* h, const float* v) { reuse_ring(r::velocity_ring, h->index, reuse_v4(v)); reuse_fault(reuse_call("linear", h->index)); }
void reuse_angular(NativeHandle* h, const float* v) { reuse_ring(r::spin_ring, h->index, reuse_v4(v)); reuse_fault(reuse_call("angular", h->index)); }
void reuse_force(NativeHandle* h, const float* v) { reuse_put(r::pending_force, h->index, reuse_v4(v)); reuse_fault(reuse_call("force", h->index)); }
void reuse_torque(NativeHandle* h, const float* v) { reuse_put(r::pending_torque, h->index, reuse_v4(v)); reuse_fault(reuse_call("torque", h->index)); }
void reuse_clear_force(NativeHandle* h, const float* v) {
    reuse_put(r::pending_force, h->index, reuse_v4(v));
    reuse_fault(reuse_call("clear_force", h->index));
}
void reuse_clear_torque(NativeHandle* h, const float* v) {
    reuse_put(r::pending_torque, h->index, reuse_v4(v));
    reuse_fault(reuse_call("clear_torque", h->index));
}
std::uint32_t reuse_free(std::uint32_t used) {
    std::uint32_t index{};
    while (index < capacity && (used & (1U << index))) ++index;
    return index;
}
// A new body has what create_native expects of one: allocated, a current generation, unit mass
// and inertia ratio, an identity frame, and no motion or pending force.
NativeHandle* reuse_allocate(std::uintptr_t world, NativeHandle* out) {
    const auto index = reuse_free(reuse_native.bodies);
    if (index == capacity || reuse_call("allocate", index)) return nullptr;
    reuse_native.bodies |= 1U << index;
    std::uint32_t bits{};
    const auto allocations = reuse_array(r::allocated_bits);
    std::uintptr_t generations{};
    if (!peek(allocations, bits) || !peek(world + r::world_body_generations, generations)) return nullptr;
    put(allocations, bits | (1U << index));
    put(generations + std::uintptr_t{index} * sizeof(std::uint32_t), std::uint32_t{1});
    reuse_put(r::inertia_ratio_array, index, V4{1, 1, 1, 0});
    reuse_put(r::local_rotation, index, V4{1, 0, 0, 0});
    reuse_put(r::local_com, index, V4{});
    reuse_put(r::inverse_mass_array, index, 1.f);
    reuse_put(r::pending_force, index, V4{});
    reuse_put(r::pending_torque, index, V4{});
    reuse_ring(r::position_ring, index, V4{});
    reuse_ring(r::rotation_ring, index, V4{1, 0, 0, 0});
    reuse_ring(r::velocity_ring, index, V4{});
    reuse_ring(r::spin_ring, index, V4{});
    *out = {world, index, 1};
    return out;
}
// A new capsule belongs to its body, with an owner and masks create_native must overwrite.
NativeHandle* reuse_capsule(std::uintptr_t world, NativeHandle* shape, const NativeHandle* body, const float*, const float*, float) {
    const auto index = reuse_free(reuse_native.shapes);
    if (index == capacity || reuse_call("capsule", index)) return nullptr;
    reuse_native.shapes |= 1U << index;
    std::uint32_t bits{};
    const auto allocations = reuse_array(r::shape_allocated_bits);
    if (!peek(allocations, bits)) return nullptr;
    put(allocations, bits | (1U << index));
    reuse_put(r::shape_generations, index, std::uint8_t{1});
    reuse_put(r::shape_body_indices, index, body->index);
    reuse_put(pb::body_owner_offset, index, std::uint32_t{0x2a});
    reuse_put(r::shape_categories, index, std::uint32_t{0x55});
    reuse_put(r::shape_collides, index, std::uint32_t{0x33});
    *shape = {world, index, 1};
    return shape;
}

// What one re-activation of a body with one capsule writes: park() again, then the end of a
// creation. No allocate, capsule, inverse mass, inertia, clear, centre of mass or owner call.
std::vector<std::string> reactivation_writes_of(std::uint32_t body, std::uint32_t shape) {
    const auto b = std::to_string(body), s = std::to_string(shape);
    return {"categories " + s + " 0", "collides " + s + " 0", "linear " + b, "angular " + b, "motion " + b + " 2",
        "transform " + b, "collides " + s + " 257", "motion " + b + " 3", "wake " + b};
}
// The seeded body is index 3 with shape 5.
const std::vector<std::string> reactivation_writes = reactivation_writes_of(body_index, shape_index);
// What a creation of one capsule writes, in create_native's order.
std::vector<std::string> allocation_writes(std::uint32_t body, std::uint32_t shape) {
    const auto b = std::to_string(body), s = std::to_string(shape);
    return {"allocate " + b, "motion " + b + " 2", "inverse_mass " + b, "inertia " + b, "clear " + b, "com " + b,
        "capsule " + s, "owner " + s, "categories " + s + " 0", "collides " + s + " 0", "transform " + b,
        "collides " + s + " 257", "motion " + b + " 3", "wake " + b};
}
void check_calls(const std::vector<std::string>& expected, const char* why) {
    if (reuse_native.calls == expected) return;
    std::cerr << "native calls were:";
    for (const auto& call : reuse_native.calls) std::cerr << " [" << call << ']';
    std::cerr << '\n';
    check(false, why);
}
void check_case(bool value, const char* name, const char* why) {
    if (!value) std::cerr << "case " << name << ": ";
    check(value, why);
}
// Whether any recorded call named the seeded body or its shape.
bool reuse_touched_seeded() {
    for (const auto& call : reuse_native.calls) {
        const auto space = call.find(' ');
        const auto kind = call.substr(0, space);
        const bool shape = kind == "categories" || kind == "collides" || kind == "owner" || kind == "capsule";
        if (std::stoul(call.substr(space + 1)) == (shape ? shape_index : body_index)) return true;
    }
    return false;
}
bool no_slot_beyond(std::size_t used) {
    return std::none_of(service.slots.begin() + static_cast<std::ptrdiff_t>(used), service.slots.end(), [](const Slot& s) { return s.attempted; });
}

class ReuseFixture : public Fixture {
public:
    std::uintptr_t pending_force{}, pending_torque{}, shape_bodies{};
    // The seeded body's own definition at a new pose: what a caller asks to bring back.
    Definition definition{};
    ReuseFixture() {
        pending_force = arena.take(capacity * sizeof(V4));
        pending_torque = arena.take(capacity * sizeof(V4));
        shape_bodies = arena.take(capacity * 4);
        put(state + r::pending_force, pending_force);
        put(state + r::pending_torque, pending_torque);
        put(state + r::shape_body_indices, shape_bodies);
        put(shape_bodies + shape_index * 4, body_index);
        reuse_native = {};
        reuse_native.state = state;
        reuse_native.world = world;
        reuse_native.bodies = 1U << body_index;
        reuse_native.shapes = 1U << shape_index;
        auto& a = service.api;
        a.allocate = reuse_allocate;
        a.capsule = reuse_capsule;
        a.motion = reuse_motion;
        a.inverse_mass = reuse_inverse_mass;
        a.clear = reuse_clear;
        a.wake = reuse_wake;
        a.categories = reuse_categories;
        a.collides = reuse_collides;
        a.owner = reuse_owner;
        a.com = reuse_com;
        a.transform = reuse_transform;
        a.linear = reuse_linear;
        a.angular = reuse_angular;
        a.force = reuse_force;
        a.torque = reuse_torque;
        a.inertia = reuse_inertia;
        a.clear_force = reuse_clear_force;
        a.clear_torque = reuse_clear_torque;
        auto& s = slot();
        // As create() leaves a body it allocated: every constructor confirmed, activated in
        // this world generation.
        s.allocation = {true, 1, 1};
        s.active_world = service.world_generation;
        definition = s.definition;
        definition.position = {4, 5, 6};
        definition.rotation = {0, 0, .6f, .8f};
        check(pool_current(s, true), "reuse fixture is not an active, current pool");
    }
    ~ReuseFixture() { reuse_native = {}; }
    // Parks the seeded body through the actual release(), then lets native steps pass.
    void park_slot(unsigned steps = 2) {
        const auto parked_at = clock_feed.sequence.load();
        check(release(owner, handle).empty(), "seeded body release failed");
        check_calls({"categories 5 0", "collides 5 0", "clear_force 3", "clear_torque 3", "linear 3", "angular 3", "motion 3 2"},
            "release did not park the seeded body in park()'s order");
        const auto& s = slot();
        check(s.retained && !s.active && !s.quarantined && !s.reuse_barred && s.parked_world == service.world_generation &&
            s.parked_serial == parked_at, "a clean park was not stamped with its world generation and step");
        for (unsigned i = 0; i < steps; ++i) step();
        reuse_native.calls.clear();
    }
    std::string reuse(Handle& out, CreateReport& report) { return create(owner, definition, out, true, report); }
};

// T2 a: the caller's own parked body comes back with only park() and arm()'s writes, under a
// new generation that leaves every old handle dead.
void reuse_reactivates_with_park_and_arm_writes_only() {
    ReuseFixture f;
    f.park_slot();
    Handle out;
    CreateReport report;
    SetLastError(0x2468);
    check(f.reuse(out, report).empty(), "the owner's own parked body was not re-activated");
    check(GetLastError() == 0x2468, "re-activation changed LastError");
    check(report.origin == Origin::reused && report.miss == ReuseMiss::none, "re-activation reported an allocation or a miss");
    check_calls(reactivation_writes, "re-activation wrote anything but park() and the end of a creation");
    check(out.slot == f.handle.slot && out.generation > f.handle.generation && out.generation == service.generation,
        "re-activation moved the slot or kept the old generation");
    const auto& s = f.slot();
    check(s.active && !s.retained && !s.quarantined && !s.reuse_barred && !s.creation_failed && !s.mailbox.failed &&
        s.handle == out && s.parked_world == 0 && s.parked_serial == 0 && s.active_world == service.world_generation,
        "re-activation bookkeeping is not that of an active body");
    check(no_slot_beyond(1), "re-activation reserved another slot");
    Sample sample;
    check(read(owner, out, sample).empty() && sample.epoch.body == out.generation && sample.epoch.world == service.world_generation,
        "the re-activated body could not be read under its new handle");
    check(sample.position == Vec{4, 5, 6} && sample.rotation == Quaternion{0, 0, .6f, .8f} && sample.velocity == Vec{} &&
        sample.angular_velocity == Vec{}, "re-activation did not start at the asked-for pose and at rest");
    const auto old = f.handle;
    Sample stale;
    check(read(owner, old, stale) == "body owner or generation unavailable", "the old handle still reads");
    Command command;
    command.epoch = sample.epoch;
    command.force = {0, 10, 0};
    std::uint64_t sequence{};
    check(submit(owner, old, command, sequence) == "body owner or generation changed" && !sequence, "the old handle still submits");
    Receipt record;
    check(receipt(owner, old, 0, record) == "body owner or generation unavailable", "the old handle still has receipts");
    check(!acknowledge(owner, old, 44).empty(), "the old handle still acknowledges");
    check(release(owner, old) == "body owner or generation unavailable", "the old handle still releases");
    check(submit(owner, out, command, sequence).empty() && sequence, "the re-activated body kept park()'s failed mailbox");
}

// T2 b and l: another owner (another VM, or this plugin after a reload) never gets the body, and
// is told only about its own slots. The process keeps eight native allocations; the owner can
// still bring its own body back when all eight are used.
void reuse_never_crosses_owners_and_keeps_the_eight_slot_cap() {
    constexpr std::uint64_t other = owner + 1;
    {
        ReuseFixture f;
        f.park_slot();
        Handle out;
        CreateReport report;
        check(create(other, f.definition, out, true, report).empty() && report.origin == Origin::allocated &&
            report.miss == ReuseMiss::none_parked && out.slot == 1, "another owner did not allocate past a parked body it does not own");
        check_calls(allocation_writes(0, 0), "a creation no longer writes in create_native's order");
        check(f.slot().owner == owner && f.slot().handle == f.handle && f.slot().retained && !f.slot().active,
            "another owner's create changed the parked body");
    }
    {
        ReuseFixture f;
        f.park_slot();
        // Another owner's own quarantined body is the only one its miss can describe.
        auto& mine = service.slots[1];
        mine = f.slot();
        mine.owner = other;
        mine.handle = {50, 1};
        mine.quarantined = true;
        Handle out;
        CreateReport report;
        check(create(other, f.definition, out, true, report).empty() && report.miss == ReuseMiss::quarantined && out.slot == 2 &&
            !reuse_touched_seeded() && f.slot().handle == f.handle, "a miss described a body the caller does not own");
    }
    {
        ReuseFixture f;
        f.park_slot();
        for (std::size_t i = 1; i < service.slots.size(); ++i) {
            auto& s = service.slots[i];
            s.attempted = s.retained = s.quarantined = true;
            s.owner = 303;
            s.handle = {100 + i, static_cast<std::uint32_t>(i)};
        }
        const std::string exhausted = "the eight retained body slots are exhausted; native destruction is unproven";
        Handle out;
        CreateReport report;
        check(create(other, f.definition, out) == exhausted && !out.generation, "a ninth plain allocation was not refused");
        check(create(other, f.definition, out, true, report) == exhausted && !out.generation && report.miss == ReuseMiss::none_parked,
            "another owner's reuse reached a parked body it does not own");
        check(reuse_native.calls.empty() && f.slot().owner == owner && f.slot().handle == f.handle && f.slot().retained,
            "a refused create touched the parked body");
        check(f.reuse(out, report).empty() && report.origin == Origin::reused && out.slot == 0,
            "with every slot used the owner could not bring back its own body");
    }
}

// T2 c: each rule a candidate must meet, broken alone, writes nothing to it and names the miss.
void reuse_disqualifiers_write_nothing_and_name_the_miss() {
    using Change = void (*)(Slot&, Definition&);
    struct Case { const char* name; Change change; ReuseMiss miss; };
    const Case variants[] = {
        {"quarantined", [](Slot& s, Definition&) { s.quarantined = true; }, ReuseMiss::quarantined},
        {"barred", [](Slot& s, Definition&) { s.reuse_barred = true; }, ReuseMiss::quarantined},
        {"no clean park", [](Slot& s, Definition&) { s.parked_world = 0; }, ReuseMiss::quarantined},
        {"unconfirmed constructor", [](Slot& s, Definition&) { ++s.allocation.shapes_attempted; }, ReuseMiss::quarantined},
        {"unconfirmed body", [](Slot& s, Definition&) { s.allocation.body_confirmed = false; }, ReuseMiss::quarantined},
        {"fewer shapes made", [](Slot& s, Definition&) { s.made = 0; }, ReuseMiss::quarantined},
        {"parked in another generation", [](Slot& s, Definition&) { s.parked_world = 22; }, ReuseMiss::world_changed},
        {"activated in another generation", [](Slot& s, Definition&) { s.active_world = 22; }, ReuseMiss::world_changed},
        {"another world", [](Slot& s, Definition&) { s.anchor.world += 64; }, ReuseMiss::world_changed},
        {"another state", [](Slot& s, Definition&) { s.anchor.state += 64; }, ReuseMiss::world_changed},
        {"another context", [](Slot& s, Definition&) { s.anchor.context += 64; }, ReuseMiss::world_changed},
        {"body in another world", [](Slot& s, Definition&) { s.body.world += 64; }, ReuseMiss::world_changed},
        {"count", [](Slot&, Definition& d) { d.count = 2; d.capsules[1] = d.capsules[0]; }, ReuseMiss::definition_differs},
        {"endpoint", [](Slot&, Definition& d) { d.capsules[0].b[1] = 1.5f; }, ReuseMiss::definition_differs},
        {"radius", [](Slot&, Definition& d) { d.capsules[0].radius = .25f; }, ReuseMiss::definition_differs},
        {"mass", [](Slot&, Definition& d) { d.mass = 13; }, ReuseMiss::definition_differs},
        {"inertia", [](Slot&, Definition& d) { d.inertia[2] = 5; }, ReuseMiss::definition_differs},
    };
    for (const auto& c : variants) {
        ReuseFixture f;
        f.park_slot();
        auto d = f.definition;
        c.change(f.slot(), d);
        Handle out;
        CreateReport report;
        check_case(create(owner, d, out, true, report).empty() && report.origin == Origin::allocated && report.miss == c.miss &&
            out.slot == 1, c.name, "a disqualified body did not fall back to allocation with its miss");
        const bool barred = std::string_view(c.name) == "barred";
        check_case(!reuse_touched_seeded() && f.slot().handle == f.handle && f.slot().retained && !f.slot().active &&
            f.slot().reuse_barred == barred, c.name, "a disqualified body was written to or barred");
    }
    {
        // An active body is not parked: nothing to bring back.
        ReuseFixture f;
        Handle out;
        CreateReport report;
        check(create(owner, f.definition, out, true, report).empty() && report.miss == ReuseMiss::none_parked && out.slot == 1 &&
            !reuse_touched_seeded() && f.slot().active && f.slot().handle == f.handle, "an active body was re-activated or touched");
    }
    {
        // A failed creation in this world prohibits reuse there too, before any write or slot.
        ReuseFixture f;
        f.park_slot();
        f.slot().creation_failed = true;
        Handle out;
        CreateReport report;
        check(create(owner, f.definition, out, true, report) == "a previous partial allocation in this world prohibits another native allocation" &&
            !out.generation && reuse_native.calls.empty() && no_slot_beyond(1), "reuse ran in a world a partial creation prohibits");
    }
}

// T2 d: without reuse_own_parked a create never looks at a parked body.
void plain_create_never_reuses() {
    ReuseFixture f;
    f.park_slot();
    Handle out;
    check(create(owner, f.definition, out).empty() && out.slot == 1, "a plain create did not allocate");
    check_calls(allocation_writes(0, 0), "a plain create wrote other than a creation");
    reuse_native.calls.clear();
    CreateReport report{Origin::reused, ReuseMiss::quarantined};
    check(create(owner, f.definition, out, false, report).empty() && out.slot == 2 && report.origin == Origin::allocated &&
        report.miss == ReuseMiss::none, "create without reuse did not allocate and report a plain allocation");
    check_calls(allocation_writes(1, 1), "create without reuse wrote other than a creation");
    check(f.slot().handle == f.handle && f.slot().retained && !f.slot().active, "a plain create touched the parked body");
}

// T2 e: the witness reads; any sign the parked body is no longer as park() left it bars it and
// falls back to allocation, with nothing written to it.
void witness_failures_bar_the_body_and_allocate() {
    using Break = void (*)(ReuseFixture&);
    struct Case { const char* name; Break change; };
    const Case variants[] = {
        {"dynamic bit set", [](ReuseFixture& f) { put(f.dynamics, 1U << body_index); }},
        // A failed read of the dynamic word must not pass for a clear bit.
        {"no dynamic word", [](ReuseFixture& f) { put(f.state + r::dynamic_bits, std::uintptr_t{}); }},
        {"collides with the world", [](ReuseFixture& f) { put(f.collisions + shape_index * 4, r::safe_world_collisions); }},
        {"in a category", [](ReuseFixture& f) { put(f.categories + shape_index * 4, 4U); }},
        {"shape owned", [](ReuseFixture& f) { put(load<std::uintptr_t>(f.state + pb::body_owner_offset) + shape_index * 4, 9U); }},
        {"shape on another body", [](ReuseFixture& f) { put(f.shape_bodies + shape_index * 4, 2U); }},
        {"shape generation", [](ReuseFixture& f) { put(load<std::uintptr_t>(f.state + r::shape_generations) + shape_index, std::uint8_t{12}); }},
        {"body generation", [](ReuseFixture& f) { put(f.body_generations + body_index * 4, body_generation + 1); }},
        {"pending force", [](ReuseFixture& f) { put(f.pending_force + body_index * sizeof(V4), V4{1, 0, 0, 0}); }},
        {"pending torque", [](ReuseFixture& f) { put(f.pending_torque + body_index * sizeof(V4), V4{0, 0, -1, 0}); }},
        {"linear velocity", [](ReuseFixture& f) { put(f.velocity + body_index * sizeof(V4), V4{0, .5f, 0, 0}); }},
        {"linear fourth lane", [](ReuseFixture& f) { put(f.velocity + body_index * sizeof(V4), V4{0, 0, 0, 1}); }},
        {"spin", [](ReuseFixture& f) { put(f.spin + body_index * sizeof(V4), V4{0, 0, .25f, 0}); }},
        {"centre of mass", [](ReuseFixture& f) { put(load<std::uintptr_t>(f.state + r::local_com) + body_index * sizeof(V4), V4{0, .1f, 0, 0}); }},
        {"inverse mass", [](ReuseFixture& f) { put(load<std::uintptr_t>(f.state + r::inverse_mass_array) + body_index * 4, 1.f / 13.f); }},
    };
    for (const auto& c : variants) {
        ReuseFixture f;
        f.park_slot();
        c.change(f);
        Handle out;
        CreateReport report;
        (void)f.reuse(out, report);
        check_case(report.origin == Origin::allocated && report.miss == ReuseMiss::parked_body_changed, c.name,
            "a changed parked body was re-activated or not reported");
        check_case(!reuse_native.calls.empty() && reuse_native.calls.front().starts_with("allocate ") && !reuse_touched_seeded(), c.name,
            "the witness wrote to the parked body or did not fall back to allocation");
        check_case(f.slot().reuse_barred && f.slot().handle == f.handle && f.slot().retained && !f.slot().active, c.name,
            "a failed witness did not bar the parked body");
    }
    {
        // The bar stays: the next request does not read the body again.
        ReuseFixture f;
        f.park_slot();
        put(f.spin + body_index * sizeof(V4), V4{.5f, 0, 0, 0});
        Handle out;
        CreateReport report;
        (void)f.reuse(out, report);
        put(f.spin + body_index * sizeof(V4), V4{});
        reuse_native.calls.clear();
        check(f.reuse(out, report).empty() && report.miss == ReuseMiss::quarantined && !reuse_touched_seeded(),
            "a barred body was read or re-activated again");
    }
    {
        // Spin's fourth lane is not one park() zeroes or checks.
        ReuseFixture f;
        f.park_slot();
        put(f.spin + body_index * sizeof(V4), V4{0, 0, 0, 5});
        Handle out;
        CreateReport report;
        check(f.reuse(out, report).empty() && report.origin == Origin::reused, "a nonzero fourth spin lane refused the parked body");
        check_calls(reactivation_writes, "the fourth spin lane changed what re-activation writes");
    }
}

// T2 f: a fault after re-activation began is treated as a partial creation: the body stays
// quarantined and barred, no handle is returned, and the world allows no more allocation.
void failed_reactivation_quarantines_and_prohibits_allocation() {
    const char* faults[] = {"collides 5 0", "motion 3 2", "transform 3", "collides 5 257", "motion 3 3", "wake 3", nullptr};
    for (const char* fault : faults) {
        const char* name = fault ? fault : "closing pool check";
        ReuseFixture f;
        f.park_slot();
        if (fault) reuse_native.fail_on = fault;
        // A shape put back in a category after the wake fails the last pool check.
        else reuse_native.after = [&f](const std::string& call) { if (call == "wake 3") put(f.categories + shape_index * 4, 7U); };
        Handle out{99, 6};
        CreateReport report;
        check_case(f.reuse(out, report) == "body_reactivation_failed" && out == Handle{}, name, "a failed re-activation was not refused");
        reuse_native.after = {};
        const auto& s = f.slot();
        check_case(s.quarantined && s.reuse_barred && s.creation_failed && s.retained && !s.active && s.parked_world == 0 &&
            s.handle.generation > f.handle.generation, name, "a failed re-activation was not quarantined and barred under a new generation");
        Sample sample;
        check_case(read(owner, f.handle, sample) == "body owner or generation unavailable", name, "the old handle survived a failed re-activation");
        const std::string prohibited = "a previous partial allocation in this world prohibits another native allocation";
        const auto calls = reuse_native.calls.size();
        check_case(create(owner + 1, f.definition, out) == prohibited && create(owner, f.definition, out, true, report) == prohibited &&
            reuse_native.calls.size() == calls && no_slot_beyond(1), name, "a failed re-activation fell back to or allowed allocation");
    }
}

// T2 g: a re-activated body is kept by the tick, parks again on release, and comes back again.
void reactivated_body_parks_and_returns_again() {
    ReuseFixture f;
    f.park_slot();
    Handle first, second;
    CreateReport report;
    check(f.reuse(first, report).empty(), "first re-activation failed");
    tick(f.base, f.client, true);
    check(f.slot().active && !f.slot().retained && f.slot().handle == first, "the tick retired a re-activated body");
    reuse_native.calls.clear();
    check(release(owner, first).empty() && f.slot().retained && !f.slot().quarantined && f.slot().parked_world == service.world_generation,
        "a re-activated body did not park cleanly again");
    f.step();
    f.step();
    reuse_native.calls.clear();
    check(f.reuse(second, report).empty() && report.origin == Origin::reused && second.slot == 0 && second.generation > first.generation,
        "a body parked twice did not come back under a third generation");
    check_calls(reactivation_writes, "a second re-activation wrote differently");
}

// T2 g, for a body the actual create() made: the seeded body's world generation is set by the
// fixture, so this is the case that shows create() stamps the generation a clean park needs.
// Without it every allocated body parks unusable and each reuse falls back to a new slot.
void an_allocated_body_parks_and_comes_back() {
    ReuseFixture f;
    Handle first, again;
    check(create(owner, f.definition, first).empty() && first.slot == 1, "a plain create did not allocate the next slot");
    check_calls(allocation_writes(0, 0), "a creation no longer writes in create_native's order");
    const auto& s = service.slots[1];
    check(s.active && s.active_world == service.world_generation, "create() did not stamp its body's world generation");
    check(release(owner, first).empty() && s.retained && !s.quarantined && !s.reuse_barred && s.parked_world == service.world_generation,
        "a body create() allocated did not park cleanly");
    f.step();
    f.step();
    reuse_native.calls.clear();
    CreateReport report;
    check(create(owner, f.definition, again, true, report).empty() && report.origin == Origin::reused && report.miss == ReuseMiss::none &&
        again.slot == 1 && again.generation > first.generation, "a body create() allocated and release() parked did not come back");
    check_calls(reactivation_writes_of(0, 0), "re-activating an allocated body wrote other than park() and the end of a creation");
    check(no_slot_beyond(2) && !reuse_touched_seeded() && f.slot().active && f.slot().handle == f.handle,
        "re-activating an allocated body reserved a slot or touched the seeded body");
}

// T2 h: a lapse in readiness between the park and the request is a new world generation.
void a_world_generation_change_prevents_reuse() {
    ReuseFixture f;
    f.park_slot();
    const auto generation = service.world_generation;
    tick(f.base, f.client, false);
    tick(f.base, f.client, true);
    check(service.world_generation == generation + 1, "a readiness lapse did not start a new world generation");
    Handle out;
    CreateReport report;
    check(f.reuse(out, report).empty() && report.origin == Origin::allocated && report.miss == ReuseMiss::world_changed && out.slot == 1 &&
        !reuse_touched_seeded(), "a body parked in an earlier world generation was re-activated");
}

// T2 i: a failed setter quarantines the body; a later clean park clears quarantined, as release
// has always reported, but the body stays barred from re-activation.
void a_failed_setter_bars_reuse_after_a_clean_park() {
    ReuseFixture f;
    // The seeded receipt's sequence is the fixture's own, ahead of the service's counter.
    f.slot().mailbox = {};
    Sample sample;
    check(read(owner, f.handle, sample).empty(), "fixture body could not publish a sample");
    Command command;
    command.epoch = sample.epoch;
    command.force = {0, 10, 0};
    std::uint64_t sequence{};
    check(submit(owner, f.handle, command, sequence).empty(), "fixture command was refused");
    f.step();
    reuse_native.fail_on = "force 3";
    NativeHandle incoming = f.slot().body;
    produce(&incoming, f.slot().anchor.wrappers[0]);
    check(f.slot().quarantined && f.slot().reuse_barred && !f.slot().active, "a faulting force setter did not quarantine and bar the body");
    tick(f.base, f.client, true);
    check(f.slot().retained && !f.slot().quarantined, "the tick did not park the body cleanly after the failed setter");
    check(release(owner, f.handle).empty(), "release no longer succeeds after a clean park");
    f.step();
    f.step();
    reuse_native.calls.clear();
    Handle out;
    CreateReport report;
    check(f.reuse(out, report).empty() && report.origin == Origin::allocated && report.miss == ReuseMiss::quarantined && out.slot == 1 &&
        !reuse_touched_seeded(), "a body whose setter failed was re-activated after a clean park");
}

// T2 j: fewer than two native steps since the park asks the caller to wait, with nothing written
// or reserved; the step after, the body comes back.
void a_body_parked_too_recently_is_pending_not_allocated() {
    ReuseFixture f;
    f.park_slot(1);
    const auto generation = service.generation;
    Handle out{5, 5};
    CreateReport report;
    check(f.reuse(out, report) == "body_reuse_pending" && out == Handle{} && reuse_native.calls.empty() && no_slot_beyond(1) &&
        service.generation == generation && f.slot().handle == f.handle && !f.slot().reuse_barred,
        "a body parked one step ago was not refused cleanly as pending");
    f.step();
    check(f.reuse(out, report).empty() && report.origin == Origin::reused && out.slot == 0, "a settled body did not come back");
}

// T2 k: every re-activation write happens under the service lock, so the step's producer cannot
// run between them.
void reactivation_writes_hold_the_service_lock() {
    ReuseFixture f;
    f.park_slot();
    bool checked{}, excluded{};
    reuse_native.after = [&](const std::string& call) {
        if (call != "transform 3") return;
        checked = true;
        excluded = TryAcquireSRWLockExclusive(&service.lock) == 0;
        if (!excluded) ReleaseSRWLockExclusive(&service.lock);
        NativeHandle incoming = f.slot().body;
        produce(&incoming, f.slot().anchor.wrappers[0]);
    };
    Handle out;
    CreateReport report;
    check(f.reuse(out, report).empty() && checked && excluded, "re-activation wrote without holding the service lock");
    reuse_native.after = {};
    check_calls(reactivation_writes, "the producer ran inside a re-activation");
}
