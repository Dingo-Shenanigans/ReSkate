#include "Extension/BMX/bmx_pose_write.h"
#include "Extension/BMX/bmx_board_visibility.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
namespace writer = dingosdk::bmx::rider::pose_write;
int failures{};
void check(bool condition, const char* text) {
    if (!condition) { std::printf("FAIL: %s\n", text); ++failures; }
}
struct Pages {
    static constexpr std::size_t size = 8 * 4096;
    std::uint8_t* data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    ~Pages() { if(data)VirtualFree(data,0,MEM_RELEASE); }
    std::uintptr_t address() const { return reinterpret_cast<std::uintptr_t>(data); }
    bool protect(DWORD protection, std::size_t offset=0) {
        DWORD previous{};return VirtualProtect(data+offset,4096,protection,&previous)!=FALSE;
    }
    void reset() { std::memset(data,0x5a,size); }
    bool matches(const std::vector<std::uint8_t>& expected) const {
        return std::memcmp(data,expected.data(),size)==0;
    }
};
void placement_revision_join() {
    writer::PlacementGate gate;
    const auto empty=gate.begin_tick();check(!gate.has_candidate()&&!gate.ready(empty),"tick starts with no candidate or pose admission");
    const auto first=gate.publish(true,501);check(gate.has_candidate()&&!gate.ready(first),"publishing active frame stages only");
    check(!gate.complete(first,501,false,true)&&!gate.ready(first),"partial or missing placement cannot admit pose or board hide");
    check(!gate.complete(first,502,true,true)&&!gate.ready(first),"another entity placement cannot admit current rider");
    check(!gate.complete(first,501,true,false)&&!gate.ready(first),"a slow placement cannot refresh an expired frame");
    check(gate.complete(first,501,true,true)&&gate.ready(first),"exact fresh successful placement admits its frame");
    const auto next=gate.publish(true,501);check(!gate.ready(first)&&!gate.ready(next),"new trick or mount frame immediately revokes old admission");
    check(!gate.complete(first,501,true,true)&&!gate.ready(next),"old receipt cannot admit a newer frame on same entity");
    check(gate.complete(next,501,true,true)&&gate.ready(next),"new exact receipt admits newer frame");
    check(!gate.complete(first,501,false,true)&&gate.ready(next),"stale failure cannot revoke completed newer frame");
    const auto tick=gate.begin_tick();check(!gate.ready(next)&&!gate.complete(tick,501,true,true),"no-publication tick never reuses previous active frame");
    const auto off=gate.publish(false,0);check(!gate.ready(off)&&!gate.complete(off,0,true,true),"inactive withdrawal is immediate regardless cleanup outcome");
    const auto carry=gate.publish(true,501);check(gate.complete(carry,501,true,true)&&gate.ready(carry),"carry or parked modes can use the same exact placement join");
    writer::PlacementGate exhausted(std::numeric_limits<std::uint64_t>::max());
    check(!exhausted.publish(true,501)&&!exhausted.has_candidate()&&!exhausted.complete(1,501,true,true),"revision exhaustion cannot wrap into an ancient receipt");
}
void writable_lanes(Pages& memory) {
    memory.reset();std::vector<std::uint8_t> expected(memory.data,memory.data+Pages::size);
    const auto buffer=memory.address()+64;
    const std::array<float,4> position{1.25f,-.5f,2.f,123.f},rotation{0,.6f,0,.8f};
    for(const auto index:{std::size_t{0},std::size_t{199},std::size_t{394}}) {
        check(writer::write(buffer,395,index,writer::Lane::position,position.data()),"fresh writable XYZ lane succeeds");
        std::memcpy(expected.data()+64+index*48+32,position.data(),12);
        check(memory.matches(expected),"XYZ write preserves its W, neighboring scale/quaternion lanes and all other bones");
        check(writer::write(buffer,395,index,writer::Lane::rotation,rotation.data()),"fresh writable quaternion lane succeeds");
        std::memcpy(expected.data()+64+index*48+16,rotation.data(),16);
        check(memory.matches(expected),"quaternion write preserves scale, position and all other bones");
    }
}
void rejected_bounds_and_source(Pages& memory) {
    memory.reset();const std::vector<std::uint8_t> expected(memory.data,memory.data+Pages::size);
    const auto buffer=memory.address()+64;const std::array<float,4> value{0,0,0,1};
    const auto reject=[&](std::uintptr_t start,std::size_t count,std::size_t index,writer::Lane lane,const float* source) {
        check(!writer::write(start,count,index,lane,source),"invalid lane/count/index/range/source is rejected");
        check(memory.matches(expected),"invalid requests never touch the destination or its neighbors");
    };
    reject(buffer,0,0,writer::Lane::rotation,value.data());
    reject(buffer,513,0,writer::Lane::rotation,value.data());
    reject(buffer,395,395,writer::Lane::rotation,value.data());
    reject(buffer+1,395,0,writer::Lane::rotation,value.data());
    reject(0,395,0,writer::Lane::rotation,value.data());
    reject(writer::highest-48,395,0,writer::Lane::rotation,value.data());
    reject(buffer,395,0,static_cast<writer::Lane>(99),value.data());
    reject(buffer,395,0,writer::Lane::rotation,nullptr);
    reject(buffer,395,0,writer::Lane::rotation,reinterpret_cast<const float*>(std::uintptr_t{4}));
    reject(buffer,395,0,writer::Lane::rotation,reinterpret_cast<const float*>(writer::highest-4));
    auto bad=value;bad[1]=std::numeric_limits<float>::quiet_NaN();
    reject(buffer,395,0,writer::Lane::rotation,bad.data());
    bad=value;bad[2]=std::numeric_limits<float>::infinity();
    reject(buffer,395,0,writer::Lane::position,bad.data());
    // No assumption about native 16-byte alignment: explicitly reject the
    // two float-aligned cases whose exact lanes would straddle a page.
    reject(memory.address()+4096-32-8,395,0,writer::Lane::position,value.data());
    reject(memory.address()+4096-16-8,395,0,writer::Lane::rotation,value.data());
}
void protected_destination(Pages& memory,DWORD protection,const char* description) {
    memory.reset();const std::vector<std::uint8_t> expected(memory.data,memory.data+Pages::size);
    const std::array<float,4> value{0,0,0,1};
    if(!memory.protect(protection)) {check(false,"fixture page protection setup succeeds");return;}
    const bool result=writer::write(memory.address()+64,395,0,writer::Lane::rotation,value.data());
    // PAGE_GUARD is one-shot: Windows consumes it when the write faults. The
    // helper catches the exception, but does not promise to restore that flag.
    check(!result,description);
    if(!memory.protect(PAGE_READWRITE)) {check(false,"fixture page protection cleanup succeeds");return;}
    check(memory.matches(expected),"same-page protected-write fault leaves the lane and every neighbor unchanged");
}
void unreadable_source(Pages& memory) {
    memory.reset();const std::vector<std::uint8_t> expected(memory.data,memory.data+Pages::size);
    if(!memory.protect(PAGE_NOACCESS,7*4096)) {check(false,"fixture unreadable source setup succeeds");return;}
    check(!writer::write(memory.address()+64,395,0,writer::Lane::rotation,
        reinterpret_cast<const float*>(memory.address()+7*4096)),"unreadable source faults before destination mutation");
    if(!memory.protect(PAGE_READWRITE,7*4096)) {check(false,"fixture source protection cleanup succeeds");return;}
    check(memory.matches(expected),"source fault preserves destination and surrounding bytes");
}
struct BoardBone {
    std::array<float,4> scale{},rotation{},position{};
    bool operator==(const BoardBone&) const = default;
};
static_assert(sizeof(BoardBone)==48 && offsetof(BoardBone,scale)==0 &&
    offsetof(BoardBone,rotation)==16 && offsetof(BoardBone,position)==32);
