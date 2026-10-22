#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dingosdk::bmx::skeleton_phase_probe::detail {
inline constexpr std::array<std::size_t,3> limits{256,128,128};
inline constexpr std::array<std::size_t,3> offsets{0,256,384};
inline constexpr std::size_t capacity=512;
// Section bounds retained from the exact supported FBCE74D5 image. The live
// collector compares loaded section headers once before registration. These
// predicates only qualify pointer evidence; they never authorize a call.
struct ImageSection {std::uint32_t rva{},size{},flags{};};
inline constexpr std::array<ImageSection,16> image_sections{{
    {0x1000,0x58a8000,0x60000020}, // .xtls
    {0x58a9000,0x741000,0x60000020}, // .tls$
    {0x5fea000,0xf90000,0x40000040}, // .rodata
    {0x6f7a000,0x9ca000,0xc0000040}, // .udata
    {0x7944000,0x579000,0x40000040}, // .ecode
    {0x7ebd000,0x405000,0xc0000040}, // .text1
    {0x82c2000,0x1c7000,0xc0000040}, // .reloc
    {0x8489000,0x1000,0x40000040}, // .xcode
    {0x848a000,0x1000,0x40000040}, // .text
    {0x848b000,0x27000,0x40000040}, // .data1
    {0x84b2000,0x1aa000,0x42000040}, // .code
    {0x865c000,0x5572b4,0xe0000020}, // .00cfg
    {0x8bb4000,0x156a,0x40000000}, // .idata
    {0x8bb6000,0xa8,0xc0000000}, // .link
    {0x8bb7000,0x13b30,0x40000040}, // .sbss
    {0x8bcb000,0x57885c,0x40000040}, // .sdata
}};
enum class ImageUse {vtable_data,code_target};
inline bool image_range(std::uintptr_t base,std::uintptr_t address,std::size_t bytes,ImageUse use) noexcept {
    constexpr std::uintptr_t highest=0x00007fffffffffffULL,image_size=0x09144000;
    constexpr std::uint32_t readable=0x40000000u,executable=0x20000000u;
    if(base<0x10000||base>highest-image_size||address<base||!bytes)return false;
    const auto rva=address-base;
    for(const auto& section:image_sections) {
        if(rva<section.rva||rva-section.rva>=section.size||bytes>section.size-(rva-section.rva))continue;
        if(!(section.flags&readable))return false;
        // Some source code sections are writable; exact source headers, rather
        // than an invented read-only-code rule, determine executable bounds.
        return use==ImageUse::code_target?(section.flags&executable)!=0:(section.flags&executable)==0;
    }
    return false;
}
// Existing hook supplies exactly one original function and noexcept observers.
// Native exceptions propagate normally; no post callback or synthesized return.
// Error accessors are injected so the exact dispatcher is unit-testable without a game.
template<class Original,class Before,class After,class GetError,class SetError>
void invoke_observed(std::uintptr_t rig,float seconds,bool effective,Original original,
    Before&& before,After&& after,GetError&& get_error,SetError&& set_error) {
    const auto incoming_error=get_error();
    const auto cookie=before();
    set_error(incoming_error);
    original(rig,seconds,effective);
    const auto returned_error=get_error();
    if(cookie)after(cookie);
    set_error(returned_error);
}
struct Owner {
    std::uintptr_t base{}, entity{}, core{}, context{}, rig{};
    bool operator==(const Owner&) const = default;
    bool present() const noexcept {return base&&entity&&core&&context&&rig;}
};
struct Clock {bool available{};std::uint64_t serial{};double time{};};
inline bool same_clock(const Clock& a,const Clock& b) noexcept {
    return a.available&&b.available&&std::isfinite(a.time)&&std::isfinite(b.time)&&a.serial==b.serial&&a.time==b.time;
}
inline bool snapshot_qualified(const Owner& first,const Owner& last,const Clock& a,const Clock& b) noexcept {
    return first.present()&&first==last&&same_clock(a,b);
}
inline bool pair_qualified(bool before_qualified,bool after_qualified,
    const Owner& before,const Owner& after,const Clock& before_clock,const Clock& after_clock) noexcept {
    // This associates two measurements only. It is NOT a world/lifetime proof,
    // a native completion fence, or permission for a control/animation write.
    return before_qualified&&after_qualified&&before.present()&&before==after&&same_clock(before_clock,after_clock);
}
// Call-local dispatch shared by passive motion/animation seams. Observers may
// fail closed, but can never suppress/repeat the original or turn a throw into
// a returned post. No lock may be held by an observer across this call.
template<class Call,class Before,class After,class GetError,class SetError>
void invoke_call_observed(Call&& call,Before&& before,After&& after,GetError&& get_error,SetError&& set_error) {
    const auto incoming_error=get_error();
    const auto cookie=before();
    set_error(incoming_error);
    call();
    const auto returned_error=get_error();
    if(cookie)after(cookie);
    set_error(returned_error);
}
inline constexpr std::size_t producer_kinds=3,producer_steps=8,producer_per_step=2,
    producer_per_kind=16,producer_records=48,producer_snapshots=80;
