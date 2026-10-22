#include "Extension/BMX/bmx_bounded_fifo.h"
#include "Extension/BMX/bmx_fall_lifecycle.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>

std::size_t allocations{};
void* operator new(std::size_t size) {
    ++allocations;
    if (auto* p=std::malloc(size)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
namespace f=dingosdk::bmx::fall_lifecycle;
void require(bool condition,const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Report {
    f::History<> history;
    std::filesystem::path root;
    std::uint64_t sequence{},getup_generation{},write_after{};
    unsigned write_attempts{};
};
using Queue=dingosdk::bmx::BoundedFifo<Report,8>;
f::Identity identity(unsigned fall) {
    return {{1,2,3,4,5},{6,7,8,9,fall}};
}
f::Observation observation(unsigned fall,unsigned edge=0) {
    f::Observation out;
    out.rider_identity={true,identity(fall).rider};out.body_identity={true,identity(fall).body};
    out.clock={true,{1000*fall+edge,100.*fall+edge/60.,1.f/60}};out.coherent_epoch=true;
    out.rider.available=out.body.available=true;
    out.rider.value.root={float(fall),float(edge),0};out.record_edge=edge<=15;
    return out;
}
void seal(f::History<>& history,unsigned fall) {
    history.reset();
    const f::RequestReceipt receipt{true,true,true,true,true,2,fall,100+fall};
    require(history.begin(identity(fall),receipt,observation(fall)),"begin source fall");
    for(unsigned edge=1;edge<=15;++edge)history.observe(observation(fall,edge));
    require(history.finish(f::Reason::body_released),"seal source fall");
}
int main() {
    auto queue=std::make_unique<Queue>();
    auto active=std::make_unique<f::History<>>();
    auto lab_history=std::make_unique<f::History<>>();
    require(!queue->front()&&!queue->pop(),"empty FIFO is harmless");
    for(unsigned fall=1;fall<=8;++fall) {
        seal(*active,fall);
        std::filesystem::path root=L"C:\\isolated-profile\\bmx-traces";
        const auto before=allocations;
        require(queue->try_emplace([&](Report& slot) noexcept {
            slot.history=*active;slot.root.swap(root);slot.sequence=fall;
            slot.getup_generation=200+fall;slot.write_after=300+fall;slot.write_attempts=0;
        }),"every sealed fall fits until explicit capacity");
        active->reset();
        require(allocations==before,"sealing into preallocated report slots allocates nothing");
    }
    unsigned fills{};
    require(!queue->try_emplace([&](Report&) noexcept { ++fills; })&&fills==0&&queue->size()==8,
        "full queue refuses newest report without touching existing slots");
    for(unsigned fall=1;fall<=8;++fall) {
        auto* head=queue->front();require(head&&head->sequence==fall,"all retained falls flush in FIFO order");
        const auto& status=head->history.status();
        require(status.phase==f::Phase::frozen&&status.identity.body.token==fall&&status.receipt.request_id==fall&&
            status.receipt.generation==100+fall&&head->getup_generation==200+fall,
            "body token, request ID and both source generations stay with their own history");
        require(head->write_after==300+fall&&head->write_attempts==0&&!head->root.empty(),
            "root and retry metadata do not inherit the next fall");
        require(head->history.size()==16&&head->history.initial().clock.serial==1000*fall&&
            head->history.initial().rider.value.root[0]==float(fall),"immutable initial row survives every later fall");
        unsigned edge{};
        head->history.each([&](const f::Sample& sample) {
            require(sample.clock.serial==1000*fall+edge&&sample.rider.value.root[1]==float(edge),
                "all fifteen early dense edges preserve exact order and source serial");
            ++edge;
        });
        const auto before=allocations;
        ++head->write_attempts;head->write_after+=2000;
        require(queue->front()==head&&queue->size()==9-fall&&head->history.status().receipt.request_id==fall,
            "failed write retains the same head and cannot consume a later fall");
        require(allocations==before,"failed-write metadata change has no allocation");
        if(fall==3)*lab_history=head->history;
        require(queue->pop(),"successful write consumes exactly one head");
    }
    require(!queue->front()&&queue->size()==0,"all eight reports drain without lost histories");
    require(lab_history->status().receipt.request_id==3&&lab_history->status().identity.body.token==3&&
        lab_history->initial().clock.serial==3000&&lab_history->size()==16,
        "a separately pinned lab history survives later ordinary flushes and queue pops");
    for(unsigned cycle=0;cycle<30;++cycle) {
        require(queue->try_emplace([&](Report& slot) noexcept {slot.sequence=100+cycle;slot.write_attempts=0;}),
            "slots remain usable after multiple ring wraps");
        require(queue->front()->sequence==100+cycle&&queue->front()->write_attempts==0,"reused slot has fresh retry metadata");
        require(queue->pop(),"wrapped report drains");
    }
    for(unsigned id=1;id<=2;++id)
        require(queue->try_emplace([&](Report& slot) noexcept {slot.sequence=id;slot.write_attempts=0;}),
            "begin bounded retry-drop fixture");
    for(unsigned attempt=1;attempt<=3;++attempt) {
        auto* head=queue->front();require(head&&head->sequence==1,"all retries stay bound to the original report ID");
        ++head->write_attempts;
        require(head->write_attempts==attempt,"write failures accumulate only on the original head");
        if(attempt==3)(void)queue->pop();
    }
    require(queue->front()&&queue->front()->sequence==2&&queue->front()->write_attempts==0,
        "dropping an exhausted head preserves the next report with its own fresh retry budget");
    std::cout << "BMX bounded FIFO preserves per-fall dense histories, identities, metadata and retry ordering without enqueue allocation.\n";
    std::cout << "sizeof(History)=" << sizeof(f::History<>) << "; sizeof(Report)=" << sizeof(Report)
        << "; sizeof(FIFO8)=" << sizeof(Queue) << '\n';
}
