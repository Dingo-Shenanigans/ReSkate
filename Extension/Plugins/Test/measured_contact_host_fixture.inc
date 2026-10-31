// Same translation unit as plugin_tests.cpp. Constructed Game facts, real Host and bindings, over
// the measured-contact diagnostic plugin in fixtures/measured-contact (its README says what it does).
#include <cmath>
#include <iterator>

namespace measured_contact_fixture_detail {
using dingosdk::Json;

std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    check(file.good(), "Measured-contact fixture file is required: " + path.string());
    return std::string(std::istreambuf_iterator<char>(file), {});
}

void frame(Host& host, TestEnvironment& environment) {
    // The actual Host builds pages after tick handlers, at its 100ms panel cadence.
    environment.now += 101;
    host.tick(0);
}

void no_actuators(const TestEnvironment& environment) {
    const auto& game = environment.fake;
    check(!game.body_submissions && environment.teleports.empty() && !environment.rays && !game.probes && !game.world_picks,
          "Measured-contact Host fixture requested an unrelated actuator or world-ray API");
}

void no_authority(const TestEnvironment& environment) {
    no_actuators(environment);
    check(environment.fake.calls.empty(), "Unexpected Game calls before measured-contact unload");
}

void only_cleanup(const TestEnvironment& environment, const std::string& id, std::size_t count) {
    no_actuators(environment);
    check(environment.fake.calls == std::vector<std::string>(count, "entities clear " + id),
          "Measured-contact unload calls differ from the exact generic cleanup sequence");
}

void no_body_calls(const TestEnvironment& environment) {
    const auto& game = environment.fake;
    check(!game.body_create_calls && !game.body_creates && !game.body_read_calls && !game.batch_calls &&
              !game.batch_rays && !game.native_casts && !game.body_release_calls,
          "Measured-contact inert/denied path reached the body backend");
}

void no_body_work(const TestEnvironment& environment) {
    no_body_calls(environment);
    no_authority(environment);
}

std::string panel_info(const TestEnvironment& environment, std::string_view label) {
    for (const auto& panel : environment.panels)
        for (const auto& widget : panel.widgets)
            if (widget.kind == plugin_ui::Widget::Kind::info && widget.text == label) return widget.help;
    throw std::runtime_error("Measured-contact page info was not built: " + std::string(label));
}

bool has_start(const TestEnvironment& environment) {
    for (const auto& panel : environment.panels)
        for (const auto& widget : panel.widgets)
            if (widget.kind == plugin_ui::Widget::Kind::button && widget.id == "start") return true;
    return false;
}

std::filesystem::path storage_path(TestEnvironment& environment, const std::string& id) {
    // Host has the default empty suffix; the Candidate id owns this storage namespace.
    return environment.storage_root() / id / "storage.json";
}

Json stored(TestEnvironment& environment, const std::string& id) {
    return Json::parse(contents(storage_path(environment, id)));
}

Json terminal(const TestEnvironment& environment, const std::string& id) {
    const std::string owner = "[" + id + "] ";
    const auto prefix = owner + "measured-contact-trial-v1 finish ";
    std::string payload;
    unsigned found{};
    for (const auto& line : environment.logs) {
        check(!(line.starts_with(owner) && line.find("rest of this second") != std::string::npos),
              "Measured-contact terminal evidence was rate suppressed");
        if (!line.starts_with(prefix)) continue;
        ++found;
        check(line.size() <= prefix.size() + 2000 && !line.ends_with(" ..."), "Measured-contact finish log was truncated");
        payload = line.substr(prefix.size());
    }
    check(found == 1, "Measured-contact needs exactly one actual prefixed terminal log");
    const auto log = Json::parse(payload);
    check(log.is_object() && log.size() == 7 && log.at("phase").is_string() && log.at("reason").is_string() &&
              log.at("released").is_boolean() && log.at("storage_queued").is_boolean() &&
              log.at("storage_detail").is_string() && log.at("query_count").is_number() &&
              log.at("submitted").is_number() && log.at("submitted").get<unsigned>() == 0,
          "Measured-contact terminal payload is not the actual expected JSON shape");
    return log;
}

void terminal_matches(const Json& log, const Json& report) {
    check(log.at("storage_queued").get<bool>() && log.at("reason") == report.at("reason") &&
              log.at("released") == report.at("released") && log.at("submitted") == report.at("submitted") &&
              log.at("query_count").get<std::size_t>() == report.at("queries").size() &&
              log.at("phase").string() == (report.at("passed").get<bool>() ? "passed" : "stopped"),
          "Actual terminal JSON disagrees with the persisted report");
}

