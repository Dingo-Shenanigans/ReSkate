// Actual owned_bodies::prepare() over the existing constructed native/TLS
// fixture. A seeded hooked flag is not a retail hook-success witness.
bool prepare_slots_empty() {
    return std::none_of(service.slots.begin(), service.slots.end(), [](const Slot& slot) { return slot.attempted; });
}
void prepare_cold_queues_only_deferred_setup() {
    Fixture f;
    service.slots = {};
    service.hooked = false;
    service.hook_attempted = false;
    service.initialization_requested = false;
    service.original = nullptr;
    service.api = {};
    test_facts::forbidden_links = 0;
    const auto generation = service.generation, world_generation = service.world_generation;
    const auto shared_clock_serial = clock_feed.serial;
    check(shared_clock_serial != 0, "fixture did not seed a shared read-only clock");
    std::string foreign;
    std::thread other([&] { foreign = prepare(); });
    other.join();
    check(foreign == "owned bodies require the current local client phase outside multiplayer" &&
        !service.initialization_requested, "foreign thread queued body preparation");
    test_facts::multiplayer_active = true;
    check(!prepare().empty() && !service.initialization_requested, "multiplayer queued body preparation");
    test_facts::multiplayer_active = false;
    service.eligible = false;
    check(!prepare().empty() && !service.initialization_requested, "ineligible client phase queued body preparation");
    service.eligible = true;
    SetLastError(0x1234);
    check(prepare() == "native body producer setup pending; call prepare again explicitly" &&
        service.initialization_requested && !service.hook_attempted && !service.hooked &&
        prepare_slots_empty() && !test_facts::mutations && !test_facts::forbidden_links,
        "cold prepare initialized hooks, attempted a body slot or treated a shared clock as producer readiness");
    check(GetLastError() == 0x1234, "cold prepare changed LastError");
    check(prepare() == "native body producer setup pending; call prepare again explicitly" &&
        service.generation == generation && service.world_generation == world_generation &&
        clock_feed.serial == shared_clock_serial && prepare_slots_empty() && !test_facts::mutations,
        "repeated native preparation reserved an identity, changed the clock or allocated");
    tick(f.base, f.client, false);
    check(!service.initialization_requested && !service.hook_attempted && prepare_slots_empty(),
        "ineligible tick kept or ran stale setup");
    tick(f.base, f.client, true);
    check(!service.initialization_requested && !service.hook_attempted && !test_facts::forbidden_links &&
        prepare_slots_empty(), "phase restoration silently requeued preparation");
}
void prepare_deferred_failure_is_terminal_without_retry() {
    Fixture f;
    service.slots = {};
    service.hooked = false;
    service.hook_attempted = false;
    service.initialization_requested = false;
    service.original = nullptr;
    service.api = {};
    test_facts::forbidden_links = 0;
    check(prepare() == "native body producer setup pending; call prepare again explicitly",
        "explicit preparation did not queue generic setup");
    test_facts::inspect_initializer_lock = true;
    test_facts::initializer_unlocked = false;
    test_facts::initializer_elapsed = 251;
    const auto requested_at = test_clock::now.load();
    SetLastError(0x5678);
    tick(f.base, f.client, true);
    test_facts::inspect_initializer_lock = false;
    test_facts::initializer_elapsed = 0;
    check(GetLastError() == 0x5678 && service.hook_attempted && !service.hooked &&
        !service.initialization_requested && test_facts::forbidden_links == 1 && test_facts::initializer_unlocked &&
        prepare_slots_empty() && !test_facts::mutations,
        "deferred preparation did not use the one fail-closed existing initializer outside the service lock");
    check(service.tick_at == requested_at && test_clock::now == requested_at + 251 && !eligible(),
        "slow setup disguised old client readiness as fresh");
    tick(f.base, f.client, true);
    check(prepare() == "native body ABI or producer hook unavailable; another producer may own it" &&
        !service.initialization_requested && prepare_slots_empty(), "failed setup was reported ready or requeued");
    tick(f.base, f.client, true);
    check(prepare() == "native body ABI or producer hook unavailable; another producer may own it" &&
        test_facts::forbidden_links == 1 && prepare_slots_empty() && !test_facts::mutations,
        "terminal unavailable preparation silently retried hooks or body work");
}
void prepare_warm_readiness_needs_fresh_capture_and_clock() {
    Fixture f;
    service.slots = {};
    service.hook_attempted = true;
    service.initialization_requested = false;
    test_facts::forbidden_links = 0;
    const auto generation = service.generation, world_generation = service.world_generation;
    const auto serial = clock_feed.serial;
    const auto body_reads = test_facts::body_calls.load(), shape_reads = test_facts::shape_calls.load();
    SetLastError(0x9876);
    check(prepare().empty() && GetLastError() == 0x9876 && prepare_slots_empty() && !service.initialization_requested,
        "constructed ready producer could not report fresh local readiness without a body");
    check(service.generation == generation && service.world_generation == world_generation && clock_feed.serial == serial &&
        test_facts::body_calls == body_reads && test_facts::shape_calls == shape_reads && !test_facts::mutations,
        "ready preparation sampled/published a body, reserved identities or changed the clock");
    clock_feed.serial = 0;
    clock_feed.sequence = 0;
    check(prepare() == "waiting for a fresh qualified 60 Hz physics step" && !service.initialization_requested &&
        prepare_slots_empty(), "hooked producer without a fresh clock reported ready or queued an allocation");
    f.step();
    check(prepare().empty(), "fresh constructed physics step did not restore sampled readiness");
    put(f.client + r::client_phase, std::uint32_t{1});
    check(prepare() == "waiting for a fresh qualified 60 Hz physics step" && !service.initialization_requested,
        "foreign native client phase reported ready");
    put(f.client + r::client_phase, std::uint32_t{13});
    put(f.context + test_world_field, std::uintptr_t{});
    check(prepare() == "waiting for a fresh qualified 60 Hz physics step" && !service.initialization_requested,
        "lost local world chain reported ready");
    put(f.context + test_world_field, f.world);
    std::string foreign;
    std::thread other([&] { foreign = prepare(); });
    other.join();
    check(foreign == "owned bodies require the current local client phase outside multiplayer",
        "foreign thread observed client readiness");
    test_facts::multiplayer_active = true;
    check(prepare() == "owned bodies require the current local client phase outside multiplayer",
        "multiplayer observed local-only producer readiness");
    test_facts::multiplayer_active = false;
    test_clock::now += 250;
    check(prepare() == "owned bodies require the current local client phase outside multiplayer" &&
        prepare_slots_empty() && !service.initialization_requested && !test_facts::mutations,
        "stale client tick reported ready or queued body work");
}
void prepare_does_not_reconcile_world_or_touch_existing_slots() {
    Fixture f;
    service.hook_attempted = true;
    service.initialization_requested = false;
    test_facts::forbidden_links = 0;
    auto& slot = f.slot();
    const auto original = slot;
    service.world += 64;
    service.state += 64;
    service.context += 64;
    const auto world = service.world, state = service.state, context = service.context, generation = service.world_generation;
    check(prepare().empty(), "fresh local readiness unexpectedly depended on a retained slot or cached world reconciliation");
    check(service.world == world && service.state == state && service.context == context && service.world_generation == generation,
        "prepare reconciled worlds instead of leaving that to the existing tick/create lifecycle");
    check(slot.attempted == original.attempted && slot.active == original.active && slot.retained == original.retained &&
        slot.quarantined == original.quarantined && slot.body == original.body && slot.made == original.made &&
        slot.handle == original.handle && slot.mailbox.count == original.mailbox.count && slot.mailbox.sequence == original.mailbox.sequence &&
        slot.latest.epoch == original.latest.epoch && slot.latest.position == original.latest.position &&
        !test_facts::mutations && !test_facts::forbidden_links,
        "preparation mutated/released/quarantined the pre-existing fixture body or mailbox");
    check(std::count_if(service.slots.begin(), service.slots.end(), [](const Slot& value) { return value.attempted; }) == 1,
        "preparation attempted another slot alongside the pre-existing constructed one");
}
