// Real Host/VM/bindings over constructed passive DTOs. These tests do not run
// an XInput hook, prove native publication or grant capture/actor authority.
namespace {
namespace passive_lease_fixture {
namespace lease = dingosdk::input_lease;
// Also reused by the capture Host fixture; the passive oracles below are retained.
struct Game : dingosdk::plugins::Game {
    struct Record { bool used{}, renewed{}; std::uint64_t owner{}; lease::Receipt receipt{}; lease::Snapshot issued{}; };
    std::array<Record, 8> records{};
    std::uint64_t next{9007199254741000ULL}, now{9007199254740997ULL}, ordinal{9007199254741033ULL};
    unsigned requests{}, reads{}, renews{}, releases{}, states{}, acks{}, closes{}, renewal_delay_ms{};
    int bad_sample{}, bad_receipt{};
    bool partial_request{}, zero_handle{};
    lease::Snapshot last_renewed{};
    Record* find(std::uint64_t owner, lease::Handle handle) noexcept {
        for (auto& row : records) if (row.used && row.owner == owner && row.receipt.handle == handle) return &row;
        return nullptr;
    }
    void refresh(Record& row) noexcept {
        if (!lease::terminal(row.receipt.status) && now >= row.receipt.expires_ms) {
            row.receipt.status = lease::Status::expired; row.receipt.reason = lease::Reason::deadline;
            row.receipt.source_released = row.receipt.polls_in_flight == 0;
        }
    }
    bool input_lease_request(std::uint64_t owner, std::uint32_t user, lease::Handle& h, lease::Reason& why) noexcept override {
        ++requests; h = {};
        for (auto& row : records) if (!row.used) {
            row = {}; row.used = true; row.owner = owner;
            row.receipt.handle = {++next}; row.receipt.user = user;
            row.receipt.requested_ms = now; row.receipt.expires_ms = now + lease::lifetime_ms;
            row.receipt.source_epoch = 9007199254741019ULL; row.receipt.context_generation = 9007199254741021ULL;
            h = zero_handle ? lease::Handle{} : row.receipt.handle;
            why = partial_request ? lease::Reason::busy : lease::Reason::none;
            return !partial_request;
        }
        why = lease::Reason::capacity; return false;
    }
    bool input_lease_read(std::uint64_t owner, lease::Handle h, lease::Snapshot& out, lease::Reason& why) noexcept override {
        ++reads; auto* row = find(owner, h);
        if (!row) { why = lease::Reason::unavailable; return false; }
        refresh(*row);
        if (lease::terminal(row->receipt.status)) { why = row->receipt.reason; return false; }
        ++now;
        out = {h, row->receipt.source_epoch, ++ordinal, now, row->receipt.context_generation, row->receipt.user, 0xffffffffU,
            lease::Pad{0xffff, 255, 0, -32768, 32767, 0, -1}};
        row->issued = out; row->renewed = false;
        row->receipt.status = lease::Status::active; row->receipt.observed_ms = out.observed_ms;
        row->receipt.last_observation = out.observation_serial;
        if (bad_sample == 1) ++out.handle.sequence;
        if (bad_sample == 2) out.user = 4;
        if (bad_sample == 3) out.source_epoch = 0;
        if (bad_sample == 4) out.context_generation = 0;
        if (bad_sample == 5) out.observation_serial = 0;
        if (bad_sample == 6) out.observed_ms = 0;
        why = lease::Reason::none; return true;
    }
    bool input_lease_renew(std::uint64_t owner, lease::Handle h, const lease::Snapshot& sample, lease::Reason& why) noexcept override {
        ++renews; auto* row = find(owner, h);
        if (!row) { why = lease::Reason::unavailable; return false; }
        refresh(*row);
        if (lease::terminal(row->receipt.status) || row->renewed || sample != row->issued) { why = lease::Reason::stale_epoch; return false; }
        now += renewal_delay_ms;
        if (now < sample.observed_ms || now - sample.observed_ms > lease::maximum_age_ms) { why = lease::Reason::source_stale; return false; }
        if (now > std::numeric_limits<std::uint64_t>::max() - lease::lifetime_ms) { why = lease::Reason::deadline; return false; }
        row->renewed = true; ++row->receipt.renewals; row->receipt.expires_ms = now + lease::lifetime_ms;
        last_renewed = sample; why = lease::Reason::none; return true;
    }
    bool input_lease_release(std::uint64_t owner, lease::Handle h, lease::Reason& why) noexcept override {
        ++releases; auto* row = find(owner, h);
        if (!row) { why = lease::Reason::unavailable; return false; }
        if (!lease::terminal(row->receipt.status)) { row->receipt.status = lease::Status::released; row->receipt.reason = lease::Reason::cancelled; }
        row->receipt.source_released = row->receipt.polls_in_flight == 0;
        why = lease::Reason::none; return true;
    }
    bool input_lease_state(std::uint64_t owner, lease::Handle h, lease::Receipt& out, lease::Reason& why) noexcept override {
        ++states; auto* row = find(owner, h);
        if (!row) { why = lease::Reason::unavailable; return false; }
        refresh(*row); out = row->receipt;
        if (bad_receipt == 1) ++out.handle.sequence;
        if (bad_receipt == 2) out.status = static_cast<lease::Status>(99);
        if (bad_receipt == 3) out.reason = static_cast<lease::Reason>(99);
        if (bad_receipt == 4) out.polls_in_flight = 129;
        if (bad_receipt == 5) out.source_released = true;
        if (bad_receipt == 6) { out.status = lease::Status::released; out.reason = lease::Reason::cancelled; out.source_released = true; out.polls_in_flight = 1; }
        if (bad_receipt == 7) { out.status = lease::Status::released; out.reason = lease::Reason::cancelled; out.source_released = false; out.polls_in_flight = 0; }
        if (bad_receipt == 8) out.requested_ms = 0;
        if (bad_receipt == 9) out.expires_ms = out.requested_ms - 1;
        if (bad_receipt == 10) { out.status = lease::Status::active; out.last_observation = out.observed_ms = 0; }
        if (bad_receipt == 11) out.orphaned = true;
        if (bad_receipt == 12) out.expires_ms = out.requested_ms + lease::lifetime_ms - 1;
        if (bad_receipt == 13) out.observed_ms = std::numeric_limits<std::uint64_t>::max();
        why = lease::Reason::none; return true;
    }
    bool input_lease_acknowledge(std::uint64_t owner, lease::Handle h, lease::Reason& why) noexcept override {
        ++acks; auto* row = find(owner, h);
        if (!row) { why = lease::Reason::unavailable; return false; }
        refresh(*row);
        if (!lease::terminal(row->receipt.status) || !row->receipt.source_released || row->receipt.polls_in_flight) { why = lease::Reason::busy; return false; }
        row->used = false; why = lease::Reason::none; return true;
    }
    void input_lease_release_all(std::uint64_t owner) noexcept override {
        ++closes;
        for (auto& row : records) if (row.used && row.owner == owner) {
            row.receipt.status = lease::Status::revoked; row.receipt.reason = lease::Reason::cancelled;
            row.receipt.orphaned = true; row.receipt.source_released = row.receipt.polls_in_flight == 0;
            if (row.receipt.source_released) row.used = false;
        }
    }
    unsigned calls() const noexcept { return requests + reads + renews + releases + states + acks; }
};
struct Environment final : dingosdk::plugins::Environment {
    Game fake;
    std::vector<std::string> logs;
    double now{1000};
    std::size_t occurrences(std::string_view token) const {
        return static_cast<std::size_t>(std::count_if(logs.begin(), logs.end(), [&](const auto& line) { return line.find(token) != std::string::npos; }));
    }
    void log(LogLevel, std::string_view value) override { logs.emplace_back(value); }
    void console(std::string_view) override {}
    double now_ms() override { return now; }
    std::optional<bool> approved(const Candidate&) override { return std::nullopt; }
    std::filesystem::path storage_root() override { return scratch / "passive-input-lease"; }
    bool builtin_command(std::string_view) override { return false; }
    dingosdk::plugins::Game& game() override { return fake; }
};
void running(const Host& host, std::string_view id) {
    const auto state = status_of(host, id);
    check(state.state == "running", std::string(id) + ": " + state.state + ": " + state.reason);
}
}

void passive_input_lease_host_fixture_v1() {
    namespace f = passive_lease_fixture;
    namespace lease = dingosdk::input_lease;
    const std::string permission = R"("input.lease")";
    unsigned groups{};
    const auto prompt = approval_prompt(permission_groups(parse_manifest(manifest(permission))));
    check(prompt.find("Reserve and read one observed controller stream, without capturing or changing input") != std::string::npos &&
          prompt.find("bind keys") == std::string::npos && prompt.find("Bind keys") == std::string::npos,
          "passive-only approval claimed a generic input binding or omitted its reservation");
    // 1. Permission precedes malformed arguments and backend access. Generic
    // input alone does not grant the selected-source observation contract.
    for (const auto& granted : {std::string{}, std::string{R"("input")"}}) {
        f::Environment e; Host host(e, Side::client);
        host.apply({script("lease-denied", R"(
            local P=reskate.input
            for _,name in {'lease','lease_read','lease_renew','lease_release','lease_state','lease_acknowledge'} do
                local ok,why=pcall(P[name], {}, {}, {})
                assert(not ok and why:find('input.lease',1,true), name)
            end
            -- The new surface exists, but passive/generic permission cannot grant capture.
            for _,name in {'lease_capture','lease_capture_state','lease_release_capture','lease_acknowledge_capture'} do
                local ok,why=pcall(P[name],{},{},{})
                assert(not ok and why:find('input.capture',1,true),name)
            end
        )", granted)});
        f::running(host, "lease-denied"); host.apply({});
        check(!e.fake.calls() && !e.fake.closes, "permission refusal reached a passive lease backend or new cleanup callback");
    }
    ++groups;
    // 2. A server receives no selected client stream, even with the permission.
    {
        f::Environment e; Host host(e, Side::server);
        host.apply({package("lease-server", {{manifest_file, manifest(permission, "server")}, {"plugin/main.lua", R"(
            for _,name in {'lease','lease_read','lease_renew','lease_release','lease_state','lease_acknowledge'} do
                local ok,why=pcall(reskate.input[name], {}, {}, {})
                assert(not ok and why:find('client phase only',1,true),name)
            end
        )"}})});
        f::running(host, "lease-server"); host.apply({});
        check(!e.fake.calls() && !e.fake.closes, "server reached passive client lease backend");
    }
    ++groups;
    // 3. No scalar coercion, forged handle, extra argument or actor operation.
    {
        f::Environment e; Host host(e, Side::client);
        host.apply({script("lease-arguments", R"(
            local P=reskate.input
            for _,bad in {'0',false,{},newproxy(),-1,4,.5,0/0,math.huge} do assert(not pcall(P.lease,bad)) end
            assert(not pcall(P.lease) and not pcall(P.lease,0,{}))
            for _,name in {'lease_read','lease_release','lease_state','lease_acknowledge'} do assert(not pcall(P[name],{})) end
            local h=assert(P.lease(0));assert(type(h)=='userdata' and getmetatable(h)=='locked')
            local twice,why=P.lease(0);assert(twice==nil and why:find('once in one tick',1,true))
            assert(not pcall(P.lease_renew,h,{}) and not pcall(P.lease_read,h,nil))
            local generic_ok,generic_why=pcall(P.key,'Space');assert(not generic_ok and generic_why:find('input',1,true),'passive permission did not grant generic input')
        )", permission)});
        f::running(host, "lease-arguments");
        check(e.fake.requests==1 && !e.fake.reads && !e.fake.renews, "malformed API or request quota reached backend");
    }
    ++groups;
    // 4. Large u64 identities stay strings. Renewal forwards the exact hidden
    // latest issued snapshot, not changed public fields or an older epoch.
    {
        f::Environment e;e.fake.renewal_delay_ms=37; Host host(e, Side::client);
        host.apply({script("lease-snapshot", R"(
            local P=reskate.input;local h=assert(P.lease(2));local first=assert(P.lease_read(h));local sample=assert(P.lease_read(h))
            assert(sample.sequence=='9007199254741001' and sample.source_epoch=='9007199254741019' and sample.context_generation=='9007199254741021')
            assert(sample.observed_ms=='9007199254740999' and sample.observation_serial=='9007199254741035')
            assert(sample.user==2 and sample.packet==4294967295 and sample.pad.buttons==65535 and sample.pad.left_x==-32768 and sample.pad.left_y==32767)
            assert(sample.mode=='observe' and sample.native_consumption=='unobserved' and sample.physical_device_identity=='unobserved' and not sample.neutral)
            assert(not pcall(function()sample.user=0 end) and not pcall(function()sample.pad.buttons=0 end))
            local ok,why=P.lease_renew(h,first);assert(ok==false and why=='stale_epoch')
            local copy=table.clone(sample);copy.source_epoch='1';copy.user=0;copy.pad={buttons=0}
            assert(P.lease_renew(h,copy),'only the hidden epoch may reach renewal')
            local renewed=assert(P.lease_state(h));assert(renewed.expires_ms=='9007199254741286' and renewed.renewals=='1','renewal interval starts at current backend time')
            ok,why=P.lease_renew(h,sample);assert(ok==false and why=='stale_epoch','one sample renews once')
        )", permission)});
        f::running(host,"lease-snapshot");
        check(e.fake.renews==3 && e.fake.last_renewed==e.fake.records[0].issued && e.fake.records[0].receipt.renewals==1,
            "mutable fields or stale observation became native renewal facts");
    }
    // Near-overflow observations can still be diagnostic while renewal refuses
    // the future interval. No premature read-time max-250 assumption is valid.
    {
        f::Environment e;e.fake.now=std::numeric_limits<std::uint64_t>::max()-lease::lifetime_ms;Host host(e,Side::client);
        host.apply({script("lease-near-limit",R"(
            local P=reskate.input;local h=assert(P.lease(0));local sample=assert(P.lease_read(h))
            assert(sample.observed_ms=='18446744073709551366')
            local row=assert(P.lease_state(h));assert(row.expires_ms=='18446744073709551615')
            local ok,why=P.lease_renew(h,sample);assert(ok==false and why=='deadline')
        )",permission)});f::running(host,"lease-near-limit");
        check(e.fake.renews==1 && e.fake.records[0].receipt.renewals==0,"near-limit diagnostic became an overflowing renewal");
    }
    ++groups;
    // 5. Terminal does not mean released while exact callbacks still drain.
    {
        f::Environment e; Host host(e,Side::client);
        host.apply({script("lease-terminal",R"(
            local P=reskate.input;local h=assert(P.lease(0));assert(P.lease_read(h))
            local ok,why=P.lease_acknowledge(h);assert(ok==false and why=='busy')
            reskate.console.command('lease-release','',function()
                assert(P.lease_release(h));local r=assert(P.lease_state(h))
                assert(r.status=='released' and not r.source_released and r.polls_in_flight==1 and not r.orphaned)
                assert(r.capture_reserved==false and r.report_debt==nil and not P.lease_acknowledge(h));reskate.log('lease-release-complete')
            end)
            reskate.console.command('lease-ack','',function()
                local r=assert(P.lease_state(h));assert(r.source_released and r.polls_in_flight==0)
                assert(P.lease_acknowledge(h));local gone,why=P.lease_state(h);assert(gone==nil and why=='unavailable');reskate.log('lease-ack-complete')
            end)
        )",R"("input.lease","console")")});
        f::running(host,"lease-terminal");e.fake.records[0].receipt.polls_in_flight=1;
        check(host.run_console("lease-release") && e.occurrences("lease-release-complete")==1,"terminal release callback");
        e.fake.records[0].receipt.polls_in_flight=0;e.fake.records[0].receipt.source_released=true;
        check(host.run_console("lease-ack") && e.occurrences("lease-ack-complete")==1 && !e.fake.records[0].used,"terminal drained record did not ACK");
        host.apply({});check(e.fake.closes==1,"owner close duplicated passive lease revocation");
    }
    ++groups;
    // 6. Backend DTO corruption is refused rather than exposed as authority.
    {
        f::Environment e; Host host(e,Side::client);
        host.apply({script("lease-malformed",R"(
            local P=reskate.input;local h=assert(P.lease(0))
            reskate.console.command('lease-bad-sample','',function()local v,why=P.lease_read(h);assert(v==nil and why:find('backend sample',1,true));reskate.log('lease-bad-sample-complete')end)
            reskate.console.command('lease-bad-receipt','',function()local v,why=P.lease_state(h);assert(v==nil and why:find('backend receipt',1,true));reskate.log('lease-bad-receipt-complete')end)
        )",R"("input.lease","console")")});
        f::running(host,"lease-malformed");
        for(int mode=1;mode<=6;++mode){e.fake.bad_sample=mode;check(host.run_console("lease-bad-sample") && e.occurrences("lease-bad-sample-complete")==static_cast<std::size_t>(mode),"malformed sample console");}
        e.fake.bad_sample=0;
        for(int mode=1;mode<=13;++mode){e.fake.bad_receipt=mode;check(host.run_console("lease-bad-receipt") && e.occurrences("lease-bad-receipt-complete")==static_cast<std::size_t>(mode),"malformed receipt console");}
    }
    ++groups;
    // 7. Opaque capabilities cannot cross a plugin call in either direction.
    {
        f::Environment e; Host host(e,Side::client);
        host.apply({script("lease-library",R"(
            local P=reskate.input
            reskate.expose({read=function(h)return P.lease_state(h)end,make=function()return P.lease(1)end})
        )",permission),script("lease-user",R"(
            local P=reskate.input;local h=assert(P.lease(0))
            reskate.console.command('lease-cross','',function()
                local other=assert(reskate.require_plugin('lease-library'))
                assert(not pcall(other.read,h),'a capability crossed VMs')
                assert(not pcall(other.read,{}),'a forged table became a handle')
                assert(not pcall(other.make),'an opaque return crossed VMs');reskate.log('lease-cross-complete')
            end)
        )",R"("input.lease","console")")});
        f::running(host,"lease-library");f::running(host,"lease-user");
        check(host.run_console("lease-cross") && e.occurrences("lease-cross-complete")==1 && !e.fake.states && e.fake.requests==2,"cross-VM attempt reached a foreign receipt");
        const auto old_owner=e.fake.records[0].owner;const auto old_handle=e.fake.records[0].receipt.handle;
        check(old_owner!=e.fake.records[1].owner,"two VMs shared an owner identity");
        lease::Snapshot no_sample{};lease::Reason why{};
        check(!e.fake.input_lease_read(old_owner+100,old_handle,no_sample,why) && why==lease::Reason::unavailable && e.fake.records[0].used,
            "a different owner adopted a live receipt");
        host.apply({});
    }
    ++groups;
    // 8. Expiry stays readable; denied/partial request cleanup is owner-scoped.
    {
        f::Environment e;Host host(e,Side::client);
        host.apply({script("lease-expired",R"(
            local P=reskate.input;local h=assert(P.lease(0));assert(P.lease_read(h))
            reskate.console.command('lease-expired','',function()
                local v,why=P.lease_read(h);assert(v==nil and why=='deadline')
                local r=assert(P.lease_state(h));assert(r.status=='expired' and r.source_released and r.polls_in_flight==0)
                assert(P.lease_acknowledge(h));reskate.log('lease-expired-complete')
            end)
        )",R"("input.lease","console")")});
        e.fake.now=e.fake.records[0].receipt.expires_ms+1;
        check(host.run_console("lease-expired") && e.occurrences("lease-expired-complete")==1 && !e.fake.records[0].used,"expired receipt cleanup unavailable");
    }
    {
        f::Environment e;Host host(e,Side::client);host.apply({script("lease-startup-fault",R"(
            assert(reskate.input.lease(0));error('constructed startup failure after reservation')
        )",permission)});
        check(status_of(host,"lease-startup-fault").state=="stopped" && e.fake.closes==1 &&
            std::none_of(e.fake.records.begin(),e.fake.records.end(),[](const auto& row){return row.used;}),
            "startup failure lost owner revocation");
    }
    for(bool zero_handle:{false,true}){
        f::Environment e;e.fake.partial_request=!zero_handle;e.fake.zero_handle=zero_handle;
        Host host(e,Side::client);host.apply({script("lease-partial",R"(
            local value,why=reskate.input.lease(0);assert(value==nil and type(why)=='string')
        )",permission)});f::running(host,"lease-partial");host.apply({});
        check(e.fake.closes==1 && std::none_of(e.fake.records.begin(),e.fake.records.end(),[](const auto& r){return r.used;}),
            "refused/invalid request lost retained owner cleanup");
    }
    {
        TestEnvironment e;Host host(e,Side::client);host.apply({script("lease-portable",R"(
            local v,why=reskate.input.lease(0);assert(v==nil and why=='unavailable')
        )",permission)});running(host,"lease-portable",e);
    }
    ++groups;
    // 9. Each VM has bounded calls; the normal next tick resets its counters.
    {
        f::Environment e;Host host(e,Side::client);host.apply({script("lease-rate",R"(
            local P=reskate.input;local h=assert(P.lease(0))
            for i=1,63 do assert(P.lease_state(h)) end
            local ok,why=pcall(P.lease_state,h);assert(not ok and why:find('64 input lease calls',1,true))
            local done=false;reskate.on('tick',function()if done then return end;done=true;assert(P.lease_state(h));assert(P.lease_release(h));assert(P.lease_acknowledge(h))end)
        )",permission)});f::running(host,"lease-rate");
        check(e.fake.states==63,"over-budget receipt reached backend");e.now+=16;host.tick(16);f::running(host,"lease-rate");
        check(e.fake.states==64 && e.fake.acks==1,"per-VM tick did not reset passive call budget");
    }
    ++groups;
    check(groups==9,"passive lease fixture group accounting");
    std::cout<<"Passive input lease Host: "<<groups<<" authored groups; constructed facts, no native publication qualification\n";
}
}

// Actual Host/VM/API over controlled DTO leaves, not a native writer simulator.
// Native copy/version/handback proof is in input_write_source_tests.cpp. These
// controllable receipts test only the sandbox/permission/opaque-token boundary.
namespace {
namespace tracked_write_fixture {
namespace lease = dingosdk::input_lease;
namespace write = dingosdk::input_tracked_write;
struct Game final : passive_lease_fixture::Game {
    bool used{}, publish_during_request{};
    write::Receipt writer{};
    lease::Snapshot admitted{};
    lease::Pad last_pad{};
    std::uint64_t generation{9007199254742000ULL}, revision{9007199254743000ULL};
    unsigned write_requests{}, write_states{}, write_releases{}, write_acks{};
    int bad_state{}, bad_accept{}, bad_passive{};
    bool matching(std::uint64_t owner, lease::Handle h) const noexcept {
        return used && writer.key.owner == owner && writer.key.lease_sequence == h.sequence;
    }
    void sync() noexcept {
        for (auto& r : records) if (r.used) {
            r.receipt.write_reserved = matching(r.owner, r.receipt.handle);
            r.receipt.source_released = lease::terminal(r.receipt.status) && !r.receipt.polls_in_flight &&
                !r.receipt.capture_reserved && !r.receipt.write_reserved;
        }
    }
    void confirmed(unsigned polls = 0) noexcept {
        writer.phase = write::Phase::active; writer.reason = write::Reason::none;
        writer.published_revision = writer.desired_revision;
        ++writer.publications; ++writer.version_publications;
        writer.last_written_kind = write::WrittenKind::desired;
        writer.output_debt = true; writer.polls_in_flight = polls; sync();
    }
    void handback() noexcept {
        // Deliberately controlled backend DTO; this is not a native observation.
        writer.phase = write::Phase::complete; writer.reason = write::Reason::cancelled;
        writer.output_debt = false; writer.output_released = true;
        writer.polls_in_flight = 0; writer.callbacks_unknown = writer.quarantined = false;
        if (writer.publications) {
            ++writer.publications; writer.handback_publications = 1;
            writer.last_written_kind = write::WrittenKind::handback;
        }
        sync();
    }
    bool input_lease_write_request(std::uint64_t owner, lease::Handle h, const lease::Snapshot& sample,
        const lease::Pad& pad, write::Version& version, write::Reason& why) noexcept override {
        ++write_requests; version = {}; why = write::Reason::unavailable;
        const auto* r = find(owner, h);
        if (!r || sample != r->issued || now < sample.observed_ms || now - sample.observed_ms > lease::maximum_age_ms) {
            why = write::Reason::source_stale; return false;
        }
        if (used && (!matching(owner, h) || writer.phase != write::Phase::active || !writer.version_publications)) {
            why = write::Reason::busy; return false;
        }
        if (!used) { writer = {}; writer.key = {owner, h.sequence, sample.source_epoch, sample.context_generation, sample.user}; ++generation; }
        used = true; admitted = sample; last_pad = pad;
        writer.version = {generation, ++revision}; writer.desired_revision = revision;
        writer.version_publications = 0; writer.phase = write::Phase::pending;
        writer.reason = write::Reason::none; writer.write_expires_ms = now + write::write_lifetime_ms;
        writer.source_expires_ms = r->receipt.expires_ms; version = writer.version; why = write::Reason::none;
        if (publish_during_request) confirmed(1); // Controlled publication/delivery interleaving, not provider proof.
        if (bad_accept == 1) version.generation = 0;
        if (bad_accept == 2) version.revision = 0;
        if (bad_accept == 3) why = write::Reason::busy;
        sync(); return true;
    }
    bool input_lease_write_state(std::uint64_t owner, lease::Handle h, write::Version version,
        write::Receipt& out) noexcept override {
        ++write_states; if (!matching(owner, h) || version != writer.version) return false; out = writer;
        switch (bad_state) {
        case 1: ++out.key.owner; break;
        case 2: ++out.key.lease_sequence; break;
        case 3: out.key.source_epoch = 0; break;
        case 4: out.key.context_generation = 0; break;
        case 5: out.key.user = 4; break;
        case 6: ++out.version.generation; break;
        case 7: ++out.version.revision; break;
        case 8: ++out.desired_revision; break;
        case 9: out.published_revision = out.desired_revision + 1; break;
        case 10: out.write_expires_ms = 0; break;
        case 11: out.source_expires_ms = 0; break;
        case 12: out.version_publications = out.publications + 1; break;
        case 13: out.cleanup_publications = out.publications + 1; break;
        case 14: out.handback_publications = 2; break;
        case 15: out.phase = static_cast<write::Phase>(99); break;
        case 16: out.reason = static_cast<write::Reason>(99); break;
        case 17: out.last_written_kind = static_cast<write::WrittenKind>(99); break;
        case 18: out.phase = write::Phase::active; break;
        case 19: out.output_released = true; break;
        case 20: out.callbacks_unknown = true; break;
        case 21: out.quarantined = true; break;
        case 22: out.orphaned = true; break;
        case 23: out.last_written_kind = write::WrittenKind::cleanup; break;
        case 24: out.last_written_kind = write::WrittenKind::handback; break;
        case 25: // A desired publication alone cannot prove complete handback.
            out.phase = write::Phase::complete; out.output_released = true;
            out.publications = out.version_publications = 1; out.published_revision = out.desired_revision;
            out.last_written_kind = write::WrittenKind::desired; break;
        case 26: // Disjoint cleanup and handback cannot both fit in total1.
            out.phase = write::Phase::releasing; out.reason = write::Reason::cancelled;
            out.publications = out.cleanup_publications = out.handback_publications = 1;
            out.last_written_kind = write::WrittenKind::handback; out.polls_in_flight = 1; break;
        case 27: // Current desired and cleanup counters likewise are disjoint.
            out.phase = write::Phase::releasing; out.reason = write::Reason::cancelled;
            out.publications = out.version_publications = out.cleanup_publications = 1;
            out.published_revision = out.desired_revision; out.output_debt = true;
            out.last_written_kind = write::WrittenKind::cleanup; break;
        case 28: out.polls_in_flight = write::maximum_callbacks + 1; break;
        case 29:
            out.published_revision = out.desired_revision - 1; out.publications = 1;
            out.last_written_kind = write::WrittenKind::desired; break; // Pending older copy still owes handback.
        case 30:
            out.phase = write::Phase::active; out.published_revision = out.desired_revision;
            out.version_publications = out.handback_publications = 1; out.publications = 2;
            out.last_written_kind = write::WrittenKind::handback; out.output_debt = true; break;
        case 31:
            out.phase = write::Phase::releasing; out.reason = write::Reason::cancelled;
            out.published_revision = out.desired_revision; out.version_publications = out.publications = 1;
            out.last_written_kind = write::WrittenKind::desired; break; // No handback cannot erase confirmed debt.
        case 32:
            out.phase = write::Phase::active; out.published_revision = out.desired_revision;
            out.version_publications = out.publications = std::numeric_limits<std::uint64_t>::max();
            out.last_written_kind = write::WrittenKind::desired; out.output_debt = true; break;
        }
        return true;
    }
    bool input_lease_write_release(std::uint64_t owner, lease::Handle h) noexcept override {
        ++write_releases; if (!matching(owner, h)) return false;
        if (writer.phase != write::Phase::complete) {
            writer.phase = write::Phase::releasing; writer.reason = write::Reason::cancelled;
            if (!writer.output_debt && !writer.polls_in_flight && !writer.callbacks_unknown) handback();
        }
        sync(); return true;
    }
    bool input_lease_write_acknowledge(std::uint64_t owner, lease::Handle h) noexcept override {
        ++write_acks;
        if (!matching(owner, h) || writer.phase != write::Phase::complete || !writer.output_released ||
            writer.output_debt || writer.callbacks_unknown || writer.polls_in_flight) return false;
        used = false; writer = {}; sync(); return true;
    }
    bool input_lease_state(std::uint64_t owner, lease::Handle h, lease::Receipt& out, lease::Reason& why) noexcept override {
        if (!passive_lease_fixture::Game::input_lease_state(owner, h, out, why)) return false;
        sync(); out = find(owner, h)->receipt;
        if (bad_passive == 1) { out.status = lease::Status::warming; out.reason = lease::Reason::none;
            out.last_observation = out.observed_ms = out.renewals = 0; out.write_reserved = true; }
        if (bad_passive == 2) { out.status = lease::Status::released; out.reason = lease::Reason::cancelled;
            out.source_released = out.write_reserved = true; }
        if (bad_passive == 3) out.capture_reserved = out.write_reserved = true;
        return true;
    }
    bool input_lease_release(std::uint64_t owner, lease::Handle h, lease::Reason& why) noexcept override {
        const bool ok = passive_lease_fixture::Game::input_lease_release(owner, h, why);
        if (ok && matching(owner, h)) (void)input_lease_write_release(owner, h); sync(); return ok;
    }
    bool input_lease_acknowledge(std::uint64_t owner, lease::Handle h, lease::Reason& why) noexcept override {
        if (matching(owner, h)) { ++acks; why = lease::Reason::busy; return false; }
        return passive_lease_fixture::Game::input_lease_acknowledge(owner, h, why);
    }
    void input_lease_release_all(std::uint64_t owner) noexcept override {
        ++closes;
        if (used && writer.key.owner == owner) {
            writer.orphaned = true; writer.phase = write::Phase::releasing; writer.reason = write::Reason::cancelled;
        }
        for (auto& r : records) if (r.used && r.owner == owner) {
            r.receipt.status = lease::Status::revoked; r.receipt.reason = lease::Reason::cancelled; r.receipt.orphaned = true;
        }
        sync();
    }
    unsigned writer_calls() const noexcept { return write_requests + write_states + write_releases + write_acks; }
};
struct Environment final : dingosdk::plugins::Environment {
    Game fake; std::vector<std::string> logs; double now{1000};
    std::size_t occurrences(std::string_view token) const {
        return static_cast<std::size_t>(std::count_if(logs.begin(), logs.end(), [&](const auto& s) { return s.find(token) != std::string::npos; }));
    }
    void log(LogLevel, std::string_view s) override { logs.emplace_back(s); }
    void console(std::string_view) override {}
    double now_ms() override { return now; }
    std::optional<bool> approved(const Candidate&) override { return std::nullopt; }
    std::filesystem::path storage_root() override { return scratch / "tracked-input-write"; }
    bool builtin_command(std::string_view) override { return false; }
    dingosdk::plugins::Game& game() override { return fake; }
};
}
void tracked_input_write_host_fixture_v1() {
    namespace f = tracked_write_fixture;
    namespace write = dingosdk::input_tracked_write;
    const std::string permission = R"("input.lease","input.write")";
    const std::string commands = R"("input.lease","input.write","console")";
    unsigned groups{};
    const auto prompt = approval_prompt(permission_groups(parse_manifest(manifest(permission))));
    check(prompt.find("- Reserve and read one observed controller stream, without capturing or changing input and "
        "replace one selected controller stream with tracked output and handback cleanup\n") != std::string::npos,
        "combined lease/writer approval omits either distinct authority or its joined presentation");
    const auto writer_prompt = approval_prompt(permission_groups(parse_manifest(manifest(R"("input.write")"))));
    check(writer_prompt.find("- Replace one selected controller stream with tracked output and handback cleanup\n") != std::string::npos &&
        writer_prompt.find("Reserve and read one observed controller stream") == std::string::npos &&
        writer_prompt.find("reserve and read one observed controller stream") == std::string::npos,
        "single-writer approval must capitalize its full cleanup authority without implying a passive read grant");
    for (const auto& granted : {std::string{}, std::string{R"("input.lease")"}, std::string{R"("input.write")"},
        std::string{R"("input.lease","input.capture")"}}) {
        f::Environment e; Host host(e, Side::client);
        const auto missing = granted == R"("input.write")" ? "input.lease" : "input.write";
        host.apply({script("write-denied", std::string{R"(
            assert(reskate.input.write==nil,'tracked permission installed an ordinary legacy writer')
            for _,name in {'lease_write','lease_write_state','lease_release_write','lease_acknowledge_write'} do
                local ok,why=pcall(reskate.input[name],{},{},{},{})
                assert(not ok and why:find(')"} + missing + R"(',1,true),name)
            end
        )", granted)});
        passive_lease_fixture::running(host, "write-denied"); host.apply({});
        check(!e.fake.calls() && !e.fake.writer_calls() && !e.fake.closes, "missing dual permission reached backend or cleanup");
    }
    { f::Environment e; Host host(e, Side::server);
      host.apply({package("write-server", {{manifest_file, manifest(permission,"server")},{"plugin/main.lua",R"(
        for _,name in {'lease_write','lease_write_state','lease_release_write','lease_acknowledge_write'} do
            local ok,why=pcall(reskate.input[name],{},{},{},{})
            assert(not ok and why:find('client phase only',1,true),name)
        end
      )"}})}); passive_lease_fixture::running(host,"write-server"); host.apply({});
      check(!e.fake.calls() && !e.fake.writer_calls() && !e.fake.closes,"server denial reached writer"); }
    { passive_lease_fixture::Environment e;Host host(e,Side::client);host.apply({script("write-unavailable",R"(
        local P=reskate.input;local h=assert(P.lease(0));local sample=assert(P.lease_read(h))
        local v,why=P.lease_write(h,sample,{});assert(v==nil and why=='unavailable')
        local ok;ok,why=P.lease_release_write(h);assert(ok==false and why=='unavailable')
        ok,why=P.lease_acknowledge_write(h);assert(ok==false and why=='unavailable')
        assert(P.write==nil)
    )",permission)});passive_lease_fixture::running(host,"write-unavailable");host.apply({});
      check(e.fake.requests==1 && e.fake.closes==1,"default unavailable writer altered passive owner cleanup"); }
    ++groups;
    { f::Environment e; Host host(e,Side::client); host.apply({script("write-parse",R"(
        local P=reskate.input;local h=assert(P.lease(0));local s=assert(P.lease_read(h));local touched=false
        for _,bad in {false,'0',newproxy(),{typo=0},{[1]=0},{left_x='0'},{left_x=0/0},{right_y=math.huge},
            {left_x=-1.01},{left_y=1.01},{left_trigger=-.01},{right_trigger=1.01},{buttons={a=1}},
            {buttons={A=true}},{buttons={[1]=true}},setmetatable({}, {__index=function()touched=true end}),
            {buttons=setmetatable({}, {__index=function()touched=true end})}} do assert(not pcall(P.lease_write,h,s,bad)) end
        assert(not pcall(P.lease_write,h,s,nil) and not pcall(P.lease_write,h,s) and not pcall(P.lease_write,h,s,{},{}))
        assert(not pcall(P.lease_write,h,{epoch=newproxy()},{}))
        local hostile=setmetatable(table.clone(s),{__index=function()touched=true end})
        assert(not pcall(P.lease_write,h,hostile,{}))
        assert(not pcall(P.lease_release_write) and not pcall(P.lease_acknowledge_write))
        assert(not pcall(P.lease_write,{},s,{}) and not pcall(P.lease_write,h,{},{}))
        assert(not pcall(P.lease_write_state,h,{}) and not pcall(P.lease_write_state,h,newproxy()))
        assert(not pcall(P.lease_release_write,h,nil) and not pcall(P.lease_acknowledge_write,h,nil))
        assert(not touched,'metatable parser invoked caller code')
    )",permission)}); passive_lease_fixture::running(host,"write-parse");
      check(e.fake.requests==1 && e.fake.reads==1 && !e.fake.writer_calls(),"malformed Pad/sample/token reached writer backend"); }
    ++groups;
    { f::Environment e; Host host(e,Side::client); host.apply({script("write-version",R"(
        local P=reskate.input;local h=assert(P.lease(0));local first=assert(P.lease_read(h));local s=assert(P.lease_read(h))
        local nope,why=P.lease_write(h,first,{});assert(nope==nil and why=='source_stale')
        local copy=table.clone(s);copy.source_epoch='1';copy.pad={buttons=0}
        local v=assert(P.lease_write(h,copy,{left_x=-1,left_y=1,right_x=-.5,right_y=.5,left_trigger=.5,right_trigger=1,
            buttons={up=true,down=true,left=true,right=true,start=true,back=true,left_stick=true,right_stick=true,
                lb=true,rb=true,a=true,b=true,x=true,y=true}}))
        assert(type(v)=='userdata' and getmetatable(v)=='locked' and not pcall(function()v.revision='1'end))
        assert(not pcall(P.lease_write_state,h,v,nil) and not pcall(P.lease_write_state,h))
        local r=assert(P.lease_write_state(h,v))
        assert(r.sequence=='9007199254741001' and r.generation=='9007199254742001' and r.revision=='9007199254743001')
        assert(r.phase=='pending' and r.version_publications=='0' and r.publications=='0' and not r.output_released)
        assert(r.source_epoch=='9007199254741019' and r.context_generation=='9007199254741021' and r.mode=='write')
        assert(r.native_consumption=='unobserved' and r.physical_device_identity=='unobserved' and r.owner==nil and r.native_clock==nil)
        assert(not pcall(function()r.phase='complete'end))
        local pending,reason=P.lease_write(h,s,{});assert(pending==nil and reason=='busy')
        local passive=assert(P.lease_state(h));assert(passive.write_reserved and not passive.capture_reserved)
    )",permission)}); passive_lease_fixture::running(host,"write-version");
      const auto& p=e.fake.last_pad;
      check(p.buttons==62463 && p.left_x==-32768 && p.left_y==32767 && p.right_x==-16384 && p.right_y==16384 &&
          p.left_trigger==128 && p.right_trigger==255 && e.fake.admitted==e.fake.records[0].issued,
          "normalized Pad quantization or original hidden sample authority changed"); }
    ++groups;
    { f::Environment e;Host host(e,Side::client);host.apply({script("write-supersede",R"(
        local P=reskate.input;local h=assert(P.lease(0));local v=assert(P.lease_write(h,assert(P.lease_read(h)),{}))
        reskate.console.command('write-next','',function()
            local old=v;v=assert(P.lease_write(h,assert(P.lease_read(h)),{}))
            local gone,why=P.lease_write_state(h,old);assert(gone==nil and why=='unavailable')
            local r=assert(P.lease_write_state(h,v));assert(r.phase=='pending' and r.version_publications=='0' and r.output_debt)
            reskate.log('write-next-complete')
        end)
        reskate.console.command('write-release','',function()
            assert(P.lease_release_write(h));local r=assert(P.lease_write_state(h,v))
            assert(r.phase=='releasing' and r.output_debt and r.polls_in_flight==1 and not r.output_released)
            assert(not P.lease_acknowledge_write(h) and not P.lease_acknowledge(h));reskate.log('write-release-complete')
        end)
        reskate.console.command('write-ack','',function()
            local r=assert(P.lease_write_state(h,v));assert(r.output_released and not r.output_debt and r.handback_publications=='1')
            assert(P.lease_acknowledge_write(h));local gone,why=P.lease_write_state(h,v);assert(gone==nil and why=='unavailable')
            assert(P.lease_release(h) and P.lease_acknowledge(h));reskate.log('write-ack-complete')
        end)
    )",commands)});passive_lease_fixture::running(host,"write-supersede");e.fake.confirmed();
      check(host.run_console("write-next") && e.occurrences("write-next-complete")==1,"latest version command failed");
      e.fake.writer.polls_in_flight=1;e.fake.now=e.fake.records[0].receipt.expires_ms+1;
      check(host.run_console("write-release") && e.occurrences("write-release-complete")==1 && e.fake.used,"release falsely acknowledged debt");
      e.fake.handback();
      check(host.run_console("write-ack") && e.occurrences("write-ack-complete")==1 && !e.fake.used && !e.fake.records[0].used,
          "positive controlled handback/drain did not allow ordered writer/passive ACK"); }
    ++groups;
    { f::Environment e;Host host(e,Side::client);host.apply({script("write-corrupt",R"(
        local P=reskate.input;local h=assert(P.lease(0));local v=assert(P.lease_write(h,assert(P.lease_read(h)),{}))
        reskate.console.command('write-bad-row','',function()
            local r,why=P.lease_write_state(h,v);assert(r==nil and why=='invalid input write backend receipt');reskate.log('write-bad-row-complete')
        end)
        reskate.console.command('write-bad-passive','',function()
            local r,why=P.lease_state(h);assert(r==nil and why:find('backend receipt',1,true));reskate.log('write-bad-passive-complete')
        end)
    )",commands)});passive_lease_fixture::running(host,"write-corrupt");
      for(int mode=1;mode<=32;++mode){e.fake.bad_state=mode;
        check(host.run_console("write-bad-row") && e.occurrences("write-bad-row-complete")==static_cast<unsigned>(mode),
            "malformed writer DTO escaped readonly boundary");}
      e.fake.bad_state=0;
      for(int mode=1;mode<=3;++mode){e.fake.bad_passive=mode;
        check(host.run_console("write-bad-passive") && e.occurrences("write-bad-passive-complete")==static_cast<unsigned>(mode),
            "impossible passive write reservation escaped boundary");} }
    { f::Environment e;Host host(e,Side::client);host.apply({script("write-retained-max",R"(
        local P=reskate.input;local h=assert(P.lease(0));local v=assert(P.lease_write(h,assert(P.lease_read(h)),{}))
        reskate.console.command('write-retained-max','',function()
            local r=assert(P.lease_write_state(h,v))
            assert(r.phase=='releasing' and r.reason=='exhausted' and r.output_debt and r.quarantined and r.callbacks_unknown)
            assert(r.publications=='18446744073709551615' and r.version_publications=='18446744073709551615')
            assert(r.polls_in_flight==0 and not r.output_released and not P.lease_acknowledge_write(h))
            reskate.log('write-retained-max-complete')
        end)
    )",commands)});passive_lease_fixture::running(host,"write-retained-max");
      auto& row=e.fake.writer;row.phase=write::Phase::releasing;row.reason=write::Reason::exhausted;
      row.publications=row.version_publications=std::numeric_limits<std::uint64_t>::max();
      row.published_revision=row.desired_revision;row.last_written_kind=write::WrittenKind::desired;
      row.output_debt=row.quarantined=row.callbacks_unknown=true;
      check(host.run_console("write-retained-max") && e.occurrences("write-retained-max-complete")==1 && e.fake.used,
          "retained exhausted counter lost exact diagnostic identity or became ACKable"); }
    ++groups;
    { f::Environment e;Host host(e,Side::client);host.apply({script("write-zero-cancel",R"(
        local P=reskate.input;local h=assert(P.lease(0));local v=assert(P.lease_write(h,assert(P.lease_read(h)),{}))
        assert(P.lease_release_write(h));local r=assert(P.lease_write_state(h,v))
        assert(r.phase=='complete' and r.output_released and r.publications=='0' and r.handback_publications=='0')
        assert(r.last_written_kind=='none' and not r.output_debt and P.lease_acknowledge_write(h))
        assert(P.lease_release(h) and P.lease_acknowledge(h))
    )",permission)});passive_lease_fixture::running(host,"write-zero-cancel");
      check(e.fake.write_acks==1 && !e.fake.used,"never-written cancellation fabricated a required publication"); }
    { f::Environment e;Host host(e,Side::client);host.apply({script("write-cancel-drain",R"(
        local P=reskate.input;local h=assert(P.lease(0));local v=assert(P.lease_write(h,assert(P.lease_read(h)),{}))
        reskate.console.command('write-zero-release','',function()
            assert(P.lease_release_write(h));local r=assert(P.lease_write_state(h,v))
            assert(r.phase=='releasing' and not r.output_debt and not r.output_released and r.polls_in_flight==1)
            assert(r.publications=='0' and r.handback_publications=='0' and not P.lease_acknowledge_write(h))
            reskate.log('write-zero-release-complete')
        end)
        reskate.console.command('write-zero-ack','',function()
            local r=assert(P.lease_write_state(h,v));assert(r.output_released and r.publications=='0' and r.handback_publications=='0')
            assert(P.lease_acknowledge_write(h) and P.lease_release(h) and P.lease_acknowledge(h));reskate.log('write-zero-ack-complete')
        end)
    )",commands)});passive_lease_fixture::running(host,"write-cancel-drain");e.fake.writer.polls_in_flight=1;
      check(host.run_console("write-zero-release") && e.occurrences("write-zero-release-complete")==1,
          "valid never-attempted no-debt releasing DTO was rejected or prematurely completed");
      e.fake.handback();check(host.run_console("write-zero-ack") && e.occurrences("write-zero-ack-complete")==1,
          "controlled callback drain did not permit never-attempted zero-publication ACK"); }
    ++groups;
    { f::Environment e;Host host(e,Side::client);host.apply({script("write-library",R"(
        local P=reskate.input;local h=assert(P.lease(1));local s=assert(P.lease_read(h));local v=assert(P.lease_write(h,s,{}))
        reskate.expose({version=function()return v end,state=function(h,v)return P.lease_write_state(h,v)end,sample=function()return s end})
    )",permission),script("write-user",R"(
        local P=reskate.input;local h=assert(P.lease(0))
        reskate.console.command('write-cross','',function()
            local other=assert(reskate.require_plugin('write-library'))
            assert(not pcall(other.version) and not pcall(other.sample) and not pcall(other.state,h,newproxy()))
            reskate.log('write-cross-complete')
        end)
    )",commands)});passive_lease_fixture::running(host,"write-library");passive_lease_fixture::running(host,"write-user");
      check(host.run_console("write-cross") && e.occurrences("write-cross-complete")==1 && !e.fake.write_states && e.fake.write_requests==1,
          "VM serialization transported a version/epoch or reached foreign writer state"); }
    ++groups;
    for(int bad=0;bad<=3;++bad){f::Environment e;e.fake.bad_accept=bad;e.fake.publish_during_request=true;Host host(e,Side::client);
      std::string source=R"(local P=reskate.input;local h=assert(P.lease(0));local v,why=P.lease_write(h,assert(P.lease_read(h)),{});)";
      source+=bad?"assert(v==nil and why=='invalid input write backend result');":"assert(type(v)=='userdata');";
      source+="error('constructed delivery/startup failure after writer request')";
      host.apply({script("write-startup-fault",source,permission)});
      check(status_of(host,"write-startup-fault").state=="stopped" && e.fake.write_requests==1 && e.fake.closes==1 &&
          e.fake.used && e.fake.writer.orphaned && e.fake.writer.output_debt && e.fake.writer.polls_in_flight==1,
          "startup/invalid result lost owner-scoped retained writer debt");
      host.apply({});check(e.fake.closes==1,"writer owner close repeated after VM removal");}
    ++groups;
    {f::Environment e;Host host(e,Side::client);host.apply({script("write-budget",R"(
        local P=reskate.input;local h=assert(P.lease(0));local v=assert(P.lease_write(h,assert(P.lease_read(h)),{}))
        for i=1,61 do assert(P.lease_write_state(h,v)) end
        local ok,why=pcall(P.lease_write_state,h,v);assert(not ok and why:find('64 input lease calls',1,true))
        local done=false;reskate.on('tick',function()if done then return end;done=true
            assert(P.lease_write_state(h,v));assert(P.lease_release_write(h));assert(P.lease_acknowledge_write(h))
            assert(P.lease_release(h) and P.lease_acknowledge(h))
        end)
    )",permission)});passive_lease_fixture::running(host,"write-budget");check(e.fake.write_states==61,"overquota writer state reached backend");
      e.now+=16;host.tick(16);passive_lease_fixture::running(host,"write-budget");
      check(e.fake.write_states==62 && e.fake.write_acks==1,"shared call quota did not reset on normal tick");}
    ++groups;
    {f::Environment e;Host host(e,Side::client);host.apply({script("write-discard",R"(
        local P=reskate.input;local h=assert(P.lease(0));local v=assert(P.lease_write(h,assert(P.lease_read(h)),{}));v=nil
        for i=1,2000 do local discarded={i,i+1,i+2} end
    )",permission)});passive_lease_fixture::running(host,"write-discard");
      check(e.fake.used && !e.fake.write_releases && !e.fake.write_acks && !e.fake.closes,
          "discarding a Version acted as cleanup authority");
      host.apply({});check(e.fake.closes==1,"unreferenced version lost ordinary owner cleanup");}
    {f::Environment e;e.fake.publish_during_request=true;Limits limits;limits.memory=1u<<20;Host host(e,Side::client,limits);
      host.apply({script("write-memory",R"(
        local P=reskate.input;local h=assert(P.lease(0));assert(P.lease_write(h,assert(P.lease_read(h)),{}))
        local keep={};while true do keep[#keep+1]=string.rep('x',200000) end
      )",permission)});
      check(status_of(host,"write-memory").state=="stopped" && e.fake.write_requests==1 && e.fake.closes==1 &&
          e.fake.used && e.fake.writer.orphaned && e.fake.writer.output_debt,
          "actual VM memory halt lost retained owner cleanup");}
    ++groups;
    check(groups==10,"tracked writer Host group accounting");
    std::cout<<"Tracked input writer Host: "<<groups<<" authored groups; controlled DTOs, exact Version-allocation fault untested, native/game unqualified\n";
}
}