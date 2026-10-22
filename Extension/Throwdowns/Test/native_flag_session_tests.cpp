#include <Windows.h>
static ULONGLONG test_now = 1000;
static ULONGLONG flag_test_clock() { return test_now; }
#define GetTickCount64 flag_test_clock
#include "Extension/Throwdowns/native_flag_session.cpp"
#undef GetTickCount64
#include <iostream>
#include <limits>
#include <map>

namespace {
using namespace dingosdk::multiplayer;
namespace kit = dingosdk::throwdowns::kit;
std::uint64_t world = 100;
std::uint32_t navigation{};
std::vector<std::uint32_t> navigations, destroyed;
std::vector<kit::Event> events;
std::map<std::string,kit::Documents> documents;
struct Move { std::uint64_t request, world; std::uint32_t mmid; kit::Vec3 at; float yaw; };
std::vector<Move> moves;
bool move_available = true, events_unlocked = true;
}
namespace dingosdk::multiplayer {
std::uint64_t native_throwdown_world() noexcept { return world; }
std::uint64_t throwdown_relay_world() noexcept { return world; }
bool local_throwdown_event_active() noexcept { return false; }
void release_native_flag_placeholder() noexcept {}
bool prepare_throwdown_injection() noexcept { return true; }
bool queue_throwdown_destroy(std::uint32_t mmid) noexcept { destroyed.push_back(mmid); return true; }
bool queue_throwdown_move(std::uint64_t request, std::uint64_t generation, std::uint32_t mmid, const std::array<float,3>& at, float yaw) noexcept {
    if (!move_available) return false;
    moves.push_back({request,generation,mmid,at,yaw}); return true;
}
namespace menu_data {
unsigned size(Address) { return 4; }
void require(bool yes, const char* message) { if (!yes) throw std::runtime_error(message); }
std::vector<Root> Context::roots(std::initializer_list<std::uint32_t>) const { return {{{1,1},0}}; }
Value Context::field(Value, std::uint32_t hash) const { return {hash,1}; }
Address Context::address(Value) const { return reinterpret_cast<Address>(&navigation); }
void Context::publish(Value, const void* bytes) const { std::memcpy(&navigation,bytes,4); navigations.push_back(navigation); }
void Context::array(Value, std::span<const std::byte> bytes, unsigned count) const {
    std::int32_t metadata{}; std::memcpy(&metadata,bytes.data(),4);
    require(count==1 && metadata==-1,"native navigation metadata differs");
}
}
}
namespace dingosdk::throwdowns::kit {
std::optional<Documents> documents_for(std::string_view mode) {
    const auto found = ::documents.find(std::string(mode));
    return found==::documents.end() ? std::nullopt : std::optional(found->second);
}
std::optional<Documents> active() { return std::nullopt; }
std::string current() { return {}; }
void post(std::string_view mode, std::string action, std::vector<std::string> args) {
    auto& mutex = multiplayer::native_flag::placement().mutex;
    if (mutex.try_lock()) mutex.unlock(); else events_unlocked = false;
    events.push_back({"test",std::move(action),std::string(mode),std::move(args)});
}
void post_to(std::string_view plugin, std::string action, std::string mode, std::vector<std::string> args) {
    events.push_back({std::string(plugin),std::move(action),std::move(mode),std::move(args)});
}
}
int main() { try {
    namespace flag = kit::flag;
    const menu_data::Context context(0,0);
    unsigned checks{};
    const auto check = [&](bool ok) { if (!ok) throw std::runtime_error("native flag check " + std::to_string(checks+1)); ++checks; };
    const auto tick = [&] { ++test_now; native_flag::tick(context); };
    const auto seed = [&](const std::string& mode, std::uint32_t game, std::uint32_t mmid) {
        native_flag::abandon(); ++world; navigation=0; documents.clear();
        kit::Documents doc; doc.plugin="test"; doc.waiting=kit::Waiting{}; doc.waiting->mode=mode; doc.waiting->can_start=true;
        documents[mode]=doc;
        check(flag::begin(mode,game,{4,120,true,0})); tick(); navigation=0;
        native_flag::observe_graph(0x0f131312); native_flag::observe_created(); native_flag::observe_graph(0x9700be92);
        const auto transform=native_flag::spawn_transform({1,2,3},native_flag::yaw_facing(30));
        native_flag::observe_position(mmid,{1,2,3},&transform); native_flag::observe_exit(false); tick();
        check(flag::state().waiting && native_flag::owns_registration(mmid));
        events.clear(); moves.clear(); navigations.clear(); destroyed.clear();
    };
    std::uint64_t request{};
    check(!flag::restore("test/duel",7,{10,20,30},90,request).empty() && !request);
    for (const auto& mode : {std::string("test/duel"),std::string("test/laps")}) {
        seed(mode,7,81);
        check(!flag::restore("other/duel",7,{10,20,30},90,request).empty());
        check(!flag::restore(mode,8,{10,20,30},90,request).empty());
        check(!flag::restore(mode,7,{std::numeric_limits<float>::quiet_NaN(),0,0},90,request).empty());
        check(!flag::restore(mode,7,{100000,0,0},90,request).empty());
        check(!flag::restore(mode,7,{10,20,30},std::numeric_limits<float>::infinity(),request).empty());
        check(flag::restore(mode,7,{10,20,30},450,request).empty() && request);
        const auto accepted=request;
        check(flag::state().restoring && !flag::state().waiting && !native_flag::start());
        check(!flag::restore(mode,7,{0,0,0},0,request).empty() && !request);
        tick(); tick(); check(moves.size()==1 && moves.back().request==accepted && moves.back().at==kit::Vec3{10,20,30});
        check(navigations.empty() && destroyed.empty() && native_flag::owns_registration(81));
        native_flag::restore_sent(accepted+1,world,81); native_flag::restore_sent(accepted,world+1,81); native_flag::restore_sent(accepted,world,82);
        check(flag::state().restoring && events.empty());
        check(native_flag::restore_dispatch(accepted,world,81));
        check(!native_flag::restore_dispatch(accepted,world,81));
        native_flag::restore_sent(accepted,world,81);
        check(flag::state().waiting && !flag::state().restoring && events.size()==1 && events.back().action=="flag_restored");
        check(events.back().mode==mode && events.back().args[0]=="7" && events.back().args[1]=="10" && std::abs(std::stof(events.back().args[4])-90)<.001f && events.back().args[5]==std::to_string(accepted));
        native_flag::restore_sent(accepted,world,81); check(events.size()==1);
        check(flag::restore(mode,7,{30,40,50},0,request).empty() && request>accepted);
        const auto failed=request; tick(); native_flag::restore_failed(failed);
        check(flag::state().waiting && events.back().action=="flag_restore_failed" && events.back().args[2]==std::to_string(failed));
        move_available=false;
        check(flag::restore(mode,7,{0,0,0},0,request).empty()); tick(); test_now+=10001; tick(); move_available=true;
        check(!flag::state().restoring && flag::state().waiting && native_flag::owns_registration(81) && events.back().action=="flag_restore_failed");
        check(flag::restore(mode,7,{0,0,0},0,request).empty()); tick(); const auto cancelled=request;
        flag::leave(); check(!native_flag::restore_pending(cancelled,world,81));
        const auto size=events.size(); native_flag::restore_sent(cancelled,world,81); check(events.size()==size);
    }
    seed("test/duel",7,81);
    check(flag::restore("test/duel",7,{0,0,0},0,request).empty()); tick(); const auto old=request, old_world=world;
    seed("test/duel",7,81); // same MMID and game reused in a new world
    check(flag::restore("test/duel",7,{4,5,6},0,request).empty() && request!=old);
    native_flag::restore_sent(old,old_world,81); check(flag::state().restoring && events.empty());
    native_flag::abandon(); ++world;
    check(flag::adopt("test/duel",7,81,{1,2,3},0,120,4));
    check(!flag::restore("test/duel",7,{0,0,0},0,request).empty());
    seed("test/duel",7,81);
    check(flag::restore("test/duel",7,{4,5,6},0,request).empty()); tick();
    check(native_flag::restore_dispatch(request,world,81)); test_now+=10001; tick();
    check(flag::state().restoring); native_flag::restore_sent(request,world,81);
    check(flag::state().waiting && events.back().action=="flag_restored");
    seed("test/duel",7,81); documents["test/duel"].waiting.reset(); documents["test/duel"].hud.active=true;
    check(!flag::restore("test/duel",7,{0,0,0},0,request).empty());
    check(events_unlocked);
    std::cout << checks << " native flag checks passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
