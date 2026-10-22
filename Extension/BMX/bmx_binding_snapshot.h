#pragma once
#include <array>
#include <cstdint>

namespace dingosdk::bmx::binding_observer::detail {
inline constexpr std::array<std::uint32_t,2> keys{0xd3ac8f97U,0x0ef72781U};
inline constexpr std::uintptr_t highest=0x00007fffffffffffULL;
inline constexpr unsigned key_index(std::uint32_t hash) noexcept {
    for(unsigned i=0;i<keys.size();++i)if(keys[i]==hash)return i;
    return static_cast<unsigned>(keys.size());
}
struct Snapshot {
    std::uintptr_t scope{}, descriptor{}, function{}, function_rva{};
    std::uint32_t hash{}, descriptor_hash{}, descriptor_flags{};
    std::array<std::uint64_t,3> scope_words{};
    std::array<std::uint64_t,2> descriptor_words{};
    bool scope_header_coherent{}, descriptor_present{}, descriptor_coherent{}, function_in_image{};
};
inline bool span(std::uintptr_t p,std::uintptr_t size) noexcept {
    return p>=0x10000&&size&&size<=highest&&p<=highest-size;
}
// No catch: a native exception keeps its native propagation path. The observer
// is required noexcept. This wrapper is also exercised without a native game.
template<class Original,class Observe,class GetError,class SetError>
std::uintptr_t invoke_observed(std::uintptr_t scope,std::uint32_t hash,Original original,
    Observe&& observe,GetError&& get_error,SetError&& set_error) {
    const auto incoming=get_error();set_error(incoming);
    const auto result=original(scope,hash);
    const auto returned_error=get_error();
    observe(scope,hash,result);
    set_error(returned_error);return result;
}
// The reader must be bounded and nonthrowing (production: memory::peek_bytes).
// A matching reread is observation evidence, not native object ownership proof.
template<class Read> Snapshot inspect(std::uintptr_t scope,std::uint32_t hash,
    std::uintptr_t descriptor,std::uintptr_t base,std::uintptr_t image_size,Read&& read) noexcept {
    Snapshot out;
    if(key_index(hash)==keys.size())return out;
    out.scope=scope;out.hash=hash;out.descriptor=descriptor;out.descriptor_present=descriptor!=0;
    std::array<std::uint64_t,3> scope_again{};
    std::array<std::uint64_t,2> descriptor_again{};
    const bool scope_first=span(scope,sizeof(out.scope_words))&&
        read(scope,out.scope_words.data(),sizeof(out.scope_words));
    const bool descriptor_first=descriptor&&span(descriptor,sizeof(out.descriptor_words))&&
        read(descriptor,out.descriptor_words.data(),sizeof(out.descriptor_words));
    const bool descriptor_second=descriptor_first&&
        read(descriptor,descriptor_again.data(),sizeof(descriptor_again));
    out.scope_header_coherent=scope_first&&
        read(scope,scope_again.data(),sizeof(scope_again))&&out.scope_words==scope_again;
    if(descriptor_second&&out.descriptor_words==descriptor_again){
        out.function=static_cast<std::uintptr_t>(out.descriptor_words[0]);
        out.descriptor_hash=static_cast<std::uint32_t>(out.descriptor_words[1]);
        out.descriptor_flags=static_cast<std::uint32_t>(out.descriptor_words[1]>>32);
        out.descriptor_coherent=out.descriptor_hash==hash;
        out.function_in_image=out.descriptor_coherent&&span(base,image_size)&&
            out.function>=base&&out.function-base<image_size;
        if(out.function_in_image)out.function_rva=out.function-base;
    }
    return out;
}
}
