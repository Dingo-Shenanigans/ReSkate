#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace dingosdk::multiplayer {
inline bool flag_preview_asset_name(std::string_view name) {
    constexpr std::string_view stock="ecs/activities/throwdowns/spotbattlethrowdownerinteractablevalidator_ecsprefab";
    if(name.size()!=stock.size())return false;
    for(std::size_t i=0;i<name.size();++i) {
        const auto c=name[i]>='A' && name[i]<='Z'?name[i]+('a'-'A'):name[i];
        if(c!=stock[i])return false;
    }
    return true;
}
inline bool flag_preview_graph(std::uint32_t hash,const std::array<std::uint32_t,10>& layout) {
    struct Graph {std::uint32_t hash;std::array<std::uint32_t,10> layout;};
    constexpr std::array graphs{
        Graph{0x73fd87ef,{80,832,1221,601,6,0,4,655395,4456459,16844800}},
        Graph{0x08a3b8c5,{80,976,1466,756,6,0,4,655409,4456459,16844800}},
        Graph{0x05df4a18,{80,832,1221,601,6,0,4,655395,4456459,16844800}},
        Graph{0x7a9c4313,{80,720,1205,561,6,0,4,655391,4456459,16844800}},
        Graph{0xbb2b2cdd,{80,832,1221,601,6,0,4,655395,4456459,16844800}},
        Graph{0x67122a51,{80,856,1221,601,6,0,4,655395,4456459,16844800}},
        Graph{0xdba685be,{80,720,1205,561,6,0,4,655391,4456459,16844800}}};
    for(const auto& graph:graphs)if(hash==graph.hash && layout==graph.layout)return true;
    return false;
}
// Own only an interpreter invocation's pointer, never the asset's shared page.
// Nested invocations and exceptional exits restore the exact previous page.
class FlagPreviewPage {
    std::uintptr_t* slot_{};
    std::uintptr_t original_{};
    alignas(16) std::array<std::byte,1024> bytes_;
public:
    void prepare(std::span<const std::byte> page,std::size_t offset,std::uintptr_t prefab) {
        if(page.size()>bytes_.size() || offset+sizeof(prefab)>page.size())return;
        std::memcpy(bytes_.data(),page.data(),page.size());
        std::memcpy(bytes_.data()+offset,&prefab,sizeof(prefab));
    }
    void publish(std::uintptr_t& slot) {
        slot_=&slot;original_=slot;slot=reinterpret_cast<std::uintptr_t>(bytes_.data());
    }
    ~FlagPreviewPage() {if(slot_)*slot_=original_;}
    FlagPreviewPage()=default;
    FlagPreviewPage(const FlagPreviewPage&)=delete;
};
}