void ready(TestEnvironment& environment, const std::string& id) {
    environment.local_skater = SkaterState{{0, 0, 0}, 0, 0, 0, false, 0};
    environment.fake.focus_now = {true, true, true};
    environment.fake.batch_contact_mode = 1;
    environment.fake.batch_ground_plane = true;
    environment.panels_open = true;
    environment.shown = id;
}

void show(Host& host, TestEnvironment& environment, const std::string& id) {
    frame(host, environment);
    running(host, id, environment);
    check(environment.pages.size() == 1 && environment.pages[0].plugin == id &&
              environment.pages[0].title == "Measured contacts" && environment.panels.size() == 1 &&
              environment.panels[0].page && environment.panels[0].title == "Measured contacts",
          "The frozen measured-contact page/title did not build through the actual Host");
}

void start(Host& host, TestEnvironment& environment, const std::string& id) {
    check(has_start(environment), "The actual measured-contact start widget is required");
    environment.actions = {plugin_ui::Action{id, "Measured contacts", "start", false}};
    frame(host, environment);
    running(host, id, environment);
    check(panel_info(environment, "Phase") == "armed" && environment.logged("armed; explicit page action"),
          "The real page action did not arm the frozen diagnostic");
}

void close_menu(Host& host, TestEnvironment& environment, const std::string& id) {
    environment.panels_open = false;
    environment.fake.focus_now.interface_open = false;
    frame(host, environment);
    running(host, id, environment);
}

void vector_lanes(const Json& value, const std::array<double, 3>& expected, const char* what) {
    check(value.is_array() && value.size() == 3, std::string(what) + " is not three persisted numeric lanes");
    for (std::size_t lane = 0; lane < expected.size(); ++lane)
        check(value.at(lane).is_number() && std::isfinite(value.at(lane).get<double>()) &&
                  value.at(lane).get<double>() == expected[lane], std::string(what) + " changed a measured lane");
}
} // namespace measured_contact_fixture_detail

