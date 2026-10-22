#pragma once
#include "bmx_getup_action.h"
#include "bmx_intent_snapshot.h"
#include <array>
#include <cstring>

namespace dingosdk::bmx::getup_callback_audit {
inline constexpr unsigned capacity=1024;
enum class Issue : unsigned { measured,callback_unqualified,owner_changed,clock_unavailable,clock_changed,
    node_changed,asset_unavailable,getter_unavailable,non_bool_value,window_closed };
enum class DispatchIssue : unsigned { not_active,unavailable,not_called,called,already_reserved,
    callback_unqualified,owner_changed,clock_unavailable,clock_changed,permission_declined,
    allowed_asset_unavailable,allowed_getter_unavailable,allowed_false,allowed_changed,getup_asset_unavailable,
    transaction_changed,cancelled_or_expired };
struct Dispatch {
    bool available{},observer_admitted{},called{},serial_already_reserved{},serial_reserved_this_callback{},
        true_entry_this_callback{},native_set_called{},clock_available{};
    std::uint64_t generation{},preflight_before{},preflight_after{},true_calls_before{},true_calls_after{},serial{};
    double native_time{};
    unsigned phase_before{},phase_after{};
    DispatchIssue issue=DispatchIssue::not_active;
};
struct Window {
    bool armed{};
    std::uint64_t started{},deadline{};
    bool begin(std::uint64_t now) noexcept {
        if(armed||now>UINT64_MAX-getup_action::start_limit_ms)return false;
        armed=true;started=now;deadline=now+getup_action::start_limit_ms;return true;
    }
    bool live(std::uint64_t now) const noexcept {return armed&&now>=started&&now<deadline;}
};
// Existing no_bail::resolve source chain, without copying protection SDK data
// or acquiring its lock. Types are supported-build RVA constants supplied by
// the adapter. Every observed chain word is reread before accepting the owner.
struct Types {
    std::uintptr_t client{},entity{},manager_offset{},manager{},player{},component{},core{};
};
template<class Read> bool owner_matches(std::uintptr_t base,const intent_observer::detail::Owner& owner,
    const Types& types,Read&& read) noexcept {
    namespace detail=intent_observer::detail;
    struct Watch {std::uintptr_t address{};std::array<unsigned char,8> bytes{};std::size_t size{};};
    std::array<Watch,40> watches{};unsigned count{};
    const auto get=[&]<class T>(std::uintptr_t address,T& value) noexcept {
        static_assert(sizeof(T)<=8);
        if(count>=watches.size()||!detail::get(address,value,read))return false;
        auto& watch=watches[count++];watch.address=address;watch.size=sizeof(T);
        std::memcpy(watch.bytes.data(),&value,sizeof(T));return true;
    };
    const auto link=[&](std::uintptr_t address,std::uintptr_t& value) noexcept {
        return get(address,value)&&detail::span(value,8);
    };
    std::uintptr_t word{},context{},manager{},begin{},end{},player{},handle{},collection{},component{},core{},
        selector{},causes{},rig{},skater_context{};
    std::uint32_t state{},offset{};std::uint8_t local{},remote{},teleport{};
    if(!owner.available||!detail::span(owner.client,8)||!detail::span(owner.entity,8)||!detail::span(owner.core,8)||
       !detail::span(owner.context,8)||!detail::span(owner.rig,8)||
       !get(owner.client,word)||word!=base+types.client||!get(owner.client+0xc4,state)||
       (state!=13&&state!=21)||!get(owner.entity,word)||word!=base+types.entity||
       !get(base+types.manager_offset,offset)||offset>0x1000000||!link(owner.client+8,context)||
       !link(context+offset,manager)||!get(owner.entity+0x20,word)||word!=context||
       !get(manager,word)||word!=base+types.manager||!link(manager+0x4c8,begin)||!link(manager+0x4d0,end)||
       end!=begin+8||!link(begin,player)||!get(player,word)||word!=base+types.player||
       !get(player+0x78,word)||word!=context||!get(player+0x45,local)||local!=1||
       !get(player+0x44,remote)||remote||!get(player+0xb8,word)||word!=owner.entity||
       !get(owner.entity+0xf8,word)||word!=player||!get(owner.entity+0x7e0,teleport)||teleport||
       !link(player+0xb0,handle)||!link(owner.entity+0x70,collection)||!link(owner.entity+0x628,component)||
       !link(component+0x70,core)||core!=owner.core||!link(core+0x3c0,skater_context)||skater_context!=owner.context||
       !link(core+0x440,selector)||!link(core+0x428,causes)||!link(core+0x438,rig)||rig!=owner.rig||
       !get(handle,word)||word!=owner.entity+8||!get(collection,word)||word!=owner.entity||
       !get(component,word)||word!=base+types.component||!get(component+0x18,word)||word!=collection||
       !get(core,word)||word!=base+types.core||!get(selector+8,word)||word!=owner.context||
       !get(causes+0x20,word)||word!=owner.context||!get(rig,word)||word!=owner.context||
       !get(rig+0x4630,word)||word!=owner.core)return false;
    for(unsigned i=0;i<count;++i) {
        std::array<unsigned char,8> after{};const auto& watch=watches[i];
        if(!read(watch.address,after.data(),watch.size)||
           std::memcmp(after.data(),watch.bytes.data(),watch.size))return false;
    }
    return true;
}
} // namespace dingosdk::bmx::getup_callback_audit
