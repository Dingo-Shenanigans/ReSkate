#include "Extension/BMX/bmx_ride_trace.h"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace dingosdk::bmx;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int main(){try{
    ride_trace::History<3> history;
    ride_trace::Sample sample;sample.step.ready=true;sample.step.body_token=4;
    for(std::uint64_t serial=10;serial<=15;++serial){
        sample.step.serial=serial;sample.input.left_x=double(serial);
        sample.step.shadow.enabled=true;sample.step.shadow.evaluated=true;sample.step.shadow.valid=true;
        sample.step.shadow.serial=serial;sample.step.shadow.token=sample.step.body_token;
        sample.step.shadow.force={double(serial),0,0};
        require(history.record(sample),"fresh accepted ride step is recorded");
    }
    std::vector<std::uint64_t> serials;std::vector<double> inputs;
    history.each([&](const auto& row){serials.push_back(row.step.serial);inputs.push_back(row.input.left_x);});
    require(serials==std::vector<std::uint64_t>{13,14,15},"ring wrap preserves newest accepted steps in chronological order");
    require(inputs==std::vector<double>{13,14,15},"each retained input belongs to its accepted step");
    history.each([](const auto& row){
        require(row.step.shadow.enabled&&row.step.shadow.evaluated&&row.step.shadow.valid&&
            row.step.shadow.serial==row.step.serial&&row.step.shadow.token==row.step.body_token&&
            row.step.shadow.force[0]==double(row.step.serial),"fixed ride history retains the exact shadow report belonging to each accepted epoch");
    });
    require(history.size()==3&&history.token()==4,"history remains bounded and pinned to its mount");
    require(!history.record(sample),"duplicate step cannot rewrite evidence");sample.step.serial=14;
    require(!history.record(sample),"older native step cannot rewrite evidence");
    sample.step.serial=16;sample.step.body_token=5;
    require(!history.record(sample),"a different body token cannot join an existing mount history");
    require(history.seal()&&!history.seal(),"nonempty evidence seals exactly once");sample.step.body_token=4;
    require(!history.record(sample),"sealed impact evidence rejects later writes");
    history.reset();require(!history.size()&&!history.token()&&!history.seal(),"reset clears mount identity and leaves an empty history unsealed");
    sample.step.body_token=0;sample.step.serial=1;
    require(!history.record(sample),"missing body identity is never evidence");sample.step.body_token=5;sample.step.serial=0;
    require(!history.record(sample),"missing native serial is never evidence");sample.step.serial=1;
    require(history.record(sample),"new mount may begin with its own native serial");
    auto snapshot=history;sample.step.serial=2;require(history.record(sample),"live mount continues after snapshot");
    require(snapshot.size()==1,"copied evidence does not grow with the live history");
    snapshot.each([](const auto& row){require(row.step.serial==1&&row.step.body_token==5,"copied evidence retains its original serial and body");});
    sample.step.serial=3;sample.step.ready=false;require(!history.record(sample),"unaccepted chassis step is never recorded");
    std::cout<<"ride trace history tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
