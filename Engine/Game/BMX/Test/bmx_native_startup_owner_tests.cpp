#include "Engine/Game/BMX/bmx_native_startup_owner.h"
#include <cstdio>
#include <cstdlib>
namespace s=dingosdk::bmx::startup_owner;
struct NativeStub {bool available{},standable{},timing{};unsigned identity{};};
unsigned checks{};void check(bool okay,const char* text){++checks;if(!okay){std::fprintf(stderr,"FAIL %s\n",text);std::exit(1);}}
int main(){
    // NativeFrame's real source read refuses before source_state.initialized.
    // This stub reproduces that lifecycle; no game/native API is executed.
    for(unsigned caller=0;caller<2;++caller){bool initialized=false;unsigned enables{},reads{};
        const auto read=[&](){++reads;return initialized?NativeStub{true,true,true,42}:NativeStub{};};
        check(!read().available,"fresh process cannot observe before native source initialization");
        auto blocked=s::initialize<NativeStub>(false,[&](){++enables;initialized=true;return true;},read);
        check(!blocked.admitted&&!blocked.enabled&&!blocked.observed&&!enables&&reads==1,"quiet gameplay gate prevents source enable and owner query");
        auto failed=s::initialize<NativeStub>(true,[&](){++enables;return false;},read);
        check(failed.admitted&&!failed.enabled&&!failed.observed&&reads==1,"failed source enable cannot query or invent an owner");
        auto ready=s::initialize<NativeStub>(true,[&](){++enables;initialized=true;return true;},read);
        check(ready.admitted&&ready.enabled&&ready.observed&&ready.frame.available&&ready.frame.identity==42&&reads==2,"both private callers initialize before requiring available owner");
        initialized=false;auto unavailable=s::initialize<NativeStub>(true,[](){return true;},read);
        check(unavailable.observed&&!unavailable.frame.available,"successful enable alone cannot invent a fresh available frame");
    }
    std::printf("native owner source initialization ordering: %u checks PASS\n",checks);
}
