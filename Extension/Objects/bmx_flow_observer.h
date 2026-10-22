#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dingosdk::profile_runtime::bmx_flow {
struct StartupContract {
    std::uint32_t key;std::array<std::uint32_t,10> layout;
    unsigned binding_offset;std::array<std::uint32_t,2> binding;
    const char* name;const char* scalar_name;
};
// Exact compiled-resource identities from the supported local game data. The
// action graphs are authored state-entry breadcrumbs, not a reader of the ESM's
// current active-state set. The root condition is sampled only after PC0 returns.
inline constexpr std::uint32_t level_class_condition_key=0xcdc24973;
inline constexpr std::array<std::uint32_t,14> level_class_condition_tail{
    0xb401,0,0,2,0x20,2,0x21,0xcc32,1,0,2,0x21,0,0x2c};
inline constexpr std::array<std::uint32_t,2> level_class_condition_binding{0xd1ee09f1,0x98};
inline constexpr std::uintptr_t bool_copy_rva=0x14b0350;
inline constexpr std::array<std::uint8_t,6> bool_copy_prefix{0x0f,0xb6,0x01,0x88,0x02,0xc3};
inline constexpr std::uint32_t dependencies_condition_key=0x5749c298;
inline constexpr std::array<std::uint32_t,75> dependencies_code{
    0x1400,0,0,2,9,0x2800,0,0,2,0xa,0x4c02,0,0,2,9,2,
    0xa,2,0xb,0x601e,2,0xb,2,8,0x7400,0,0,2,0xc,0x9802,0,0,
    2,8,2,0xc,2,0xd,0xac00,0,0,2,0,0xd002,0,0,2,0,
    0,0,2,0xe,0xf402,0,0,2,0xd,2,0xe,2,0xf,0x11001,0,0,
    2,0xf,2,0x10,0x12832,1,0,2,0x10,0,0x2c};
inline constexpr std::array<std::uint32_t,18> dependencies_bindings{
    0x5cc8bee9,0,0xed285e1b,0x14,0xeabfec6f,0x28,0x6e93a795,0x60,
    0x8eb2196d,0x74,0x002970f7,0x98,0x8d5ff168,0xac,0x8eb2196d,0xd0,0xd1ee09f1,0xf4};