enum class ProducerKind : unsigned {motion,animation,writer};
struct ProducerReservation {bool accepted{};std::size_t record{},slot{};};
struct ProducerBudget {
    Owner pinned{};bool sealed{},anchor_available{};std::uint64_t anchor{},highest_serial{};
    std::array<std::size_t,producer_kinds> taken{},unknown{};
    std::array<std::array<std::size_t,producer_steps>,producer_kinds> epochs{};
    bool same_owner(const Owner& current) noexcept {
        if(!current.present())return false; // unknown never authorizes remembered-owner reads
        if(!pinned.present()){pinned=current;return true;}
        if(pinned==current)return true;
        sealed=true;return false; // confirmed replacement is terminal, never rebound
    }
    ProducerReservation reserve(ProducerKind kind,const Owner& current,const Clock& clock) noexcept {
        const auto k=static_cast<std::size_t>(kind);
        if(sealed||k>=producer_kinds||!same_owner(current))return {};
        const bool known=clock.available&&std::isfinite(clock.time)&&clock.time>=0;
        if(known) {
            if(!anchor_available){anchor=clock.serial;highest_serial=clock.serial;anchor_available=true;}
            // Shared across ALL kinds, including attempts dropped by a full
            // kind/epoch quota. A later lower serial never rearms any quota.
            if(clock.serial<highest_serial){sealed=true;return {};}
            highest_serial=clock.serial;
            if(clock.serial-anchor>=producer_steps){sealed=true;return {};}
            if(taken[k]>=producer_per_kind)return {};
            auto& used=epochs[k][static_cast<std::size_t>(clock.serial-anchor)];
            if(used>=producer_per_step)return {};
            ++used;
        } else {
            if(taken[k]>=producer_per_kind)return {};
            if(unknown[k]>=producer_per_step)return {};
            ++unknown[k];
        }
        const auto slot=taken[k]++;
        if(taken[0]==producer_per_kind&&taken[1]==producer_per_kind&&taken[2]==producer_per_kind)sealed=true;
        return {true,k*producer_per_kind+slot,slot};
    }
};
inline constexpr std::size_t producer_pre_index(std::size_t record) noexcept {
    return record<32?record*2:64+(record-32);
}
inline constexpr std::size_t producer_post_index(std::size_t record) noexcept {
    return record<32?record*2+1:producer_snapshots;
}
inline bool producer_cookie_matches(std::uint64_t cookie,std::uint64_t invocation,
    std::uint64_t recorded_invocation,std::uint32_t thread,std::uint32_t recorded_thread) noexcept {
    return cookie&&cookie<=32&&invocation&&invocation==recorded_invocation&&thread==recorded_thread;
}
inline bool cookie_matches(std::uint64_t cookie,std::uint64_t invocation,
    std::uint64_t recorded_invocation,std::uint32_t thread,std::uint32_t recorded_thread) noexcept {
    return cookie&&cookie<=capacity&&invocation&&invocation==recorded_invocation&&thread==recorded_thread;
}
template<std::size_t N> bool finite_words(const std::array<std::uint32_t,N>& words,std::size_t begin,std::size_t count) noexcept {
    if(begin>N||count>N-begin)return false;
    for(auto i=begin;i<begin+count;++i)if(!std::isfinite(std::bit_cast<float>(words[i])))return false;
    return true;
}
// A prediction of the statically retained guards, never proof the copy ran.
inline bool copy_guard_prediction(bool effective,bool flags_available,std::uint32_t flags,
    bool offset_available,const std::array<std::uint32_t,4>& offset,bool& prediction_known) noexcept {
    prediction_known=flags_available&&offset_available&&finite_words(offset,0,3);
    if(!prediction_known)return false;
    const auto x=std::bit_cast<float>(offset[0]),y=std::bit_cast<float>(offset[1]),z=std::bit_cast<float>(offset[2]);
    const auto length_squared=x*x+y*y+z*z;
    // Keep the native-like float arithmetic and strict <1 boundary explicit.
    prediction_known=std::isfinite(length_squared);
    return prediction_known&&effective&&(flags&0x8000u)&&length_squared<1.0f;
}
}
