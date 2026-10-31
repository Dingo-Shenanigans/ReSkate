// Host/API cases for probe_batch's exclusion facts and the options beside them, over
// controlled Game leaf results. A fact is reported from the backend's result, never
// from the request; query_preset "all" and include_hit_owner need
// world.physics.observe; require_named_actor_categories_absent needs only
// world.physics and world.raycast; a hit owner is an equality-only token. Nothing
// here qualifies a retail read or what a category or owner slot means.
namespace {
void exclusion_facts_host_fixture_v1() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    auto& game = environment.fake;
    const std::string shapes = R"lua(
        local hit_fields = {status = true, position = true, fraction = true, motion = true, normal_available = true,
            surface_velocity_available = true, normal = true, surface_velocity = true}
        local batch_fields = {status = true, reason = true, native_casts = true, budget_exhausted = true, epoch = true,
            local_actor_excluded = true, owned_bodies_excluded = true, rays = true}
        -- Counts t's fields; each must be an allowed one or one of the named extras.
        local function count(t, allowed, ...)
            local extra, n = {...}, 0
            for k in t do
                n += 1
                assert(allowed[k] or table.find(extra, k), "unexpected field " .. tostring(k))
            end
            return n
        end
    )lua";
    host.apply({script("facts-no-ray", R"lua(
        local ok, why = pcall(reskate.world.physics.probe_batch, {}, {}, {}, {require_named_actor_categories_absent = true})
        assert(not ok and why:find('"world.raycast"', 1, true), tostring(why))
        reskate.log("named requirement without world.raycast denied")
    )lua", R"("world.physics")"), script("facts-no-physics", R"lua(
        local ok, why = pcall(reskate.world.physics.probe_batch, {}, {}, {}, {require_named_actor_categories_absent = true})
        assert(not ok and why:find('"world.physics"', 1, true), tostring(why))
        reskate.log("named requirement without world.physics denied")
    )lua", R"("world.raycast")"), script("facts-plain", batch_main(shapes + R"lua(
        reskate.console.command("facts-observe-denied", "", function()
            for _, options in {{query_preset = "all"}, {include_hit_owner = true},
                    {query_preset = "all", require_named_actor_categories_absent = true},
                    {include_hit_owner = true, require_named_actor_categories_absent = true}} do
                local ok, why = pcall(P.probe_batch, h, epoch, rays(32), options)
                assert(not ok and why:find('"world.physics.observe"', 1, true), tostring(why))
            end
            -- The denials spent nothing: two full ordinary batches still fit this tick.
            local a = P.probe_batch(h, epoch, rays(32))
            local b = P.probe_batch(h, epoch, rays(32), {include_hit_owner = false, query_preset = "camera"})
            assert(a.status == "available" and a.native_casts == 32 and b.status == "available" and b.native_casts == 32)
            for _, r in b.rays do assert(r.status == "hit" and r.owner == nil and r.owner_assigned == nil) end
            reskate.log("observe-less diagnostics denied before any counter")
        end)
        reskate.console.command("facts-default", "", function()
            for _, options in {false, {}, {require_contact = false}, {exclude_local_actor = false},
                    {include_shape_categories = false, include_hit_owner = false}} do
                local out = options and P.probe_batch(h, epoch, rays(2), options) or P.probe_batch(h, epoch, rays(2))
                assert(out.status == "available" and out.native_casts == 2 and out.local_actor_excluded == false and out.owned_bodies_excluded)
                assert(count(out, batch_fields) == 7, "an earlier call's batch fields changed")
                for _, r in out.rays do assert(r.status == "hit" and count(r, hit_fields) == 6, "an earlier call's hit fields changed") end
            end
            reskate.log("earlier calls keep their tables")
        end)
        reskate.console.command("facts-alone", "", function()
            -- The constructed backend runs these batches and establishes no fact.
            local out = P.probe_batch(h, epoch, rays(1), {exclude_local_actor = true})
            assert(out.status == "available" and out.local_actor_excluded == false and count(out, batch_fields) == 7,
                "the actor exclusion flag alone became a fact")
            for _, value in {true, false} do
                out = P.probe_batch(h, epoch, rays(1), {require_named_actor_categories_absent = value})
                assert(out.status == "available" and out.named_actor_categories_absent == false and out.local_actor_excluded == false,
                    "the named requirement alone became a fact")
                assert(count(out, batch_fields, "named_actor_categories_absent") == 8, "the requirement added another batch field")
                for _, r in out.rays do assert(count(r, hit_fields) == 6, "the requirement added a hit field") end
            end
            out = P.probe_batch(h, epoch, rays(1), {exclude_local_actor = true, require_named_actor_categories_absent = true})
            assert(out.status == "available" and out.local_actor_excluded == false and out.named_actor_categories_absent == false,
                "both flags alone became facts")
            reskate.log("request flags alone established no fact")
        end)
        reskate.console.command("facts-named", "", function()
            -- A production-shaped request: measured contact, the camera preset and the check.
            local out = P.probe_batch(h, epoch, rays(2), {require_contact = true, query_preset = "camera", require_named_actor_categories_absent = true})
            assert(out.status == "available" and out.native_casts == 2 and out.requested_query_preset == "camera")
            assert(out.named_actor_categories_absent == true and out.local_actor_excluded == false, "the backend's fact was not reported")
            assert(count(out, batch_fields, "requested_query_preset", "named_actor_categories_absent") == 9, "the requirement added another batch field")
            for _, r in out.rays do
                assert(r.status == "hit" and r.motion == "measured" and r.in_categories == nil and r.owner == nil and r.owner_assigned == nil,
                    "the requirement showed a category or an owner")
                assert(count(r, hit_fields) == 8, "hit field count under the requirement")
            end
            reskate.log("named requirement ran with world.physics and world.raycast")
        end)
        -- args: a tag, then L and/or N for the two flags, or - for neither.
        reskate.console.command("facts-case", "", function(args)
            local options = {}
            if args[2]:find("L", 1, true) then options.exclude_local_actor = true end
            if args[2]:find("N", 1, true) then options.require_named_actor_categories_absent = true end
            local out = P.probe_batch(h, epoch, rays(2), options)
            if out.status ~= "available" then unavailable(out, out.reason) end
            reskate.log(string.format("facts %s: %s %s local=%s named=%s casts=%s", args[1], out.status, tostring(out.reason),
                tostring(out.local_actor_excluded), tostring(out.named_actor_categories_absent), tostring(out.native_casts)))
        end)
    )lua"), R"("world.physics","world.raycast","console")"), script("facts-observe", batch_main(shapes + R"lua(
        local kept
        reskate.console.command("facts-all", "", function()
            local out = P.probe_batch(h, epoch, rays(2), {query_preset = "all"})
            assert(out.status == "available" and out.native_casts == 2 and out.requested_query_preset == "all" and out.local_actor_excluded == false)
            assert(out.named_actor_categories_absent == nil and count(out, batch_fields, "requested_query_preset") == 8, "all added another batch field")
            for _, r in out.rays do assert(r.status == "hit" and count(r, hit_fields) == 6, "all added a hit field") end
            reskate.log("all ran with world.physics.observe")
        end)
        reskate.console.command("facts-all-named", "", function()
            local out = P.probe_batch(h, epoch, rays(1), {query_preset = "all", require_named_actor_categories_absent = true})
            unavailable(out, "named_actor_categories_unavailable")
            assert(out.native_casts == 0 and out.named_actor_categories_absent == false and out.local_actor_excluded == false and
                out.requested_query_preset == "all", "the refused requirement claimed a fact or a cast")
            reskate.log("all with the named requirement refused before any cast")
        end)
        reskate.console.command("facts-strict", "", function()
            for _, bad in {1, 0, "true", "false", {}, function() end, newproxy()} do
                assert(not pcall(P.probe_batch, h, epoch, rays(1), {require_named_actor_categories_absent = bad}), "requirement " .. tostring(bad))
                assert(not pcall(P.probe_batch, h, epoch, rays(1), {include_hit_owner = bad}), "owner option " .. tostring(bad))
            end
            for _, bad in {"All", "ALL", "all ", "everything", 4294967295, -1} do
                assert(not pcall(P.probe_batch, h, epoch, rays(1), {query_preset = bad}), "preset " .. tostring(bad))
            end
            -- Still no raw mask, beside "all" or alone.
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {query_mask = 4294967295}), "raw mask option")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {filter = 1, query_preset = "all"}), "raw filter beside all")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {query_preset = "all", query_mask = 1}), "raw mask beside all")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {named_actor_categories_absent = true}), "a result field as an option")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {require_named_actor_categories = true}), "misspelled requirement")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {include_hit_owners = true}), "misspelled owner option")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {owner = true}), "a hit field as an option")
            reskate.log("exclusion options are strict")
        end)
        reskate.console.command("facts-owners", "", function()
            local out = P.probe_batch(h, epoch, rays(4), {include_hit_owner = true})
            assert(out.status == "available" and out.native_casts == 4 and count(out, batch_fields) == 7, "the owner option added a batch field")
            local a, b, c, d = out.rays[1], out.rays[2], out.rays[3], out.rays[4]
            assert(a.status == "hit" and a.owner_assigned == false and a.owner == nil and count(a, hit_fields, "owner_assigned") == 7,
                "an unassigned owner slot became a token")
            for _, r in {b, c, d} do
                assert(r.status == "hit" and r.owner_assigned == true and type(r.owner) == "userdata" and getmetatable(r.owner) == "locked",
                    "an assigned owner slot is not an opaque token")
                assert(count(r, hit_fields, "owner_assigned", "owner") == 8, "hit field count with an owner token")
            end
            assert(b.owner == c.owner and b.owner ~= d.owner and c.owner ~= d.owner, "owner equality does not follow the slot")
            assert(b.owner ~= b.body and b.owner ~= 42 and b.owner ~= true, "an owner token equals another kind of value")
            assert(not pcall(function() return b.owner.slot end) and not pcall(function() return b.owner[1] end), "an owner token can be read")
            assert(not pcall(function() b.owner.slot = 1 end) and not pcall(setmetatable, b.owner, {}), "an owner token can be changed")
            assert(not pcall(P.read, b.owner) and not pcall(P.probe_batch, b.owner, epoch, rays(1)), "an owner token is an owned handle")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {exclude_bodies = {b.owner}}), "an owner token is an exclusion")
            -- Batches on one measured epoch are comparable, a later one included.
            local again = P.probe_batch(h, epoch, {ray, ray}, {include_hit_owner = true})
            assert(again.status == "available" and again.rays[2].owner == b.owner and again.rays[2].owner ~= d.owner,
                "owner tokens from one epoch's batches do not compare")
            kept = b.owner
            reskate.log("owner tokens compare by slot within one epoch")
        end)
        reskate.console.command("facts-owner-missing", "", function()
            local out = P.probe_batch(h, epoch, rays(2), {include_hit_owner = true})
            assert(out.status == "available", "an unreadable owner slot refused the batch")
            local a, b = out.rays[1], out.rays[2]
            assert(a.status == "hit" and a.owner_assigned == nil and a.owner == nil and count(a, hit_fields) == 6,
                "an unreadable owner slot became a field")
            assert(b.owner_assigned == true and type(b.owner) == "userdata", "a readable owner slot was lost")
            reskate.log("an unreadable owner slot is absent")
        end)
        reskate.console.command("facts-owner-clear", "", function()
            local out = P.probe_batch(h, epoch, rays(2), {include_hit_owner = true})
            unavailable(out, "invalid_query_backend_result")
            for _, r in out.rays do assert(r.owner == nil and r.owner_assigned == nil) end
            reskate.log("an owner slot on a clear row was refused")
        end)
        reskate.console.command("facts-owners-later", "", function()
            local later = assert(P.read(h))
            assert(later.serial ~= sample.serial, "the fixture did not advance the measured epoch")
            local out = P.probe_batch(h, later.epoch, {ray, ray}, {include_hit_owner = true})
            assert(out.status == "available" and out.rays[2].owner_assigned == true and out.rays[2].owner ~= kept,
                "an owner token compared equal across measured epochs")
            local same = P.probe_batch(h, later.epoch, {ray, ray}, {include_hit_owner = true})
            assert(same.status == "available" and same.rays[2].owner == out.rays[2].owner, "owner tokens on one later epoch differ")
            reskate.log("owner tokens differ across measured epochs")
        end)
    )lua"), R"("world.physics","world.raycast","world.physics.observe","console")")});
    running(host, "facts-no-ray", environment);
    running(host, "facts-no-physics", environment);
    running(host, "facts-plain", environment);
    running(host, "facts-observe", environment);
    check(environment.logged("named requirement without world.raycast denied") &&
        environment.logged("named requirement without world.physics denied") && !game.batch_calls && !game.native_casts,
        "a missing world.physics or world.raycast permission reached the backend\n" + environment.all());

    // The fake offers stray categories and owner slots on every hit, which a batch
    // that did not ask for them must never show.
    game.batch_category_mode = 2;
    game.batch_categories.fill(3);
    game.batch_owner_mode = 2;
    game.batch_owners.fill(0x2a);
    ticks(host, environment, 1);
    auto calls = game.batch_calls;
    auto casts = game.native_casts;
    check(host.run_console("facts-observe-denied") && game.batch_calls == calls + 2 && game.native_casts == casts + 64 &&
        !game.last_batch.include_hit_owner && game.last_batch.preset == body_query::QueryPreset::camera &&
        environment.logged("observe-less diagnostics denied before any counter"),
        "an option that needs world.physics.observe reached the backend or a counter without it\n" + environment.all());
    ticks(host, environment, 1);
    calls = game.batch_calls;
    check(host.run_console("facts-default") && game.batch_calls == calls + 5 && environment.logged("earlier calls keep their tables"),
        "an earlier call's result table changed\n" + environment.all());
    ticks(host, environment, 1);
    check(host.run_console("facts-alone") && game.last_batch.exclude_local_actor && game.last_batch.require_named_actor_categories_absent &&
        environment.logged("request flags alone established no fact"), "a request flag alone became a fact\n" + environment.all());

    // The requirement runs with world.physics and world.raycast alone, and reports
    // only the backend's fact: no category or owner, even ones the backend offers.
    ticks(host, environment, 1);
    game.batch_contact_mode = 1;
    game.batch_claims_named_categories = true;
    check(host.run_console("facts-named") && game.last_batch.require_named_actor_categories_absent && game.last_batch.require_contact &&
        game.last_batch.preset == body_query::QueryPreset::camera && !game.last_batch.include_shape_categories &&
        !game.last_batch.include_hit_owner && environment.logged("named requirement ran with world.physics and world.raycast"),
        "the requirement was not forwarded alone or its fact was not reported\n" + environment.all());
    game.batch_contact_mode = 0;
    game.batch_claims_named_categories = false;
    game.batch_category_mode = 0;
    game.batch_owner_mode = 0;
    game.batch_owners = {};

    // A fact the request did not ask for, or both facts at once, refuse the batch;
    // a requested fact is reported only when the backend established it.
    struct Case { const char* tag; const char* flags; bool local, named; const char* expected; };
    for (const Case& c : {
             Case{"unrequested-local", "-", true, false, "unavailable invalid_query_backend_result local=false named=nil casts=2"},
             Case{"unrequested-named", "-", false, true, "unavailable invalid_query_backend_result local=false named=nil casts=2"},
             Case{"named-for-local", "L", false, true, "unavailable invalid_query_backend_result local=false named=nil casts=2"},
             Case{"local-for-named", "N", true, false, "unavailable invalid_query_backend_result local=false named=false casts=2"},
             Case{"both", "LN", true, true, "unavailable invalid_query_backend_result local=false named=false casts=2"},
             Case{"local", "LN", true, false, "available nil local=true named=false casts=2"},
             Case{"named", "LN", false, true, "available nil local=false named=true casts=2"},
             Case{"named-only", "N", false, true, "available nil local=false named=true casts=2"},
             Case{"local-only", "L", true, false, "available nil local=true named=nil casts=2"},
             Case{"neither", "LN", false, false, "available nil local=false named=false casts=2"}}) {
        ticks(host, environment, 1);
        game.batch_claims_local_actor = c.local;
        game.batch_claims_named_categories = c.named;
        const auto expected = std::string("facts ") + c.tag + ": " + c.expected;
        check(host.run_console(std::string("facts-case ") + c.tag + " " + c.flags) && environment.logged(expected),
            "a backend fact was reported against the request: " + expected + "\n" + environment.all());
    }
    // A refusal carries no fact, whatever the backend left set, and keeps its own
    // reason and actual casts: the named requirement's refusals and the per-ray ones.
    game.batch_claims_local_actor = game.batch_claims_named_categories = true;
    for (const auto& [reason, refused_casts] : {std::pair{body_query::Failure::named_actor_categories_unavailable, 0U},
             std::pair{body_query::Failure::hit_categories_unavailable, 2U}, std::pair{body_query::Failure::hit_categories_inconsistent, 2U},
             std::pair{body_query::Failure::hit_named_actor_category, 2U}, std::pair{body_query::Failure::owned_body_hit, 1U},
             std::pair{body_query::Failure::hit_identity_unqualified, 1U}}) {
        ticks(host, environment, 1);
        game.batch_refusal = reason;
        game.batch_refusal_casts = refused_casts;
        casts = game.native_casts;
        const std::string named = body_query::name(reason);
        const auto expected = "facts " + named + ": unavailable " + named + " local=false named=false casts=" + std::to_string(refused_casts);
        check(host.run_console("facts-case " + named + " LN") && environment.logged(expected) && game.native_casts == casts + refused_casts,
            "a refused batch kept a fact, lost its reason or its casts: " + expected + "\n" + environment.all());
    }
    game.batch_refusal = body_query::Failure::none;
    game.batch_refusal_casts = 0;
    game.batch_claims_local_actor = game.batch_claims_named_categories = false;

    // With world.physics.observe: the all preset runs, and the requirement's refusal
    // under it is reported with no cast and no fact.
    ticks(host, environment, 1);
    check(host.run_console("facts-all") && game.last_batch.preset == body_query::QueryPreset::all &&
        environment.logged("all ran with world.physics.observe"), "the all preset did not reach the backend\n" + environment.all());
    ticks(host, environment, 1);
    game.batch_refusal = body_query::Failure::named_actor_categories_unavailable;
    casts = game.native_casts;
    check(host.run_console("facts-all-named") && game.native_casts == casts && game.last_batch.preset == body_query::QueryPreset::all &&
        game.last_batch.require_named_actor_categories_absent && environment.logged("all with the named requirement refused before any cast"),
        "all with the requirement spent a cast or claimed a fact\n" + environment.all());
    game.batch_refusal = body_query::Failure::none;
    ticks(host, environment, 1);
    calls = game.batch_calls;
    casts = game.native_casts;
    check(host.run_console("facts-strict") && game.batch_calls == calls && game.native_casts == casts &&
        environment.logged("exclusion options are strict"), "a malformed option reached the backend\n" + environment.all());

    // Owner tokens: unassigned is a known false, an assigned slot an opaque token
    // that compares by slot within one measured epoch, and unreadable is absent.
    game.batch_owner_mode = 1;
    game.batch_owners[0] = 0xffffffffU;
    game.batch_owners[1] = game.batch_owners[2] = 0x2a;
    game.batch_owners[3] = 0x2b;
    ticks(host, environment, 1);
    check(host.run_console("facts-owners") && game.last_batch.include_hit_owner && environment.logged("owner tokens compare by slot within one epoch"),
        "owner tokens were not opaque or did not compare by slot\n" + environment.all());
    ticks(host, environment, 1);
    game.batch_owner_missing[0] = true;
    check(host.run_console("facts-owner-missing") && environment.logged("an unreadable owner slot is absent"),
        "an unreadable owner slot became a value\n" + environment.all());
    game.batch_owner_missing = {};
    ticks(host, environment, 1);
    game.batch_clear = true;
    game.batch_owner_mode = 3;
    check(host.run_console("facts-owner-clear") && environment.logged("an owner slot on a clear row was refused"),
        "an owner slot on a clear row was accepted\n" + environment.all());
    game.batch_clear = false;
    game.batch_owner_mode = 1;
    // A later measured epoch: the same slot gives a token that compares unequal.
    for (auto& entry : game.owned) ++entry.second.sample.epoch.serial;
    ticks(host, environment, 1);
    check(host.run_console("facts-owners-later") && environment.logged("owner tokens differ across measured epochs"),
        "an owner token compared equal across measured epochs\n" + environment.all());
    running(host, "facts-plain", environment);
    running(host, "facts-observe", environment);
    check(!game.body_submissions, "exclusion fact queries submitted body commands");
}
}