struct NativeRelocation {
    unsigned pc;std::uintptr_t rva;unsigned prefix_size;std::array<std::uint8_t,24> prefix;
};
// Exact fast-invocation adapters from the supported image, not the different
// return-by-value functions behind them. The VM calls these relocated pointers.
inline constexpr std::array<NativeRelocation,9> dependencies_relocations{{
    {0,0x59c1550,22,{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xe8,0x42,0xc4,0xd9,0xfa,0x88,0x03,0x48,0x83,0xc4,0x20,0x5b,0xc3}},
    {0x14,0x59c1530,22,{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xe8,0x42,0xc4,0xd9,0xfa,0x88,0x03,0x48,0x83,0xc4,0x20,0x5b,0xc3}},
    {0x28,0x16eb6e0,20,{0x80,0x39,0,0x75,0x0a,0x80,0x3a,0,0x75,5,0x41,0xc6,0,0,0xc3,0x41,0xc6,0,1,0xc3}},
    {0x60,0x5999820,22,{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xe8,0x82,0xac,0xeb,0xfa,0x88,0x03,0x48,0x83,0xc4,0x20,0x5b,0xc3}},
    {0x74,0x16e7130,20,{0x80,0x39,0,0x74,0x0a,0x80,0x3a,0,0x74,5,0x41,0xc6,0,1,0xc3,0x41,0xc6,0,0,0xc3}},
    {0x98,0x5c4d6f0,5,{0xe9,0xbb,0x42,0x21,0xfd}},
    {0xac,0x2d6d630,24,{0x40,0x53,0x48,0x83,0xec,0x20,0x49,0x8b,0xd8,0x33,0xc0,0x4c,0x8b,0xc1,0x48,0x8b,0x0a,0x48,0x85,0xc9,0x74,8,0x49,0x8b}},
    {0xd0,0x16e7130,20,{0x80,0x39,0,0x74,0x0a,0x80,0x3a,0,0x74,5,0x41,0xc6,0,1,0xc3,0x41,0xc6,0,0,0xc3}},
    {0xf4,0x14b0350,6,{0x0f,0xb6,1,0x88,2,0xc3}}
}};
inline constexpr std::array<StartupContract,11> startup_contracts{{
    {0x129086b2,{64,144,116,129,4,0,1,1310729,2949131,0},0x45c,{0x09ed0cc5,0x1e4},
        "Flow/StateMachines/Main/AsyncPrepareManager_ConstructorWrapper","current_realm"},
    {0x4f83ddd3,{48,64,1,32,4,0,1,3,0,0},0x118,{0xe3025263,0x10},
        "Flow/StateMachines/Main/AsyncPrepareManager__CallbackEventObserverData_6108e508-a0a1-423d-aa10-be36313e0538","error_code_is_zero"},
    {0x7a374bb6,{96,256,221,413,7,0,2,655389,3473441,2048},0x954,{0x3c29212a,0x38},
        "Gameplay/Characters/Data/RecipeManager/PlayerRecipeManager__OnGameCoreStartup_InitialSpawnPlayer",""},
    {0x8e8a8b37,{48,200,17,83,4,0,4,8,720896,0},0x2e4,{0x90c8f816,0x100},
        "Flow/StateMachines/Main/Flow_SpawnPlayer/BA279F7_AG_CommandActionData_0_OnBegin",""},
    {0x5aa1d33a,{48,40,41,63,3,0,0,327685,2293760,0},0x234,{0x575ea096,0xdc},
        "Flow/StateMachines/Main/Flow_Root/A66D949_AG_CommandActionData_0_OnBegin",""},
    {0xbe6020e7,{48,64,0,16,3,0,1,1,0,0},0xd8,{0x3c0313ad,0x10},
        "Flow/StateMachines/Main/Flow_Async_Prepare/AE129BF_AG_CommandActionData_0_OnBegin",""},
    {0x13e251da,{64,448,153,386,10,0,7,983063,3014678,16843776},0x9a0,{0xc01b0c8a,0x10},
        "Flow/StateMachines/Main/Flow_Root/B4FC981_AG_CommandActionData_0_OnBegin",""},
    {0xfca4ac75,{48,104,9,32,3,0,1,2,720896,0},0x178,{0xbae9993d,0x58},
        "Flow/StateMachines/Main/Flow_Root/AB9099F_AG_CommandActionData_0_OnBegin",""},
    {0x74faadb7,{48,96,33,54,3,0,1,327685,2228224,0},0x230,{0x002970f7,0x10},
        "Flow/StateMachines/Main/Flow_Root/B12792C_AG_CommandActionData_0_OnBegin",""},
    {level_class_condition_key,{48,24,34,52,3,0,0,327685,2228224,0},0x1d8,{0x002970f7,0},
        "Flow/StateMachines/Main/Flow_Root/C8FAABC_CG","level_class_condition"},
    {dependencies_condition_key,{48,8,17,75,3,0,0,9,720896,0},0x1b4,{0x5cc8bee9,0},
        "Flow/StateMachines/Main/DingoFlow_ScreenManager_Root/B5909DD_CG","dependencies_ready"}
}};
enum class ResourceMatch { matched,layout_rejected,binding_rejected };
struct ResourceEvidence {
    std::uintptr_t resource{};
    std::uint32_t key{};
    std::array<std::uint32_t,2> first_binding{},final_binding{};
    std::array<std::uint32_t,14> tail{};
    std::array<std::uint32_t,75> dependencies_code{};
    std::array<std::uint32_t,18> dependencies_bindings{};
    bool layout_matched{},first_readable{},tail_attempted{},tail_readable{},final_readable{},coherent{};
    bool dependencies_attempted{},dependencies_code_readable{},dependencies_bindings_readable{};
};
template<class Read> ResourceEvidence startup_resource_evidence(std::uint32_t key,
    std::uintptr_t resource,Read&& read) noexcept {
    ResourceEvidence result;
    const auto found=std::find_if(startup_contracts.begin(),startup_contracts.end(),[&](const auto& c){return c.key==key;});
    if(found==startup_contracts.end())return result;
    const auto bytes=std::max<std::uintptr_t>((key==level_class_condition_key||key==dependencies_condition_key)?0x200:0x48,
        std::uintptr_t{found->binding_offset}+8);
    if(resource<0x10000||resource>0x00007fffffffffffULL-bytes)return result;
    std::array<std::uint32_t,10> layout{};
    if(!read(resource+0x10,&result.key,sizeof(result.key))||result.key!=key||
        !read(resource+0x20,layout.data(),sizeof(layout))||layout!=found->layout)return result;
    result.resource=resource;result.layout_matched=true;
    result.first_readable=read(resource+found->binding_offset,result.first_binding.data(),sizeof(result.first_binding));
    if(key==level_class_condition_key){
        result.tail_attempted=true;
        result.tail_readable=read(resource+0x1a0,result.tail.data(),sizeof(result.tail));
        result.final_readable=read(resource+0x1f8,result.final_binding.data(),sizeof(result.final_binding));
    }
    if(key==dependencies_condition_key){
        result.dependencies_attempted=true;
        result.dependencies_code_readable=read(resource+0x88,result.dependencies_code.data(),sizeof(result.dependencies_code));
        result.dependencies_bindings_readable=read(resource+0x1b4,result.dependencies_bindings.data(),sizeof(result.dependencies_bindings));
    }
    std::uint32_t current_key{};std::array<std::uint32_t,10> current_layout{};
    std::array<std::uint32_t,2> first{},last{};std::array<std::uint32_t,14> tail{};
    result.coherent=result.first_readable&&
        read(resource+found->binding_offset,first.data(),sizeof(first))&&first==result.first_binding;
    if(result.tail_attempted)result.coherent=result.coherent&&result.tail_readable&&result.final_readable&&
        read(resource+0x1a0,tail.data(),sizeof(tail))&&tail==result.tail&&
        read(resource+0x1f8,last.data(),sizeof(last))&&last==result.final_binding;
    if(result.dependencies_attempted){
        std::array<std::uint32_t,75> code{};std::array<std::uint32_t,18> bindings{};
        result.coherent=result.coherent&&result.dependencies_code_readable&&result.dependencies_bindings_readable&&
            read(resource+0x88,code.data(),sizeof(code))&&code==result.dependencies_code&&
            read(resource+0x1b4,bindings.data(),sizeof(bindings))&&bindings==result.dependencies_bindings;
    }
    // Identity is checked after all content rereads. Never claim that a later
    // allocation with a different key/layout is the same observed resource.
    result.coherent=result.coherent&&read(resource+0x10,&current_key,sizeof(current_key))&&current_key==key&&
        read(resource+0x20,current_layout.data(),sizeof(current_layout))&&current_layout==layout;
    return result;
}
template<class Read> ResourceMatch match_startup_resource(const StartupContract& contract,
    std::uintptr_t resource,Read&& read,std::uintptr_t image_base=0) noexcept {
    constexpr std::uintptr_t highest_address=0x00007fffffffffffULL;
    const auto bytes=std::max<std::uintptr_t>((contract.key==level_class_condition_key||contract.key==dependencies_condition_key)?0x200:0x48,
        std::uintptr_t{contract.binding_offset}+8);
    std::array<std::uint32_t,10> layout{};std::array<std::uint32_t,2> binding{};
    if(resource<0x10000||bytes>highest_address||resource>highest_address-bytes||
        !read(resource+0x20,layout.data(),sizeof(layout))||layout!=contract.layout)return ResourceMatch::layout_rejected;
    if(!read(resource+contract.binding_offset,binding.data(),sizeof(binding))||binding!=contract.binding)
        return ResourceMatch::binding_rejected;
    if(contract.key==level_class_condition_key){
        std::array<std::uint32_t,14> tail{};std::array<std::uint8_t,6> prefix{};
        if(image_base<0x10000||image_base>highest_address-bool_copy_rva-prefix.size()||
            !read(image_base+bool_copy_rva,prefix.data(),prefix.size())||prefix!=bool_copy_prefix)
            return ResourceMatch::binding_rejected;
        auto expected=level_class_condition_tail;
        // The loader replaces serialized native-call words 1/2 with its direct
        // function pointer. NativeFunctionInfo7158b18 binds d1ee09f1 to this exact
        // two-argument byte-copy implementation; never ignore these two words.
        const auto target=image_base+bool_copy_rva;
        expected[1]=static_cast<std::uint32_t>(target);expected[2]=static_cast<std::uint32_t>(target>>32);
        if(!read(resource+0x1a0,tail.data(),sizeof(tail))||tail!=expected||
            !read(resource+0x1f8,binding.data(),sizeof(binding))||binding!=level_class_condition_binding)
            return ResourceMatch::binding_rejected;
    }
    if(contract.key==dependencies_condition_key){
        auto expected=dependencies_code;
        for(const auto& relocation:dependencies_relocations){
            std::array<std::uint8_t,24> prefix{};
            if(image_base<0x10000||image_base>highest_address-relocation.rva-relocation.prefix_size||
                !read(image_base+relocation.rva,prefix.data(),relocation.prefix_size)||
                !std::equal(prefix.begin(),prefix.begin()+relocation.prefix_size,relocation.prefix.begin()))
                return ResourceMatch::binding_rejected;
            const auto target=image_base+relocation.rva;
            expected[relocation.pc/4+1]=static_cast<std::uint32_t>(target);
            expected[relocation.pc/4+2]=static_cast<std::uint32_t>(target>>32);
        }
        std::array<std::uint32_t,75> code{};std::array<std::uint32_t,18> bindings{};
        if(!read(resource+0x88,code.data(),sizeof(code))||code!=expected||
            !read(resource+0x1b4,bindings.data(),sizeof(bindings))||bindings!=dependencies_bindings)
            return ResourceMatch::binding_rejected;
    }
    return ResourceMatch::matched;
}
template<class Read> bool read_level_class_condition_result(std::uint32_t key,std::uint32_t pc,
    bool after,std::uintptr_t locals,Read&& read,std::uint32_t& value) noexcept {
    // EBX declares ConditionOut as primitive bool. Native Bool copy at PC0x98
    // writes L21; the exact guarded tail then stores L21 through argument S0.
    if(key!=level_class_condition_key||pc||!after||locals<0x10000||locals>0x00007fffffffffffULL-0x22)return false;
    std::uint8_t result{};
    if(!read(locals+0x21,&result,sizeof(result))||result>1)return false;
    value=result;return true;
}
template<class Read> bool read_dependencies_condition_result(std::uint32_t key,std::uint32_t pc,
    bool after,std::uintptr_t locals,Read&& read,std::uint32_t& value,std::array<std::uint8_t,8>& factors) noexcept {
    // The guarded graph computes (A||B)&&C&&D. L09..L10 contain A,B,OR,C,
    // AND1,D,AND2,final; native Bool copy at PC0xf4 writes L10, then opcode32
    // at PC0x110 stores L10 through its primitive-bool S0 output argument.
    // Opcode0x1e at PC0x4c copies the OR byte; it does not negate it.
    if(key!=dependencies_condition_key||pc||!after||locals<0x10000||locals>0x00007fffffffffffULL-0x11)return false;
    std::array<std::uint8_t,8> first{},second{};
    if(!read(locals+9,first.data(),first.size())||!read(locals+9,second.data(),second.size())||first!=second||
        std::any_of(first.begin(),first.end(),[](auto v){return v>1;})||
        first[2]!=(first[0]||first[1])||first[4]!=(first[2]&&first[3])||
        first[6]!=(first[4]&&first[5])||first[7]!=first[6])return false;
    value=first[7];factors=first;return true;
}
struct Operands {
    std::uintptr_t vm{},instance{},locals{};
    std::uint64_t captures{},at_ms{};
    std::array<std::uint8_t,43> flags{}; // local offsets0x59..0x83
    std::array<std::uint64_t,9> words{}; // raw local words0x08..0x48
};
struct Graph {
    std::uint32_t key{};
    std::array<std::uint32_t,10> layout{};
    std::uintptr_t resource{},last_resource{};
    std::uint64_t count{},first_ms{},last_ms{},instance_matches{},layout_mismatch_calls{};
    Operands before_online,after_expression;
};
struct RoutineSample {
    std::uintptr_t vm{},instance{};
    std::uint32_t entry_pc{},scalar{};
    std::uint64_t at_ms{};
    bool after{},scalar_valid{};
    std::array<std::uint8_t,8> dependency_factors{};
    bool dependency_factors_valid{};
};
struct Routine {
    std::uint32_t key{};
    std::array<std::uint32_t,10> layout{};
    std::uintptr_t resource{};
    std::uint64_t started{},finished{};
    std::vector<RoutineSample> samples;
    RoutineSample latest_exit;
};
struct Snapshot {
    std::uint64_t calls{},unresolved{},overflow{};
    std::vector<Graph> graphs;
    std::vector<Routine> routines;
};
class Observer {
public:
    static constexpr std::size_t limit=64;
    static constexpr std::size_t routine_limit=startup_contracts.size(),routine_sample_limit=32;
    void unresolved() {
        std::lock_guard lock(mutex_);++state_.calls;++state_.unresolved;
    }
    void record(std::uint32_t key,std::uintptr_t resource,const std::array<std::uint32_t,10>& layout,
        bool instance_matches,std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);++state_.calls;
        auto found=std::find_if(state_.graphs.begin(),state_.graphs.end(),[&](const Graph& graph){return graph.key==key;});
        if(found==state_.graphs.end()){
            if(state_.graphs.size()==limit){++state_.overflow;return;}
            Graph graph;graph.key=key;graph.layout=layout;graph.resource=graph.last_resource=resource;
            graph.first_ms=graph.last_ms=now_ms;state_.graphs.push_back(graph);
            found=state_.graphs.end()-1;
        }
        ++found->count;found->last_ms=now_ms;found->last_resource=resource;
        found->instance_matches+=instance_matches?1u:0u;
        found->layout_mismatch_calls+=found->layout!=layout?1u:0u;
    }
    void operands(std::uint32_t key,bool after,Operands value) {
        std::lock_guard lock(mutex_);
        const auto found=std::find_if(state_.graphs.begin(),state_.graphs.end(),[&](const Graph& graph){return graph.key==key;});
        if(found==state_.graphs.end())return;
        auto& target=after?found->after_expression:found->before_online;
        value.captures=target.captures+1;target=value;
    }
    void routine(std::uint32_t key,std::uintptr_t resource,const std::array<std::uint32_t,10>& layout,
        RoutineSample sample) {
        std::lock_guard lock(mutex_);
        auto found=std::find_if(state_.routines.begin(),state_.routines.end(),[&](const Routine& value){return value.key==key;});
        if(found==state_.routines.end()){
            if(state_.routines.size()==routine_limit)return;
            Routine value;value.key=key;value.resource=resource;value.layout=layout;state_.routines.push_back(value);
            found=state_.routines.end()-1;
        }
        if(sample.after){++found->finished;found->latest_exit=sample;}else ++found->started;
        if(found->samples.size()<routine_sample_limit)found->samples.push_back(sample);
    }
    Snapshot snapshot() const {
        Snapshot result;{std::lock_guard lock(mutex_);result=state_;}
        std::sort(result.graphs.begin(),result.graphs.end(),[](const Graph& a,const Graph& b){
            return a.count!=b.count?a.count>b.count:a.key<b.key;
        });
        return result;
    }
