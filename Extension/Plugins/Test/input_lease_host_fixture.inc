// Real Host/VM/bindings over constructed passive DTOs. These tests do not run
// an XInput hook, prove native publication or grant capture/actor authority.
namespace {
namespace passive_lease_fixture {
namespace lease = dingosdk::input_lease;
struct Game final : dingosdk::plugins::Game {
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
            assert(P.lease_capture==nil and P.lease_capture_state==nil and P.lease_release_capture==nil and P.lease_acknowledge_capture==nil)
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
                assert(r.capture_reserved==nil and r.report_debt==nil and not P.lease_acknowledge(h));reskate.log('lease-release-complete')
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
