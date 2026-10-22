// Client-thread only; included inside the exact private operator namespace.
namespace markers {
namespace policy=trial_marker;
struct State {policy::Producer producer;bool activation_attempted{},activation_written{},terminal_attempted{},terminal_written{};
    const char* activation_issue="not_attempted";const char* terminal_issue="not_attempted";DWORD last_error{};};
State& state(){static State value;return value;}
void initialize(std::uint64_t generation) noexcept {state().producer.initialize(::dingosdk::runtime::recording_gate::coupled_capture_identity(),generation);}
Json epoch(const native_coupled::policy::Epoch& e){auto bodies=Json::array();for(const auto& b:e.bodies)bodies.push_back(Json{{"world",b.world},{"index",b.index},{"generation",b.generation}});
    return Json{{"owner_world",e.owner_world},{"bodies",std::move(bodies)},{"token",e.token},{"serial",e.serial},{"time",e.time},{"h",e.seconds}};}
Json common(const char* event){const auto& p=state().producer;const auto& id=p.identity();return Json{{"schema",1},{"event",event},
    {"process_id",id.process_id},{"process_creation_filetime",std::to_string(id.creation_filetime)},
    {"capture_nonce",std::string(id.nonce.data(),id.nonce.size())},{"operator_generation",p.generation()}};}
bool write(const Json& value,const wchar_t* suffix,const char*& issue) noexcept {
    auto& s=state();try {
        constexpr wchar_t exact[]=LR"(C:\Users\Jeffr\Desktop\reskate\build\bmx-user-data-coupled-test)";
        const auto root=user_data::override_root();const auto& id=s.producer.identity();
        if(!s.producer.configured()||!root||root->native()!=exact||!user_data::runtime_development_selected(L"bmx-user-data-coupled-test",exact)||id.process_id!=GetCurrentProcessId()){
            issue="marker_scope_or_current_process_invalid";return false;}
        const std::wstring nonce(id.nonce.begin(),id.nonce.end());const auto stem=L"coupled-"+std::to_wstring(id.process_id)+L"-"+std::to_wstring(id.creation_filetime)+L"-"+nonce+suffix;
        const auto destination=*root/stem,temp=*root/(stem+L".tmp");const auto text=value.dump();if(text.empty()||text.size()>65536){issue="marker_size_invalid";return false;}
        const auto file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE){s.last_error=GetLastError();issue="exclusive_marker_temp_create_failed";return false;}
        DWORD written{};const bool complete=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(file);
        const auto error=GetLastError();CloseHandle(file);
        if(!complete){s.last_error=error;DeleteFileW(temp.c_str());issue="marker_write_or_flush_failed";return false;}
        // No REPLACE_EXISTING: immutable final path, same-directory atomic move.
        if(!MoveFileExW(temp.c_str(),destination.c_str(),0)){s.last_error=GetLastError();DeleteFileW(temp.c_str());issue="exclusive_marker_publish_failed";return false;}
        issue="published";return true;
    }catch(...){issue="marker_export_exception";return false;}
}
void activation(const native_coupled::Frame& actual,bool qualified,std::uint64_t begun,std::uint64_t succeeded,bool entry_ready,bool render_verified) noexcept {
    auto& s=state();s.activation_attempted=true;
    if(!s.producer.activation(begun,succeeded,actual.epoch,true,entry_ready,render_verified,qualified,actual.active,native_coupled::status().force_adoption_enabled)){
        s.activation_issue=s.producer.configured()?"activation_proof_not_qualified":"no_valid_released_capture_gate";return;}
    try {auto value=common("coupled_trial_activated");const auto& a=s.producer.activated();
        value["activation_begun_tick_ms"]=a.begun;value["activation_succeeded_tick_ms"]=a.succeeded;value["conservative_expiry_tick_ms"]=a.expiry;value["epoch"]=epoch(a.epoch);
        value["entry_ready"]=true;value["actual_spring_render_verified"]=true;value["backend_active"]=true;value["normal_force_adoption_enabled"]=false;
        s.activation_written=write(value,L"-activation.json",s.activation_issue);
    }catch(...){s.activation_issue="activation_json_exception";}
}
void terminal(const native_coupled::Frame* actual,const char* issue) noexcept {
    auto& s=state();s.terminal_attempted=true;if(!s.producer.terminal()){s.terminal_issue="no_valid_released_capture_gate_or_already_terminal";return;}
    try {auto value=common("coupled_trial_terminal");value["terminal_tick_ms"]=GetTickCount64();value["issue"]=issue;
        value["epoch"]=actual&&s.producer.terminal_epoch(actual->epoch)?epoch(actual->epoch):Json(nullptr);
        s.terminal_written=write(value,L"-terminal.json",s.terminal_issue);
    }catch(...){s.terminal_issue="terminal_json_exception";}
}
Json status(){const auto& s=state();return Json{{"configured",s.producer.configured()},{"activation_attempted",s.activation_attempted},
    {"activation_written",s.activation_written},{"activation_issue",s.activation_issue},{"terminal_attempted",s.terminal_attempted},
    {"terminal_written",s.terminal_written},{"terminal_issue",s.terminal_issue},{"last_win32_error",s.last_error}};}
}
