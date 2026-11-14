// Actual Host/VM/API calls over controlled DTOs, not a native capture simulator.
// The inherited passive fake deliberately permits two owners for VM-isolation
// cases. Native one-live-lease/publication rules are tested by the native lane.
namespace {
namespace selected_capture_fixture {
namespace lease = dingosdk::input_lease;
namespace capture = dingosdk::input_lease_capture;
struct Game final : passive_lease_fixture::Game {
    struct Capture { bool used{}; capture::Receipt receipt{}; } cap;
    unsigned capture_requests{}, capture_states{}, capture_releases{}, capture_acks{};
    int bad_state{}, bad_request{}, bad_passive{};
    bool debt_on_request{};
    std::uint64_t next_generation{9007199254742000ULL}, next_revision{9007199254743000ULL};
    lease::Snapshot admitted{};
    bool matching(std::uint64_t owner, lease::Handle h) const noexcept {
        return cap.used && cap.receipt.key.owner==owner && cap.receipt.key.lease_sequence==h.sequence;
    }
    bool reserved() const noexcept {
        return cap.used && (cap.receipt.phase!=capture::Phase::complete || cap.receipt.debt ||
            cap.receipt.polls_in_flight || cap.receipt.callbacks_unknown);
    }
    void sync() noexcept {
        for(auto& row:records) if(row.used){
            row.receipt.capture_reserved=matching(row.owner,row.receipt.handle) && reserved();
            row.receipt.source_released=lease::terminal(row.receipt.status) && !row.receipt.polls_in_flight && !row.receipt.capture_reserved;
        }
    }
    void activate(unsigned polls=0) noexcept {
        cap.receipt.phase=capture::Phase::capturing;cap.receipt.debt=true;
        cap.receipt.publications=1;cap.receipt.polls_in_flight=polls;++cap.receipt.revision;sync();
    }
    void release_capture() noexcept {
        if(cap.receipt.phase!=capture::Phase::complete){
            cap.receipt.phase=cap.receipt.debt?capture::Phase::releasing:capture::Phase::complete;
            cap.receipt.reason=capture::Reason::cancelled;++cap.receipt.revision;
        }
        sync();
    }
    void witnessed_neutral() noexcept {
        cap.receipt.phase=capture::Phase::complete;cap.receipt.debt=false;
        ++cap.receipt.publications;++cap.receipt.neutral_releases;++cap.receipt.revision;sync();
    }
    void reclaim_orphan() noexcept {
        if(cap.used && cap.receipt.orphaned && !reserved()){
            const auto owner=cap.receipt.key.owner;cap={};
            for(auto& row:records) if(row.used && row.owner==owner && row.receipt.orphaned && !row.receipt.polls_in_flight)row.used=false;
        }
        sync();
    }
    bool input_lease_capture_request(std::uint64_t owner,lease::Handle h,const lease::Snapshot& sample,capture::Reason& why) noexcept override {
        ++capture_requests;auto* row=find(owner,h);
        if(!row){why=capture::Reason::unavailable;return false;}
        if(cap.used){why=capture::Reason::busy;return false;}
        if(sample!=row->issued){why=capture::Reason::source_stale;return false;}
        cap.used=true;cap.receipt.key={owner,h.sequence,sample.source_epoch,sample.context_generation,row->receipt.user};
        cap.receipt.phase=capture::Phase::waiting_neutral;cap.receipt.generation=++next_generation;
        cap.receipt.revision=++next_revision;cap.receipt.expires_ms=row->receipt.expires_ms;admitted=sample;
        if(debt_on_request || bad_request)activate(1);
        sync();why=capture::Reason::none;
        if(bad_request==1){why=capture::Reason::busy;return false;}
        if(bad_request==2){why=capture::Reason::busy;return true;}
        if(bad_request==3){why=static_cast<capture::Reason>(99);return false;}
        return true;
    }
    bool input_lease_capture_state(std::uint64_t owner,lease::Handle h,capture::Receipt& out) noexcept override {
        ++capture_states;if(!matching(owner,h))return false;out=cap.receipt;
        switch(bad_state){
        case 1:++out.key.owner;break;case 2:++out.key.lease_sequence;break;case 3:out.key.user=4;break;
        case 4:out.key.source_epoch=0;break;case 5:out.key.context_generation=0;break;
        case 6:out.phase=static_cast<capture::Phase>(99);break;case 7:out.reason=static_cast<capture::Reason>(99);break;
        case 8:out.generation=0;break;case 9:out.revision=0;break;case 10:out.expires_ms=0;break;
        case 11:out.polls_in_flight=129;break;case 12:out.neutral_releases=out.publications+1;break;
        case 13:out.phase=capture::Phase::idle;break;case 14:out.phase=capture::Phase::complete;out.debt=true;break;
        case 15:out.phase=capture::Phase::waiting_neutral;out.debt=true;break;
        case 16:out.phase=capture::Phase::waiting_neutral;out.publications=1;break;
        case 17:out.phase=capture::Phase::waiting_neutral;out.quarantined=true;break;
        case 18:out.phase=capture::Phase::capturing;out.debt=false;out.publications=1;break;
        case 19:out.phase=capture::Phase::capturing;out.debt=true;out.publications=0;break;
        case 20:out.phase=capture::Phase::releasing;out.debt=false;break;
        case 21:out.phase=capture::Phase::waiting_neutral;out.orphaned=true;break;
        case 22:out.phase=capture::Phase::capturing;out.orphaned=true;out.debt=true;out.publications=1;break;
        case 23:out.callbacks_unknown=true;out.phase=capture::Phase::releasing;out.debt=true;out.quarantined=false;break;
        case 24:out.callbacks_unknown=true;out.phase=capture::Phase::complete;out.debt=false;out.quarantined=true;break;
        case 25:out.callbacks_unknown=true;out.phase=capture::Phase::capturing;out.debt=true;out.publications=1;out.quarantined=true;break;
        case 26:out.key.user=(out.key.user+1)%4;break;
        case 27:out.phase=capture::Phase::capturing;out.debt=true;out.publications=1;out.quarantined=true;out.callbacks_unknown=false;break;
        default:break;
        }
        return true;
    }
    bool input_lease_capture_release(std::uint64_t owner,lease::Handle h) noexcept override {
        ++capture_releases;if(!matching(owner,h))return false;release_capture();return true;
    }
    bool input_lease_capture_acknowledge(std::uint64_t owner,lease::Handle h) noexcept override {
        ++capture_acks;if(!matching(owner,h) || cap.receipt.phase!=capture::Phase::complete || cap.receipt.debt ||
            cap.receipt.polls_in_flight || cap.receipt.callbacks_unknown)return false;
        cap={};sync();return true;
    }
    bool input_lease_state(std::uint64_t owner,lease::Handle h,lease::Receipt& out,lease::Reason& why) noexcept override {
        if(!passive_lease_fixture::Game::input_lease_state(owner,h,out,why))return false;
        sync();auto* row=find(owner,h);out=row->receipt;
        if(bad_passive==1){out.status=lease::Status::released;out.reason=lease::Reason::cancelled;out.capture_reserved=true;out.source_released=true;out.polls_in_flight=0;}
        if(bad_passive==2){out.status=lease::Status::warming;out.reason=lease::Reason::none;out.last_observation=out.observed_ms=out.renewals=0;out.capture_reserved=true;out.source_released=false;}
        return true;
    }
    bool input_lease_release(std::uint64_t owner,lease::Handle h,lease::Reason& why) noexcept override {
        const bool ok=passive_lease_fixture::Game::input_lease_release(owner,h,why);
        if(ok && matching(owner,h))release_capture();sync();return ok;
    }
    bool input_lease_acknowledge(std::uint64_t owner,lease::Handle h,lease::Reason& why) noexcept override {
        ++acks;auto* row=find(owner,h);if(!row){why=lease::Reason::unavailable;return false;}
        refresh(*row);sync();
        if(!lease::terminal(row->receipt.status) || !row->receipt.source_released || row->receipt.polls_in_flight || row->receipt.capture_reserved){why=lease::Reason::busy;return false;}
        row->used=false;why=lease::Reason::none;return true;
    }
    void input_lease_release_all(std::uint64_t owner) noexcept override {
        ++closes;
        if(cap.used && cap.receipt.key.owner==owner){cap.receipt.orphaned=true;release_capture();}
        for(auto& row:records)if(row.used && row.owner==owner){
            row.receipt.status=lease::Status::revoked;row.receipt.reason=lease::Reason::cancelled;row.receipt.orphaned=true;
        }
        sync();reclaim_orphan();
        for(auto& row:records)if(row.used && row.owner==owner && row.receipt.orphaned && row.receipt.source_released)row.used=false;
    }
    unsigned capture_calls() const noexcept {return capture_requests+capture_states+capture_releases+capture_acks;}
};
struct Environment final : dingosdk::plugins::Environment {
    Game fake;std::vector<std::string> logs;double now{1000};
    void log(LogLevel,std::string_view value) override {logs.emplace_back(value);}
    void console(std::string_view) override {}
    double now_ms() override {return now;}
    std::optional<bool> approved(const Candidate&) override {return std::nullopt;}
    std::filesystem::path storage_root() override {return scratch/"selected-stream-capture";}
    bool builtin_command(std::string_view) override {return false;}
    dingosdk::plugins::Game& game() override {return fake;}
    std::size_t occurrences(std::string_view word) const {return static_cast<std::size_t>(std::count_if(logs.begin(),logs.end(),[&](const auto& line){return line.find(word)!=std::string::npos;}));}
};
}
void selected_stream_capture_host_fixture_v1(){
    namespace f=selected_capture_fixture;namespace capture=dingosdk::input_lease_capture;
    const std::string permission=R"("input.lease","input.capture")";
    const std::string commands=R"("input.lease","input.capture","console")";
    unsigned groups{};
    const auto prompt=approval_prompt(permission_groups(parse_manifest(manifest(permission))));
    // One INPUT summary capitalizes only its first action: lease then capture.
    check(prompt.find("Reserve and read one observed controller stream, without capturing or changing input")!=std::string::npos &&
        prompt.find("capture one selected controller stream, with neutral-release cleanup")!=std::string::npos &&
        prompt.find("bind keys")==std::string::npos && prompt.find("Bind keys")==std::string::npos &&
        prompt.find("Record your screen")==std::string::npos && prompt.find("record your screen")==std::string::npos,
        "combined lease/capture approval misrepresented permission scope");
    const auto capture_prompt=approval_prompt(permission_groups(parse_manifest(manifest(R"("input.capture")"))));
    check(capture_prompt.find("Capture one selected controller stream, with neutral-release cleanup")!=std::string::npos &&
        capture_prompt.find("bind keys")==std::string::npos && capture_prompt.find("Bind keys")==std::string::npos &&
        capture_prompt.find("Record your screen")==std::string::npos && capture_prompt.find("record your screen")==std::string::npos,
        "capture-only approval omitted its title or claimed unrelated input/screen access");
    // 1. Every missing permission is refused before args/backend/owner cleanup.
    for(const auto& granted:{std::string{},std::string{R"("input")"},std::string{R"("input.lease")"},std::string{R"("capture")"},std::string{R"("input.capture")"}}){
        f::Environment e;Host host(e,Side::client);
        const auto needed=granted==R"("input.capture")"?"input.lease":"input.capture";
        const auto source=std::string("local needed='")+needed+"';"+R"(
            for _,name in {'lease_capture','lease_capture_state','lease_release_capture','lease_acknowledge_capture'} do
                local ok,why=pcall(reskate.input[name],{},{},{})
                assert(not ok and why:find(needed,1,true),name)
            end
        )";
        host.apply({script("capture-denied",source,granted)});
        passive_lease_fixture::running(host,"capture-denied");host.apply({});
        check(!e.fake.calls() && !e.fake.capture_calls() && !e.fake.closes,"capture permission denial reached backend/cleanup");
    }
    ++groups;
    // 2. Client phase precedes malformed arguments even when both permissions exist.
    {f::Environment e;Host host(e,Side::server);host.apply({package("capture-server",{{manifest_file,manifest(permission,"server")},{"plugin/main.lua",R"(
        for _,name in {'lease_capture','lease_capture_state','lease_release_capture','lease_acknowledge_capture'} do
            local ok,why=pcall(reskate.input[name],{},{},{})
            assert(not ok and why:find('client phase only',1,true),name)
        end
    )"}})});passive_lease_fixture::running(host,"capture-server");host.apply({});
    check(!e.fake.calls() && !e.fake.capture_calls() && !e.fake.closes,"server reached capture backend");}++groups;
    // 3. Existing locked handles/epochs, exact arity and plain-table checks.
    {f::Environment e;Host host(e,Side::client);host.apply({script("capture-arguments",R"(
        local P=reskate.input
        for _,name in {'lease_capture','lease_capture_state','lease_release_capture','lease_acknowledge_capture'} do
            assert(not pcall(P[name]) and not pcall(P[name],{}) and not pcall(P[name],newproxy()))
        end
        local h=assert(P.lease(0));local s=assert(P.lease_read(h))
        assert(not pcall(P.lease_capture,h) and not pcall(P.lease_capture,h,s,nil))
        assert(not pcall(P.lease_capture,h,{}) and not pcall(P.lease_capture,h,{epoch=newproxy()}))
        assert(not pcall(P.lease_capture,h,setmetatable(table.clone(s),{})))
        for _,name in {'lease_capture_state','lease_release_capture','lease_acknowledge_capture'} do assert(not pcall(P[name],h,{})) end
    )",permission)});passive_lease_fixture::running(host,"capture-arguments");check(!e.fake.capture_calls(),"malformed capture arguments reached backend");}++groups;
    // 4. Exact opaque issued snapshot, readonly receipts and large decimal u64s.
    {f::Environment e;Host host(e,Side::client);host.apply({script("capture-snapshot",R"(
        local P=reskate.input;local h=assert(P.lease(2));local old=assert(P.lease_read(h));local s=assert(P.lease_read(h))
        local ok,why=P.lease_capture(h,old);assert(ok==false and why=='source_stale')
        local clone=table.clone(s);clone.sequence='1';clone.user=0;clone.pad={buttons=0}
        local admitted=table.pack(P.lease_capture(h,clone));assert(admitted.n==1 and admitted[1]==true)
        local r=assert(P.lease_capture_state(h))
        assert(r.sequence=='9007199254741001' and r.user==2 and r.source_epoch=='9007199254741019' and r.context_generation=='9007199254741021')
        assert(r.generation=='9007199254742001' and r.revision=='9007199254743001' and r.publications=='0' and r.neutral_releases=='0')
        assert(r.phase=='waiting_neutral' and not r.debt and not r.callbacks_unknown and not r.quarantined and r.polls_in_flight==0)
        assert(r.owner==nil and r.key==nil and r.mode=='capture' and r.source_identity=='observed_stream' and r.native_consumption=='unobserved' and r.physical_device_identity=='unobserved')
        assert(not pcall(function()r.phase='complete'end))
        reskate.console.command('capture-max','',function()
            local row=assert(P.lease_capture_state(h));assert(row.expires_ms=='18446744073709551615' and row.generation=='18446744073709551615');reskate.log('capture-max-complete')
        end)
    )",commands)});passive_lease_fixture::running(host,"capture-snapshot");
    check(e.fake.capture_requests==2 && e.fake.admitted==e.fake.records[0].issued,"public sample projections changed native capture authority");
    e.fake.cap.receipt.expires_ms=e.fake.cap.receipt.generation=std::numeric_limits<std::uint64_t>::max();
    check(host.run_console("capture-max") && e.occurrences("capture-max-complete")==1,"near-limit valid capture receipt refused");}++groups;
    // 5. Complete is not drained; passive and capture ACK duties remain distinct.
    {f::Environment e;Host host(e,Side::client);host.apply({script("capture-drain",R"(
        local P=reskate.input;local h=assert(P.lease(0));assert(P.lease_capture(h,assert(P.lease_read(h))))
        reskate.console.command('capture-release','',function()
            local c=assert(P.lease_capture_state(h));assert(c.phase=='capturing' and c.debt and c.polls_in_flight==1)
            assert(P.lease_state(h).capture_reserved);assert(P.lease_release_capture(h));assert(P.lease_release(h))
            local r=assert(P.lease_state(h));assert(r.status=='released' and r.capture_reserved and not r.source_released)
            assert(not P.lease_acknowledge(h) and not P.lease_acknowledge_capture(h));reskate.log('capture-release-complete')
        end)
        reskate.console.command('capture-complete','',function()
            local c=assert(P.lease_capture_state(h));assert(c.phase=='complete' and not c.debt and c.polls_in_flight==1)
            local r=assert(P.lease_state(h));assert(r.capture_reserved and not r.source_released)
            assert(not P.lease_acknowledge_capture(h) and not P.lease_acknowledge(h));reskate.log('capture-complete-complete')
        end)
        reskate.console.command('capture-ack','',function()
            local r=assert(P.lease_state(h));assert(r.source_released and not r.capture_reserved)
            assert(P.lease_acknowledge(h));assert(P.lease_capture_state(h).phase=='complete')
            assert(P.lease_acknowledge_capture(h));local gone,why=P.lease_capture_state(h);assert(gone==nil and why=='unavailable');reskate.log('capture-ack-complete')
        end)
    )",commands)});passive_lease_fixture::running(host,"capture-drain");e.fake.activate(1);
    check(host.run_console("capture-release") && e.occurrences("capture-release-complete")==1,"capture release debt protocol");
    e.fake.witnessed_neutral();check(host.run_console("capture-complete") && e.occurrences("capture-complete-complete")==1,"completed pending callback proof");
    e.fake.cap.receipt.polls_in_flight=0;e.fake.sync();check(host.run_console("capture-ack") && e.occurrences("capture-ack-complete")==1 && !e.fake.cap.used,"distinct ACK after real fixture drain");
    host.apply({});check(e.fake.closes==1,"capture duplicated touched-owner close");}++groups;
    // 6. Twenty-seven malformed capture modes, two passive capture-reservation relations.
    {f::Environment e;Host host(e,Side::client);host.apply({script("capture-malformed",R"(
        local P=reskate.input;local h=assert(P.lease(0));assert(P.lease_capture(h,assert(P.lease_read(h))))
        reskate.console.command('capture-bad','',function()local v,why=P.lease_capture_state(h);assert(v==nil and why=='invalid input capture backend receipt');reskate.log('capture-bad-complete')end)
        reskate.console.command('capture-bad-passive','',function()local v,why=P.lease_state(h);assert(v==nil and why=='invalid input lease backend receipt');reskate.log('capture-bad-passive-complete')end)
        reskate.console.command('capture-unknown','',function()
            local c=assert(P.lease_capture_state(h));assert(c.callbacks_unknown and c.quarantined and c.debt and c.phase=='releasing' and c.polls_in_flight==0)
            assert(not P.lease_acknowledge_capture(h) and P.lease_state(h).capture_reserved);reskate.log('capture-unknown-complete')
        end)

    )",commands)});passive_lease_fixture::running(host,"capture-malformed");
    for(int mode=1;mode<=27;++mode){e.fake.bad_state=mode;check(host.run_console("capture-bad") && e.occurrences("capture-bad-complete")==static_cast<std::size_t>(mode),"malformed capture case not completed");}
    e.fake.bad_state=0;for(int mode=1;mode<=2;++mode){e.fake.bad_passive=mode;check(host.run_console("capture-bad-passive") && e.occurrences("capture-bad-passive-complete")==static_cast<std::size_t>(mode),"malformed passive capture-reservation relation");}
    e.fake.bad_passive=0;e.fake.cap.receipt.phase=capture::Phase::releasing;e.fake.cap.receipt.debt=true;e.fake.cap.receipt.quarantined=true;e.fake.cap.receipt.callbacks_unknown=true;
    check(host.run_console("capture-unknown") && e.occurrences("capture-unknown-complete")==1,"unknown callback duty became clean");
    host.apply({});check(e.fake.cap.used && e.fake.cap.receipt.callbacks_unknown && e.fake.cap.receipt.orphaned && e.fake.reserved(),"owner close cleared unknown callback duty");}
    // Independent known-lost-source DTO: this is not a transition clearing the
    // preceding record's sticky unknown duty, and no neutral witness is invented.
    {f::Environment e;Host host(e,Side::client);host.apply({script("capture-quarantine",R"(
        local P=reskate.input;local h=assert(P.lease(0));assert(P.lease_capture(h,assert(P.lease_read(h))))
        reskate.console.command('capture-quarantine','',function()
            local c=assert(P.lease_capture_state(h));assert(c.phase=='complete' and c.quarantined and not c.debt and not c.callbacks_unknown and c.neutral_releases=='0');reskate.log('capture-quarantine-complete')
        end)
    )",commands)});passive_lease_fixture::running(host,"capture-quarantine");
    e.fake.cap.receipt.phase=capture::Phase::complete;e.fake.cap.receipt.debt=false;e.fake.cap.receipt.quarantined=true;e.fake.cap.receipt.publications=1;
    check(host.run_console("capture-quarantine") && e.occurrences("capture-quarantine-complete")==1,"quarantine diagnostics were misreported as neutral witness");}++groups;
    // 7. Partial/invalid request and startup errors preserve pending owner duty.
    for(int mode=0;mode<=3;++mode){f::Environment e;e.fake.bad_request=mode;e.fake.debt_on_request=true;Host host(e,Side::client);
    const auto source=std::string(R"(local P=reskate.input;local h=assert(P.lease(0));local s=assert(P.lease_read(h));local ok,why=P.lease_capture(h,s);)")+
        (mode==0?"assert(ok==true)":mode==1?"assert(ok==false and why=='busy')":mode==2?"assert(ok==false and why=='invalid input capture backend result')":"assert(ok==false and why=='unavailable')")+
        ";error('controlled failure with capture debt')";
    host.apply({script("capture-startup-fault",source,permission)});
    check(status_of(host,"capture-startup-fault").state=="stopped" && e.fake.closes==1 && e.fake.cap.used && e.fake.cap.receipt.orphaned &&
        e.fake.cap.receipt.phase==capture::Phase::releasing && e.fake.cap.receipt.debt && e.fake.cap.receipt.polls_in_flight==1 && e.fake.records[0].used,
        "startup/partial failure erased capture cleanup duty");host.apply({});check(e.fake.closes==1,"owner cleanup called twice");
    e.fake.witnessed_neutral();e.fake.cap.receipt.polls_in_flight=0;e.fake.reclaim_orphan();check(!e.fake.cap.used && !e.fake.records[0].used,"exact controlled neutral/drain did not reclaim orphan");}++groups;
    // 8. Capture calls share the existing64-call interval; normal tick resets it.
    {f::Environment e;Host host(e,Side::client);host.apply({script("capture-budget",R"(
        local P=reskate.input;local h=assert(P.lease(0));assert(P.lease_capture(h,assert(P.lease_read(h))))
        for i=1,61 do assert(P.lease_capture_state(h)) end
        local ok,why=pcall(P.lease_capture_state,h);assert(not ok and why:find('64 input lease calls',1,true))
        local done=false;reskate.on('tick',function()if done then return end;done=true
            assert(P.lease_capture_state(h));assert(P.lease_release_capture(h));assert(P.lease_acknowledge_capture(h));assert(P.lease_release(h));assert(P.lease_acknowledge(h))
        end)
    )",permission)});passive_lease_fixture::running(host,"capture-budget");check(e.fake.capture_states==61,"capture overbudget reached backend");
    e.now+=16;host.tick(16);passive_lease_fixture::running(host,"capture-budget");check(e.fake.capture_states==62 && e.fake.capture_acks==1,"normal tick did not reset shared budget");}++groups;
    // 9. Inter-VM serialization cannot transport either opaque handle/epoch.
    {f::Environment e;Host host(e,Side::client);host.apply({script("capture-library",R"(
        local P=reskate.input;reskate.expose({state=function(h)return P.lease_capture_state(h)end,make=function()local h=assert(P.lease(1));assert(P.lease_read(h));return h end})
    )",permission),script("capture-user",R"(
        local P=reskate.input;local h=assert(P.lease(0));assert(P.lease_capture(h,assert(P.lease_read(h))))
        reskate.console.command('capture-cross','',function()
            local other=assert(reskate.require_plugin('capture-library'))
            assert(not pcall(other.state,h) and not pcall(other.state,{}));assert(not pcall(other.make));reskate.log('capture-cross-complete')
        end)
    )",commands)});passive_lease_fixture::running(host,"capture-library");passive_lease_fixture::running(host,"capture-user");
    check(host.run_console("capture-cross") && e.occurrences("capture-cross-complete")==1 && !e.fake.capture_states && e.fake.requests==2,"opaque capture capability crossed VM owner boundary");
    check(e.fake.records[0].owner!=e.fake.records[1].owner,"two VMs shared an owner");}++groups;
    // 10. Portable/default capture facade refuses without a native backend.
    {passive_lease_fixture::Environment e;Host host(e,Side::client);host.apply({script("capture-unavailable",R"(
        local P=reskate.input;local h=assert(P.lease(0));local s=assert(P.lease_read(h))
        local ok,why=P.lease_capture(h,s);assert(ok==false and why=='unavailable')
        local r;r,why=P.lease_capture_state(h);assert(r==nil and why=='unavailable')
        ok,why=P.lease_release_capture(h);assert(ok==false and why=='unavailable')
        ok,why=P.lease_acknowledge_capture(h);assert(ok==false and why=='unavailable')
    )",permission)});passive_lease_fixture::running(host,"capture-unavailable");}++groups;
    check(groups==10,"capture Host fixture group accounting");
    std::cout<<"Selected-stream capture Host: "<<groups<<" authored groups; controlled DTOs, native publication unrun\n";
}
}