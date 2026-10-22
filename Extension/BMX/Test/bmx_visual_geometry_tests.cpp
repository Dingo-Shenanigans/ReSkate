#include "Extension/BMX/bmx_visual_geometry.h"
#include <iostream>
#include <stdexcept>

namespace {
using Bytes=std::vector<std::byte>;
using dingosdk::bmx::visual::detail::decode_geometry;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void word(Bytes& bytes,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=std::byte(value>>(i*8));}
void number(Bytes& bytes,std::size_t at,float value){word(bytes,at,std::bit_cast<std::uint32_t>(value));}
Bytes fixture(){
    Bytes bytes(84);constexpr char magic[]="RSBMX001";
    for(unsigned i=0;i<8;++i)bytes[i]=std::byte(magic[i]);
    word(bytes,8,1);word(bytes,12,40);word(bytes,16,1);
    for(unsigned i=0;i<6;++i)number(bytes,20+i*4,i<3?-1.f:1.f);
    word(bytes,44,5);
    constexpr std::array<float,9> points{-.5f,-.5f,-.5f,.5f,-.5f,.5f,0,.5f,0};
    for(unsigned i=0;i<9;++i)number(bytes,48+i*4,points[i]);
    return bytes;
}
}
int main(){
    try{
        const auto valid=fixture();const auto mesh=decode_geometry(valid);
        check(mesh.size()==1&&mesh[0].part==5&&mesh[0].p[0][0]==-.5f&&mesh[0].p[2][1]==.5f,"valid geometry did not round trip");
        for(std::size_t size=0;size<valid.size();++size)check(decode_geometry({valid.data(),size}).empty(),"truncated resource accepted");
        {auto bytes=valid;bytes.push_back(std::byte{});check(decode_geometry(bytes).empty(),"trailing bytes accepted");}
        {auto bytes=valid;bytes[0]=std::byte{};check(decode_geometry(bytes).empty(),"bad magic accepted");}
        for(const auto [at,value]:{std::pair<std::size_t,std::uint32_t>{8,2},{12,0},{16,0},{16,100001},{44,6},
            {20,0x7fc00000},{20,0x42c80000},{48,0x7fc00000},{48,0x42c80000}}){
            auto bytes=valid;word(bytes,at,value);check(decode_geometry(bytes).empty(),"malformed header/part/float accepted");
        }
        {auto bytes=valid;number(bytes,20,2);check(decode_geometry(bytes).empty(),"reversed bounds accepted");}
        {auto bytes=valid;number(bytes,48,-2);check(decode_geometry(bytes).empty(),"vertex below bounds accepted");}
        std::cout<<"BMX geometry decoder cases passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
