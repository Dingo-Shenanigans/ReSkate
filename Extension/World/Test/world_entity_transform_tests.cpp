#include "Extension/World/world_entities_contract.h"
#include <cstdio>
#include <map>
#include <stdexcept>
using namespace dingosdk;
int main() { try {
    constexpr std::uintptr_t base=0x10000000, entity=0x2000, owner=0x3000, context=0x4000, source=0x5000, transform=0x6000;
    namespace p=addr::world_props;
    const std::map<std::uintptr_t,std::uintptr_t> valid{
        {entity+p::entity_context,context},{entity+p::entity_owner,owner},{owner+p::bus_context,context},
        {owner+p::bus_transform,transform},{source+p::bus_transform,0x7000},{transform,base+addr::native_skater::blueprint_transform_vtable}};
    auto memory=valid; bool animated{}; unsigned checks{};
    const auto read=[&](auto at) { const auto i=memory.find(at);return i==memory.end()?std::uintptr_t{}:i->second; };
    const auto byte=[&](auto) { return animated; };
    const auto get=[&](auto bus,auto parent) { return world_entities::detail::object_blueprint_transform(base,entity,owner,context,bus,parent,read,byte); };
    const auto check=[&](bool yes,const char* why) { ++checks;if(!yes)throw std::runtime_error(why); };
    check(get(owner,source)==transform,"An isolated object's blueprint transform can move");
    check(get(source,source)==0,"The skater source bus cannot be claimed");
    check(get(owner,owner)==0,"A source bus cannot be moved as its child");
    check(get(0,source)==0&&get(owner,0)==0,"Missing creation ownership is refused");
    for(const auto at:{entity+p::entity_context,entity+p::entity_owner,owner+p::bus_context,owner+p::bus_transform,transform}) {
        memory=valid;memory[at]=0;check(get(owner,source)==0,"Missing or recycled native ownership is refused");
    }
    memory=valid;memory[source+p::bus_transform]=transform;check(get(owner,source)==0,"A shared parent transform is never written");
    memory=valid;animated=true;check(get(owner,source)==0,"An animation-driven transform is never written");
    animated=false;memory=valid;memory[owner+p::bus_transform]=std::numeric_limits<std::uintptr_t>::max();
    check(get(owner,source)==0,"Invalid transform arithmetic is refused");
    std::printf("%u object blueprint transform checks passed; native visual validation remains required.\n",checks);return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;} }
