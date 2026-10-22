#pragma once
#include "bmx_bail.h"
#include <cstdint>

namespace dingosdk::bmx::impact_receipt {
// Request identity only. Matching does not prove a fall/reset succeeded; the
// caller still evaluates the native observations belonging to this receipt.
struct Receipt {
    bool accepted{},status_available{},intent_armed{};
    std::uint64_t request_id{},generation{};
    std::uintptr_t entity{};
};
inline Receipt capture(bool accepted,const bail::Status& status,std::uintptr_t expected_entity) noexcept {
    return {accepted,status.ready&&status.intent_status_available&&expected_entity!=0&&
        status.entity==expected_entity&&!status.owner_changed&&!status.intent_owner_expired,
        status.intent_armed,status.request_id,status.intent_generation,expected_entity};
}
inline bool matches(const Receipt& receipt,const bail::Status& status) noexcept {
    if(!receipt.accepted||!receipt.status_available||!status.ready||!status.intent_status_available||
       !receipt.request_id||receipt.request_id!=status.request_id||!receipt.entity||receipt.entity!=status.entity||
       status.owner_changed||status.intent_owner_expired||receipt.intent_armed!=status.intent_armed)return false;
    return !receipt.intent_armed||(receipt.generation!=0&&receipt.generation==status.intent_generation);
}
}
