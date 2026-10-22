#pragma once
// Source readback requires the native movement source to be initialized first.
// This ordering helper never weakens the caller's subsequent owner/clock/pose
// checks. A failed enable cannot produce an observed owner.
namespace dingosdk::bmx::startup_owner {
template<class Frame> struct Result {bool admitted{},enabled{},observed{};Frame frame;};
template<class Frame,class Enable,class Read> Result<Frame> initialize(bool quiet_gameplay_admitted,Enable&& enable,Read&& read) {
    Result<Frame> out;if(!quiet_gameplay_admitted)return out;out.admitted=true;
    out.enabled=bool(enable());if(!out.enabled)return out;
    out.frame=read();out.observed=true;return out;
}
}