bool read_bytes(std::uintptr_t source,void* destination,std::size_t bytes) noexcept {
    __try { std::memcpy(destination,reinterpret_cast<const void*>(source),bytes);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void published_output_guard() {
    const std::uintptr_t data=0x20000,buffer=0x40000;
    std::uintptr_t observed=buffer;bool available=true;unsigned reads{};
    const auto read=[&](std::uintptr_t p,void* out,std::size_t bytes) noexcept {
        ++reads;if(!available||p!=data+0x10||bytes!=sizeof(observed))return false;
        std::memcpy(out,&observed,bytes);return true;
    };
    check(writer::published_buffer_matches(read,data,buffer),"borrowed published output exactly matches independently resolved pose");
    observed=buffer+48;
    check(!writer::published_buffer_matches(read,data,buffer),"different current render output cannot be overwritten or measured as rendered");
    observed=buffer;available=false;
    check(!writer::published_buffer_matches(read,data,buffer),"unavailable publication is unknown, never a retained-buffer fallback");
    available=true;const auto before=reads;
    check(!writer::published_buffer_matches(read,0,buffer)&&!writer::published_buffer_matches(read,writer::highest-8,buffer)&&
        !writer::published_buffer_matches(read,data,0)&&!writer::published_buffer_matches(read,data,buffer+1),"invalid data/expected spans rejected");
    check(reads==before,"invalid spans never call reader");
    check(writer::published_buffer_matches(read,data,buffer),"fresh initial proof succeeds");
    observed=buffer+48;
    check(!writer::published_buffer_matches(read,data,buffer),"a changed publication fails the second prewrite proof");
}
void ancestor_commit_guard(Pages& memory) {
    memory.reset();const auto buffer=memory.address()+64;
    std::array<BoardBone,8> original{};
    for(unsigned i=0;i<original.size();++i) {
        original[i].scale={1.f+i/32.f,1.f,1.f,1000.f+i};
        original[i].rotation={0,0,0,1};original[i].position={float(i),2.f,3.f,-2000.f-float(i)};
    }
    constexpr int hips=6;const std::array<int,8> parents{-1,0,1,2,3,4,5,0};
    const auto reset=[&] { std::memcpy(reinterpret_cast<void*>(buffer),original.data(),sizeof(original)); };
    unsigned reads=0,writes=0;
    const auto read=[&](std::uintptr_t source,void* destination,std::size_t bytes) {
        ++reads;check(bytes==sizeof(BoardBone),"ancestor guard reads only one fixed bone at a time");
        return read_bytes(source,destination,bytes);
    };
    const auto guard=[&] {return writer::ancestors_unchanged<BoardBone>(read,buffer,8,original,parents,hips);};
    const auto commit=[&] {
        if(!guard())return false;
        ++writes;if(!writer::write(buffer,8,hips,writer::Lane::position,original[hips].position.data()))return false;
        ++writes;return writer::write(buffer,8,hips,writer::Lane::rotation,original[hips].rotation.data());
    };
    const auto refuse=[&] {
        const std::vector<std::uint8_t> before(memory.data,memory.data+Pages::size);writes=0;
        check(!commit()&&writes==0,"stale ancestor refuses the entire solution before any hips or quaternion write");
        check(memory.matches(before),"ancestor refusal leaves every native destination byte unchanged");
    };
    reset();const std::vector<std::uint8_t> fresh(memory.data,memory.data+Pages::size);
    check(guard()&&reads==6,"all ancestors through the root are checked with a bounded read count");
    check(memory.matches(fresh),"successful ancestor guard is read-only");
    check(commit()&&writes==2,"unchanged ancestors allow the existing lane writes");
    for(unsigned ancestor=0;ancestor<6;++ancestor)for(unsigned axis=0;axis<4;++axis) {
        reset();auto live=original;
        if(axis<3) {live[ancestor].position[axis]+=.01f;std::memcpy(reinterpret_cast<void*>(buffer),live.data(),sizeof(live));refuse();}
        reset();live=original;
        if(axis<3) {live[ancestor].scale[axis]+=.01f;std::memcpy(reinterpret_cast<void*>(buffer),live.data(),sizeof(live));refuse();}
        reset();live=original;live[ancestor].rotation[axis]+=.01f;
        std::memcpy(reinterpret_cast<void*>(buffer),live.data(),sizeof(live));refuse();
    }
    reset();auto metadata=original;
    for(auto& bone:metadata) {bone.scale[3]=std::numeric_limits<float>::quiet_NaN();bone.position[3]=9999.f;}
    metadata[hips].position[0]+=5;metadata[7].position[0]+=100;
    std::memcpy(reinterpret_cast<void*>(buffer),metadata.data(),sizeof(metadata));
    check(guard(),"metadata W, hips itself and nonancestor changes do not reject unchanged source ancestors");
    reset();auto nonfinite=original;nonfinite[0].position[0]=std::numeric_limits<float>::infinity();
    std::memcpy(reinterpret_cast<void*>(buffer),nonfinite.data(),sizeof(nonfinite));refuse();
    reset();
    for(const int invalid:{-2,4,5,8}) {
        auto bad=parents;bad[4]=invalid;
        const auto before=std::vector<std::uint8_t>(memory.data,memory.data+Pages::size);
        check(!writer::ancestors_unchanged<BoardBone>(read,buffer,8,original,bad,hips),"negative, self, cyclic and out-of-range ancestor links reject");
        check(memory.matches(before),"invalid ancestor chain cannot write native pose bytes");
    }
    auto rootless=parents;rootless[hips]=-1;
    check(!writer::ancestors_unchanged<BoardBone>(read,buffer,8,original,rootless,hips),"hips without a parent cannot authorize a parent-local write");
    const auto reject_bounds=[&](std::uintptr_t start,std::size_t count,std::span<const BoardBone> snapshot,std::span<const int> tree,int joint) {
        reads=0;check(!writer::ancestors_unchanged<BoardBone>(read,start,count,snapshot,tree,joint),"invalid ancestor range or snapshot rejects");
        check(reads==0,"invalid ancestor range is refused before reading native bytes");
    };
    reject_bounds(buffer,0,original,parents,hips);reject_bounds(buffer,513,original,parents,hips);
    reject_bounds(buffer,7,original,parents,hips);reject_bounds(buffer,8,std::span<const BoardBone>(original).first(7),parents,hips);
    reject_bounds(buffer,8,original,{},hips);reject_bounds(buffer,8,original,parents,-1);reject_bounds(buffer,8,original,parents,8);
    reject_bounds(0,8,original,parents,hips);reject_bounds(buffer+1,8,original,parents,hips);
    reject_bounds(writer::highest-48,8,original,parents,hips);
    for(unsigned fail_on=1;fail_on<=6;++fail_on) {
        unsigned calls=0;writes=0;const auto before=std::vector<std::uint8_t>(memory.data,memory.data+Pages::size);
        const auto failed_read=[&](std::uintptr_t source,void* destination,std::size_t bytes) {
            if(++calls==fail_on)return false;return read_bytes(source,destination,bytes);
        };
        if(writer::ancestors_unchanged<BoardBone>(failed_read,buffer,8,original,parents,hips)) {
            ++writes;writer::write(buffer,8,hips,writer::Lane::position,original[hips].position.data());
        }
        check(calls==fail_on&&writes==0&&memory.matches(before),"read failure at any ancestor prevents every native write");
    }
    if(!memory.protect(PAGE_NOACCESS,7*4096)) {check(false,"ancestor fixture protection setup succeeds");return;}
    check(!writer::ancestors_unchanged<BoardBone>(read_bytes,memory.address()+7*4096,8,original,parents,hips),
        "unreadable native ancestor safely refuses under the caller's guarded reader");
    if(!memory.protect(PAGE_READWRITE,7*4096))check(false,"ancestor fixture protection cleanup succeeds");
}
void board_descendants_only(Pages& memory) {
    memory.reset();const auto buffer=memory.address()+64;
    std::array<BoardBone,16> original{};
    for(unsigned i=0;i<original.size();++i) {
        original[i].scale={1.f+i/32.f,1.f,1.f,1000.f+i};
        original[i].rotation={0,0,0,1};original[i].position={float(i),2.f,3.f,-2000.f-float(i)};
    }
    original[4].scale[0]=original[4].scale[1]=original[4].scale[2]=0;
    std::memcpy(reinterpret_cast<void*>(buffer),original.data(),sizeof(original));
    auto expected=std::vector<std::uint8_t>(memory.data,memory.data+Pages::size);
    auto hidden=original;
    check(dingosdk::bmx::rider::apply_mounted_board_hide<BoardBone>(hidden,true),"exact16-bone board hide is active");
    unsigned writes=0;
    for(std::size_t i=2;i<hidden.size();++i) {
        if(std::equal(hidden[i].scale.begin(),hidden[i].scale.begin()+3,original[i].scale.begin()))continue;
        check(writer::write(buffer,16,i,writer::Lane::board_scale,hidden[i].scale.data()),"changed board descendant scaleXYZ succeeds");++writes;
        std::memset(expected.data()+64+i*48,0,12);
    }
    check(writes==13&&memory.matches(expected),"only13 changed descendants are written; both roots, scaleW, rotations, positions and neighbors remain native");
    std::array<BoardBone,16> fresh{};std::memcpy(fresh.data(),reinterpret_cast<const void*>(buffer),sizeof(fresh));
    auto again=fresh;
    check(dingosdk::bmx::rider::apply_mounted_board_hide<BoardBone>(again,true),"fresh already-hidden pose is accepted");
    unsigned repeated=0;
    for(std::size_t i=2;i<again.size();++i)if(!std::equal(again[i].scale.begin(),again[i].scale.begin()+3,fresh[i].scale.begin())) {
        ++repeated;check(writer::write(buffer,16,i,writer::Lane::board_scale,again[i].scale.data()),"repeat changed lane succeeds");
    }
    check(repeated==0&&memory.matches(expected),"already-zero current descendants perform no writes");
    const auto reject=[&](std::size_t count,std::size_t index) {
        check(!writer::write(buffer,count,index,writer::Lane::board_scale,hidden[2].scale.data()),"wrong board count or root/out-of-range index is rejected");
        check(memory.matches(expected),"rejected scale lane cannot poison roots or other native metadata");
    };
    reject(16,0);reject(16,1);reject(16,16);reject(15,2);reject(17,2);reject(395,2);
    // Release does not restore any cached pose. A later native refresh may
    // supply its own descendants; root0/1 were never modified by BMX.
    auto released=fresh;
    check(!dingosdk::bmx::rider::apply_mounted_board_hide<BoardBone>(released,false)&&released==fresh,
        "unmounted policy leaves the fresh board pose unchanged");
}
void protected_board_destination(Pages& memory,DWORD protection) {
    memory.reset();const std::vector<std::uint8_t> expected(memory.data,memory.data+Pages::size);
    const std::array<float,4> zero{};
    if(!memory.protect(protection)) {check(false,"board fixture protection setup succeeds");return;}
    check(!writer::write(memory.address()+64,16,2,writer::Lane::board_scale,zero.data()),"protected board scale lane returns false under SEH");
    if(!memory.protect(PAGE_READWRITE)) {check(false,"board fixture protection cleanup succeeds");return;}
    check(memory.matches(expected),"faulting board write leaves both roots and all neighboring fields untouched");
}
}
int main() {
    SYSTEM_INFO system{};GetSystemInfo(&system);
    check(system.dwPageSize==4096,"Windows x64 fixture uses the validated minimum page size");
    Pages memory;
    if(!memory.data) {std::puts("VirtualAlloc fixture failed");return 1;}
    placement_revision_join();writable_lanes(memory);rejected_bounds_and_source(memory);
    protected_destination(memory,PAGE_READONLY,"read-only destination returns false without changing page access");
    protected_destination(memory,PAGE_NOACCESS,"no-access destination returns false");
    protected_destination(memory,PAGE_READWRITE|PAGE_GUARD,"guard-page destination returns false");
    unreadable_source(memory);
    published_output_guard();
    ancestor_commit_guard(memory);
    board_descendants_only(memory);
    protected_board_destination(memory,PAGE_READONLY);
    protected_board_destination(memory,PAGE_NOACCESS);
    protected_board_destination(memory,PAGE_READWRITE|PAGE_GUARD);
    if(!failures)std::puts("BMX pose changed-lane write checks passed");
    return failures?1:0;
}
