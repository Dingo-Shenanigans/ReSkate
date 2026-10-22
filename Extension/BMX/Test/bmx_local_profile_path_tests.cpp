#include "Extension/BMX/bmx_local_profile_path.h"
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace {
namespace paths = dingosdk::bmx::local_profile_path;
constexpr std::uintptr_t base_address=0x10000000, platform_address=0x20000;
constexpr std::uintptr_t configuration_address=0x30000, path_address=0x40000;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Memory {
    std::map<std::uintptr_t, std::uint8_t> bytes;
    std::function<void(std::uintptr_t, std::size_t)> mutate;
    unsigned content_reads{};
    template<class T> void put(std::uintptr_t address, const T& value) {
        const auto* data=reinterpret_cast<const std::uint8_t*>(&value);
        for(std::size_t i=0;i<sizeof(value);++i)bytes[address+i]=data[i];
    }
    void path(std::string_view value) {
        for(std::size_t i=0;i<value.size();++i)put(path_address+i,value[i]);
        put(path_address+value.size(),char{});
    }
    bool read(std::uintptr_t address,void* data,std::size_t size) {
        if(address==path_address&&size>1)++content_reads;
        if(mutate)mutate(address,size);
        auto* out=static_cast<std::uint8_t*>(data);
        for(std::size_t i=0;i<size;++i){const auto found=bytes.find(address+i);if(found==bytes.end())return false;out[i]=found->second;}
        return true;
    }
    auto inspect(std::uintptr_t image=base_address,std::uintptr_t platform=platform_address,
        std::uintptr_t configuration=configuration_address) {
        return paths::inspect_storage_path(image,platform,configuration,
            [&](std::uintptr_t address,void* data,std::size_t size){return read(address,data,size);});
    }
    Memory(){put(platform_address,base_address+paths::pc_storage_vtable_rva);
        put(configuration_address+0x90,path_address);path("C:\\tests\\reskate\\Game\\Skate\\data");}
};
void payloads() {
    Memory memory;const std::string ascii="C:\\tests\\reskate\\Game\\Skate\\data";
    const auto result=memory.inspect();
    check(result.coherent&&result.path_pointer==path_address&&result.length==ascii.size()&&
        std::string_view(result.text.data(),result.length)==ascii&&result.text[result.length]==0,
        "ASCII native Windows absolute path is read as narrow bytes");
    const std::string utf8="C:\\tests\\r\xc3\xa9skate\\\xe8\xb7\xaf\\data";
    memory.path(utf8);const auto unicode=memory.inspect();
    check(unicode.coherent&&unicode.length==utf8.size()&&std::string_view(unicode.text.data(),unicode.length)==utf8,
        "multibyte UTF-8 survives exactly with byte length");
    memory.path(std::string(260,'x'));const auto maximum=memory.inspect();
    check(maximum.coherent&&maximum.length==260&&maximum.text[260]==0,"260-byte payload plus NUL fits the native bound");
    memory.path("x");check(memory.inspect().coherent,"one-byte payload needs no readable memory beyond its NUL");
    memory.path(std::string("x\xff",2));check(memory.inspect().coherent,"UTF-8 validity belongs to the conversion caller");
}
void malformed() {
    Memory empty;empty.path("");check(!empty.inspect().coherent,"empty path is rejected");
    Memory long_path;long_path.path(std::string(261,'x'));
    check(!long_path.inspect().coherent&&std::string_view(long_path.inspect().status)=="invalid_path_length",
        "261 nonzero bytes are rejected without an unbounded search");
    Memory unterminated;unterminated.bytes.erase(path_address+31);
    check(!unterminated.inspect().coherent,"unreadable payload or terminator is rejected");
    Memory wrong_type;wrong_type.put(platform_address,base_address+paths::pc_storage_vtable_rva+8);
    check(!wrong_type.inspect().coherent,"only exact native PC storage type is supported");
    Memory null_path;null_path.put(configuration_address+0x90,std::uintptr_t{});
    check(!null_path.inspect().coherent,"null path pointer is rejected");
    Memory overflow;overflow.put(configuration_address+0x90,std::uintptr_t{0xffffffffffffffffULL});
    check(!overflow.inspect().coherent,"overflowing path pointer is rejected before character reads");
    Memory edge;edge.put(configuration_address+0x90,paths::highest_address-1);edge.put(paths::highest_address-1,'x');
    check(!edge.inspect().coherent,"a path crossing the canonical address boundary is rejected");
    Memory missing;missing.bytes.erase(configuration_address+0x94);
    check(!missing.inspect().coherent,"unreadable configuration pointer is rejected");
    for(const auto value:{std::uintptr_t{},paths::highest_address,std::uintptr_t{0xfffffffffffffff8ULL}}){
        unsigned reads{};const auto rejected=paths::inspect_storage_path(value,platform_address,configuration_address,
            [&](auto,auto,auto){++reads;return false;});
        check(!rejected.coherent&&!reads,"invalid image base is rejected before target reads");
    }
    Memory addresses;
    check(!addresses.inspect(base_address,platform_address+1).coherent&&
        !addresses.inspect(base_address,platform_address,configuration_address+1).coherent&&
        !addresses.inspect(base_address,platform_address,paths::highest_address-0x80).coherent,
        "unaligned or overflowing native object spans are rejected");
}
void races() {
    for(unsigned kind=0;kind<5;++kind){Memory memory;
        memory.mutate=[&](std::uintptr_t address,std::size_t size){
            if(address!=path_address||size<=1)return;
            if(kind==0&&memory.content_reads==1)memory.put(path_address,'D');
            if(kind==1&&memory.content_reads==1)memory.put(configuration_address+0x90,path_address+0x100);
            if(kind==2&&memory.content_reads==1)memory.put(platform_address,base_address+paths::pc_storage_vtable_rva+8);
            if(kind==3&&memory.content_reads==2)memory.put(path_address,'D');
            if(kind==4&&memory.content_reads==2)memory.put(platform_address,base_address+paths::pc_storage_vtable_rva+8);
        };
        const auto result=memory.inspect();
        check(!result.coherent&&std::string_view(result.status)=="changed","content, pointer and platform-type races are rejected");
    }
    const auto result=paths::inspect_storage_path(base_address,platform_address,configuration_address,
        [](auto,auto,auto)->bool{throw 1;});
    check(!result.coherent&&std::string_view(result.status)=="reader_exception","reader exceptions fail closed");
}
}
int main(){payloads();malformed();races();std::cout<<"BMX native storage-path snapshot cases passed\n";}
