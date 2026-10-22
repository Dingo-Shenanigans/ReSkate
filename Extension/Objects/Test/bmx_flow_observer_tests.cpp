#include "Extension/Objects/bmx_flow_observer.h"
#include "Engine/Core/Json/json.h"
#include <iostream>
#include <limits>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using dingosdk::profile_runtime::bmx_flow::Observer;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void identities(){
    Observer observer;std::array<std::uint32_t,10> layout{};layout[0]=80;
    observer.unresolved();observer.record(12,0x10000,layout,true,10);
    auto changed=layout;changed[1]=4;observer.record(12,0x20000,changed,false,30);
    observer.record(4,0x30000,layout,true,20);
    const auto snapshot=observer.snapshot();
    check(snapshot.calls==4&&snapshot.unresolved==1&&!snapshot.overflow,"call accounting");
    check(snapshot.graphs.size()==2&&snapshot.graphs[0].key==12,"count ordering");
    const auto& graph=snapshot.graphs[0];
    check(graph.count==2&&graph.first_ms==10&&graph.last_ms==30,"timestamps/count");
    check(graph.resource==0x10000&&graph.last_resource==0x20000&&graph.layout==layout,"first identity retained");
    check(graph.instance_matches==1&&graph.layout_mismatch_calls==1,"identity differences counted");
    dingosdk::profile_runtime::bmx_flow::Operands operands;operands.vm=0x40000;operands.flags[0]=1;
    observer.operands(12,false,operands);operands.vm=0x50000;observer.operands(12,false,operands);
    observer.operands(12,true,operands);observer.operands(99,true,operands);
    const auto sampled=observer.snapshot();
    check(sampled.calls==4&&sampled.graphs.size()==2,"operand capture changes no query counts or keys");
    check(sampled.graphs[0].before_online.captures==2&&sampled.graphs[0].before_online.vm==0x50000&&
        sampled.graphs[0].after_expression.captures==1&&sampled.graphs[0].after_expression.flags[0]==1,"operand phase/capture isolation");
}
void bounded(){
    Observer observer;std::array<std::uint32_t,10> layout{};
    for(std::uint32_t i=0;i<64;++i)observer.record(i,0x10000+i,layout,true,i);
    observer.record(64,0x20000,layout,true,64);observer.record(0,0x10000,layout,true,65);
    const auto snapshot=observer.snapshot();
    check(snapshot.graphs.size()==64&&snapshot.overflow==1&&snapshot.calls==66,"key bound");
    check(snapshot.graphs.front().key==0&&snapshot.graphs.front().count==2,"known key counted after full");
    check(snapshot.graphs[1].key==1&&snapshot.graphs[63].key==63,"deterministic tie order");
}
void concurrent(){
    Observer observer;std::array<std::uint32_t,10> layout{};std::vector<std::thread> threads;
    for(unsigned i=0;i<8;++i)threads.emplace_back([&]{for(unsigned j=0;j<1000;++j)observer.record(7,0x10000,layout,true,j);});
    for(auto& thread:threads)thread.join();
    const auto snapshot=observer.snapshot();
    check(snapshot.calls==8000&&snapshot.graphs.size()==1&&snapshot.graphs[0].count==8000&&snapshot.graphs[0].instance_matches==8000,"concurrent counts");
}
void routines(){
    Observer observer;std::array<std::uint32_t,10> layout{};
    dingosdk::profile_runtime::bmx_flow::RoutineSample sample;sample.vm=0x10000;sample.entry_pc=16;
    for(unsigned i=0;i<100;++i){sample.after=i%2!=0;sample.at_ms=i;observer.routine(1,0x20000,layout,sample);}
    for(std::uint32_t key=2;key<=Observer::routine_limit+1;++key)observer.routine(key,0x30000+key,layout,sample);
    const auto snapshot=observer.snapshot();
    check(snapshot.routines.size()==Observer::routine_limit&&snapshot.routines[0].samples.size()==32,"routine and sample bounds");
    check(snapshot.routines[0].started==50&&snapshot.routines[0].finished==50,"routine totals survive sample saturation");
    check(snapshot.routines[0].samples.front().at_ms==0&&snapshot.routines[0].samples.back().at_ms==31,"first samples retained");
    check(snapshot.routines[0].latest_exit.after&&snapshot.routines[0].latest_exit.at_ms==99,
        "latest exit remains observable after first-sample saturation");
    check(snapshot.routines[0].samples[0].entry_pc==16&&!snapshot.calls&&snapshot.graphs.empty(),"routine cursor and query isolation");
}
void startup_contracts(){
    namespace flow=dingosdk::profile_runtime::bmx_flow;
    constexpr std::array<std::uint32_t,4> original_keys{0x129086b2,0x4f83ddd3,0x7a374bb6,0x8e8a8b37};
    constexpr std::array<flow::StartupContract,7> authored_entries{{
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
        {0xcdc24973,{48,24,34,52,3,0,0,327685,2228224,0},0x1d8,{0x002970f7,0},
            "Flow/StateMachines/Main/Flow_Root/C8FAABC_CG","level_class_condition"},
        {0x5749c298,{48,8,17,75,3,0,0,9,720896,0},0x1b4,{0x5cc8bee9,0},
            "Flow/StateMachines/Main/DingoFlow_ScreenManager_Root/B5909DD_CG","dependencies_ready"}
    }};
    check(flow::startup_contracts.size()==11&&Observer::routine_limit==11,"all eleven startup lanes fit the bounded observer");
    for(std::size_t i=0;i<original_keys.size();++i)
        check(flow::startup_contracts[i].key==original_keys[i],"existing startup keys remain in their original lanes");
    for(const auto& expected:authored_entries){
        const auto found=std::find_if(flow::startup_contracts.begin(),flow::startup_contracts.end(),
            [&](const auto& value){return value.key==expected.key;});
        check(found!=flow::startup_contracts.end(),"authored state-entry key exists");
        check(found->layout==expected.layout&&found->binding_offset==expected.binding_offset&&found->binding==expected.binding&&
            std::string_view{found->name}==expected.name&&std::string_view{found->scalar_name}==expected.scalar_name,
            "entry identity matches the extracted resource contract");
    }
    for(const auto& contract:flow::startup_contracts){
        constexpr std::uintptr_t base=0x10000,image_base=0x140000000ULL;
        std::vector<unsigned char> bytes(0x1000);
        const auto store=[&](std::size_t offset,const auto& value){std::memcpy(bytes.data()+offset,&value,sizeof(value));};
        auto loaded_tail=flow::level_class_condition_tail;
        loaded_tail[1]=static_cast<std::uint32_t>(image_base+flow::bool_copy_rva);
        loaded_tail[2]=static_cast<std::uint32_t>((image_base+flow::bool_copy_rva)>>32);
        auto code_prefix=flow::bool_copy_prefix;
        store(0x20,contract.layout);store(contract.binding_offset,contract.binding);
        if(contract.key==flow::level_class_condition_key){
            store(0x1a0,loaded_tail);store(0x1f8,flow::level_class_condition_binding);
        }
        auto loaded_code=flow::dependencies_code;
        for(const auto& relocation:flow::dependencies_relocations){
            const auto target=image_base+relocation.rva;
            loaded_code[relocation.pc/4+1]=static_cast<std::uint32_t>(target);
            loaded_code[relocation.pc/4+2]=static_cast<std::uint32_t>(target>>32);
        }
        if(contract.key==flow::dependencies_condition_key){
            store(0x88,loaded_code);store(0x1b4,flow::dependencies_bindings);
        }
        std::uintptr_t unreadable{},mutated_adapter{};unsigned reads{},mutated_byte{};
        const auto read=[&](std::uintptr_t p,void* out,std::size_t size){
            ++reads;
            if(contract.key==flow::dependencies_condition_key){
                for(const auto& relocation:flow::dependencies_relocations){
                    if(p!=image_base+relocation.rva||size!=relocation.prefix_size)continue;
                    if(p==unreadable)return false;
                    std::memcpy(out,relocation.prefix.data(),size);
                    if(p==mutated_adapter)static_cast<unsigned char*>(out)[mutated_byte]^=1;
                    return true;
                }
            }
            if(p==image_base+flow::bool_copy_rva&&size==code_prefix.size()){
                if(p==unreadable)return false;
                std::memcpy(out,code_prefix.data(),size);return true;
            }
            if(p==unreadable||p<base||p-base>bytes.size()||size>bytes.size()-(p-base))return false;
            std::memcpy(out,bytes.data()+(p-base),size);return true;
        };
        const auto result=[&]{return flow::match_startup_resource(contract,base,read,image_base);};
        check(result()==flow::ResourceMatch::matched,"exact resource accepts");
        for(std::size_t i=0;i<contract.layout.size();++i){
            auto changed=contract.layout;changed[i]^=1;store(0x20,changed);
            check(result()==flow::ResourceMatch::layout_rejected,"every layout-word mutation rejects");
        }
        store(0x20,contract.layout);
        for(std::size_t i=0;i<contract.binding.size();++i){
            auto changed=contract.binding;changed[i]^=1;store(contract.binding_offset,changed);
            check(result()==flow::ResourceMatch::binding_rejected,"binding hash and PC mutations reject");
        }
        store(contract.binding_offset,contract.binding);
        if(contract.key==flow::level_class_condition_key){
            for(std::size_t i=0;i<flow::level_class_condition_tail.size();++i){
                auto changed=loaded_tail;changed[i]^=1;store(0x1a0,changed);
                check(result()==flow::ResourceMatch::binding_rejected,"every result-tail opcode/operand mutation rejects");
            }
            store(0x1a0,flow::level_class_condition_tail);
            check(result()==flow::ResourceMatch::binding_rejected,"unfixed serialized zero function pointer rejects");
            store(0x1a0,loaded_tail);
            for(std::size_t i=0;i<code_prefix.size();++i){
                code_prefix=flow::bool_copy_prefix;code_prefix[i]^=1;
                check(result()==flow::ResourceMatch::binding_rejected,"every approved-function prologue byte mutation rejects");
            }
            code_prefix=flow::bool_copy_prefix;
            check(flow::match_startup_resource(contract,base,read)==flow::ResourceMatch::binding_rejected,
                "condition scalar requires a verified image-base target");
            for(std::size_t i=0;i<flow::level_class_condition_binding.size();++i){
                auto changed=flow::level_class_condition_binding;changed[i]^=1;store(0x1f8,changed);
                check(result()==flow::ResourceMatch::binding_rejected,"final bool binding hash/PC mutations reject");
            }
            store(0x1f8,flow::level_class_condition_binding);
            unreadable=base+0x1a0;check(result()==flow::ResourceMatch::binding_rejected,"unreadable result tail rejects");
            unreadable=base+0x1f8;check(result()==flow::ResourceMatch::binding_rejected,"unreadable final bool binding rejects");
        }
        if(contract.key==flow::dependencies_condition_key){
            for(std::size_t i=0;i<loaded_code.size();++i){
                auto changed=loaded_code;changed[i]^=1;store(0x88,changed);
                check(result()==flow::ResourceMatch::binding_rejected,"every dependency code/operand/relocation word mutation rejects");
            }
            store(0x88,flow::dependencies_code);
            check(result()==flow::ResourceMatch::binding_rejected,"serialized unrelocated dependency calls reject");
            store(0x88,loaded_code);
            for(std::size_t i=0;i<flow::dependencies_bindings.size();++i){
                auto changed=flow::dependencies_bindings;changed[i]^=1;store(0x1b4,changed);
                check(result()==flow::ResourceMatch::binding_rejected,"every dependency binding hash/PC mutation rejects");
            }
            store(0x1b4,flow::dependencies_bindings);
            for(const auto& relocation:flow::dependencies_relocations){
                mutated_adapter=image_base+relocation.rva;
                for(mutated_byte=0;mutated_byte<relocation.prefix_size;++mutated_byte)
                    check(result()==flow::ResourceMatch::binding_rejected,"every dependency adapter fingerprint byte mutation rejects");
                mutated_adapter=0;unreadable=image_base+relocation.rva;
                check(result()==flow::ResourceMatch::binding_rejected,"unreadable dependency adapter rejects");unreadable=0;
            }
            check(flow::match_startup_resource(contract,base,read)==flow::ResourceMatch::binding_rejected&&
                flow::match_startup_resource(contract,base,read,image_base+0x1000)==flow::ResourceMatch::binding_rejected,
                "dependency calls require their exact image-base targets");
            unreadable=base+0x88;check(result()==flow::ResourceMatch::binding_rejected,"unreadable dependency code rejects");
            unreadable=base+0x1b4;check(result()==flow::ResourceMatch::binding_rejected,"unreadable dependency bindings reject");
        }
        unreadable=base+0x20;check(result()==flow::ResourceMatch::layout_rejected,"unreadable layout rejects");
        unreadable=base+contract.binding_offset;check(result()==flow::ResourceMatch::binding_rejected,"unreadable binding rejects");
        const auto before=reads;
        check(flow::match_startup_resource(contract,0,read)==flow::ResourceMatch::layout_rejected&&
            flow::match_startup_resource(contract,0x00007ffffffffff8ULL,read)==flow::ResourceMatch::layout_rejected&&reads==before,
            "invalid resource span is rejected before reading");
    }
    Observer observer;flow::RoutineSample sample;
    for(const auto& contract:flow::startup_contracts){
        observer.routine(contract.key,0x10000,contract.layout,sample);
        sample.after=true;observer.routine(contract.key,0x10000,contract.layout,sample);sample.after=false;
    }
    const auto snapshot=observer.snapshot();
    check(snapshot.routines.size()==11,"every watched entry can be retained concurrently");
    for(const auto& routine:snapshot.routines)
        check(routine.started==1&&routine.finished==1,"every watched entry preserves paired counts");
}
void condition_result(){
    namespace flow=dingosdk::profile_runtime::bmx_flow;
    constexpr std::uintptr_t locals=0x10000;
    unsigned reads{};std::uint8_t native_result{};bool readable=true;std::uint32_t sampled=99;
    const auto read=[&](std::uintptr_t p,void* out,std::size_t size){
        ++reads;if(!readable||p!=locals+0x21||size!=1)return false;
        *static_cast<std::uint8_t*>(out)=native_result;return true;
    };
    const auto sample=[&](std::uint32_t key,std::uint32_t pc,bool after,std::uintptr_t p){
        return flow::read_level_class_condition_result(key,pc,after,p,read,sampled);
    };
    check(sample(flow::level_class_condition_key,0,true,locals)&&sampled==0,"completed false result is observed");
    native_result=1;check(sample(flow::level_class_condition_key,0,true,locals)&&sampled==1,"completed true result is observed");
    const auto before=reads;
    check(!sample(flow::level_class_condition_key,0,false,locals)&&!sample(flow::level_class_condition_key,0x10,true,locals)&&
        !sample(0,0,true,locals)&&!sample(flow::level_class_condition_key,0,true,0)&&
        !sample(flow::level_class_condition_key,0,true,0x00007ffffffffff8ULL)&&reads==before,
        "entry/fragment/unrelated/invalid samples never read a stale result");
    native_result=2;check(!sample(flow::level_class_condition_key,0,true,locals)&&sampled==1,"non-boolean local rejects without replacing result");
    readable=false;check(!sample(flow::level_class_condition_key,0,true,locals)&&sampled==1,"unreadable result rejects without replacement");
}
void dependencies_result(){
    namespace flow=dingosdk::profile_runtime::bmx_flow;
    constexpr std::uintptr_t locals=0x10000;
    std::array<std::uint8_t,8> native{},factors{};
    std::uint32_t sampled=99;unsigned reads{},fail_read{},change_read{};
    const auto read=[&](std::uintptr_t p,void* out,std::size_t size){
        ++reads;if(reads==fail_read||p!=locals+9||size!=native.size())return false;
        std::memcpy(out,native.data(),size);
        if(reads==change_read)static_cast<std::uint8_t*>(out)[0]^=1;
        return true;
    };
    const auto sample=[&](std::uint32_t key=flow::dependencies_condition_key,std::uint32_t pc=0,
        bool after=true,std::uintptr_t p=0x10000){
        return flow::read_dependencies_condition_result(key,pc,after,p,read,sampled,factors);
    };
    for(unsigned bits=0;bits<16;++bits){
        const bool a=(bits&1)!=0,b=(bits&2)!=0,c=(bits&4)!=0,d=(bits&8)!=0;
        const auto either=static_cast<std::uint8_t>(a||b),first=static_cast<std::uint8_t>((a||b)&&c);
        const auto all=static_cast<std::uint8_t>((a||b)&&c&&d);
        native={static_cast<std::uint8_t>(a),static_cast<std::uint8_t>(b),either,static_cast<std::uint8_t>(c),
            first,static_cast<std::uint8_t>(d),all,all};
        check(sample()&&sampled==all&&factors==native,"all sixteen dependency truth combinations match authored formula");
    }
    const auto valid=native,retained=factors;
    for(std::size_t i=0;i<native.size();++i){
        native=valid;native[i]=2;
        check(!sample()&&sampled==1&&factors==retained,"every non-boolean dependency local rejects without replacing evidence");
    }
    for(const auto i:std::array<std::size_t,4>{2,4,6,7}){
        native=valid;native[i]^=1;
        check(!sample()&&factors==retained,"inconsistent OR/AND/final-copy intermediates reject");
    }
    native=valid;reads=0;fail_read=1;check(!sample(),"unreadable first dependency sample rejects");
    reads=0;fail_read=2;check(!sample(),"unreadable second dependency sample rejects");
    reads=0;fail_read=0;change_read=2;check(!sample()&&factors==retained,"changing dependency locals reject");
    change_read=0;const auto before=reads;
    check(!sample(0)&&!sample(flow::dependencies_condition_key,1)&&!sample(flow::dependencies_condition_key,0,false)&&
        !sample(flow::dependencies_condition_key,0,true,0)&&
        !sample(flow::dependencies_condition_key,0,true,0x00007ffffffffff8ULL)&&reads==before,
        "only completed full dependency expressions read boolean factors");
    Observer observer;flow::RoutineSample current;current.after=true;current.scalar_valid=true;
    current.dependency_factors_valid=true;current.dependency_factors=valid;current.scalar=1;
    for(unsigned i=0;i<40;++i){current.at_ms=i;observer.routine(flow::dependencies_condition_key,locals,{},current);}
    current.dependency_factors.fill(0);current.scalar=0;current.at_ms=40;
    observer.routine(flow::dependencies_condition_key,locals,{},current);
    const auto latest=observer.snapshot().routines.front();
    check(latest.samples.size()==32&&latest.samples.front().scalar==1&&latest.latest_exit.at_ms==40&&
        latest.latest_exit.scalar==0&&latest.latest_exit.dependency_factors_valid&&latest.latest_exit.dependency_factors[0]==0,
        "polling dependency changes remain visible after bounded first history fills");
}
void mismatch_evidence(){
    namespace flow=dingosdk::profile_runtime::bmx_flow;
    constexpr std::uintptr_t base=0x10000;
    const auto& contract=*std::find_if(flow::startup_contracts.begin(),flow::startup_contracts.end(),
        [](const auto& value){return value.key==flow::level_class_condition_key;});
    std::vector<unsigned char> bytes(0x1000);
    const auto store=[&](std::size_t offset,const auto& value){std::memcpy(bytes.data()+offset,&value,sizeof(value));};
    store(0x10,contract.key);store(0x20,contract.layout);store(contract.binding_offset,contract.binding);
    auto changed_tail=flow::level_class_condition_tail;changed_tail[0]^=0x80;
    store(0x1a0,changed_tail);store(0x1f8,flow::level_class_condition_binding);
    unsigned reads{},tail_reads{};bool mutate_identity{};std::uintptr_t unreadable{};
    const auto read=[&](std::uintptr_t p,void* out,std::size_t size){
        ++reads;
        if(p==base+0x1a0&&++tail_reads==2&&mutate_identity)store(0x10,contract.key^1);
        if(p==unreadable||p<base||p-base>bytes.size()||size>bytes.size()-(p-base))return false;
        std::memcpy(out,bytes.data()+(p-base),size);return true;
    };
    check(flow::match_startup_resource(contract,base,read)==flow::ResourceMatch::binding_rejected,"diagnostic never accepts the rejected contract");
    auto evidence=flow::startup_resource_evidence(contract.key,base,read);
    check(evidence.layout_matched&&evidence.coherent&&evidence.resource==base&&evidence.key==contract.key&&
        evidence.first_binding==contract.binding&&evidence.tail==changed_tail&&
        evidence.final_binding==flow::level_class_condition_binding,"actual mismatching words are retained without reading locals");
    const auto before=reads;
    check(!flow::startup_resource_evidence(0,base,read).layout_matched&&
        !flow::startup_resource_evidence(contract.key,0,read).layout_matched&&reads==before,"unknown key/invalid span performs no evidence reads");
    auto changed_layout=contract.layout;changed_layout[0]^=1;store(0x20,changed_layout);
    check(!flow::startup_resource_evidence(contract.key,base,read).layout_matched,"evidence requires the exact layout");
    store(0x20,contract.layout);unreadable=base+0x1f8;
    evidence=flow::startup_resource_evidence(contract.key,base,read);
    check(evidence.layout_matched&&!evidence.final_readable&&!evidence.coherent,"unreadable evidence is marked incomplete");
    unreadable=0;mutate_identity=true;tail_reads=0;
    check(!flow::startup_resource_evidence(contract.key,base,read).coherent,"identity change after content reread invalidates evidence");
    const auto& ordinary=flow::startup_contracts.front();
    store(0x10,ordinary.key);store(0x20,ordinary.layout);store(ordinary.binding_offset,ordinary.binding);
    evidence=flow::startup_resource_evidence(ordinary.key,base,read);
    check(evidence.layout_matched&&evidence.coherent&&!evidence.tail_attempted&&!evidence.final_readable,
        "other allowlisted resources expose only their own bounded binding");
}
void dependencies_mismatch_evidence(){
    namespace flow=dingosdk::profile_runtime::bmx_flow;
    constexpr std::uintptr_t base=0x10000;
    const auto& contract=flow::startup_contracts.back();
    std::vector<unsigned char> bytes(0x200);
    const auto store=[&](std::size_t offset,const auto& value){std::memcpy(bytes.data()+offset,&value,sizeof(value));};
    auto code=flow::dependencies_code;code[1]=0x12345678;
    store(0x10,contract.key);store(0x20,contract.layout);store(0x88,code);store(0x1b4,flow::dependencies_bindings);
    unsigned code_reads{};bool mutate_identity{};std::uintptr_t unreadable{};
    const auto read=[&](std::uintptr_t p,void* out,std::size_t size){
        if(p==base+0x88&&++code_reads==2&&mutate_identity)store(0x10,contract.key^1);
        if(p==unreadable||p<base||p-base>bytes.size()||size>bytes.size()-(p-base))return false;
        std::memcpy(out,bytes.data()+(p-base),size);return true;
    };
    auto evidence=flow::startup_resource_evidence(contract.key,base,read);
    check(evidence.layout_matched&&evidence.coherent&&evidence.dependencies_attempted&&
        evidence.dependencies_code_readable&&evidence.dependencies_code==code&&
        evidence.dependencies_bindings_readable&&evidence.dependencies_bindings==flow::dependencies_bindings&&
        !evidence.tail_attempted,"dependency mismatch captures only its exact bounded code and bindings");
    unreadable=base+0x88;evidence=flow::startup_resource_evidence(contract.key,base,read);
    check(!evidence.dependencies_code_readable&&!evidence.coherent,"unreadable dependency code is incomplete evidence");
    unreadable=base+0x1b4;evidence=flow::startup_resource_evidence(contract.key,base,read);
    check(!evidence.dependencies_bindings_readable&&!evidence.coherent,"unreadable dependency binding is incomplete evidence");
    unreadable=0;mutate_identity=true;code_reads=0;
    check(!flow::startup_resource_evidence(contract.key,base,read).coherent,
        "dependency evidence rechecks resource identity after complete content rereads");
}
void log_line_budget(){
    namespace flow=dingosdk::profile_runtime::bmx_flow;using dingosdk::Json;
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    Json report{{"event","bmx_flow_diagnostics"},{"enabled",true},{"calls",maximum},{"unresolved",maximum},
        {"overflow_calls",maximum},{"max_keys",64},{"time_basis","GetTickCount64 milliseconds"},
        {"startup_sample_limit",32},{"startup_logged_first_samples",1},{"graphs",Json::array()},
        {"startup_routines",Json::array()},{"startup_coverage",{{"entries",maximum},{"exits",maximum},
            {"inactive_boundaries",maximum},{"identity_unreadable",maximum},{"exceptions",maximum}}}};
    for(const auto& contract:flow::startup_contracts){
        Json latest{{"phase","exit"},{"entry_pc",std::numeric_limits<std::uint32_t>::max()},
            {"at_ms",maximum},{"vm",maximum},{"instance",maximum}};
        if(*contract.scalar_name)latest[contract.scalar_name]=std::numeric_limits<std::uint32_t>::max();
        if(contract.key==flow::dependencies_condition_key)
            latest["dependency_factors"]={{"native_gate_5cc8bee9",1},{"native_gate_ed285e1b",0},
                {"either_native_gate",1},{"profile_group_0_query",0},{"either_gate_and_profile_query",0},
                {"resolved_level_class",1},{"all_factors",0},{"condition_out",0}};
        Json lane{{"key",contract.key},{"name",contract.name},{"layout",contract.layout},{"resource",maximum},
            {"started",maximum},{"finished",maximum},{"key_seen_entries",maximum},{"key_seen_exits",maximum},
            {"layout_rejected",maximum},{"binding_rejected",maximum},{"instance_rejected",maximum},
            {"mismatch_capture_state",2},{"mismatch_capture_layout_matched",true},{"samples",Json::array()},
            {"latest_exit",latest},{"first_binding_mismatch",{{"dependencies_code_words",flow::dependencies_code},
                {"dependencies_binding_words",flow::dependencies_bindings}}}};
        for(unsigned i=0;i<32;++i)lane["samples"].push_back(latest);
        report["startup_routines"].push_back(std::move(lane));
    }
    for(unsigned i=0;i<64;++i)report["graphs"].push_back(Json{{"key",i},{"count",maximum},
        {"resource",maximum},{"last_resource",maximum},{"layout",flow::startup_contracts.front().layout},
        {"before_online",Json{{"flags",std::array<std::uint64_t,43>{}},{"words",std::array<std::uint64_t,9>{}}}}});
    const auto full=report.dump();check(full.size()>8192,"saturated native report reproduces logger truncation risk");
    const auto encoded=flow::compact_flow_report(report);const auto compact=Json::parse(encoded);
    check(encoded.size()<=7600,"all eleven lanes with maximum-width counters and latest results fit logger budget");
    check(compact["startup_routines"].size()==11&&compact.contains("log_omissions"),"compaction keeps every watched lane and explains omitted details");
    for(std::size_t i=0;i<11;++i){
        const auto& before=report["startup_routines"][i];const auto& after=compact["startup_routines"][i];
        for(const char* field:{"key","started","finished","key_seen_entries","key_seen_exits","layout_rejected", "binding_rejected","instance_rejected","latest_exit"})
            check(before[field]==after[field],"compaction preserves exact counts and latest evidence for every lane");
    }
    check(report.dump()==full,"log compaction leaves the full observation/report copy unchanged");
    Json small{{"event","bmx_flow_diagnostics"},{"enabled",true},{"startup_routines",Json::array()},{"graphs",Json::array()}};
    check(flow::compact_flow_report(small)==small.dump(),"small native reports retain all details without omission flags");
    std::cout<<"Worst-case compact Flow JSON: "<<encoded.size()<<" bytes\n";
}
}
int main(){try{identities();bounded();concurrent();routines();startup_contracts();condition_result();dependencies_result();mismatch_evidence();dependencies_mismatch_evidence();log_line_budget();std::cout<<"BMX Flow observer cases passed\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
