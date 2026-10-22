#include "Extension/BMX/bmx_impact_receipt.h"
#include <iostream>
#include <stdexcept>

namespace b=dingosdk::bmx;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
b::bail::Status typed_status(){
    b::bail::Status status;status.ready=true;status.intent_status_available=true;
    status.request_id=41;status.entity=0x123000;status.intent_armed=true;status.intent_generation=9;
    return status;
}
}
int main(){try{
    auto status=typed_status();
    const auto receipt=b::impact_receipt::capture(true,status,status.entity);
    require(receipt.accepted&&receipt.status_available&&receipt.intent_armed&&receipt.request_id==41&&
        receipt.generation==9&&receipt.entity==status.entity,"receipt records the accepted request and its qualified owner/generation");
    require(b::impact_receipt::matches(receipt,status),"current typed request matches its own receipt");
    status.intent_written=true;status.intent_reset=true;status.ragdoll_observed=true;status.consumed=true;
    require(b::impact_receipt::matches(receipt,status),"later observations may advance within the same accepted request");
    require(!b::impact_receipt::matches(b::impact_receipt::capture(false,status,status.entity),status),
        "a rejected new crash never consumes a previous globally successful fall/reset");
    auto unavailable=status;unavailable.intent_status_available=false;
    require(!b::impact_receipt::matches(b::impact_receipt::capture(true,unavailable,status.entity),status),
        "unavailable initial status cannot become a valid receipt merely because a later read succeeds");
    require(!b::impact_receipt::matches(receipt,unavailable),"unavailable current status cannot complete the receipt");
    auto changed=status;changed.request_id++;
    require(!b::impact_receipt::matches(receipt,changed),"another accepted request cannot satisfy the older impact");
    changed=status;changed.intent_generation++;
    require(!b::impact_receipt::matches(receipt,changed),"new pulse generation cannot satisfy an older impact");
    changed=status;changed.intent_generation=0;
    require(!b::impact_receipt::matches(receipt,changed),"missing current pulse generation cannot match typed evidence");
    require(!b::impact_receipt::matches(b::impact_receipt::capture(true,changed,changed.entity),changed),
        "missing captured pulse generation never forms typed evidence");
    changed=status;changed.entity++;
    require(!b::impact_receipt::matches(receipt,changed),"another native rider cannot satisfy the impact");
    require(!b::impact_receipt::matches(b::impact_receipt::capture(true,status,changed.entity),changed),
        "initial status from a different rider remains invalid even if the later owner matches the expected rider");
    require(!b::impact_receipt::matches(b::impact_receipt::capture(true,status,0),status),"missing expected rider is unavailable");
    changed=status;changed.owner_changed=true;
    require(!b::impact_receipt::matches(receipt,changed),"owner replacement invalidates retained entity values");
    changed=status;changed.intent_owner_expired=true;
    require(!b::impact_receipt::matches(receipt,changed),"expired pulse ownership cannot certify a completed impact");
    changed=status;changed.intent_armed=false;
    require(!b::impact_receipt::matches(receipt,changed),"legacy leaf status cannot satisfy a typed pulse receipt");
    auto leaf=status;leaf.intent_armed=false;leaf.intent_generation=0;
    const auto leaf_receipt=b::impact_receipt::capture(true,leaf,leaf.entity);
    require(b::impact_receipt::matches(leaf_receipt,leaf),"accepted legacy leaf uses matching request and rider without a pulse generation");
    leaf.intent_generation=99;
    require(b::impact_receipt::matches(leaf_receipt,leaf),"irrelevant retained pulse counters cannot reject a correctly identified legacy request");
    leaf.intent_armed=true;
    require(!b::impact_receipt::matches(leaf_receipt,leaf),"typed pulse status cannot satisfy a legacy leaf receipt");
    changed=status;changed.request_id=0;
    require(!b::impact_receipt::matches(b::impact_receipt::capture(true,changed,changed.entity),changed),"zero request identity is never accepted evidence");
    changed=status;changed.ready=false;
    require(!b::impact_receipt::matches(receipt,changed)&&
        !b::impact_receipt::matches(b::impact_receipt::capture(true,changed,changed.entity),status),
        "uninitialized current or captured status is unavailable despite default intent availability");
    require(!b::impact_receipt::matches({},status),"empty receipt never matches old global status");
    std::cout<<"impact receipt identity tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
