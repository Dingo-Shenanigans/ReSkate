#pragma once
#include <cstdint>
#include <optional>

namespace dingosdk {
struct CountSettingSample {
    std::uintptr_t manager{},address{},type{};
    std::uint32_t value{};
    bool operator==(const CountSettingSample&) const = default;
};
// Adds a bounded temporary allowance to a renderer count. External edits and
// replaced settings objects relinquish ownership until the effect is cleared.
class TransientCountLease {
public:
    template<class Read,class Write> bool update(std::uint32_t extra,Read&& read,Write&& write) {
        if (!extra) return restore(read,write);
        if (extra>4096 || yielded_) return false;
        const auto current=read();
        if (!current) return false;
        if (applied_ && *current!=*applied_) {original_.reset(); applied_.reset(); yielded_=true; return false;}
        if ((original_ ? original_->value : current->value)>65536-extra) return false;
        if (!original_) original_=current;
        return apply(original_->value+extra,*current,read,write);
    }
    template<class Read,class Write> bool restore(Read&& read,Write&& write) {
        if (!original_) {yielded_=false; return true;}
        const auto current=read();
        if (!current) return false;
        if (applied_ && *current==*applied_ && !apply(original_->value,*current,read,write)) return false;
        original_.reset(); applied_.reset(); yielded_=false;
        return true;
    }
private:
    template<class Read,class Write> bool apply(std::uint32_t value,const CountSettingSample& current,Read&& read,Write&& write) {
        applied_=current;
        if (value==current.value) return true;
        const bool accepted=write(current,value);
        const auto after=read();
        if (!after) {applied_->value=value; return false;}
        auto expected=current; expected.value=value;
        if (*after==expected) {applied_=after; return accepted;}
        if (*after!=current) {original_.reset(); applied_.reset(); yielded_=true;}
        return false;
    }
    std::optional<CountSettingSample> original_,applied_;
    bool yielded_{};
};
}
