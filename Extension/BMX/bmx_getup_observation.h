#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace dingosdk::bmx::getup_observation {
inline constexpr std::uint64_t wall_limit_ms=45000;
inline constexpr double native_limit_seconds=30.0,sample_interval_seconds=.1;
inline constexpr unsigned capacity=320;
inline constexpr std::uint16_t domain_count=0xbc0;
inline constexpr unsigned populated_domain_budget=16;
enum class DomainResult { empty,miss,found };
// Each asset owns its scalar cursor and quota. A missing early asset cannot
// consume another asset's work or restart its search forever at domain zero.
struct DomainCursor {
    std::uint16_t next{};
    template<class Query> bool search(unsigned budget,Query&& query) noexcept {
        for(unsigned visited=0;visited<domain_count&&budget;++visited) {
            const auto domain=next;next=static_cast<std::uint16_t>((next+1)%domain_count);
            const auto result=query(domain);
            if(result==DomainResult::found)return true;
            if(result==DomainResult::miss)--budget;
        }
        return false;
    }
};
inline constexpr std::array<std::uint8_t,16> getup_guid{
    0x07,0x5b,0x0f,0x83,0x72,0x2f,0xb8,0xe7,0x83,0x0f,0x5b,0x07,0x2f,0x72,0xe7,0xb8};
inline constexpr std::array<std::uint8_t,16> allowed_guid{
    0x34,0x0d,0xda,0xb7,0x3a,0xbb,0x76,0xbc,0xb7,0xda,0x0d,0x34,0xbb,0x3a,0xbc,0x76};
struct BoolObservation {
    std::array<std::uint8_t,16> guid{};
    bool asset_verified{},getter_available{},value{},binding_present_known{},binding_present{};
};
enum class Decision { due,wait,expired,clock_rejected };
// Diagnostic sampling only. A paused clock cannot produce repeated samples,
// an advancing wall clock bounds a paused session, and no new clock epoch can
// be silently accepted as another sample of the old fall.
struct Window {
    std::uint64_t started_ms{},start_serial{},last_serial{};
    double start_time{},last_time{};
    unsigned samples{};
    bool ready{};
    bool begin(std::uint64_t serial,double time,std::uint64_t now) noexcept {
        if(!serial||!std::isfinite(time)||time<0)return false;
        *this={now,serial,0,time,0,0,true};return true;
    }
    Decision consider(std::uint64_t serial,double time,std::uint64_t now) const noexcept {
        if(!ready||now<started_ms||!std::isfinite(time)||serial<start_serial||time<start_time)
            return Decision::clock_rejected;
        if(now-started_ms>=wall_limit_ms||time-start_time>native_limit_seconds+1e-6||samples>=capacity)
            return Decision::expired;
        if(samples) {
            if(serial<last_serial||time<last_time||(serial==last_serial&&time!=last_time))return Decision::clock_rejected;
            if(serial==last_serial||time-last_time<sample_interval_seconds-1e-6)return Decision::wait;
        }
        return Decision::due;
    }
    bool commit(std::uint64_t serial,double time,std::uint64_t now) noexcept {
        if(consider(serial,time,now)!=Decision::due)return false;
        last_serial=serial;last_time=time;++samples;return true;
    }
};
} // namespace dingosdk::bmx::getup_observation
