// Actual Host/VM/API over constructed readiness leaf results. No native body,
// hook, clock or preparation success is executed or proved by these cases.
namespace {
void body_prepare_host_fixture_v1() {
    const std::string permission = R"("world.physics")";
    unsigned scenarios{};
    const auto id = [&] { return "body-prepare-" + std::to_string(++scenarios); };
    const auto no_effects = [](const TestEnvironment& e) {
        check(!e.fake.body_create_calls && !e.fake.body_read_calls && !e.fake.body_release_calls &&
            !e.fake.body_submit_entries && !e.fake.body_submissions && e.fake.owned.empty() &&
            !e.fake.batch_calls && e.teleports.empty(), "prepare reached owned-body, ray batch or teleport effects");
    };

    // 1. Existing world.physics permission is required before argument checks.
    for (const auto& granted : {std::string{}, std::string{R"("world.physics.observe")"}}) {
        TestEnvironment e;
        Host host(e, Side::client);
        const auto name = id();
        host.apply({script(name, R"(
            local ok, why = pcall(reskate.world.physics.prepare, {proof = true})
            assert(not ok and why:find('world.physics', 1, true))
        )", granted)});
        running(host, name, e);
        check(!e.fake.body_prepares, "missing base permission reached prepare backend");
        no_effects(e);
    }

    // 2. Server authority refuses even a syntactically valid zero-argument call.
    {
        TestEnvironment e;
        Host host(e, Side::server);
        const auto name = id();
        host.apply({package(name, {{manifest_file, manifest(permission, "server")}, {"plugin/main.lua", R"(
            local ok, why = pcall(reskate.world.physics.prepare)
            assert(not ok and why:find('client phase only', 1, true))
        )"}})});
        running(host, name, e);
        check(!e.fake.body_prepares, "server reached prepare backend");
        no_effects(e);
    }

    // 3. Only no arguments: no owner, definition, body handle, proof or callback.
    {
        TestEnvironment e;
        Host host(e, Side::client);
        const auto name = id();
        host.apply({script(name, R"(
            local P = reskate.world.physics
            assert(not pcall(P.prepare, nil))
            assert(not pcall(P.prepare, {}, {}))
            for _, bad in {1, 'ready', false, {}, newproxy(), function() return true end} do
                assert(not pcall(P.prepare, bad))
            end
            local ok, why = P.prepare()
            assert(ok == true and why == nil and type(ok) == 'boolean')
            local again, reason = P.prepare()
            assert(again == false and reason:find('once in one tick', 1, true))
        )", permission)});
        running(host, name, e);
        check(e.fake.body_prepares == 1, "arity or quota failure reached backend");
        no_effects(e);
    }

    // 4. A refused pending call consumes this tick's attempt. Later explicit
    // calls distinguish ready and terminal unavailable, with no hidden create.
    {
        TestEnvironment e;
        e.fake.body_prepare_error = "native body producer setup pending; call prepare again explicitly";
        Host host(e, Side::client);
        const auto name = id();
        host.apply({script(name, R"(
            local P = reskate.world.physics
            local ok, why = P.prepare()
            assert(ok == false and why == 'native body producer setup pending; call prepare again explicitly')
            ok, why = P.prepare()
            assert(ok == false and why:find('once in one tick', 1, true))
            local phase = 0
            reskate.on('tick', function()
                if phase == 2 then return end
                local ready, reason = P.prepare()
                if phase == 0 then
                    assert(ready == true and reason == nil)
                else
                    assert(ready == false and reason == 'native body ABI or producer hook unavailable; another producer may own it')
                end
                local repeated, why = P.prepare()
                assert(repeated == false and why:find('once in one tick', 1, true))
                phase += 1
                if phase == 2 then reskate.log('explicit readiness calls preserve pending ready and refusal') end
            end)
        )", permission)});
        running(host, name, e);
        check(e.fake.body_prepares == 1, "pending refusal was not charged");
        e.fake.body_prepare_error.clear();
        ticks(host, e, 1);
        running(host, name, e);
        e.fake.body_prepare_error = "native body ABI or producer hook unavailable; another producer may own it";
        ticks(host, e, 1);
        running(host, name, e);
        check(e.fake.body_prepares == 3 && e.logged("explicit readiness calls preserve pending ready and refusal"),
            "readiness call was silently retried or tick counter failed\n" + e.all());
            no_effects(e);
    }

    // 5. Each VM gets its own fixed attempt; one VM cannot consume another's.
    {
        TestEnvironment e;
        Host host(e, Side::client);
        const auto first = id(), second = id();
        const auto main = R"(
            local P = reskate.world.physics
            assert(P.prepare() and not P.prepare())
            local done = false
            reskate.on('tick', function()
                if done then return end
                assert(P.prepare() and not P.prepare())
                done = true
            end)
        )";
        host.apply({script(first, main, permission), script(second, main, permission)});
        running(host, first, e);
        running(host, second, e);
        check(e.fake.body_prepares == 2, "one VM's quota affected another VM");
        ticks(host, e, 1);
        running(host, first, e);
        running(host, second, e);
        check(e.fake.body_prepares == 4, "actual plugin tick did not reset independent VM quotas");
        no_effects(e);
    }

    // 6. Reload creates only a fresh local attempt counter, never a body request
    // or a transferable/retained preparation handle or definition.
    {
        TestEnvironment e;
        Host host(e, Side::client);
        const auto name = id();
        const auto candidate = script(name, R"(
            local value, why = reskate.world.physics.prepare()
            assert(value == true and why == nil)
        )", permission);
        host.apply({candidate});
        running(host, name, e);
        host.apply({});
        host.apply({candidate});
        running(host, name, e);
        check(e.fake.body_prepares == 2, "reload inherited another VM's prepare counter or requested body work");
        no_effects(e);
    }
    std::cout << "Body prepare Lua: 6 groups, " << scenarios << " constructed Host scenarios; native setup not executed\n";
}
} // namespace