private:
    mutable std::mutex mutex_;
    Snapshot state_;
};
// The SDK logger has an8KiB line buffer. Prefer complete latest condition
// results and per-lane coverage over older graph/operand history. This operates
// on the report copy, never on the bounded in-memory observations.
template<class Json> std::string compact_flow_report(Json report) {
    constexpr std::size_t line_budget=7600;
    auto encoded=report.dump();
    if(encoded.size()<=line_budget)return encoded;
    report["log_omissions"]=Json::object();
    auto& omitted=report["log_omissions"];
    if(report.contains("graphs")){
        omitted["graphs"]=report["graphs"].size();report["graphs"]=Json::array();
    }
    encoded=report.dump();if(encoded.size()<=line_budget)return encoded;
    std::size_t mismatch_count{};
    for(auto& lane:report["startup_routines"])mismatch_count+=lane.erase("first_binding_mismatch");
    omitted["raw_binding_mismatches"]=mismatch_count;
    encoded=report.dump();if(encoded.size()<=line_budget)return encoded;
    std::size_t first_count{};
    for(auto& lane:report["startup_routines"]){
        if(!lane.contains("samples"))continue;
        first_count+=lane["samples"].size();lane["samples"]=Json::array();
    }
    omitted["first_samples"]=first_count;
    encoded=report.dump();if(encoded.size()<=line_budget)return encoded;
    for(auto& lane:report["startup_routines"]){lane.erase("name");lane.erase("layout");}
    omitted["lane_names_and_layouts"]=true;
    return report.dump();
}
}
