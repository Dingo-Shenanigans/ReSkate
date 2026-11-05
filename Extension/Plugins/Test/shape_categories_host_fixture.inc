// Host/API cases for probe_batch's opt-in include_shape_categories over controlled
// Game leaf results. The fake copies values in after execute, as the native adapter
// does. Nothing here qualifies a retail category read or what a category means.
namespace {
void shape_categories_host_fixture_v1() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    auto& game = environment.fake;
    const std::string shapes = R"lua(
        local hit_fields = {status = true, position = true, fraction = true, motion = true, normal_available = true,
            surface_velocity_available = true, normal = true, surface_velocity = true}
        local batch_fields = {status = true, reason = true, native_casts = true, budget_exhausted = true, epoch = true,
            local_actor_excluded = true, owned_bodies_excluded = true, rays = true}
        local function count(t, allowed, extra)
            local n = 0
            for k in t do
                n += 1
                assert(allowed[k] or k == extra, "unexpected field " .. tostring(k))
            end
            return n
        end
        -- The option adds no batch-level field: no availability flag, reason or owner.
        local function plain_batch(out) assert(count(out, batch_fields) == (out.reason and 8 or 7), "batch field count") end
    )lua";
    host.apply({script("cat-no-ray", R"lua(
        local ok, why = pcall(reskate.world.physics.probe_batch, {}, {}, {}, {include_shape_categories = true})
        assert(not ok and why:find('"world.raycast"', 1, true), tostring(why))
        reskate.log("category option without world.raycast denied")
    )lua", R"("world.physics","world.physics.observe")"), script("cat-no-physics", R"lua(
        local ok, why = pcall(reskate.world.physics.probe_batch, {}, {}, {}, {include_shape_categories = true})
        assert(not ok and why:find('"world.physics"', 1, true), tostring(why))
        reskate.log("category option without world.physics denied")
    )lua", R"("world.raycast","world.physics.observe")"), script("cat-no-observe", batch_main(R"lua(
        reskate.console.command("cat-denied-only", "", function()
            local ok, why = pcall(P.probe_batch, h, epoch, rays(32), {include_shape_categories = true})
            assert(not ok and why:find('"world.physics.observe"', 1, true), tostring(why))
        end)
        reskate.console.command("cat-denied", "", function()
            local ok, why = pcall(P.probe_batch, h, epoch, rays(32), {include_shape_categories = true})
            assert(not ok and why:find('"world.physics.observe"', 1, true), tostring(why))
            -- The denied opt-in spent nothing: two full ordinary batches still fit this tick.
            local a = P.probe_batch(h, epoch, rays(32))
            local b = P.probe_batch(h, epoch, rays(32), {include_shape_categories = false})
            assert(a.status == "available" and a.native_casts == 32 and b.status == "available" and b.native_casts == 32)
            for _, r in a.rays do assert(r.status == "hit" and r.in_categories == nil) end
            for _, r in b.rays do assert(r.status == "hit" and r.in_categories == nil) end
            reskate.log("observe-less ordinary batches ran")
        end)
    )lua"), R"("world.physics","world.raycast","console")"), script("cat-batch", batch_main(shapes + R"lua(
        reskate.console.command("cat-default", "", function()
            for _, options in {false, {}, {include_shape_categories = false}, {require_contact = false, include_shape_categories = false}} do
                local out = options and P.probe_batch(h, epoch, rays(2), options) or P.probe_batch(h, epoch, rays(2))
                assert(out.status == "available" and out.native_casts == 2)
                plain_batch(out)
                for _, r in out.rays do
                    assert(r.status == "hit" and r.in_categories == nil, "a stray backend category escaped")
                    assert(count(r, hit_fields) == 6, "hit field count without the option")
                end
            end
        end)
        reskate.console.command("cat-values", "", function()
            local out = P.probe_batch(h, epoch, rays(4), {include_shape_categories = true})
            assert(out.status == "available" and out.native_casts == 4 and not out.requested_query_preset)
            plain_batch(out)
            local expected = {0, 1, 2147483648, 4294967295}
            for i, r in out.rays do
                assert(r.status == "hit" and type(r.in_categories) == "number" and r.in_categories == expected[i], tostring(r.in_categories))
                assert(r.in_categories >= 0 and math.floor(r.in_categories) == r.in_categories, "category is not an exact unsigned value")
                assert(count(r, hit_fields, "in_categories") == 7, "hit field count with the option")
            end
        end)
        reskate.console.command("cat-missing", "", function()
            local out = P.probe_batch(h, epoch, rays(2), {include_shape_categories = true})
            assert(out.status == "available" and out.rays[1].in_categories == 0, "a known zero mask was dropped")
            assert(out.rays[2].status == "hit" and out.rays[2].in_categories == nil and count(out.rays[2], hit_fields) == 6,
                "an unavailable category became a value")
        end)
        reskate.console.command("cat-clear", "", function()
            local out = P.probe_batch(h, epoch, rays(2), {include_shape_categories = true})
            assert(out.status == "available")
            for _, r in out.rays do assert(r.status == "clear" and r.in_categories == nil and not r.position) end
        end)
        reskate.console.command("cat-impossible", "", function()
            local out = P.probe_batch(h, epoch, rays(2), {include_shape_categories = true})
            unavailable(out, "invalid_query_backend_result")
            for _, r in out.rays do assert(r.in_categories == nil) end
        end)
        reskate.console.command("cat-mixed", "", function()
            local out = P.probe_batch(h, epoch, {ray, {from = v(0, 2, 0), to = v(0, 1, 0)}, ray}, {include_shape_categories = true, query_preset = "terrain"})
            assert(out.status == "available" and out.native_casts == 3 and out.requested_query_preset == "terrain")
            local a, b, c = out.rays[1], out.rays[2], out.rays[3]
            assert(a.status == "hit" and a.in_categories == 7 and b.status == "clear" and b.in_categories == nil and
                c.status == "hit" and c.in_categories == 9, "categories were not bound to their own rays")
        end)
        reskate.console.command("cat-refused", "", function()
            local out = P.probe_batch(h, epoch, rays(2), {include_shape_categories = true, exclude_local_actor = true})
            unavailable(out, "local_actor_exclusion_unavailable")
            assert(out.native_casts == 0 and out.local_actor_excluded == false)
            plain_batch(out)
            for _, r in out.rays do assert(r.in_categories == nil) end
        end)
        reskate.console.command("cat-exclusion", "", function()
            local out = P.probe_batch(h, epoch, rays(1), {include_shape_categories = true, exclude_bodies = {other}})
            assert(out.status == "available" and out.owned_bodies_excluded and out.rays[1].in_categories == 5)
        end)
        reskate.console.command("cat-budget", "", function()
            local a = P.probe_batch(h, epoch, rays(32), {include_shape_categories = true})
            local b = P.probe_batch(h, epoch, rays(32))
            assert(a.status == "available" and a.native_casts == 32 and b.status == "available" and b.native_casts == 32)
            local ok, why = pcall(P.probe_batch, h, epoch, rays(1), {include_shape_categories = true})
            assert(not ok and why:find("more than 64 raycasts"), "the option changed the logical ray budget")
        end)
        reskate.console.command("cat-strict", "", function()
            for _, bad in {1, 0, "true", "false", {}, function() end, newproxy()} do
                assert(not pcall(P.probe_batch, h, epoch, rays(1), {include_shape_categories = bad}), "category option " .. tostring(bad))
            end
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {include_shape_category = true}), "misspelled option")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {in_categories = true}), "a result field as an option")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {include_shape_categories = true, query_mask = 1}), "a raw mask beside the option")
        end)
    )lua"), R"("world.physics","world.raycast","world.physics.observe","console")")});
    running(host, "cat-no-ray", environment);
    running(host, "cat-no-physics", environment);
    running(host, "cat-no-observe", environment);
    running(host, "cat-batch", environment);
    check(environment.logged("category option without world.raycast denied") &&
        environment.logged("category option without world.physics denied") && !game.batch_calls && !game.native_casts,
        "a missing world.physics or world.raycast permission reached the backend\n" + environment.all());

    // Without world.physics.observe the opt-in fails before any counter or backend
    // work, and the same plugin's ordinary queries still run. The fake offers a
    // stray category throughout, which an ordinary query must never show.
    game.batch_category_mode = 2;
    game.batch_categories.fill(3);
    auto calls = game.batch_calls;
    auto casts = game.native_casts;
    check(host.run_console("cat-denied-only") && game.batch_calls == calls && game.native_casts == casts,
        "a denied opt-in reached the backend or spent a cast\n" + environment.all());
    ticks(host, environment, 1);
    check(host.run_console("cat-denied") && game.batch_calls == calls + 2 && game.native_casts == casts + 64 &&
        !game.last_batch.include_shape_categories && environment.logged("observe-less ordinary batches ran"),
        "a denied opt-in spent budget or broke ordinary queries\n" + environment.all());
    ticks(host, environment, 1);
    calls = game.batch_calls;
    check(host.run_console("cat-default") && game.batch_calls == calls + 4 && !game.last_batch.include_shape_categories,
        "a batch without the opt-in forwarded true or emitted a stray category\n" + environment.all());

    // Opted-in values are exact uint32 numbers on hits only; missing is not zero.
    game.batch_category_mode = 1;
    game.batch_categories = {};
    game.batch_categories[1] = 1;
    game.batch_categories[2] = 0x80000000U;
    game.batch_categories[3] = 0xffffffffU;
    ticks(host, environment, 1);
    casts = game.native_casts;
    check(host.run_console("cat-values") && game.last_batch.include_shape_categories && game.native_casts == casts + 4,
        "exact categories did not reach Lua, the opt-in was not forwarded or casts changed\n" + environment.all());
    game.batch_category_missing[1] = true;
    ticks(host, environment, 1);
    check(host.run_console("cat-missing"), "a missing category became zero or a known zero was dropped\n" + environment.all());
    game.batch_category_missing = {};
    game.batch_clear = true;
    ticks(host, environment, 1);
    check(host.run_console("cat-clear"), "a clear ray carried a category\n" + environment.all());
    game.batch_category_mode = 3;
    ticks(host, environment, 1);
    check(host.run_console("cat-impossible"), "a category on a clear row was accepted\n" + environment.all());
    game.batch_clear = false;
    game.batch_category_mode = 1;
    game.batch_ground_plane = true;
    game.batch_categories[0] = 7;
    game.batch_categories[1] = 8;
    game.batch_categories[2] = 9;
    ticks(host, environment, 1);
    check(host.run_console("cat-mixed") && game.last_batch.preset == body_query::QueryPreset::terrain,
        "categories were not bound to their own rays or changed the preset\n" + environment.all());
    game.batch_ground_plane = false;

    // A refusal never shows a category, even one a backend left in a row.
    game.batch_category_mode = 4;
    game.batch_refusal = body_query::Failure::local_actor_exclusion_unavailable;
    ticks(host, environment, 1);
    casts = game.native_casts;
    check(host.run_console("cat-refused") && game.native_casts == casts && game.last_batch.include_shape_categories &&
        game.last_batch.exclude_local_actor, "a refused batch published a category or spent a cast\n" + environment.all());
    game.batch_refusal = body_query::Failure::none;
    game.batch_category_mode = 1;
    game.batch_categories[0] = 5;

    // Exclusions, the shared native cast budget and the logical ray budget are unchanged.
    ticks(host, environment, 1);
    check(host.run_console("cat-exclusion") && game.last_batch.exclusions == 1 && game.last_batch.include_shape_categories,
        "the opt-in changed owned exclusions\n" + environment.all());
    ticks(host, environment, 1);
    casts = game.native_casts;
    check(host.run_console("cat-budget") && game.native_casts == casts + 64, "the opt-in changed cast or ray budgets\n" + environment.all());
    ticks(host, environment, 1);
    calls = game.batch_calls;
    casts = game.native_casts;
    check(host.run_console("cat-strict") && game.batch_calls == calls && game.native_casts == casts,
        "a non-boolean or misspelled category option reached the backend\n" + environment.all());
    running(host, "cat-batch", environment);
    running(host, "cat-no-observe", environment);
    check(!game.body_submissions, "category queries submitted body commands");
}
}
