#include "Runtime/request_scheduler.h"
#include <iostream>
#include <stdexcept>
using namespace dingosdk;
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main() { try {
    runtime::RequestScheduler queue; const runtime::RequestContext ready{true,true,true,true};
    const overlay::DebugRequest prior{overlay::DebugAction::set_first_person,true};
    check(queue.enqueue(prior,ready,1),"Initial enqueue failed");
    const overlay::DebugRequest batch[]{{overlay::DebugAction::set_first_person_fov,false,100},
        {overlay::DebugAction::set_first_person_spring,true}};
    check(!queue.enqueue_batch(batch,{},1),"Unready profile accepted");
    check(queue.enqueue_batch(batch,ready,1),"Valid profile rejected");
    auto take=[&]() {auto item=queue.take<overlay::DebugRequest>(1);queue.finish_work();return item;};
    check(take()->action==prior.action,"Profile reordered prior work");
    check(take()->action==batch[0].action && take()->action==batch[1].action,"Profile lost FIFO order");
    check(!take(),"Rejected profile leaked requests");
    for(unsigned i=0;i<15;++i) check(queue.enqueue(prior,ready,1),"Queue filled too early");
    check(!queue.enqueue_batch(batch,ready,1),"Over-capacity batch accepted");
    unsigned count{};while(const auto item=take()) {check(item->action==prior.action,"Partial profile leaked");++count;}
    check(count==15,"Failed profile changed original queue");
    std::cout<<"Native profile queue admission passed\n";return 0;
} catch(const std::exception& e) {std::cerr<<e.what();return 1;} }
