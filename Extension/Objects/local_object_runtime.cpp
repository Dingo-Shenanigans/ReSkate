#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/local_object.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Engine/Game/Abi/native_data.h"
#include "local_object_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/Progression/local_entitlement_trigger_runtime.h"
#include "Extension/Objects/object_categories.h"
#include "Extension/Customization/local_customization.h"
#include "Engine/Core/Platform/user_data_paths.h"
#include "bmx_flow_observer.h"
#include <format>

namespace dingosdk::profile_runtime {
// Build-kit category subscription: the browser needs this separately from ownership.

ObjectRuntime& object_runtime() { static auto* r = new ObjectRuntime; return *r; }

namespace {
bool bmx_flow_diagnostics_enabled() noexcept {
    // The launcher validates the requested development root before the game resumes.
    // Read this opt-in once; ordinary launches never collect graph observations.
    static const bool enabled=[] {
        std::array<wchar_t,2> value{};
        return user_data::development_requested()&&
            GetEnvironmentVariableW(L"RESKATE_BMX_FLOW_DIAGNOSTICS",value.data(),static_cast<DWORD>(value.size()))==1&&value[0]==L'1';
    }();
    return enabled;
}
bmx_flow::Observer& bmx_flow_observer() {static auto* observer=new bmx_flow::Observer;return *observer;}
constexpr std::uint32_t compensation_key=0xafde90d2;
constexpr std::array<std::uint32_t,10> compensation_layout{64,552,132,486,4,0,6,327721,720907,67584};
// Each native model read supplies its typed value then a separate resolved flag.
// Include both so an authored false is distinguishable from a missing model.
constexpr std::array<unsigned,16> before_flags{0x59,0x5a,0x5b,0x5c,0x5d,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a};
constexpr std::array<unsigned,42> after_flags{0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,
    0x6b,0x6c,0x6e,0x6f,0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,0x80,0x81,0x82,0x83};
using bmx_flow::startup_contracts;
struct StartupTargetCoverage {
    std::atomic<std::uint64_t> entries{},exits{},layout_rejected{},binding_rejected{},instance_rejected{};
    std::atomic<unsigned> mismatch_capture{};
    bmx_flow::ResourceEvidence mismatch;
    std::uint32_t mismatch_pc{};
    bool mismatch_after{};
};
struct StartupCoverage {
    std::atomic<std::uint64_t> entries{},exits{},inactive{},identity_unreadable{},exceptions{};
    std::array<StartupTargetCoverage,startup_contracts.size()> targets;
};
StartupCoverage& startup_coverage() {static auto* value=new StartupCoverage;return *value;}
void observe_bmx_startup(std::uintptr_t vm,std::uint32_t pc,bool after) noexcept {
    if(!bmx_flow_diagnostics_enabled())return;
    auto& coverage=startup_coverage();
    (after?coverage.exits:coverage.entries).fetch_add(1,std::memory_order_relaxed);
    if(!local_runtime().active.load(std::memory_order_acquire)){coverage.inactive.fetch_add(1,std::memory_order_relaxed);return;}
    try {
        std::uintptr_t resource{},instance{},instance_resource{},locals{};std::uint32_t key{};
        if(!vm||!memory::peek(vm+0x38,resource)||!memory::peek(resource+0x10,key)){
            coverage.identity_unreadable.fetch_add(1,std::memory_order_relaxed);return;
        }
        const auto contract=std::find_if(startup_contracts.begin(),startup_contracts.end(),[&](const auto& value){return value.key==key;});
        if(contract==startup_contracts.end())return;
        auto& target=coverage.targets[static_cast<std::size_t>(contract-startup_contracts.begin())];
        (after?target.exits:target.entries).fetch_add(1,std::memory_order_relaxed);
        const auto match=bmx_flow::match_startup_resource(*contract,resource,
            [](std::uintptr_t address,void* out,std::size_t bytes) noexcept {return memory::peek_bytes(address,out,bytes);},local_runtime().base);
        if(match==bmx_flow::ResourceMatch::layout_rejected){
            target.layout_rejected.fetch_add(1,std::memory_order_relaxed);return;
        }
        if(match==bmx_flow::ResourceMatch::binding_rejected){
            // One immutable, bounded snapshot per allowlisted key. No locals are
            // read on a rejected contract, even if its outer layout matched.
            unsigned empty{};
            if(target.mismatch_capture.compare_exchange_strong(empty,1,std::memory_order_acq_rel)){
                target.mismatch_pc=pc;target.mismatch_after=after;
                target.mismatch=bmx_flow::startup_resource_evidence(key,resource,
                    [](std::uintptr_t address,void* out,std::size_t bytes) noexcept {return memory::peek_bytes(address,out,bytes);});
                target.mismatch_capture.store(2,std::memory_order_release);
            }
            target.binding_rejected.fetch_add(1,std::memory_order_relaxed);return;
        }
        if(!memory::peek(vm+0x30,instance)||!memory::peek(instance,instance_resource)||instance_resource!=resource){
            target.instance_rejected.fetch_add(1,std::memory_order_relaxed);return;
        }
        bmx_flow::RoutineSample sample;sample.vm=vm;sample.instance=instance;sample.entry_pc=pc;
        sample.after=after;sample.at_ms=GetTickCount64();
        // The constructor header starts its fragment atPC0x10. The callback's
        // ordinary PC0 dispatcher also computes its output. Other cursors
        // expose identity only, never a claim about stale local values.
        const bool complete_entry=(key==0x4f83ddd3&&pc==0)||(key==0x129086b2&&(pc==0||pc==0x10));
        if(after&&complete_entry&&memory::peek(instance+contract->layout[0]+16,locals)&&locals>=0x10000&&
            locals<=memory::highest_user_address-contract->layout[2]){
            if(key==0x129086b2)sample.scalar_valid=memory::peek(locals+0x70,sample.scalar);
            else {std::uint8_t success{};if(memory::peek(locals,success)&&success<=1){sample.scalar=success;sample.scalar_valid=true;}}
        }
        if(key==bmx_flow::level_class_condition_key&&after&&pc==0&&
            memory::peek(instance+contract->layout[0]+16,locals))
            sample.scalar_valid=bmx_flow::read_level_class_condition_result(key,pc,after,locals,
                [](std::uintptr_t address,void* out,std::size_t bytes) noexcept {return memory::peek_bytes(address,out,bytes);},sample.scalar);
        if(key==bmx_flow::dependencies_condition_key&&after&&pc==0&&
            memory::peek(instance+contract->layout[0]+16,locals)){
            sample.dependency_factors_valid=bmx_flow::read_dependencies_condition_result(key,pc,after,locals,
                [](std::uintptr_t address,void* out,std::size_t bytes) noexcept {return memory::peek_bytes(address,out,bytes);},
                sample.scalar,sample.dependency_factors);
            sample.scalar_valid=sample.dependency_factors_valid;
        }
        bmx_flow_observer().routine(key,resource,contract->layout,sample);
    }catch(...){coverage.exceptions.fetch_add(1,std::memory_order_relaxed);}
}
void observe_compensation_operands(std::uintptr_t vm,bool after) noexcept {
    if(!bmx_flow_diagnostics_enabled()||!local_runtime().active.load(std::memory_order_acquire))return;
    try {
        std::uintptr_t resource{},instance{},instance_resource{},locals{};std::uint32_t key{},word{};
        std::array<std::uint32_t,10> layout{};std::array<std::uint32_t,2> binding{};
        if(!vm||!memory::peek(vm+0x38,resource)||!memory::peek(resource+0x10,key)||key!=compensation_key||
            !memory::peek(resource+0x20,layout)||layout!=compensation_layout||
            !memory::peek(resource+0xb18,binding)||binding!=std::array<std::uint32_t,2>{0x3b3777a8,0x284}||
            !memory::peek(resource+0x56c,word)||word!=0x29800||
            !memory::peek(vm+0x30,instance)||!memory::peek(instance,instance_resource)||instance_resource!=resource||
            !memory::peek(instance+64+16,locals)||locals<0x10000||locals>memory::highest_user_address-132)return;
        bmx_flow::Operands sample;sample.vm=vm;sample.instance=instance;sample.locals=locals;sample.at_ms=GetTickCount64();
        if(!memory::peek_bytes(locals+0x59,sample.flags.data(),sample.flags.size())||
            !memory::peek_bytes(locals+8,sample.words.data(),sizeof(sample.words)))return;
        if(after){for(unsigned offset:after_flags)if(sample.flags[offset-0x59]>1)return;}
        else {for(unsigned offset:before_flags)if(sample.flags[offset-0x59]>1)return;}
        bmx_flow_observer().operands(key,after,sample);
    }catch(...){}
}
void observe_bmx_flow() noexcept {
    if(!bmx_flow_diagnostics_enabled())return;
    try {
        std::uintptr_t resource{},instance{},instance_resource{};std::uint32_t key{};
        std::array<std::uint32_t,10> layout{};
        if(!executing_expression||!memory::peek(executing_expression+0x38,resource)||
            !memory::peek(resource+0x10,key)||!memory::peek(resource+0x20,layout)){
            bmx_flow_observer().unresolved();return;
        }
        const bool matches=memory::peek(executing_expression+0x30,instance)&&memory::peek(instance,instance_resource)&&instance_resource==resource;
        // These verified resource lanes expose identity/layout, not a name pointer.
        // Keep raw identity for later asset mapping; never guess a graph name.
        bmx_flow_observer().record(key,resource,layout,matches,GetTickCount64());
        if(key==compensation_key&&matches)observe_compensation_operands(executing_expression,false);
    }catch(...){}
}
}

void bmx_flow_expression_started(std::uintptr_t vm,std::uint32_t pc) noexcept {
    PreserveError preserve;
    observe_bmx_startup(vm,pc,false);
}

void bmx_flow_expression_finished(std::uintptr_t vm,std::uint32_t pc) noexcept {
    PreserveError preserve;
    observe_compensation_operands(vm,true);
    observe_bmx_startup(vm,pc,true);
}

std::string bmx_flow_diagnostics() {
    PreserveError preserve;
    const bool enabled=bmx_flow_diagnostics_enabled();
    dingosdk::Json result{{"event","bmx_flow_diagnostics"},{"enabled",enabled},{"graphs",dingosdk::Json::array()}};
    if(enabled){
        const auto snapshot=bmx_flow_observer().snapshot();
        result["calls"]=snapshot.calls;result["unresolved"]=snapshot.unresolved;result["overflow_calls"]=snapshot.overflow;
        result["max_keys"]=bmx_flow::Observer::limit;result["time_basis"]="GetTickCount64 milliseconds";
        const auto operands=[](const bmx_flow::Operands& sample,bool after){
            dingosdk::Json flags=dingosdk::Json::object();
            const auto add=[&](unsigned offset){flags[std::format("L{:02x}",offset)]=sample.flags[offset-0x59];};
            if(after){for(unsigned offset:after_flags)add(offset);}else {for(unsigned offset:before_flags)add(offset);}
            return dingosdk::Json{{"captures",sample.captures},{"at_ms",sample.at_ms},{"vm",sample.vm},{"instance",sample.instance},
                {"locals",sample.locals},{"flags",flags},{"raw_words_08_to_48",sample.words}};
        };
        for(const auto& graph:snapshot.graphs){dingosdk::Json entry{{"key",graph.key},{"count",graph.count},
            {"resource",graph.resource},{"last_resource",graph.last_resource},{"layout",graph.layout},
            {"first_ms",graph.first_ms},{"last_ms",graph.last_ms},{"instance_match_calls",graph.instance_matches},
            {"layout_mismatch_calls",graph.layout_mismatch_calls}};
            if(graph.before_online.captures)entry["before_online"]=operands(graph.before_online,false);
            if(graph.after_expression.captures)entry["after_expression"]=operands(graph.after_expression,true);
            result["graphs"].push_back(std::move(entry));
        }
        result["startup_routines"]=dingosdk::Json::array();
        result["startup_sample_limit"]=bmx_flow::Observer::routine_sample_limit;
        result["startup_logged_first_samples"]=1;
        const auto count=[](const auto& value){return value.load(std::memory_order_relaxed);};
        const auto& coverage=startup_coverage();
        result["startup_coverage"]={{"entries",count(coverage.entries)},{"exits",count(coverage.exits)},
            {"inactive_boundaries",count(coverage.inactive)},{"identity_unreadable",count(coverage.identity_unreadable)},
            {"exceptions",count(coverage.exceptions)}};
        for(const auto& contract:startup_contracts){
            const auto& target=coverage.targets[static_cast<std::size_t>(&contract-startup_contracts.data())];
            dingosdk::Json entry{{"key",contract.key},{"name",contract.name},{"layout",contract.layout},
                {"started",0},{"finished",0},{"samples",dingosdk::Json::array()},
                {"key_seen_entries",count(target.entries)},{"key_seen_exits",count(target.exits)},
                {"layout_rejected",count(target.layout_rejected)},{"binding_rejected",count(target.binding_rejected)},
                {"instance_rejected",count(target.instance_rejected)}};
            const auto captured=target.mismatch_capture.load(std::memory_order_acquire);
            entry["mismatch_capture_state"]=captured;
            if(captured==2){
                const auto& evidence=target.mismatch;
                entry["mismatch_capture_layout_matched"]=evidence.layout_matched;
                if(evidence.layout_matched){
                    dingosdk::Json actual{{"resource",evidence.resource},{"key",evidence.key},
                        {"entry_pc",target.mismatch_pc},{"phase",target.mismatch_after?"exit":"entry"},
                        {"coherent",evidence.coherent},{"first_binding_offset",contract.binding_offset},
                        {"first_binding_readable",evidence.first_readable},{"tail_attempted",evidence.tail_attempted}};
                    if(evidence.first_readable)actual["first_binding"]=evidence.first_binding;
                    if(evidence.tail_attempted){
                        actual["tail_offset"]=0x1a0;actual["tail_readable"]=evidence.tail_readable;
                        actual["final_binding_offset"]=0x1f8;actual["final_binding_readable"]=evidence.final_readable;
                        if(evidence.tail_readable)actual["tail_words"]=evidence.tail;
                        if(evidence.final_readable)actual["final_binding"]=evidence.final_binding;
                    }
                    if(evidence.dependencies_attempted){
                        actual["dependencies_code_offset"]=0x88;
                        actual["dependencies_code_readable"]=evidence.dependencies_code_readable;
                        actual["dependencies_bindings_offset"]=0x1b4;
                        actual["dependencies_bindings_readable"]=evidence.dependencies_bindings_readable;
                        if(evidence.dependencies_code_readable)actual["dependencies_code_words"]=evidence.dependencies_code;
                        if(evidence.dependencies_bindings_readable)actual["dependencies_binding_words"]=evidence.dependencies_bindings;
                    }
                    entry["first_binding_mismatch"]=std::move(actual);
                }
            }
            const auto found=std::find_if(snapshot.routines.begin(),snapshot.routines.end(),[&](const auto& value){return value.key==contract.key;});
            if(found!=snapshot.routines.end()){
                entry["resource"]=found->resource;entry["started"]=found->started;entry["finished"]=found->finished;
                const auto describe_sample=[&](const auto& sample){
                    dingosdk::Json item{{"phase",sample.after?"exit":"entry"},{"entry_pc",sample.entry_pc},
                        {"at_ms",sample.at_ms},{"vm",sample.vm},{"instance",sample.instance}};
                    if(sample.scalar_valid)item[contract.scalar_name]=sample.scalar;
                    if(sample.dependency_factors_valid){
                        const auto& f=sample.dependency_factors;
                        item["dependency_factors"]={{"native_gate_5cc8bee9",f[0]},{"native_gate_ed285e1b",f[1]},
                            {"either_native_gate",f[2]},{"profile_group_0_query",f[3]},
                            {"either_gate_and_profile_query",f[4]},{"resolved_level_class",f[5]},
                            {"all_factors",f[6]},{"condition_out",f[7]}};
                    }
                    return item;
                };
                if(!found->samples.empty())entry["samples"].push_back(describe_sample(found->samples.front()));
                // Retain current evidence when a polling condition exceeds the
                // fixed first32 history. Log only the first sample plus latest
                // exit: dumping the full history exceeds the logger's8KiB line.
                if(found->finished)entry["latest_exit"]=describe_sample(found->latest_exit);
            }
            result["startup_routines"].push_back(std::move(entry));
        }
    }
    return bmx_flow::compact_flow_report(std::move(result));
}

bool local_object_browser_ready() noexcept {
    PreserveError preserve;
    observe_bmx_flow();
    if (!executing_expression || !local_runtime().active.load(std::memory_order_acquire)) return false;
    std::uintptr_t instance{}, resource{}, current{};
    std::uint32_t key{};
    std::array<std::uint32_t, 10> layout{};
    // Asked by every online check the game's flow scripts make: the running graph's key is
    // peeked first, and the rest read only for the object browser's own graph.
    if (!memory::peek(executing_expression + 0x38, current) || !memory::peek(current + 0x10, key) || key != 0x7f3a3f0b ||
        !read(executing_expression + 0x30, instance) || !read(instance, resource) || current != resource ||
        !read(resource + 0x20, layout) || layout != std::array<std::uint32_t, 10>{
            80, 360, 127, 304, 5, 0, 1, 327704, 1441814, 16910336}) return false;
    // This constructor can use the local category subscription already installed.
    // Its callback waits for inventory; startup/backend and other UI graphs retain
    // the offline result. No global connection state is changed.
    dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_browser_ready\"}");
    return true;
}