void measured_contact_host_fixture_v1() {
    namespace f = measured_contact_fixture_detail;
    const auto directory = std::filesystem::path(RESKATE_SOURCE_DIR) /
        "Extension/Plugins/Test/fixtures/measured-contact";
    const std::map<std::string, std::string> files{
        {manifest_file, f::contents(directory / manifest_file)},
        {"plugin/main.lua", f::contents(directory / "plugin/main.lua")}
    };
    check(parse_manifest(files.at(manifest_file)).permissions ==
              std::set<std::string, std::less<>>{"input", "menu", "skater.read", "storage", "world.physics", "world.raycast"},
          "The measured-contact fixture permissions differ from the frozen manifest");
    unsigned cases{};

    // Isolated boundary cases exercise the actual checked_text length and callback build.
    for (const auto length : {24u, 25u}) {
        TestEnvironment environment;
        Host host(environment, Side::client);
        const auto id = "measured-title-" + std::to_string(length);
        const std::string title(length, 'T');
        host.apply({script(id, "reskate.menu.page(\"" + title +
            "\",function(ui) ui:label(\"title callback built\") end)", R"("menu")")});
        if (length == 24) {
            running(host, id, environment);
            environment.panels_open = true;
            environment.shown = id;
            f::frame(host, environment);
            check(environment.pages.size() == 1 && environment.pages[0].title == title &&
                      environment.panels.size() == 1 && environment.panels[0].title == title &&
                      environment.panels[0].widgets.size() == 1 &&
                      environment.panels[0].widgets[0].text == "title callback built",
                  "A 24-character page title did not actually build its callback");
        } else {
            stopped(host, id, "plugin/main.lua raised an error as it ran");
            const auto owner = "[" + id + "] plugin/main.lua: ";
            const auto errors = std::count_if(environment.logs.begin(), environment.logs.end(),
                [&](const auto& line) {
                    return line.starts_with(owner) &&
                    line.find("a page's title is longer than 24 characters") != std::string::npos; });
            check(errors == 1, "A 25-byte page title needs one actual owner-prefixed length-error log");
            f::frame(host, environment);
            check(environment.pages.empty() && environment.panels.empty(), "A 25-character title produced a page");
            // A failed startup still releases what the plugin may own (Host::load collects it), once.
            f::only_cleanup(environment,id,1); environment.fake.calls.clear();
        }
        f::no_body_work(environment);
        ++cases;
    }

    // Malformed body/epoch arguments cannot bypass either permission's earlier guard.
    for (const bool missing_physics : {false, true}) {
        TestEnvironment environment;
        Host host(environment, Side::client);
        const std::string id = missing_physics ? "measured-denied-physics" : "measured-denied-ray";
        const std::string permission = missing_physics ? "world.physics" : "world.raycast";
        const std::string grant = missing_physics ? R"("world.raycast")" : R"("world.physics")";
        host.apply({script(id, "local ok,why=pcall(reskate.world.physics.probe_batch,{},{},{},"
            "{require_contact=true,exclude_local_actor=false}); assert(not ok and why:find(\"" + permission +
            "\",1,true) and why:find('needs the',1,true) and why:find('permission in reskate-plugin.json',1,true)); "
            "reskate.log(\"measured permission caught\")", grant)});
        running(host, id, environment);
        check(environment.logged("measured permission caught"), "The expected probe permission denial was not observed");
        check(!environment.fake.focus_reads && !environment.skater_reads,
              "Permission microcase reached a focus/skater backend during load");
        f::no_body_work(environment);
        ++cases;
    }

    struct PermissionCase { const char* id; const char* source; };
    const PermissionCase permission_cases[] {
        {
            "measured-denied-storage", R"(
            for _, name in {'get', 'set'} do
                local ok, why = pcall(reskate.storage[name], {}, {})
                assert(not ok and why:find('needs the "storage" permission', 1, true), name)
            end
        )"},
        {"measured-denied-menu", R"(
            local ok, why = pcall(reskate.menu.page, {}, function() end)
            assert(not ok and why:find('needs the "menu" permission', 1, true))
        )"},
        {"measured-denied-input", R"(
            local ok, why = pcall(reskate.input.focus)
            assert(not ok and why:find('needs the "input" permission', 1, true))
        )"},
        {"measured-denied-skater", R"(
            local ok, why = pcall(reskate.skater.get)
            assert(not ok and why:find('needs the "skater.read" permission', 1, true))
        )"}
    };
    for (const auto& denied : permission_cases) {
        TestEnvironment environment;
        Host host(environment, Side::client);
        host.apply({script(denied.id, denied.source)});
        running(host, denied.id, environment);
        // Assert here: later Host::follow_game ticks may legitimately observe the skater.
        check(!environment.fake.focus_reads && !environment.skater_reads,
              "Denied input/skater API reached its backend before returning the permission error");
        f::no_body_work(environment);
        environment.panels_open = true;
        environment.shown = denied.id;
        f::frame(host, environment);
        check(environment.pages.empty() && environment.panels.empty() &&
                  !std::filesystem::exists(f::storage_path(environment, denied.id)),
              "A denied storage/menu call created a page or persisted a value");
        ++cases;
    }

    // Production binding types/opaque epochs and actual vector serialization, with a constructed Game.
    {
        TestEnvironment environment;
        Host host(environment, Side::client);
        auto& game = environment.fake;
        game.batch_contact_mode = 1;
        host.apply({script("measured-physics-contract", R"(
            local P, v = reskate.world.physics, vector.create
            local d = {mass = 12, inertia = v(.4, .1, .4), position = v(2, 1.5, 0), rotation = {0, 0, 0, 1},
                capsules = {{a = v(0, -.25, 0), b = v(0, .25, 0), radius = .12}}}
            local h, other = assert(P.create(d)), assert(P.create(d))
            local before = assert(P.read(h))
            local expected = {epoch = true, serial = true, time = true, seconds = true, position = true, velocity = true,
                angular_velocity = true, inertia = true, mass = true, rotation = true}
            local count = 0
            for key in before do assert(expected[key], key); count += 1 end
            assert(type(before) == 'table' and count == 10)
            assert(type(before.epoch) == 'userdata' and getmetatable(before.epoch) == 'locked')
            assert(type(before.serial) == 'string' and before.serial:match('^[1-9][0-9]*$'))
            for _, name in {'time', 'seconds', 'mass'} do assert(type(before[name]) == 'number') end
            for _, name in {'position', 'velocity', 'angular_velocity', 'inertia'} do assert(type(before[name]) == 'vector') end
            assert(type(before.rotation) == 'table' and #before.rotation == 4 and before.rotation[4] == 1)
            for i = 1, 4 do assert(type(before.rotation[i]) == 'number') end
            assert(not before.world and not before.body and not before.generation and not before.pointer)
            local after = assert(P.read(h))
            assert(after.epoch ~= before.epoch and after.serial == before.serial and after.time == before.time)
            assert(after.position == before.position and after.velocity == before.velocity and after.mass == before.mass)
            assert(1 / after.velocity.x == 1 / before.velocity.x, 'zero sign preserved on equal reads')
            local ray = {from = v(2, 2, 0), to = v(2, -2, 0)}
            local out = P.probe_batch(h, before.epoch, {ray}, {require_contact = true, exclude_local_actor = false})
            assert(out.status == 'available' and not out.reason and out.native_casts == 1)
            assert(out.epoch == before.epoch and out.epoch ~= after.epoch)
            assert(out.owned_bodies_excluded and not out.local_actor_excluded)
            local hit = out.rays[1]
            assert(hit.status == 'hit' and type(hit.position) == 'vector' and hit.fraction == .5)
            assert(hit.motion == 'measured' and hit.normal_available == true and hit.surface_velocity_available == true)
            assert(type(hit.normal) == 'vector' and hit.normal == v(0, 1, 0))
            assert(type(hit.surface_velocity) == 'vector' and hit.surface_velocity == v(2, 0, -1))
            assert(not hit.body and not hit.velocity and not hit.feature and not hit.world and not hit.owner)
            assert(not pcall(P.probe_batch, h, {}, {ray}, {require_contact = true}))
            assert(not pcall(P.probe_batch, other, before.epoch, {ray}, {require_contact = true}))
            reskate.log('measured production physics contract observed')
        )", R"("world.physics","world.raycast")")});
        running(host, "measured-physics-contract", environment);
        check(environment.logged("measured production physics contract observed") && game.body_create_calls == 2 &&
                  game.body_read_calls == 2 && game.batch_calls == 1 && game.batch_rays == 1 && game.native_casts == 1 &&
                  game.last_batch.require_contact && !game.last_batch.exclude_local_actor,
              "The actual physics binding/permission boundary did not match its constructed contract");
        f::no_authority(environment);
        ++cases;
    }

    // The exact immutable entry/manifest, driven through real menu action timing and persistence.
    {
        const std::string id = "measured-exact-success";
        TestEnvironment environment;
        Host host(environment, Side::client);
        f::ready(environment, id);
        const auto candidate = package(id, files);
        host.apply({candidate});
        running(host, id, environment);
        f::no_body_work(environment);
        f::show(host, environment, id);
        f::no_body_work(environment);
        check(f::panel_info(environment, "Phase") == "idle" && f::has_start(environment), "Exact source was not initially inert");
        f::show(host, environment, id);
        f::close_menu(host, environment, id);
        environment.panels_open = true;
        environment.fake.focus_now.interface_open = true;
        f::show(host, environment, id);
        environment.actions = {plugin_ui::Action{id, "Measured contacts", "wrong-start-key", false}};
        f::frame(host, environment);
        f::no_body_work(environment);
        check(f::panel_info(environment, "Phase") == "idle" && !environment.logged("armed; explicit page action") &&
                  !environment.logged("measured-contact-trial-v1 finish") &&
                  !std::filesystem::exists(f::storage_path(environment, id)),
              "Repeated eligible page builds, menu reopen or a wrong widget key armed/persisted a trial");
        f::start(host, environment, id);
        f::no_body_work(environment);
        f::frame(host, environment);
        f::no_body_work(environment); // An armed tick with the interface open does no body work.
        f::close_menu(host, environment, id);
        auto& game = environment.fake;
        check(game.body_create_calls == 1 && game.body_creates == 1 && game.body_read_calls == 2 &&
                  game.batch_calls == 1 && game.batch_rays == 3 && game.native_casts == 3 && game.body_release_calls == 1 &&
                  game.last_batch.count == 3 && game.last_batch.require_contact && !game.last_batch.exclude_local_actor,
              "Exact source did not perform one create, one three-ray measured batch and one explicit release");
        const auto finish = f::terminal(environment, id);
        check(finish.at("storage_queued").get<bool>(), "Exact source storage call was not accepted");
        f::no_authority(environment);
        const auto all_releases = game.body_releases.size();
        host.apply({});
        check(game.body_release_calls == 1 && game.body_releases.size() > all_releases,
              "Unloading a completed diagnostic repeated its explicit release or omitted Host owner cleanup");
        const auto committed_path = f::storage_path(environment, id);
        std::error_code committed_error;
        const bool committed = std::filesystem::is_regular_file(committed_path, committed_error);
        check(committed, "Measured-contact exact-success storage commit is not a regular file: " + committed_path.string() +
              "\nfilesystem status: " + committed_error.message() + "\n" + environment.all());
        const auto saved_bytes = f::contents(f::storage_path(environment, id));
        const auto document = f::stored(environment, id);
        const auto& report = document.at("measured_contact_trial_v1");
        f::terminal_matches(finish, report);
        check(report.at("passed").get<bool>() && report.at("released").get<bool>() &&
                  report.at("release_attempted").get<bool>() && report.at("release_call_ok").get<bool>() &&
                  !report.at("unreturned_allocation_unknown").get<bool>() && report.at("submitted").get<unsigned>() == 0 &&
                  !report.at("actor_exclusion_requested").get<bool>() && report.at("contact_measurement_requested").get<bool>() &&
                  report.at("queries").size() == 1 && report.at("creates").size() == 1,
              "Actual persisted success ledger did not retain the bounded outcome");
        const auto& query = report.at("queries").at(std::size_t{0});
        const auto& response = query.at("response");
        check(query.at("sample_exact_equal").get<bool>() && query.at("after_read_ok").get<bool>() &&
                  query.at("before") == query.at("after") && response.at("epoch_matches").get<bool>() &&
                  response.at("native_casts").get<unsigned>() == 3 && response.at("status").string() == "available" &&
                  !response.contains("reason") && response.at("rays").size() == 3,
              "Persisted query did not retain exact-sample branch evidence/original epoch echo/counts");
        check(query.at("requests").size() == game.last_batch.count, "Persisted submitted segment count changed");
        for (std::size_t i = 0; i < game.last_batch.count; ++i) {
            const auto& hit = response.at("rays").at(i);
            const auto& request = game.last_batch.rays[i];
            std::array<double, 3> from{}, to{}, point{};
            const float fraction = -request.from[1] / (request.to[1] - request.from[1]);
            for (std::size_t lane = 0; lane < from.size(); ++lane) {
                from[lane] = request.from[lane];
                to[lane] = request.to[lane];
                point[lane] = request.from[lane] + (request.to[lane] - request.from[lane]) * fraction;
            }
            f::vector_lanes(query.at("requests").at(i).at("from"), from, "submitted from");
            f::vector_lanes(query.at("requests").at(i).at("to"), to, "submitted to");
            f::vector_lanes(hit.at("position"), point, "constructed intersection");
            check(hit.at("fraction").get<double>() == fraction, "Persisted fraction differs from the actual submitted segment intersection");
            check(hit.at("status").string() == "hit" && hit.at("motion").string() == "measured" &&
                      hit.at("normal_available").get<bool>() && hit.at("surface_velocity_available").get<bool>(),
                  "Actual contact availability fields were lost in persisted storage");
            f::vector_lanes(hit.at("normal"), {0, 1, 0}, "normal");
            f::vector_lanes(hit.at("surface_velocity"), {2, 0, -1}, "surface velocity");
        }
        f::only_cleanup(environment, id, 1);
        // Reload exact source: its real storage.get must park the UI at previous trial without work.
        environment.panels_open = true;
        game.focus_now.interface_open = true;
        host.apply({candidate});
        f::show(host, environment, id);
        check(f::panel_info(environment, "Phase") == "previous trial" && !f::has_start(environment),
              "Exact source did not reread its persisted completed ledger");
        environment.actions = {plugin_ui::Action{id, "Measured contacts", "start", false}};
        f::frame(host, environment);
        f::close_menu(host, environment, id);
        f::only_cleanup(environment, id, 1);
        host.apply({});
        check(game.body_create_calls == 1 && game.body_read_calls == 2 && game.batch_calls == 1 && game.body_release_calls == 1 &&
                  f::contents(f::storage_path(environment, id)) == saved_bytes,
              "Completed-ledger reload repeated work or overwrote the persisted result");
        f::only_cleanup(environment, id, 2);
        (void)f::terminal(environment, id);
        ++cases;
    }

    // A preexisting ledger is a separate exact-byte storage namespace case, not the success case's memory.
    {
        const std::string id = "measured-preexisting-ledger";
        TestEnvironment environment;
        Host host(environment, Side::client);
        f::ready(environment, id);
        const std::string original = "{\"measured_contact_trial_v1\":{\"sentinel\":\"preserve exact bytes\"}}\n";
        write(f::storage_path(environment, id), original);
        host.apply({package(id, files)});
        f::show(host, environment, id);
        check(f::panel_info(environment, "Phase") == "previous trial" && !f::has_start(environment),
              "An actual preseeded ledger did not prevent another trial");
        environment.actions = {plugin_ui::Action{id, "Measured contacts", "start", false}};
        f::frame(host, environment);
        f::close_menu(host, environment, id);
        f::no_body_work(environment);
        host.apply({});
        f::no_body_calls(environment);
        f::only_cleanup(environment, id, 1);
        check(f::contents(f::storage_path(environment, id)) == original, "Preexisting storage bytes were overwritten");
        ++cases;
    }

    // Armed with no handle: unload must not claim a release that was never attempted.
    {
        const std::string id = "measured-unload-armed";
        TestEnvironment environment;
        Host host(environment, Side::client);
        f::ready(environment, id);
        host.apply({package(id, files)});
        f::show(host, environment, id);
        f::start(host, environment, id);
        f::no_body_work(environment);
        host.apply({});
        const auto document = f::stored(environment, id);
        const auto& report = document.at("measured_contact_trial_v1");
        f::terminal_matches(f::terminal(environment, id), report);
        check(report.at("reason").string() == "plugin unload" && !report.at("passed").get<bool>() &&
                  !report.at("released").get<bool>() && !report.at("release_attempted").get<bool>() &&
                  !report.contains("release_call_ok") && report.at("release_detail").string() == "no body handle; no release attempted" &&
                  report.at("creates").empty() && report.at("queries").empty(),
              "Armed unload invented native allocation/release success");
        f::no_body_calls(environment);
        f::only_cleanup(environment, id, 1);
        ++cases;
    }

    // Constructed read unavailability leaves one allocated handle for the actual unload callback.
    {
        const std::string id = "measured-unload-allocated";
        TestEnvironment environment;
        Host host(environment, Side::client);
        f::ready(environment, id);
        auto& game = environment.fake;
        game.test_body_read_unavailable = true;
        host.apply({package(id, files)});
        f::show(host, environment, id);
        f::start(host, environment, id);
        f::close_menu(host, environment, id);
        check(game.body_create_calls == 1 && game.body_read_calls == 1 && !game.batch_calls && !game.native_casts &&
                  !game.body_release_calls, "Constructed unavailable sample did not leave exactly one incomplete allocation");
        f::no_authority(environment);
        const auto all_releases = game.body_releases.size();
        host.apply({});
        check(game.body_release_calls == 1 && game.body_releases.size() > all_releases,
              "Allocated unload did not distinguish one script release from automatic owner cleanup");
        const auto document = f::stored(environment, id);
        const auto& report = document.at("measured_contact_trial_v1");
        f::terminal_matches(f::terminal(environment, id), report);
        check(report.at("reason").string() == "plugin unload" && !report.at("passed").get<bool>() &&
                  report.at("released").get<bool>() && report.at("release_attempted").get<bool>() &&
                  report.at("release_call_ok").get<bool>() && report.at("queries").empty(),
              "Allocated unload lost its actual accepted release/refused contact distinction");
        f::frame(host, environment);
        check(game.body_create_calls == 1 && game.body_read_calls == 1 && game.body_release_calls == 1 && !game.batch_calls,
              "An unloaded incomplete trial repeated a body operation");
        f::only_cleanup(environment, id, 1);
        ++cases;
    }

    // Two known preallocation refusals permit only the one separately pressed readiness retry.
    {
        const std::string id = "measured-readiness-bound";
        TestEnvironment environment;
        Host host(environment, Side::client);
        f::ready(environment, id);
        auto& game = environment.fake;
        game.test_body_create_refusal = "waiting for a fresh qualified 60 Hz physics step";
        host.apply({package(id, files)});
        f::show(host, environment, id);
        f::start(host, environment, id);
        f::close_menu(host, environment, id);
        check(game.body_create_calls == 1 && !game.body_creates && !game.body_read_calls && !game.batch_calls,
              "Initial constructed readiness refusal allocated or queried");
        environment.panels_open = true;
        game.focus_now.interface_open = true;
        f::show(host, environment, id);
        check(f::panel_info(environment, "Phase") == "refused", "Readiness refusal did not remain explicit");
        f::start(host, environment, id);
        f::frame(host, environment);
        check(game.body_create_calls == 1, "The menu-open retry allocated before a separate gameplay tick");
        f::close_menu(host, environment, id);
        environment.panels_open = true;
        game.focus_now.interface_open = true;
        f::show(host, environment, id);
        check(!f::has_start(environment), "The exact source offered more than one readiness retry");
        environment.actions = {plugin_ui::Action{id, "Measured contacts", "start", false}};
        f::frame(host, environment);
        f::close_menu(host, environment, id);
        f::no_authority(environment);
        host.apply({});
        const auto document = f::stored(environment, id);
        const auto& report = document.at("measured_contact_trial_v1");
        f::terminal_matches(f::terminal(environment, id), report);
        check(game.body_create_calls == 2 && !game.body_creates && !game.body_read_calls && !game.batch_calls &&
                  !game.native_casts && !game.body_release_calls && report.at("creates").size() == 2 &&
                  report.at("readiness_retries").get<unsigned>() == 1 && !report.at("released").get<bool>() &&
                  !report.at("release_attempted").get<bool>() && !report.at("unreturned_allocation_unknown").get<bool>(),
              "Exact known-readiness retry exceeded its bound or claimed a nonexistent release");
        f::only_cleanup(environment, id, 1);
        ++cases;
    }

    struct ContactFailure { const char* id; int mode; bool clear; unsigned drift, casts; };
    const ContactFailure contact_failures[] {
        { "measured-missing-contact", 0, false, 0, 1 },
        {"measured-clear-contact", 1, true, 0, 3},
        {"measured-malformed-contact", 4, false, 0, 3},
        {"measured-numeric-drift", 1, false, 1, 3}
    };
    for (const auto& failure : contact_failures) {
        const std::string id = failure.id;
        TestEnvironment environment;
        Host host(environment, Side::client);
        f::ready(environment, id);
        auto& game = environment.fake;
        game.batch_contact_mode = failure.mode;
        game.batch_clear = failure.clear;
        game.batch_drift_field = failure.drift;
        host.apply({package(id, files)});
        f::show(host, environment, id);
        f::start(host, environment, id);
        f::close_menu(host, environment, id);
        const auto finish = f::terminal(environment, id);
        for (unsigned i = 0; i < 3; ++i) f::frame(host, environment);
        check(game.body_create_calls == 1 && game.body_read_calls == 2 && game.batch_calls == 1 &&
                  game.native_casts == failure.casts && game.body_release_calls == 1 &&
                  finish.at("phase").string() == "stopped" && finish.at("query_count").get<unsigned>() == 1,
              "A contact refusal/clear/malformed/changed result passed, retried or lost actual cast accounting");
        f::no_authority(environment);
        host.apply({});
        const auto document = f::stored(environment, id);
        const auto& report = document.at("measured_contact_trial_v1");
        f::terminal_matches(f::terminal(environment, id), report);
        const auto& query = report.at("queries").at(std::size_t{0});
        const auto& response = query.at("response");
        check(!report.at("passed").get<bool>() && report.at("queries").size() == 1 &&
                  response.at("native_casts").get<unsigned>() == game.native_casts &&
                  response.at("status").string() == (failure.clear ? "available" : "unavailable"),
              "Exact-entry negative ledger misreported the actual query result");
        if (failure.drift) {
            check(!query.at("sample_exact_equal").get<bool>() && !(query.at("before") == query.at("after")) &&
                      report.at("reason").string() == "postquery sample changed or unavailable; no retry",
                  "An actual numeric sample drift did not reach the frozen diagnostic's no-retry branch");
        } else {
            check(query.at("sample_exact_equal").get<bool>() && query.at("before") == query.at("after"),
                  "A pure backend control was incorrectly labelled as changed numeric sample evidence");
        }
        if (failure.clear) {
            check(!response.contains("reason"), "A successful clear batch invented a backend refusal reason");
            for (const auto& ray : response.at("rays"))
                check(ray.at("status").string() == "clear" && !ray.contains("normal") && !ray.contains("surface_velocity"),
                      "A clear contact control manufactured measured vectors");
        } else {
            const char* reason = failure.mode == 0 ? "contact_measurement_unavailable" :
                failure.drift ? "body_epoch_or_observation_changed" : "invalid_query_backend_result";
            check(response.at("reason").string() == reason, "Exact-entry negative ledger lost its typed backend reason");
            for (const auto& ray : response.at("rays"))
                check(ray.at("status").string() == "unavailable" && !ray.contains("position") &&
                          !ray.contains("normal") && !ray.contains("surface_velocity"),
                      "An unavailable contact batch leaked a partial measured hit");
        }
        f::only_cleanup(environment, id, 1);
        ++cases;
    }

    // The real storage quota rejects the complete report and rolls back; terminal remains bounded.
    {
        const std::string id = "measured-storage-quota";
        TestEnvironment environment;
        Limits limits;
        limits.storage_bytes = 256;
        Host host(environment, Side::client, limits);
        f::ready(environment, id);
        host.apply({package(id, files)});
        f::show(host, environment, id);
        f::start(host, environment, id);
        f::close_menu(host, environment, id);
        const auto finish = f::terminal(environment, id);
        check(finish.at("phase").string() == "passed" && finish.at("released").get<bool>() &&
                  !finish.at("storage_queued").get<bool>() &&
                  finish.at("storage_detail").string().find("storage is full") != std::string::npos,
              "Quota control did not observe the actual rejected storage.set and separate measured/release outcome");
        for (unsigned i = 0; i < 3; ++i) f::frame(host, environment);
        auto& game = environment.fake;
        check(game.body_create_calls == 1 && game.body_read_calls == 2 && game.batch_calls == 1 &&
                  game.native_casts == 3 && game.body_release_calls == 1,
              "Storage rejection repeated the terminal query or cleanup");
        f::no_authority(environment);
        host.apply({});
        check(!std::filesystem::exists(f::storage_path(environment, id)),
              "Rejected, rolled-back quota report unexpectedly became a persisted ledger");
        (void)f::terminal(environment, id);
        f::only_cleanup(environment, id, 1);
        ++cases;
    }

    // Install the test-only filesystem obstruction after load, isolating save from storage-read errors.
    {
        const std::string id = "measured-storage-write-failure";
        TestEnvironment environment;
        Host host(environment, Side::client);
        f::ready(environment, id);
        host.apply({package(id, files)});
        f::show(host, environment, id);
        f::no_body_work(environment);
        const auto path = f::storage_path(environment, id);
        check(!std::filesystem::exists(path), "Write-failure namespace was not fresh at actual load");
        f::start(host, environment, id);
        f::no_body_work(environment);
        std::filesystem::create_directories(path);
        write(path / "sentinel", "save obstruction remains\n");
        f::close_menu(host, environment, id);
        const auto finish = f::terminal(environment, id);
        check(finish.at("storage_queued").get<bool>() && !environment.logged("storage could not be read"),
              "Write-failure control was polluted by an initial read failure or a rejected storage.set");
        f::no_authority(environment);
        host.apply({});
        const std::string warning = "[" + id + "] its storage could not be saved: ";
        const auto warnings = std::count_if(environment.logs.begin(), environment.logs.end(),
            [&](const auto& line) { return line.starts_with(warning); });
        check(warnings == 1 && std::filesystem::is_directory(path) &&
                  f::contents(path / "sentinel") == "save obstruction remains\n",
              "Actual save failure did not remain a distinct single warning with its obstruction preserved");
        auto temporary = path;
        temporary += ".tmp";
        const auto temporary_document = dingosdk::Json::parse(f::contents(temporary));
        f::terminal_matches(finish, temporary_document.at("measured_contact_trial_v1"));
        // This is a failed commit with retained temporary JSON, never a committed storage.json ledger.
        check(!std::filesystem::is_regular_file(path) && environment.fake.body_create_calls == 1 &&
                  environment.fake.body_read_calls == 2 && environment.fake.batch_calls == 1 &&
                  environment.fake.native_casts == 3 && environment.fake.body_release_calls == 1,
              "A filesystem save failure became persisted success or repeated native-facing work");
        f::frame(host, environment);
        (void)f::terminal(environment, id);
        f::only_cleanup(environment, id, 1);
        ++cases;
    }

    check(cases == 20, "Measured-contact fixture case inventory changed");
    std::cout << "Measured contacts: 20 production Host cases with constructed Game facts; no native/gameplay proof\n";
}
