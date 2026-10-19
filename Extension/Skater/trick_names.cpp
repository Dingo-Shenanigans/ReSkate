#include "trick_names.h"
#include "trick_name_text.h"
#include "trick_identity.h"
#include "trick_logger.h"
#include "skate_detector.h"
#include "no_bail.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/profile.h"
#include "Engine/Game/Build/20260929/trick_names.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <format>

namespace dingosdk::trick_names {
namespace {
using namespace multiplayer::menu_data;
namespace layout = addr::trick_names;
struct Created { Address manager{}; Handle handle{}; std::uint64_t at{}; };
struct Row {
    bool preexisting{}, emitted{};
    std::uint64_t id{}, changed{}, revision{};
    std::string signature;
    Json pending;
    std::uint64_t first_seen{};
};
struct State {
    Address base{}, manager{};
    std::uint64_t generation{}, next_scan{}, next_status{}, serial{}, observations{}, unresolved{}, failures{};
    std::mutex mutex;
    std::atomic<bool> world_ready{};
    std::atomic<std::uint64_t> creation_drops{};
    std::vector<Created> created;
    std::map<Handle, Row> rows;
    NativeTrickCatalog catalog;
    decltype(game::native_data().models.create) original{};
    bool attempted{}, hooked{}, localization{}, capturing{};
    std::string status;
};
State& state() { static auto* s = new State; return *s; }
std::uint32_t schema(Address type) { return read<std::uint32_t>(read<Address>(type)); }
std::uint64_t create_hook(Address manager, Address type, std::uint64_t id,
    std::uint32_t owner, bool option, std::uint8_t realm) {
    auto& s = state();
    const auto result = s.original(manager, type, id, owner, option, realm);
    const auto error = GetLastError();
    if (result && (trick_logger::enabled() || skate::active()) && s.world_ready) {
        try {
            if (schema(type) == layout::row_schema) {
                std::lock_guard lock(s.mutex);
                if (s.created.size() < 256) s.created.push_back({manager, result,GetTickCount64()});
                else ++s.creation_drops;
            }
        } catch (...) {}
    }
    SetLastError(error);
    return result;
}
void install(Address base) {
    auto& s = state();
    if (s.attempted) return;
    s.attempted = true; s.base = base;
    const auto& contract = addr::profile::model_create_contract;
    std::array<unsigned char, 32> actual{};
    s.localization = memory::peek(base + layout::localized_string.rva, actual) && actual == layout::localized_string.bytes;
    if (!memory::peek(base + contract.rva, actual) || actual != contract.bytes) return;
    auto* target = reinterpret_cast<void*>(base + contract.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&create_hook), reinterpret_cast<void**>(&s.original)) != HookOk) return;
    s.hooked = hook_enable(target) == HookOk;
}
std::string localized(Address base, std::string_view raw, bool& complete) {
    const auto lookup = [base](std::string_view key) {
        if (!state().localization) return std::string{};
        std::uint32_t hash = 0xffffffff;
        for (const unsigned char c : key) hash = hash * 33 + c;
        const auto at = reinterpret_cast<Address (*)(std::uint32_t, bool)>(base + layout::localized_string.rva)(hash, false);
        return string(at, 1024);
    };
    return plain_text(resolve_text(raw, lookup, complete));
}
struct Label { std::string raw, text; bool resolved{}; };
Label label(const Context& context, Value row, std::uint32_t field) {
    const auto wrapper = context.field(row, field);
    require(schema(wrapper.type) == layout::label_schema && size(wrapper.type) == 104, "Native trick label schema differs");
    Label result;
    result.raw = context.text(context.field(wrapper, layout::label_text), 1024);
    result.text = localized(context.base, result.raw, result.resolved);
    return result;
}
Json observation(const Context& context, Value model, const Row& row) {
    auto j = Json::object();
    auto raw = Json::object(), readable = Json::object();
    bool complete = true;
    struct Part { const char* name; std::uint32_t hash; };
    constexpr Part parts[]{
        {"base_name",0x53bac52d},{"stance",0x062499e8},{"direction",0xf5cd4270},
        {"body_rotation",0xd813cc83},{"held_rotation",0xcf29a30d},{"late",0x9a2a9749},
        {"nollie",0x6cd6a294},{"body_flip_rotation",0xd7a6e0da},{"body_flip_direction",0x680e60dd},
        {"dark_catch",0xb7d7ac17},{"dark_flip",0xe547253f},{"manual_type",0x2bad0ea8},
        {"underflip",0xa2d7acc5},{"completed_info",0x0205e59c}};
    for (const auto& part : parts) {
        const auto text = label(context, model, part.hash);
        raw[part.name] = text.raw; readable[part.name] = text.text;
        complete = complete && text.resolved;
    }
    j["name"] = readable.at("base_name");
    j["name_resolved"] = complete;
    j["raw_labels"] = raw; j["labels"] = readable;
    j["model_handle"] = model.handle; j["observation_id"] = row.id;
    j["source"] = "native_trick_history";
    const auto parent = context.field(model, 0xb99b6691);
    require(kind(parent.type) == 18 && size(parent.type) == 8, "Native trick parent field differs");
    j["parent_stack_handle"] = read<Handle>(context.address(parent));
    j["preexisting"] = row.preexisting;j["first_seen_ms"]=row.first_seen;
    j["landed"] = nullptr; // A ticker row alone does not establish a landed outcome.
    const auto number = [&](std::uint32_t hash) {
        const auto v = context.field(model, hash);
        require(kind(v.type) == 19 && size(v.type) == 4, "Native trick rotation field differs");
        return read<float>(context.address(v));
    };
    j["body_rotations_raw"] = number(0x6be1143e);
    j["held_rotations_raw"] = number(0x95eda884);
    std::string display;
    for (const auto key : {"stance", "nollie", "direction", "body_rotation", "held_rotation", "late", "base_name", "underflip", "dark_catch", "dark_flip", "body_flip_direction", "body_flip_rotation"}) {
        const auto& text = readable.at(key).string();
        if (!text.empty()) display += (display.empty() ? "" : " ") + text;
    }
    j["display_name"] = display;
    j["comparison_key"] = comparison_key(raw);
    const auto physics=watched_physics_state();
    const bool onboard=physics.valid && ((physics.state>=100 && physics.state<300) || (physics.state>=400 && physics.state<500));
    return state().catalog.annotate(std::move(j),onboard);
}
bool emit(Row& row) {
    if (row.pending.is_null()) return false;
    auto value = row.pending;
    value["kind"] = row.emitted ? "trick_name_update" : "trick_name";
    value["revision"] = row.revision + 1;
    if (!trick_logger::named_trick(std::move(value))) return false;
    ++row.revision; row.emitted = true;
    auto& s = state(); ++s.observations;
    if (!row.pending.at("name_resolved").get<bool>()) ++s.unresolved;
    row.pending = {};
    return true;
}
}
void tick(std::uintptr_t base, bool world_ready) noexcept {
    auto& s = state();
    s.world_ready = world_ready && (trick_logger::enabled() || skate::active());
    if (!(trick_logger::enabled() || skate::active())) {s.capturing=false;return;}
    const bool new_capture=!std::exchange(s.capturing,true);
    if (!world_ready) { trick_logger::name_status("Native ticker: waiting for a playable world."); return; }
    try {
        if (!game::native_data().models.value) return;
        install(base);
        const auto generation = trick_logger::generation();
        const auto ui = read<Address>(base + addr::engine::ui_manager);
        const auto manager = ui ? read<Address>(ui + 0x140) : 0;
        if (!manager) return;
        const auto now = GetTickCount64();
        const bool baseline = new_capture || s.generation != generation || s.manager != manager;
        if (baseline) {
            s.rows.clear(); s.serial = s.observations = s.unresolved = s.failures = 0;
            s.status.clear(); s.creation_drops = 0;
            s.generation = generation; s.manager = manager; s.next_scan = s.next_status = 0;
            s.catalog={};
            for(const auto& entry:skate::metadata) {
                bool complete{};
                const auto text=localized(base,entry.id,complete);
                if(complete)s.catalog.add(entry.id,text);
            }
            logging::log(logging::Level::info,logging::Channel::progression,
                "Native trick identity catalog: {} unique labels, {} ambiguous; localization contract {}.",
                s.catalog.unique_labels(),s.catalog.ambiguous_labels(),s.localization?"verified":"unavailable");
        }
        std::vector<Created> created;
        { std::lock_guard lock(s.mutex); created.swap(s.created); }
        const Context context(base, manager);
        game::ModelWriteLock lock(manager);
        for (const auto& item : created)
            if (item.manager == manager && !s.rows.contains(item.handle) && s.rows.size() < 128)
                {auto row=Row{false,false,++s.serial};row.first_seen=item.at;s.rows.emplace(item.handle,std::move(row));}
        if (now >= s.next_scan) {
            s.next_scan = now + (s.hooked ? 1000 : 200);
            for (const auto& root : context.roots({layout::row_schema}))
                if (root.value && !s.rows.contains(root.model.handle) && s.rows.size() < 128)
                    {auto row=Row{baseline,false,++s.serial};row.first_seen=now;s.rows.emplace(root.model.handle,std::move(row));}
        }
        for (auto it = s.rows.begin(); it != s.rows.end();) {
            auto& row = it->second;
            try {
                const auto type = context.type_of(it->first);
                if (!type || schema(type) != layout::row_schema) { emit(row); it = s.rows.erase(it); continue; }
                auto value = observation(context, {it->first, type}, row);
                if (!value.at("name").string().empty()) {
                    skate::observe(value);
                    // One event per native row, followed by explicit revisions.
                    // Never collapse separate rows merely because names match.
                    const auto signature = value.at("comparison_key").string() + "|" + value.at("display_name").string();
                    if (signature != row.signature) { row.signature = signature; row.pending = std::move(value); row.changed = now; }
                    if (!row.pending.is_null() && now - row.changed >= 50) emit(row);
                }
                ++it;
            } catch (...) { ++s.failures; emit(row); it = s.rows.erase(it); }
        }
        if (now >= s.next_status) {
            s.next_status = now + 1000;
            const auto status = std::format("Native ticker: {} observations, {} unresolved, {} read failures, {} discovery drops; {}.",
                s.observations, s.unresolved, s.failures, s.creation_drops.load(), s.hooked ? "creation hook active" : "polling fallback");
            if (s.status != status) { s.status = status; trick_logger::name_status(status); }
        }
    } catch (...) { trick_logger::name_status("Native ticker unavailable: schema or model manager could not be read."); }
}
}