void initialize_object_functions(std::uintptr_t base) {
    auto& f = object_runtime().functions;
    f.allocate = reinterpret_cast<decltype(f.allocate)>(base + object_allocate_contract.rva);
    f.construct = reinterpret_cast<decltype(f.construct)>(base + object_ctor_contract.rva);
    f.initialize = reinterpret_cast<decltype(f.initialize)>(base + object_register_contract.rva);
    f.parse = reinterpret_cast<decltype(f.parse)>(base + object_parse_contract.rva);
    f.release = reinterpret_cast<decltype(f.release)>(base + object_release_contract.rva);
}

// Read-only category service DTOs consumed synchronously by the native parser.

// Native conversion deep-copies strings and nested arrays into its result class.

bool accept_object_category(const void*, const void*) { return true; }

std::string object_title(const std::string& key, const dingosdk::Json& overrides) {
    const auto metadata = profile::item_display_metadata(key, overrides);
    if (metadata.contains("title") && metadata.at("title").is_string()) return metadata.at("title").string();
    std::string title = key.starts_with("own_") ? key.substr(4) : key;
    std::replace(title.begin(), title.end(), '_', ' ');
    return title;
}

void fill_object_categories(std::uintptr_t result, const profile::Snapshot& snapshot) {
    const auto section = snapshot.extensions.value("object_dropper", dingosdk::Json::object());
    const auto catalog = section.value("catalog", dingosdk::Json::object());
    std::vector<objects::InstalledObject> owned;
    for (const auto& [key, info] : cosmetic_runtime().items)
        if (info.build_kit && installed_item_owned(snapshot, key, info.hash, true))
            owned.push_back({key, info.category, object_title(key, catalog.value(key, dingosdk::Json::object()))});
    // Titles come from the cache's category records. Their icons are cdn:/ links
    // that cannot load offline, and the browser would draw the link as text.
    const auto layout = objects::browser_layout(owned, content_cache::catalogs());
    std::vector<ObjectServiceCategory> categories(layout.size());
    std::vector<std::vector<ObjectServiceSubcategory>> subcategories(layout.size());
    // The DTOs borrow these strings until the native parser has copied them.
    std::vector<std::vector<std::string>> icons(layout.size());
    for (std::size_t index = 0; index < layout.size(); ++index) {
        const auto& group = layout[index];
        auto& parent = categories[index]; auto& children = subcategories[index];
        parent.id.set(group.id); parent.title.set(group.title);
        parent.hash = cosmetic_hash(group.id);
        parent.priority = static_cast<std::uint32_t>(index);
        children.resize(group.tiles.size());
        icons[index].reserve(group.tiles.size());
        for (std::size_t item = 0; item < group.tiles.size(); ++item) {
            auto& child = children[item];
            // Each subcategory produces one selectable tile. Its identity must
            // match OwnableData.presentation.category (local_cosmetic_catalog.cpp).
            const auto& tile = group.tiles[item];
            // "baseitem:<asset key>" resolves to the object's own thumbnail
            // from the game data (the DelMarBaseItemAsset Thumbnail chain).
            icons[index].push_back("baseitem:" + tile.items.front());
            child.id.set(tile.id); child.title.set(tile.title); child.icon.set(icons[index].back());
            child.hash = cosmetic_hash(tile.id);
            child.priority = static_cast<std::uint32_t>(item);
        }
        const auto begin = reinterpret_cast<std::uintptr_t>(children.data());
        const auto end = begin + children.size() * sizeof(ObjectServiceSubcategory);
        parent.subcategories = {begin, end, end};
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(categories.data());
    const std::array<std::uintptr_t, 3> vector{begin, begin + categories.size() * sizeof(ObjectServiceCategory),
        begin + categories.size() * sizeof(ObjectServiceCategory)};
    std::array<std::uintptr_t, 2> shared{reinterpret_cast<std::uintptr_t>(&vector), 0};
    const std::array<std::uintptr_t, 4> predicate{0, 0, 0, reinterpret_cast<std::uintptr_t>(&accept_object_category)};
    // The native converter consumes this additional reference, including on an empty list.
    std::uintptr_t consumed{};
    game::native_data().values.null_reference(&consumed, result & ~std::uintptr_t{4});
    object_runtime().functions.parse(shared.data(), &consumed, predicate.data(), predicate.data());
}

void* object_categories_hook(void* destination, const void* callback, const void* group) {
    auto& s = local_runtime(); auto& o = object_runtime(); auto& f = game::native_data().values;
    if (!s.active.load(std::memory_order_acquire)) return o.functions.subscribe(destination, callback, group);
    {
        PreserveError preserve;
        std::lock_guard lock(s.native_mutex);
        try {
            std::string id;
            if (identifier(group, id) && id == "qdbuildkit" && o.pending.available()) {
                game::NativeDelegateGuard retained;
                f.copy_delegate(&retained.value, callback);
                if (retained.value) {
                    o.pending.push({retained.value}); retained.value = 0;
                    dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_categories_queued\"}");
                }
                return f.null_reference(destination, 0);
            }
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_categories_failed\",\"operation\":\"subscribe\"}"); }
    }
    return o.functions.subscribe(destination, callback, group);
}

void update_object_categories() {
    auto& o = object_runtime();
    if (o.pending.empty() || !cosmetic_runtime().published_inventory) return;
    std::uintptr_t allocator{};
    if (!read(local_runtime().base + addr::engine::ui_allocator, allocator) || !allocator) return;
    o.pending.deliver([&](const ObjectRuntime::Pending&, std::uintptr_t callback) {
        try {
            auto* raw = o.functions.allocate(0x20, 8, allocator, 0, 1);
            if (!raw) throw std::bad_alloc();
            o.functions.construct(raw);
            o.functions.initialize(raw, local_runtime().base + addr::local_object::category_result_type, 0, 0, 1);
            ObjectReferenceGuard result;
            game::native_data().values.null_reference(&result.value, reinterpret_cast<std::uintptr_t>(raw));
            fill_object_categories(result.value, (*local_runtime().store->shared_snapshot()));
            game::native_data().values.invoke(&callback, &result.value);
            dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_categories_published\"}");
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::objects, "{\"event\":\"local_object_categories_failed\",\"operation\":\"callback\"}"); }
    });
}
}
