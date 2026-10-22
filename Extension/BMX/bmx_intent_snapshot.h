#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::bmx::intent_observer::detail {
inline constexpr std::uintptr_t highest=0x00007fffffffffffULL;
inline constexpr std::uintptr_t node_vtable_rva=0x6178cf0;
inline constexpr std::uintptr_t data_vtable_rva=0x61870c0;
inline constexpr std::uintptr_t asset_vtable_rva=0x61aaf88;
inline constexpr std::uintptr_t query_vtable_rva=0x61ab030;
inline constexpr std::uintptr_t bool_vtable_rva=0x61ab080;
inline constexpr std::uintptr_t entity_manager_rva=0x73c5e80;
inline constexpr std::array<std::uint8_t,16> wipeout_guid{
    0x7e,0x34,0x3a,0x4c,0xda,0x86,0xdd,0xa5,0x4c,0x3a,0x34,0x7e,0x86,0xda,0xa5,0xdd};
inline constexpr std::array<std::uint8_t,16> authored_guid{
    0x70,0x34,0x67,0x90,0x20,0x56,0xeb,0x46,0x88,0x27,0x57,0x47,0x10,0x19,0xee,0x37};
enum class Kind : unsigned { begin,update };
enum class Stage : unsigned {
    image_span,event_unreadable,node_unreadable,node_type_mismatch,
    node_reread_unreadable,node_changed,event_reread_unreadable,event_changed,
    data_unqualified,intent_unqualified,qualified
};
inline const char* stage_name(Stage stage) noexcept {
    switch(stage){
    case Stage::image_span:return "image_span";
    case Stage::event_unreadable:return "event_unreadable";
    case Stage::node_unreadable:return "node_unreadable";
    case Stage::node_type_mismatch:return "node_type_mismatch";
    case Stage::node_reread_unreadable:return "node_reread_unreadable";
    case Stage::node_changed:return "node_changed";
    case Stage::event_reread_unreadable:return "event_reread_unreadable";
    case Stage::event_changed:return "event_changed";
    case Stage::data_unqualified:return "data_unqualified";
    case Stage::intent_unqualified:return "intent_unqualified";
    case Stage::qualified:return "qualified";
    }
    return "unknown";
}
inline bool span(std::uintptr_t p,std::size_t n) noexcept {
    return p>=0x10000&&n&&n<=highest&&p<=highest-n;
}
template<class T,class Read> bool get(std::uintptr_t p,T& out,Read&& read) noexcept {
    return span(p,sizeof(T))&&read(p,&out,sizeof(T));
}
struct Asset {
    std::uintptr_t property{},asset{};
    std::array<std::uint8_t,16> guid{};
    bool readable{},coherent{},typed{},request_wipeout{};
};
template<class Read> Asset inspect_asset(std::uintptr_t base,std::uintptr_t property,Read&& read) noexcept {
    Asset out;out.property=property;
    if(property<0x10060||!span(base,bool_vtable_rva+8))return out;
    out.asset=property-0x60;
    std::array<std::uintptr_t,3> first{},again{};
    std::array<std::uint8_t,16> guid_again{};
    const auto roots=[&](auto& values) noexcept {
        return get(out.asset,values[0],read)&&get(out.asset+0x20,values[1],read)&&get(property,values[2],read);
    };
    out.readable=span(out.asset-16,16)&&roots(first)&&get(out.asset-16,out.guid,read);
    out.coherent=out.readable&&roots(again)&&get(out.asset-16,guid_again,read)&&first==again&&out.guid==guid_again;
    out.typed=out.coherent&&first[0]==base+asset_vtable_rva&&first[1]==base+query_vtable_rva&&first[2]==base+bool_vtable_rva;
    out.request_wipeout=out.typed&&out.guid==wipeout_guid;
    return out;
}
struct Handle {
    std::uint64_t value{};
    std::uintptr_t manager{},table{},slot{},entity{};
    std::uint32_t mask{},index{};
    bool coherent{};
};
// Exact native1897c50 table lookup expressed as bounded reads, not a native
// call. The index cap only limits this diagnostic; failure is not absence.
template<class Read> Handle inspect_handle(std::uintptr_t base,std::uint64_t value,Read&& read) noexcept {
    Handle out;out.value=value;
    if(!(value&(std::uint64_t{1}<<62))||!span(base,entity_manager_rva+8)||
       !get(base+entity_manager_rva,out.manager,read)||!span(out.manager,0x28)||
       !get(out.manager+0x10,out.mask,read)||!get(out.manager+0x20,out.table,read))return out;
    out.index=static_cast<std::uint32_t>(value)&out.mask;
    if(out.index>0x100000||!span(out.table,static_cast<std::size_t>(out.index)*0x140+16))return out;
    out.slot=out.table+static_cast<std::uintptr_t>(out.index)*0x140;
    std::array<std::uint64_t,2> first{},again{};
    std::uintptr_t manager_again{},table_again{};std::uint32_t mask_again{};
    if(!get(out.slot,first,read)||!get(out.slot,again,read)||first!=again||first[0]!=value||
       !get(out.manager+0x10,mask_again,read)||mask_again!=out.mask||
       !get(out.manager+0x20,table_again,read)||table_again!=out.table||
       !get(base+entity_manager_rva,manager_again,read)||manager_again!=out.manager)return out;
    out.entity=static_cast<std::uintptr_t>(first[1]);out.coherent=span(out.entity,8);return out;
}
struct Header {
    std::uintptr_t vtable{},data_word{},root_word{},data_vtable{};
    Stage stage{Stage::image_span};
    bool event_readable{},node_span_valid{},node_readable_120{},vtable_readable{},node_type_matches{},
        node_rereadable_120{},node_equal{},event_rereadable{},event_equal{},data_vtable_readable{};
};
struct Snapshot {
    Kind kind{};
    std::uintptr_t event_params{},node{},root{},data{},context_wrapper{},binding{},begin_context_argument{};
    std::uint64_t entity_handle{},root_handle{};
    std::array<std::uint8_t,16> data_guid{};
    std::array<std::uint8_t,2> reset{},update_after{};
    Asset input,mirror;
    Handle handle;
    Header header;
    bool node_coherent{},root_coherent{},data_coherent{},authored_wipeout{},target{},update_after_coherent{},begin_context_readable{};
};
template<class Read> Snapshot inspect(std::uintptr_t base,Kind kind,std::uintptr_t event_params,
    std::uintptr_t update_input,std::uintptr_t update_mirror,Read&& read) noexcept {
    Snapshot out;out.kind=kind;out.event_params=event_params;
    std::array<std::uint64_t,15> first{},again{};
    auto& header=out.header;
    if(!span(base,bool_vtable_rva+8))return out;
    header.stage=Stage::event_unreadable;
    if(!(header.event_readable=get(event_params,out.node,read)))return out;
    header.stage=Stage::node_unreadable;header.node_span_valid=span(out.node,sizeof(first));
    if(!(header.node_readable_120=get(out.node,first,read))){
        // A bounded first-word fallback identifies a readable prefix without
        // following any pointer or qualifying an incomplete native node.
        header.vtable_readable=get(out.node,header.vtable,read);return out;
    }
    header.vtable_readable=true;header.vtable=first[0];header.data_word=first[1];header.root_word=first[5];
    header.stage=Stage::node_type_mismatch;
    if(!(header.node_type_matches=first[0]==base+node_vtable_rva))return out;
    out.data=static_cast<std::uintptr_t>(first[1]&~std::uint64_t{4});
    out.root=static_cast<std::uintptr_t>(first[5]);out.entity_handle=first[9];
    out.context_wrapper=static_cast<std::uintptr_t>(first[10]);out.binding=static_cast<std::uintptr_t>(first[11]);
    out.input=inspect_asset(base,static_cast<std::uintptr_t>(first[12]),read);
    out.mirror=first[13]==first[12]?out.input:inspect_asset(base,static_cast<std::uintptr_t>(first[13]),read);
    std::uintptr_t data_again{},event_again{};std::uint64_t root_again{};
    std::array<std::uint8_t,2> reset_again{};
    std::array<std::uint8_t,16> data_guid_again{};
    header.data_vtable_readable=span(out.data,0x5a)&&get(out.data,header.data_vtable,read);
    out.data_coherent=header.data_vtable_readable&&header.data_vtable==base+data_vtable_rva&&
        get(out.data+0x58,out.reset,read)&&get(out.data-16,out.data_guid,read)&&
        get(out.data,data_again,read)&&data_again==header.data_vtable&&
        get(out.data+0x58,reset_again,read)&&reset_again==out.reset&&
        get(out.data-16,data_guid_again,read)&&data_guid_again==out.data_guid;
    out.authored_wipeout=out.data_coherent&&out.data_guid==authored_guid;
    out.root_coherent=span(out.root,0x28)&&get(out.root+0x20,out.root_handle,read)&&
        get(out.root+0x20,root_again,read)&&root_again==out.root_handle&&out.root_handle==out.entity_handle;
    if(kind==Kind::update){
        std::array<std::uint8_t,2> values_again{};
        out.update_after_coherent=get(update_input,out.update_after[0],read)&&get(update_mirror,out.update_after[1],read)&&
            get(update_input,values_again[0],read)&&get(update_mirror,values_again[1],read)&&values_again==out.update_after&&
            out.update_after[0]<=1&&out.update_after[1]<=1;
    }else out.begin_context_readable=get(update_input,out.begin_context_argument,read);
    out.handle=inspect_handle(base,out.entity_handle,read);
    header.stage=Stage::node_reread_unreadable;
    if(!(header.node_rereadable_120=get(out.node,again,read)))return out;
    header.stage=Stage::node_changed;
    if(!(header.node_equal=first==again))return out;
    header.stage=Stage::event_reread_unreadable;
    if(!(header.event_rereadable=get(event_params,event_again,read)))return out;
    header.stage=Stage::event_changed;
    if(!(header.event_equal=event_again==out.node))return out;
    out.node_coherent=true;
    out.target=out.node_coherent&&out.data_coherent&&(out.input.request_wipeout||out.mirror.request_wipeout);
    header.stage=out.target?Stage::qualified:out.data_coherent?Stage::intent_unqualified:Stage::data_unqualified;
    return out;
}
struct Owner {std::uintptr_t client{},entity{},core{},context{},rig{};bool available{};};
enum class ProbePhase { arm,follow,closed };
inline ProbePhase probe_phase(std::uint64_t started,std::uint64_t now) noexcept {
    if(now<started||now-started>=2250)return ProbePhase::closed;
    return now-started<250?ProbePhase::arm:ProbePhase::follow;
}
inline bool typed_control(const Snapshot& sample) noexcept {
    return sample.node_coherent&&sample.data_coherent&&sample.root_coherent&&sample.handle.coherent&&
        (sample.input.typed||sample.mirror.typed)&&span(sample.binding,8)&&span(sample.context_wrapper,8);
}
template<class Read> bool control_matches(std::uintptr_t base,const Snapshot& sample,const Owner& owner,Read&& read) noexcept {
    if(!typed_control(sample)||!owner.available||!span(owner.client,8)||!span(owner.entity,8)||
        !span(owner.core,8)||!span(owner.context,8)||!span(owner.rig,8)||sample.handle.entity!=owner.entity)return false;
    const auto current=inspect_handle(base,sample.entity_handle,read);
    return current.coherent&&current.entity==owner.entity;
}
struct OwnerHint {std::uintptr_t entity{};std::uint64_t observed_ms{};bool coherent{};};
inline bool owner_bank(const Snapshot& sample,const OwnerHint& hint,std::uint64_t now)noexcept{
    return hint.coherent&&hint.entity&&now>=hint.observed_ms&&now-hint.observed_ms<=250&&sample.target&&
        sample.root_coherent&&sample.handle.coherent&&sample.handle.entity==hint.entity;
}
struct Correlation {Owner canonical;std::uint64_t age_ms{};bool available{},full_handle_still_maps{},entity_matches{};};
template<class Read> Correlation correlate(std::uintptr_t base,const Snapshot& sample,std::uint64_t observed_ms,
    std::uint64_t now,const Owner& owner,Read&& read) noexcept {
    Correlation out;out.canonical=owner;
    if(now<observed_ms)return out;
    out.age_ms=now-observed_ms;
    if(!owner.available||!span(owner.client,8)||!span(owner.entity,8)||!span(owner.core,8)||
       !span(owner.context,8)||!span(owner.rig,8)||out.age_ms>250||!sample.target||!sample.root_coherent||!sample.handle.coherent)return out;
    const auto current=inspect_handle(base,sample.entity_handle,read);
    out.available=true;out.full_handle_still_maps=current.coherent&&current.entity==sample.handle.entity;
    out.entity_matches=out.full_handle_still_maps&&current.entity==owner.entity;return out;
}
template<class Read> void advance_correlation(Correlation& value,bool& done,std::uintptr_t base,const Snapshot& sample,
    std::uint64_t observed_ms,std::uint64_t now,const Owner& owner,Read&& read)noexcept{
    if(done)return;
    value=correlate(base,sample,observed_ms,now,owner,read);
    done=value.entity_matches||now<observed_ms||now-observed_ms>250;
}
// No catch: the original's native exception propagation remains intact.
// Observe must be noexcept; both observed signatures return void.
template<class Original,class Observe,class GetError,class SetError,class... Args>
void forward_observed(Original original,Observe&& observe,GetError&& get_error,SetError&& set_error,Args... args){
    const auto incoming=get_error();set_error(incoming);original(args...);
    const auto returned=get_error();observe(args...);set_error(returned);
}
}
