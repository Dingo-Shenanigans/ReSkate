// The plugin host apart from the game: manifests, the reskate API, storage, modules, hot reload,
// and the sandbox against plugins written to break it (loops, memory, error floods, escapes).
#include "Extension/Plugins/plugin_approval.h"
#include "Extension/Plugins/plugin_content.h"
#include "Extension/Plugins/plugin_discovery.h"
#include "Extension/Plugins/plugin_host.h"
#include "Extension/Plugins/plugin_listing.h"
#include "Extension/Objects/placement_geometry.h"
#include "Extension/Plugins/plugin_manifest.h"
#include "Extension/Plugins/plugin_server_scope.h"
#include "Extension/Plugins/plugin_session_feed.h"
#include "Extension/Plugins/plugin_wire.h"
#include "Extension/Throwdowns/challenge_registry.h"
#include "Extension/Throwdowns/custom_modes.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include "Engine/Vfs/mod_list.h"
#include "Engine/Game/Input/pad_output.h"
#include "Engine/Game/Input/playstation_report.h"
#include "Engine/Game/Input/gameplay_options.h"
#ifdef _WIN32 // reads memory as the game does (Engine/Core/Platform/memory.h)
#include "Engine/Game/Skater/animation_values.h"
#include "Extension/Plugins/plugin_font.h" // Windows reads a plugin's fonts
#endif
#include "Extension/UI/Overlay/glyph_pages.h"
#include "Extension/Customization/board_material.h"
#include "Extension/Localization/text_overrides.h"
#include "miniz.h" // the tests pack the zips they hand the unpacker
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace dingosdk::plugins;
namespace plugin_ui = dingosdk::plugin_ui;
namespace board_material = dingosdk::board_material;
namespace mods = dingosdk::mods;
namespace owned_body = dingosdk::owned_body;
namespace body_query = dingosdk::body_query;
namespace traffic_vehicles = dingosdk::traffic_vehicles;
namespace {
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }

std::filesystem::path scratch;


// The wider set's game, faked: it records what plugins asked of it and answers as told.
struct FakeGame final : Game {
    std::map<std::string, ui_values::Values> ui_models;
    ui_values::Reservations ui_reservations;
    std::string ui_error;
    std::string publish_ui_values(std::string_view plugin, const ui_values::Values& values) override {
        if (!ui_error.empty()) return ui_error;
        if (const auto error = ui_reservations.reserve(plugin, values); !error.empty()) return error;
        if (values.empty()) return {};
        auto& models = ui_models[std::string(plugin)];
        for (const auto& [name, value] : values) models.insert_or_assign(name, value);
        return {};
    }

    // Native bodies a plugin owns, as the fake keeps them: one sample per body, advanced by
    // body_producer_step, which also produces a pending command the way the game's synchronizer would.
    struct OwnedBody {
        std::uint64_t owner{};
        owned_body::Handle handle{};
        owned_body::Definition definition{};
        owned_body::Sample sample{};
        owned_body::Sample latest{};
        owned_body::Mailbox mailbox{};
        bool released{}, quarantined{};
    };
    std::map<std::uint64_t, OwnedBody> owned;
    std::vector<std::uint64_t> body_releases;
    unsigned body_creates{}, body_submissions{}, body_release_calls{};
    unsigned body_submit_entries{};
    unsigned body_prepares{};
    std::string body_prepare_error;
    std::string body_prepare() override { ++body_prepares; return body_prepare_error; }
    std::string test_body_create_refusal;
    unsigned body_create_calls{}, body_read_calls{};
    bool test_body_read_unavailable{};
    bool batch_ground_plane{};
    std::uint64_t body_command_sequence{};
    unsigned batch_calls{}, batch_rays{};
    bool batch_available{true}, batch_clear{}, batch_change_after{}, batch_bad_count{};
    unsigned batch_drift_field{};
    unsigned batch_failure_ray{std::numeric_limits<unsigned>::max()};
    body_query::Failure batch_refusal{body_query::Failure::none};
    unsigned batch_refusal_casts{};
    bool batch_refusal_accounting_valid{true};
    body_query::Request last_batch;
    std::string body_create(std::uint64_t owner, const owned_body::Definition& definition, owned_body::Handle& handle) override {
        ++body_create_calls;
        if (!owned_body::valid(definition) || owned.size() >= owned_body::retained_capacity) return "definition or capacity";
        if (!test_body_create_refusal.empty()) return test_body_create_refusal;
        ++body_creates;
        handle = {body_creates, body_creates - 1};
        auto& b = owned[handle.generation];
        b.owner = owner;
        b.handle = handle;
        b.definition = definition;
        b.sample.epoch = {77, handle.generation, 100, 3, static_cast<float>(owned_body::native_seconds)};
        b.sample.position = definition.position;
        b.sample.rotation = definition.rotation;
        b.sample.mass = definition.mass;
        b.sample.inertia = definition.inertia;
        return {};
    }
    // create with options, as the fake answers it: it records who asked and whether reuse was
    // asked for, then makes a body and returns the scripted origin and miss. It never chooses
    // a parked body itself; that rule is the service's, tested in owned_body_source_tests.cpp.
    std::vector<std::pair<std::uint64_t, bool>> body_reuse_requests;
    owned_body::CreateReport test_reuse_report{owned_body::Origin::reused, owned_body::ReuseMiss::none};
    std::string test_reuse_refusal;
    std::string body_create_reusing(std::uint64_t owner, const owned_body::Definition& definition, owned_body::Handle& handle, bool reuse,
                                    owned_body::CreateReport& report) override {
        body_reuse_requests.emplace_back(owner, reuse);
        if (!test_reuse_refusal.empty()) return test_reuse_refusal;
        const auto why = body_create(owner, definition, handle);
        if (why.empty()) report = reuse ? test_reuse_report : owned_body::CreateReport{};
        return why;
    }
    OwnedBody* body_of(std::uint64_t owner, const owned_body::Handle& h) {
        const auto at = owned.find(h.generation);
        return at != owned.end() && at->second.owner == owner && at->second.handle == h ? &at->second : nullptr;
    }
    std::string body_read(std::uint64_t owner, const owned_body::Handle& h, owned_body::Sample& sample) override {
        ++body_read_calls;
        if (test_body_read_unavailable) return "constructed sample unavailable";
        auto* b = body_of(owner, h);
        if (!b || b->released || b->quarantined) return "owner or generation";
        b->latest = b->sample;
        (void)b->mailbox.observe_successor(b->sample);
        sample = b->sample;
        return {};
    }
    std::string body_submit(std::uint64_t owner, const owned_body::Handle& h, const owned_body::Command& command, std::uint64_t& sequence) override {
        ++body_submit_entries; // Count entry even when the backend rejects a zero/invalid packet.
        sequence = 0;
        auto* b = body_of(owner, h);
        if (!b || b->released || body_command_sequence == std::numeric_limits<std::uint64_t>::max()) return "owner, generation or packet";
        // A command the inbox refuses is answered in the service's own words (submit in
        // Extension/World/owned_bodies.cpp), so a host test can show a plugin is handed them as they are.
        if (!b->mailbox.submit(command, b->sample.epoch, 0, body_command_sequence + 1))
            return "body command invalid, repeated, stopped, pending or receipt ring full";
        sequence = ++body_command_sequence;
        ++body_submissions;
        return {};
    }
    int batch_contact_mode{};
    unsigned batch_available_corruptions{};
    // Constructed category sidecars, copied in after execute like the native adapter:
    // 1 into opted-in hits, 2 into hits even without the opt-in, 3 onto clear rows of
    // an available result, 4 onto a refused result's first row.
    int batch_category_mode{};
    std::array<std::uint32_t, body_query::maximum_rays> batch_categories{};
    std::array<bool, body_query::maximum_rays> batch_category_missing{};
    // Constructed owner slots, the same way: 1 into opted-in hits, 2 into hits even
    // without the opt-in, 3 onto clear rows of an available result.
    int batch_owner_mode{};
    std::array<std::uint32_t, body_query::maximum_rays> batch_owners{};
    std::array<bool, body_query::maximum_rays> batch_owner_missing{};
    // Constructed backend facts, put on every result whether or not it was asked for.
    bool batch_claims_local_actor{}, batch_claims_named_categories{};
    body_query::Result body_probe_batch(std::uint64_t owner, const owned_body::Handle& h, const body_query::Request& request, unsigned allowed) override {
        ++batch_calls;
        last_batch = request;
        if (batch_refusal != body_query::Failure::none) {
            body_query::Result out;
            out.count = request.count;
            out.failure = batch_refusal;
            out.native_casts = batch_refusal_casts;
            out.accounting_valid = batch_refusal_accounting_valid;
            out.local_actor_excluded = batch_claims_local_actor;
            out.named_actor_categories_absent = batch_claims_named_categories;
            if (batch_category_mode == 4) { out.rays[0].status = body_query::Status::hit; out.rays[0].in_categories = 9; out.rays[0].in_categories_available = true; }
            native_casts += out.native_casts;
            return out;
        }
        auto* selected = body_of(owner, h);
        unsigned fences{}, rays{};
        const auto fence = [&] {
            if (!selected || selected->released || selected->quarantined || !body_query::same_sample(selected->sample, request.observation) ||
                !body_query::same_sample(selected->latest, request.observation) ||
                (++fences > 1 && batch_change_after)) return false;
            for (std::size_t i = 0; i < request.exclusions; ++i) {
                const auto* other = body_of(owner, request.excluded[i]);
                if (!other || other->released || other->quarantined || other->sample.epoch.world != request.observation.epoch.world) return false;
            }
            return true;
        };
        auto result = body_query::execute(request, allowed, 1000, [&](const body_query::Ray& ray, unsigned permitted) {
            ++batch_rays;
            const auto index = rays++;
            if (!index && batch_drift_field) {
                switch (batch_drift_field) {
                case 1: selected->sample.position[0] += .001f; break;
                case 2: selected->sample.velocity[1] += .001f; break;
                case 3: selected->sample.angular_velocity[2] += .001f; break;
                case 4: selected->sample.rotation[0] += .001f; break;
                }
            }
            const auto cast_count = std::min(permitted, query_cast_cost);
            native_casts += cast_count;
            body_query::Probe out;
            out.native_casts = batch_bad_count ? permitted + 1 : cast_count;
            out.budget_exhausted = cast_count < query_cast_cost;
            if (!batch_available || !cast_count || out.budget_exhausted || index == batch_failure_ray) return out;
            out.hit.status = batch_clear ? body_query::Status::clear : body_query::Status::hit;
            for (std::size_t i = 0; i < ray.from.size(); ++i) out.hit.position[i] = (ray.from[i] + ray.to[i]) * .5f;
            out.hit.fraction = .5f;
            if (batch_ground_plane && !batch_clear) {
                // Constructed fixture geometry only; the shared query fences still apply.
                const float dy = ray.to[1] - ray.from[1];
                const float fraction = dy != 0 ? -ray.from[1] / dy : std::numeric_limits<float>::quiet_NaN();
                if (!std::isfinite(dy) || !std::isfinite(fraction) || fraction < 0 || fraction > 1) {
                    out.hit = {};
                    out.hit.status = body_query::Status::clear;
                    return out; // Preserve this attempted cast's actual accounting.
                }
                out.hit.fraction = fraction;
                for (std::size_t i = 0; i < ray.from.size(); ++i)
                    out.hit.position[i] = ray.from[i] + (ray.to[i] - ray.from[i]) * fraction;
            }
            if (batch_contact_mode && !batch_clear) {
                out.hit.normal_available = out.hit.surface_velocity_available = true;
                out.hit.normal = {0, 1, 0};
                out.hit.surface_velocity = {2, 0, -1};
                if (batch_contact_mode == 2) out.hit.normal[1] = std::numeric_limits<float>::quiet_NaN();
                if (batch_contact_mode == 3) out.hit.normal[1] = -1;
            }
            return out;
        }, fence);
        // Deliberately malformed AVAILABLE results exercise the binding's own
        // independent result guard, after the shared query core has accepted.
        if (result.available && batch_contact_mode >= 4) {
            ++batch_available_corruptions;
            if (batch_contact_mode == 4) result.rays[0].normal[1] = std::numeric_limits<float>::quiet_NaN();
            if (batch_contact_mode == 5) result.rays[0].normal[1] = -1;
            if (batch_contact_mode == 6) result.rays[0].surface_velocity_available = false;
        }
        for (std::size_t i = 0; result.available && batch_category_mode >= 1 && batch_category_mode <= 3 && i < result.count; ++i) {
            auto& hit = result.rays[i];
            const bool copied = batch_category_mode == 3 ? hit.status == body_query::Status::clear :
                hit.status == body_query::Status::hit && !batch_category_missing[i] &&
                (request.include_shape_categories || batch_category_mode == 2);
            if (copied) { hit.in_categories = batch_categories[i]; hit.in_categories_available = true; }
        }
        for (std::size_t i = 0; result.available && batch_owner_mode >= 1 && batch_owner_mode <= 3 && i < result.count; ++i) {
            auto& hit = result.rays[i];
            const bool copied = batch_owner_mode == 3 ? hit.status == body_query::Status::clear :
                hit.status == body_query::Status::hit && !batch_owner_missing[i] && (request.include_hit_owner || batch_owner_mode == 2);
            if (copied) { hit.owner = batch_owners[i]; hit.owner_available = true; hit.owner_assigned = batch_owners[i] != 0xffffffffU; }
        }
        if (result.available) {
            result.local_actor_excluded = batch_claims_local_actor;
            result.named_actor_categories_absent = batch_claims_named_categories;
        }
        return result;
    }
    std::string body_receipt(std::uint64_t owner, const owned_body::Handle& h, std::uint64_t selected, owned_body::Receipt& receipt) override {
        auto* b = body_of(owner, h);
        if (!b) return "owner or generation";
        if (!b->released && !b->quarantined && b->mailbox.needs_successor()) {
            b->latest = b->sample;
            (void)b->mailbox.observe_successor(b->sample);
        }
        if (!b->mailbox.find(selected, receipt)) return "receipt sequence unavailable";
        receipt.retained = b->released;
        receipt.quarantined = b->quarantined;
        return {};
    }
    std::string body_acknowledge(std::uint64_t owner, const owned_body::Handle& h, std::uint64_t sequence) override {
        auto* b = body_of(owner, h);
        return b && b->mailbox.acknowledge(sequence) ? std::string{} : "not ready to acknowledge";
    }
    std::string body_release(std::uint64_t owner, const owned_body::Handle& h) override {
        ++body_release_calls;
        auto* b = body_of(owner, h);
        if (!b) return "owner or generation";
        b->released = true;
        b->mailbox.cancel();
        return {};
    }
    void body_release_all(std::uint64_t owner) override {
        body_releases.push_back(owner);
        for (auto& [id, b] : owned) { (void)id; if (b.owner == owner) { b.released = true; b.mailbox.cancel(); } }
    }
    // The clock edge alone: the physics step that publishes every body's next sample.
    void body_clock_step() {
        for (auto& [id, b] : owned) {
            (void)id;
            if (b.released || b.quarantined) continue;
            ++b.sample.epoch.serial;
            b.sample.epoch.time += b.sample.epoch.seconds;
        }
    }
    // One synchronizer call on a listed wrapper: it takes a body's pending command only when the
    // clock reads the step after that command's epoch. Returns how many commands it took.
    unsigned body_synchronizer_call(bool complete = true) {
        unsigned taken{};
        for (auto& [id, b] : owned) {
            (void)id;
            if (b.released || b.quarantined) continue;
            owned_body::Command out;
            if (b.mailbox.consume(b.sample.epoch, 1, out)) {
                ++taken;
                (void)b.mailbox.finish(true, complete, complete);
                if (!complete) b.quarantined = true;
            }
        }
        return taken;
    }
    // The full-rate order: the edge, then the synchronizer, and the plugin's tick after both.
    // A test of the half-rate order ticks the host between body_clock_step and body_synchronizer_call.
    void body_producer_step(bool complete = true) {
        body_clock_step();
        (void)body_synchronizer_call(complete);
    }
    struct Effect { std::string owner; EffectRequest request; bool stopped{}; float alpha{1}; };
    std::map<std::uint64_t, Effect> effects;
    std::uint64_t effect_serial{}, effect_epoch{1};
    bool effects_ready{true};
    std::vector<std::pair<std::string, std::string>> effect_bundles;   // bundle, asset: each one asked for
    EffectBundleState effect_bundle(std::string_view, std::string_view bundle, std::string_view asset) override {
        effect_bundles.emplace_back(std::string(bundle), std::string(asset));
        return effect_bundles.size() == 1 ? EffectBundleState::loading : EffectBundleState::ready;
    }
    std::uint64_t effect_create(std::string_view plugin, const EffectRequest& request) override {
        if (!effects_ready) return 0;
        const auto id = ++effect_serial;
        effects.emplace(id, Effect{std::string(plugin), request});
        return id;
    }
    void effect_move(std::string_view plugin, std::uint64_t id, const EffectMatrix& matrix) override {
        if (auto it = effects.find(id); it != effects.end() && it->second.owner == plugin) it->second.request.transform = matrix;
    }
    void effect_stop(std::string_view plugin, std::uint64_t id) override {
        if (auto it = effects.find(id); it != effects.end() && it->second.owner == plugin) it->second.stopped = true;
    }
    void effect_opacity(std::string_view plugin, std::uint64_t id, float value) override {
        if (auto it = effects.find(id); it != effects.end() && it->second.owner == plugin) it->second.alpha = value;
    }
    void effect_release(std::string_view plugin, std::uint64_t id) override {
        if (auto it = effects.find(id); it != effects.end() && it->second.owner == plugin) effects.erase(it);
    }
    std::vector<std::uint64_t> effects_live(std::string_view plugin) override {
        std::vector<std::uint64_t> result;
        for (const auto& [id, e] : effects) if (e.owner == plugin) result.push_back(id);
        return result;
    }
    void effects_clear(std::string_view plugin) override { std::erase_if(effects, [&](const auto& e) { return e.second.owner == plugin; }); }
    std::pair<bool, std::uint64_t> effects_state() override { return {effects_ready, effect_epoch}; }
    ObjectPhysicsClaims physics_claims;
    std::map<std::uint64_t, ObjectPhysics> object_physics_now;
    std::map<std::uint64_t, ObjectMotion> object_motion_now;
    std::map<std::uint64_t, std::string> physics_owners;
    std::map<std::string, MarkerEvents> marker_feed;
    std::optional<SkaterMovement> movement_now;
    std::optional<ObjectContact> contact_now;
    std::optional<SkaterMovement> skater_movement() override { return movement_now; }
    std::optional<ObjectContact> object_contact(std::string_view plugin, const Vec3&, const Vec3&) override {
        if (contact_now && physics_claims.owns(plugin, contact_now->id)) return contact_now;
        return std::nullopt;
    }
    std::string object_physics(std::string_view plugin, std::uint64_t id, const std::optional<ObjectPhysics>& value) override {
        if (!value) {
            if (!physics_claims.owns(plugin, id)) return "not the owner";
            physics_claims.erase(id); object_physics_now.erase(id); object_motion_now.erase(id); physics_owners.erase(id); return {};
        }
        if (id != 42) return "not a live local placement";
        if (!valid_object_physics(*value)) return "invalid mass";
        if (auto why = physics_claims.claim(plugin, id); !why.empty()) return why;
        object_physics_now[id] = *value; physics_owners[id] = plugin;
        if (!object_motion_now.contains(id)) object_motion_now[id] = {{1, 0, 2}, {}, {0, 0, 0, 1}, value->dynamic};
        object_motion_now[id].dynamic = value->dynamic;
        object_motion_now[id].mass = value->mass;
        object_motion_now[id].surface = value->surface;
        object_motion_now[id].authored_mass = 1;
        return {};
    }
    std::string object_velocity(std::string_view plugin, std::uint64_t id, const Vec3& value) override {
        if (!physics_claims.owns(plugin, id)) return "not the owner";
        object_motion_now[id].velocity = value; return {};
    }
    std::string object_pose(std::string_view plugin, std::uint64_t id, const Vec3& at, const std::array<float, 4>& q) override {
        if (!physics_claims.owns(plugin, id)) return "not the owner";
        object_motion_now[id].position = at; object_motion_now[id].rotation = q; return {};
    }
    std::optional<ObjectMotion> object_motion(std::string_view plugin, std::uint64_t id) override {
        if (!physics_claims.owns(plugin, id)) return std::nullopt;
        return object_motion_now.at(id);
    }
    std::optional<MarkerEvents> marker_events(std::string_view plugin) override {
        return std::exchange(marker_feed[std::string(plugin)], {});
    }
    std::string tod_now = "afternoon";
    std::string time_of_day_now() override { return tod_now; }
    std::string world_controls_text() override { return "fog=1\nfog_distance=0.5\n"; }
    std::vector<std::pair<std::string, std::string>> atmosphere_set;
    std::string set_atmosphere(const std::vector<std::pair<std::string, std::string>>& values) override {
        atmosphere_set.insert(atmosphere_set.end(), values.begin(), values.end());
        return {};
    }
    std::optional<std::uint32_t> surface_set;
    std::uint32_t surface_fields{};
    std::string skater_surface_sound(std::string_view, std::optional<std::uint32_t> word, std::uint32_t fields) override {
        surface_set = word;
        surface_fields = fields;
        return {};
    }
    // Each trainer preset held (its name, and its values) or let go (nullopt).
    std::vector<std::optional<std::pair<std::string, std::map<std::string, double, std::less<>>>>> trainer_presets;
    std::string trainer_preset(std::string_view, std::string_view name, std::optional<std::map<std::string, double, std::less<>>> values) override {
        if (values) trainer_presets.emplace_back(std::pair(std::string(name), std::move(*values)));
        else trainer_presets.emplace_back(std::nullopt);
        return {};
    }
    std::vector<std::pair<std::string, bool>> chosen_variants; // each choice, and whether it went in now
    std::string choose_variant(std::string_view, const std::filesystem::path&, std::string_view variant, bool now) override {
        chosen_variants.emplace_back(std::string(variant), now);
        return {};
    }
    std::optional<std::uint32_t> state;
    std::uint64_t wipe{};
    std::vector<std::string> calls;
    std::vector<ChatLine> lines;
    std::vector<ChatCommandInfo> chat_command_list; // as the plugins last gave it
    std::vector<ChatCommandCall> typed_commands;    // waiting for the next tick
    Track now_playing;
    ThrowdownState throwdown;
    std::vector<std::pair<std::string, std::string>> presses;
    std::map<std::string, std::uint32_t> binds;
    std::vector<std::string> released;
    std::optional<std::uint32_t> physics_state() override { return state; }
    std::uint64_t wipeouts() override { return wipe; }
    std::string outfit_set(std::string_view plugin, std::string_view slot, std::string_view item, std::vector<float> placement, bool covers) override {
        calls.push_back(std::format("outfit {} {} {} {} {}", plugin, slot, item, placement.size(), covers));
        return {};
    }
    void outfit_clear(std::string_view plugin, std::string_view slot) override { calls.push_back(std::format("outfit clear {} {}", plugin, slot)); }
    std::string outfit_stand_ins(std::string_view plugin, std::optional<StandIns> wanted) override {
        if (!wanted) {
            calls.push_back(std::format("stand-ins {} default", plugin));
            return {};
        }
        calls.push_back(std::format("stand-ins {} {} {} {} {}", plugin, wanted->random ? "random" : "first", wanted->parts,
                                    wanted->seed, wanted->items.empty() ? std::string("all") : wanted->items.front()));
        for (const auto& rule : wanted->exclude) calls.push_back(std::format("stand-ins {} exclude {}", plugin, rule));
        return {};
    }
    std::string camera_cycle(std::string_view plugin, CycleCamera place) override {
        calls.push_back(std::format("cycle {} {}", plugin, place == CycleCamera::out ? "out" : place == CycleCamera::ready ? "ready" : "active"));
        return {};
    }
    std::vector<CycleNews> cycle_news;
    std::vector<CycleNews> take_camera_cycle() override { return std::exchange(cycle_news, {}); }
    void hide_nametags(std::string_view plugin, bool hidden) override { calls.push_back(std::format("nametags {} {}", plugin, hidden ? "off" : "on")); }
    std::set<std::string, std::less<>> installed; // items found by their whole ending, as Pack/<ending>
    std::string outfit_find(std::string_view ending) override {
        if (installed.contains(ending)) return "Pack/" + std::string(ending);
        return ending == "roadrash_leg1" ? "RoadRash_Leg1" : "";
    }
    std::vector<OutfitSlot> outfit_slots() override { return {{"tattoo_legl", "", {0.5f, 1}}, {"moles", "moles_empty", {}}}; }
    std::optional<std::array<BoardBone, 16>> board_bones() override {
        std::array<BoardBone, 16> bones{};
        bones[3].position = {0, 0.1f, 0};
        return bones;
    }
    std::string board_set(std::string_view plugin, int bone, const BoardBone& value, bool world) override {
        calls.push_back(std::format("board {} {} {:.1f} {}", plugin, bone, value.position[1], world ? "world" : "local"));
        return {};
    }
    std::string board_mount(std::string_view plugin, std::optional<BoardMount> mount) override {
        calls.push_back(mount ? std::format("board mount {} {:.2f},{:.2f},{:.2f} r{:.2f},{:.2f},{:.2f},{:.2f} {} {:.2f}", plugin, mount->position[0],
                                            mount->position[1], mount->position[2], mount->rotation[0], mount->rotation[1], mount->rotation[2],
                                            mount->rotation[3], mount->with_skater ? "skater" : "world", mount->weight)
                              : std::format("board mount {} none", plugin));
        if (mount)
            for (const auto& bone : mount->bones)
                calls.push_back(std::format("board mount bone {} {}{}", bone.bone,
                                            bone.position ? std::format("at {:.2f},{:.2f},{:.2f}", (*bone.position)[0], (*bone.position)[1], (*bone.position)[2]) : std::string("held"),
                                            bone.rotation ? std::format(" r{:.2f},{:.2f},{:.2f},{:.2f}", (*bone.rotation)[0], (*bone.rotation)[1], (*bone.rotation)[2], (*bone.rotation)[3]) : std::string()));
        return {};
    }
    std::string board_hide(std::string_view plugin, int bone, bool everyone) override {
        calls.push_back(std::format("board hide {} {}{}", plugin, bone, everyone ? " everyone" : ""));
        return {};
    }
    void board_park(std::string_view plugin, std::vector<int> bones) override { calls.push_back(std::format("board park {} {}", plugin, bones.size())); }
    std::string board_scale(std::string_view plugin, int bone, std::optional<Vec3> size) override {
        if (!size) calls.push_back(std::format("board scale {} {} none", plugin, bone));
        else calls.push_back(std::format("board scale {} {} {:.1f} {:.1f} {:.1f}", plugin, bone, (*size)[0], (*size)[1], (*size)[2]));
        return {};
    }
    // The Example declared-input deck as the game holds it: seven live selectors, and the eight colours (and a
    // number) its mod declares for its shader. What plugins hold of it is kept by the same rules as
    // the game's (board_material.h: take, listed, release). Off: no deck declares anything.
    bool declared_rules{}; // the deck answers as the Example deck (else as before: everything is taken)
    bool declared_deck{};  // with declared_rules: the Example deck is equipped (else a deck that declares nothing)
    std::vector<board_material::detail::Hold> declared_holds;
    static constexpr std::array<std::uint64_t, 8> declared_keys{
        0x5b2d035c7746f31aULL, 0x51745c0fd2d117d3ULL, 0x5c1058b72fe158edULL, 0x5a4e08ac201c369fULL,
        0x57fc15cf6583c0c7ULL, 0x5a2f52ddbd12f283ULL, 0x55fda1e1521b9097ULL, 0x580bde15aa576f12ULL};
    static constexpr std::uint64_t declared_number = 0x7777777777777777ULL;
    board_material::detail::Found declared_board() const {
        using namespace board_material;
        detail::Found found;
        detail::Material deck{0x100, 0x200, Part::deck, {}};
        for (std::uint64_t k = 1; k <= 7; ++k)
            deck.nodes.push_back({0x300 + k, {0x5000 + k, k % 2 ? Kind::integer : Kind::toggle, {}, static_cast<std::int32_t>(k)}, 0x1234});
        found.materials.push_back(deck);
        mods::MaterialDeclaration declaration{"Example", "deck", {"Example_Deck"}, {}};
        for (const auto key : declared_keys) declaration.parameters.push_back({key, static_cast<std::uint16_t>(key), true, {.5f, .5f, .5f}});
        declaration.parameters.push_back({declared_number, 0x4242, false, {.25f, 0, 0}});
        auto never_sold = [](std::string_view) { return false; };
        found.declared = detail::declared_for({declaration}, Part::deck, declared_deck ? "Example_Deck" : "Plain_Deck", never_sold, "Example");
        return found;
    }
    bool declared_holds_deck(int part) const { return declared_rules && part == 0; }
    std::optional<std::vector<BoardParameter>> board_parameters(int part, bool preview_scope = false) override {
        if (preview_scope) calls.push_back("preview parameters");
        if (declared_holds_deck(part)) {
            std::vector<BoardParameter> out;
            for (const auto& p : board_material::detail::listed(declared_board(), board_material::Part::deck)) {
                using board_material::Kind;
                out.push_back({p.key, p.kind == Kind::colour ? "colour" : p.kind == Kind::number ? "number" : p.kind == Kind::integer ? "integer" : "switch",
                               p.value, p.integer, p.inherited});
            }
            return out;
        }
        if (part != 1) return std::vector<BoardParameter>{};
        return std::vector<BoardParameter>{{0x873f48af36e50d59ULL, "colour", {.2f, .3f, .4f}, 0},
                                           {0x841c7d74837f1d39ULL, "number", {1, 0, 0}, 0},
                                           {0xb00abec403499469ULL, "integer", {}, 13},
                                           {0xa5f9a6b8098279adULL, "switch", {}, 1}};
    }
    std::string board_set_parameter(std::string_view plugin, int part, std::uint64_t key, std::array<float, 3> value, bool colour, bool preview_scope = false) override {
        if (preview_scope) calls.push_back("preview set");
        if (declared_holds_deck(part)) {
            auto why = board_material::detail::take(declared_holds, declared_board(), plugin, board_material::Part::deck, key, value, colour);
            if (why.empty()) calls.push_back(std::format("board parameter {} {} {:016x} {} {:.1f}", plugin, part, key, colour ? "colour" : "number", value[0]));
            return why;
        }
        calls.push_back(std::format("board parameter {} {} {:016x} {} {:.1f}", plugin, part, key, colour ? "colour" : "number", value[0]));
        return {};
    }
    void board_clear_parameter(std::string_view plugin, int part, std::optional<std::uint64_t> key, bool preview_scope = false) override {
        if (preview_scope) calls.push_back("preview clear");
        calls.push_back(std::format("board parameter clear {} {} {}", plugin, part, key ? std::format("{:016x}", *key) : "all"));
        board_material::detail::release(declared_holds, plugin, part < 0 ? std::nullopt : std::optional(static_cast<board_material::Part>(part)), key);
    }
    std::size_t declared_held(std::string_view plugin) const {
        return static_cast<std::size_t>(std::count_if(declared_holds.begin(), declared_holds.end(), [&](const auto& h) { return h.owner == plugin; }));
    }
    // The general services a game layer in Lua reads and drives (plugin_api_services.cpp).
    std::uint64_t world_id() override { return 77; }
    std::string world_map() override { return "test_map"; }
    std::string pad_flight(std::string_view plugin, bool on) override {
        calls.push_back(std::format("pad flight {} {}", plugin, on));
        return {};
    }
    void competition(std::string_view plugin, bool on) override { calls.push_back(std::format("competition {} {}", plugin, on)); }
    void spectate(std::uint64_t player) override { calls.push_back(std::format("spectate {}", player)); }
    bool stock_physics_ready() override { return true; }
    void prepare_physics() override { calls.push_back("prepare physics"); }
    void trick_feed(std::string_view plugin, bool on) override { calls.push_back(std::format("trick feed {} {}", plugin, on)); }
    std::vector<std::array<float, 2>> trick_gesture(std::string_view trick) override {
        if (trick == "Kickflip") return {{0, 0}, {0.5f, 1}};
        return {};
    }
    std::string bone_cam(std::string_view plugin, const std::vector<std::string>& words) override {
        std::string line = "bone cam " + std::string(plugin);
        for (const auto& w : words) line += " " + w;
        calls.push_back(line);
        if (words.empty()) return "game";
        return words.front() == "ringing" ? "1" : "";
    }
    std::vector<ContentFile> content_files;
    std::map<std::string, std::string> content_texts;
    std::string content_list(std::string_view folder, std::vector<ContentFile>& out) override {
        calls.push_back(std::format("content list {}", folder));
        out = content_files;
        return {};
    }
    std::string content_read(std::string_view mod, std::string_view folder, std::string_view path, std::string& text) override {
        const auto found = content_texts.find(std::format("{}/{}/{}", mod, folder, path));
        if (found == content_texts.end()) return "not found";
        text = found->second;
        return {};
    }
    std::string trick_label(std::string_view plugin, std::string_view from, std::string_view to) override {
        calls.push_back(std::format("trick label {} {}={}", plugin, from, to));
        return {};
    }
    std::map<std::string, std::string> strings; // "<plugin> <key>" -> text
    std::string strings_set(std::string_view plugin, const std::vector<std::pair<std::string, std::string>>& texts) override {
        calls.push_back(std::format("strings {} {} {}={}", plugin, texts.size(), texts.empty() ? "" : texts[0].first,
                                    texts.empty() ? "" : texts[0].second));
        for (const auto& [key, text] : texts) strings[std::format("{} {}", plugin, key)] = text;
        return {};
    }
    std::string strings_case_rules(std::string_view plugin, std::string_view rules) override {
        calls.push_back(std::format("case rules {} {}", plugin, rules));
        return {};
    }
    std::optional<bool> string_exists(std::string_view key) override {
        return std::any_of(strings.begin(), strings.end(), [&](const auto& s) { return s.first.ends_with(" " + std::string(key)); });
    }
    std::string trick_record_as(std::string_view plugin, std::vector<std::uint32_t> from, std::uint32_t to, int within_ms) override {
        std::string ids;
        for (const auto id : from) ids += (ids.empty() ? "" : ",") + std::to_string(id);
        calls.push_back(std::format("trick record {} {}->{} {}", plugin, ids, to, within_ms));
        return {};
    }
    std::string board_toss(std::string_view plugin, std::vector<int> bones, const Vec3& velocity, const Vec3&, float ground) override {
        calls.push_back(std::format("board toss {} {} {:.0f} {:.0f}", plugin, bones.size(), velocity[1], ground));
        return {};
    }
    SkaterNews news;
    SkaterNews take_skater_news() override { return std::exchange(news, {}); }
    std::vector<PhysicsStep> steps;
    std::vector<PhysicsStep> take_physics_steps() override { return std::exchange(steps, {}); }
    std::string xray(std::string_view plugin, bool on) override {
        calls.push_back(std::format("xray {} {}", plugin, on ? "on" : "off"));
        return {};
    }
    std::string hide_skater(std::string_view plugin, bool hidden) override {
        calls.push_back(std::format("hide skater {} {}", plugin, hidden ? "on" : "off"));
        return {};
    }
    std::string hud_corner(std::string_view plugin, bool hidden) override {
        calls.push_back(std::format("corner {} {}", plugin, hidden ? "on" : "off"));
        return {};
    }
    // A plugin's score line: refused as told (score_refusal), else one plugin's line at a time on its
    // 500 ms lease, as Extension/UI/score_line_hud.cpp keeps it (by `clock`, the test's time).
    std::string score_line(std::string_view plugin, const std::optional<ScoreLine>& line) override {
        if (!line) calls.push_back(std::format("score_line {} clear", plugin));
        else {
            std::string tricks;
            for (const auto& trick : line->tricks) tricks += (tricks.empty() ? "" : "|") + trick;
            calls.push_back(std::format("score_line {} tricks={} points={} line={} x{:.2f} timer={:.2f} closed={}", plugin, tricks, line->points,
                line->line, line->multiplier, line->timer, line->closed ? 1 : 0));
            if (!score_refusal.empty()) return score_refusal;
        }
        return score_lines.ask(plugin, line, clock ? *clock : 0);
    }
    dingosdk::score_line::LineOwner score_lines;
    std::string score_refusal;
    const double* clock{};
    std::string game_speed(std::string_view plugin, float speed) override {
        calls.push_back(std::format("slow {} {:.2f}", plugin, speed));
        return {};
    }
    std::string shape_launch(std::string_view plugin, std::string_view from, std::optional<LaunchShape> shape) override {
        calls.push_back(std::format("launch {} {} {}", plugin, from, shape && shape->across ? std::format("{:.2f}", *shape->across) : "none"));
        return {};
    }
    std::vector<LaunchNews> launches;
    std::vector<LaunchNews> take_launches() override { return std::exchange(launches, {}); }
    std::string hide_pad(std::string_view plugin, const PadHidden& h) override {
        calls.push_back(std::format("hide {} {} {} {}", plugin, h.left_trigger, h.right_trigger, h.left_x_grinding) +
                        (h.right_stick || h.left_stick_click || h.right_stick_click
                             ? std::format(" stick {} {} {}", h.right_stick, h.left_stick_click, h.right_stick_click) : std::string{}) +
                        (h.buttons ? std::format(" buttons {:04x}", h.buttons) : std::string{}));
        return {};
    }
    std::string cruise(std::string_view plugin, float speed, bool may_slow) override {
        calls.push_back(std::format("cruise {} {:.1f} {}", plugin, speed, may_slow));
        return {};
    }
    // (The 1 key, 0x31, is the one held: a string "1" must ask for it, and the number 1 for the left mouse button.)
    bool key_down(int key) override { return key == 0x31; }
    bool mouse_held{};
    std::optional<MouseMotion> mouse_motion(std::string_view, bool hold) override {
        mouse_held = hold;
        return MouseMotion{3, -2, 1};
    }
    static const char* shown_name(Shown shown) { return shown == Shown::others ? "others" : shown == Shown::me ? "me" : "everyone"; }
    std::string skater_carry(std::string_view plugin, std::optional<Carry> c) override {
        calls.push_back(c ? std::format("carry {} {}{} {:.1f} {:.1f} {:.1f} front {:.2f} grip {} w {:.2f} crouch {:.2f} hunch {:.2f} turn {:.2f} step {:.2f}{}{} {}",
                                        plugin, c->left ? "left" : "right", c->both ? " both" : "", c->front[0], c->front[1], c->front[2],
                                        c->front_from_grip[2], c->worn.has_value(), c->weight, c->crouch, c->hunch, c->turn, c->step,
                                        c->still ? " still" : "", c->push_straightens ? " push" : "", shown_name(c->shown))
                          : std::format("carry {} none", plugin));
        if (c && (c->support[0] != 0 || c->support[1] != 0 || c->support[2] != 0))
            calls.push_back(std::format("carry support {} {:.2f} {:.2f} {:.2f}", plugin, c->support[0], c->support[1], c->support[2]));
        if (c && (c->exact || c->look_at))
            calls.push_back(std::format("carry held {}{} look {}", plugin, c->exact ? " exact" : "",
                                        c->look_at ? std::format("{:.1f} {:.1f} {:.1f}", (*c->look_at)[0], (*c->look_at)[1], (*c->look_at)[2]) : "none"));
        return {};
    }
    std::string skater_pose(std::string_view plugin, const std::vector<JointOffset>& offsets, float weight, Shown shown, bool still) override {
        // A whole pose (every joint from 2 set): its size and one joint of it, not 393 of them.
        if (offsets.size() > 200 && offsets[5].place && offsets[5].scale_to) {
            const auto& o = offsets[5]; // joint 7
            calls.push_back(std::format("pose {} {:.2f} whole {} from {} to {}: {} p{:.2f},{:.2f},{:.2f} r{:.2f},{:.2f},{:.2f},{:.2f} s{:.2f},{:.2f},{:.2f}", plugin,
                                        weight, offsets.size(), offsets.front().joint, offsets.back().joint, o.joint, (*o.place->position)[0],
                                        (*o.place->position)[1], (*o.place->position)[2], (*o.place->rotation)[0], (*o.place->rotation)[1],
                                        (*o.place->rotation)[2], (*o.place->rotation)[3], (*o.scale_to)[0], (*o.scale_to)[1], (*o.scale_to)[2]));
            return {};
        }
        std::string text = std::format("pose {} {:.2f}", plugin, weight);
        if (shown != Shown::everyone) text += std::format(" {}", shown_name(shown));
        if (still) text += " still";
        auto sorted = offsets;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.joint < b.joint; });
        for (const auto& o : sorted) {
            text += std::format(" {}:{:.2f},{:.2f},{:.2f},{:.2f}x{:.2f}", o.joint, o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3], o.scale[0]);
            if (o.scale[1] != o.scale[0] || o.scale[2] != o.scale[0]) text += std::format(",{:.2f},{:.2f}", o.scale[1], o.scale[2]);
            if (o.place) {
                const auto& pl = *o.place;
                text += pl.frame == dingosdk::pose_place::Frame::joint ? std::format(" place@{}", pl.joint)
                        : pl.frame == dingosdk::pose_place::Frame::skater ? std::string(" place@skater")
                        : pl.frame == dingosdk::pose_place::Frame::world  ? std::string(" place@world")
                                                                : std::string(" place@parent");
                if (pl.position) text += std::format(" p{:.2f},{:.2f},{:.2f}", (*pl.position)[0], (*pl.position)[1], (*pl.position)[2]);
                if (pl.rotation) text += std::format(" r{:.2f},{:.2f},{:.2f},{:.2f}", (*pl.rotation)[0], (*pl.rotation)[1], (*pl.rotation)[2], (*pl.rotation)[3]);
            }
            if (o.scale_to) text += std::format(" s={:.2f},{:.2f},{:.2f}", (*o.scale_to)[0], (*o.scale_to)[1], (*o.scale_to)[2]);
            if (o.keep_children) text += " keep";
        }
        calls.push_back(text);
        // The order the joints are posed in, when any is set to a place or keeps its children.
        if (std::any_of(offsets.begin(), offsets.end(), [](const auto& o) { return o.place || o.keep_children; })) {
            std::string order = std::format("pose order {}", plugin);
            for (const auto& o : offsets) order += std::format(" {}", o.joint);
            calls.push_back(order);
        }
        return {};
    }
    std::string typed = "";
    std::string typed_text(bool capture) override { return capture ? std::exchange(typed, {}) : std::string{}; }
    std::string room_phase = "idle";
    std::string room_open(std::string_view plugin, bool quiet) override {
        calls.push_back("room open " + std::string(plugin));
        if (quiet) calls.push_back("room quiet " + std::string(plugin));
        room_phase = "open";
        return {};
    }
    void room_leave(std::string_view plugin) override {
        calls.push_back("room leave " + std::string(plugin));
        room_phase = "idle";
    }
    RoomState room_state(std::string_view) override { return {room_phase, {}}; }
    std::string room_show(std::string_view plugin, bool on, bool skater) override {
        calls.push_back(std::format("room show {} {} {}", plugin, on, skater));
        return {};
    }
    std::array<float, 3> room_place() override { return {1.0f, -69.99f, -5.25f}; }
    std::string set_setting(std::string_view plugin, std::string_view name, std::optional<std::string> value) override {
        calls.push_back(std::format("setting {} {} {}", plugin, name, value ? *value : std::string("none")));
        return {};
    }
    bool skater_held(std::string_view plugin) override { return plugin == "flipper"; }
    std::string fly(std::string_view plugin, std::optional<Vec3> v) override {
        calls.push_back(v ? std::format("fly {} {:.1f} {:.1f} {:.1f}", plugin, (*v)[0], (*v)[1], (*v)[2]) : std::format("fly {} none", plugin));
        return {};
    }
    std::string set_velocity(std::string_view plugin, const Vec3& v, bool add) override {
        calls.push_back(std::format("velocity {} {:.1f} {:.1f} {:.1f}{}", plugin, v[0], v[1], v[2], add ? " add" : ""));
        return {};
    }
    std::string flip_speed(std::string_view plugin, float factor) override {
        calls.push_back(std::format("flip speed {} {:.2f}", plugin, factor));
        return {};
    }
    std::string skater_reach(std::string_view plugin, std::optional<Reach> r) override {
        calls.push_back(r ? std::format("reach {} {} {:.1f} {:.1f} {:.1f} {} {:.2f}", plugin, r->left ? "left" : "right", r->at[0], r->at[1], r->at[2],
                                        r->rotation.has_value(), r->weight) +
                                (r->shown != Shown::everyone ? std::format(" {}", shown_name(r->shown)) : std::string{})
                          : std::format("reach {} none", plugin));
        return {};
    }
    std::optional<HandPlace> skater_hand(bool left) override {
        if (left) return std::nullopt;
        return HandPlace{{1, 2, 3}, {0, 0, 0, 1}};
    }
    std::string screen_shader(std::string_view plugin, std::optional<ScreenShader> s) override {
        if (!s) {
            calls.push_back(std::format("shader {} none", plugin));
            return {};
        }
        calls.push_back(std::format("shader {} {} {} {} {:.2f} {:.2f} {:.2f}", plugin, s->file, static_cast<int>(s->kind), s->bytes ? s->bytes->size() : 0,
                                    s->values[0], s->values[1], s->values[47]));
        // As the overlay answers one that took too long on the GPU.
        if (s->file == "shaders/slow.hlsl") return "it took 9.00 ms a frame on the GPU (at most 3.0)";
        return {};
    }
    std::string world_shader(std::string_view plugin, std::optional<WorldShader> s) override {
        if (!s) {
            calls.push_back(std::format("world {} none", plugin));
            return {};
        }
        calls.push_back(std::format("world {} {} {} {} {} {:.2f} {:.2f}", plugin, s->file, static_cast<int>(s->kind), s->bytes ? s->bytes->size() : 0,
                                    s->heights, s->values[0], s->values[47]));
        return {};
    }
    std::string world_shader_data(std::string_view plugin, std::shared_ptr<const std::string> bytes) override {
        float first = 0;
        if (bytes && bytes->size() >= 4) std::memcpy(&first, bytes->data(), 4);
        calls.push_back(bytes ? std::format("world data {} {} {:.2f}", plugin, bytes->size(), first) : std::format("world data {} none", plugin));
        return {};
    }
    void world_shader_anchor(std::string_view plugin, std::optional<Vec3> at) override {
        calls.push_back(at ? std::format("world anchor {} {:.1f} {:.1f} {:.1f}", plugin, (*at)[0], (*at)[1], (*at)[2]) : std::format("world anchor {} none", plugin));
    }
    std::optional<double> world_shader_cost(std::string_view) override { return 4.5; }
    double world_shader_clock() override { return 123.25; }
    std::string screen_look(std::string_view plugin, std::optional<ScreenLook> l) override {
        calls.push_back(l ? std::format("look {} {:.2f} {:.0f} {:.2f} {}{}{}{}{}{}{}{}{}{}{} {:.1f} [{}|{}|{}|{}|{}]", plugin, l->bend, l->fov, l->age, l->bars, l->hood,
                                        l->grade, l->soft, l->scanlines, l->grain, l->dropouts, l->head_switching, l->tracking, l->smear, l->iris, l->rolling,
                                        l->text[0], l->text[1], l->text[2], l->text[3], l->text[4])
                          : std::format("look {} none", plugin));
        if (l && l->edge_blur > 0) calls.push_back(std::format("look edge {} {:.2f} {:.2f}", plugin, l->edge_blur, l->edge_start));
        if (l && l->tone) calls.push_back(std::format("look tone {} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}", plugin, l->tone_dark[2], l->tone_bright[0],
                                                      l->tone_bright[1], l->tone_curve, l->tone_noise));
        if (l && (l->bars_strength != 1 || l->hood_strength != 1 || l->grade_strength != 1 || l->soft_strength != 1 || l->scanlines_strength != 1 ||
                  l->grain_strength != 1 || l->dropouts_strength != 1 || l->head_switching_strength != 1 || l->smear_strength != 1))
            calls.push_back(std::format("look strengths {} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}", plugin, l->bars_strength,
                                        l->hood_strength, l->grade_strength, l->soft_strength, l->scanlines_strength, l->grain_strength,
                                        l->dropouts_strength, l->head_switching_strength, l->smear_strength));
        return {};
    }
    // One plugin's viewmodel at a time, as the real one.
    std::string viewmodeler;
    std::string viewmodel_load(std::string_view plugin, const std::filesystem::path& file) override {
        if (!viewmodeler.empty() && viewmodeler != plugin) return viewmodeler + " has the viewmodel";
        viewmodeler = plugin;
        calls.push_back(std::format("viewmodel load {} {}", plugin, file.filename().string()));
        return {};
    }
    void viewmodel_unload(std::string_view plugin) override {
        if (viewmodeler != plugin) return;
        viewmodeler.clear();
        calls.push_back(std::format("viewmodel unload {}", plugin));
    }
    std::optional<ViewmodelInfo> viewmodel_info(std::string_view plugin) override {
        if (viewmodeler != plugin) return std::nullopt;
        return ViewmodelInfo{"ready", "", {{"FPS_Pistol_Fire", 0.25}, {"FPS_Pistol_Idle", 2}}, {"Root", "Rif_end"}};
    }
    std::string viewmodel_show(std::string_view plugin, std::optional<ViewmodelLook> l) override {
        if (viewmodeler != plugin) return "this plugin has no viewmodel loaded";
        calls.push_back(l ? std::format("viewmodel show {} {:.2f} {:.2f} {:.2f} {:.0f} {:.0f} {:.0f} {:.0f} {:.1f} {:.1f}", plugin, l->offset[0], l->offset[1],
                                        l->offset[2], l->yaw, l->pitch, l->roll, l->fov, l->ambient, l->brightness)
                          : std::format("viewmodel hide {}", plugin));
        return {};
    }
    std::string viewmodel_play(std::string_view plugin, std::string_view clip, bool loop, float speed, float fade) override {
        if (viewmodeler != plugin) return "this plugin has no viewmodel loaded";
        if (clip != "FPS_Pistol_Fire" && clip != "FPS_Pistol_Idle") return "the viewmodel has no clip \"" + std::string(clip) + "\"";
        calls.push_back(std::format("viewmodel play {} {} {} {:.1f} {:.2f}", plugin, clip, loop, speed, fade));
        return {};
    }
    void viewmodel_stop(std::string_view plugin) override {
        if (viewmodeler == plugin) calls.push_back(std::format("viewmodel stop {}", plugin));
    }
    std::optional<ViewmodelState> viewmodel_state(std::string_view plugin) override {
        if (viewmodeler != plugin) return std::nullopt;
        return ViewmodelState{"FPS_Pistol_Fire", 0.1, 0.25, false};
    }
    std::optional<ViewmodelBone> viewmodel_bone(std::string_view plugin, std::string_view bone) override {
        if (viewmodeler != plugin || bone != "Rif_end") return std::nullopt;
        return ViewmodelBone{{0.08f, -0.2f, 0.4f}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    }
    std::map<std::string, Tuning> tunings{{"physicsmode.jumpmaxheight", {"PhysicsMode.JumpMaxHeight", "Jump height", "Jumps", "number", 1.2, 1.2, ""}},
                                          {"physicsmode.autopushenabled", {"PhysicsMode.AutoPushEnabled", "Auto push", "Push", "flag", 0, 0, ""}}};
    std::optional<Tuning> tuning(std::string_view id, std::string& why) override {
        std::string key(id);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto at = tunings.find(key);
        if (at == tunings.end()) {
            why = "no tuning value is called \"" + std::string(id) + "\"";
            return std::nullopt;
        }
        return at->second;
    }
    std::vector<Tuning> find_tuning(std::string_view text, std::size_t most) override {
        std::vector<Tuning> out;
        for (const auto& [key, t] : tunings)
            if (key.find(text) != std::string::npos && out.size() < most) out.push_back(t);
        return out;
    }
    std::string tune(std::string_view plugin, std::string_view id, std::optional<double> value) override {
        calls.push_back(value ? std::format("tune {} {} {:.2f}", plugin, id, *value) : std::format("tune {} {} none", plugin, id));
        return {};
    }
    // 395 joints: joint j has scale 1, no turn, and stands j / 100 m along x from its parent.
    bool has_local_pose = true;
    std::vector<float> skater_local_pose() override {
        std::vector<float> out;
        if (!has_local_pose) return out;
        for (int j = 0; j < 395; ++j) out.insert(out.end(), {1, 1, 1, 0, 0, 0, 1, static_cast<float>(j) / 100, 0, 0});
        return out;
    }
    // The same, as drawn: the game's body physics has lifted every joint 5 cm since.
    std::vector<float> skater_drawn_pose() override {
        std::vector<float> out;
        if (!has_local_pose) return out;
        for (int j = 0; j < 395; ++j) out.insert(out.end(), {1, 1, 1, 0, 0, 0, 1, static_cast<float>(j) / 100, 0.05f, 0});
        return out;
    }
    std::optional<JointPlace> skater_joint(std::uint64_t player, std::uint16_t joint) override {
        if (player == 7 || joint == 300) return std::nullopt;
        return JointPlace{{static_cast<float>(player), static_cast<float>(joint), 0}, {0, 0, 0, 1}, 1};
    }
    std::string screen_slap(std::string_view plugin) override {
        calls.push_back(std::format("slap {}", plugin));
        return {};
    }
    std::optional<Vec3> board_velocity() override { return Vec3{1, -2, 3}; }
    std::optional<Vec3> board_spin() override { return Vec3{0, 4, 0}; }
    std::optional<SlamNews> last_slam() override { return SlamNews{7.5f, 12, 100}; }
    std::string wipeout(float force) override {
        calls.push_back(std::format("wipeout {:.1f}", force));
        return {};
    }
    std::string skydive() override {
        calls.push_back("skydive");
        return {};
    }
    std::string control(std::string_view action) override { calls.push_back("control " + std::string(action)); return {}; }
    void hide_hud(std::string_view plugin, bool hidden) override { calls.push_back(std::string("hud ") + std::string(plugin) + (hidden ? " on" : " off")); }
    std::vector<CatalogItem> object_catalog() override { return {{"bench_01", "Bench", "Benches"}}; }
    std::vector<GameObject> objects() override { return {{42, "bench_01", {1, 0, 2}, {0, 0, 0, 1}, 1}}; }
    std::string edit_object(std::string_view op, const GameObject& o) override {
        calls.push_back(std::format("{} {} {} {:.1f}", op, o.id, o.item, o.position[0]));
        return {};
    }
    // World objects as the fake keeps them: by plugin and id, ready at once unless hidden.
    std::map<std::pair<std::string, std::uint32_t>, std::pair<std::string, std::array<float, 16>>> entities;
    std::string entity_set(std::string_view plugin, std::uint32_t id, std::string_view asset, const std::array<float, 16>& m, bool hidden,
                           std::optional<EntityAttach> attach, bool solid) override {
        if (entities.size() >= 600) return "too many";
        entities[{std::string(plugin), id}] = {std::string(asset) + (hidden ? " hidden" : ""), m};
        calls.push_back(std::format("entity {} {} {} {:.2f} {:.2f} {:.2f} {:.2f}{}{}", plugin, id, asset, m[12], m[13], m[14], m[8], hidden ? " hidden" : "",
                                    attach ? std::format(" on {} {}{}{}{}", attach->player, attach->joint, attach->upright ? " upright" : "", attach->turn ? "" : " held",
                                                     attach->ground > 0 ? std::format(" ground {:.1f}", attach->ground) : std::string{}) : std::string{}) +
                        (solid ? "" : " ghost"));
        return {};
    }
    void entity_remove(std::string_view plugin, std::uint32_t id) override { entities.erase({std::string(plugin), id}); }
    std::optional<std::array<Vec3, 2>> entity_bounds(std::string_view blueprint) override {
        if (blueprint != "World/Props/TrashCan_01") return std::nullopt;
        return std::array<Vec3, 2>{Vec3{-0.3f, 0, -0.3f}, Vec3{0.3f, 0.9f, 0.3f}};
    }
    std::vector<std::string> entity_blueprints(std::string_view prefix, bool meshes_only) override {
        std::vector<std::string> out;
        for (const auto* name : {"World/Props/Bench_01", "World/Props/TrashCan_01", "World/Vehicles/Car_01"})
            if (std::string_view(name).starts_with(prefix) && (!meshes_only || entity_bounds(name))) out.emplace_back(name);
        return out;
    }
    // reskate.world.probe's ray, as told; the bounded adapter below counts its native casts.
    WorldProbe probe_result;
    unsigned probes{}, world_picks{};
    std::uint64_t vehicle_pick_owner{};
    std::uint64_t next_vehicle{};
    unsigned vehicle_takes{}, vehicle_releases{}, vehicle_release_alls{};
    std::string vehicle_take_refusal;
    std::map<std::uint64_t, traffic_vehicles::Sample> vehicles;
    std::map<std::uint64_t, std::uint64_t> vehicle_owners;
    Vec3 probe_from{}, probe_to{};
    WorldProbe world_probe(const Vec3& from, const Vec3& to) {
        ++probes;
        probe_from = from;
        probe_to = to;
        return probe_result;
    }
    CountedProbe world_probe_bounded(const Vec3& from, const Vec3& to, unsigned allowed) override {
        if (!allowed) return {};
        const auto cast_count = std::min(allowed, probe_cast_cost);
        native_casts += cast_count;
        const auto probe = world_probe(from, to);
        return {cast_count && cast_count == probe_cast_cost ? probe : WorldProbe{}, cast_count};
    }
    // The counted queries' native cost per ray (batch rays, world.probe), and the native casts
    // they spent. world.pick and world.raycast are not counted.
    unsigned query_cast_cost{1}, probe_cast_cost{1}, native_casts{};
    // The loaded level's map: none until a test gives it one.
    std::optional<MapArea> the_map;
    std::optional<MapArea> map_area() override { return the_map; }
    std::optional<WorldPick> world_pick(const Vec3& from, const Vec3&) override {
        ++world_picks;
        if (from[1] > 50) return std::nullopt; // the sky: nothing met
        WorldPick pick{{1, 0, 2}, true, std::array<float, 16>{0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 3, 1}, "World/Props/TrashCan_01"};
        pick.vehicle_owner = vehicle_pick_owner;
        return pick;
    }
    std::string vehicle_take(std::uint64_t owner, std::uint64_t entity, std::uint64_t, std::uint64_t& handle) override {
        ++vehicle_takes;
        if (!vehicle_take_refusal.empty()) return vehicle_take_refusal;
        if (!entity) return "not a vehicle";
        unsigned active{};
        for (const auto& [id, sample] : vehicles) if (vehicle_owners[id] == owner &&
            (sample.state == traffic_vehicles::State::finding || sample.state == traffic_vehicles::State::held)) ++active;
        if (active >= 4) return "too many vehicles";
        handle = ++next_vehicle;
        vehicle_owners[handle] = owner;
        vehicles[handle] = {traffic_vehicles::State::finding, {{1, 0, 2}, {0, 0, 0, 1}}, {}};
        return {};
    }
    traffic_vehicles::Sample vehicle_read(std::uint64_t owner, std::uint64_t handle) override {
        if (vehicle_owners[handle] != owner) return {};
        auto sample = vehicles.at(handle);
        if (sample.state == traffic_vehicles::State::finding) vehicles[handle].state = traffic_vehicles::State::held;
        return sample;
    }
    std::string vehicle_set_pose(std::uint64_t owner, std::uint64_t handle, const traffic_vehicles::Pose& pose) override {
        if (vehicle_owners[handle] != owner || vehicles[handle].state != traffic_vehicles::State::held) return "vehicle is not held";
        vehicles[handle].pose = pose;
        return {};
    }
    bool vehicle_release(std::uint64_t owner, std::uint64_t handle) override {
        if (vehicle_owners[handle] != owner) return false;
        ++vehicle_releases;
        vehicles[handle].state = traffic_vehicles::State::released;
        return true;
    }
    void vehicle_release_all(std::uint64_t owner) override {
        ++vehicle_release_alls;
        for (auto& [id, sample] : vehicles) if (vehicle_owners[id] == owner) sample.state = traffic_vehicles::State::released;
    }
    std::string skater_copy(std::string_view plugin, std::optional<SkaterCopyPlace> place) override {
        calls.push_back(place ? std::format("copy {} at {:.0f} {:.0f} {:.0f} board {}", plugin, place->position[0], place->position[1], place->position[2], place->board)
                              : std::format("copy {} gone", plugin));
        copy_on = place.has_value();
        return {};
    }
    bool copy_on{};
    bool skater_copy_shown() override { return copy_on; }
    std::string piece_set(std::string_view plugin, std::uint32_t id, std::string_view prefab, const std::array<float, 16>& m) override {
        if (!prefab.starts_with("lighting/prefabs/")) return "only the game's light rigs";
        calls.push_back(std::format("piece {} {} {} at {:.0f} {:.0f} {:.0f}", plugin, id, prefab, m[12], m[13], m[14]));
        return {};
    }
    void piece_remove(std::string_view plugin, std::uint32_t id) override { calls.push_back(std::format("piece remove {} {}", plugin, id)); }
    bool piece_ready(std::string_view, std::uint32_t id) override { return id == 1; }
    void entity_clear(std::string_view plugin) override {
        std::erase_if(entities, [&](const auto& e) { return e.first.first == plugin; });
        calls.push_back("entities clear " + std::string(plugin));
    }
    int entity_status(std::string_view plugin, std::uint32_t id) override {
        const auto found = entities.find({std::string(plugin), id});
        return found == entities.end() ? 0 : found->second.first.ends_with(" hidden") ? 5 : 4;
    }
    bool entities_ready(std::string_view) override { return true; }
    std::string set_environment(std::string_view kind, std::string_view key, std::string_view value) override {
        calls.push_back(std::format("env {} {} {}", kind, key, value));
        return key == "never" ? "refused" : "";
    }
    std::vector<std::pair<std::string, double>> world_controls() override { return {{"fog", 0}, {"fog_distance", 1.5}, {"clouds", -1}}; }
    std::vector<Destination> destinations() override { return {{"bam", "BAM", "bam"}}; }
    std::vector<LevelInfo> levels() override { return {{"Levels/Game/X/X", "X", true}}; }
    std::string travel(std::string_view kind, std::string_view target) override { calls.push_back(std::format("travel {} {}", kind, target)); return {}; }
    std::string ai(std::string_view op, int count) override { calls.push_back(std::format("ai {} {}", op, count)); return {}; }
    int ai_count() override { return 3; }
    std::vector<PeerSkater> peer_skaters() override { return {{7, {1, 2, 3}, 0.5f, 40}}; }
    std::vector<Body> body_list; // what bodies() reads
    std::set<std::pair<std::string, std::uint64_t>> hidden_list;
    std::vector<Body> bodies() override { return body_list; }
    std::string hide_player(std::string_view plugin, std::uint64_t player, bool hidden) override {
        calls.push_back(std::format("hide {} {} {}", plugin, player, hidden ? "on" : "off"));
        if (hidden) hidden_list.insert({std::string(plugin), player});
        else hidden_list.erase({std::string(plugin), player});
        return {};
    }
    std::vector<std::uint64_t> hidden_players(std::string_view plugin) override {
        std::vector<std::uint64_t> out;
        for (const auto& [owner, who] : hidden_list)
            if (owner == plugin) out.push_back(who);
        return out;
    }
    std::string place_player(std::string_view plugin, std::uint64_t player, std::optional<SkaterPlacement> placement) override {
        if (!placement) calls.push_back(std::format("place {} {} back", plugin, player));
        else
            calls.push_back(std::format("place {} {} at {:.1f} {:.1f} {:.1f} turned {} joint {}{}", plugin, player, placement->position[0], placement->position[1],
                                        placement->position[2], placement->rotation ? std::format("{:.2f}", (*placement->rotation)[3]) : "no",
                                        placement->joint ? std::to_string(*placement->joint) : "none", placement->board ? "" : " no board"));
        return {};
    }
    std::map<std::uint64_t, PlayerOutfit> outfits; // what player_outfit() reads
    std::optional<PlayerOutfit> player_outfit(std::uint64_t player) override {
        const auto found = outfits.find(player);
        if (found == outfits.end()) return std::nullopt;
        return found->second;
    }
    std::string hide_items(std::string_view plugin, HiddenItems items) override {
        std::string line = std::format("hide-items {}", plugin);
        for (const auto& name : items.everyone) line += " " + name;
        for (const auto& [player, names] : items.players)
            for (const auto& name : names) line += std::format(" {}:{}", player, name);
        calls.push_back(line);
        return {};
    }
    void set_nametag(std::string_view plugin, std::uint64_t player, std::string text, std::uint32_t colour) override {
        calls.push_back(std::format("tag {} {} {} {:08x}", plugin, player, text, colour));
    }
    std::string voice_volume(std::uint64_t player, float volume) override { calls.push_back(std::format("voice {} {:.1f}", player, volume)); return {}; }
    std::vector<ChatLine> chat_since(std::uint64_t sequence) override {
        std::vector<ChatLine> result;
        for (const auto& line : lines) if (line.sequence > sequence) result.push_back(line);
        return result;
    }
    std::string send_chat(std::string_view text) override { calls.push_back("chat " + std::string(text)); return {}; }
    std::string local_chat(std::string_view plugin, std::string_view text, std::uint32_t colour) override {
        calls.push_back(std::format("notice {} {} {:08x}", plugin, text, colour));
        return {};
    }
    void set_chat_commands(std::vector<ChatCommandInfo> commands) override { chat_command_list = std::move(commands); }
    std::vector<ChatCommandCall> take_chat_commands() override { return std::exchange(typed_commands, {}); }
    ThrowdownState throwdowns() override { return throwdown; }
    ClipRef recorded;
    std::vector<std::string> game_clip_names(std::string_view containing, std::size_t most, std::string& why) override {
        if (!game_clips_ready) {
            why = "the game's animations are still being read";
            return {};
        }
        std::vector<std::string> out;
        for (const char* name : {"animation/dingo/c_proto_onb_trick_o_kickflip_high", "animation/dingo/c_proto_onb_trick_o_ollie_mid",
                                 "animation/dingo/seq_c_proto_onb_trick_o_kickflip_low"})
            if (std::string_view(name).find(containing) != std::string_view::npos && out.size() < most) out.push_back(name);
        return out;
    }
    // A game clip: three frames of the whole skeleton, 0.1 s apart, its takeoff and landing marked.
    ClipRef game_clip(std::string_view name, std::string& why) override {
        if (!game_clips_ready) {
            why = "the game's animations are still being read";
            return nullptr;
        }
        if (name.starts_with("animation/dingo/seq_")) {
            why = std::string(name) + " is a SequenceContainerAsset, not a clip";
            return nullptr;
        }
        calls.push_back("game clip " + std::string(name));
        auto clip = std::make_shared<dingosdk::anim_studio::Clip>();
        clip->joints = dingosdk::anim_studio::skeleton_joints;
        clip->times = {0, 0.1f, 0.2f};
        clip->frames.resize(3 * clip->joints);
        clip->board.resize(3);
        clip->takeoff = 0.05f;
        clip->landing = 0.15f;
        return clip;
    }
    bool game_clips_ready{};
    // The fake's clips stand regular, and a mirror is the clip with its takeoff moved, to tell it apart.
    std::optional<bool> clip_goofy(const ClipRef& clip) override {
        if (!game_clips_ready || !clip) return std::nullopt;
        return clip->takeoff > 0.07f;
    }
    ClipRef mirror_clip(const ClipRef& clip, std::string& why) override {
        if (!game_clips_ready) {
            why = "the game's animations are still being read";
            return nullptr;
        }
        auto out = std::make_shared<dingosdk::anim_studio::Clip>(*clip);
        out->takeoff += 0.05f;
        calls.push_back("mirror");
        return out;
    }
    std::string play_clip(std::string_view plugin, ClipRef clip, float speed, float blend, bool loop, float at, bool paused, bool floor) override {
        calls.push_back(std::format("play {} {} frames {:.1f} {:.2f} {}", plugin, clip->times.size(), speed, blend, loop) +
                        (at > 0 || paused ? std::format(" at {:.2f}{}", at, paused ? " paused" : "") : std::string{}) + (floor ? " floor" : ""));
        return {};
    }
    void seek_clip(std::string_view plugin, float seconds) override { calls.push_back(std::format("seek {} {:.2f}", plugin, seconds)); }
    void pause_clip(std::string_view plugin, bool paused) override { calls.push_back(std::format("pause {} {}", plugin, paused)); }
    std::optional<float> clip_time(std::string_view) override { return 0.25f; }
    void stop_clip(std::string_view plugin) override { calls.push_back("stop " + std::string(plugin)); }
    std::string replace_trick(std::string_view plugin, std::string trick, std::string pop, ClipRef clip, float) override {
        calls.push_back(std::format("replace {} {} {} {}", plugin, trick, pop.empty() ? "any" : pop, clip ? "clip" : "none"));
        return {};
    }
    std::string record_clip(std::string_view plugin, float seconds) override {
        calls.push_back(std::format("record {} {:.1f}", plugin, seconds));
        auto clip = std::make_shared<dingosdk::anim_studio::Clip>();
        clip->joints = dingosdk::anim_studio::skeleton_joints;
        clip->times = {0, 0.25f, 0.5f};
        clip->frames.resize(clip->times.size() * clip->joints);
        clip->takeoff = 0.1f;
        clip->landing = 0.4f;
        recorded = std::move(clip);
        return {};
    }
    ClipRef take_recording(std::string_view) override { return std::exchange(recorded, nullptr); }
    std::string current_trick() override { return "kickflip"; }
    std::optional<FlightInput> flight_input(std::string_view) override { return FlightInput{1, 0, 0.5f, 3, 4, true}; }
    std::optional<PadInput> pad = PadInput{0.25f, 0, 0, 0, 0, 1, 0x1000, false};
    std::optional<PadInput> gamepad(int controller) override { read_controller = controller; return pad; }
    std::optional<bool> gameplay_option(std::string_view name) override { return name == "example" ? std::optional<bool>{true} : std::nullopt; }
    std::string hold_gameplay_option(std::string_view, std::string_view name, std::optional<bool>) override { return name == "example" ? "" : "unknown option"; }
    std::optional<RailTarget> rail_target() override { return RailTarget{true, true, {0, 0, 1}}; }
    std::string select_rail_target(std::string_view, std::optional<int>, bool) override { return {}; }
    std::optional<FlipState> flip_state_result;
    unsigned flip_state_reads{};
    std::optional<FlipState> flip_state() override { ++flip_state_reads; return flip_state_result; }
    std::optional<SkaterFrame> skater_frame() override { return SkaterFrame{{1, 2, 3}, {0, 0, 1}, false, true, false, false, 42}; }
    std::vector<AnimationValue> animation_values() override { return {{"Animation/Dingo/Bool.Anim.DoingTrick", 1, true, false}, {"Animation/Dingo/Float.Physics.Example", 0.5, false, true}}; }
    std::string set_animation_value(std::string_view name, double) override { return name == "Animation/Dingo/Float.Physics.Example" ? "" : "not a live numeric physics output"; }
    int read_controller = -1;
    std::optional<PadInput> written_pad; unsigned written_controller{};
    std::string write_pad(std::string_view, unsigned controller, std::optional<PadInput> value) override { written_controller = controller; written_pad = value; return {}; }
    std::optional<StickInput> joystick(int device) override {
        if (device > 1) return std::nullopt;
        StickInput stick;
        stick.axes = {0, -1, 0.5f, 0, 0, 0, 0.25f, -0.75f};
        stick.buttons = 0x4;
        stick.name = device == 1 ? "Pad" : "Radio";
        stick.device = device == 1 ? 1 : 0;
        return stick;
    }
    std::vector<StickDevice> joysticks() override { return {{0, "Radio"}, {1, "Pad"}}; }
    std::string capture_input(std::string_view plugin, bool on) override {
        calls.push_back(std::format("capture {} {}", plugin, on ? "on" : "off"));
        return {};
    }
    std::uint64_t sound_ids{};
    bool hold_waves{}; // keep each sound's wave, as a voice still playing does
    std::vector<std::shared_ptr<const Wave>> held_waves;
    std::uint64_t play_sound(std::string_view plugin, SoundStart sound) override {
        calls.push_back(std::format("sound {} {} {:.2f} {:.2f} {}{}", plugin, sound.wave ? std::to_string(sound.wave->samples.size()) : "motor", sound.volume,
                                    sound.pitch, sound.loop, sound.position ? std::format(" at {} {} {}", (*sound.position)[0], (*sound.position)[1], (*sound.position)[2]) : sound.listener ? " at listener" : ""));
        if (hold_waves && sound.wave) held_waves.push_back(std::move(sound.wave));
        return ++sound_ids;
    }
    bool change_sound(std::string_view plugin, std::uint64_t id, const SoundChange& change) override {
        calls.push_back(std::format("change {} {} {:.1f}{}", plugin, id, change.throttle.value_or(-1),
                                    change.position && !*change.position ? " skater" : ""));
        return true;
    }
    void stop_sound(std::string_view plugin, std::uint64_t id) override { calls.push_back(std::format("stop sound {} {}", plugin, id)); }
    bool sound_playing(std::string_view, std::uint64_t id) override { return id <= sound_ids; }
    std::string host_throwdown(std::string_view plugin, std::string_view series, std::vector<std::uint64_t> players, int most) override {
        calls.push_back(std::format("host {} {} {} players max {}", plugin, series, players.size(), most));
        return {};
    }
    std::string throwdown_status(std::string_view) override { return "open"; }
    std::vector<ThrowdownMode> modes;
    std::string set_throwdown_mode(std::string_view plugin, const ThrowdownMode& mode) override {
        calls.push_back(std::format("mode {} {} {}", plugin, mode.id, mode.name));
        std::erase_if(modes, [&](const auto& m) { return m.id == mode.id; });
        modes.push_back(mode);
        return {};
    }
    void remove_throwdown_mode(std::string_view plugin, std::string_view id) override { calls.push_back(std::format("remove mode {} {}", plugin, id)); }
    // The throwdown kit: documents go to the real store (so the tests read them parsed); the flag is
    // the fake's own (kit::flag is the game's).
    int kit_documents{};
    std::string publish_throwdown_ui(std::string_view plugin, const dingosdk::Json& document) override {
        ++kit_documents;
        return dingosdk::throwdowns::kit::publish(plugin, document);
    }
    dingosdk::throwdowns::kit::flag::State flag;
    bool throwdown_flag_begin(std::string_view mode, std::uint32_t game, const dingosdk::throwdowns::kit::flag::Options& o) override {
        calls.push_back(std::format("flag begin {} {} {} {} {} {:.1f}", mode, game, o.players, o.seconds, o.setup_page, o.cursor_height));
        if (flag.busy) return false;
        flag = {true, false, true, true, std::string(mode), game};
        return true;
    }
    dingosdk::throwdowns::kit::flag::State throwdown_flag_state() override { return flag; }
    void throwdown_flag_leave(std::string_view plugin) override {
        calls.push_back(std::format("flag leave {}", plugin));
        if (flag.mode.starts_with(std::string(plugin) + "/")) flag = {};
    }
    // A plugin's data folder, as a map by "<plugin>/<path>".
    std::map<std::string, std::string> files;
    std::string files_list(std::string_view plugin, std::string_view folder, std::vector<std::string>& names) override {
        const auto prefix = std::string(plugin) + "/" + (folder.empty() ? "" : std::string(folder) + (folder.ends_with(':') ? "" : "/"));
        for (const auto& [path, text] : files) {
            if (!path.starts_with(prefix)) continue;
            auto rest = path.substr(prefix.size());
            if (const auto slash = rest.find('/'); slash != std::string::npos) rest = rest.substr(0, slash + 1);
            if (std::find(names.begin(), names.end(), rest) == names.end()) names.push_back(rest);
        }
        return {};
    }
    std::string files_read(std::string_view plugin, std::string_view path, std::string& text) override {
        const auto found = files.find(std::string(plugin) + "/" + std::string(path));
        if (found == files.end()) return "not found";
        text = found->second;
        return {};
    }
    std::string files_write(std::string_view plugin, std::string_view path, std::string_view text) override {
        files[std::string(plugin) + "/" + std::string(path)] = std::string(text);
        return {};
    }
    std::string files_remove(std::string_view plugin, std::string_view path) override {
        return files.erase(std::string(plugin) + "/" + std::string(path)) ? std::string{} : std::string("not found");
    }
    // The rest of reskate.files over the same made-up files: times kept beside them, a hash that says
    // what it hashed, and a Recycle Bin that holds what went in.
    std::map<std::string, double> file_times;
    std::vector<std::string> recycled;
    bool recycle_bin_full{};
    std::string files_info(std::string_view plugin, std::string_view path, FileInfo& found) override {
        const auto key = std::string(plugin) + "/" + std::string(path);
        if (const auto file = files.find(key); file != files.end()) {
            found = {file->second.size(), file_times.contains(key) ? file_times[key] : 1000.0, false};
            return {};
        }
        for (const auto& [name, text] : files)
            if (name.starts_with(key + "/")) {
                found = {0, 1000.0, true};
                return {};
            }
        return "not found";
    }
    std::string files_move(std::string_view plugin, std::string_view from, std::string_view to) override {
        const auto found = files.find(std::string(plugin) + "/" + std::string(from));
        if (found == files.end()) return "not found";
        files[std::string(plugin) + "/" + std::string(to)] = found->second;
        files.erase(found);
        return {};
    }
    std::string files_copy(std::string_view plugin, std::string_view from, std::string_view to) override {
        const auto found = files.find(std::string(plugin) + "/" + std::string(from));
        if (found == files.end()) return "not found";
        files[std::string(plugin) + "/" + std::string(to)] = found->second;
        return {};
    }
    std::string files_touch(std::string_view plugin, std::string_view path, double modified) override {
        const auto key = std::string(plugin) + "/" + std::string(path);
        if (!files.contains(key)) return "not found";
        file_times[key] = modified;
        return {};
    }
    std::string files_recycle(std::string_view plugin, const std::vector<std::string>& paths) override {
        if (recycle_bin_full) return "they could not be put in the Recycle Bin";
        for (const auto& path : paths)
            if (!files.contains(std::string(plugin) + "/" + path)) return path + ": not found";
        for (const auto& path : paths) {
            files.erase(std::string(plugin) + "/" + path);
            recycled.push_back(path);
        }
        return {};
    }
    std::string files_hash(std::string_view plugin, std::string_view path, std::string& digest) override {
        const auto found = files.find(std::string(plugin) + "/" + std::string(path));
        if (found == files.end()) return "not found";
        digest = "hash of " + found->second;
        return {};
    }
    // A data file's place: a made-up path, and a stamp that follows what the file holds.
    std::string files_place(std::string_view plugin, std::string_view path, std::filesystem::path& file, std::uint64_t& stamp) override {
        const auto found = files.find(std::string(plugin) + "/" + std::string(path));
        file = std::filesystem::path("data") / std::string(plugin) / std::string(path);
        stamp = found == files.end() ? 0 : std::hash<std::string>{}(found->second) | 1;
        return {};
    }
    // reskate.picture: the requests as they came, each one's job done at once (or kept, for the test
    // to finish), a text half its size wide a byte, and a picture's size by where it is.
    std::vector<picture::Request> pictures;
    std::vector<std::shared_ptr<picture::Job>> picture_jobs;
    bool pictures_wait{};
    std::string picture_refusal, picture_failure;
    std::string picture_make(std::string_view plugin, picture::Request request, std::shared_ptr<picture::Job> job) override {
        if (!picture_refusal.empty()) return picture_refusal;
        calls.push_back(std::format("picture {} {} {}x{} steps {}", plugin, request.path, request.width, request.height, request.steps.size()));
        pictures.push_back(std::move(request));
        picture_jobs.push_back(job);
        if (!pictures_wait) {
            job->why = picture_failure;
            job->done = true;
        }
        return {};
    }
    // A zip that is a file: what each made-up zip holds, by "<plugin>/<path>"; and the requests to
    // unpack one as they came, each one's job done at once (or kept, for the test to finish). A zip
    // that unpacks writes each file's name as its content into the made-up files.
    std::map<std::string, std::vector<zip_file::Entry>> zips;
    std::vector<zip_file::Request> unzips;
    std::vector<std::shared_ptr<zip_file::Job>> unzip_jobs;
    bool unzips_wait{};
    std::string unzip_refusal, unzip_failure;
    std::string zip_list(std::string_view plugin, std::string_view path, std::vector<zip_file::Entry>& entries) override {
        const auto key = std::string(plugin) + "/" + std::string(path);
        calls.push_back("zip list " + key);
        if (!files.contains(key)) return "not found";
        const auto found = zips.find(key);
        if (found == zips.end()) return "it is not a zip";
        entries = found->second;
        return {};
    }
    std::string files_unzip(std::string_view plugin, zip_file::Request request, std::shared_ptr<zip_file::Job> job) override {
        if (!unzip_refusal.empty()) return unzip_refusal;
        const auto zip = zips.find(std::string(plugin) + "/" + request.zip);
        if (zip == zips.end()) return "not found";
        calls.push_back(std::format("unzip {} {} to {} only {} overwrite {}", plugin, request.zip, request.folder,
                                    request.all ? std::string("all") : std::to_string(request.only.size()), request.overwrite));
        if (!unzips_wait) {
            job->why = unzip_failure;
            if (unzip_failure.empty())
                for (const auto& entry : zip->second) {
                    if (!request.all && std::find(request.only.begin(), request.only.end(), entry.name) == request.only.end()) continue;
                    const auto between = request.folder.empty() || request.folder.ends_with(':') ? "" : "/";
                    files[std::string(plugin) + "/" + request.folder + between + entry.name] = entry.name;
                    ++job->count;
                }
            job->done = true;
        }
        unzips.push_back(std::move(request));
        unzip_jobs.push_back(std::move(job));
        return {};
    }
    std::optional<double> picture_text_width(std::string_view, const picture::File& font, std::string_view text, float size) override {
        if (font.name.find("missing") != std::string::npos) return std::nullopt;
        return static_cast<double>(text.size()) * size * 0.5;
    }
    // A plugin's own fonts as the fake reads them, at a size of 20: one named "unreadable" cannot be
    // read; one named "digits" has the digits (14 wide) and a colon (6) and nothing else; any other has
    // every character to 127 (8 wide, in a box) and an e with an acute accent (9).
    std::vector<std::string> font_asks;
    std::shared_ptr<const plugin_ui::detail::FontMetrics> plugin_font_metrics(std::string_view plugin, std::string_view path,
                                                                              const std::filesystem::path& file) override {
        font_asks.push_back(std::format("{} {}", plugin, path));
        std::error_code error;
        if (path.find("unreadable") != std::string_view::npos || !std::filesystem::is_regular_file(file, error)) return nullptr;
        auto metrics = std::make_shared<plugin_ui::detail::FontMetrics>();
        metrics->own = true;
        metrics->size = 20;
        metrics->ascent = 16;
        if (path.find("digits") != std::string_view::npos) {
            metrics->lacks.fill(true);
            for (int c = '0'; c <= ':'; ++c) {
                metrics->lacks[static_cast<std::size_t>(c - 32)] = false;
                metrics->advance[static_cast<std::size_t>(c - 32)] = c == ':' ? 6.0f : 14.0f;
                metrics->box[static_cast<std::size_t>(c - 32)] = {2, 2, 12, 16};
            }
        } else {
            metrics->advance.fill(8);
            metrics->box.fill({1, 3, 7, 16});
            metrics->lacks[127 - 32] = true;
            metrics->wide = {{0xe9, 9.0f}};
        }
        return metrics;
    }
    std::optional<std::pair<std::uint32_t, std::uint32_t>> picture_size(std::string_view plugin, const picture::File& file) override {
        if (!file.data.empty()) {
            if (!files.contains(std::string(plugin) + "/" + file.data)) return std::nullopt;
            return std::pair<std::uint32_t, std::uint32_t>{64, 32};
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(file.file, error)) return std::nullopt;
        return std::pair<std::uint32_t, std::uint32_t>{256, 128};
    }
    std::optional<DialogSpec> dialog;
    std::string dialog_open(std::string_view plugin, const DialogSpec& spec) override {
        calls.push_back(std::format("dialog {} {} rows {} buttons {}", plugin, spec.id, spec.rows.size(), spec.buttons.size()));
        dialog = spec;
        return {};
    }
    // The window's focus (reskate.input.focus), and how often it was read.
    InputFocus focus_now{};
    unsigned focus_reads{};
    InputFocus input_focus() override { ++focus_reads; return focus_now; }
    void dialog_close(std::string_view plugin, std::string_view id) override { calls.push_back(std::format("dialog close {} {}", plugin, id)); }
    std::string skater_push(std::string_view plugin, const Vec3& v) override {
        calls.push_back(std::format("push {} {:.0f} {:.0f} {:.0f}", plugin, v[0], v[1], v[2]));
        return {};
    }
    std::string skater_push_wait(std::string_view plugin) override {
        calls.push_back(std::format("push wait {}", plugin));
        return {};
    }
    std::string skater_push_reset(std::string_view plugin) override {
        calls.push_back(std::format("push reset {}", plugin));
        return {};
    }
    std::string skater_hold(std::string_view plugin, bool on) override {
        calls.push_back(std::format("hold {} {}", plugin, on));
        return {};
    }
    std::string kick(std::uint64_t player, std::string_view why) override { calls.push_back(std::format("kick {} {}", player, why)); return {}; }
    std::string announce(std::string_view plugin, std::string_view text) override {
        calls.push_back(std::format("announce {} {}", plugin, text));
        return {};
    }
    std::optional<Role> role(std::uint64_t player) override {
        if (player == 1) return Role{"moderator", 50, true};
        return std::nullopt;
    }
    std::vector<ServerPark> server_parks() override { return {{"street", "San Vansterdam", true, true, 12, 5}, {"plaza", "Isle of Grom", false, false, 0, 0}}; }
    std::string spawn_park(std::string_view plugin, std::string_view name, unsigned minutes) override {
        calls.push_back(std::format("park spawn {} {} {}", plugin, name, minutes));
        return name == "plaza" ? "plaza belongs on Isle of Grom, and the server is on San Vansterdam." : std::string{};
    }
    std::string remove_park(std::string_view plugin, std::string_view name) override {
        calls.push_back(std::format("park remove {} {}", plugin, name));
        return {};
    }
    std::string server_hide(std::string_view plugin, std::uint64_t target, const std::vector<std::uint64_t>& viewers, const Hiding& what) override {
        calls.push_back(std::format("hide {} {} from {} skater {} voice {} sound {}", plugin, target, players_text(viewers), what.skater, what.voice,
                                    what.sound));
        return target == 9 ? "that player is not on the server" : std::string{};
    }
    std::string server_unhide(std::string_view plugin, std::uint64_t target, const std::vector<std::uint64_t>& viewers) override {
        calls.push_back(std::format("unhide {} {} from {}", plugin, target, players_text(viewers)));
        return {};
    }
    static std::string players_text(const std::vector<std::uint64_t>& players) {
        std::string text;
        for (const auto id : players) text += (text.empty() ? "" : ",") + std::to_string(id);
        return text.empty() ? "all" : text;
    }
    std::string server_card(std::string_view plugin, const std::vector<std::uint64_t>& players, std::string_view text, unsigned seconds) override {
        calls.push_back(std::format("card {} {} {} {}", plugin, players_text(players), text, seconds));
        return {};
    }
    std::uint32_t next_poll = 41;
    std::string server_poll(std::string_view plugin, const std::vector<std::uint64_t>& players, std::string_view question,
                            const std::vector<std::string>& answers, unsigned seconds, std::uint32_t& id) override {
        std::string listed;
        for (const auto& answer : answers) listed += (listed.empty() ? "" : "|") + answer;
        calls.push_back(std::format("poll {} {} {} {} {}", plugin, players_text(players), question, listed, seconds));
        if (players == std::vector<std::uint64_t>{9}) return "that player is not on the server";
        id = next_poll++;
        return {};
    }
    std::string server_poll_close(std::string_view plugin, std::uint32_t id) override {
        calls.push_back(std::format("poll close {} {}", plugin, id));
        return {};
    }
    std::vector<PollNews> poll_news;
    std::vector<PollNews> take_poll_news() override { return std::exchange(poll_news, {}); }
    std::optional<CameraView> camera_view() override { return CameraView{{1, 2, 3}, {0, 0, 1}, 60}; }
    std::string set_camera(std::string_view plugin, const std::array<float, 16>& pose, float fov) override {
        calls.push_back(std::format("camera {} {:.1f} {:.1f} {:.1f} back {:.2f} {:.2f} {:.2f} fov {:.0f}", plugin, pose[12], pose[13], pose[14],
                                    pose[8], pose[9], pose[10], fov));
        return {};
    }
    void release_camera(std::string_view plugin) override { calls.push_back("camera release " + std::string(plugin)); }
    std::string freeze(std::string_view plugin, bool on) override {
        calls.push_back(std::format("freeze {} {}", plugin, on ? "on" : "off"));
        return {};
    }
    Track track() override { return now_playing; }
    std::string music(std::string_view command) override { calls.push_back("music " + std::string(command)); return {}; }
    bool spatialize_music(std::optional<MusicSpatial> spatial) override {
        if (!spatial) calls.push_back("spatial off");
        else calls.push_back(std::format("spatial {:.2f} {:.2f} {:.2f} {:.2f} {} {:.2f} {}", spatial->left, spatial->right, spatial->lpf_left, spatial->lpf_right,
                                         spatial->itd, spatial->env, spatial->mute_loops));
        return spatial.has_value();
    }
    Progress progress() override { return {true, {{"historic", "Historic", 4}}, {{"c1", "line", "historic", 2, 1, 3}}, 1500, 9000, 2}; }
    std::optional<std::string> setting(std::string_view name) override {
        if (std::find(render_settings.begin(), render_settings.end(), name) != render_settings.end()) return "true";
        return name == "Render.Fov" ? std::optional<std::string>("90") : std::nullopt;
    }
    std::vector<std::string> setting_names() override {
        auto names = render_settings;
        names.insert(names.begin(), "Render.Fov");
        return names;
    }
    std::string bind(std::string_view plugin, std::string_view action, std::uint32_t combo) override {
        binds[std::string(plugin) + "/" + std::string(action)] = combo;
        return {};
    }
    std::vector<std::pair<std::string, std::string>> take_presses() override { return std::exchange(presses, {}); }
    void release(std::string_view plugin) override {
        released.emplace_back(plugin);
        score_lines.release(plugin);
        entity_clear(plugin);
        effects_clear(plugin);
        viewmodel_unload(plugin);
        board_material::detail::release(declared_holds, plugin, std::nullopt, std::nullopt);
        std::vector<std::uint64_t> ids;
        for (const auto& [id, owner] : physics_owners) if (owner == plugin) ids.push_back(id);
        for (auto id : ids) (void)object_physics(plugin, id, std::nullopt);
        marker_feed.erase(std::string(plugin));
        if (auto it = ui_models.find(std::string(plugin)); it != ui_models.end())
            for (auto& [name, value] : it->second) { (void)name; value = 0; }
    }
    // The game menu, and a screen capture of something turning once every `turning` frames
    // (reskate.capture, room.turn, menu.game).
    GameMenu menu_now;
    unsigned menu_reads{};
    std::vector<std::string> render_settings; // more settings, each on
    CaptureStatus capture_now{"idle"};
    double turning = 100;
    std::vector<CaptureStart> capture_starts;
    float turned{};
    std::optional<RoomPose> preview;
    std::optional<RoomPose> room_pose(bool skater) override {
        calls.push_back(skater ? "pose skater" : "pose board");
        return preview;
    }
    std::string room_turn(std::string_view, float value, int stick) override {
        if (value != 0 && !menu_now.open) return "the game's menu is not open";
        turned = value;
        calls.push_back(std::format("turn {:.2f} {}", value, stick));
        return {};
    }
    GameMenu game_menu() override { ++menu_reads; return menu_now; }
    MenuState menu_state_now; // what is up over the game (menu.state, menu_changed)
    MenuState menu_state() override { return menu_state_now; }
    std::string capture_start(std::string_view plugin, const CaptureStart& request) override {
        capture_starts.push_back(request);
        capture_now = {"recording"};
        calls.push_back(std::format("capture start {}", plugin));
        return {};
    }
    void capture_stop(std::string_view) override {
        if (capture_now.state == "recording") capture_now.state = "stopped";
    }
    void capture_discard(std::string_view plugin) override {
        capture_now = {"idle"};
        calls.push_back(std::format("capture discard {}", plugin));
    }
    CaptureStatus capture_status(std::string_view) override { return capture_now; }
    float capture_compare(std::string_view, std::uint32_t a, std::uint32_t b) override {
        if (a >= capture_now.taken || b >= capture_now.taken) return -1;
        return 100 * static_cast<float>(std::abs(std::sin(3.14159265358979 * (double(a) - double(b)) / turning)));
    }
    std::vector<std::uint32_t> capture_kept(std::string_view) override {
        std::vector<std::uint32_t> out;
        for (std::uint32_t n = 0; n < capture_now.taken; ++n) out.push_back(n);
        return out;
    }
    std::string capture_save(std::string_view, const CaptureSave&) override {
        capture_now = {"saving", capture_now.taken, capture_now.taken};
        return {};
    }
    std::string files_reveal(std::string_view plugin, std::string_view folder) override {
        calls.push_back(std::format("reveal {} {}", plugin, folder));
        return {};
    }
    // The items worn on top, and what the item grid was asked to show.
    std::vector<std::string> on_top_now;
    static std::string joined(const std::vector<std::string>& list) {
        std::string out;
        for (const auto& one : list) out += (out.empty() ? "" : ",") + one;
        return out;
    }
    std::string outfit_on_top(std::string_view plugin, std::vector<std::string> items) override {
        calls.push_back(std::format("on top {} [{}]", plugin, joined(items)));
        on_top_now = std::move(items);
        return {};
    }
    OnTop outfit_on_top_state(std::string_view) override { return {on_top_now, on_top_now, {}}; }
    std::string grid_show(std::string_view plugin, std::optional<std::vector<std::string>> keys) override {
        calls.push_back(keys ? std::format("show {} [{}]", plugin, joined(*keys)) : std::format("show {} none", plugin));
        return {};
    }
    // Item origins and the player's starred keys, as reskate.mods.item_origins and reskate.menu.favorites read them.
    struct OriginRow {
        std::string key;
        bool from_mod{};
        bool game_item{};
        std::string folder, title, author, version;
    };
    bool origins_ready{};
    std::vector<OriginRow> origin_rows;
    ItemFacts item_facts(const std::vector<std::string>& keys) override {
        ItemFacts out;
        out.ready = origins_ready;
        if (!origins_ready) return out;
        const auto lower = [](std::string_view text) {
            std::string folded(text);
            for (auto& c : folded)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            return folded;
        };
        out.facts.resize(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const auto wanted = lower(keys[i]);
            for (const auto& row : origin_rows)
                if (lower(row.key) == wanted) {
                    out.facts[i] = {row.from_mod, row.game_item, row.folder, row.title, row.author, row.version};
                    break;
                }
        }
        return out;
    }
    bool favorites_ready{};
    std::string favorites_why = "not loaded";
    std::vector<std::string> favorite_keys;
    std::uint64_t favorite_generation{};
    MenuFavorites menu_favorites() override {
        if (!favorites_ready) return {false, favorites_why, {}, favorite_generation};
        return {true, {}, favorite_keys, favorite_generation};
    }
    std::uint64_t favorites_generation() override { return favorite_generation; }
    bool called(std::string_view part) const {
        return std::any_of(calls.begin(), calls.end(), [&](const auto& c) { return c.find(part) != std::string::npos; });
    }
};

struct TestEnvironment final : Environment {
    std::vector<std::string> logs, lines;
    double now = 1000;
    void log(LogLevel, std::string_view text) override { logs.emplace_back(text); }
    void console(std::string_view text) override { lines.emplace_back(text); }
    std::optional<bool> approved(const Candidate&) override { return std::nullopt; }
    double now_ms() override { return now; }
    std::filesystem::path storage_root() override { return scratch / "storage"; }
    bool builtin_command(std::string_view word) override { return word == "perf" || word == "load"; }
    // A skater, when there is one, on flat ground at y = 0.
    std::optional<SkaterState> local_skater;
    bool teleport_forbidden{}, teleport_unavailable{};
    std::vector<std::pair<Vec3, float>> teleports;
    std::vector<std::optional<bool>> teleport_equipment;
    int rays{};
    unsigned skater_reads{};
    std::optional<SkaterState> skater() override { ++skater_reads; return local_skater; }
    std::optional<ScoreState> score_now;
    std::optional<ScoreState> score() override { return score_now; }
    std::vector<std::string> toasts, sounds;
    std::vector<ModInfo> mod_list;
    SessionState session_now;
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<Received> incoming;
    std::vector<plugin_ui::Layer> layers;
    std::vector<plugin_ui::Panel> panels;
    std::vector<plugin_ui::Action> actions;
    bool panels_open{};
    void toast(std::string_view plugin, LogLevel, std::string_view text) override { toasts.push_back(std::string(plugin) + ": " + std::string(text)); }
    bool ui_sound(std::string_view name) override {
        if (name != "select" && name != "cancel" && name != "navigate" && name != "toggle" && name != "tab") return false;
        sounds.emplace_back(name);
        return true;
    }
    std::vector<ModInfo> mods() override { return mod_list; }
    SessionState session() override { return session_now; }
    bool send_message(std::vector<std::uint8_t> bytes) override {
        sent.push_back(std::move(bytes));
        return true;
    }
    std::vector<Received> take_messages() override { return std::exchange(incoming, {}); }
    // A camera at the origin looking down -Z, as the game's matrix has it: rows right, up, back.
    std::optional<Vec3> world_to_screen(const Vec3& p) override {
        if (p[2] >= -0.05f) return std::nullopt;
        const float depth = -p[2];
        return Vec3{960 + p[0] * 1000 / depth, 540 - p[1] * 1000 / depth, depth};
    }
    void publish_layers(std::vector<plugin_ui::Layer> value) override { layers = std::move(value); }
    bool panels_shown() override { return panels_open; }
    std::string shown;
    std::string shown_page() override { return shown; }
    std::vector<plugin_ui::PageEntry> pages;
    void publish_pages(std::vector<plugin_ui::PageEntry> value) override { pages = std::move(value); }
    void publish_panels(std::vector<plugin_ui::Panel> value) override { panels = std::move(value); }
    std::vector<plugin_ui::Action> take_actions() override { return std::exchange(actions, {}); }
    std::vector<HttpRequest> requests;
    std::vector<HttpResponse> responses;
    bool http_request(HttpRequest request) override {
        requests.push_back(std::move(request));
        return true;
    }
    std::vector<HttpResponse> take_http_responses() override { return std::exchange(responses, {}); }
    std::map<std::uint64_t, HttpProgress> http_going;
    std::vector<std::uint64_t> http_cancelled;
    std::optional<HttpProgress> http_progress(std::string_view, std::uint64_t id) override {
        const auto found = http_going.find(id);
        return found == http_going.end() ? std::nullopt : std::optional<HttpProgress>(found->second);
    }
    void http_cancel(std::string_view, std::uint64_t id) override { http_cancelled.push_back(id); }
    FakeGame fake;
    Game& game() override { return fake; }
    Teleport teleport(std::string_view, const Vec3& position, float heading, std::optional<bool> on_board = std::nullopt) override {
        if (teleport_forbidden) return Teleport::forbidden;
        if (teleport_unavailable || !local_skater) return Teleport::unavailable;
        teleports.emplace_back(position, heading);
        teleport_equipment.push_back(on_board);
        return Teleport::sent;
    }
    std::optional<Vec3> raycast(const Vec3& from, const Vec3& to) override {
        ++rays;
        if ((from[1] > 0) == (to[1] > 0)) return std::nullopt;
        const float f = from[1] / (from[1] - to[1]);
        return Vec3{from[0] + (to[0] - from[0]) * f, 0, from[2] + (to[2] - from[2]) * f};
    }
    bool logged(std::string_view part) const {
        return std::any_of(logs.begin(), logs.end(), [&](const auto& line) { return line.find(part) != std::string::npos; });
    }
    bool printed(std::string_view part) const {
        return std::any_of(lines.begin(), lines.end(), [&](const auto& line) { return line.find(part) != std::string::npos; });
    }
    std::string all() const {
        std::string text;
        for (const auto& line : logs) text += line + "\n";
        return text;
    }
};

std::string manifest(std::string permissions = "", std::string side = "client") {
    return R"({"api": 1, "entry": "plugin/main.lua", "side": ")" + side + R"(", "permissions": [)" + permissions + "]}";
}

void write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

// A mod folder holding a plugin: its reskate-plugin.json and files.
Candidate package(const std::string& id, const std::map<std::string, std::string>& files, std::string fingerprint = "1") {
    const auto directory = scratch / "mods" / id;
    std::filesystem::remove_all(directory);
    for (const auto& [name, text] : files) write(directory / name, text);
    return {id, directory, std::move(fingerprint), {}};
}
Candidate script(const std::string& id, const std::string& main, const std::string& permissions = "") {
    return package(id, {{manifest_file, manifest(permissions)}, {"plugin/main.lua", main}});
}

void running(const Host& host, std::string_view id, const TestEnvironment& environment);

void the_time_of_day_and_world_controls_are_read() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("melt", R"(
        assert(reskate.world.time() == "afternoon")
        assert(reskate.world.controls_text() == "fog=1\nfog_distance=0.5\n")
        assert(reskate.world.set_atmosphere({["Fog.Enable"] = 1, ["Fog.FogColor"] = "0,1450,2500"}))
        assert(not pcall(reskate.world.set_atmosphere, {["Fog.Enable"] = true}), "a value is a number or text")
    )", R"("world.environment")")});
    running(host, "melt", environment);
    TestEnvironment denied_environment;
    Host denied(denied_environment, Side::client);
    denied.apply({script("melt-denied", R"(
        assert(not pcall(reskate.world.time), "needs world.environment")
        assert(not pcall(reskate.world.controls_text), "needs world.environment")
        assert(not pcall(reskate.world.set_atmosphere, {["Fog.Enable"] = 0}), "needs world.environment")
    )")});
    running(denied, "melt-denied", denied_environment);
    check(denied_environment.fake.atmosphere_set.empty(), "a plugin without world.environment set no atmosphere");
}

void the_skater_surface_sound_is_set_for_some_fields() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("surface-sound", R"(
        assert(reskate.skater.surface_sound(0xd60, {1, 3}))
        assert(not pcall(reskate.skater.surface_sound, 0xd60, {19}), "fields are 1 to 18")
        assert(not pcall(reskate.skater.surface_sound, 0.5), "a fraction is no surface")
        assert(not pcall(reskate.skater.surface_sound, -1), "a negative is no surface")
        assert(not pcall(reskate.skater.surface_sound, 116 * 64), "material slot 116 is past the game's grid")
        assert(not pcall(reskate.skater.surface_sound, 116 * 524288), "property slot 116 is past the game's grid")
        assert(not pcall(reskate.skater.surface_sound, 0xffffffff), "a word of every bit names no surface")
        assert(reskate.skater.surface_sound(115 * 64 + 115 * 524288), "the grid's last slots are surfaces")
        assert(reskate.skater.surface_sound(0xd60, {1, 3}))
    )", R"("skater.control")")});
    running(host, "surface-sound", environment);
    check(environment.fake.surface_set == 0xd60u && environment.fake.surface_fields == 0b101u,
          "surface_sound passes the surface word and its fields on");
    TestEnvironment denied_environment;
    Host denied(denied_environment, Side::client);
    denied.apply({script("surface-denied", R"(
        assert(not pcall(reskate.skater.surface_sound, 7), "needs skater.control")
    )")});
    running(denied, "surface-denied", denied_environment);
    check(!denied_environment.fake.surface_set, "a plugin without skater.control set no surface sound");
}

void a_variant_can_wait_for_the_next_launch() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("variant-chooser", R"(
        assert(reskate.package.choose("snowskate"))
        assert(reskate.package.choose("standard", {now = false}))
        assert(reskate.package.choose("standard", {now = true}))
        assert(not pcall(reskate.package.choose, "standard", 7), "options are a table")
    )", R"("variants")")});
    running(host, "variant-chooser", environment);
    const std::vector<std::pair<std::string, bool>> expected{{"snowskate", true}, {"standard", false}, {"standard", true}};
    check(environment.fake.chosen_variants == expected, "choose keeps a variant for the next launch only when now is false");
    // Without the permission it is refused, for the next launch as well as now.
    TestEnvironment denied_environment;
    Host denied(denied_environment, Side::client);
    denied.apply({script("variant-denied", R"(
        assert(not pcall(reskate.package.choose, "standard", {now = false}), "needs variants")
        assert(not pcall(reskate.package.choose, "standard"), "needs variants")
    )")});
    running(denied, "variant-denied", denied_environment);
    check(denied_environment.fake.chosen_variants.empty(), "a plugin without variants chose no variant");
}

void a_plugin_holds_a_trainer_preset() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("preset-holder", R"(
        assert(reskate.trainer.hold_preset("SnowSkate", {["PhysicsMode.JumpMaxHeight"] = 1.29, ["trick.gravity"] = 1.15}))
        assert(reskate.trainer.hold_preset(nil))
        assert(not pcall(reskate.trainer.hold_preset, "", {a = 1}), "a name is needed")
        assert(not pcall(reskate.trainer.hold_preset, "x", {}), "a value is needed")
        assert(not pcall(reskate.trainer.hold_preset, "x", {a = "high"}), "values are numbers")
    )", R"("trainer")")});
    running(host, "preset-holder", environment);
    const auto& held = environment.fake.trainer_presets;
    check(held.size() == 2 && held[0] && held[0]->first == "SnowSkate" && held[0]->second.size() == 2 &&
              held[0]->second.contains("physicsmode.jumpmaxheight") && held[0]->second.at("trick.gravity") == 1.15 && !held[1],
          "a preset is held with its values (ids in lower case), then let go");

    // Without the permission it is refused.
    TestEnvironment denied_environment;
    Host denied(denied_environment, Side::client);
    denied.apply({script("preset-denied", R"(
        local ok, why = pcall(reskate.trainer.hold_preset, "SnowSkate", {a = 1})
        assert(not ok and why:find("trainer", 1, true), tostring(why))
    )")});
    running(denied, "preset-denied", denied_environment);
    check(denied_environment.fake.trainer_presets.empty(), "a plugin without trainer held a trainer preset");
}

void traffic_vehicles_are_permissioned_bounded_and_released() {
    using traffic_vehicles::Quaternion;
    Quaternion zero{};
    check(!traffic_vehicles::normalize(zero), "a zero quaternion was accepted");
    Quaternion double_identity{0, 0, 0, 2};
    check(traffic_vehicles::normalize(double_identity) && double_identity[3] == 1,
          "a finite quaternion was not normalized");
    check(traffic_vehicles::parent_candidate(11, 12, {0, 0, 0}, {0, 0, 9}, 1) &&
          !traffic_vehicles::parent_candidate(11, 12, {0, 0, 0}, {0, 0, 11}, 1) &&
          !traffic_vehicles::parent_candidate(11, 12, {0, 0, 0}, {0, 0, 1}, 4),
          "parent climb exceeded its distance or depth bounds");
    check(traffic_vehicles::agent_candidate(0x310000000100, 0x930200000100, {0, 0, 0}, {0.2f, 0, 0}) &&
          !traffic_vehicles::agent_candidate(0x310000000100, 0x310000000200, {0, 0, 0}, {0.2f, 0, 0}) &&
          !traffic_vehicles::agent_candidate(0x310000000100, 0x930200000100, {0, 0, 0}, {2, 0, 0}),
          "agent choice ignored its id range or proximity");

    TestEnvironment denied_environment; Host denied(denied_environment, Side::client);
    denied_environment.fake.vehicle_pick_owner = 0x310000000100;
    denied.apply({script("vehicle-denied", R"(
        local v=vector.create
        local ok,why=pcall(reskate.world.vehicles.take,v(0,0,0),v(0,0,2))
        assert(not ok and why:find('world.vehicles',1,true),tostring(why))
    )")});
    running(denied, "vehicle-denied", denied_environment);
    check(!denied_environment.fake.vehicle_takes && !denied_environment.fake.world_picks,
          "a plugin without world.vehicles reached the world query");

    TestEnvironment environment; Host host(environment, Side::client);
    auto& game = environment.fake;
    game.vehicle_pick_owner = 0x310000000100;
    host.apply({script("vehicle-holder", R"(
        local v=vector.create
        local V=reskate.world.vehicles
        assert(not pcall(V.take,v(0,0,0),v(1001,0,0)),"long ray")
        assert(not pcall(V.take,v(math.huge,0,0),v(0,0,0)),"nonfinite ray")
        local none,why=V.take(v(0,51,0),v(0,52,0))
        assert(none==nil and why=="nothing hit",tostring(why))
        local car=assert(V.take(v(0,0,0),v(0,0,2)))
        assert(car:state()=="finding" and car:state()=="held")
        local position,rotation=car:pose()
        assert(position and rotation[4]==1)
        assert(not pcall(car.set_pose,car,v(1,2,3),{0,0,0,0}),"zero rotation")
        assert(not pcall(car.set_pose,car,v(1,2,3),{0,0,0,math.huge}),"nonfinite rotation")
        assert(car:set_pose(v(4,5,6),{0,0,0,2}))
        position,rotation=car:pose()
        assert(position.x==4 and rotation[4]==1,"pose reached game normalized")
        assert(car:release() and car:state()=="released")
        for i=1,4 do assert(V.take(v(0,0,0),v(0,0,2))) end
        none,why=V.take(v(0,0,0),v(0,0,2))
        assert(none==nil and why=="too many vehicles",tostring(why))
    )", R"("world.vehicles")")});
    running(host, "vehicle-holder", environment);
    check(game.vehicle_takes == 6 && game.vehicle_releases == 1 && game.vehicles.size() == 5 &&
          game.vehicles.at(1).pose.position == Vec3{4, 5, 6},
          "vehicle pose, release or four-vehicle limit did not reach the fake game");
    host.apply({});
    check(game.vehicle_release_alls >= 1 && std::ranges::all_of(game.vehicles, [](const auto& item) {
              return item.second.state == traffic_vehicles::State::released;
          }), "unloading the plugin left vehicles held");

    TestEnvironment failed_environment; Host failed(failed_environment, Side::client);
    failed_environment.fake.vehicle_pick_owner = 0x310000000100;
    failed_environment.fake.vehicle_take_refusal = "layout differs";
    failed.apply({script("vehicle-refused", R"(
        local v=vector.create
        local car,why=reskate.world.vehicles.take(v(0,0,0),v(0,0,2))
        assert(car==nil and why=="layout differs",tostring(why))
    )", R"("world.vehicles")")});
    running(failed, "vehicle-refused", failed_environment);
}

Status status_of(const Host& host, std::string_view id) {
    for (const auto& status : host.status()) if (status.id == id) return status;
    throw std::runtime_error("no plugin " + std::string(id));
}
void ticks(Host& host, TestEnvironment& environment, int count) {
    for (int i = 0; i < count; ++i) {
        environment.now += 16.7;
        host.tick(16.7);
    }
}
void running(const Host& host, std::string_view id, const TestEnvironment& environment) {
    const auto status = status_of(host, id);
    check(status.state == "running", std::string(id) + " should run, and is " + status.state + ": " + status.reason + "\n" + environment.all());
}
void stopped(const Host& host, std::string_view id, std::string_view because) {
    const auto status = status_of(host, id);
    check(status.state == "stopped", std::string(id) + " should have stopped, and is " + status.state);
    check(status.reason.find(because) != std::string::npos, std::string(id) + " stopped for \"" + status.reason + "\", not for " + std::string(because));
}

void effects_require_resident_roots() {
    auto declaration = dingosdk::Json::parse(manifest(R"("effects")"));
    const std::string particle = "effects/reskate/test/ebp_spark";
    declaration["effects"] = dingosdk::Json::array({particle});
    const auto refused = [&](const dingosdk::Json& value) {
        try { (void)parse_manifest(value.dump()); } catch (const std::exception&) { return; }
        throw std::runtime_error("An effect without valid resident coverage was accepted");
    };
    refused(declaration);
    for (const auto* bundle : {"win32/levels/test/sublevel", "win32/levels/test/root",
                               "win32/levels/game/dingolevel_root/dingolevel_root_extra"}) {
        declaration["resident"] = dingosdk::Json::array({dingosdk::Json{
            {"bundle", bundle}, {"roots", dingosdk::Json::array({"effects/reskate/test/"})}}});
        refused(declaration);
    }
    for (const auto* roots : {R"([])", R"(["effects/reskate/test"])", R"(["effects/reskate/tes/"])",
                             R"(["effects/reskate/test/ebp_spark_extra"])"}) {
        declaration["resident"] = dingosdk::Json::array({dingosdk::Json{
            {"bundle", "win32/levels/game/dingolevel_root/dingolevel_root"}, {"roots", dingosdk::Json::parse(roots)}}});
        refused(declaration);
    }
    for (const auto* root : {"effects/reskate/test/", "EFFECTS/RESKATE/TEST/", "effects/reskate/test/ebp_spark"}) {
        declaration["resident"] = dingosdk::Json::array({dingosdk::Json{
            {"bundle", "win32/levels/game/dingolevel_root/dingolevel_root"}, {"roots", dingosdk::Json::array({root})}}});
        check(parse_manifest(declaration.dump()).effects.contains(particle), "Exact and directory roots cover an effect");
    }
    declaration["resident"][0]["bundle"] = "WIN32/LEVELS/GAME/DINGOLEVEL_ROOT/DINGOLEVEL_ROOT";
    check(parse_manifest(declaration.dump()).effects.contains(particle), "Resident bundle names are normalized consistently with the merge");
    declaration["resident"] = dingosdk::Json::parse(R"([
        {"bundle":"win32/levels/game/dingolevel_root/dingolevel_root","roots":["effects/reskate/other/"]},
        {"bundle":"win32/levels/test/sublevel","roots":["effects/reskate/test/"]}
    ])");
    refused(declaration); // Coverage must be in the same row as the supported bundle.
    // The merge discards the entire resident list when any row or shader is malformed.
    for (const auto* bad : {R"([{"roots":["effects/reskate/test/"]}])",
                           R"([{"bundle":"win32/levels/game/dingolevel_root/dingolevel_root","roots":["effects/reskate/test/"],"shaders":[{"name":"depot","roots":[]}]}])",
                           R"([{"bundle":"win32/levels/game/dingolevel_root/dingolevel_root","roots":["effects/reskate/test/"]},{"bundle":7}])"}) {
        declaration["resident"] = dingosdk::Json::parse(bad);
        refused(declaration);
    }
    bool denied = false;
    try { (void)parse_manifest(manifest(R"("events.physics.geometry")")); }
    catch (const std::exception&) { denied = true; }
    check(denied, "Geometry does not bypass the base physics permission");
    denied = false;
    try { (void)parse_manifest(manifest(R"("world.physics.reuse")")); }
    catch (const std::exception&) { denied = true; }
    check(denied, "Body reuse does not come without the world physics permission");
}

void expired_effects_return_their_quota() {
    TestEnvironment environment;
    Limits limits;
    limits.call_ms = 1000;
    Host host(environment, Side::client, limits);
    auto declaration = dingosdk::Json::parse(manifest(R"("effects", "console")"));
    declaration["effects"] = dingosdk::Json::array({"effects/reskate/test/ebp_spark", "world/materials/decal/reskate_test/stain_dv"});
    declaration["resident"] = dingosdk::Json::parse(R"([{"bundle":"win32/levels/game/dingolevel_root/dingolevel_root", "roots":["effects/reskate/test/", "world/materials/decal/reskate_test/"]}])");
    host.apply({package("effects-expire", {{manifest_file, declaration.dump()}, {"plugin/main.lua", R"(
        local fx = reskate.effects
        local m = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}
        reskate.console.command("fxfill", "", function()
            for i=1,64 do assert(fx.particle("effects/reskate/test/ebp_spark", m)) end
            for i=1,2048 do assert(fx.decal("world/materials/decal/reskate_test/stain_dv", m)) end
            assert(fx.particle("effects/reskate/test/ebp_spark", m) == nil)
            assert(fx.decal("world/materials/decal/reskate_test/stain_dv", m) == nil)
        end)
    )"}})});
    auto& game = environment.fake;
    check(host.run_console("fxfill") && game.effects.size() == 2112, "Both effect quotas can be filled");
    game.effects.clear(); // Ten-minute native expiry, with no change to the world epoch.
    ticks(host, environment, 1);
    check(host.run_console("fxfill") && game.effects.size() == 2112, "Expiry returns both quotas without Lua release/clear");
    ticks(host, environment, 1);
    game.effects.clear();
    ++game.effect_epoch;
    // A console call can arrive after the native reset but before Plugin::tick.
    check(host.run_console("fxfill") && game.effects.size() == 2112, "A reset returns quota even before the next VM tick");
    running(host, "effects-expire", environment);
}

void game_effects_need_their_own_permission() {
    // effects.game cannot stand alone, and is a known permission.
    bool refused = false;
    try { (void)parse_manifest(manifest(R"("effects.game")")); } catch (const std::exception&) { refused = true; }
    check(refused, "effects.game without effects is refused");
    check(parse_manifest(manifest(R"("effects", "effects.game")")).permissions.contains("effects.game"), "effects.game is a permission");
    check(game_effect_asset("effects/environments/effectblueprints/ebp_env_thick_smoke_popup") &&
          !game_effect_asset("effects/reskate/test/ebp_spark") && !game_effect_asset("world/materials/decal/reskate_test/stain_dv") &&
          !game_effect_asset("/effects/a") && !game_effect_asset("effects//a") && !game_effect_asset("Effects/A") && !game_effect_asset(""),
          "A game effect's name is a plain asset path that is not a mod's own");

    TestEnvironment environment;
    Host host(environment, Side::client);
    // Without effects.game, a plugin with effects still cannot name one of the game's.
    host.apply({package("fx-private-only", {{manifest_file, manifest(R"("effects", "console")")}, {"plugin/main.lua", R"(
        local m = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}
        reskate.console.command("fxstock", "", function()
            local ok, why = pcall(reskate.effects.particle, "effects/environments/effectblueprints/ebp_env_thick_smoke_popup", m)
            assert(not ok and why:find("effects.game"), tostring(why))
            ok, why = pcall(reskate.effects.load, "win32/effects/gestures/effectblueprints/ebp_x_bundle", "effects/gestures/effectblueprints/ebp_x")
            assert(not ok and why:find("effects.game"), tostring(why))
        end)
    )"}})});
    check(host.run_console("fxstock") && environment.fake.effects.empty() && environment.fake.effect_bundles.empty(),
          "A plugin without effects.game cannot play the game's effects or ask for its bundles");

    // With it, the game's particle effects play; decals and the plugin's own undeclared names do not.
    host.apply({package("fx-game", {{manifest_file, manifest(R"("effects", "effects.game", "console")")}, {"plugin/main.lua", R"(
        local m = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}
        reskate.console.command("fxgame", "", function()
            assert(reskate.effects.particle("effects/environments/effectblueprints/ebp_env_thick_smoke_popup", m))
            assert(not pcall(reskate.effects.decal, "world/materials/decal/some_game_decal", m))
            assert(not pcall(reskate.effects.particle, "effects/reskate/test/undeclared", m))
            assert(not pcall(reskate.effects.particle, "Effects/Upper", m))
            -- One of the game's effect bundles can be asked for; only those under win32/effects/.
            local bundle = "win32/effects/gestures/effectblueprints/ebp_gesture_metaldance_flameburst_bundle"
            local effect = "effects/gestures/effectblueprints/ebp_gesture_metaldance_flameburst"
            assert(reskate.effects.load(bundle, effect) == "loading")
            assert(reskate.effects.load(bundle, effect) == "ready")
            assert(not pcall(reskate.effects.load, "win32/levels/game/bam_levelroot/bam_levelroot", effect))
            assert(not pcall(reskate.effects.load, bundle, "effects/reskate/test/ebp_spark"))
        end)
    )"}})});
    check(host.run_console("fxgame") && environment.fake.effects.size() == 1 &&
          environment.fake.effects.begin()->second.request.game &&
          environment.fake.effects.begin()->second.request.asset == "effects/environments/effectblueprints/ebp_env_thick_smoke_popup",
          "A plugin with effects.game plays one of the game's particle effects, marked as the game's");
    check(environment.fake.effect_bundles.size() == 2 &&
          environment.fake.effect_bundles.front().first == "win32/effects/gestures/effectblueprints/ebp_gesture_metaldance_flameburst_bundle",
          "It can ask for one of the game's effect bundles, and nothing outside win32/effects/ reaches the game");
}

void plugins_render_private_effects() {
    TestEnvironment environment;
    // Filling every decal slot tests the resource quota, with room for a busy CI machine.
    Limits limits;
    limits.call_ms = 1000;
    Host host(environment, Side::client, limits);
    host.apply({script("effects-denied", R"(
        for _,f in pairs(reskate.effects) do
            local ok,why=pcall(f)
            assert(not ok and why:find("permission"), tostring(why))
        end
        reskate.log("all effects denied")
    )")});
    running(host, "effects-denied", environment);
    check(environment.logged("all effects denied"), environment.all());
    check(environment.fake.effects.empty(), "A plugin without permission cannot reach the renderer");
    auto declaration = dingosdk::Json::parse(manifest(R"("effects", "console")"));
    const std::string particle = "effects/reskate/test/ebp_spark";
    const std::string material = "world/materials/decal/reskate_test/stain_dv";
    declaration["effects"] = dingosdk::Json::array({particle, material});
    declaration["resident"] = dingosdk::Json::parse(R"([{"bundle":"win32/levels/game/dingolevel_root/dingolevel_root", "roots":["effects/reskate/test/", "world/materials/decal/reskate_test/"]}])");
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        auto invalid = declaration;
        if (scenario == 0) invalid["permissions"] = dingosdk::Json::array();
        else if (scenario == 1) invalid["effects"] = particle;
        else {
            auto names = dingosdk::Json::array();
            for (unsigned i = 0; i < 129; ++i) names.push_back(particle + std::to_string(i));
            invalid["effects"] = std::move(names);
        }
        bool refused = false;
        try { (void)parse_manifest(invalid.dump()); } catch (const std::exception&) { refused = true; }
        check(refused, "An effect allowlist needs permission, an array and at most 128 names");
    }
    for (const auto* forbidden : {"effects/gestures/effectblueprints/ebp_gesture_applause", "effects/reskate/../store/item", "world/materials/decal/paid"}) {
        auto invalid = declaration;
        invalid["effects"] = dingosdk::Json::array({forbidden});
        bool refused = false;
        try { (void)parse_manifest(invalid.dump()); } catch (const std::exception&) { refused = true; }
        check(refused, "Stock, store and path escapes are refused in effects manifests");
    }
    const auto main = R"(
        local fx=reskate.effects
        local m={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}
        local p="effects/reskate/test/ebp_spark"
        local d="world/materials/decal/reskate_test/stain_dv"
        assert(not pcall(fx.particle,"effects/reskate/test/not_declared",m))
        assert(not pcall(fx.particle,"effects/gestures/effectblueprints/ebp_gesture_applause",m))
        assert(not pcall(fx.decal,p,m))
        local broken=table.clone(m); broken[1]=0/0
        assert(not pcall(fx.particle,p,broken))
        broken=table.clone(m); broken[4]=1
        assert(not pcall(fx.particle,p,broken))
        broken=table.clone(m); broken[1]=0
        assert(not pcall(fx.particle,p,broken))
        assert(not pcall(fx.decal,d,m,{lift=2}))
        for i=1,64 do assert(fx.particle(p,m)) end
        assert(fx.particle(p,m)==nil,"particle quota")
        for i=1,2048 do assert(fx.decal(d,m,{attach=true,lift=.04})) end
        assert(fx.decal(d,m)==nil,"decal quota")
        fx.clear()
        local id=assert(fx.particle(p,m))
        m[13]=3; fx.move(id,m); fx.stop(id)
        assert(not pcall(fx.opacity,id,-1))
        local decal=assert(fx.decal(d,m,{attach=true,lift=.04})); fx.opacity(decal,.5)
        reskate.console.command("fxgone","",function() fx.release(id); fx.release(decal) end)
        reskate.console.command("fxfail","",function() error("intentional") end)
    )";
    host.apply({package("effects-owner", {{manifest_file, declaration.dump()}, {"plugin/main.lua", main}})});
    running(host, "effects-owner", environment);
    auto& game = environment.fake;
    check(game.effects.size() == 2, "Quota objects are cleared before the two live effects");
    for (const auto& [id, e] : game.effects) {
        check(e.request.transform[12] == 3, "Effects move without native addresses");
        check(e.request.decal ? e.alpha == .5f && e.request.attach : e.stopped, "Stop, opacity and attachment reach the game");
    }
    const auto owned_id = game.effects.begin()->first;
    auto intruder = R"(assert(not pcall(reskate.effects.release, )" + std::to_string(owned_id) + R"()); reskate.log("foreign effect denied"))";
    host.apply({{ "effects-owner", scratch / "mods" / "effects-owner", "1", {} },
                package("effects-intruder", {{manifest_file, declaration.dump()}, {"plugin/main.lua", intruder}})});
    running(host, "effects-intruder", environment);
    check(environment.logged("foreign effect denied") && game.effects.size() == 2, "Handles never cross plugin ownership");
    check(host.run_console("fxgone") && game.effects.empty(), "Release disposes both owned references");
    host.reload("effects-owner");
    running(host, "effects-owner", environment);
    check(game.effects.size() == 2, "Reload starts from a clean owner");
    for (int i = 0; i < 6; ++i) (void)host.run_console("fxfail");
    stopped(host, "effects-owner", "errors");
    check(game.effects.empty(), "Stopping a failing VM releases its effects");
}

void failed_plugin_startup_releases_effects() {
    for (const auto side : {Side::client, Side::server}) {
        TestEnvironment environment;
        const std::string suffix = side == Side::server ? "@server" : "";
        const std::string owner = "effect-startup" + suffix;
        Host host(environment, side, {}, suffix);
        auto declaration = dingosdk::Json::parse(manifest(R"("effects", "console")", "both"));
        declaration["effects"] = dingosdk::Json::array({"effects/reskate/test/ebp_spark", "world/materials/decal/reskate_test/stain_dv"});
        declaration["resident"] = dingosdk::Json::parse(R"([{"bundle":"win32/levels/game/dingolevel_root/dingolevel_root", "roots":["effects/reskate/test/", "world/materials/decal/reskate_test/"]}])");
        // The other copy of this same plugin keeps its effects when this one fails.
        auto& game = environment.fake;
        const auto other = game.effect_create(side == Side::server ? "effect-startup" : "effect-startup@server", {});
        const std::string main = R"(
            local m = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}
            assert(reskate.effects.particle("effects/reskate/test/ebp_spark", m))
            assert(reskate.effects.decal("world/materials/decal/reskate_test/stain_dv", m))
            reskate.console.command("fxstartup", "", function() end)
        )";
        host.apply({package("effect-startup", {{manifest_file, declaration.dump()},
            {"plugin/main.lua", main + R"(error("intentional effect startup failure"))"}})});
        stopped(host, "effect-startup", "error");
        check(environment.logged("intentional effect startup failure") && game.effect_serial == 3,
              "Startup reached the failure after creating both effects\n" + environment.all());
        check(game.effects.size() == 1 && game.effects.contains(other), "Failed startup releases only its own effects immediately");
        check(game.released == std::vector<std::string>{owner}, "Startup failure releases the full owner id, including its server suffix");
        ticks(host, environment, 2);
        check(game.effects.size() == 1 && game.effects.contains(other), "Later ticks preserve the other copy's effects");
        (void)host.switch_on(owner);
        stopped(host, "effect-startup", "error");
        check(game.effect_serial == 5 && game.effects.size() == 1 && game.effects.contains(other),
              "Repeated startup failures do not accumulate effects");
        write(scratch / "mods/effect-startup/plugin/main.lua", main);
        (void)host.switch_on(owner);
        running(host, "effect-startup", environment);
        check(host.run_console("fxstartup") && game.effects.size() == 3, "A repaired plugin can reclaim its command and create new effects");
        host.shutdown();
        check(game.effects.size() == 1 && game.effects.contains(other), "Shutdown still releases only this copy's effects");
    }
}

void plugins_track_effect_lifetimes() {
    TestEnvironment environment;
    Limits limits;
    limits.call_ms = 1000; // Exercise all 8192 operations independently of the timing tests.
    Host host(environment, Side::client, limits);
    auto declaration = dingosdk::Json::parse(manifest(R"("effects", "console")"));
    declaration["effects"] = dingosdk::Json::array({"effects/reskate/test/ebp_spark"});
    declaration["resident"] = dingosdk::Json::parse(R"([{"bundle":"win32/levels/game/dingolevel_root/dingolevel_root", "roots":["effects/reskate/test/", "world/materials/decal/reskate_test/"]}])");
    host.apply({package("effect-lifetime", {{manifest_file, declaration.dump()}, {"plugin/main.lua", R"(
        local fx = reskate.effects
        local asset = "effects/reskate/test/ebp_spark"
        local transform = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}
        local ready, epoch = fx.state()
        assert(ready)
        local expired = assert(fx.particle(asset, transform))
        reskate.console.command("fxmissing", "", function()
            local available, current = fx.state()
            assert(not available and current ~= epoch)
            local id, why = fx.particle(asset, transform)
            assert(id == nil and why:find("unavailable"))
            fx.release(expired)
            assert(not pcall(fx.release, expired), "a released handle cannot be reused")
            fx.clear(); fx.clear()
            reskate.log("expired effects cleared")
        end)
        reskate.console.command("fxbudget", "", function()
            for i = 1, 8192 do fx.state() end
            local ok, why = pcall(fx.state)
            assert(not ok and why:find("8192"))
            reskate.log("effect call limit enforced")
        end)
        reskate.console.command("fxnew", "", function()
            assert(fx.state())
            local id = assert(fx.particle(asset, transform))
            assert(id ~= expired, "a new world never reuses a stale handle")
            reskate.log("effect created after reset")
        end)
    )"}})});
    running(host, "effect-lifetime", environment);
    auto& game = environment.fake;
    check(game.effects.size() == 1, "An effect belongs to the running plugin");
    game.effects_ready = false;
    ++game.effect_epoch;
    game.effects.clear(); // Native world teardown precedes the plugin's next callback.
    check(host.run_console("fxmissing") && environment.logged("expired effects cleared"), environment.all());
    game.effects_ready = true;
    ticks(host, environment, 1);
    check(host.run_console("fxbudget") && environment.logged("effect call limit enforced"), environment.all());
    ticks(host, environment, 1);
    check(host.run_console("fxnew") && environment.logged("effect created after reset") && game.effects.size() == 1,
          "The next tick restores the operation allowance\n" + environment.all());
    host.reload("effect-lifetime");
    running(host, "effect-lifetime", environment);
    check(game.effects.size() == 1, "Reload releases the previous VM's native effects");
    host.apply({});
    check(game.effects.empty(), "Removing a plugin releases all its effects");
}

void physics_steps_include_optional_geometry() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("physics-geometry", R"(
        local samples = 0
        reskate.on("physics_step", function(step)
            samples += 1
            local body = step.bodies["Left forearm"]
            if samples == 1 then
                assert(step.at == 1000 and math.abs(step.seconds - .05) < .000001)
                assert(step.state == 300 and step.wipeout and step.ragdoll and not step.airborne)
                assert(body.impact == 12 and body.slide == 5 and body.world and not body.board)
                assert(body.point[1] == 2 and body.point[2] == 0 and body.point[3] == 4)
                assert(body.normal[1] == 0 and body.normal[2] == 1 and body.normal[3] == 0)
                local expected = {1,0,0,0,0,1,0,0,0,0,1,0,2,.25,4,1}
                for _, name in {"Pelvis", "Left forearm"} do
                    local pose = step.poses[name]
                    assert(#pose.transform == 16 and #pose.velocity == 3)
                    for i, value in expected do assert(pose.transform[i] == value) end
                    assert(pose.velocity[1] == 5 and pose.velocity[2] == -10 and pose.velocity[3] == 0)
                end
                assert(step.bodies.Pelvis == nil, "a body can have a pose without a contact")
                reskate.log("contact geometry received")
            else
                assert(step.state == nil and step.ragdoll == nil)
                assert(body.point == nil and body.normal == nil and next(step.poses) == nil)
                reskate.log("missing geometry stays absent")
            end
        end)
    )", R"("events.physics", "events.physics.geometry")"), script("physics-basic", R"(
        reskate.on("physics_step", function(step)
            assert(step.poses == nil and step.state == nil)
            local body = step.bodies["Left forearm"]
            assert(body and body.impact == 12 and body.point == nil and body.normal == nil)
            reskate.log("basic physics has no geometry")
        end)
    )", R"("events.physics")"), script("physics-denied", R"(
        local ok, why = pcall(reskate.on, "physics_step", function() error("not permitted") end)
        assert(not ok and why:find("events.physics"))
        reskate.log("physics permission enforced")
    )")});
    running(host, "physics-geometry", environment);
    running(host, "physics-denied", environment);
    check(environment.logged("physics permission enforced"), environment.all());
    PhysicsStep value;
    value.at = environment.now; value.seconds = .05f; value.state = 300;
    value.wipeout = true; value.ragdoll = true;
    const EffectMatrix matrix{1,0,0,0,0,1,0,0,0,0,1,0,2,.25f,4,1};
    value.poses.emplace("Pelvis", PhysicsStep::Pose{matrix, {5, -10, 0}});
    value.poses.emplace("Left forearm", PhysicsStep::Pose{matrix, {5, -10, 0}});
    value.bodies.push_back({"Left forearm", 12, 5, false, false, true, Vec3{2,0,4}, Vec3{0,1,0}});
    environment.fake.steps.push_back(value);
    ticks(host, environment, 1);
    check(environment.logged("contact geometry received") && environment.logged("basic physics has no geometry"), environment.all());
    value.state.reset(); value.ragdoll.reset(); value.poses.clear();
    value.bodies[0].point.reset(); value.bodies[0].normal.reset();
    environment.fake.steps.push_back(value);
    ticks(host, environment, 1);
    check(environment.logged("missing geometry stays absent"), environment.all());
}

void plugin_approval_follows_the_plugins_own_files() {
    const auto mod = scratch / "approval-mod";
    std::filesystem::remove_all(mod);
    write(mod / manifest_file, manifest(R"("settings.write")"));
    write(mod / "plugin" / "main.lua", "print('a')");
    write(mod / "clothes" / "shirt.dat", "cloth");
    const auto first = plugin_fingerprint(mod);
    write(mod / "clothes" / "shirt.dat", "a different shirt");
    write(mod / "clothes" / "new.dat", "more");
    check(plugin_fingerprint(mod) == first, "A cosmetic asset of the mod does not change the plugin's fingerprint");
    write(mod / "plugin" / "main.lua", "print('b')");
    const auto second = plugin_fingerprint(mod);
    check(second != first, "A change to a plugin file does");
    write(mod / "plugin" / "extra.lua", "return 1");
    check(plugin_fingerprint(mod) != second, "and so does a new file in the plugin's folder");
    write(mod / manifest_file, manifest(R"("settings.write", "console")"));
    check(plugin_fingerprint(mod) != second, "and its manifest");

    // Bytes moved between the end of one file and the next file's name must not give the same print.
    const auto split = scratch / "approval-split", merged = scratch / "approval-merged";
    for (const auto& folder : {split, merged}) {
        std::filesystem::remove_all(folder);
        write(folder / manifest_file, manifest());
    }
    // As the first version of the print laid it out: a file's name in UTF-16, then "|", then its bytes.
    const auto named = [](std::string_view name) {
        std::string bytes;
        for (const char c : name) bytes += {c, '\0'};
        return bytes + '|';
    };
    write(split / "plugin" / "a.dat", "X" + named("plugin/b.lua") + "EVIL");
    write(split / "plugin" / "c.lua", "C");
    write(split / "plugin" / "d.lua", "D");
    write(merged / "plugin" / "a.dat", "X");
    write(merged / "plugin" / "b.lua", "EVIL");
    write(merged / "plugin" / "c.lua", "C" + named("plugin/d.lua") + "D");
    check(plugin_fingerprint(split) != plugin_fingerprint(merged), "Moving bytes between a file and the next one's name changes the print");
    check(plugin_fingerprint(mod).size() > 60, "The print is a SHA-256");

    const auto file = scratch / "approvals" / "plugin_approvals.json";
    std::filesystem::remove_all(file.parent_path());
    {
        Approvals approvals(file);
        check(approvals.state("pack", "1-a") == Approval::asking, "A plugin nobody answered for asks");
        approvals.set("pack", "1-a", true);
        approvals.set("other", "2-b", false);
        check(approvals.state("pack", "1-a") == Approval::on && approvals.state("other", "2-b") == Approval::off, "Answers are kept");
        check(approvals.state("pack", "1-changed") == Approval::asking, "An answer is void for other files");
    }
    Approvals again(file);
    check(again.state("pack", "1-a") == Approval::on && again.state("other", "2-b") == Approval::off, "Answers come back from the file");
    check(again.state("pack", "1-changed") == Approval::asking && again.state("unknown", "1-a") == Approval::asking, "and a changed or unknown plugin asks again");
    again.set_developer(true);
    check(again.state("pack", "1-changed") == Approval::on, "In developer mode an approved plugin keeps its approval when its files change");
    check(again.state("other", "3-c") == Approval::off, "and a refusal holds as well");
    check(again.state("unknown", "1-a") == Approval::asking, "but a plugin nobody answered for still asks");
    again.set_developer(false);
    check(again.state("pack", "1-changed") == Approval::on && again.state("pack", "1-a") == Approval::asking,
          "The answer moved to the new files, so the old ones ask again");

    const auto groups = permission_groups(parse_manifest(manifest(R"("net.http", "audio", "files", "audio.ui", "settings.write", "storage")")));
    check(groups.size() == 5 && groups[0].risk == Risk::red && groups[1].risk == Risk::red && groups[2].risk == Risk::amber &&
              groups[3].risk == Risk::grey && groups[4].risk == Risk::grey,
          "Permissions come under a category each, the riskiest first");
    const auto sound = std::find_if(groups.begin(), groups.end(), [](const auto& g) { return g.category == "SOUND"; });
    check(sound != groups.end() && sound->phrases.size() == 2 && sound->risk == Risk::grey, "A category lists each of its permissions once");
    const auto input_groups = permission_groups(parse_manifest(manifest(R"("input", "input.write")")));
    check(input_groups.size() == 1 && input_groups.front().category == "INPUT" &&
          input_groups.front().phrases.size() == 2 && input_groups.front().risk == Risk::amber &&
          approval_prompt(input_groups).find("controller input while you play") != std::string::npos,
          "Controller replacement is disclosed in the player's input approval group");
    const auto write_prompt = approval_prompt(permission_groups(parse_manifest(manifest(R"("input.write")"))));
    check(write_prompt.find("controller input while you play") != std::string::npos &&
          write_prompt.find("bind keys") == std::string::npos,
          "Write-only input permission does not claim controller-reading or bind access");
    const auto none = permission_groups(parse_manifest(manifest()));
    check(none.size() == 1 && none.front().category == "BASIC" && none.front().risk == Risk::grey &&
              none.front().phrases.front().starts_with("No special permissions"),
          "A plugin with no permissions says so");
    for (const auto name : known_permissions())
        check(!permission_category(name).empty(), "Every known permission has a category: " + std::string(name));

    auto summary_manifest = manifest(R"("net.http", "chat.read", "settings.write")");
    summary_manifest.pop_back();
    summary_manifest += R"(, "domains": ["api.example.com"]})";
    const auto summary_groups = permission_groups(parse_manifest(summary_manifest));
    const auto summary_prompt = approval_prompt(summary_groups);
    check(summary_prompt.starts_with("This mod includes a plugin - a script that runs inside your game.\n\nWhat it can do\n"),
          "The question introduces the plugin and its capabilities in plain language");
    check(summary_prompt.find("Change game settings") != std::string::npos && summary_prompt.find("Read chat") != std::string::npos &&
              summary_prompt.find("Send requests over the internet") != std::string::npos,
          "The summary follows the permissions this manifest actually requested");
    check(summary_prompt.find("Send chat") == std::string::npos && summary_prompt.find("throwdowns") == std::string::npos &&
              summary_prompt.find("api.example.com") == std::string::npos,
          "It leaves unrequested capabilities and full domain details out of the question");
    check(summary_prompt.find("! ") == std::string::npos && summary_prompt.find("harmless") == std::string::npos &&
              summary_prompt.find("SETTINGS") == std::string::npos && summary_prompt.find("WEB") == std::string::npos,
          "It does not show risk marks, category headings, or harmless labels");
    const auto web = std::find_if(summary_groups.begin(), summary_groups.end(), [](const auto& group) { return group.category == "WEB"; });
    check(web != summary_groups.end() && web->phrases.front().find("api.example.com") != std::string::npos,
          "The full domain detail remains available in Plugin Manager");
    check(summary_prompt.find("Only turn it on if you trust its source. Full permissions: Insert menu -> Plugins -> Plugin Manager.") != std::string::npos &&
              summary_prompt.ends_with("The rest of the mod works with the plugin off."),
          "The question gives the approved trust, navigation, and off-state wording");
    check(approval_prompt(none).find("No special permissions") != std::string::npos,
          "A permissionless plugin says it has no special permissions");
    for (const auto name : known_permissions()) {
        auto requested = std::string("\"") + std::string(name) + "\"";
        if (name == "events.physics.geometry") requested = R"("events.physics", "events.physics.geometry")";
        if (name == "world.physics.reuse") requested = R"("world.physics", "world.physics.reuse")";
        if (name == "effects.game") requested = R"("effects", "effects.game")";
        const auto requested_prompt = approval_prompt(permission_groups(parse_manifest(manifest(requested))));
        check(requested_prompt.find("\n- ") != std::string::npos && requested_prompt.find("No special permissions") == std::string::npos,
              "A request for " + std::string(name) + " has a capability summary");
        if (name == "room")
            check(requested_prompt.find("character room, where others see your skater") != std::string::npos,
                  "The room permission says others can see the skater");
        if (name == "events.physics.geometry")
            check(requested_prompt.find("Follow detailed skater physics, including geometry, each step") != std::string::npos,
                  "The geometry permission says it adds detailed physics geometry");
        if (name == "hud.score")
            check(requested_prompt.find("its own tricks and points in skate.'s score HUD (they never count)") != std::string::npos,
                  "The score HUD permission says what it shows never counts");
    }

    // Every requested permission group contributes a capability summary, while the card remains bounded.
    {
        std::string all;
        for (const auto name : known_permissions()) all += (all.empty() ? "\"" : ", \"") + std::string(name) + "\"";
        const auto everything = permission_groups(parse_manifest(manifest(all)));
        const auto prompt = approval_prompt(everything);
        check(prompt.size() < 3000, "Summaries for every capability stay concise enough for the question");
        for (const auto& group : everything)
            check(approval_prompt(std::vector<PermissionGroup>{group}).find("\n- ") != std::string::npos,
                  "The question summarizes requested capabilities for " + group.category);
        check(prompt.find("!") == std::string::npos && prompt.find("harmless") == std::string::npos,
              "The all-permissions summary does not restore risk marks or harmless wording");

        std::string domains = R"("domains":[)";
        constexpr std::string_view digits = "0123456789abcdef";
        for (std::size_t i = 0; i < digits.size(); ++i) {
            if (i) domains += ", ";
            std::string label(63, 'a');
            label.back() = digits[i];
            const auto host = label + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd');
            check(host.size() == 253, "The long-host fixture uses the manifest's maximum host length");
            domains += "\"" + host + "\"";
        }
        domains += "]";
        auto maximal_manifest = manifest(all);
        maximal_manifest.pop_back();
        maximal_manifest += ", " + domains + "}";
        const auto maximal_groups = permission_groups(parse_manifest(maximal_manifest));
        const auto maximal_prompt = approval_prompt(maximal_groups);
        check(maximal_prompt.size() < 3000, "Maximum domain names do not expand the concise prompt");
        const auto maximal_web = std::find_if(maximal_groups.begin(), maximal_groups.end(), [](const auto& group) { return group.category == "WEB"; });
        const auto first_host = std::string(62, 'a') + "0." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd');
        check(maximal_web != maximal_groups.end() &&
                  std::any_of(maximal_web->phrases.begin(), maximal_web->phrases.end(), [&](const auto& phrase) {
                      return phrase.find(first_host) != std::string::npos;
                  }),
              "The complete host list remains in the full permission details");
        for (std::size_t i = 0; i < digits.size(); ++i) {
            std::string label(63, 'a');
            label.back() = digits[i];
            const auto host = label + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd');
            check(maximal_prompt.find(host) == std::string::npos, "The prompt omits full host " + std::to_string(i));
        }
    }
    check(permission_groups(parse_manifest(manifest(R"("audio", "settings.read", "settings.write")"))).front().category == "SETTINGS",
          "A category is as risky as its riskiest permission");

    // A host with an environment that has not approved the plugin holds it until the player does.
    struct Strict final : Environment {
        bool allowed{};
        void log(LogLevel, std::string_view) override {}
        void console(std::string_view) override {}
        double now_ms() override { return 0; }
        std::filesystem::path storage_root() override { return scratch / "approval-storage"; }
        bool builtin_command(std::string_view) override { return false; }
        std::optional<bool> approved(const Candidate&) override { return allowed; }
        void approve(const Candidate&, bool on) override { allowed = on; }
    } environment;
    write(mod / manifest_file, manifest());
    Host host(environment, Side::client);
    const std::vector<Candidate> found{{"approval-mod", mod, "1", {}, "p"}};
    host.apply(found);
    check(host.status().front().state == "switched off", "A plugin that is not approved does not load");
    check(host.switch_on("approval-mod").find("started") != std::string::npos && host.status().front().state == "running", "and runs once turned on");
    environment.allowed = false;
    host.apply(found);
    check(host.status().front().state == "switched off", "An approval taken back unloads it");
    host.shutdown();
}

void a_manifest_says_what_a_plugin_is() {
    const auto good = parse_manifest(manifest(R"("console", "storage")"));
    check(good.api == 1 && good.entry == "plugin/main.lua" && good.side == Side::client, "A manifest is read");
    check(good.permissions.contains("console") && good.permissions.contains("storage"), "with its permissions");
    const auto refused = [](const std::string& text, std::string_view because) {
        try {
            parse_manifest(text);
        } catch (const std::exception& error) {
            check(std::string(error.what()).find(because) != std::string::npos, "Refused for \"" + std::string(error.what()) + "\", not " + std::string(because));
            return;
        }
        throw std::runtime_error("A manifest that should be refused was read: " + text);
    };
    refused(R"({"entry": "main.lua"})", "\"api\"");
    refused(R"({"api": 2, "entry": "main.lua"})", "update ReSkate");
    refused(R"({"api": 1, "entry": "../main.lua"})", "inside the mod folder");
    refused(R"({"api": 1, "entry": "C:/main.lua"})", "inside the mod folder");
    refused(R"({"api": 1, "entry": "a\\main.lua"})", "inside the mod folder");
    refused(R"({"api": 1, "entry": "main.txt"})", ".lua");
    refused(R"({"api": 1, "entry": "main.lua", "side": "moon"})", "\"side\"");
    refused(R"({"api": 1, "entry": "main.lua", "permissions": ["root"]})", "unknown permission");
    refused("not json", "not valid JSON");
    check(safe_relative_path("a/b.lua") == "a/b.lua", "A path inside the package is kept");
    for (const auto* bad : {"", "/a", "a//b", "./a", "a/../b", "..", "a\\b", "c:a", "a.", "a /b", "a\nb"})
        check(safe_relative_path(bad).empty(), std::string("A path that could leave the package is refused: ") + bad);
}

void plugins_own_typed_bodies_and_acknowledge_measured_results() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const auto candidate = script("owned-body", R"(
        local P, v = reskate.world.physics, vector.create
        local definition = {mass = 12, inertia = v(2, 3, 4), position = v(1, 2, 3), rotation = {0, 0, 0, 1}, capsules = {{a = v(0, 0, 0), b = v(0, .5, 0), radius = .1}}}
        local h = assert(P.create(definition))
        assert(type(h) == "userdata" and getmetatable(h) == "locked")
        local s = assert(P.read(h))
        assert(type(s.epoch) == "userdata" and s.position == v(1, 2, 3) and s.mass == 12 and s.rotation[4] == 1 and type(s.serial) == "string")
        local other = assert(P.create(definition))
        assert(not pcall(P.read, {}), "forged body")
        assert(not pcall(P.submit, h, {}, {}), "forged epoch")
        assert(not pcall(P.submit, other, s.epoch, {}), "epoch transplanted to another body")
        assert(not pcall(P.submit, h, s.epoch, {impulse = v(0, 2, 0)}), "anonymous impulse")
        assert(not pcall(P.submit, h, s.epoch, {force = v(40000, 40000, 0)}), "vector norm")
        assert(not pcall(P.submit, h, s.epoch, {torque = v(0 / 0, 0, 0)}), "NaN")
        assert(not pcall(P.submit, h, s.epoch, {impulse_id = 1.5}), "fractional event")
        assert(not pcall(P.submit, h, s.epoch, {native_handle = 42}), "raw handle field")
        definition.mask = 0xffffffff
        assert(not pcall(P.create, definition), "arbitrary collision mask")
        definition.mask = nil
        assert(P.submit(h, s.epoch, {impulse = v(0, 2, 0), impulse_id = 1}))
        assert(not P.submit(h, s.epoch, {}), "immutable pending packet")
        local queued = assert(P.receipt(h))
        assert(queued.outcome == "queued" and not queued.successor and not P.acknowledge(h, queued.sequence))
        assert(not pcall(P.acknowledge, h, 1), "sequence string cannot be a coerced number")
        assert(not pcall(P.acknowledge, h, queued.sequence .. string.char(0) .. "junk"), "sequence cannot hide trailing bytes")
        local done = false
        reskate.on("tick", function()
            if done then return end
            local r = assert(P.receipt(h))
            if r.outcome == "queued" then return end
            assert(r.outcome == "applied" and r.force_attempted and r.torque_attempted)
            if not r.successor then
                assert(not P.acknowledge(h, r.sequence), "no fictitious successor")
                return
            end
            assert(tonumber(r.produced_serial) == tonumber(s.serial) + 1)
            assert(tonumber(r.successor.serial) > tonumber(r.produced_serial))
            assert(P.acknowledge(h, r.sequence))
            assert(not P.acknowledge(h, r.sequence), "ack twice")
            assert(not P.submit(h, s.epoch, {}), "old observed serial")
            assert(P.release(h) and P.release(other))
            assert(not P.read(h))
            assert(P.receipt(h).retained)
            done = true
            reskate.log("owned body acknowledged its measured successor")
        end)
    )", R"("world.physics")");
    host.apply({candidate});
    running(host, "owned-body", environment);
    check(environment.fake.body_creates == 2 && environment.fake.body_submissions == 1, "API forwarded only bounded accepted definitions and one packet");
    environment.fake.body_producer_step();
    ticks(host, environment, 1);
    running(host, "owned-body", environment);
    check(!environment.logged("acknowledged its measured"), "setter completion was not treated as an integrated successor");
    environment.fake.body_producer_step();
    ticks(host, environment, 1);
    running(host, "owned-body", environment);
    check(environment.logged("owned body acknowledged its measured successor"), environment.all());
    const auto old_owner = environment.fake.owned.begin()->second.owner;
    host.apply({});
    check(std::find(environment.fake.body_releases.begin(), environment.fake.body_releases.end(), old_owner) != environment.fake.body_releases.end(), "VM close releases its instance");
    host.apply({candidate});
    running(host, "owned-body", environment);
    check(environment.fake.owned.at(3).owner != old_owner, "reloaded VM reused its old native owner epoch");
    owned_body::Sample sample;
    check(!environment.fake.body_read(environment.fake.owned.at(3).owner, {1, 0}, sample).empty(), "another VM instance adopted a retained body");
}

void plugins_pipeline_native_steps_and_select_owned_receipts() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("body-pipeline", R"(
        local P, v = reskate.world.physics, vector.create
        local d = {mass = 12, inertia = v(2, 3, 4), position = v(0, 2, 0), rotation = {0, 0, 0, 1}, capsules = {{a = v(0, 0, 0), b = v(0, .5, 0), radius = .1}}}
        local h, other = assert(P.create(d)), assert(P.create(d))
        local ids = {}
        local function publish(sample)
            local ok, why, id = P.submit(h, sample.epoch, {force = v(0, 20, 0)})
            assert(ok and why == nil and type(id) == "string")
            table.insert(ids, id)
            return id
        end
        publish(assert(P.read(h)))
        local ok, why, foreign = P.submit(other, assert(P.read(other)).epoch, {})
        assert(ok and why == nil and type(foreign) == "string" and foreign ~= ids[1])
        local done = false
        reskate.on("tick", function()
            if done then return end
            local sample = assert(P.read(h))
            local first = assert(P.receipt(h))
            assert(first.sequence == ids[1] and first.outcome == "applied")
            assert(not P.receipt(h, foreign), "foreign body's sequence selected a local receipt")
            assert(not P.receipt(h, "999999999999"), "absent sequence")
            assert(not pcall(P.receipt, h, 1) and not pcall(P.receipt, h, ids[1] .. string.char(0) .. "x")
                and not pcall(P.receipt, h, "0" .. ids[1]), "malformed selection")
            if #ids == 1 then assert(not first.successor and not P.acknowledge(h, ids[1])) end
            if #ids >= 2 then assert(first.successor.serial == "102", "first successor moved to a later observation") end
            if #ids < 4 then
                local id = publish(sample)
                assert(P.receipt(h, id).outcome == "queued" and P.receipt(h).sequence == ids[1])
                return
            end
            local accepted, reason = P.submit(h, sample.epoch, {})
            assert(not accepted and type(reason) == "string", "full receipt ring lost backpressure")
            assert(not P.acknowledge(h, ids[2]), "ack order")
            assert(P.acknowledge(h, ids[1]) and not P.receipt(h, ids[1]), "freed record stayed selectable")
            local fifth = publish(sample)
            assert(P.receipt(h, fifth).outcome == "queued" and P.receipt(h).sequence == ids[2])
            assert(tonumber(P.receipt(h, ids[2]).produced_serial) == 102 and tonumber(P.receipt(h, ids[3]).produced_serial) == 103
                and tonumber(P.receipt(h, ids[4]).produced_serial) == 104, "consecutive 60Hz control skipped a native step")
            done = true
            reskate.log("body pipeline advanced four native steps without awaiting acknowledgement")
        end)
    )", R"("world.physics")")});
    running(host, "body-pipeline", environment);
    for (unsigned i = 0; i < 4; ++i) { environment.fake.body_producer_step(); ticks(host, environment, 1); running(host, "body-pipeline", environment); }
    check(environment.logged("body pipeline advanced four native steps"), environment.all());
    check(environment.fake.body_submissions == 6, "full-ring refusal was forwarded or consecutive commands were dropped");
    const auto pending_sequence = environment.fake.owned.at(1).mailbox.sequence;
    host.apply({});
    auto& retired = environment.fake.owned.at(1);
    owned_body::Receipt canceled;
    check(retired.released && !retired.mailbox.pending && retired.mailbox.failed && retired.mailbox.find(pending_sequence, canceled) &&
        canceled.outcome == owned_body::Outcome::rejected, "VM unload did not revoke a pipelined pending packet");
}
void plugins_can_inspect_later_partial_failure_without_acknowledging_unmeasured_head() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("body-pipeline-failure", R"(
        local P, v = reskate.world.physics, vector.create
        local h = assert(P.create({mass = 12, inertia = v(2, 3, 4), position = v(0, 2, 0), rotation = {0, 0, 0, 1}, capsules = {{a = v(0, 0, 0), b = v(0, .5, 0), radius = .1}}}))
        local ok, why, first = P.submit(h, assert(P.read(h)).epoch, {impulse = v(0, 2, 0), impulse_id = 1})
        assert(ok and not why and type(first) == "string")
        local second, held, done
        reskate.on("tick", function()
            if done then return end
            if not second then
                held = assert(P.read(h)).epoch
                assert(P.receipt(h, first).outcome == "applied" and not P.receipt(h, first).successor)
                local ok, why, id = P.submit(h, held, {impulse = v(0, 2, 0), impulse_id = 2})
                assert(ok and not why)
                second = id
                return
            end
            local head, later = assert(P.receipt(h)), assert(P.receipt(h, second))
            assert(head.sequence == first and head.outcome == "applied" and not head.successor and head.quarantined)
            assert(later.outcome == "partial" and later.force_attempted and not later.torque_attempted and later.quarantined)
            assert(not P.acknowledge(h, first) and not P.acknowledge(h, second), "failure waived successor or ack order")
            assert(not P.read(h) and not P.submit(h, held, {}), "failed owner stayed writable")
            assert(P.release(h) and P.receipt(h, second).retained)
            done = true
            reskate.log("later partial body receipt is inspectable behind unmeasured head")
        end)
    )", R"("world.physics")")});
    running(host, "body-pipeline-failure", environment);
    environment.fake.body_producer_step();
    ticks(host, environment, 1);
    running(host, "body-pipeline-failure", environment);
    environment.fake.body_producer_step(false);
    ticks(host, environment, 1);
    running(host, "body-pipeline-failure", environment);
    check(environment.logged("later partial body receipt is inspectable"), environment.all());
    check(environment.fake.body_submissions == 2 && environment.fake.owned.at(1).mailbox.failed, "partial command was replayed or failed mailbox reopened");
}

// A plugin that reads, acknowledges and submits on every tick, driven through frames in the two
// orders the game runs them in, as counted there per frame. 'f' is the full-rate order: clock
// edge, three synchronizer calls on listed wrappers, tick, three more calls. 'h' is the half-rate
// order: clock edge, tick, three calls, where the tick that reads sample N+1 still finds its
// command for N queued. Either way, and across changes of order, every tick's command is
// accepted and one is applied on every step, each in the step after its own epoch.
void plugins_command_every_step_when_the_tick_precedes_the_synchronizer() {
    for (const std::string_view order : {"ffffffff", "hhhhhhhh", "hhhffffhhhhfff", "fhfhhffhhf"}) {
        TestEnvironment environment;
        Host host(environment, Side::client);
        host.apply({script("body-every-step", R"(
            local P, v = reskate.world.physics, vector.create
            local h = assert(P.create({mass = 12, inertia = v(2, 3, 4), position = v(0, 2, 0), rotation = {0, 0, 0, 1},
                capsules = {{a = v(0, 0, 0), b = v(0, .5, 0), radius = .1}}}))
            local last_produced, acknowledged, most_rows = nil, 0, 0
            local waiting = {}
            reskate.on("tick", function()
                local sample = assert(P.read(h))
                while #waiting > 0 do
                    local r = assert(P.receipt(h))
                    assert(r.sequence == waiting[1], "the oldest receipt is not the oldest command")
                    if r.outcome == "queued" then break end
                    assert(r.outcome == "applied" and r.force_attempted and r.torque_attempted, "a command ended as " .. r.outcome)
                    if not r.successor then break end
                    local produced = tonumber(r.produced_serial)
                    assert(produced == tonumber(r.requested_serial) + 1, "a command was applied on another step than the one after its epoch")
                    assert(last_produced == nil or produced == last_produced + 1, "a step went without a command")
                    last_produced = produced
                    assert(P.acknowledge(h, r.sequence))
                    table.remove(waiting, 1)
                    acknowledged += 1
                end
                local ok, why, id = P.submit(h, sample.epoch, {force = v(0, 20, 0)})
                assert(ok and why == nil, "a tick's command was refused: " .. tostring(why))
                table.insert(waiting, id)
                most_rows = math.max(most_rows, #waiting)
                assert(not P.submit(h, sample.epoch, {}), "one measured epoch was commanded twice")
                reskate.log("every-step acknowledged=" .. acknowledged .. " most_rows=" .. most_rows)
            end)
        )", R"("world.physics")")});
        running(host, "body-every-step", environment);
        auto& fake = environment.fake;
        unsigned taken{};
        for (const char kind : order) {
            fake.body_clock_step();
            // Of a frame's calls at most one takes a command: the first before a full-rate
            // frame's tick, or the first after a half-rate frame's.
            if (kind != 'h') for (unsigned call = 0; call < 3; ++call) taken += fake.body_synchronizer_call();
            ticks(host, environment, 1);
            const auto before = taken;
            for (unsigned call = 0; call < 3; ++call) taken += fake.body_synchronizer_call();
            check(kind == 'h' || taken == before, "order " + std::string(order) + ": a call after the tick took the command for the step it read");
            running(host, "body-every-step", environment);
        }
        const auto frames = static_cast<unsigned>(order.size());
        const auto& box = fake.owned.at(1).mailbox;
        check(fake.body_submissions == frames && taken == frames - 1 && box.last_serial == fake.owned.at(1).sample.epoch.serial - 1,
            "order " + std::string(order) + ": submitted " + std::to_string(fake.body_submissions) + " commands, applied " + std::to_string(taken) +
            " in " + std::to_string(frames) + " frames\n" + environment.all());
        // A result is acknowledged two ticks after its command, and the plugin never holds more
        // than two receipts after its submit: the same in both orders.
        check(environment.logged("every-step acknowledged=" + std::to_string(frames - 2) + " most_rows=2"), "order " + std::string(order) + "\n" + environment.all());
        for (std::size_t i = 0; i < box.count; ++i) {
            const auto outcome = box.receipts[(box.head + i) % box.receipts.size()].outcome;
            check(outcome == owned_body::Outcome::applied || outcome == owned_body::Outcome::queued, "a command expired or was rejected");
        }
    }
}
// How a plugin learns, before it relies on it, that this runtime queues a second command:
// reskate.world.physics.limits() returns {pending_commands = 2}. A runtime whose inbox holds one
// command has no limits function. It needs world.physics like every other call there, runs on
// the client only, and nothing readable without the permission says the number.
void plugins_read_how_many_commands_a_body_holds_pending() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("physics-limits", R"(
        local P, v = reskate.world.physics, vector.create
        local limits = P.limits()
        assert(type(limits) == "table" and getmetatable(limits) == nil, "limits is a plain table")
        assert(type(limits.pending_commands) == "number" and limits.pending_commands == 2, "limits did not say the inbox holds two pending commands")
        local count = 0
        for key in limits do assert(key == "pending_commands", key); count += 1 end
        assert(count == 1, "limits says more than it was asked to")
        limits.pending_commands = 9
        assert(P.limits().pending_commands == 2, "a plugin changed what limits returns")
        assert(not pcall(P.limits, 1) and not pcall(P.limits, {}), "limits takes no arguments")
        -- What it says is true of submit: the next sample's command queues behind a pending one.
        local h = assert(P.create({mass = 12, inertia = v(2, 3, 4), position = v(0, 2, 0), rotation = {0, 0, 0, 1},
            capsules = {{a = v(0, 0, 0), b = v(0, .5, 0), radius = .1}}}))
        local first = assert(P.read(h))
        assert(first.pending_commands == nil and first.limits == nil, "read's sample grew a field")
        local refusal = "body command invalid, repeated, stopped, pending or receipt ring full"
        local _, _, first_id = assert(P.submit(h, first.epoch, {}))
        local second_id, second_serial, stage = nil, nil, 0
        -- A refused command comes back as false and the game's own words, with no sequence.
        local function refused(epoch, why)
            local results = table.pack(P.submit(h, epoch, {}))
            assert(results.n == 2 and results[1] == false and results[2] == refusal, why .. ": " .. tostring(results[2]))
        end
        -- Read by its sequence, a command's receipt is its own; with no sequence it is the oldest.
        local function both_queued()
            local own = assert(P.receipt(h, second_id))
            assert(own.sequence == second_id and own.outcome == "queued" and own.requested_serial == second_serial and
                own.produced_serial == "0" and own.successor == nil and not own.force_attempted and not own.torque_attempted,
                "receipt(h, sequence) is not the second command's own queued receipt")
            local oldest = assert(P.receipt(h))
            assert(oldest.sequence == first_id and oldest.outcome == "queued" and oldest.requested_serial == first.serial,
                "receipt(h) is not the oldest command's receipt")
            local same = assert(P.receipt(h, first_id))
            assert(same.sequence == first_id and same.outcome == "queued", "receipt(h, sequence) lost the first command's receipt")
        end
        reskate.on("tick", function()
            local sample = assert(P.read(h))
            if sample.serial == first.serial then return end
            stage += 1
            if stage == 1 then
                -- The clock reads the step after the first command's epoch and no call has taken it.
                assert(assert(P.receipt(h)).outcome == "queued", "this case needs the first command still pending")
                local held = 1
                local ok, why, id = P.submit(h, sample.epoch, {})
                if ok then held += 1 end
                assert(held == P.limits().pending_commands, "the inbox held " .. held .. " pending commands")
                assert(why == nil and tonumber(id) == tonumber(first_id) + 1, "the second command did not get the next sequence")
                second_id, second_serial = id, sample.serial
                both_queued()
                refused(sample.epoch, "a second command for one epoch was not refused in the game's words")
                both_queued()
                reskate.log("physics limits read")
            elseif stage == 2 then
                -- The clock moved on again with both still pending: this sample's command would be a third.
                assert(sample.serial ~= second_serial, "this case needs a third step")
                refused(sample.epoch, "a third pending command was not refused in the game's words")
                both_queued()
                reskate.log("physics limits third refused")
            elseif stage == 3 then
                -- A call has read this step: the first command's step was missed, the second was applied.
                local expired, applied = assert(P.receipt(h)), assert(P.receipt(h, second_id))
                assert(expired.sequence == first_id and expired.outcome == "expired" and expired.produced_serial == "0",
                    "the first command did not expire on its own: " .. expired.outcome)
                assert(applied.sequence == second_id and applied.outcome == "applied" and applied.produced_serial == sample.serial,
                    "the second command was not applied in the step after its epoch: " .. applied.outcome)
                -- Neither refusal spent a sequence: the next command accepted is numbered after the second.
                local ok, why, id = P.submit(h, sample.epoch, {})
                assert(ok and why == nil and tonumber(id) == tonumber(second_id) + 1, "a refused command spent a sequence")
                reskate.log("physics limits sequence kept")
            end
        end)
    )", R"("world.physics")"), script("physics-limits-denied", R"(
        local P = reskate.world.physics
        local ok, why = pcall(P.limits)
        assert(not ok and string.find(why, "world.physics", 1, true), tostring(why))
        for name, value in P do assert(type(value) == "function", name .. " is a value a plugin can read without the permission") end
        reskate.log("physics limits denied")
    )")});
    running(host, "physics-limits", environment);
    running(host, "physics-limits-denied", environment);
    // The half-rate order: the clock moves on and the tick comes before any synchronizer call.
    environment.fake.body_clock_step();
    ticks(host, environment, 1);
    running(host, "physics-limits", environment);
    check(environment.logged("physics limits read") && environment.logged("physics limits denied"), environment.all());
    check(environment.fake.body_creates == 1 && environment.fake.body_submissions == 2, "the inbox did not hold what limits said");
    // Another clock edge with both commands still pending: the tick's command would be a third.
    environment.fake.body_clock_step();
    ticks(host, environment, 1);
    running(host, "physics-limits", environment);
    check(environment.logged("physics limits third refused"), environment.all());
    check(environment.fake.body_submissions == 2 && environment.fake.body_command_sequence == 2 && environment.fake.body_submit_entries == 4 &&
        environment.fake.owned.at(1).mailbox.count == 2, "a refused command reached the inbox or spent a sequence");
    // A synchronizer call reads that step: it is the second command's, and the first's has gone.
    check(environment.fake.body_synchronizer_call() == 1, "the second command was not taken in the step after its epoch");
    ticks(host, environment, 1);
    running(host, "physics-limits", environment);
    check(environment.logged("physics limits sequence kept") && environment.fake.body_command_sequence == 3, environment.all());
    TestEnvironment server_environment;
    Host server(server_environment, Side::server);
    server.apply({package("server-limits", {{manifest_file, manifest(R"("world.physics")", "server")}, {"plugin/main.lua", R"(
        local ok, why = pcall(reskate.world.physics.limits)
        assert(not ok and string.find(why, "client phase only"), tostring(why))
    )"}})});
    running(server, "server-limits", server_environment);
}

// create(definition, {reuse = true}) through the binding: it forwards this VM's own instance and
// the flag, parses its options strictly, counts one reuse request a tick, and returns the
// origin and miss the game reports. Which parked body qualifies is the service's rule.
void plugins_reuse_their_own_parked_bodies() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const auto reuser = [](const std::string& id) {
        return script(id, R"(
            local P, v = reskate.world.physics, vector.create
            local d = {mass = 12, inertia = v(2, 3, 4), position = v(0, 2, 0), rotation = {0, 0, 0, 1}, capsules = {{a = v(0, 0, 0), b = v(0, .5, 0), radius = .1}}}
            assert(select("#", P.create(d)) == 1, "plain create grew a return value")
            local body, why, origin, miss = P.create(d, {reuse = true})
            assert(type(body) == "userdata" and getmetatable(body) == "locked" and why == nil and origin == "reused" and miss == nil,
                "reuse did not return body, nil, origin")
            assert(P.read(body), "a reused body is not this VM's body")
            local again, refused = P.create(d, {reuse = true})
            assert(again == nil and refused == "asked to reuse a native body more than once in one tick", tostring(refused))
            local plain, none, allocated, extra = P.create(d, {reuse = false})
            assert(type(plain) == "userdata" and none == nil and allocated == "allocated" and extra == nil, "reuse = false")
            for _, options in {{reuse = 1}, {reuse = "true"}, {bogus = true}, {reuse = true, slot = 1}, {reuse = true, owner = 1}} do
                assert(not pcall(P.create, d, options), "malformed options were accepted")
            end
            assert(not pcall(P.create, d, true), "options that are not a table were accepted")
            local ticks = 0
            reskate.on("tick", function()
                ticks += 1
                if ticks == 1 then
                    local b, w, o, m = P.create(d, {reuse = true})
                    assert(type(b) == "userdata" and w == nil and o == "allocated" and m == "definition_differs", tostring(o) .. " " .. tostring(m))
                elseif ticks == 2 then
                    local b, w, o = P.create(d, {reuse = true})
                    assert(b == nil and w == "body_reuse_pending" and o == nil, tostring(w))
                    -- A refused reuse still uses this tick's one request: the retry waits a tick.
                    local again, refused = P.create(d, {reuse = true})
                    assert(again == nil and refused == "asked to reuse a native body more than once in one tick", tostring(refused))
                    reskate.log("reuse options answered")
                end
            end)
        )", R"("world.physics", "world.physics.reuse")");
    };
    auto& fake = environment.fake;
    const auto by_owner = [&](std::size_t from) {
        std::map<std::uint64_t, std::pair<unsigned, unsigned>> owners; // reuse = true, reuse = false
        for (std::size_t i = from; i < fake.body_reuse_requests.size(); ++i) {
            const auto& [owner, reuse] = fake.body_reuse_requests[i];
            ++(reuse ? owners[owner].first : owners[owner].second);
        }
        return owners;
    };
    host.apply({reuser("reuse-a"), reuser("reuse-b")});
    running(host, "reuse-a", environment);
    running(host, "reuse-b", environment);
    // Each: one reuse, the refused second one never forwarded, and reuse = false.
    auto owners = by_owner(0);
    check(fake.body_reuse_requests.size() == 4 && owners.size() == 2, "the quota or the options did not reach the game as asked");
    for (const auto& [owner, counts] : owners)
        check(owner && counts.first == 1 && counts.second == 1, "a plugin's own instance or reuse flag was not forwarded");
    fake.test_reuse_report = {owned_body::Origin::allocated, owned_body::ReuseMiss::definition_differs};
    ticks(host, environment, 1);
    running(host, "reuse-a", environment);
    running(host, "reuse-b", environment);
    check(fake.body_reuse_requests.size() == 6 && by_owner(4) == decltype(owners){{owners.begin()->first, {1, 0}}, {std::next(owners.begin())->first, {1, 0}}},
        "a new tick did not allow one more reuse request for each plugin");
    fake.test_reuse_refusal = "body_reuse_pending";
    const auto refused_from = fake.body_reuse_requests.size();
    ticks(host, environment, 1);
    running(host, "reuse-a", environment);
    running(host, "reuse-b", environment);
    check(environment.logged("reuse options answered"), environment.all());
    check(fake.body_reuse_requests.size() == refused_from + 2 &&
        by_owner(refused_from) == decltype(owners){{owners.begin()->first, {1, 0}}, {std::next(owners.begin())->first, {1, 0}}},
        "a refused reuse did not use up its tick's one request for each plugin");
    // A reloaded VM is a new instance, so it cannot name the old one's parked bodies.
    fake.test_reuse_refusal.clear();
    fake.test_reuse_report = {owned_body::Origin::reused, owned_body::ReuseMiss::none};
    const auto before = fake.body_reuse_requests.size();
    host.apply({});
    // The fake's handles name at most eight slots; the reloaded VM's bodies start afresh.
    fake.owned.clear();
    fake.body_creates = 0;
    host.apply({reuser("reuse-a")});
    running(host, "reuse-a", environment);
    const auto reloaded = by_owner(before);
    check(reloaded.size() == 1 && !owners.contains(reloaded.begin()->first), "a reloaded VM forwarded an old instance as the owner");
}

void native_body_permission_and_client_boundary_are_enforced() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("no-body-permission", R"(
        local P = reskate.world.physics
        for _, name in {"create", "read", "submit", "receipt", "acknowledge", "release", "probe_batch"} do
            local ok, why = pcall(P[name], {})
            assert(not ok and string.find(why, "world.physics"), name)
        end
        local ok, why = pcall(P.create, {}, {reuse = true})
        assert(not ok and string.find(why, "world.physics"), "create with options")
    )")});
    running(host, "no-body-permission", environment);
    check(environment.fake.body_creates == 0 && environment.fake.body_submissions == 0, "missing permission reached Game");
    // world.physics alone allocates but cannot ask to reuse: that needs world.physics.reuse.
    TestEnvironment reuse_environment;
    Host reuse_host(reuse_environment, Side::client);
    reuse_host.apply({script("no-reuse-permission", R"(
        local P, v = reskate.world.physics, vector.create
        local d = {mass = 12, inertia = v(2, 3, 4), position = v(0, 2, 0), rotation = {0, 0, 0, 1}, capsules = {{a = v(0, 0, 0), b = v(0, .5, 0), radius = .1}}}
        local ok, why = pcall(P.create, d, {reuse = true})
        assert(not ok and string.find(why, "world.physics.reuse", 1, true), tostring(why))
        assert(type(P.create(d)) == "userdata", "plain create needs only world.physics")
        assert(select("#", P.create(d, nil)) == 1, "a nil options argument changed plain create")
        local body, none, origin = P.create(d, {reuse = false})
        assert(type(body) == "userdata" and none == nil and origin == "allocated", "reuse = false needs only world.physics")
        assert(select("#", P.create(d, {})) == 3, "empty options did not return the origin")
        reskate.log("reuse needs its own permission")
    )", R"("world.physics")")});
    running(reuse_host, "no-reuse-permission", reuse_environment);
    const auto& asked = reuse_environment.fake.body_reuse_requests;
    check(reuse_environment.logged("reuse needs its own permission") && reuse_environment.fake.body_creates == 4 && asked.size() == 2 &&
        !asked[0].second && !asked[1].second, "a plugin without world.physics.reuse reached the game with a reuse request");
    TestEnvironment server_environment;
    Host server(server_environment, Side::server);
    auto server_plugin = package("server-body", {{manifest_file, manifest(R"("world.physics","world.raycast","world.physics.reuse")", "server")}, {"plugin/main.lua", R"(
        local ok, why = pcall(reskate.world.physics.create, {})
        assert(not ok and string.find(why, "client phase only"))
        ok, why = pcall(reskate.world.physics.create, {}, {reuse = true})
        assert(not ok and string.find(why, "client phase only"))
        ok, why = pcall(reskate.world.physics.probe_batch, {}, {}, {})
        assert(not ok and string.find(why, "client phase only"))
    )"}});
    server.apply({server_plugin});
    running(server, "server-body", server_environment);
    check(server_environment.fake.body_creates == 0 && server_environment.fake.body_reuse_requests.empty(), "server reached native body creation");
}

std::string batch_main(std::string body) {
    return R"(
        local P, v = reskate.world.physics, vector.create
        local d = {mass = 12, inertia = v(2, 3, 4), position = v(0, 2, 0), rotation = {0, 0, 0, 1}, capsules = {{a = v(0, 0, 0), b = v(0, .5, 0), radius = .1}}}
        local h, other = assert(P.create(d)), assert(P.create(d))
        local sample = assert(P.read(h))
        local epoch = sample.epoch
        local ray = {from = v(0, 2, 0), to = v(0, -2, 0)}
        local function rays(n) local out = {} for i = 1, n do out[i] = ray end return out end
        local function unavailable(out, reason)
            assert(out.status == "unavailable" and out.reason == reason, tostring(out.reason))
            for _, r in out.rays do assert(r.status == "unavailable" and not r.position and not r.fraction and not r.body and not r.motion) end
        end
    )" + body;
}

void body_contact_details_are_known_and_epoch_bound() {
    body_query::Request last;
    unsigned corruptions{};
    const auto load = [&](const char* id, int mode, bool clear, std::string assertions) {
        // Each case starts a fresh synthetic native world. Retained body slots
        // are never erased/recycled merely to make another case fit.
        TestEnvironment environment;
        Host host(environment, Side::client);
        auto& game = environment.fake;
        game.batch_contact_mode = mode;
        game.batch_clear = clear;
        host.apply({script(id, batch_main(assertions), R"("world.physics","world.raycast")")});
        running(host, id, environment);
        last = game.last_batch;
        corruptions += game.batch_available_corruptions;
        check(game.body_submissions == 0, "contact reads submitted body commands");
    };
    load("contact-measured", 1, false, R"(
        local out = P.probe_batch(h, epoch, rays(1), {require_contact = true, exclude_local_actor = true})
        -- The constructed backend runs the batch but establishes no actor exclusion,
        -- and the binding reports the backend's fact, never the request's flag.
        assert(out.status == "available" and out.epoch == epoch and out.local_actor_excluded == false and out.owned_bodies_excluded)
        local hit = out.rays[1]
        assert(hit.motion == "measured" and hit.normal_available and hit.normal == v(0, 1, 0))
        assert(hit.surface_velocity_available and hit.surface_velocity == v(2, 0, -1))
        assert(not pcall(P.probe_batch, h, epoch, rays(1), {require_contact = 1}))
    )");
    check(last.require_contact && last.exclude_local_actor, "contact options were dropped");
    load("contact-unknown", 0, false, R"(
        unavailable(P.probe_batch(h, epoch, rays(1), {require_contact = true}), "contact_measurement_unavailable")
        local out = P.probe_batch(h, epoch, rays(1))
        assert(out.status == "available" and out.epoch == epoch and not out.local_actor_excluded)
        assert(out.rays[1].normal_available == false and out.rays[1].surface_velocity_available == false)
        assert(not out.rays[1].normal and not out.rays[1].surface_velocity)
    )");
    for (int mode = 2; mode <= 6; ++mode) {
        load(("contact-malformed-" + std::to_string(mode)).c_str(), mode, false, R"(
            unavailable(P.probe_batch(h, epoch, rays(1), {require_contact = true}), "invalid_query_backend_result")
        )");
    }
    check(corruptions == 3, "binding guard was not given all three malformed AVAILABLE backend results");
    load("contact-clear", 1, true, R"(
        local out = P.probe_batch(h, epoch, rays(1), {require_contact = true})
        assert(out.status == "available" and out.rays[1].status == "clear")
        assert(not out.rays[1].normal and not out.rays[1].surface_velocity)
    )");
}

// A named query preset is a request option, not a claim about the result. A call
// without it keeps the exact earlier result fields; an explicit preset adds only
// the echo of what was asked, on a hit, a clear and a refusal alike.
void owned_body_batches_select_a_named_query_preset() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    auto& game = environment.fake;
    host.apply({script("preset-no-ray", R"(
        local ok, why = pcall(reskate.world.physics.probe_batch, {}, {}, {}, {query_preset = "terrain"})
        assert(not ok and why:find("world.raycast"))
        reskate.log("query preset without world.raycast denied")
    )", R"("world.physics")"), script("preset-batch", batch_main(R"(
        local fields = {status = true, reason = true, native_casts = true, budget_exhausted = true, epoch = true,
            local_actor_excluded = true, owned_bodies_excluded = true, rays = true}
        local function shape(out, echo)
            local n = 0
            for k in out do
                n += 1
                assert(fields[k] or (echo and k == "requested_query_preset"), "unexpected batch field " .. tostring(k))
            end
            assert(n == (out.reason and 8 or 7) + (echo and 1 or 0), "batch field count " .. n)
            assert(out.requested_query_preset == echo, "request echo " .. tostring(out.requested_query_preset))
        end
        reskate.console.command("preset-default", "", function()
            local out = P.probe_batch(h, epoch, rays(1))
            assert(out.status == "available")
            shape(out, nil)
            out = P.probe_batch(h, epoch, rays(1), {require_contact = false, exclude_local_actor = false})
            assert(out.status == "available")
            shape(out, nil)
        end)
        reskate.console.command("preset-hit", "", function(args)
            local out = P.probe_batch(h, epoch, rays(2), {query_preset = args[1]})
            assert(out.status == "available" and out.native_casts == 2 and not out.local_actor_excluded)
            shape(out, args[1])
            for _, r in out.rays do assert(r.status == "hit" and r.position == v(0, 0, 0) and r.fraction == .5 and r.motion == "unknown") end
        end)
        reskate.console.command("preset-clear", "", function()
            local out = P.probe_batch(h, epoch, rays(1), {query_preset = "terrain", require_contact = true})
            assert(out.status == "available")
            shape(out, "terrain")
            local r = out.rays[1]
            assert(r.status == "clear" and not r.position and not r.normal and not r.surface_velocity and not r.body and not r.motion)
        end)
        reskate.console.command("preset-refused", "", function()
            local out = P.probe_batch(h, epoch, rays(1), {query_preset = "terrain", exclude_local_actor = true})
            unavailable(out, "local_actor_exclusion_unavailable")
            shape(out, "terrain")
            assert(out.native_casts == 0 and out.local_actor_excluded == false and out.owned_bodies_excluded == false)
        end)
        reskate.console.command("preset-contact", "", function(args)
            local out = P.probe_batch(h, epoch, rays(1), {query_preset = "terrain", require_contact = true})
            shape(out, "terrain")
            if args[1] == "missing" then unavailable(out, "contact_measurement_unavailable") return end
            local r = out.rays[1]
            assert(out.status == "available" and r.motion == "measured" and r.normal == v(0, 1, 0))
            assert(r.surface_velocity_available and r.surface_velocity == v(2, 0, -1), "moving measured surface lost its velocity")
        end)
        reskate.console.command("preset-invalid", "", function()
            for _, bad in {"Terrain", "TERRAIN", "static", "", "terrain" .. string.char(0), "camera ", 1, 0, true, {}, function() end} do
                assert(not pcall(P.probe_batch, h, epoch, rays(1), {query_preset = bad}), "preset " .. tostring(bad))
            end
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {query_mask = 1}), "raw mask option")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {filter = 1, query_preset = "terrain"}), "raw filter beside a preset")
            assert(not pcall(P.probe_batch, h, epoch, rays(1), {query_presets = "terrain"}), "misspelled option")
            assert(not pcall(P.probe_batch, h, {}, rays(1), {query_preset = "terrain"}), "forged epoch with a preset")
            assert(not pcall(P.probe_batch, other, epoch, rays(1), {query_preset = "terrain"}), "another body's epoch with a preset")
            assert(not pcall(P.probe_batch, epoch, epoch, rays(1), {query_preset = "terrain"}), "an epoch is not a body")
        end)
        reskate.log("query preset batches ready")
    )"), R"("world.physics","world.raycast","console")")});
    running(host, "preset-no-ray", environment);
    running(host, "preset-batch", environment);
    check(environment.logged("query preset without world.raycast denied") && environment.logged("query preset batches ready") &&
        !game.batch_calls, environment.all());
    check(host.run_console("preset-default") && game.batch_calls == 2 && game.last_batch.preset == body_query::QueryPreset::camera,
        "a batch without a preset changed its result fields or left camera\n" + environment.all());
    for (const auto preset : {body_query::QueryPreset::terrain, body_query::QueryPreset::camera}) {
        ticks(host, environment, 1);
        const auto calls = game.batch_calls;
        check(host.run_console(std::string("preset-hit ") + body_query::name(preset)) && game.batch_calls == calls + 1 &&
            game.last_batch.preset == preset, std::string("explicit preset did not reach the backend: ") + body_query::name(preset) + "\n" + environment.all());
    }
    ticks(host, environment, 1);
    game.batch_clear = true;
    check(host.run_console("preset-clear") && game.last_batch.preset == body_query::QueryPreset::terrain && game.last_batch.require_contact,
        "terrain clear invented geometry or lost its options\n" + environment.all());
    game.batch_clear = false;
    ticks(host, environment, 1);
    game.batch_refusal = body_query::Failure::local_actor_exclusion_unavailable;
    auto casts = game.native_casts;
    check(host.run_console("preset-refused") && game.last_batch.preset == body_query::QueryPreset::terrain &&
        game.last_batch.exclude_local_actor && game.native_casts == casts,
        "a terrain request echo became actor exclusion or spent a cast\n" + environment.all());
    game.batch_refusal = body_query::Failure::none;
    ticks(host, environment, 1);
    check(host.run_console("preset-contact missing"), "terrain admitted a hit without contact measurements\n" + environment.all());
    ticks(host, environment, 1);
    game.batch_contact_mode = 1;
    check(host.run_console("preset-contact measured") && game.last_batch.preset == body_query::QueryPreset::terrain,
        "terrain lost a measured moving contact\n" + environment.all());
    game.batch_contact_mode = 0;
    ticks(host, environment, 1);
    const auto calls = game.batch_calls;
    casts = game.native_casts;
    check(host.run_console("preset-invalid") && game.batch_calls == calls && game.native_casts == casts,
        "a rejected preset option reached the backend\n" + environment.all());
    // Another VM's owner or a stale generation is refused with a preset set, before any cast.
    auto request = game.last_batch;
    request.preset = body_query::QueryPreset::terrain;
    const auto owner = game.owned.at(1).owner;
    casts = game.native_casts;
    auto refused = game.body_probe_batch(owner + 1, {1, 0}, request, 64);
    check(!refused.available && !refused.native_casts && game.native_casts == casts, "another VM's body was queried with a preset");
    refused = game.body_probe_batch(owner, {1, 7}, request, 64);
    check(!refused.available && !refused.native_casts && game.native_casts == casts, "a stale body generation was queried with a preset");
}

void owned_body_batches_are_permissioned_opaque_and_transactional() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("batch-no-ray", R"(
        local ok, why = pcall(reskate.world.physics.probe_batch, {}, {}, {}, {require_contact = true})
        assert(not ok and why:find("world.raycast"))
    )", R"("world.physics")"), script("batch-no-body", R"(
        local ok, why = pcall(reskate.world.physics.probe_batch, {}, {}, {}, {require_contact = true})
        assert(not ok and why:find("world.physics"))
    )", R"("world.raycast")"), script("body-batch", batch_main(R"(
        local out = P.probe_batch(h, epoch, rays(2), {exclude_bodies = {other, h}, exclude_local_actor = true})
        assert(out.status == "available" and out.native_casts == 2 and #out.rays == 2 and not out.reason)
        assert(out.local_actor_excluded == false, "a request flag alone became an exclusion fact")
        local a, b = out.rays[1], out.rays[2]
        assert(a.status == "hit" and a.position == v(0, 0, 0) and a.fraction == .5 and a.motion == "unknown")
        assert(b.status == "hit" and a.body == nil and b.body == nil, "a hit carries no body token")
        assert(not a.normal and not a.velocity and not a.surface_velocity and not a.feature and not a.world)
        assert(not pcall(P.probe_batch, h, {}, rays(1)) and not pcall(P.probe_batch, other, epoch, rays(1)), "epoch transplant")
        local next = P.probe_batch(h, epoch, rays(1))
        assert(next.status == "available" and next.rays[1].body == nil)
        assert(not pcall(P.probe_batch, h, epoch, {}) and not pcall(P.probe_batch, h, epoch, rays(33)), "batch bound")
        assert(not pcall(P.probe_batch, h, epoch, {ray, raw_pointer = 123}), "array keys")
        assert(not pcall(P.probe_batch, h, epoch, {{from = ray.from, to = ray.to, normal = v(0, 1, 0)}}), "geometry policy input")
        assert(not pcall(P.probe_batch, h, epoch, {{from = ray.from, to = ray.from}}), "zero ray")
        assert(not pcall(P.probe_batch, h, epoch, {{from = ray.from, to = v(0 / 0, 0, 0)}}), "nonfinite ray")
        assert(not pcall(P.probe_batch, h, epoch, rays(1), {exclude_bodies = {123}}), "raw native exclusion")
        assert(not pcall(P.probe_batch, h, epoch, rays(1), {exclude_bodies = {[2] = other}}), "sparse exclusion")
        assert(not pcall(P.probe_batch, h, epoch, rays(1), {exclude_local_actor = 1}), "boolean actor switch")
        assert(not pcall(P.probe_batch, h, epoch, rays(1), {native_world = 1}), "no native world option")
        assert(not pcall(P.probe_batch, h, epoch, rays(1), {["exclude_bodies" .. string.char(0) .. "raw"] = {}}), "NUL option name")
        assert(not pcall(P.probe_batch, h, epoch, rays(1), {}, {}), "extra arguments")
        assert(P.submit(h, epoch, {force = v(0, 20, 0)}), "query did not consume command epoch")
        local first = assert(P.receipt(h))
        assert(P.probe_batch(h, epoch, rays(1)).status == "available")
        local after = assert(P.receipt(h))
        assert(after.sequence == first.sequence and after.outcome == "queued" and not after.successor, "query consumed pending packet")
        reskate.console.command("batch-pending", "", function()
            assert(P.probe_batch(h, epoch, rays(1)).status == "available")
        end)
        reskate.console.command("batch-excluded", "", function()
            unavailable(P.probe_batch(h, epoch, rays(1), {exclude_bodies = {other}}), "body_epoch_or_observation_changed")
            assert(P.receipt(h).sequence == first.sequence and P.receipt(h).outcome == "queued")
        end)
        reskate.log("owned batch API verified")
    )"), R"("world.physics","world.raycast","console")")});
    running(host, "batch-no-ray", environment);
    running(host, "batch-no-body", environment);
    running(host, "body-batch", environment);
    check(environment.logged("owned batch API verified") && environment.fake.batch_calls == 3, environment.all());
    auto& game = environment.fake;
    const auto mailbox = game.owned.at(1).mailbox;
    check(host.run_console("batch-pending"), "query while a packet is pending");
    const auto& after = game.owned.at(1).mailbox;
    check(after.pending && after.count == mailbox.count && after.sequence == mailbox.sequence && after.last_submitted == mailbox.last_submitted &&
        after.command.epoch == mailbox.command.epoch && after.oldest().outcome == mailbox.oldest().outcome && !after.oldest().successor_available,
        "read-only query changed command/receipt state");
    game.owned.at(2).sample.epoch.world = 78;
    const auto casts = game.native_casts;
    check(host.run_console("batch-excluded") && game.native_casts == casts, "foreign-world exclusion reached native query");
    auto request = game.last_batch;
    const auto owner = game.owned.at(1).owner;
    auto refused = game.body_probe_batch(owner + 1, {1, 0}, request, 64);
    check(!refused.available && !refused.native_casts && game.native_casts == casts, "another VM adopted an owned exclusion/anchor");
    refused = game.body_probe_batch(owner, {1, 7}, request, 64);
    check(!refused.available && !refused.native_casts && game.native_casts == casts, "stale handle generation/slot reached query");
    Game portable;
    check(!portable.body_probe_batch(1, {1, 0}, {}, 64).available && !portable.world_probe_bounded({}, {}, 64).probe.available &&
        !portable.world_probe_bounded({}, {}, 64).native_casts && !portable.world_pick({}, {}),
        "portable backend fabricated collision or native calls");
}
void owned_body_batches_share_actual_and_logical_query_limits() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    auto& game = environment.fake;
    game.body_list = {{9, {{1, {1, 0, 0}, {1, 2, 0}, .16f}}}};
    game.probe_result = {true, false};
    game.query_cast_cost = game.probe_cast_cost = 2;
    host.apply({script("batch-budget", batch_main(R"(
        local from, to = v(0, 1, 0), v(10, 1, 0)
        local down, ground = v(0, 1, 0), v(0, -1, 0)
        local function upstream_rays(when)
            assert(reskate.world.pick(from, to), "world.pick " .. when)
            assert(reskate.world.raycast(down, ground) == v(0, 0, 0) and not reskate.world.raycast(from, to), "world.raycast " .. when)
            local player = reskate.session.raycast(from, to)
            assert(player and player.player == "9" and math.abs(player.distance - 0.84) < 1e-4, "session.raycast " .. when)
        end
        reskate.console.command("batch-mixed", "", function()
            for i = 1, 4 do assert(reskate.world.probe(from, to).status == "clear") end
            upstream_rays("before the native casts are spent")
            local batch = P.probe_batch(h, epoch, rays(28))
            assert(batch.status == "available" and batch.native_casts == 56)
            unavailable(P.probe_batch(h, epoch, rays(1)), "native_cast_budget_exhausted")
            assert(reskate.world.probe(from, to).status == "unavailable")
            -- The native cast budget is world.probe's and probe_batch's alone. The rays plugins
            -- already had still count only as raycasts, and answer as they always did.
            upstream_rays("after the native casts are spent")
        end)
        reskate.console.command("batch-zero", "", function()
            unavailable(P.probe_batch(h, epoch, rays(32)), "query_preconditions_unavailable")
            unavailable(P.probe_batch(h, epoch, rays(32)), "query_preconditions_unavailable")
            local ok, why = pcall(P.probe_batch, h, epoch, rays(1))
            assert(not ok and why:find("more than 64 raycasts"), "each input ray spends logical budget")
            assert(not pcall(reskate.world.probe, from, to) and not pcall(reskate.session.raycast, from, to, {world = false}))
        end)
        reskate.console.command("batch-partial", "", function()
            for i = 1, 61 do reskate.world.probe(from, to) end
            local batch = P.probe_batch(h, epoch, rays(2))
            unavailable(batch, "native_cast_budget_exhausted")
            assert(batch.native_casts == 3 and batch.budget_exhausted, "last allowed cast is counted")
        end)
        reskate.console.command("batch-once", "", function(args)
            local batch = P.probe_batch(h, epoch, rays(2))
            unavailable(batch, args[1])
            if args[1] == "invalid_query_backend_result" or args[1] == "query_backend_exception" then
                assert(not batch.native_casts, "invalid backend count is not a measured count")
                unavailable(P.probe_batch(h, epoch, rays(1)), "native_cast_budget_exhausted")
            else assert(batch.native_casts == tonumber(args[2])) end
        end)
        reskate.console.command("batch-refusal-budget", "", function()
            local batch = P.probe_batch(h, epoch, rays(2))
            unavailable(batch, "body_exclusions_unavailable")
            assert(batch.native_casts == 0 and not batch.budget_exhausted)
            for i = 1, 32 do assert(reskate.world.probe(from, to).status == "clear") end
            assert(reskate.world.probe(from, to).status == "unavailable", "zero-cast refusal must leave all 64 native casts")
        end)
        reskate.console.command("batch-clear", "", function()
            local batch = P.probe_batch(h, epoch, rays(32))
            assert(batch.status == "available" and batch.native_casts == 64)
            for _, r in batch.rays do assert(r.status == "clear" and not r.position and not r.body and not r.motion) end
        end)
        reskate.console.command("batch-unknown", "", function()
            local batch = P.probe_batch(h, epoch, rays(1))
            assert(batch.status == "available" and batch.rays[1].status == "hit" and batch.rays[1].motion == "unknown" and not batch.rays[1].body)
        end)
        reskate.console.command("batch-expired", "", function()
            unavailable(P.probe_batch(h, epoch, rays(1)), "body_epoch_or_observation_changed")
            assert(not P.submit(h, epoch, {}), "query must not relabel an expired epoch")
        end)
        reskate.console.command("batch-latest", "", function(args)
            local pending
            if args[1] == "read" then
                assert(P.read(h).serial == sample.serial, "fixture keeps listener epoch")
            else
                pending = assert(P.receipt(h))
                assert(pending.outcome == "applied" and not pending.successor and pending.produced_serial == sample.serial,
                    "receipt refresh occurs while waiting for a later successor")
            end
            local out = P.probe_batch(h, epoch, rays(2))
            unavailable(out, "body_epoch_or_observation_changed")
            assert(out.native_casts == 0, "new latest cannot rebind the immutable old sample")
            if pending then
                local after = assert(P.receipt(h, pending.sequence))
                assert(after.outcome == "applied" and not after.successor and after.produced_serial == pending.produced_serial)
            end
        end)
    )"), R"("world.physics","world.raycast","session.skaters","console")")});
    running(host, "batch-budget", environment);
    const auto picks_before = game.world_picks, probes_before = game.probes;
    const auto rays_before = environment.rays;
    check(host.run_console("batch-mixed") && game.native_casts == 64 && game.batch_calls == 1 && game.probes == probes_before + 4,
        "world.probe and batches escaped their 64 native casts, or a probe past them reached its backend\n" + environment.all());
    check(game.world_picks == picks_before + 2 && environment.rays == rays_before + 6,
        "spending the native casts kept world.pick, world.raycast or session.raycast's wall check from its backend\n" + environment.all());
    ticks(host, environment, 1);
    game.query_cast_cost = 0;
    auto before = game.batch_calls;
    auto casts = game.native_casts;
    check(host.run_console("batch-zero") && game.batch_calls == before + 2 && game.native_casts == casts, "zero-call unavailable batches escaped logical cap\n" + environment.all());
    ticks(host, environment, 1);
    game.batch_change_after = true;
    check(host.run_console("batch-once body_epoch_or_observation_changed 0") && game.native_casts == casts,
        "zero-cast precondition reason overrode the post-query observation fence\n" + environment.all());
    game.batch_change_after = false;
    for (const auto reason : {body_query::Failure::client_phase_unavailable, body_query::Failure::local_actor_exclusion_unavailable,
        body_query::Failure::body_exclusions_unavailable}) {
        ticks(host, environment, 1);
        game.batch_refusal = reason;
        casts = game.native_casts;
        check(host.run_console(std::string("batch-once ") + body_query::name(reason) + " " + std::to_string(game.batch_refusal_casts)) &&
            game.native_casts == casts + game.batch_refusal_casts && !game.owned.at(1).mailbox.pending &&
            !game.owned.at(1).mailbox.count && !game.owned.at(1).mailbox.failed && !game.body_submissions,
            "typed backend refusal was lost, invented geometry or mutated command state\n" + environment.all());
    }
    ticks(host, environment, 1);
    game.batch_refusal = body_query::Failure::body_exclusions_unavailable;
    game.query_cast_cost = 2;
    casts = game.native_casts;
    const bool refusal_budget_command = host.run_console("batch-refusal-budget");
    check(refusal_budget_command && game.native_casts == casts + 64 && !game.body_submissions,
        "known zero-cast exclusion refusal poisoned or spent the shared native allowance\n" + environment.all());
    for (const auto reason : {body_query::Failure::query_backend_exception, body_query::Failure::unavailable}) {
        ticks(host, environment, 1);
        game.batch_refusal = reason;
        game.batch_refusal_casts = 2;
        game.batch_refusal_accounting_valid = false;
        before = game.batch_calls;
        casts = game.native_casts;
        const auto expected = reason == body_query::Failure::query_backend_exception ? "query_backend_exception" : "invalid_query_backend_result";
        check(host.run_console(std::string("batch-once ") + expected + " 0") && game.batch_calls == before + 1 &&
            game.native_casts == casts + 2 && !game.owned.at(1).mailbox.pending && !game.owned.at(1).mailbox.count &&
            !game.owned.at(1).mailbox.failed && !game.body_submissions,
            "unknown catch accounting lost its reason, exposed a count or failed to close the remaining budget\n" + environment.all());
    }
    game.batch_refusal = body_query::Failure::none;
    game.batch_refusal_casts = 0;
    game.batch_refusal_accounting_valid = true;
    ticks(host, environment, 1);
    game.query_cast_cost = 2;
    game.probe_cast_cost = 1;
    casts = game.native_casts;
    check(host.run_console("batch-partial") && game.native_casts == casts + 64, "partial batch retry overspent cap\n" + environment.all());
    ticks(host, environment, 1);
    game.batch_change_after = true;
    check(host.run_console("batch-once body_epoch_or_observation_changed 4"), "post-query fence did not discard whole batch\n" + environment.all());
    ticks(host, environment, 1);
    game.batch_change_after = false;
    game.batch_failure_ray = 1;
    check(host.run_console("batch-once query_unavailable 4"), "partial unavailable geometry escaped\n" + environment.all());
    ticks(host, environment, 1);
    game.batch_failure_ray = std::numeric_limits<unsigned>::max();
    game.query_cast_cost = 3;
    check(host.run_console("batch-once ray_cast_limit_exhausted 2"), "per-ray two-cast bound waived\n" + environment.all());
    ticks(host, environment, 1);
    game.query_cast_cost = 2;
    game.batch_bad_count = true;
    before = game.batch_calls;
    check(host.run_console("batch-once invalid_query_backend_result 0") && game.batch_calls == before + 1, "malformed backend accounting did not close tick\n" + environment.all());
    ticks(host, environment, 1);
    game.batch_bad_count = false;
    game.batch_clear = true;
    check(host.run_console("batch-clear"), "all-clear full batch failed\n" + environment.all());
    ticks(host, environment, 1);
    game.batch_clear = false;
    check(host.run_console("batch-unknown"), "a hit carried a body token\n" + environment.all());
    const auto canonical = game.last_batch.observation;
    auto& selected = game.owned.at(1);
    const auto sequence = game.body_command_sequence;
    for (unsigned field = 1; field <= 4; ++field) {
        ticks(host, environment, 1);
        selected.sample = selected.latest = canonical;
        game.batch_drift_field = field;
        check(host.run_console("batch-once body_epoch_or_observation_changed 4") && body_query::same_sample(selected.latest, canonical) &&
            selected.sample.epoch == canonical.epoch && !selected.mailbox.pending && !selected.mailbox.count && game.body_command_sequence == sequence,
            "same-epoch native pose/velocity drift escaped or query published its read-only observation\n" + environment.all());
    }
    game.batch_drift_field = 0;
    ticks(host, environment, 1);
    selected.sample = selected.latest = canonical;
    selected.sample.velocity[0] = .001f;
    casts = game.native_casts;
    check(host.run_console("batch-once body_epoch_or_observation_changed 0") && game.native_casts == casts && body_query::same_sample(selected.latest, canonical),
        "pre-query native drift was relabeled as a canonical same-epoch sample\n" + environment.all());
    for (const char* route : {"batch-latest read", "batch-latest receipt"}) {
        ticks(host, environment, 1);
        selected.sample = selected.latest = canonical;
        selected.sample.position[2] += .001f;
        casts = game.native_casts;
        const bool receipt = std::string_view(route).ends_with("receipt");
        if (receipt) {
            // Requested99 -> produced100 is applied but cannot be confirmed by another
            // serial100 observation. Receipt must still reread native pose and replace latest.
            owned_body::Command prior;
            prior.epoch = canonical.epoch;
            --prior.epoch.serial;
            prior.epoch.time -= prior.epoch.seconds;
            owned_body::Command applied;
            check(selected.mailbox.submit(prior, prior.epoch, 0, 1) && selected.mailbox.consume(canonical.epoch, 1, applied) &&
                selected.mailbox.finish(true, true, true) && selected.mailbox.needs_successor(), "applied/unmeasured receipt setup");
        }
        check(host.run_console(route) && game.native_casts == casts && body_query::same_sample(selected.latest, selected.sample) &&
            !body_query::same_sample(selected.latest, canonical) && selected.latest.epoch == canonical.epoch && selected.mailbox.count == (receipt ? 1u : 0u) &&
            (!receipt || (selected.mailbox.oldest().outcome == owned_body::Outcome::applied && !selected.mailbox.oldest().successor_available)),
            "same-epoch read/receipt latest replacement rebound an older immutable query token\n" + environment.all());
    }
    selected.sample = selected.latest = canonical;
    ticks(host, environment, 1);
    game.body_producer_step();
    casts = game.native_casts;
    check(host.run_console("batch-expired") && game.native_casts == casts && game.body_submissions == 0, "expired query mutated command admission\n" + environment.all());
    running(host, "batch-budget", environment);
}

void physics_claims_are_bounded_and_exclusive() {
    ObjectPhysicsClaims claims;
    check(!claims.claim("a", 0).empty(), "Zero is not a layout ID");
    check(!claims.claim("", 1).empty(), "A claim needs an owner");
    for (std::uint64_t id = 1; id <= 32; ++id) check(claims.claim("a", id).empty(), "A plugin can claim 32 objects");
    check(claims.claim("a", 1).empty(), "An owner may update its existing claim at the limit");
    check(!claims.claim("b", 1).empty() && claims.owns("a", 1), "Another plugin cannot steal a physics claim");
    check(!claims.claim("a", 33).empty(), "A plugin cannot claim 33 objects");
    for (std::uint64_t id = 33; id <= 128; ++id)
        check(claims.claim(std::to_string((id - 1) / 32), id).empty(), "Other plugins share the bounded global pool");
    check(!claims.claim("fifth", 129).empty(), "The process limit is 128 objects");
    claims.erase(1);
    check(claims.claim("fifth", 129).empty(), "Releasing a claim frees capacity");
    claims.clear();
    check(!claims.owns("a", 2), "World reset clears claims");
    check(!valid_object_velocity({46, 0, 0}) && !valid_object_velocity({32, 32, 0}), "Velocity bounds apply to magnitude");
    check(!valid_object_rotation({0, 0, 0, 0}) && valid_object_rotation({0, 0, 0, 1}), "Rotation must be a unit quaternion");
}

void marker_consumers_do_not_replay_or_steal_operations() {
    MarkerJournal journal;
    journal.push(1, true);
    check(journal.poll("a", 1)->events.empty(), "A new subscriber ignores prior operations");
    check(journal.poll("b", 1)->events.empty(), "Subscribers start independently");
    journal.push(2, true); journal.push(1, false);
    check(journal.poll("a", 1)->events == std::vector<std::string>{"returned"}, "Only the local owner's marker is exposed");
    check(journal.poll("b", 1)->events == std::vector<std::string>{"returned"}, "One subscriber does not drain another's events");
    check(journal.poll("a", 1)->events.empty(), "An operation is consumed once");
    for (unsigned i = 0; i < 33; ++i) journal.push(1, true);
    const auto overflow = journal.poll("a", 1);
    check(overflow->overflow && overflow->events.size() == 32, "Slow consumers get bounded history and an overflow notice");
    journal.reset();
    check(journal.poll("b", 1)->events.empty(), "Level reset drops pending operations");
    journal.push(1, false); journal.release("a");
    check(journal.poll("a", 1)->events.empty(), "Reload does not replay operations from the prior instance");
    for (unsigned i = 0; i < 126; ++i) check(journal.poll(std::to_string(i), 1).has_value(), "Consumer slots are bounded");
    check(!journal.poll("too-many", 1), "A process has at most 128 consumers");
    journal.release("a");
    check(journal.poll("replacement", 1).has_value(), "Unload frees a consumer slot");
}

void plugins_control_only_claimed_object_physics() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    environment.fake.marker_feed["physics"] = {{"saved", "returned"}, false};
    host.apply({script("physics", R"(
        assert(reskate.objects.physics("42", {mass=25, dynamic=true}))
        assert(reskate.objects.velocity("42", vector.create(3,0,4)))
        local motion = reskate.objects.motion("42")
        assert(motion.dynamic and motion.velocity.x == 3 and motion.rotation[4] == 1)
        assert(reskate.objects.pose("42", vector.create(2,3,4), {0,0,0,1}))
        assert(reskate.objects.motion("42").position.y == 3)
        for _, id in ipairs({"0", "-1", "42tail", "42\0tail", "18446744073709551616"}) do
            assert(not pcall(reskate.objects.physics, id, {}))
        end
        assert(not pcall(reskate.objects.physics, 42, {}))
        assert(not pcall(reskate.objects.physics, "42", {mass=0/0}))
        assert(not pcall(reskate.objects.physics, "42", {mass=math.huge}))
        assert(not pcall(reskate.objects.physics, "42", {mass=0}))
        assert(not pcall(reskate.objects.physics, "42", {mass=20001}))
        assert(not pcall(reskate.objects.physics, "42", {mass=.09}))
        assert(not pcall(reskate.objects.physics, "42", {surface=1}))
        assert(not pcall(reskate.objects.physics, "42", {mass="25"}))
        assert(not pcall(reskate.objects.physics, "42", {dynamic=1}))
        assert(not pcall(reskate.objects.velocity, "42", vector.create(32,32,0)))
        assert(not pcall(reskate.objects.velocity, "42", vector.create(0/0,0,0)))
        assert(not pcall(reskate.objects.pose, "42", vector.create(0,0,0), {0,0,0,0}))
        assert(not pcall(reskate.objects.pose, "42", vector.create(0,0,0), {0,0,0,"1"}))
        assert(not reskate.objects.physics("99", {}))
        local events, overflow = reskate.marker.events()
        assert(#events == 2 and events[1] == "saved" and events[2] == "returned" and not overflow)
        assert(#reskate.marker.events() == 0)
    )", R"("objects.physics", "marker.read")")});
    running(host, "physics", environment);
    check(environment.fake.object_physics_now.at(42).mass == 25, "Typed physics options reach the game");
    host.apply({script("sneaky", R"(
        assert(not pcall(reskate.objects.physics, "42", {}))
        assert(not pcall(reskate.objects.physics, "42", nil))
        assert(not pcall(reskate.objects.velocity, "42", vector.create(1,0,0)))
        assert(not pcall(reskate.objects.pose, "42", vector.create(0,0,0), {0,0,0,1}))
        assert(not pcall(reskate.objects.motion, "42"))
        assert(not pcall(reskate.marker.events))
        assert(not pcall(reskate.skater.movement))
        assert(not pcall(reskate.objects.contact, vector.create(0,0,0), vector.create(1,0,0)))
    )")});
    running(host, "sneaky", environment);
    check(environment.fake.object_physics_now.empty(), "Stopping a plugin releases its physics claims");
    Game absent;
    check(absent.object_physics("p", 42, ObjectPhysics{}) == unavailable && !absent.marker_events("p"), "No native provider means unavailable");
    Host server(environment, Side::server);
    server.apply({package("server-physics", {{manifest_file, manifest(R"("objects.physics", "marker.read")", "server")},
        {"plugin/main.lua", R"(
            assert(not pcall(reskate.objects.physics, "42", {}))
            assert(not pcall(reskate.objects.motion, "42"))
            assert(not pcall(reskate.objects.pose, "42", vector.create(0,0,0), {0,0,0,1}))
            assert(not pcall(reskate.objects.velocity, "42", vector.create(0,0,0)))
            assert(not pcall(reskate.marker.events))
        )"}})});
    running(server, "server-physics", environment);
}

void moving_edges_keep_their_authored_identity() {
    namespace geometry = dingosdk::placement_geometry;
    geometry::Edge edge;
    edge.from = {1, 0, 0, 7}; edge.to = {2, 0, 0, 8};
    edge.normal_a = {0, 1, 0, 9}; edge.normal_b = {0, 0, 1, 10};
    edge.identity = {123, 456}; edge.details = {7, 8, 9, 10}; edge.flags = {3, 4, 5, 6};
    const auto rotated = geometry::transform(edge, {0, 0, .70710678f, .70710678f}, {3, 4, 5});
    check(geometry::distance2(geometry::xyz(rotated.from), {3, 5, 5}) < .00001f, "Moving edges follow rotation and translation");
    check(rotated.identity == edge.identity && rotated.details == edge.details && rotated.flags == edge.flags, "Moving edges preserve native chain/shape metadata");
    check(rotated.from[3] == 7 && rotated.normal_a[3] == 9, "Opaque metadata lanes are not transformed");
    check(geometry::overlaps(rotated, {3, 5, 5}, {.1f, .1f, .1f}) && !geometry::overlaps(rotated, {0, 0, 0}, {.1f, .1f, .1f}), "Restored edges obey query bounds");
    const auto restored = geometry::transform(rotated, {0, 0, -.70710678f, .70710678f}, {-4, 3, -5});
    check(geometry::distance2(geometry::xyz(restored.from), geometry::xyz(edge.from)) < .00001f, "Local edge snapshots round-trip through moving transforms");
}

void object_contact_queries_are_owned_and_budgeted() {
    TestEnvironment environment;
    environment.fake.contact_now = ObjectContact{42, {0,0,1}};
    environment.fake.movement_now = SkaterMovement{};
    environment.fake.movement_now->focused = true;
    environment.fake.movement_now->stick = {0, 0, 1};
    Host host(environment, Side::client);
    host.apply({script("queries", R"(
        assert(reskate.objects.physics("42", {mass=.3,surface=true}))
        assert(reskate.objects.motion("42").surface)
        local movement=reskate.skater.movement()
        assert(movement.focused and movement.walking_velocity and movement.stick.z==1 and movement.device==0)
        assert(not pcall(reskate.objects.contact,vector.create(0,0,0),vector.create(11,0,0)))
        assert(not pcall(reskate.objects.contact,vector.create(0/0,0,0),vector.create(1,0,0)))
        for i=1,64 do
            local hit=reskate.objects.contact(vector.create(0,0,0),vector.create(0,0,1))
            assert(hit.id=="42" and hit.position.z==1)
        end
        assert(not pcall(reskate.objects.contact,vector.create(0,0,0),vector.create(0,0,1)))
        reskate.on("tick",function()
            assert(reskate.objects.contact(vector.create(0,0,0),vector.create(0,0,1)))
        end)
    )", R"("objects.physics","skater.read","input")"),
    script("outsider", R"(
        reskate.on("tick",function()
        assert(not reskate.objects.physics("42",{}))
        assert(not reskate.objects.physics("42",nil))
        assert(reskate.objects.motion("42")==nil)
        assert(reskate.objects.contact(vector.create(0,0,0),vector.create(0,0,1))==nil)
        assert(not pcall(reskate.skater.movement))
        end)
    )", R"("objects.physics")"),
    script("reader", R"(
        -- skater.read alone: the movement, without the stick, controller or focus ("input").
        local movement=assert(reskate.skater.movement())
        assert(movement.velocity and movement.walking_velocity and movement.on_foot==false)
        assert(movement.stick==nil and movement.device==nil and movement.focused==nil)
    )", R"("skater.read")")});
    ticks(host,environment,2);
    running(host,"queries",environment);running(host,"outsider",environment);running(host,"reader",environment);
}

void ui_value_reservations_enforce_limits_and_collisions() {
    ui_values::Reservations registry;
    ui_values::Values first, rest;
    for (int i = 0; i < 20; ++i) first.emplace("v" + std::to_string(i), static_cast<float>(i));
    for (int i = 20; i < 32; ++i) rest.emplace("v" + std::to_string(i), static_cast<float>(i));
    check(registry.reserve("quota", first).empty() && registry.reserve("quota", rest).empty(),
          "Different partial calls merge into 32 lifetime reservations");
    check(registry.records().size() == 32, "The quota counts the union, not the last batch");
    check(registry.reserve("quota", {{"v0", 200.f}}).empty() && registry.records().size() == 32,
          "Re-publishing an existing name does not consume quota");
    check(registry.reserve("quota", {{"v32", 1.f}}) == "UI value limit reached",
          "The 33rd lifetime name is refused");
    const auto before = registry.records().size();
    check(registry.reserve("new", {{"good", 1.f}, {"bad", std::numeric_limits<float>::infinity()}}) == "invalid UI value" &&
          registry.records().size() == before, "Invalid batches are rejected without partial reservation");
    check(ui_values::symbol("layout", "sHaG9mdHCM") == ui_values::symbol("layout", "yX391OQUvL"),
          "Same-batch collision fixture hashes match");
    check(registry.reserve("layout", {{"sHaG9mdHCM", 1.f}, {"yX391OQUvL", 2.f}}) ==
              "UI symbol collision or reserved symbol", "Same-batch hash collisions fail closed");
    // Known collision across distinct plugin namespaces, not merely within one batch.
    check(ui_values::symbol("alpha", "enfwbtm118vb") == ui_values::symbol("bravo", "aixvpnvhxvhq"),
          "The cross-plugin collision fixture is real");
    check(registry.reserve("alpha", {{"enfwbtm118vb", 1.f}}).empty(), "First claimant reserves a symbol");
    check(registry.reserve("bravo", {{"aixvpnvhxvhq", 2.f}}) == "UI symbol collision",
          "A different plugin cannot claim the same symbol");
    check(registry.records().size() == before + 1, "Collisions do not leak reservations");

    ui_values::Reservations process;
    for (int plugin = 0; plugin < 8; ++plugin) {
        ui_values::Values batch;
        for (int name = 0; name < 32; ++name) batch.emplace("v" + std::to_string(name), static_cast<float>(name));
        check(process.reserve("process" + std::to_string(plugin), batch).empty(), "Eight plugins fill the process quota");
    }
    check(process.records().size() == ui_values::maximum_process_values, "Exactly 256 names were reserved");
    check(process.reserve("ninth", {{"one", 1.f}}) == "UI value limit reached" &&
          process.records().size() == ui_values::maximum_process_values, "257th name is refused atomically");

    const auto id = ui_values::symbol("quota", "v0");
    check(!registry.unchanged("quota", {{"v0", 0.f}}, 0x1234), "A reservation alone is not published");
    registry.mark_published(id, 0x1234, 0x5678, 0.f);
    check(registry.unchanged("quota", {{"v0", 0.f}}, 0x1234), "Identical value in the same manager is a no-op");
    check(!registry.unchanged("quota", {{"v0", 1.f}}, 0x1234), "A changed float must publish");
    check(!registry.unchanged("quota", {{"v0", 0.f}}, 0x2222), "A new manager must re-register");
    check(!registry.unchanged("quota", {{"v0", 0.f}, {"v1", 1.f}}, 0x1234),
          "An unregistered name forces publication");
}

void plugins_publish_only_their_own_ui_values() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("layout", R"(
        assert(type(reskate.ui.symbol("left")) == "number")
        assert(reskate.ui.symbol("left") ~= reskate.ui.symbol("top"))
        assert(reskate.ui.values({left = 100, top = 165, width = 620}))
        assert(reskate.ui.values({width = 625}))
        assert(not pcall(reskate.ui.values, {width = 0/0}))
        assert(not pcall(reskate.ui.values, {width = math.huge}))
        assert(not pcall(reskate.ui.values, {width = 100001}))
        assert(not pcall(reskate.ui.values, {width = "620"}))
        assert(not pcall(reskate.ui.values, {[1] = 2}))
        assert(not pcall(reskate.ui.values, {["other.left"] = 2}))
        assert(not pcall(reskate.ui.values, {["left\0tail"] = 2}))
        assert(not pcall(reskate.ui.symbol, ""))
        assert(not pcall(reskate.ui.symbol, "../stock"))
        local many = {}
        for i = 1, 33 do many["v" .. i] = i end
        assert(not pcall(reskate.ui.values, many))
        local ok, why = reskate.ui.values({sHaG9mdHCM = 1, yX391OQUvL = 2})
        assert(not ok and why == "UI symbol collision or reserved symbol")
    )", R"("ui.models")")});
    running(host, "layout", environment);
    check(environment.fake.ui_models.at("layout").at("width") == 625 &&
          environment.fake.ui_models.at("layout").at("left") == 100 &&
          environment.fake.ui_models.at("layout").at("top") == 165,
          "Partial UI updates merge instead of replacing prior fields");
    check(ui_values::symbol("layout", "left") != ui_values::symbol("other", "left"), "UI values are namespaced by plugin");
    environment.fake.ui_error = "unavailable here";
    host.apply({script("notready", R"(
        local ok, why = reskate.ui.values({width = 620})
        assert(not ok and why == "unavailable here")
    )", R"("ui.models")")});
    running(host, "notready", environment);
    host.apply({script("sneaky", R"(
        assert(not pcall(reskate.ui.values, {left = 1}))
        assert(not pcall(reskate.ui.symbol, "left"))
    )")});
    running(host, "sneaky", environment);
    check(!environment.fake.ui_models.contains("sneaky"), "No permission means no UI model write");
    // A listen host has a real game provider, so refusing server copies must
    // happen at the API layer, not rely on Game's default provider.
    TestEnvironment server_environment;
    Host server(server_environment, Side::server, {}, "@server");
    server.apply({package("server-layout", {
        {manifest_file, manifest(R"("ui.models")", "both")},
        {"plugin/main.lua", R"(
            local ok, why = reskate.ui.values({left = 8})
            assert(not ok and why == "ui.models is client-only")
            assert(not pcall(reskate.ui.symbol, "left"))
        )"}
    })});
    running(server, "server-layout", server_environment);
    check(server_environment.fake.ui_models.empty() && server_environment.fake.ui_reservations.records().empty(),
          "Listen-server copies never reserve or publish native UI values");
    Game absent;
    check(absent.publish_ui_values("layout", {{"left", 1.f}}) == unavailable,
          "Dedicated-server Game provider is unavailable");
}

void a_plugin_loads_ticks_runs_its_commands_and_unloads() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("hello", R"(
        local ticks = 0
        reskate.on("load", function() reskate.log("loaded " .. reskate.id .. " api " .. reskate.api_version) end)
        reskate.on("tick", function(dt) ticks += 1 end)
        reskate.on("unload", function() reskate.log("bye after " .. ticks) end)
        reskate.console.command("hello there", "Says hello", function(args)
            reskate.console.print("hello " .. table.concat(args, ",") .. " after " .. ticks)
        end)
        print("printed", 1, true)
        reskate.console.command("greet", "Greets", function(args) reskate.console.print("greet " .. (args[1] or "")) end,
            {{name = "loud", args = "[name]", help = "Greets loudly"}, {name = "soft"}})
        assert(not pcall(reskate.console.command, "bad", "x", function() end, {{name = "Two Words"}}), "a subcommand is one word")
    )", R"("console")")});
    running(host, "hello", environment);
    check(environment.logged("[hello] loaded hello api 1"), "The load handler ran\n" + environment.all());
    check(environment.logged("[hello] printed\t1\ttrue"), "print goes to the log");
    ticks(host, environment, 3);
    check(host.run_console("hello there a b"), "Its command runs");
    check(environment.printed("hello a,b after 3"), "with its arguments, after its ticks");
    check(host.run_console("HELLO There"), "whatever the case");
    check(!host.run_console("nothing here"), "and other text is left to ReSkate");
    check(status_of(host, "hello").commands.size() == 4, "Its commands are listed, with the subcommands one declared");
    {
        const auto commands = status_of(host, "hello").commands;
        check(std::any_of(commands.begin(), commands.end(),
                          [](const auto& c) { return c.name == "greet loud" && c.args == "[name]" && c.help == "Greets loudly"; }),
              "A subcommand is listed with its arguments and help");
    }
    check(host.run_console("greet loud") && environment.printed("greet loud"), "and its command's function runs it");
    host.apply({});
    check(environment.logged("[hello] bye after 3"), "Unloading runs its unload handler");
    check(host.status().empty(), "and it is gone");
    check(!host.run_console("hello there"), "with its command");
}

// Another player's outfit read, and items hidden on everyone or on one player: the calls, their
// checks and both permissions; and the list the session reads, with each plugin's going when it does.
void plugins_read_and_hide_other_players_items() {
    namespace feed = dingosdk::plugin_feed;
    feed::hide_items("a", {{"own_skull_mask"}, {{7, {"own_nolan_tee"}}}});
    const auto before = feed::hidden_items_revision();
    feed::hide_items("b", {{"own_hat"}, {}});
    check(feed::hidden_items_revision() != before, "A plugin's new list changes the revision, so outfits are put on again");
    const auto same = feed::hidden_items_revision();
    feed::hide_items("b", {{"own_hat"}, {}});
    check(feed::hidden_items_revision() == same, "The same list again changes nothing: no player is dressed again for it");
    check(feed::item_hidden(7, "own_skull_mask") && feed::item_hidden(9, "own_skull_mask") && feed::item_hidden(7, "own_nolan_tee") &&
              !feed::item_hidden(9, "own_nolan_tee") && feed::item_hidden(9, "own_hat"),
          "An item hidden on everyone, one hidden on one player only, and every plugin's list together");
    feed::release_hidden_items("a");
    check(!feed::item_hidden(7, "own_skull_mask") && feed::item_hidden(7, "own_hat"), "A plugin's list goes when it does");
    feed::hide_items("b", {});
    check(!feed::item_hidden(7, "own_hat"), "Empty lists take a plugin's away");

    TestEnvironment environment;
    auto& game = environment.fake;
    environment.session_now = {true, false, {{1, "Me", true, true}, {7, "Ann", false, false}, {8, "Bob", false, false}}};
    game.outfits[7] = {{"Own_Nolan_Tee", "Own_Skull_Mask"}, {"Own_Deck_X"}};
    game.outfits[1] = {{"Own_Mine"}, {}};
    Host host(environment, Side::client);
    host.apply({script("filter", R"(
      reskate.console.command("go", "x", function()
        local ann = reskate.session.outfit("7")
        assert(ann and ann.skater[1] == "Own_Nolan_Tee" and ann.skater[2] == "Own_Skull_Mask" and ann.board[1] == "Own_Deck_X")
        assert(reskate.session.outfit({id = "7"}).board[1] == "Own_Deck_X", "a player table works too")
        assert(reskate.session.outfit("8") == nil, "no outfit yet")
        assert(reskate.session.outfit("9") == nil, "not in the session")
        assert(reskate.session.outfit("1") == nil, "not this player's own")
        assert(not pcall(reskate.session.outfit, "nobody"), "a player")
        assert(reskate.session.hide_items({items = {"Own_Skull_Mask"}, players = {["7"] = {"Own_Nolan_Tee"}}}))
        assert(reskate.session.hide_items(nil))
        assert(not pcall(reskate.session.hide_items, {items = {""}}), "an item has a name")
        assert(not pcall(reskate.session.hide_items, {items = {12}}), "an item is a name")
        assert(not pcall(reskate.session.hide_items, {items = "Own_Hat"}), "items is a list")
        assert(not pcall(reskate.session.hide_items, {players = {[7] = {"Own_Hat"}}}), "players is keyed by ids")
        assert(not pcall(reskate.session.hide_items, {players = {["x"] = {"Own_Hat"}}}), "an id is a player's")
        local many = {}
        for i = 1, 4097 do many[i] = "Own_" .. i end
        assert(not pcall(reskate.session.hide_items, {items = many}), "at most 4096 names")
        local crowd = {}
        for i = 1, 65 do crowd[tostring(100 + i)] = {"Own_Hat"} end
        assert(not pcall(reskate.session.hide_items, {players = crowd}), "items of at most 64 players")
        crowd[tostring(101)] = nil
        assert(reskate.session.hide_items({players = crowd}), "64 players is fine")
        assert(reskate.session.hide_items({items = {"Own_A"}}))
        for _ = 1, 20 do reskate.session.hide_items({items = {"Own_C"}}) end
        local ok, why = reskate.session.hide_items({items = {"Own_B"}})
        assert(ok == false and why == "asked more than 20 times in a second", why)
        reskate.log("hid")
      end)
      reskate.console.command("again", "x", function()
        assert(reskate.session.hide_items({items = {"Own_B"}}), "a second later it can change again")
        reskate.log("again")
      end)
    )", R"("console", "session.read", "session.skaters")"), script("reader", R"(
        local ok, err = pcall(reskate.session.hide_items, {items = {"Own_Hat"}})
        assert(not ok and err:find("needs the \"session.skaters\" permission"), err)
        reskate.log("refused hiding")
    )", R"("session.read")"), script("hider", R"(
        local ok, err = pcall(reskate.session.outfit, "7")
        assert(not ok and err:find("needs the \"session.read\" permission"), err)
        reskate.log("refused reading")
    )", R"("session.skaters")")});
    ticks(host, environment, 1); // the session reaches the plugins at a tick
    host.run_console("go");
    environment.now += 1000;
    host.run_console("again");
    running(host, "filter", environment);
    running(host, "reader", environment);
    running(host, "hider", environment);
    check(environment.logged("[filter] hid") && environment.logged("[filter] again") && environment.logged("[reader] refused hiding") && environment.logged("[hider] refused reading"),
          "The calls ran\n" + environment.all());
    check(game.called("hide-items filter Own_Skull_Mask 7:Own_Nolan_Tee") && std::ranges::count(game.calls, std::string("hide-items filter")) == 1,
          "A list on everyone and on one player, then none\n" + environment.all());
    check(game.called("hide-items filter Own_A") && game.called("hide-items filter Own_B") &&
              std::ranges::count(game.calls, std::string("hide-items filter Own_B")) == 1,
          "A change past the twentieth in a second is refused and reaches nothing; a second later it goes through\n" + environment.all());
    check(!game.called("hide-items reader") && !game.called("hide-items hider Own_Hat"), "A plugin without the permission hides nothing");
}

// What this PC shows in place of other players' items it does not have: asked with
// "outfit.stand_ins", the options checked, and refused without the permission.
void plugins_choose_stand_ins() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    host.apply({script("dresser", R"(
        assert(reskate.outfit.stand_ins({pick = "random", seed = 7}))
        assert(reskate.outfit.stand_ins({pick = "first", parts = "board", items = {"Own_Truck_Gen_Default_00001"}}))
        assert(reskate.outfit.stand_ins(nil))
        assert(not pcall(reskate.outfit.stand_ins, {pick = "sometimes"}), "pick is first or random")
        assert(not pcall(reskate.outfit.stand_ins, {parts = "hat"}), "parts is both, skater or board")
        assert(not pcall(reskate.outfit.stand_ins, {seed = -1}), "a seed is 0 or more")
        assert(not pcall(reskate.outfit.stand_ins, {items = {""}}), "an item has a name")
        assert(not pcall(reskate.outfit.stand_ins, {items = {12}}), "an item is a name")
        local rules = {"Own_Costume*", "Own_TopShirt_Gen_Hoodie_00003"}
        for i = 3, 64 do rules[i] = "*Rule" .. i .. "*" end
        assert(reskate.outfit.stand_ins({pick = "random", exclude = rules}))
        rules[65], rules[66] = "*Rule65*", "*Rule66*" -- 65 with a star, beside the plain name
        local ok, err = pcall(reskate.outfit.stand_ins, {exclude = rules})
        assert(not ok and tostring(err):find("at most 64 names with"), "at most 64 exclude rules with a star")
        -- A second on (at most 4 changes a second): plain names go far past 64.
        reskate.time.after(1100, function()
            local names = {}
            for i = 1, 4096 do names[i] = "Own_Item_" .. i end
            assert(reskate.outfit.stand_ins({pick = "random", exclude = names}), "4096 plain names")
            names[4097] = "Own_Item_4097"
            assert(not pcall(reskate.outfit.stand_ins, {exclude = names}), "and no more")
            reskate.log("many names")
        end)
        assert(not pcall(reskate.outfit.stand_ins, {exclude = {"*"}}), "excluding everything is not a name")
        assert(not pcall(reskate.outfit.stand_ins, {exclude = "Own_Hat"}), "exclude is a list")
        reskate.log("dressed")
    )", R"("outfit.stand_ins")"), script("plain", R"(
        local ok, err = pcall(reskate.outfit.stand_ins, {pick = "random"})
        assert(not ok and err:find("needs the \"outfit.stand_ins\" permission"), err)
        reskate.log("refused")
    )", R"("outfit")")});
    running(host, "dresser", environment);
    running(host, "plain", environment);
    check(environment.logged("[dresser] dressed") && environment.logged("[plain] refused"), "The calls ran\n" + environment.all());
    check(game.called("stand-ins dresser random 0 7 all") && game.called("stand-ins dresser first 2 0 Own_Truck_Gen_Default_00001") &&
              game.called("stand-ins dresser default") && game.called("stand-ins dresser exclude Own_Costume*") &&
              game.called("stand-ins dresser exclude Own_TopShirt_Gen_Hoodie_00003") &&
              game.called("stand-ins dresser exclude *Rule64*") && !game.called("stand-ins dresser exclude *Rule65*"),
          "Random from every installed item, the first of a list on the board, and the defaults again\n" + environment.all());
    check(!game.called("stand-ins plain"), "A plugin without the permission chooses no stand-ins");
    ticks(host, environment, 80);
    check(environment.logged("[dresser] many names") && game.called("stand-ins dresser exclude Own_Item_4096") &&
              !game.called("stand-ins dresser exclude Own_Item_4097"),
          "4096 plain names may be left out, and no more\n" + environment.all());
}

// reskate.chat.command: "/name" typed in chat runs the plugin's function with the rest of the line,
// the list reaches the chat and leaves with the plugin, the chat's own names and another plugin's
// are refused, and a plugin without "chat.commands" cannot add one.
void plugins_add_chat_commands() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    host.apply({script("emoter", R"(
        reskate.chat.command("emote", "Play an emote", function(text, words)
            reskate.log("emote [" .. text .. "] " .. #words .. " " .. tostring(words[1]) .. " " .. tostring(words[2]))
        end, "<name|off>")
        assert(not pcall(reskate.chat.command, "vote", "x", function() end), "the chat's own")
        assert(not pcall(reskate.chat.command, "Emote2", "x", function() end), "uppercase")
        assert(not pcall(reskate.chat.command, "two words", "x", function() end), "one word")
        assert(not pcall(reskate.chat.command, "x", "x", function() end), "two letters at least")
        assert(not pcall(reskate.chat.command, "9lives", "x", function() end), "a letter first")
        assert(not pcall(reskate.chat.command, "emote", "x", function() end), "twice")
        assert(not pcall(reskate.chat.command, "line", "two\nlines", function() end), "one line of help")
        assert(not pcall(reskate.chat.command, "nofn", "x", "not a function"), "a function")
        for i = 1, 7 do reskate.chat.command("c" .. i, "x", function() end) end
        assert(not pcall(reskate.chat.command, "c8", "x", function() end), "at most 8")
        reskate.log("commands set")
    )", R"("chat.commands")"), script("plain", R"(
        local ok, err = pcall(reskate.chat.command, "dance", "x", function() end)
        assert(not ok and err:find("needs the \"chat.commands\" permission"), err)
        reskate.log("plain refused")
    )", R"("chat.local")"), script("rival", R"(
        local ok, err = pcall(reskate.chat.command, "emote", "x", function() end)
        assert(not ok and err:find("emoter"), err)
        reskate.log("rival refused")
    )", R"("chat.commands")")});
    running(host, "emoter", environment);
    running(host, "plain", environment);
    running(host, "rival", environment);
    check(environment.logged("[emoter] commands set") && environment.logged("[plain] plain refused") && environment.logged("[rival] rival refused"),
          "The calls ran\n" + environment.all());
    ticks(host, environment, 1);
    const auto& list = game.chat_command_list;
    check(list.size() == 8 && std::any_of(list.begin(), list.end(), [](const auto& c) {
              return c.plugin == "emoter" && c.name == "emote" && c.args == "<name|off>" && c.help == "Play an emote";
          }),
          "The chat is given the plugin's commands");
    game.typed_commands = {{"emote", "floss  now"}, {"dance", "x"}};
    ticks(host, environment, 1);
    check(environment.logged("[emoter] emote [floss  now] 2 floss now"), "A typed command runs with the rest of its line\n" + environment.all());
    check(game.typed_commands.empty(), "and the chat's typed commands are taken");
    host.switch_off("emoter");
    ticks(host, environment, 1);
    check(game.chat_command_list.empty(), "A plugin's chat commands go when it stops");
}

void a_permission_must_be_declared() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("bare", R"(
        local ok, err = pcall(reskate.console.command, "x", "x", function() end)
        assert(not ok and err:find("needs the \"console\" permission"), err)
        ok, err = pcall(reskate.storage.get, "x")
        assert(not ok and err:find("needs the \"storage\" permission"), err)
        -- A mod's variants are read freely, but choosing one (which loads the level again) is asked for.
        assert(reskate.package.variants() == nil, "a mod without variants has none")
        ok, err = pcall(reskate.package.choose, "wide")
        assert(not ok and err:find("needs the \"variants\" permission"), err)
        ok, err = pcall(reskate.tricks.label, "Board Stall", "Rock to Fakie")
        assert(not ok and err:find("needs the \"hud\" permission"), err)
        ok, err = pcall(reskate.tricks.record_as, {9}, 187, 100)
        assert(not ok and err:find("needs the \"skater.control\" permission"), err)
        ok, err = pcall(reskate.mods.content, "parks")
        assert(not ok and err:find("needs the \"mods.read\" permission"), err)
        ok, err = pcall(reskate.board.set_parameter, "grip", "graphic", {r = 1, g = 0, b = 0})
        assert(not ok and err:find("needs the \"board\" permission"), err)
        ok, err = pcall(reskate.board.parameters, "deck")
        assert(not ok and err:find("needs the \"board\" permission"), err)
        ok, err = pcall(reskate.strings.set, "ID_MENU_PLAY", "Play")
        assert(not ok and err:find("needs the \"strings\" permission"), err)
        ok, err = pcall(reskate.strings.set_many, {ID_MENU_PLAY = "Play"})
        assert(not ok and err:find("needs the \"strings\" permission"), err)
        ok, err = pcall(reskate.strings.exists, "ID_MENU_PLAY")
        assert(not ok and err:find("needs the \"strings\" permission"), err)
        ok, err = pcall(reskate.strings.set_case_rules, "tr")
        assert(not ok and err:find("needs the \"strings\" permission"), err)
        assert(reskate.strings.hash("ID_MENU_PLAY"):sub(1, 1) == "#", "a hash is computed freely")
        ok, err = pcall(reskate.board.scale, 6, 1.5)
        assert(not ok and err:find("needs the \"board\" permission"), err)
        ok, err = pcall(reskate.board.mount, {position = vector.create(0, 0, 0)})
        assert(not ok and err:find("needs the \"board\" permission"), err)
        ok, err = pcall(reskate.input.joysticks)
        assert(not ok and err:find("needs the \"input\" permission"), err)
        ok, err = pcall(reskate.input.joystick, 1)
        assert(not ok and err:find("needs the \"input\" permission"), err)
        ok, err = pcall(reskate.hud.score_line, {points = 5})
        assert(not ok and err:find("needs the \"hud.score\" permission"), err)
        ok, err = pcall(reskate.hud.score_line, nil)
        assert(not ok and err:find("needs the \"hud.score\" permission"), err)
        reskate.console.print("never")
    )")});
    stopped(host, "bare", "raised an error");
    check(environment.logged("needs the \"console\" permission"), "The error names the permission\n" + environment.all());
}

// ---- reskate.hud.score_line: a plugin's own line in skate.'s score HUD, display only.
// "hud" alone does not give it: hud.score is a permission of its own.
void a_score_line_needs_its_own_permission() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("corner", R"(
        assert(reskate.hud.corner(false))
        local ok, err = pcall(reskate.hud.score_line, {points = 5})
        assert(not ok and err:find("needs the \"hud.score\" permission"), err)
        reskate.log("hud alone is refused")
    )", R"("hud")")});
    running(host, "corner", environment);
    check(environment.logged("[corner] hud alone is refused") && !environment.fake.called("score_line"),
          "A plugin with only \"hud\" cannot show a score line\n" + environment.all());
}

// What a plugin passes is checked before the game sees it: only the keys there are, their types and
// ranges, and no name the HUD could not show.
void a_score_line_is_checked_before_the_game_sees_it() {
    TestEnvironment environment;
    auto& game = environment.fake;
    game.clock = &environment.now;
    Host host(environment, Side::client);
    host.apply({script("sample-line", R"(
        assert(reskate.hud.score_line({tricks = {"First action", "Second action"}, points = 549.9, line = 4200.7, multiplier = 2.4, timer = 0.5}))
        assert(reskate.hud.score_line({}), "every key has a default")
        assert(reskate.hud.score_line({line = 937, closed = true}))
        assert(reskate.hud.score_line({tricks = {"B\195\188r", string.rep("x", 63), "3", "4", "5"}, points = 999999999, multiplier = 99, timer = 0}))
        local function refused(line, part)
            local ok, err = pcall(reskate.hud.score_line, line)
            assert(not ok and err:find(part, 1, true), tostring(err))
        end
        refused(5, "takes a table")
        refused("First action", "takes a table")
        refused({banner = "Landed"}, "no key \"banner\"")
        refused({[1] = "First action"}, "a line's keys are")
        refused({tricks = "First action"}, "tricks is a list")
        refused({tricks = {a = "First action"}}, "tricks is a list")
        refused({tricks = {"A", nil, "C"}}, "tricks is a list")
        refused({tricks = {1}}, "tricks is a list")
        refused({tricks = {"1", "2", "3", "4", "5", "6"}}, "at most 5 tricks")
        refused({tricks = {""}}, "1 to 63 bytes")
        refused({tricks = {string.rep("x", 64)}}, "1 to 63 bytes")
        refused({tricks = {"First\naction"}}, "no control characters")
        refused({tricks = {"First\127action"}}, "no control characters")
        refused({tricks = {"First\255action"}}, "UTF-8")
        refused({points = 0 / 0}, "points is 0 to 999999999")
        refused({points = -1}, "points is 0 to 999999999")
        refused({points = 1e9}, "points is 0 to 999999999")
        refused({points = math.huge}, "points is 0 to 999999999")
        refused({points = "5"}, "points is a number")
        refused({line = -0.5}, "line is 0 to 999999999")
        refused({multiplier = 0.5}, "multiplier is 1 to 99")
        refused({multiplier = 100}, "multiplier is 1 to 99")
        refused({timer = -0.1}, "timer is 0 to 1")
        refused({timer = 1.1}, "timer is 0 to 1")
        refused({closed = 1}, "closed is true or false")
        assert(reskate.hud.score_line(nil))
        assert(reskate.hud.score_line())
        reskate.log("checked")
    )", R"("hud.score")")});
    running(host, "sample-line", environment);
    check(environment.logged("[sample-line] checked"), "The plugin's lines were taken or refused as they should be\n" + environment.all());
    check(game.called("score_line sample-line tricks=First action|Second action points=549 line=4200 x2.40 timer=0.50 closed=0"),
          "A line reaches the game, its points and total floored");
    check(game.called("score_line sample-line tricks= points=0 line=0 x1.00 timer=1.00 closed=0"), "and an empty one with every default");
    check(game.called("score_line sample-line tricks= points=0 line=937 x1.00 timer=1.00 closed=1"), "and a closed one");
    check(game.called("points=999999999 line=0 x99.00 timer=0.00 closed=0"), "and one at the top of each range");
    const auto lines = std::count_if(game.calls.begin(), game.calls.end(), [](const auto& c) { return c.starts_with("score_line "); });
    check(lines == 6 && game.called("score_line sample-line clear"), "No refused line reached the game, and nil clears it");
}

// A refusal of the game's comes back to the plugin as false and why, and the plugin goes on.
void a_score_line_refusal_comes_back_to_the_plugin() {
    TestEnvironment environment;
    auto& game = environment.fake;
    game.clock = &environment.now;
    game.score_refusal = "never during a throwdown or match";
    Host host(environment, Side::client);
    host.apply({script("sample-line", R"(
        local ok, why = reskate.hud.score_line({tricks = {"First action"}, points = 5})
        assert(ok == false and why == "never during a throwdown or match", tostring(why))
        assert(reskate.hud.score_line(nil), "clearing is never refused")
        reskate.log("told why")
    )", R"("hud.score")")});
    running(host, "sample-line", environment);
    check(environment.logged("[sample-line] told why"), "The refusal reached the plugin\n" + environment.all());
}

// One plugin's line at a time: another is told whose is up until that one's lease of 500 ms after its
// last call runs out, or it stops.
void one_plugins_score_line_shows_at_a_time() {
    TestEnvironment environment;
    auto& game = environment.fake;
    game.clock = &environment.now;
    Host host(environment, Side::client);
    const auto first = script("first", R"(
        reskate.console.command("first line", "x", function()
            local ok, why = reskate.hud.score_line({points = 1})
            reskate.log("first " .. tostring(ok) .. " " .. tostring(why))
        end)
    )", R"("hud.score", "console")");
    host.apply({first, script("second", R"(
        reskate.console.command("second line", "x", function()
            local ok, why = reskate.hud.score_line({points = 2})
            reskate.log("second " .. tostring(ok) .. " " .. tostring(why))
        end)
    )", R"("hud.score", "console")")});
    running(host, "first", environment);
    running(host, "second", environment);
    host.run_console("first line");
    host.run_console("second line");
    check(environment.logged("[first] first true nil") && environment.logged("[second] second false first shows its line there"),
          "The second plugin is told whose line is up\n" + environment.all());
    environment.logs.clear();
    environment.now += 500;
    host.run_console("second line");
    check(environment.logged("[second] second false first shows its line there"), "The first's line holds 500 ms after its last call");
    environment.logs.clear();
    environment.now += 1;
    host.run_console("second line");
    check(environment.logged("[second] second true nil"), "and then the second may show its own\n" + environment.all());
    environment.logs.clear();
    host.run_console("first line");
    check(environment.logged("[first] first false second shows its line there"), "The second's line now holds");
    // The second stopping gives its line up at once.
    environment.logs.clear();
    host.apply({first});
    host.run_console("first line");
    check(std::find(game.released.begin(), game.released.end(), "second") != game.released.end() && environment.logged("[first] first true nil"),
          "A plugin that stops lets its line go\n" + environment.all());
}

void command_names_are_checked() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("first", R"(
        reskate.console.command("dom start", "Starts", function() end)
        assert(not pcall(reskate.console.command, "perf mine", "x", function() end), "ReSkate's own")
        assert(not pcall(reskate.console.command, "plugin mine", "x", function() end), "the plugin command")
        assert(not pcall(reskate.console.command, "Bad Name", "x", function() end), "uppercase")
        assert(not pcall(reskate.console.command, "a b c d", "x", function() end), "four words")
        assert(not pcall(reskate.console.command, "dom start", "x", function() end), "twice")
    )", R"("console")"),
        script("second", R"(
        local ok, err = pcall(reskate.console.command, "dom", "x", function() end)
        assert(not ok and err:find("first"), err)
        reskate.console.command("other", "x", function() end)
    )", R"("console")")});
    running(host, "first", environment);
    running(host, "second", environment);
}

void storage_is_kept_across_loads_and_has_a_quota() {
    TestEnvironment environment;
    Limits limits;
    limits.storage_bytes = 300;
    std::filesystem::remove_all(scratch / "storage" / "keeper");
    const auto keeper = script("keeper", R"(
        local runs = reskate.storage.add("runs")
        reskate.storage.set("table", {a = 1, list = {1, 2, 3}, name = "x"})
        local back = reskate.storage.get("table")
        assert(back.a == 1 and back.list[3] == 3 and back.name == "x")
        reskate.storage.set("gone", true)
        reskate.storage.set("gone", nil)
        assert(reskate.storage.get("gone") == nil)
        local ok, err = pcall(reskate.storage.set, "big", string.rep("x", 1000))
        assert(not ok and err:find("full"), err)
        assert(reskate.storage.get("big") == nil)
        assert(not pcall(reskate.storage.set, "f", print), "a function is not JSON")
        assert(not pcall(reskate.storage.set, "", 1), "an empty key")
        local cycle = {} cycle.me = cycle
        assert(not pcall(reskate.storage.set, "c", cycle), "a cycle")
        reskate.log("runs " .. runs)
    )", R"("storage")");
    {
        Host host(environment, Side::client, limits);
        host.apply({keeper});
        running(host, "keeper", environment);
        host.apply({});
        host.apply({keeper});
        running(host, "keeper", environment);
    }
    check(environment.logged("[keeper] runs 1") && environment.logged("[keeper] runs 2"), "Storage is kept from one load to the next\n" + environment.all());
    check(std::filesystem::exists(scratch / "storage" / "keeper" / "storage.json"), "in the plugin's own file");
}

void packages_json_and_modules() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({package("modules", {
        {manifest_file, manifest()},
        {"data/parks.json", R"({"parks": [{"name": "bowl", "spots": 3}]})"},
        {"data/notes.txt", "hello"},
        {"plugin/lib/util.lua", "loads = (loads or 0) + 1\nreturn {double = function(x) return x * 2 end}"},
        {"plugin/rules.luau", "return {name = 'rules'}"},
        {"plugin/cycle_a.lua", "return require('cycle_b')"},
        {"plugin/cycle_b.lua", "return require('cycle_a')"},
        {"plugin/main.lua", R"(
            assert(reskate.package.read("data/notes.txt") == "hello")
            assert(reskate.package.read("data/missing.txt") == nil)
            local parks = reskate.package.read_json("data/parks.json")
            assert(parks.parks[1].name == "bowl" and parks.parks[1].spots == 3)
            local listing = table.concat(reskate.package.list("data"), ",")
            assert(listing == "notes.txt,parks.json", listing)
            assert(table.concat(reskate.package.list(), ","):find("plugin/"), "folders end in /")
            for _, bad in {"../x", "/x", "C:/x", "data\\notes.txt", "data/../../x"} do
                assert(not pcall(reskate.package.read, bad), bad)
            end
            local util = require("./lib/util")
            assert(util.double(4) == 8)
            assert(require("lib/util") == util and loads == 1, "a module runs once")
            assert(require("rules").name == "rules", ".luau too")
            local ok, err = pcall(require, "cycle_a")
            assert(not ok and err:find("requires itself"), err)
            assert(not pcall(require, "missing"))
            assert(not pcall(require, "../data/notes"))
            local text = reskate.json.encode({a = {1, 2}, b = "x", c = true})
            local back = reskate.json.decode(text)
            assert(back.a[2] == 2 and back.b == "x" and back.c == true, text)
            assert(not pcall(reskate.json.decode, "{nope"))
            assert(not pcall(reskate.json.encode, {[{}] = 1}))
            assert(not pcall(reskate.json.encode, 0/0))
            reskate.log("modules fine")
        )"}})});
    running(host, "modules", environment);
    check(environment.logged("modules fine"), "Packages, JSON and modules work\n" + environment.all());
}

// What a plugin must not be able to reach or change. Each line checks one way out.
void the_sandbox_holds() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("escape", R"(
        assert(io == nil and package == nil and dofile == nil and loadfile == nil, "no files")
        assert(loadstring == nil and load == nil, "no loading code, bytecode or text")
        assert(os.execute == nil and os.remove == nil and os.rename == nil and os.getenv == nil and os.exit == nil, "no os")
        assert(not pcall(function() reskate.log = nil end), "reskate is read only")
        assert(not pcall(function() reskate.storage.get = print end), "and its tables")
        assert(not pcall(rawset, reskate, "x", 1), "even by rawset")
        assert(not pcall(setmetatable, reskate, {}), "or a metatable")
        assert(not pcall(function() string.rep = nil end), "the libraries are read only")
        assert(not pcall(function() getmetatable("").__index = {} end), "and the string metatable")
        assert(not pcall(function() _G.print = nil end) or print ~= nil, "print stays")
        mine = 1
        assert(mine == 1, "its own globals work")
        local function deep(n) return deep(n + 1) + 1 end
        assert(not pcall(deep, 1), "endless recursion is an error, not a crash")
        assert(not pcall(require, "C:/Windows/win.ini"))
        assert(not pcall(reskate.package.read, "../../../Windows/win.ini"))
        assert(not pcall(reskate.time.after, -1, print))
        assert(not pcall(reskate.time.every, 0, print))
        assert(not pcall(reskate.on, "made_up", print))
        assert(not pcall(reskate.log, "x", "loud"))
        reskate.log("sandbox fine")
    )")});
    running(host, "escape", environment);
    check(environment.logged("sandbox fine"), "Every way out is closed\n" + environment.all());
}

// ReSkate's own reading and compiling is not a plugin's running time (Limits::io_ms): modules that
// take far longer to compile than a call may run still load, the time is let off only up to the
// allowance, and a plugin that reads a file in a loop is stopped all the same.
void reskates_own_reading_is_not_a_plugins_time() {
    // About 1 MB of code (the most a source file may be) in a function that never runs. Measured on
    // a 32-thread desktop: the two compile for 130 ms idle and 560 ms with every core busy, while the
    // rest of the load is 1 to 2 ms idle and 3 to 15 ms busy. The limit sits well between the two:
    // at 5 ms a busy machine (another build running) once stopped the first case.
    constexpr double limit_ms = 40;
    std::string big = "local function never()\n";
    while (big.size() < 1'000'000 - 64) big += "    do local a = {1, 2, 3, x = 'y'} if a.x == 'z' then a[1] = a[2] + a[3] end end\n";
    big += "end\nreturn {}\n";
    // The loop after them gives the interrupt its safe points, where it looks at the clock.
    const std::string main = R"(require("big") require("big2") for i = 1, 1000 do end reskate.log("big loaded"))";
    const auto modules = [&](const char* id) {
        return package(id, {{manifest_file, manifest()}, {"plugin/main.lua", main}, {"plugin/big.lua", big}, {"plugin/big2.lua", big}});
    };
    {
        // With a build running on top of that, Windows can still hold the thread past the limit
        // anywhere in the load (twice in ten runs here). That says nothing about compiles: a load
        // that counted them would be stopped every time, so it gets a few tries.
        std::string why;
        for (int attempt = 0; attempt < 5; ++attempt) {
            TestEnvironment environment;
            Limits limits;
            limits.call_ms = limits.hang_ms = limit_ms;
            limits.io_ms = 2000; // two compiles on a busy machine may pass the default 250 ms
            Host host(environment, Side::client, limits);
            host.apply({modules("big-module")});
            const auto status = status_of(host, "big-module");
            if (status.state == "running" && environment.logged("big loaded")) {
                why.clear();
                break;
            }
            why = status.state + ": " + status.reason + "\n" + environment.all();
        }
        check(why.empty(), "A module's compile is not counted against the load's 40 ms, and big-module is " + why);
    }
    {
        TestEnvironment environment;
        Limits limits;
        limits.call_ms = limits.hang_ms = limit_ms;
        limits.io_ms = 0;
        Host host(environment, Side::client, limits);
        host.apply({modules("big-module-counted")});
        stopped(host, "big-module-counted", "ran longer than 40 ms");
    }
    {
        TestEnvironment environment;
        Limits limits;
        limits.call_ms = limits.hang_ms = limit_ms;
        limits.io_ms = 20;
        Host host(environment, Side::client, limits);
        host.apply({package("read-loop", {{manifest_file, manifest()}, {"plugin/main.lua", R"(while true do reskate.package.read("plugin/big.lua") end)"},
                                          {"plugin/big.lua", big}})});
        stopped(host, "read-loop", "ran longer than 40 ms");
    }
}

// A call past call_ms is said, with where the script was, and goes on: one slow call (a hard disk,
// Windows holding the game's thread) is no reason to stop a plugin. One past hang_ms is stopped,
// saying where it was, and unload's own share of time still cuts its handlers off.
// A Steam ID reaches the game exactly: as a string (which Lua also takes for a number, and which
// went through a number and came out as another player), never as a number that cannot hold it.
void a_steam_id_reaches_the_game_exactly() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("watcher", R"(
        reskate.session.spectate("76561198000000001")
        assert(not pcall(reskate.session.spectate, 76561198000000001), "a Steam ID as a number")
        assert(not pcall(reskate.session.spectate, 1.5), "a whole number")
        reskate.session.spectate()
        -- One reading of a player everywhere: digits and nothing after them, a player table, or a
        -- number while it is exact. "123abc" once passed as player 123.
        reskate.session.spectate({id = "76561198000000002"})
        reskate.session.spectate(12345)
        for _, junk in {"76561198000000001x", "123abc", "", " 1", "+1", {id = "123abc"}, {}, true} do
            assert(not pcall(reskate.session.spectate, junk), "session.spectate refuses " .. tostring(junk))
            assert(not pcall(reskate.session.tag, junk, "x"), "session.tag refuses " .. tostring(junk))
            assert(not pcall(reskate.net.send, junk, "x"), "net.send refuses " .. tostring(junk))
        end
        -- "0" is no player, but it stops a Spectate, as the docs say.
        assert(not pcall(reskate.session.tag, "0", "x") and not pcall(reskate.net.send, "0", "x"), "player 0 is no player")
        reskate.session.spectate("76561198000000003")
        reskate.session.spectate("0")
        assert(not pcall(reskate.objects.move, "42abc", vector.create(0, 0, 0)), "an object's id has nothing after its digits")
        assert(not pcall(reskate.objects.remove, 42), "and is a string")
        reskate.log("watched")
    )", R"("session.read", "session.nametags", "net.session", "world.objects")")});
    running(host, "watcher", environment);
    check(environment.logged("watched"), "The plugin ran\n" + environment.all());
    check(environment.fake.called("spectate 76561198000000001"), "The Steam ID arrives exactly\n" + environment.all());
    check(environment.fake.called("spectate 0"), "and nothing stops spectating");
    check(environment.fake.called("spectate 76561198000000002") && environment.fake.called("spectate 12345"),
          "A player table and an exact number are players too\n" + environment.all());
    const auto& calls = environment.fake.calls;
    const auto third = std::find(calls.begin(), calls.end(), "spectate 76561198000000003");
    check(third != calls.end() && std::find(third, calls.end(), "spectate 0") != calls.end(), "\"0\" stops spectating\n" + environment.all());
}

void a_slow_call_is_said_and_a_hung_one_stopped() {
    TestEnvironment environment;
    Limits limits;
    limits.call_ms = 5;
    limits.hang_ms = 300;
    Host host(environment, Side::client, limits);
    host.apply({script("slow", R"(
        local function spin(s) local t = os.clock() while os.clock() - t < s do end end
        local n = 0
        reskate.on("tick", function()
            n += 1
            if n == 1 then spin(0.03) end
            if n == 3 then reskate.log("slow went on") end
        end)
    )"),
                script("hung", R"(reskate.on("tick", function() while true do end end))")});
    running(host, "slow", environment);
    ticks(host, environment, 3);
    running(host, "slow", environment);
    check(environment.logged("[slow] the tick handler took") && environment.logged("over its 5 ms (at plugin/main.lua:") &&
              environment.logged("ReSkate stops a plugin past 300 ms"),
          "A slow call is said, with where it was\n" + environment.all());
    check(environment.logged("slow went on"), "and the plugin goes on\n" + environment.all());
    stopped(host, "hung", "the tick handler ran longer than 300 ms (at plugin/main.lua:1");
}

void a_runaway_plugin_is_stopped_and_the_rest_go_on() {
    TestEnvironment environment;
    Limits limits;
    limits.hang_ms = limits.call_ms; // (a_slow_call_is_said_and_a_hung_one_stopped has the two apart)
    Host host(environment, Side::client, limits);
    const auto start = std::chrono::steady_clock::now();
    host.apply({
        script("bystander", R"(local n = 0 reskate.on("tick", function() n += 1 if n == 30 then reskate.log("bystander ticked 30") end end))"),
        script("loop", "while true do end"),
        script("catches", "while true do pcall(function() while true do end end) end"),
        script("tick_loop", R"(reskate.on("tick", function() while true do end end))"),
        // (100 MB a tick: 512 MB cannot be filled inside one call's 100 ms. Buffers, each its own: Luau keeps one
        // copy of a string however often it is made, so a table of the same long string holds 100 MB, and was
        // only ever stopped for the copies it had let go of.)
        script("memory", R"(local t = {} reskate.on("tick", function() t[#t + 1] = buffer.create(100000000) end))"),
        script("memory_caught", R"(
            local t = {}
            reskate.on("tick", function()
                local ok = pcall(function() t[#t + 1] = buffer.create(100000000) end)
                if not ok then reskate.log("survived the memory bomb") end
            end)
        )"),
        // 250 MB held, and 100 MB made and let go of every tick: never more than 350 MB at once.
        script("churn", R"(
            local held = buffer.create(250000000)
            local last
            reskate.on("tick", function() last = string.rep("z", 100000000) end)
        )"),
        script("huge", R"(local s = string.rep("x", 600000000))"),
        script("buffer", R"(local b = buffer.create(1e9))"),
        script("errors", R"(reskate.on("tick", function() error("boom") end))"),
        script("timers", R"(
            local ok, err = pcall(function() for i = 1, 2000 do reskate.time.after(1000, function() end) end end)
            assert(not ok and err:find("timers"), err)
            reskate.log("timers capped")
        )"),
        script("chatty", R"(for i = 1, 1000 do reskate.log("line " .. i) end)"),
        script("coroutines", R"(while true do coroutine.wrap(function() end)() end)"),
        script("slow_unload", R"(reskate.on("unload", function() while true do end end))"),
    });
    for (const auto* id : {"loop", "catches"}) stopped(host, id, "ran longer than 100 ms");
    stopped(host, "coroutines", ""); // its time or its memory, whichever runs out first
    for (const auto* id : {"huge", "buffer"}) stopped(host, id, "more than 512 MB");
    running(host, "tick_loop", environment);
    ticks(host, environment, 1);
    stopped(host, "tick_loop", "the tick handler ran longer than 100 ms (at plugin/main.lua:1");
    check(environment.logged("[tick_loop] stopped: the tick handler"), "A stop is logged, which the console shows");
    ticks(host, environment, 4); // five errors, with the tick above
    running(host, "errors", environment);
    ticks(host, environment, 1);
    stopped(host, "errors", "more than 5 errors");
    running(host, "timers", environment);
    check(environment.logged("timers capped"), "Timers have a limit");
    const auto chatty = std::count_if(environment.logs.begin(), environment.logs.end(),
        [](const auto& line) { return line.starts_with("[chatty] line "); });
    check(chatty == 50, "50 lines a second, and the rest left out: " + std::to_string(chatty));
    ticks(host, environment, 30);
    for (const auto* id : {"memory", "memory_caught"}) stopped(host, id, "more than 512 MB");
    check(!environment.logged("survived the memory bomb"), "A memory bomb cannot be caught and carried on from");
    running(host, "churn", environment); // what it let go of is collected before it counts against it
    running(host, "bystander", environment);
    check(environment.logged("bystander ticked 30"), "The others go on ticking");
    const auto unloading = std::chrono::steady_clock::now();
    host.apply({});
    const auto unload_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - unloading).count();
    check(unload_ms < 500, "An unload handler that never ends is cut off: " + std::to_string(unload_ms) + " ms");
    const auto total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    check(total < 20, "and none of it hangs the host: " + std::to_string(total) + " s");
}

// The player's own limits (plugins::LimitChoices, as the host takes them): a slow plugin runs on
// where the player has slow plugins left alone, or allowed longer; one already running takes the new
// limits; and with ReSkate's own given back, it is stopped as before.
void the_player_sets_how_strict_the_limits_are() {
    TestEnvironment environment;
    Limits lenient;
    lenient.tick_ms = std::numeric_limits<double>::infinity();
    lenient.errors = std::numeric_limits<int>::max();
    Host host(environment, Side::client, lenient);
    host.apply({script("busy", R"(reskate.on("tick", function() local s = os.clock() while os.clock() - s < 0.020 do end end))"),
        script("faulty", R"(reskate.on("tick", function() error("again") end))")});
    ticks(host, environment, 60 * 7);
    running(host, "busy", environment);
    running(host, "faulty", environment);
    // Allowed 25 ms a tick, 20 ms is within it.
    Limits longer;
    longer.tick_ms = 25;
    host.set_limits(longer);
    ticks(host, environment, 60 * 7);
    running(host, "busy", environment);
    stopped(host, "faulty", "more than 5 errors");
    // ReSkate's own again: the one running is held to them from then on.
    host.set_limits({});
    ticks(host, environment, 60 * 7);
    stopped(host, "busy", "ms a tick for 5 s, over its 16 ms");
}

void a_plugin_over_its_time_budget_is_stopped() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    // 20 ms a tick is over the 16 ms budget; 3 ms, which a slow PC could take for a fair plugin, is not.
    host.apply({script("busy", R"(reskate.on("tick", function() local s = os.clock() while os.clock() - s < 0.020 do end end))"),
        script("steady", R"(reskate.on("tick", function() local s = os.clock() while os.clock() - s < 0.003 do end end))"),
        script("light", R"(reskate.on("tick", function() local s = 0 for i = 1, 100 do s += i end end))")});
    ticks(host, environment, 60 * 7);
    stopped(host, "busy", "ms a tick for 5 s, over its 16 ms");
    running(host, "steady", environment);
    running(host, "light", environment);
}

void a_plugin_reloads_when_its_files_change() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("live", R"(reskate.log("version 1") reskate.on("unload", function() reskate.log("unloading 1") end))")});
    running(host, "live", environment);
    auto changed = script("live", R"(reskate.log("version 2"))");
    changed.fingerprint = "2";
    host.apply({changed});
    check(environment.logged("unloading 1") && environment.logged("version 2"), "A changed plugin unloads and loads again\n" + environment.all());
    host.apply({changed});
    check(std::count_if(environment.logs.begin(), environment.logs.end(), [](const auto& line) { return line.find("version 2") != std::string::npos; }) == 1,
        "and an unchanged one is left alone");
    check(host.switch_off("live").find("switched off") != std::string::npos, "It can be switched off by hand");
    check(status_of(host, "live").state == "switched off", "and stays off");
    changed.fingerprint = "3";
    host.apply({changed});
    check(status_of(host, "live").state == "switched off", "even when its files change");
    check(host.switch_on("live").find("started") != std::string::npos, "until it is switched on");
    running(host, "live", environment);
    check(host.reload("live").find("reloaded") != std::string::npos, "and reloaded by hand");
    check(host.reload("nobody").starts_with("error: "), "A plugin that is not there cannot be");
}

void plugins_distinguish_clear_unavailable_and_hit_world_probes() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("bare-probe", R"(
        local ok, why = pcall(reskate.world.probe, vector.create(1, 2, 3), vector.create(1, -2, 3))
        assert(not ok and why:find("world.raycast"), tostring(why))
        reskate.log("probe permission refused")
    )"), script("surface", R"(
        local from, to = vector.create(1, 2, 3), vector.create(1, -2, 3)
        local function fields(result)
            for key in pairs(result) do
                assert(key == "status" or key == "position" or key == "fraction", "no unqualified native fields")
            end
            assert(result.normal == nil and result.surface_velocity == nil and result.world == nil and result.body == nil)
        end
        reskate.console.command("surface", "x", function(args)
            local result = reskate.world.probe(from, to)
            fields(result)
            assert(result.status == args[1], "distinct probe status: " .. result.status)
            if result.status == "hit" then
                assert(result.position == vector.create(1, 0, 3) and result.fraction == 0.5, "actual hit fields")
            else
                assert(result.position == nil and result.fraction == nil, "only hits carry geometry")
            end
            reskate.log("surface " .. args[1])
        end)
        reskate.console.command("surface-bad", "x", function()
            assert(not pcall(reskate.world.probe, vector.create(0 / 0, 0, 0), to), "NaN")
            assert(not pcall(reskate.world.probe, from, vector.create(math.huge, 0, 0)), "infinity")
            assert(not pcall(reskate.world.probe, vector.create(100001, 0, 0), to), "outside map bounds")
            assert(not pcall(reskate.world.probe, {1, 2, 3}, to), "a vector is required")
            assert(not pcall(reskate.world.probe, vector.create(0, 0, 0), vector.create(1001, 0, 0)), "ray length")
            assert(not pcall(reskate.world.probe, from, to, 1234), "no native handle or exclusion argument")
            local empty = reskate.world.probe(from, from)
            assert(empty.status == "unavailable" and empty.position == nil and empty.fraction == nil, "zero length is not clear")
            reskate.log("bad probe inputs refused")
        end)
        reskate.console.command("surface-budget", "x", function()
            for i = 1, 32 do reskate.world.probe(from, to) end
            for i = 1, 16 do reskate.world.raycast(from, to) end
            for i = 1, 16 do reskate.world.pick(from, to) end
            local ok, why = pcall(reskate.world.probe, from, to)
            assert(not ok and why:find("more than 64 raycasts"), "one shared raycast limit")
            assert(not pcall(reskate.world.raycast, from, to) and not pcall(reskate.world.pick, from, to), "old APIs share the cap")
            reskate.log("probe shared budget refused")
        end)
    )", R"("console", "world.raycast")")});
    running(host, "bare-probe", environment);
    running(host, "surface", environment);
    auto& game = environment.fake;
    check(game.probes == 0 && environment.logged("probe permission refused"), "Permission denial never reaches the backend");
    check(host.run_console("surface unavailable"), "The default backend is unavailable");
    game.probe_result = {true, false, {std::numeric_limits<float>::quiet_NaN(), 0, 0}, std::numeric_limits<float>::infinity()};
    check(host.run_console("surface clear"), "A successful miss stays distinct and exposes no hit fields");
    game.probe_result = {true, true, {1, 0, 3}, 0.5f};
    check(host.run_console("surface hit") && game.probe_from == Vec3{1, 2, 3} && game.probe_to == Vec3{1, -2, 3},
          "The actual segment and hit pass through the generic Game method");
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const Game::WorldProbe malformed[] = {
        {true, true, {nan, 0, 3}, .5f}, {true, true, {inf, 0, 3}, .5f},
        {true, true, {1, 0, 3}, nan}, {true, true, {1, 0, 3}, inf},
        {true, true, {1, 0, 3}, -.1f}, {true, true, {1, 0, 3}, 1.1f},
        {true, true, {2, 0, 3}, .5f}, {true, true, {100001, 0, 3}, .5f},
    };
    for (const auto& value : malformed) {
        game.probe_result = value;
        check(host.run_console("surface unavailable"), "Malformed backend hits are unavailable, not clear or geometry");
    }
    const auto before_bad = game.probes;
    check(host.run_console("surface-bad") && environment.logged("bad probe inputs refused") && game.probes == before_bad,
          "Invalid and zero-length inputs never reach the native query adapter");
    ticks(host, environment, 1);
    const auto before_budget = game.probes;
    const auto rays_before = environment.rays;
    const auto picks_before = game.world_picks;
    check(host.run_console("surface-budget") && environment.logged("probe shared budget refused") && game.probes == before_budget + 32 &&
          environment.rays == rays_before + 16 && game.world_picks == picks_before + 16,
          "world.probe shares world.raycast and world.pick's 64 raycasts a tick, and refused calls never reach their backend");
    ticks(host, environment, 1);
    game.probe_result = {};
    check(host.run_console("surface unavailable") && game.probes == before_budget + 33, "A new tick renews the query budget");
    running(host, "surface", environment);
}

void the_skater_can_be_read_moved_and_the_world_probed() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("bare", R"(
        local ok, err = pcall(reskate.skater.get)
        assert(not ok and err:find("skater.read"), err)
        ok, err = pcall(reskate.skater.read_pose)
        assert(not ok and err:find("skater.read"), err)
        ok, err = pcall(reskate.skater.pose, buffer.create(395 * 40))
        assert(not ok and err:find("skater.animation"), err)
        ok, err = pcall(reskate.skater.teleport, vector.create(0, 0, 0))
        assert(not ok and err:find("skater.teleport"), err)
        ok, err = pcall(reskate.world.raycast, vector.create(0, 1, 0), vector.create(0, -1, 0))
        assert(not ok and err:find("world.raycast"), err)
        reskate.log("bare refused")
    )"), script("mover", R"(
        reskate.console.command("probe", "x", function()
            local s = reskate.skater.get()
            if not s then reskate.log("no skater") return end
            reskate.log(string.format("at %.1f %.1f %.1f facing %.2f speed %.1f up %.1f air %s %.1f", s.position.x, s.position.y,
                s.position.z, s.heading, s.speed, s.vertical_speed, tostring(s.airborne), s.air_time))
            assert(reskate.skater.position() == s.position and reskate.skater.heading() == s.heading)
            assert(reskate.skater.speed() == s.speed and reskate.skater.on_ground() == not s.airborne)
            local hit = reskate.world.raycast(s.position + vector.create(0, 2, 0), s.position - vector.create(0, 5, 0))
            assert(hit and math.abs(hit.y) < 0.01, "the ground under the skater")
            assert(reskate.world.raycast(vector.create(0, 5, 0), vector.create(0, 1, 0)) == nil, "a clear ray")
        end)
        reskate.console.command("move", "x", function(args)
            local ok, why = reskate.skater.teleport(vector.create(10, 0, 20), args[1] and tonumber(args[1]) or nil)
            reskate.log("teleport " .. tostring(ok) .. " " .. tostring(why))
        end)
        reskate.console.command("bad", "x", function()
            assert(not pcall(reskate.skater.teleport, vector.create(0 / 0, 0, 0)), "NaN")
            assert(not pcall(reskate.skater.teleport, vector.create(1e7, 0, 0)), "far away")
            assert(not pcall(reskate.skater.teleport, {1, 2, 3}), "a table is not a vector")
            assert(not pcall(reskate.skater.teleport, vector.create(0, 0, 0), 0 / 0), "a NaN heading")
            assert(not pcall(reskate.world.raycast, vector.create(0, 0, 0), vector.create(2000, 0, 0)), "too long")
            reskate.log("bad refused")
        end)
        reskate.console.command("rays", "x", function()
            local count = 0
            local ok, err = pcall(function()
                for i = 1, 100 do reskate.world.raycast(vector.create(0, 1, 0), vector.create(0, -1, 0)) count += 1 end
            end)
            reskate.log("rays " .. count .. " " .. tostring(err):match("more than %d+ raycasts") or "")
        end)
    )", R"("console", "skater.read", "skater.teleport", "world.raycast")")});
    running(host, "bare", environment);
    check(environment.logged("bare refused"), "Each needs its permission\n" + environment.all());
    host.run_console("probe");
    check(environment.logged("[mover] no skater"), "Without a skater there is nothing to read");
    host.run_console("move");
    check(environment.logged("teleport false teleporting is unavailable"), "nor to move");
    environment.local_skater = SkaterState{{1, 0, 2}, 1.5f, 4, 0.5f, true, 0.25f};
    host.run_console("probe");
    check(environment.logged("at 1.0 0.0 2.0 facing 1.50 speed 4.0 up 0.5 air true 0."), "The skater is read\n" + environment.all());
    running(host, "mover", environment);
    host.run_console("move 0.5");
    check(environment.teleports.size() == 1 && environment.teleports[0].first == Vec3{10, 0, 20} &&
          std::abs(environment.teleports[0].second - 0.5f) < 1e-6f, "A teleport is sent where it was asked");
    environment.now += 1000;
    host.run_console("move");
    check(environment.teleports.size() == 2 && std::abs(environment.teleports[1].second - 1.5f) < 1e-6f, "facing as the skater does by default");
    host.run_console("move");
    host.run_console("move");
    check(environment.teleports.size() == 3 && environment.logged("asked more than twice in a second"), "Two a second");
    environment.now += 1000;
    environment.teleport_forbidden = true;
    host.run_console("move");
    check(environment.logged("the host has turned teleporting off"), "and none when the host says no");
    host.run_console("bad");
    check(environment.logged("bad refused"), "Bad positions are refused\n" + environment.all());
    ticks(host, environment, 1); // the count is per tick: the probes above used two
    host.run_console("rays");
    check(environment.logged("rays 64 more than 64 raycasts"), "64 raycasts a tick\n" + environment.all());
    ticks(host, environment, 1);
    host.run_console("probe");
    running(host, "mover", environment);
}

void teleport_equipment_choices_keep_permissions_and_host_limits() {
    TestEnvironment environment;
    environment.local_skater = SkaterState{{1, 0, 2}, 1.5f, 0, 0, false, 0};
    Host host(environment, Side::client);
    host.apply({script("bare", R"(
        for _, on_board in ipairs({true, false}) do
            local ok, err = pcall(reskate.skater.teleport, vector.create(10, 0, 20), nil, {on_board = on_board})
            assert(not ok and err:find("skater.teleport"), err)
        end
        reskate.log("equipment permission refused")
    )"), script("teleport-options", R"(
        reskate.console.command("choose-teleport", "equipment", function(args)
            local options
            if args[1] == "on" then options = {on_board = true}
            elseif args[1] == "off" then options = {on_board = false}
            elseif args[1] == "empty" then options = {} end
            local ok, why = reskate.skater.teleport(vector.create(10, 0, 20), nil, options)
            reskate.log("choice " .. tostring(ok) .. " " .. tostring(why))
        end)
        reskate.console.command("bad-teleport-options", "validation", function()
            for _, bad in ipairs({false, true, 0, "on", {true}, {on_board = 1}, {on_board = "false"},
                {on_board = {}}, {on_board = function() end}, {on_board = true, stance = 1}, {["on_board\0extra"] = true}}) do
                assert(not pcall(reskate.skater.teleport, vector.create(10, 0, 20), nil, bad), "invalid teleport options")
            end
            reskate.log("equipment options refused")
        end)
    )", R"("console", "skater.teleport")")});
    running(host, "bare", environment);
    check(environment.logged("equipment permission refused") && environment.teleports.empty(),
          "Equipment choices require the existing teleport permission and cannot reach the host without it");
    check(host.run_console("bad-teleport-options") && environment.logged("equipment options refused") && environment.teleports.empty(),
          "Non-tables, non-boolean choices and unknown keys are rejected before forwarding");
    check(host.run_console("choose-teleport on") && host.run_console("choose-teleport off") && environment.teleports.size() == 2 &&
          environment.teleport_equipment == std::vector<std::optional<bool>>{true, false} &&
          environment.teleports[0].first == Vec3{10, 0, 20} && std::abs(environment.teleports[0].second - 1.5f) < 1e-6f,
          "True and false remain distinct per-request choices with the existing default heading");
    host.run_console("choose-teleport");
    check(environment.teleports.size() == 2 && environment.logged("asked more than twice in a second"),
          "Equipment choices and legacy calls share the existing teleport rate limit");
    environment.now += 1000;
    check(host.run_console("choose-teleport") && host.run_console("choose-teleport empty") && environment.teleports.size() == 4 &&
          !environment.teleport_equipment[2].has_value() && !environment.teleport_equipment[3].has_value(),
          "Omitted and empty options forward no equipment override");
    environment.now += 1000;
    environment.teleport_forbidden = true;
    host.run_console("choose-teleport on");
    check(environment.teleports.size() == 4 && environment.logged("the host has turned teleporting off"),
          "An explicit equipment choice cannot bypass the host teleport policy");
    environment.teleport_forbidden = false;
    environment.teleport_unavailable = true;
    host.run_console("choose-teleport off");
    check(environment.teleports.size() == 4 && environment.logged("teleporting is unavailable"),
          "An explicit equipment choice cannot bypass an unavailable skater or load");
    running(host, "teleport-options", environment);
}

void tricks_reach_plugins_that_ask_for_them() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("bare", R"(
        local ok, err = pcall(reskate.on, "trick_landed", function() end)
        assert(not ok and err:find("events.tricks"), err)
        ok, err = pcall(reskate.score.current)
        assert(not ok and err:find("events.tricks"), err)
        reskate.log("bare refused")
    )")});
    check(environment.logged("bare refused") && !host.wants_tricks(), "Tricks need events.tricks, and none is read without it");
    host.apply({script("bare", "reskate.log('bare')"), script("tricks", R"(
        reskate.on("trick_started", function(e) reskate.log(string.format("started at %.0f %.0f %.0f", e.at.x, e.at.y, e.at.z)) end)
        reskate.on("trick_landed", function(e)
            reskate.log(string.format("landed %.0f x%.1f from %.0f to %.0f in %.0f ms", e.points, e.multiplier, e.started_at.x, e.landed_at.x, e.ms))
        end)
        reskate.on("line_broken", function(e) reskate.log("broken by a " .. e.reason) end)
        reskate.console.command("score", "x", function()
            local s = reskate.score.current()
            reskate.log(s and string.format("score %.0f x%.1f %s %.2f", s.line, s.multiplier, tostring(s.in_trick), s.timer) or "no score")
        end)
    )", R"("console", "events.tricks")")});
    check(host.wants_tricks(), "With a plugin that has events.tricks, tricks are read");
    TrickNews started;
    started.started_at = {1, 2, 3};
    host.trick(started);
    TrickNews landed;
    landed.kind = TrickNews::Kind::landed;
    landed.points = 250;
    landed.multiplier = 2;
    landed.started_at = {1, 2, 3};
    landed.landed_at = {9, 2, 3};
    landed.ms = 1200;
    host.trick(landed);
    TrickNews broken;
    broken.kind = TrickNews::Kind::broken;
    broken.reason = "slam";
    host.trick(broken);
    check(environment.logged("[tricks] started at 1 2 3") && environment.logged("[tricks] landed 250 x2.0 from 1 to 9 in 1200 ms") &&
          environment.logged("[tricks] broken by a slam"), "Each trick event reaches it, with its values\n" + environment.all());
    host.run_console("score");
    environment.score_now = ScoreState{708, 2, true, {}, {}, false, 0.5f};
    host.run_console("score");
    check(environment.logged("[tricks] no score") && environment.logged("[tricks] score 708 x2.0 true 0.50"), "and the score HUD as it is");
    running(host, "tricks", environment);
}

void plugins_draw_pictures() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const std::string main = R"(
        reskate.on("draw", function(d)
            d:image("icons/flag.png", vector.create(0, 0, 0), vector.create(64, 64, 0), {colour = 0xffffff80})
            d:world_image("icons/flag.png", vector.create(0, 2, -10), 1.5, {uv = {0, 0, 0.5, 0.25}})
            assert(not pcall(d.image, d, "icons/flag.png", vector.create(0, 0, 0), vector.create(1, 1, 0), {uv = {0, 0, 2, 1}}), "uv 0 to 1")
            assert(not pcall(d.image, d, "notes.txt", vector.create(0, 0, 0), vector.create(1, 1, 0)), "only pictures")
            assert(not pcall(d.image, d, "../other/flag.png", vector.create(0, 0, 0), vector.create(1, 1, 0)), "only its own package")
            assert(not pcall(d.image, d, "icons/missing.png", vector.create(0, 0, 0), vector.create(1, 1, 0)), "only files there")
            assert(not pcall(d.world_image, d, "icons/flag.png", vector.create(0, 0, 0), -1), "a height")
            reskate.log("drew")
        end)
    )";
    host.apply({package("pictures", {{manifest_file, manifest(R"("overlay")")}, {"plugin/main.lua", main}, {"icons/flag.png", "not really a png"},
                                     {"notes.txt", "x"}})});
    ticks(host, environment, 1);
    running(host, "pictures", environment);
    check(environment.logged("[pictures] drew") && environment.layers.size() == 1 && environment.layers[0].shapes.size() == 2,
          "A plugin draws pictures from its package\n" + environment.all());
    const auto& picture = environment.layers[0].shapes[0];
    check(picture.kind == plugin_ui::Shape::Kind::image && picture.text.ends_with("flag.png") && picture.style.colour == 0x80ffffff,
          "as the picture's file, tinted");
    check(environment.layers[0].shapes[1].kind == plugin_ui::Shape::Kind::world_image && environment.layers[0].shapes[1].radius == 1.5f &&
              environment.layers[0].shapes[1].style.uv[2] == 0.5f && environment.layers[0].shapes[1].style.uv[3] == 0.25f,
          "and in the world, so many metres tall, a part of it");
}

// The drawing a plugin with screens of its own needs: lines that stay open, triangles of its own (through
// a picture or not), what was drawn turned, moved and faded after, a letter's own measure, and
// pictures out of its data folder. None of it is one plugin's: a chart, a compass, a label on a slant.
void plugins_draw_meshes_marks_and_data_pictures() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const std::string main = R"(
        local V = vector.create
        reskate.on("draw", function(d)
            -- A chart's line: open, not back to its first point.
            d:poly({V(0, 50, 0), V(20, 10, 0), V(40, 30, 0), V(60, 5, 0)}, {closed = false, thickness = 3})
            d:poly({V(0, 0, 0), V(10, 0, 0), V(0, 10, 0)})
            -- A compass needle: two triangles in their own colours.
            d:mesh({100, 100, 0, 0, 0xff0000ff,  110, 140, 0, 0, 0xff0000ff,  90, 140, 0, 0, 0x800000ff,  100, 180, 0, 0, 0xffffffff},
                   {1, 2, 3, 2, 4, 3})
            -- A picture cut to a triangle.
            d:mesh({0, 0, 0, 0, 0xffffffff,  64, 0, 1, 0, 0xffffffff,  32, 64, 0.5, 1, 0xffffff80}, {1, 2, 3}, {image = "icons/flag.png"})
            assert(not pcall(d.mesh, d, {0, 0, 0, 0}, {1, 2, 3}), "five numbers a corner")
            assert(not pcall(d.mesh, d, {0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 2, 2, 0, 0, 1}, {1, 2, 4}), "a triangle of its own corners")
            assert(not pcall(d.mesh, d, {0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 2, 2, 0, 0, 1}, {1, 2}), "three corners a triangle")
            assert(not pcall(d.mesh, d, {0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 2, 2, 0, 0, 1}, {1, 2, 3}, {image = "../x.png"}), "its own pictures")
            -- A label on a slant: what was drawn since the mark, turned about its middle.
            local label = d:mark()
            d:rect(V(200, 200, 0), V(300, 230, 0), {fill = true})
            d:text(V(210, 205, 0), "SALE")
            d:adjust(label, {rotate = math.rad(-4), about = V(250, 215, 0)})
            -- A list's rows, faded at the ends of its box, and a card sliding in.
            local rows = d:mark()
            d:text(V(10, 400, 0), "a row")
            d:adjust(rows, {fade = {top = 390, bottom = 600, band = 20, taller = 100}, offset = V(0, 12, 0), alpha = 0.5})
            assert(label == 1 and rows == 2, "marks are numbered from 1 each frame")
            assert(not pcall(d.adjust, d, 3, {alpha = 1}), "only a mark of this frame")
            assert(not pcall(d.adjust, d, rows, {alpha = 2}), "alpha 0 to 1")
            assert(not pcall(d.adjust, d, rows, {rotate = 1}), "a turn is about a point")
            -- A letter's own measure, and a text with letters past ASCII.
            local h = d:glyph("H", {font = "heading", size = 40})
            assert(h and math.abs(h.advance - 24) < 0.01 and h.top == 8 and h.bottom == 32 and h.left == 2 and h.right == 22, "a letter's box")
            assert(d:glyph("\u{416}", {font = "heading", size = 40}).advance == 30, "a Cyrillic letter's advance")
            assert(d:measure("\u{416}\u{416}", {font = "heading", size = 40}).x == 60, "measured by its letters, not its bytes")
            assert(d:measure("H\u{e9}", {font = "heading", size = 40}).x == 24 + 20, "one the font lacks is as wide as ?")
            -- A picture the plugin wrote into its data folder, drawn again when it is written again.
            d:image("data:covers/mine.png", V(0, 0, 0), V(32, 32, 0))
            d:image("data:covers/none.png", V(0, 0, 0), V(32, 32, 0))
            assert(not pcall(d.image, d, "data:../other/x.png", V(0, 0, 0), V(1, 1, 0)), "only its own data folder")
            assert(not pcall(d.image, d, "data:notes.txt", V(0, 0, 0), V(1, 1, 0)), "only pictures")
            reskate.log("drew")
        end)
        reskate.console.command("repaint", "x", function() reskate.files.write("covers/mine.png", "another") end)
    )";
    // A font as the overlay hands it over: 20 pixels, every letter 12 wide in a box, and a Cyrillic one.
    plugin_ui::detail::FontMetrics metrics;
    metrics.size = 20;
    metrics.advance.fill(12);
    metrics.advance['?' - 32] = 10;
    metrics.box.fill({1, 4, 11, 16});
    metrics.wide = {{0x416, 15.0f}};
    plugin_ui::set_font(2, metrics);
    plugin_ui::set_display(1920, 1080);
    environment.fake.files["shapes/covers/mine.png"] = "a png";
    host.apply({package("shapes", {{manifest_file, manifest(R"("overlay", "files", "console")")}, {"plugin/main.lua", main},
                                   {"icons/flag.png", "not really a png"}}),
                script("plain", R"(
        reskate.on("draw", function(d)
            local ok, why = pcall(d.image, d, "data:covers/mine.png", vector.create(0, 0, 0), vector.create(1, 1, 0))
            assert(not ok and why:find("files"), why)
            reskate.log("refused")
        end)
    )", R"("overlay")")});
    ticks(host, environment, 1);
    running(host, "shapes", environment);
    check(environment.logged("[shapes] drew"), "A plugin draws lines, triangles and adjusted shapes\n" + environment.all());
    check(environment.logged("[plain] refused"), "A data folder's picture needs the files permission\n" + environment.all());
    using Kind = plugin_ui::Shape::Kind;
    const plugin_ui::Layer* layer{};
    for (const auto& one : environment.layers) if (one.plugin == "shapes") layer = &one;
    check(layer && layer->shapes.size() == 12, "Each is one shape\n" + environment.all());
    const auto& shapes = layer->shapes;
    check(shapes[0].kind == Kind::poly && shapes[0].fit == 1 && shapes[1].fit == 0, "A polygon's outline can stay open");
    check(shapes[2].kind == Kind::mesh && shapes[2].points.size() == 4 && shapes[2].indices == std::vector<std::uint16_t>{0, 1, 2, 1, 3, 2} &&
              shapes[2].colours[2] == 0xff000080 && shapes[2].text.empty(), "A mesh is its corners, colours and triangles");
    check(shapes[3].kind == Kind::mesh && shapes[3].text.ends_with("flag.png") && shapes[3].uvs[2] == std::array<float, 2>{0.5f, 1.0f},
          "and can go through a picture of the package");
    check(shapes[4].kind == Kind::mark && shapes[4].seed == 1 && shapes[7].kind == Kind::adjust && shapes[7].seed == 1 &&
              std::abs(shapes[7].turn + 0.0698132f) < 1e-5f && shapes[7].points[0][0] == 250 && shapes[7].alpha == 1 && shapes[7].fit == 0,
          "What was drawn since a mark is turned about a point");
    check(shapes[10].kind == Kind::adjust && shapes[10].seed == 2 && shapes[10].fit == 3 && shapes[10].edge == 20 &&
              shapes[10].points[2] == plugin_ui::Vec3{390, 600, 100} && shapes[10].points[1][1] == 12 && shapes[10].alpha == 0.5f,
          "or moved, faded, and faded out at the ends of a box");
    check(shapes[11].kind == Kind::image && shapes[11].text.find("mine.png|") != std::string::npos,
          "A data folder's picture is drawn by its file and a stamp, and one that is not there is not drawn");
    const auto first = shapes[11].text;
    host.run_console("repaint");
    environment.now += 600;
    ticks(host, environment, 1);
    for (const auto& one : environment.layers) if (one.plugin == "shapes") layer = &one;
    check(layer->shapes.size() == 12 && layer->shapes[11].text != first && layer->shapes[11].text.find("mine.png|") != std::string::npos,
          "Written again, it is another picture\n" + environment.all());
    plugin_ui::set_font(2, {});
}

// Fonts of a plugin's own: its manifest names the font files of its package that its texts may be
// drawn in.
void a_manifest_names_its_fonts() {
    const auto with = [](const std::string& fonts) { return R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["overlay"], "fonts": )" + fonts + "}"; };
    const auto read = parse_manifest(with(R"(["fonts/Display-Regular.ttf", "fonts/Digits.OTF", "fonts/Display-Regular.ttf"])"));
    check(read.fonts == std::vector<std::string>{"fonts/Display-Regular.ttf", "fonts/Digits.OTF"}, "A manifest's fonts are read, each once");
    check(parse_manifest(manifest()).fonts.empty() && parse_manifest(with("[]")).fonts.empty(), "and a manifest need have none");
    check(parse_manifest(R"({"api": 1, "entry": "plugin/main.lua", "fonts": ["a.ttf"]})").fonts.size() == 1, "Fonts need no permission of their own");
    const auto refused = [&](const std::string& fonts, std::string_view because) {
        try {
            (void)parse_manifest(with(fonts));
        } catch (const std::exception& error) {
            check(std::string(error.what()).find(because) != std::string::npos, "Refused for \"" + std::string(error.what()) + "\", not " + std::string(because));
            return;
        }
        throw std::runtime_error("Fonts that should be refused were read: " + fonts);
    };
    refused(R"(["1.ttf", "2.ttf", "3.ttf", "4.ttf", "5.ttf", "6.ttf", "7.ttf", "8.ttf", "9.ttf"])", "at most 8 font files");
    refused(R"("fonts/Display-Regular.ttf")", "a list of at most 8 font files");
    refused(R"([7])", "a list of font files");
    refused(R"(["fonts/notes.txt"])", "\"fonts/notes.txt\" in \"fonts\" must be a .ttf or .otf file");
    refused(R"(["fonts/Display.woff2"])", ".ttf or .otf");
    refused(R"(["../other/Display.ttf"])", "\"../other/Display.ttf\" in \"fonts\" must be a path inside the mod folder");
    refused(R"(["C:/Windows/Fonts/arial.ttf"])", "inside the mod folder");
    refused(R"(["fonts\\Display.ttf"])", "inside the mod folder");

    // That each is a file of the package, and not a huge one, is checked with the folder in hand.
    const auto there = package("fonts_there", {{manifest_file, with(R"(["fonts/Display-Regular.ttf"])")}, {"plugin/main.lua", ""},
                                               {"fonts/Display-Regular.ttf", "a font"}});
    check(check_manifest_files(parse_manifest(with(R"(["fonts/Display-Regular.ttf"])")), there.directory).empty(), "A font that is in the package passes");
    check(check_manifest_files(parse_manifest(with(R"(["fonts/Gone.ttf"])")), there.directory) == "\"fonts/Gone.ttf\" in \"fonts\" is not a file in the mod folder",
          "One that is not there is refused");
    std::filesystem::create_directories(there.directory / "fonts" / "Folder.ttf");
    check(!check_manifest_files(parse_manifest(with(R"(["fonts/Folder.ttf"])")), there.directory).empty(), "and a folder by a font's name");
    write(there.directory / "fonts" / "Huge.ttf", std::string(maximum_font_bytes + 1, 'x'));
    check(check_manifest_files(parse_manifest(with(R"(["fonts/Huge.ttf"])")), there.directory) == "\"fonts/Huge.ttf\" in \"fonts\" is larger than 8 MB",
          "and one larger than 8 MB");
    write(there.directory / "fonts" / "Empty.ttf", "");
    check(check_manifest_files(parse_manifest(with(R"(["fonts/Empty.ttf"])")), there.directory).find("cannot be read") != std::string::npos, "and an empty one");

    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({there, package("fonts_gone", {{manifest_file, with(R"(["fonts/Gone.ttf"])")}, {"plugin/main.lua", ""}})});
    running(host, "fonts_there", environment);
    stopped(host, "fonts_gone", "\"fonts/Gone.ttf\" in \"fonts\" is not a file in the mod folder");
}

// A plugin draws and measures text in a font of its own, exactly as in one of the menus' fonts: any
// plugin with screens of its own wants its own face on them.
void plugins_draw_text_in_their_own_fonts() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const std::string main = R"(
        local V = vector.create
        local display = "fonts/Display-Regular.ttf"
        reskate.on("draw", function(d)
            d:text(V(100, 100, 0), "WELCOME", {font = display, size = 40, align = "right", colour = "#ff8000"})
            -- Measured by the font's own advances (8 a letter at its size of 20), not a menu font's (10).
            assert(d:measure("WELCOME", {font = display, size = 40}).x == 7 * 16, "measured in its own font")
            assert(d:measure("WELCOME", {font = "body", size = 40}).x == 7 * 20, "the menus' fonts measure as they did")
            assert(d:measure("WELCOME", {size = 40}).x == d:measure("WELCOME", {font = "bold", size = 40}).x, "and bold is still the one when none is named")
            local s = d:glyph("S", {font = display, size = 40})
            assert(s.advance == 16 and s.left == 2 and s.top == 6 and s.right == 14 and s.bottom == 32, "a letter's box in its own font")
            -- UTF-8: an accented letter the font has is its own (9), and one it lacks is drawn in body, so measured there (15).
            assert(d:glyph("\u{e9}", {font = display, size = 40}).advance == 18, "an accented letter of its own")
            assert(d:glyph("\u{416}", {font = display, size = 40}).advance == 30, "a letter it lacks is body's")
            assert(d:measure("S\u{e9}\u{416}", {font = display, size = 40}).x == 16 + 18 + 30, "measured by its letters, each in the font that draws it")
            -- Wrapped as the menus' fonts are: "AA AA" is 40 wide, and the third word would pass 50.
            local wrapped = d:measure("AA AA AA", {font = display, size = 20, wrap = 50})
            assert(wrapped.x == 40 and wrapped.y == 40, "wrapped to two lines")
            d:text(V(10, 300, 0), "AA AA AA", {font = display, size = 20, wrap = 50})
            -- In the world, and turned and faded with what else was drawn since a mark.
            d:world_text(V(0, 1, -10), "over there", {font = display})
            local label = d:mark()
            d:text(V(210, 205, 0), "SALE", {font = display, size = 30})
            d:adjust(label, {rotate = math.rad(-4), about = V(250, 215, 0), alpha = 0.5})
            -- A font file the manifest does not name is refused, whether or not it is in the package.
            local ok, why = pcall(d.text, d, V(0, 0, 0), "x", {font = "fonts/Other.ttf"})
            assert(not ok and why:find("a font file is named in the manifest's \"fonts\" first", 1, true), why)
            ok, why = pcall(d.measure, d, "x", {font = "../elsewhere/Display-Regular.ttf"})
            assert(not ok and why:find("named in the manifest's \"fonts\" first", 1, true), why)
            ok, why = pcall(d.text, d, V(0, 0, 0), "x", {font = "comic"})
            assert(not ok and why:find("font is", 1, true), why)
            -- One the game cannot read is body: drawn so, and measured so.
            d:text(V(0, 500, 0), "plain", {font = "fonts/unreadable.ttf", size = 20})
            assert(d:measure("plain", {font = "fonts/unreadable.ttf", size = 20}).x == 50, "a font that cannot be read measures as body")
            assert(d:glyph("p", {font = "fonts/unreadable.ttf", size = 20}).advance == 10, "letter by letter too")
            reskate.log("drew")
        end)
    )";
    plugin_ui::detail::FontMetrics body;
    body.size = 20;
    body.advance.fill(10);
    body.box.fill({1, 4, 9, 16});
    body.wide = {{0x416, 15.0f}};
    plugin_ui::set_font(0, body);
    plugin_ui::set_font(1, body);
    plugin_ui::set_display(1920, 1080);
    const std::string fonts = R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["overlay"], "fonts": ["fonts/Display-Regular.ttf", "fonts/unreadable.ttf"]})";
    const std::string unseen = R"({"api": 1, "entry": "plugin/main.lua", "fonts": ["fonts/Display-Regular.ttf"]})";
    host.apply({package("signs", {{manifest_file, fonts}, {"plugin/main.lua", main}, {"fonts/Display-Regular.ttf", "a font"}, {"fonts/unreadable.ttf", "not one"},
                                  {"fonts/Other.ttf", "a font the manifest does not name"}}),
                // A font is no way round the permission drawing needs: without it there is no draw handler to draw in.
                package("unseen", {{manifest_file, unseen}, {"fonts/Display-Regular.ttf", "a font"}, {"plugin/main.lua", R"(
        local ok, why = pcall(reskate.on, "draw", function(d) d:text(vector.create(0, 0, 0), "x", {font = "fonts/Display-Regular.ttf"}) end)
        assert(not ok and why:find("overlay"), why)
        reskate.log("refused")
    )"}})});
    ticks(host, environment, 3);
    running(host, "signs", environment);
    running(host, "unseen", environment);
    check(environment.logged("[signs] drew"), "A plugin draws and measures text in its own font\n" + environment.all());
    check(environment.logged("[unseen] refused"), "Drawing in a font still needs the overlay permission\n" + environment.all());
    using Kind = plugin_ui::Shape::Kind;
    check(environment.layers.size() == 1 && environment.layers[0].plugin == "signs", "Only the plugin that may draw has a layer\n" + environment.all());
    const auto& layer = environment.layers[0];
    check(layer.fonts && layer.fonts->size() == 2 && (*layer.fonts)[0].ends_with("Display-Regular.ttf") &&
              std::filesystem::path((*layer.fonts)[0]).is_absolute() && (*layer.fonts)[1].ends_with("unreadable.ttf"),
          "The layer says where the plugin's fonts are, in its manifest's order");
    const auto& shapes = layer.shapes;
    check(shapes.size() == 7, "Each text is one shape\n" + environment.all());
    check(shapes[0].kind == Kind::text && shapes[0].text == "WELCOME" && shapes[0].style.font == plugin_ui::own_fonts && shapes[0].style.size == 40 &&
              shapes[0].style.align == 2 && shapes[0].style.colour == 0xff0080ff,
          "A text carries its font's number, and its style as any text does");
    check(shapes[1].style.font == plugin_ui::own_fonts && shapes[1].style.wrap == 50, "wrapped too");
    check(shapes[2].kind == Kind::world_text && shapes[2].style.font == plugin_ui::own_fonts, "and in the world");
    check(shapes[3].kind == Kind::mark && shapes[4].kind == Kind::text && shapes[4].style.font == plugin_ui::own_fonts && shapes[5].kind == Kind::adjust &&
              shapes[5].alpha == 0.5f,
          "It is adjusted with whatever else was drawn since a mark");
    check(shapes[6].text == "plain" && shapes[6].style.font == 1, "A text in a font that cannot be read is body's");
    const auto said = std::count_if(environment.logs.begin(), environment.logs.end(),
                                    [](const auto& line) { return line.find("the font fonts/unreadable.ttf cannot be read") != std::string::npos; });
    check(said == 1, "which is said once\n" + environment.all());
    check(environment.fake.font_asks == std::vector<std::string>{"signs fonts/Display-Regular.ttf", "signs fonts/unreadable.ttf"},
          "The game is asked for a font's metrics once, however often it is drawn");
    // What the overlay sets a text with is what measured it: each character's place adds up to its width.
    const auto metrics = environment.fake.plugin_font_metrics("signs", "fonts/Display-Regular.ttf", std::filesystem::path((*layer.fonts)[0]));
    struct Placed {
        std::uint32_t code;
        float x;
        int line;
        bool own;
    };
    std::vector<Placed> placed;
    const auto extent = plugin_ui::set_text(*metrics, &body, 20, "AA AA \xd0\x96", 50,
                                            [&](std::uint32_t code, float x, int line, bool own) { placed.push_back({code, x, line, own}); });
    check(extent == plugin_ui::measure(*metrics, 20, "AA AA \xd0\x96", 50) && extent == std::array<float, 2>{40, 40}, "A set text is as big as it measured");
    check(placed.size() == 6 && placed[0].x == 0 && placed[0].own && placed[2].code == ' ' && placed[2].x == 16 && placed[4].x == 32 && placed[4].line == 0 &&
              placed[5].code == 0x416 && placed[5].x == 0 && placed[5].line == 1 && !placed[5].own,
          "Each character has its place: a wrapped word starts its line without its space, and one the font lacks is body's");
    plugin_ui::set_font(0, {});
    plugin_ui::set_font(1, {});
}

// A use that is not a menu: a speedometer's digits in a seven-segment face, kept to the right of a
// box by measuring them, and a clock whose digits each sit in a cell so they do not shuffle about.
void a_plugin_sets_a_speedometer_in_its_own_digits() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const std::string main = R"(
        local V = vector.create
        local digits = {font = "fonts/digits.otf", size = 60, shadow = false}
        local speed, time = 42, "1:07"
        reskate.on("draw", function(d)
            -- The speed ends at x = 1800 whatever its digits are: the text starts its own width before it.
            local text = tostring(speed)
            local wide = d:measure(text, digits).x
            assert(wide == #text * 42, "each digit is 14 wide at 20, so 42 at 60")
            d:text(V(1800 - wide, 900, 0), text, digits)
            d:text(V(1808, 930, 0), "km/h", {font = "body", size = 20})
            -- The clock: every character in a cell as wide as a digit, the colon in the middle of its own.
            local cell = d:glyph("0", digits).advance
            assert(cell == 42 and d:glyph(":", digits).advance == 18, "a digit's and the colon's advance")
            local x = 1800 - #time * cell
            for i = 1, #time do
                local c = time:sub(i, i)
                d:text(V(x + (cell - d:glyph(c, digits).advance) / 2, 820, 0), c, digits)
                x += cell
            end
            -- A face of digits alone still measures a letter: body draws it.
            assert(d:measure("42 km", digits).x == 2 * 42 + 3 * 30, "what the face lacks is measured in body")
            speed += 965
        end)
    )";
    plugin_ui::detail::FontMetrics body;
    body.size = 20;
    body.advance.fill(10);
    plugin_ui::set_font(1, body);
    plugin_ui::set_display(1920, 1080);
    const std::string manifest_text = R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["overlay"], "fonts": ["fonts/digits.otf"]})";
    host.apply({package("speedo", {{manifest_file, manifest_text}, {"plugin/main.lua", main}, {"fonts/digits.otf", "a font"}})});
    ticks(host, environment, 1);
    running(host, "speedo", environment);
    check(environment.layers.size() == 1 && environment.layers[0].shapes.size() == 6, "The speed, its unit and the clock's four characters\n" + environment.all());
    const auto* shapes = &environment.layers[0].shapes;
    check((*shapes)[0].text == "42" && (*shapes)[0].points[0][0] == 1800 - 84 && (*shapes)[0].style.font == plugin_ui::own_fonts && !(*shapes)[0].style.shadow,
          "Two digits start two digits' width before the box's edge");
    check((*shapes)[1].text == "km/h" && (*shapes)[1].style.font == 1, "The unit beside them is in a menu font");
    check((*shapes)[2].text == "1" && (*shapes)[2].points[0][0] == 1800 - 4 * 42 && (*shapes)[3].text == ":" && (*shapes)[3].points[0][0] == 1800 - 3 * 42 + 12 &&
              (*shapes)[5].text == "7" && (*shapes)[5].points[0][0] == 1800 - 42,
          "The clock's characters are each in their cell");
    ticks(host, environment, 1);
    running(host, "speedo", environment);
    shapes = &environment.layers[0].shapes;
    check((*shapes)[0].text == "1007" && (*shapes)[0].points[0][0] == 1800 - 4 * 42, "Four digits start four digits' width before it: the number stays against the edge");
    check(environment.fake.font_asks.size() == 1, "and the font was read once");
    plugin_ui::set_font(1, {});
}

// The glyph pages behind those texts, as far as they are sums (Extension/UI/Overlay/glyph_pages.h):
// the size glyphs are made at, where they go on a page, and which pages make way.
void glyph_pages_are_sized_filled_and_let_go() {
    namespace pages = dingosdk::overlay::glyph_pages;
    // Sizes: every 2 pixels to 32, every 4 to 96, every 8 to 256, never smaller than the text.
    check(pages::size_for(18) == 18 && pages::size_for(18.2f) == 20 && pages::size_for(31.5f) == 32 && pages::size_for(32) == 32, "Up to 32 pixels, every 2");
    check(pages::size_for(33) == 36 && pages::size_for(36) == 36 && pages::size_for(95.1f) == 96, "then every 4 up to 96");
    check(pages::size_for(97) == 104 && pages::size_for(250) == 256 && pages::size_for(256) == 256, "then every 8 up to 256");
    check(pages::size_for(2.7f) == 8 && pages::size_for(0) == 8 && pages::size_for(-1) == 8 && pages::size_for(800) == 256, "and no smaller than 8 or larger than 256");
    int sizes = 0, last = 0;
    for (float pixels = 8; pixels <= 256; pixels += 0.25f) {
        const int made = pages::size_for(pixels);
        check(static_cast<float>(made) >= pixels && static_cast<float>(made) < pixels * 1.25f + 0.01f, "A glyph is made no smaller than it is drawn, and at most a quarter larger");
        check(made >= last, "A larger text is never made smaller");
        if (made != last) ++sizes;
        last = made;
    }
    check(sizes == 13 + 16 + 20, "Text from 8 to 256 pixels is made at 49 sizes");

    // Places: boxes on a page do not touch each other or its edge, and the page grows as they need.
    struct Box {
        int x, y, w, h;
    };
    pages::Shelves shelves;
    std::vector<Box> boxes;
    check(shelves.height == pages::least_page_height, "A page starts small");
    std::uint32_t seed = 12345;
    const auto next = [&](int low, int high) {
        seed = seed * 1664525u + 1013904223u;
        return low + static_cast<int>((seed >> 8) % static_cast<std::uint32_t>(high - low + 1));
    };
    int heights = 0, before = shelves.height;
    for (;;) {
        const int w = next(3, 40), h = next(8, 48);
        const auto place = shelves.find(w, h);
        if (!place) break;
        check(place->height >= shelves.height && place->height <= pages::most_page_height && (place->height & (place->height - 1)) == 0,
              "A page grows by doubling, to at most 1024");
        shelves.take(*place, w, h);
        check(place->x >= 1 && place->y >= 1 && place->x + w + 1 <= pages::page_width && place->y + h + 1 <= shelves.height, "A box is on the page, a pixel inside its edge");
        boxes.push_back({place->x, place->y, w, h});
        if (shelves.height != before) ++heights;
        before = shelves.height;
    }
    check(heights == 4 && shelves.height == pages::most_page_height, "The page doubled from 64 to 1024 as it filled");
    long long area = 0;
    for (const auto& box : boxes) area += static_cast<long long>(box.w) * box.h;
    check(boxes.size() > 800 && area * 2 > static_cast<long long>(pages::page_width) * pages::most_page_height,
          std::format("and held more than half its area in glyphs before it was full: {} boxes, {} pixels", boxes.size(), area));
    // (Each against its neighbours on the page: sorted by row, a box is compared with those that could reach it.)
    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    for (std::size_t i = 0; i < boxes.size(); ++i)
        for (std::size_t j = i + 1; j < boxes.size() && boxes[j].y <= boxes[i].y + boxes[i].h; ++j) {
            const auto& a = boxes[i];
            const auto& b = boxes[j];
            const bool apart = a.x + a.w + 1 <= b.x || b.x + b.w + 1 <= a.x || a.y + a.h + 1 <= b.y || b.y + b.h + 1 <= a.y;
            check(apart, "Two boxes are a pixel apart at least");
        }
    pages::Shelves fresh;
    check(!fresh.find(0, 5) && !fresh.find(5, 0) && !fresh.find(pages::page_width - 1, 5) && !fresh.find(5, pages::most_page_height - 1),
          "A box with no size, or too large for any page, has no place");
    const auto widest = fresh.find(pages::page_width - 2, pages::most_page_height - 2);
    check(widest && widest->x == 1 && widest->y == 1 && widest->height == pages::most_page_height, "The largest box a page takes fills it");
    const auto unchanged = fresh.find(10, 10);
    check(unchanged && unchanged->height == pages::least_page_height && fresh.x == 1 && fresh.y == 1, "Asking where a box would go moves nothing");

    // Letting go: the pages drawn from longest ago, never one drawn from in the last frame, and only as many as it takes.
    const std::size_t mb = 1u << 20;
    const std::vector<pages::Held> held{{4 * mb, 10.0}, {4 * mb, 3.0}, {2 * mb, 7.0}, {4 * mb, 20.0}, {1 * mb, 5.0}};
    check(pages::make_room(held, mb, 16 * mb, 20.0) == std::vector<std::size_t>{}, "With room, nothing goes");
    check(pages::make_room(held, 2 * mb, 16 * mb, 20.0) == std::vector<std::size_t>{1}, "Without, the page drawn from longest ago goes");
    check(pages::make_room(held, 8 * mb, 16 * mb, 20.0) == (std::vector<std::size_t>{1, 4, 2}), "then the next oldest, as many as it takes");
    check(pages::make_room(held, 12 * mb, 16 * mb, 20.0) == (std::vector<std::size_t>{1, 4, 2, 0}), "down to the pages of the last frame");
    check(!pages::make_room(held, 13 * mb, 16 * mb, 20.0), "A page drawn from in the last frame never goes: then there is no room");
    check(pages::make_room(held, 13 * mb, 16 * mb, 21.0) == (std::vector<std::size_t>{1, 4, 2, 0, 3}), "Once it is no longer drawn it can");
    check(!pages::make_room(held, 17 * mb, 16 * mb, 100.0) && pages::make_room({}, 16 * mb, 16 * mb, 0.0) == std::vector<std::size_t>{},
          "and more than the whole cap never fits");
    check(pages::bytes_of(pages::most_page_height) == 4 * mb && pages::most_bytes == 64 * mb, "A full page is 4 MB on the GPU, and all of them at most 64 MB");
}

// Files a plugin manages: a file's size and time, copies and moves, the Recycle Bin, a hash; a folder
// beside the game it was granted by name; and hashes and zips of bytes it holds. A route planner's
// routes, a replay tool's recordings: nothing here is one plugin's.
void plugins_manage_files_and_granted_folders() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const std::string main = R"(
        local files = reskate.files
        assert(files.write("notes/a.txt", "one"))
        local i = files.info("notes/a.txt")
        assert(i.size == 3 and i.folder == false and i.modified == 1000, "a file's size and time")
        assert(files.info("notes").folder == true, "a folder")
        local none, why = files.info("nothing.txt")
        assert(none == nil and why == "not found")
        assert(files.touch("notes/a.txt", 1234) and files.info("notes/a.txt").modified == 1234, "a file's time set")
        assert(files.copy("notes/a.txt", "notes/b.txt") and files.read("notes/b.txt") == "one", "a copy")
        assert(files.move("notes/b.txt", "kept/c.txt") and files.read("notes/b.txt") == nil and files.read("kept/c.txt") == "one", "a move")
        assert(files.hash("kept/c.txt") == "hash of one", "a file's hash")
        local ok, said = files.recycle({"notes/a.txt", "gone.txt"})
        assert(not ok and said == "gone.txt: not found" and files.read("notes/a.txt") == "one", "all or none go to the Recycle Bin")
        assert(files.recycle({"notes/a.txt", "kept/c.txt"}) and files.read("notes/a.txt") == nil, "to the Recycle Bin")
        assert(not pcall(files.info, "../other/x.txt") and not pcall(files.move, "a.txt", "../x.txt") and not pcall(files.recycle, {"C:/x"}),
               "only inside its folders")
        assert(not pcall(files.touch, "notes/a.txt", -1), "a time since 1970")
        -- A folder beside the game the manifest names.
        assert(files.write("My Routes:park/loop.json", "{}") and files.read("My Routes:park/loop.json") == "{}", "a granted folder")
        assert(#files.list("My Routes:") == 1 and files.list("My Routes:park")[1] == "loop.json", "listed")
        assert(files.copy("My Routes:park/loop.json", "copy.json") and files.read("copy.json") == "{}", "between a granted folder and the data folder")
        assert(not pcall(files.read, "Other:x.txt"), "only a folder the manifest names")
        assert(not pcall(files.read, "My Routes:../x.txt") and not pcall(files.read, "My Routes:a:b"), "only inside it")
        assert(not pcall(files.read, "My Routes:"), "a file call names a file")
        -- Bytes it holds: hashes, and a zip's files.
        assert(reskate.hash.sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256")
        assert(reskate.hash.crc32("123456789") == 0xCBF43926, "CRC-32")
        local function stored(name, data)
            local crc = reskate.hash.crc32(data)
            local head = string.pack("<I4I2I2I2I2I2I4I4I4I2I2", 0x04034b50, 20, 0, 0, 0, 0, crc, #data, #data, #name, 0) .. name
            local entry = string.pack("<I4I2I2I2I2I2I2I4I4I4I2I2I2I2I2I4I4", 0x02014b50, 20, 20, 0, 0, 0, 0, crc, #data, #data, #name, 0, 0, 0, 0, 0, 0) .. name
            return head .. data .. entry .. string.pack("<I4I2I2I2I2I4I4I2", 0x06054b50, 0, 0, 1, 1, #entry, #head + #data, 0)
        end
        local entries = reskate.zip.entries(stored("tricks/a.txt", "hello"))
        assert(#entries == 1 and entries[1].name == "tricks/a.txt" and entries[1].bytes == "hello", "a zip's files")
        local bad = stored("a.txt", "hello")
        bad = string.sub(bad, 1, 35) .. "J" .. string.sub(bad, 37)
        local nothing, reason = reskate.zip.entries(bad)
        assert(nothing == nil and reason == "a.txt in it is damaged", reason)
        assert(reskate.zip.entries("not a zip") == nil, "only a zip")
        reskate.log("managed")
    )";
    host.apply({package("keeper", {{manifest_file, R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["files", "files.folders"], "folders": ["My Routes"]})"},
                                   {"plugin/main.lua", main}}),
                script("plain", R"(
        local ok, why = pcall(reskate.files.read, "My Routes:park/loop.json")
        assert(not ok and why:find("files"), why)
        ok, why = pcall(reskate.files.info, "a.txt")
        assert(not ok and why:find("files"), why)
        assert(reskate.hash.crc32("") == 0, "hashes need no permission")
        reskate.log("refused")
    )"), script("half", R"(
        local ok, why = pcall(reskate.files.read, "My Routes:park/loop.json")
        assert(not ok and why:find("files.folders", 1, true), why)
        ok, why = pcall(reskate.files.list, "My Routes:")
        assert(not ok and why:find("files.folders", 1, true), why)
        reskate.log("its own folder only")
    )", R"("files")")});
    running(host, "keeper", environment);
    check(environment.logged("[half] its own folder only"), "With files alone a plugin reaches no folder beside the game\n" + environment.all());
    check(environment.logged("[keeper] managed"), "A plugin manages its files and a granted folder\n" + environment.all());
    check(environment.logged("[plain] refused"), "A granted folder needs files.folders, and the rest needs files\n" + environment.all());
    check(environment.fake.recycled == std::vector<std::string>{"notes/a.txt", "kept/c.txt"}, "What it recycled went to the Recycle Bin");
    check(environment.fake.files.contains("keeper/My Routes:park/loop.json"), "A granted folder's path reaches the game with its folder");
    const auto refused = [](const std::string& text, std::string_view because) {
        try {
            (void)parse_manifest(text);
        } catch (const std::exception& error) {
            return std::string_view(error.what()).find(because) != std::string_view::npos;
        }
        return false;
    };
    check(refused(R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["files", "files.folders"], "folders": ["Mods"]})", "none of the game's"),
          "A plugin is not granted the Mods folder");
    for (const auto* name : {"Data", "logs", "..", "a/b", ".hidden", "con", "nul.txt", "x:y", ""})
        check(!granted_folder_name(name), std::string("Not a folder to grant: ") + name);
    check(granted_folder_name("My Routes") && granted_folder_name("Replays-2"), "A plain name is");
    check(refused(R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["files"], "folders": ["My Routes"]})", "files.folders"),
          "Folders need their permission");
    const auto groups = permission_groups(parse_manifest(R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["files", "files.folders"], "folders": ["My Routes"]})"));
    check(std::any_of(groups.begin(), groups.end(), [](const auto& group) {
              return group.category == "FILES" && group.risk == Risk::red &&
                     std::find(group.phrases.begin(), group.phrases.end(), "Read and write files in folders beside the game (My Routes)") != group.phrases.end();
          }), "The player is told which folders, in red");
}

// Pictures a plugin composes into files: drawing steps, checked and handed to the game, which draws
// them off the plugin's thread and says how each went in a later tick; a text measured as a step
// draws it; a picture's size. A postcard, a score card, a map's thumbnail: nothing here is one plugin's.
void plugins_compose_pictures_from_steps() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const std::string painter = R"(
        local P = reskate.picture
        local function said(name) return function(ok, why) reskate.log(name .. " " .. tostring(ok) .. " " .. tostring(why)) end end
        -- A postcard: paper, a photo over it, a shade, a label on a slant, a mark in the corner.
        assert(P.make("cards/one.png", 512, 512, {
            {kind = "fill", colour = {0.1, 0.2, 0.3}},
            {kind = "tile", picture = "art/paper.png", texel = 4},
            {kind = "image", picture = "data:shots/a.jpg", fit = "cover", focus = 0.25, alpha = 0.9},
            {kind = "image", picture = "art/badge.png", x = 10, y = 20, w = 64, h = 32},
            {kind = "gradient", y0 = 300, y1 = 512, colour = {0, 0, 0}, a0 = 0, a1 = 0.8},
            {kind = "layer", w = 420, h = 120, cx = 256, cy = 400, angle = -0.07, steps = {
                {kind = "poly", points = {{0, 8}, {420, 0}, {410, 120}, {6, 112}}, colour = {1, 1, 1, 1}},
                {kind = "poly", points = {{0, 0}, {10, 0}, {10, 10}}, picture = "art/paper.png", texel = 2},
                {kind = "text", font = "fonts/hand.ttf", text = "Hello", size = 48, x = 210, y = 78, colour = {0, 0, 0}, align = "center"},
            }},
            {kind = "poly", points = {{0, 0}, {50, 0}, {25, 40}}, colour = {1, 0, 0, 0.5}},
            {kind = "text", font = "fonts/hand.ttf", text = "corner", size = 12, x = 500, y = 500, colour = {1, 1, 1}, align = "right"},
        }, said("one")))

        -- What is not a picture to draw is an error that says what is wrong with it.
        local function refused(why, path, width, height, steps)
            local ok, message = pcall(P.make, path, width, height, steps)
            assert(not ok and message:find(why, 1, true), why .. ": " .. tostring(message))
        end
        local function step_refused(why, step) refused(why, "cards/no.png", 64, 64, {step}) end
        refused(".png", "cards/one.jpg", 64, 64, {})
        refused("data folder", "../one.png", 64, 64, {})
        refused("2048", "cards/no.png", 0, 64, {})
        refused("2048", "cards/no.png", 64, 4096, {})
        refused("2048", "cards/no.png", 64.5, 64, {})
        refused("a step is a table", "cards/no.png", 64, 64, {"fill"})
        step_refused("kind", {kind = "blur"})
        step_refused("needs colour", {kind = "fill"})
        step_refused("each 0 to 1", {kind = "fill", colour = {2, 0, 0}})
        step_refused("each 0 to 1", {kind = "fill", colour = {1, 0}})
        step_refused("not a finite", {kind = "image", picture = "art/badge.png", x = 0 / 0, y = 0, w = 1, h = 1})
        step_refused("not a finite", {kind = "image", picture = "art/badge.png", x = 0, y = math.huge, w = 1, h = 1})
        step_refused("million", {kind = "image", picture = "art/badge.png", x = 1e9, y = 0, w = 1, h = 1})
        step_refused("needs w", {kind = "image", picture = "art/badge.png", x = 0, y = 0, h = 1})
        step_refused("fit", {kind = "image", picture = "art/badge.png", fit = "contain"})
        step_refused("alpha is 0 to 1", {kind = "image", picture = "art/badge.png", fit = "cover", alpha = 2})
        step_refused("needs picture", {kind = "tile"})
        step_refused(".png or .jpg", {kind = "tile", picture = "art/paper.gif"})
        step_refused("package", {kind = "tile", picture = "../other/paper.png"})
        step_refused("data folder", {kind = "tile", picture = "data:../other/a.png"})
        step_refused("texel", {kind = "tile", picture = "art/paper.png", texel = 0})
        step_refused("a1 is 0 to 1", {kind = "gradient", y0 = 0, y1 = 8, colour = {0, 0, 0}, a0 = 0, a1 = 1.5})
        local triangle = {kind = "poly", points = {{0, 0}, {8, 0}, {8, 8}}, colour = {1, 1, 1}}
        step_refused("\"poly\" and \"text\"", {kind = "layer", w = 8, h = 8, cx = 4, cy = 4, steps = {{kind = "layer", w = 8, h = 8, cx = 4, cy = 4, steps = {triangle}}}})
        step_refused("\"poly\" and \"text\"", {kind = "layer", w = 8, h = 8, cx = 4, cy = 4, steps = {{kind = "fill", colour = {0, 0, 0}}}})
        step_refused("4096", {kind = "layer", w = 5000, h = 8, cx = 4, cy = 4, steps = {triangle}})
        step_refused("needs steps", {kind = "layer", w = 8, h = 8, cx = 4, cy = 4})
        step_refused("at least 3 points", {kind = "poly", points = {{0, 0}, {8, 8}}, colour = {1, 1, 1}})
        step_refused("{x, y}", {kind = "poly", points = {{0, 0}, {8, 8}, {1, "a"}}, colour = {1, 1, 1}})
        step_refused("{x, y}", {kind = "poly", points = {{0, 0}, {8, 8}, {1, 0 / 0}}, colour = {1, 1, 1}})
        step_refused("needs colour", {kind = "poly", points = {{0, 0}, {8, 0}, {8, 8}}})
        local words = {kind = "text", font = "fonts/hand.ttf", text = "x", size = 12, x = 0, y = 0, colour = {1, 1, 1}}
        local function text_refused(why, field, value)
            local step = {}
            for name, given in words do step[name] = given end
            step[field] = value
            step_refused(why, step)
        end
        text_refused("512 bytes", "text", string.rep("a", 513))
        text_refused(".ttf or .otf", "font", "fonts/hand.woff")
        text_refused("package", "font", "../other/hand.ttf")
        text_refused("align", "align", "middle")
        text_refused("size", "size", 0)
        text_refused("needs text", "text", nil)

        -- At its limits a picture is still drawn; one step or one point past them it is not, and a
        -- layer's own steps count.
        local steps, ring = {}, {}
        for i = 1, 255 do steps[i] = {kind = "fill", colour = {0, 0, 0}} end
        for i = 1, 20000 do ring[i] = {32 + 30 * math.cos(i), 32 + 30 * math.sin(i)} end
        steps[256] = {kind = "poly", points = ring, colour = {1, 1, 1}}
        assert(P.make("cards/full.png", 64, 64, steps))
        steps[257] = {kind = "fill", colour = {0, 0, 0}}
        refused("256 steps", "cards/no.png", 64, 64, steps)
        steps[257] = nil
        steps[255] = {kind = "layer", w = 8, h = 8, cx = 4, cy = 4, steps = {triangle}}
        refused("256 steps", "cards/no.png", 64, 64, steps)
        ring[20001] = {0, 0}
        step_refused("20000 points", {kind = "poly", points = ring, colour = {1, 1, 1}})

        -- Four a second, and the fifth is told so (it is not an error: it can be asked again).
        assert(P.make("cards/three.png", 8, 8, {}))
        assert(P.make("My Routes:cards/four.png", 8, 8, {}, said("four")))
        local ok, why = P.make("cards/five.png", 8, 8, {})
        assert(ok == false and why == "more than 4 pictures in a second", tostring(why))

        -- A text is measured as it is drawn, and a picture says its size.
        assert(P.text_width("fonts/hand.ttf", "Hello", 20) == 50, "a text's width")
        local none, because = P.text_width("fonts/missing.ttf", "Hello", 20)
        assert(none == nil and because == "the font cannot be read", tostring(because))
        assert(not pcall(P.text_width, "fonts/hand.woff", "x", 20) and not pcall(P.text_width, "../other/hand.ttf", "x", 20), "a font of its package")
        assert(not pcall(P.text_width, "fonts/hand.ttf", "x", 0) and not pcall(P.text_width, "fonts/hand.ttf", string.rep("a", 513), 20), "a size, a line")
        local w, h = P.size("art/paper.png")
        assert(w == 256 and h == 128, "a package picture's size")
        w, h = P.size("data:shots/a.jpg")
        assert(w == 64 and h == 32, "a data folder picture's size")
        local nothing, reason = P.size("art/none.png")
        assert(nothing == nil and reason:find("PNG"), tostring(reason))
        assert(not pcall(P.size, "notes.txt") and not pcall(P.size, "data:../other/a.png"), "a picture of its own")

        reskate.console.command("paint", "x", function(args)
            local started, said_why = P.make("cards/" .. args[1] .. ".png", 8, 8, {}, said(args[1]))
            reskate.log("asked " .. args[1] .. " " .. tostring(started) .. " " .. tostring(said_why))
        end)
        reskate.log("painted")
    )";
    // Not a square with a label: a score card, wide and short, its number against the right edge and a
    // caption set left of it by the number's measure; and a map's thumbnail with a route over it,
    // written into a folder beside the game.
    const std::string scorer = R"(
        local P = reskate.picture
        local font, score = "fonts/digits.otf", "12,340"
        local wide = P.text_width(font, score, 64)
        assert(P.make("cards/score.png", 600, 200, {
            {kind = "fill", colour = {0.05, 0.05, 0.08}},
            {kind = "gradient", y0 = 100, y1 = 200, colour = {1, 1, 1}, a0 = 0, a1 = 0.15},
            {kind = "text", font = font, text = score, size = 64, x = 580, y = 130, colour = {1, 1, 1}, align = "right"},
            {kind = "text", font = font, text = "BEST RUN", size = 20, x = 580 - wide - 16, y = 130, colour = {1, 1, 1, 0.6}, align = "right"},
        }, function(ok) reskate.log("score card " .. tostring(ok)) end))
        assert(P.make("My Routes:thumbs/loop.png", 256, 256, {
            {kind = "image", picture = "data:maps/park.png", x = 0, y = 0, w = 256, h = 256},
            {kind = "poly", points = {{40, 200}, {90, 60}, {200, 80}, {210, 190}, {204, 192}, {195, 86}, {94, 67}, {46, 202}}, colour = {1, 0.8, 0, 0.9}},
        }))
        reskate.log("scored")
    )";
    const std::string folders = R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["files", "files.folders", "console"], "folders": ["My Routes"]})";
    environment.fake.files["painter/shots/a.jpg"] = "a photo";
    host.apply({package("painter", {{manifest_file, folders}, {"plugin/main.lua", painter}, {"art/paper.png", "paper"}, {"art/badge.png", "badge"},
                                    {"fonts/hand.ttf", "a font"}, {"notes.txt", "x"}}),
                package("scorer", {{manifest_file, folders}, {"plugin/main.lua", scorer}, {"fonts/digits.otf", "a font"}}),
                package("plain", {{manifest_file, manifest()}, {"art/paper.png", "paper"}, {"fonts/hand.ttf", "a font"}, {"plugin/main.lua", R"(
        local P = reskate.picture
        local ok, why = pcall(P.make, "a.png", 8, 8, {})
        assert(not ok and why:find("files"), why)
        ok, why = pcall(P.size, "data:shots/a.jpg")
        assert(not ok and why:find("files"), why)
        assert(P.text_width("fonts/hand.ttf", "Hello", 20) == 50, "measuring needs no permission")
        assert(P.size("art/paper.png") == 256, "nor does the size of its own picture")
        reskate.log("refused")
    )"}}),
                script("half", R"(
        local ok, why = pcall(reskate.picture.make, "My Routes:a.png", 8, 8, {})
        assert(not ok and why:find("files.folders", 1, true), why)
        reskate.log("its own folder only")
    )", R"("files")")});
    running(host, "painter", environment);
    check(environment.logged("[painter] painted"), "A plugin composes pictures\n" + environment.all());
    check(environment.logged("[scorer] scored"), "A plugin composes a score card and a thumbnail\n" + environment.all());
    check(environment.logged("[plain] refused"), "Writing a picture needs the files permission, measuring does not\n" + environment.all());
    check(environment.logged("[half] its own folder only"), "A picture in a folder beside the game needs files.folders\n" + environment.all());
    auto& fake = environment.fake;
    check(!fake.called("picture plain") && !fake.called("picture half") && !fake.called("no.png") && !fake.called("five.png"),
          "A picture that was refused never reaches the game");
    check(!environment.logged("one true"), "Nothing is called back in the call that asked");
    ticks(host, environment, 1);
    check(environment.logged("[painter] one true nil") && environment.logged("[painter] four true nil") && environment.logged("[scorer] score card true"),
          "A tick later each picture's function hears how it went\n" + environment.all());

    using Kind = picture::Step::Kind;
    const auto same_file = [](const std::filesystem::path& a, const std::filesystem::path& b) { return a.lexically_normal() == b.lexically_normal(); };
    const auto request = [&](std::string_view path) -> const picture::Request& {
        const auto found = std::find_if(fake.pictures.begin(), fake.pictures.end(), [&](const auto& one) { return one.path == path; });
        check(found != fake.pictures.end(), "The game was asked for " + std::string(path));
        return *found;
    };
    const auto& card = request("cards/one.png");
    check(card.width == 512 && card.height == 512 && card.steps.size() == 8, "The steps reach the game as given, in order");
    check(card.steps[0].kind == Kind::fill && card.steps[0].colour == picture::Colour{0.1f, 0.2f, 0.3f, 1.0f}, "A fill, opaque when it gives no alpha");
    check(card.steps[1].kind == Kind::tile && card.steps[1].picture.name == "art/paper.png" && card.steps[1].picture.data.empty() &&
              same_file(card.steps[1].picture.file, scratch / "mods" / "painter" / "art" / "paper.png") && card.steps[1].texel == 4,
          "A tiled picture of the package, by its file");
    check(card.steps[2].kind == Kind::image && card.steps[2].cover && card.steps[2].focus == 0.25f && card.steps[2].alpha == 0.9f &&
              card.steps[2].picture.data == "shots/a.jpg" && card.steps[2].picture.file.empty(),
          "A picture of the data folder filling the canvas, by its path there");
    check(card.steps[3].kind == Kind::image && !card.steps[3].cover && card.steps[3].x == 10 && card.steps[3].y == 20 && card.steps[3].w == 64 &&
              card.steps[3].h == 32 && card.steps[3].alpha == 1,
          "A picture in a box");
    check(card.steps[4].kind == Kind::gradient && card.steps[4].y0 == 300 && card.steps[4].y1 == 512 && card.steps[4].a0 == 0 && card.steps[4].a1 == 0.8f,
          "A gradient's rows and alphas");
    const auto& layer = card.steps[5];
    check(layer.kind == Kind::layer && layer.w == 420 && layer.h == 120 && layer.x == 256 && layer.y == 400 && layer.angle == -0.07f && layer.steps.size() == 3,
          "A layer's size, middle, angle and steps");
    check(layer.steps[0].kind == Kind::poly && layer.steps[0].points.size() == 4 && layer.steps[0].points[2] == std::array<float, 2>{410, 120} &&
              layer.steps[0].colour == picture::Colour{1, 1, 1, 1},
          "A polygon's points and colour");
    check(layer.steps[1].kind == Kind::poly && layer.steps[1].picture.name == "art/paper.png" && layer.steps[1].texel == 2 && layer.steps[1].colour.a == 0,
          "A polygon filled with a picture falls back to nothing when it names no colour");
    check(layer.steps[2].kind == Kind::text && layer.steps[2].font.name == "fonts/hand.ttf" &&
              same_file(layer.steps[2].font.file, scratch / "mods" / "painter" / "fonts" / "hand.ttf") && layer.steps[2].text == "Hello" &&
              layer.steps[2].size == 48 &&
              layer.steps[2].x == 210 && layer.steps[2].y == 78 && layer.steps[2].align == picture::Align::center,
          "A text's font, size, place and alignment");
    check(card.steps[6].kind == Kind::poly && card.steps[6].colour == picture::Colour{1, 0, 0, 0.5f} && card.steps[7].kind == Kind::text &&
              card.steps[7].align == picture::Align::right,
          "Polygons and texts go on the canvas too");
    const auto& full = request("cards/full.png");
    check(full.steps.size() == 256 && full.steps[255].points.size() == 20000, "A picture at its limits reaches the game whole");
    check(request("My Routes:cards/four.png").steps.empty(), "A granted folder's path reaches the game with its folder");

    const auto& score = request("cards/score.png");
    check(score.width == 600 && score.height == 200 && score.steps.size() == 4 && score.steps[1].kind == Kind::gradient &&
              score.steps[2].align == picture::Align::right && score.steps[2].x == 580 && score.steps[3].x == 580 - 192 - 16 &&
              score.steps[3].colour.a == 0.6f,
          "A score card: a number against the right edge and a caption set by its measure");
    const auto& thumb = request("My Routes:thumbs/loop.png");
    check(thumb.steps.size() == 2 && thumb.steps[0].picture.data == "maps/park.png" && thumb.steps[1].points.size() == 8,
          "A map's thumbnail: its picture and a route over it");

    // At most eight are being drawn at once; each is heard of when it is done, not before.
    fake.pictures_wait = true;
    for (int round = 0; round < 2; ++round) {
        environment.now += 1100;
        for (const char* name : {"a", "b", "c", "d"}) host.run_console("paint " + std::string(name) + std::to_string(round));
    }
    environment.now += 1100;
    host.run_console("paint late");
    check(environment.logged("asked d1 true nil") && environment.logged("asked late false it draws 8 pictures already"),
          "A ninth picture waits for one of the eight\n" + environment.all());
    ticks(host, environment, 2);
    check(!environment.logged("[painter] a0 true"), "A picture still being drawn is not called back");
    fake.picture_jobs.back()->done = true;
    ticks(host, environment, 1);
    check(environment.logged("[painter] d1 true nil") && !environment.logged("[painter] a0 true"), "The one that is done is, whatever the order");
    for (const auto& job : fake.picture_jobs) job->done = true;
    ticks(host, environment, 1);
    check(environment.logged("[painter] a0 true nil") && environment.logged("[painter] c1 true nil"), "and the rest when they are\n" + environment.all());
    // One the game could not write says so in its function; one it cannot start, in the call.
    fake.pictures_wait = false;
    fake.picture_failure = "the file cannot be written";
    environment.now += 1100;
    host.run_console("paint torn");
    ticks(host, environment, 1);
    check(environment.logged("[painter] torn false the file cannot be written"), "A picture that could not be written says why\n" + environment.all());
    fake.picture_failure.clear();
    fake.picture_refusal = "the folder holds 50000 files already";
    host.run_console("paint crowded");
    check(environment.logged("asked crowded false the folder holds 50000 files already"), "One that cannot be started says why at once\n" + environment.all());
    running(host, "painter", environment);
}

// Zips that are files: a plugin lists one from its directory and has it unpacked into a folder, off
// its thread, hearing how it went a tick later. A set of level layouts shipped as one file, a
// download, a backup put back: nothing here is one plugin's.
void plugins_list_and_unpack_zip_files() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    auto& fake = environment.fake;
    const std::string folders = R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["files", "files.folders", "console"], "folders": ["My Routes"]})";
    const std::string levels = R"(
        local files, zip = reskate.files, reskate.zip
        local function said(name)
            return function(ok, why, count) reskate.log(name .. " " .. tostring(ok) .. " " .. tostring(why) .. " " .. tostring(count)) end
        end
        -- A zip of level layouts in the data folder: what it holds, without unpacking it.
        local entries = assert(zip.list("layouts.zip"))
        assert(#entries == 3 and entries[1].name == "layouts/bowl.json" and entries[1].size == 1200 and entries[1].packed == 300 and
               entries[1].crc == 0xCBF43926 and entries[3].name == "readme.txt", "a zip file's list")
        assert(#assert(zip.list("My Routes:shared/more.zip")) == 1, "a zip of a granted folder")
        local none, why = zip.list("nothing.zip")
        assert(none == nil and why == "not found", tostring(why))
        none, why = zip.list("notes.txt")
        assert(none == nil and why == "it is not a zip", tostring(why))
        none, why = zip.list("crowded.zip")
        assert(none == nil and why == "it holds more than 4096 files", tostring(why))
        assert(not pcall(zip.list, "../other/a.zip") and not pcall(zip.list) and not pcall(zip.list, "nul.zip") and not pcall(zip.list, "Other:a.zip"),
               "a zip of its own folders")

        -- One layout out of it, by its name there; and all of it into a folder beside the game, over
        -- what is there.
        assert(files.unzip("layouts.zip", "current", said("one"), {only = {"layouts/bowl.json"}}))
        assert(files.unzip("layouts.zip", "My Routes:shared", said("all"), {overwrite = true}))
        -- Two at once, and the third is told so (it is not an error: it can be asked again).
        local ok, because = files.unzip("layouts.zip", "third")
        assert(ok == false and because == "it unpacks 2 zips already", tostring(because))

        -- What is not a zip to unpack is an error that says what is wrong with it.
        local function refused(reason, ...)
            local fine, message = pcall(files.unzip, ...)
            assert(not fine and message:find(reason, 1, true), reason .. ": " .. tostring(message))
        end
        refused("data folder", "../a.zip", "x")
        refused("data folder", "layouts.zip", "../x")
        refused("folders", "layouts.zip", "C:/x")
        refused("device", "layouts.zip", "x/nul")
        refused("folders", "layouts.zip", "Other:x")
        refused("function", "layouts.zip", "x", "said")
        refused("table", "layouts.zip", "x", nil, "all")
        refused("list of names", "layouts.zip", "x", nil, {only = "layouts/bowl.json"})
        refused("list of names", "layouts.zip", "x", nil, {only = {1}})
        refused("1 to 4096 names", "layouts.zip", "x", nil, {only = {}})
        local many = {}
        for i = 1, 4097 do many[i] = "f" .. i end
        refused("1 to 4096 names", "layouts.zip", "x", nil, {only = many})
        refused("longer than 512", "layouts.zip", "x", nil, {only = {string.rep("a", 513)}})

        reskate.console.command("unpack", "x", function(args)
            local started, said_why = files.unzip("layouts.zip", args[1], said(args[1]), {overwrite = true})
            reskate.log("asked " .. args[1] .. " " .. tostring(started) .. " " .. tostring(said_why))
        end)
        reskate.log("listed")
    )";
    // Two more plugins that unpack, for what all plugins may have going together.
    const auto other = [&](const std::string& id, const std::string& command) {
        fake.files[id + "/saves.zip"] = "PK";
        fake.zips[id + "/saves.zip"] = {{"slot1.sav", 10, 10, 1}};
        return script(id, R"(
        reskate.console.command(")" + command + R"(", "x", function(args)
            local started, why = reskate.files.unzip("saves.zip", args[1], function(ok, because, count)
                reskate.log(args[1] .. " " .. tostring(ok) .. " " .. tostring(because) .. " " .. tostring(count))
            end)
            reskate.log("asked " .. args[1] .. " " .. tostring(started) .. " " .. tostring(why))
        end)
    )", R"("files", "console")");
    };
    fake.files["levels/layouts.zip"] = "PK";
    fake.zips["levels/layouts.zip"] = {{"layouts/bowl.json", 1200, 300, 0xCBF43926u}, {"layouts/plaza.json", 900, 250, 2}, {"readme.txt", 40, 40, 3}};
    fake.files["levels/My Routes:shared/more.zip"] = "PK";
    fake.zips["levels/My Routes:shared/more.zip"] = {{"extra.json", 5, 5, 4}};
    fake.files["levels/notes.txt"] = "not a zip";
    fake.files["levels/crowded.zip"] = "PK";
    fake.zips["levels/crowded.zip"].resize(4097);
    host.apply({package("levels", {{manifest_file, folders}, {"plugin/main.lua", levels}}), other("second", "unpack2"), other("third", "unpack3"),
                script("plain", R"(
        local ok, why = pcall(reskate.zip.list, "a.zip")
        assert(not ok and why:find("files"), why)
        ok, why = pcall(reskate.files.unzip, "a.zip", "x")
        assert(not ok and why:find("files"), why)
        assert(reskate.zip.entries("not a zip") == nil, "a zip it holds as bytes needs no permission")
        reskate.log("refused")
    )"),
                script("half", R"(
        local ok, why = pcall(reskate.files.unzip, "a.zip", "My Routes:x")
        assert(not ok and why:find("files.folders", 1, true), why)
        ok, why = pcall(reskate.zip.list, "My Routes:a.zip")
        assert(not ok and why:find("files.folders", 1, true), why)
        reskate.log("its own folder only")
    )", R"("files")")});
    running(host, "levels", environment);
    check(environment.logged("[levels] listed"), "A plugin lists and unpacks zip files\n" + environment.all());
    check(environment.logged("[plain] refused"), "A zip file needs the files permission, a zip held as bytes does not\n" + environment.all());
    check(environment.logged("[half] its own folder only"), "A zip in a folder beside the game needs files.folders\n" + environment.all());
    check(fake.called("zip list levels/layouts.zip") && fake.called("zip list levels/My Routes:shared/more.zip"),
          "The game is asked for a zip's list by its path, a granted folder's with its folder");
    check(!fake.called("zip list plain") && !fake.called("unzip plain") && !fake.called("unzip half") && !fake.called("to third") && !fake.called("to x "),
          "What was refused never reaches the game");
    check(fake.unzips.size() == 2 && fake.unzips[0].zip == "layouts.zip" && fake.unzips[0].folder == "current" && !fake.unzips[0].all &&
              fake.unzips[0].only == std::vector<std::string>{"layouts/bowl.json"} && !fake.unzips[0].overwrite,
          "One named file of a zip reaches the game as asked");
    check(fake.unzips[1].folder == "My Routes:shared" && fake.unzips[1].all && fake.unzips[1].only.empty() && fake.unzips[1].overwrite,
          "and a whole zip into a granted folder, over what is there");
    check(!environment.logged("one true"), "Nothing is called back in the call that asked");
    ticks(host, environment, 1);
    check(environment.logged("[levels] one true nil 1") && environment.logged("[levels] all true nil 3"),
          "A tick later each zip's function hears how it went, and how many files\n" + environment.all());
    check(fake.files.contains("levels/current/layouts/bowl.json") && !fake.files.contains("levels/current/readme.txt") &&
              fake.files.contains("levels/My Routes:shared/readme.txt"),
          "The files are where they were asked for");

    // Two for a plugin and four for all of them; each is heard of when it is done, not before.
    fake.unzips.clear();
    fake.unzip_jobs.clear();
    fake.unzips_wait = true;
    const auto job = [&](std::string_view folder) {
        for (std::size_t i = 0; i < fake.unzips.size(); ++i)
            if (fake.unzips[i].folder == folder) return fake.unzip_jobs[i];
        throw std::runtime_error("no zip is being unpacked into " + std::string(folder));
    };
    // (As the thread that unpacked one lets go of it.)
    const auto let_go = [&](std::string_view folder) {
        for (std::size_t i = 0; i < fake.unzips.size(); ++i)
            if (fake.unzips[i].folder == folder) {
                fake.unzips.erase(fake.unzips.begin() + static_cast<std::ptrdiff_t>(i));
                fake.unzip_jobs.erase(fake.unzip_jobs.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
    };
    for (const char* name : {"a", "b", "c"}) host.run_console("unpack " + std::string(name));
    check(environment.logged("asked b true nil") && environment.logged("asked c false it unpacks 2 zips already"),
          "A plugin's third zip waits for one of its two\n" + environment.all());
    host.run_console("unpack2 d");
    host.run_console("unpack2 e");
    host.run_console("unpack3 f");
    check(environment.logged("asked e true nil") && environment.logged("asked f false 4 zips are being unpacked already"),
          "and a fifth for all plugins waits for one of the four\n" + environment.all());
    ticks(host, environment, 2);
    check(!environment.logged("[levels] a ") && !environment.logged("[second] d "), "A zip still being unpacked is not called back");
    job("e")->done = true;
    ticks(host, environment, 1);
    check(environment.logged("[second] e true nil 0") && !environment.logged("[second] d "), "The one that is done is, whatever the order");
    let_go("e");
    host.run_console("unpack3 f");
    check(environment.logged("asked f true nil"), "and then there is room for another\n" + environment.all());
    // One that could not be unpacked says why in its function; one that cannot start, in the call.
    job("a")->why = "layouts/bowl.json in it is damaged";
    job("a")->done = true;
    ticks(host, environment, 1);
    check(environment.logged("[levels] a false layouts/bowl.json in it is damaged 0"), "A zip that could not be unpacked says why\n" + environment.all());
    let_go("a");
    fake.unzip_refusal = "that folder is being unpacked into already";
    host.run_console("unpack g");
    check(environment.logged("asked g false that folder is being unpacked into already"), "One that cannot be started says why at once\n" + environment.all());
    fake.unzip_refusal.clear();

    // A plugin that is loaded again while its zip is unpacked: the unpack is told to stop, and what
    // it says afterwards reaches nobody.
    const auto left = job("b");
    for (const char* name : {"d", "f"}) job(name)->done = true;
    ticks(host, environment, 1);
    fake.unzips.clear();
    fake.unzip_jobs.clear();
    fake.unzips_wait = false;
    check(!left->abandoned && zip_file::Job::alive() == 1, "A zip being unpacked goes on while its plugin runs");
    check(host.reload("levels") == "Plugin levels reloaded.", "The plugin loads again\n" + environment.all());
    check(left->abandoned.load(), "A plugin that stops tells its unpack to stop");
    left->done = true;
    ticks(host, environment, 2);
    running(host, "levels", environment);
    check(!environment.logged("[levels] b "), "and is not called with how it went\n" + environment.all());
}

// A zip packed as the tests need one: stored or deflated files, folders, a file whose CRC is wrong,
// and one whose directory says it unpacks to more than it does.
struct Packed {
    std::string name, data;
    bool deflate{}, damaged{};
    std::uint32_t claims{}; // the size it says it unpacks to, when not its data's
};
std::string zip_of(const std::vector<Packed>& files) {
    std::string out, directory;
    const auto u16 = [](std::string& to, std::size_t value) {
        to.push_back(static_cast<char>(value & 0xff));
        to.push_back(static_cast<char>((value >> 8) & 0xff));
    };
    const auto u32 = [&](std::string& to, std::size_t value) {
        u16(to, value & 0xffff);
        u16(to, (value >> 16) & 0xffff);
    };
    for (const auto& file : files) {
        std::string body = file.data;
        if (file.deflate) {
            std::size_t length = 0;
            void* packed = tdefl_compress_mem_to_heap(file.data.data(), file.data.size(), &length, TDEFL_DEFAULT_MAX_PROBES);
            check(packed != nullptr, "The test's zip is packed");
            body.assign(static_cast<const char*>(packed), length);
            mz_free(packed);
        }
        const std::size_t crc = mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(file.data.data()), file.data.size()) ^ (file.damaged ? 1u : 0u);
        const std::size_t method = file.deflate ? 8 : 0, size = file.claims ? file.claims : file.data.size(), offset = out.size();
        u32(out, 0x04034b50);
        for (const std::size_t value : {std::size_t{20}, std::size_t{0}, method, std::size_t{0}, std::size_t{0}}) u16(out, value);
        for (const std::size_t value : {crc, body.size(), size}) u32(out, value);
        u16(out, file.name.size());
        u16(out, 0);
        out += file.name + body;
        u32(directory, 0x02014b50);
        for (const std::size_t value : {std::size_t{20}, std::size_t{20}, std::size_t{0}, method, std::size_t{0}, std::size_t{0}}) u16(directory, value);
        for (const std::size_t value : {crc, body.size(), size}) u32(directory, value);
        u16(directory, file.name.size());
        for (int i = 0; i < 4; ++i) u16(directory, 0); // no extra field or comment, disk 0, no attributes
        u32(directory, file.name.ends_with('/') ? 0x10 : 0);
        u32(directory, offset);
        directory += file.name;
    }
    const auto start = out.size();
    out += directory;
    u32(out, 0x06054b50);
    for (const std::size_t value : {std::size_t{0}, std::size_t{0}, files.size(), files.size()}) u16(out, value);
    u32(out, directory.size());
    u32(out, start);
    u16(out, 0);
    return out;
}

// The reader itself, without a plugin: zips packed here are listed and unpacked on disk, and what is
// written, and what is refused, is looked at.
void the_zip_reader_lists_and_unpacks_a_file() {
    const auto root = scratch / "zips";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto read = [](const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto files_in = [](const std::filesystem::path& folder) {
        std::vector<std::string> names;
        std::error_code error;
        if (std::filesystem::exists(folder, error))
            for (const auto& entry : std::filesystem::recursive_directory_iterator(folder))
                if (entry.is_regular_file()) names.push_back(entry.path().lexically_relative(folder).generic_string());
        std::sort(names.begin(), names.end());
        return names;
    };
    int zips = 0;
    const auto zip_file_of = [&](const std::vector<Packed>& files) {
        const auto file = root / std::format("{}.zip", ++zips);
        write(file, zip_of(files));
        return file;
    };
    // Unpacks `zip` into the folder `name` under the root, which has room for `room` more files.
    std::size_t count = 0;
    const auto unpack = [&](const std::filesystem::path& zip, const std::string& name, zip_file::Request request = {}, std::size_t room = 1000) {
        request.folder = name;
        return zip_file::unpack(request, {zip, root, root / name, room}, count);
    };
    const auto only = [](std::vector<std::string> names) {
        zip_file::Request request;
        request.all = false;
        request.only = std::move(names);
        return request;
    };
    zip_file::Request over;
    over.overwrite = true;

    // A zip of stored and deflated files, a folder of its own and an empty file.
    std::string long_text;
    for (int i = 0; i < 4000; ++i) long_text += "{\"x\": " + std::to_string(i % 7) + "}, ";
    const auto good = zip_file_of({{"readme.txt", "hello"}, {"levels/", ""}, {"levels/park/bowl.json", long_text, true}, {"levels/empty.bin", ""}});
    std::vector<zip_file::Entry> entries;
    check(zip_file::list(good, entries).empty() && entries.size() == 3, "A zip file's list leaves its folders out");
    check(entries[0].name == "readme.txt" && entries[0].size == 5 && entries[0].packed == 5 &&
              entries[0].crc == mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>("hello"), 5),
          "A stored file's name, size and CRC");
    check(entries[1].name == "levels/park/bowl.json" && entries[1].size == long_text.size() && entries[1].packed < long_text.size() / 4 &&
              entries[2].name == "levels/empty.bin" && entries[2].size == 0,
          "A deflated file's size unpacked and packed, in the zip's order");
    check(unpack(good, "a").empty() && count == 3, "A zip unpacks");
    check(files_in(root / "a") == std::vector<std::string>{"levels/empty.bin", "levels/park/bowl.json", "readme.txt"} && read(root / "a" / "readme.txt") == "hello" &&
              read(root / "a" / "levels" / "park" / "bowl.json") == long_text && read(root / "a" / "levels" / "empty.bin").empty(),
          "to its files, each under its own name, and nothing beside them");
    // A file that is there is an error before anything is written, unless it may be replaced.
    write(root / "a" / "readme.txt", "mine");
    std::filesystem::remove(root / "a" / "levels" / "empty.bin");
    check(unpack(good, "a") == "readme.txt is there already" && count == 0 && read(root / "a" / "readme.txt") == "mine" &&
              !std::filesystem::exists(root / "a" / "levels" / "empty.bin"),
          "A file that is there already stops the unpack before it writes anything");
    check(unpack(good, "a", over).empty() && count == 3 && read(root / "a" / "readme.txt") == "hello" && std::filesystem::exists(root / "a" / "levels" / "empty.bin"),
          "With overwrite it is replaced");
    // One named file of it, as a plugin takes one layout out of a set.
    check(unpack(good, "b", only({"levels/park/bowl.json"})).empty() && count == 1 && files_in(root / "b") == std::vector<std::string>{"levels/park/bowl.json"},
          "Only the files named are unpacked");
    check(unpack(good, "c", only({"readme.txt", "levels/none.json"})) == "levels/none.json is not in it" && !std::filesystem::exists(root / "c"),
          "A name the zip does not hold is an error, and nothing is written");
    std::filesystem::create_directories(root / "b" / "readme.txt");
    check(unpack(good, "b", over) == "readme.txt: a folder is there", "A file is not unpacked over a folder");
    check(unpack(good, "d", {}, 2) == "the folder has room for 2 more files, and these are 3" && !std::filesystem::exists(root / "d"),
          "A zip of more files than the folder has room for is not unpacked");
    check(unpack(good, "d", only({"readme.txt"}), 1).empty(), "One that fits is");

    // A name that could leave the folder, or that a file here may not have: nothing is written.
    for (const char* name : {"../evil.txt", "/evil.txt", "C:/evil.txt", "sub\\evil.txt", "sub/../../evil.txt", "nul.txt", "sub/CON", "./evil.txt", "sub//evil.txt",
                             "trailing.", "caf\xc3\xa9.txt", "half.unpacking.tmp"}) {
        const auto bad = zip_file_of({{"fine.txt", "fine"}, {name, "evil"}});
        check(unpack(bad, "e") == std::string(name) + " in it is not a name a file here may have" && !std::filesystem::exists(root / "e"),
              std::string("A zip with a name a file may not have writes nothing: ") + name);
        check(zip_file::list(bad, entries).empty() && entries.size() == 2 && entries[1].name == name, "Its list still says what it holds");
        // (The files a plugin names are the ones checked, so it can take the rest of such a zip.)
        check(unpack(bad, "e", only({"fine.txt"})).empty() && read(root / "e" / "fine.txt") == "fine", "The other files of it can be taken by name");
        check(unpack(bad, "e2", only({name})) == std::string(name) + " in it is not a name a file here may have" && !std::filesystem::exists(root / "e2"),
              "but never the one with such a name");
        std::filesystem::remove_all(root / "e");
    }
    check(!std::filesystem::exists(scratch / "evil.txt") && !std::filesystem::exists(root / "evil.txt") && !std::filesystem::exists(root / "sub"),
          "Nothing was written outside the folder");
    check(unpack(zip_file_of({{std::string(190, 'n') + ".txt", "x"}}), "longer-than-a-path").find("not a name a file here may have") != std::string::npos,
          "A name too long for a data path is refused");
    check(unpack(zip_file_of({{"a.txt", "1"}, {"A.TXT", "2"}}), "f") == "A.TXT is in it twice" && !std::filesystem::exists(root / "f"),
          "Two files that Windows takes for one are refused");

    // A damaged file: the unpack stops, and what it wrote is removed, the folders it made too.
    const auto torn = zip_file_of({{"first.txt", "one"}, {"deep/second.bin", long_text, true, true}, {"third.txt", "three"}});
    check(unpack(torn, "g") == "deep/second.bin in it is damaged" && count == 0 && !std::filesystem::exists(root / "g"),
          "A file that does not match its CRC stops the unpack, and nothing of it stays");
    write(root / "h" / "first.txt", "old");
    check(unpack(torn, "h", over) == "deep/second.bin in it is damaged" && files_in(root / "h") == std::vector<std::string>{"first.txt"} &&
              read(root / "h" / "first.txt") == "old" && !std::filesystem::exists(root / "h" / "deep"),
          "What was there before a damaged zip is as it was");
    check(unpack(zip_file_of({{"stored.bin", "abcdef", false, true}}), "g") == "stored.bin in it is damaged", "A stored file is checked too");
    check(unpack(zip_file_of({{"a", "file"}, {"a/b.txt", "under it"}}), "i") == "a in it is a file and a folder" && !std::filesystem::exists(root / "i"),
          "A zip that names a file and a folder alike is refused");

    // The caps, by what the zip says of itself.
    check(unpack(zip_file_of({{"big.bin", "x", true, false, 600u << 20}}), "j") == "big.bin in it is larger than 512 MB", "A file of more than 512 MB");
    std::vector<Packed> heavy;
    for (int i = 0; i < 5; ++i) heavy.push_back({std::format("part{}.bin", i), "x", true, false, 500u << 20});
    check(unpack(zip_file_of(heavy), "j") == "it unpacks to more than 2 GB" && !std::filesystem::exists(root / "j"), "A zip of more than 2 GB in all");
    std::vector<Packed> crowd;
    for (int i = 0; i < 4097; ++i) crowd.push_back({std::format("f{}.txt", i), ""});
    const auto crowded = zip_file_of(crowd);
    check(zip_file::list(crowded, entries) == "it holds more than 4096 files" && entries.empty() && unpack(crowded, "j") == "it holds more than 4096 files",
          "A zip of more than 4096 files is neither listed nor unpacked");
    crowd.pop_back();
    check(zip_file::list(zip_file_of(crowd), entries).empty() && entries.size() == 4096, "One of 4096 is");

    // What is not a zip.
    write(root / "text.zip", "hello, this is not a zip at all");
    check(zip_file::list(root / "text.zip", entries) == "it is not a zip" && unpack(root / "text.zip", "j") == "it is not a zip", "A file that is not a zip");
    check(zip_file::list(root / "none.zip", entries) == "not found" && unpack(root / "none.zip", "j") == "not found", "A file that is not there");
    auto cut = zip_of({{"readme.txt", "hello"}});
    cut.resize(cut.size() - 8);
    write(root / "cut.zip", cut);
    check(zip_file::list(root / "cut.zip", entries) == "it is not a zip", "A zip cut short");

    // An unpack that is told to stop writes nothing; and one on a thread of its own says how it went.
    const std::atomic<bool> stop{true};
    zip_file::Request all;
    all.folder = "k";
    check(zip_file::unpack(all, {good, root, root / "k", 1000}, count, &stop) == "it was stopped" && !std::filesystem::exists(root / "k"),
          "An unpack that is stopped leaves nothing");
    const int before = zip_file::Job::alive();
    {
        auto job = std::make_shared<zip_file::Job>();
        check(zip_file::Job::alive() == before + 1, "The zips being unpacked are counted");
        check(zip_file::unpack_later(all, {good, root, root / "k", 1000}, job).empty(), "An unpack starts on a thread of its own");
        for (int i = 0; i < 2000 && !job->done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(job->done && job->why.empty() && job->count == 3 && read(root / "k" / "levels" / "park" / "bowl.json") == long_text,
              "and says how it went when it is done: " + job->why);
        // (The thread lets go of the job a moment after it says it is done.)
        for (int i = 0; i < 2000 && job.use_count() > 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(zip_file::Job::alive() == before, "and no longer once it is let go of");
}

#ifdef _WIN32
// The renderer itself, without a plugin: what it writes is read again and looked at, a pixel at a time.
void the_picture_renderer_draws_what_it_is_told() {
    using Kind = picture::Step::Kind;
    const auto folder = scratch / "pictures";
    std::filesystem::create_directories(folder);
    const auto made = [&](const picture::Request& request, const std::string& name) {
        const auto why = picture::make(request, folder / name);
        check(why.empty(), name + " is drawn: " + why);
        check(!std::filesystem::exists(folder / (name + ".tmp")), "Nothing is left beside " + name);
        std::ifstream in(folder / name, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        picture::Pixels pixels;
        check(picture::decode(bytes, pixels) && pixels.width == static_cast<std::uint32_t>(request.width) &&
                  pixels.height == static_cast<std::uint32_t>(request.height),
              name + " is a PNG of the size asked for");
        for (std::size_t i = 3; i < pixels.rgba.size(); i += 4) check(pixels.rgba[i] == 255, name + " is opaque");
        return pixels;
    };
    const auto at = [](const picture::Pixels& pixels, int x, int y) {
        const auto* p = pixels.rgba.data() + (static_cast<std::size_t>(y) * pixels.width + static_cast<std::size_t>(x)) * 4;
        return std::array<int, 3>{p[0], p[1], p[2]};
    };
    const auto is = [](const std::array<int, 3>& pixel, int r, int g, int b, int slack = 2) {
        return std::abs(pixel[0] - r) <= slack && std::abs(pixel[1] - g) <= slack && std::abs(pixel[2] - b) <= slack;
    };
    const auto fill = [](picture::Colour colour) {
        picture::Step step;
        step.kind = Kind::fill;
        step.colour = colour;
        return step;
    };
    const auto poly = [](std::vector<std::array<float, 2>> points, picture::Colour colour) {
        picture::Step step;
        step.kind = Kind::poly;
        step.points = std::move(points);
        step.colour = colour;
        return step;
    };
    const auto named = [&](const std::string& name) { return picture::File{name, folder / name, {}}; };
    const picture::Colour red{1, 0, 0, 1}, green{0, 1, 0, 1}, blue{0, 0, 1, 1}, yellow{1, 1, 0, 1}, white{1, 1, 1, 1}, black{0, 0, 0, 1};

    // A fill, a polygon that goes round twice (a square and a smaller one inside it, one outline), a
    // triangle whose slanted edge cuts pixels in half, and a layer turned a quarter clockwise.
    picture::Request shapes;
    shapes.width = shapes.height = 64;
    shapes.steps.push_back(fill(blue));
    shapes.steps.push_back(poly({{8, 8}, {40, 8}, {40, 40}, {8, 40}, {8, 8}, {16, 16}, {32, 16}, {32, 32}, {16, 32}, {16, 16}}, red));
    shapes.steps.push_back(poly({{48, 8}, {60, 8}, {48, 20}}, red));
    picture::Step layer;
    layer.kind = Kind::layer;
    layer.w = 20;
    layer.h = 10;
    layer.x = 32;
    layer.y = 52;
    layer.angle = 1.5707963f;
    layer.steps.push_back(poly({{0, 0}, {20, 0}, {20, 10}, {0, 10}}, green));
    layer.steps.push_back(poly({{0, 0}, {4, 0}, {4, 10}, {0, 10}}, yellow));
    shapes.steps.push_back(layer);
    const auto drawn = made(shapes, "shapes.png");
    check(is(at(drawn, 50, 50), 0, 0, 255) && is(at(drawn, 2, 2), 0, 0, 255), "Outside every shape the fill shows");
    check(is(at(drawn, 10, 30), 255, 0, 0) && is(at(drawn, 36, 12), 255, 0, 0) && is(at(drawn, 8, 8), 255, 0, 0) && is(at(drawn, 39, 39), 255, 0, 0),
          "Inside the polygon its colour does, to its edge");
    check(is(at(drawn, 24, 24), 0, 0, 255) && is(at(drawn, 17, 30), 0, 0, 255), "Where its outline goes round twice it is not filled: even-odd");
    check(is(at(drawn, 7, 20), 0, 0, 255) && is(at(drawn, 40, 20), 0, 0, 255), "and nothing past its edge");
    check(is(at(drawn, 50, 10), 255, 0, 0) && is(at(drawn, 58, 18), 0, 0, 255) && is(at(drawn, 53, 14), 128, 0, 128, 16),
          "A slanted edge is smoothed: a pixel it halves is half the polygon's colour");
    check(is(at(drawn, 32, 50), 0, 255, 0) && is(at(drawn, 32, 60), 0, 255, 0) && is(at(drawn, 24, 52), 0, 0, 255) && is(at(drawn, 40, 52), 0, 0, 255),
          "A layer turned a quarter stands where it lay");
    check(is(at(drawn, 32, 43), 255, 255, 0) && is(at(drawn, 30, 45), 255, 255, 0), "and clockwise: its left end is at the top");

    // Pictures the renderer made, read again by it: tiled, stretched, and filling a canvas.
    picture::Request checker;
    checker.width = checker.height = 2;
    checker.steps = {fill(white), poly({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, black), poly({{1, 1}, {2, 1}, {2, 2}, {1, 2}}, black)};
    const auto squares = made(checker, "checker.png");
    check(is(at(squares, 0, 0), 0, 0, 0) && is(at(squares, 1, 0), 255, 255, 255) && is(at(squares, 0, 1), 255, 255, 255) && is(at(squares, 1, 1), 0, 0, 0),
          "A polygon on whole pixels fills them and no others");
    check(picture::size_of(folder / "checker.png") == std::pair<std::uint32_t, std::uint32_t>{2, 2}, "A picture's size is read from its file");
    write(folder / "notes.png", "not a picture");
    picture::Pixels none;
    check(!picture::size_of(folder / "notes.png") && !picture::size_of(folder / "nothing.png") && !picture::decode("not a picture", none),
          "and what is not a PNG or a JPEG has none");

    picture::Request tiled;
    tiled.width = tiled.height = 16;
    picture::Step tile;
    tile.kind = Kind::tile;
    tile.picture = named("checker.png");
    tile.texel = 4;
    tiled.steps = {fill(red), tile};
    const auto tiles = made(tiled, "tiled.png");
    check(is(at(tiles, 1, 1), 0, 0, 0) && is(at(tiles, 3, 3), 0, 0, 0) && is(at(tiles, 5, 1), 255, 255, 255) && is(at(tiles, 5, 5), 0, 0, 0) &&
              is(at(tiles, 9, 1), 0, 0, 0) && is(at(tiles, 9, 5), 255, 255, 255) && is(at(tiles, 15, 15), 0, 0, 0),
          "A tiled picture repeats from the top left, four pixels to a texel, the nearest");

    picture::Request stretched;
    stretched.width = stretched.height = 16;
    picture::Step image;
    image.kind = Kind::image;
    image.picture = named("checker.png");
    image.w = image.h = 16;
    stretched.steps = {fill(red), image};
    const auto smooth = made(stretched, "stretched.png");
    check(is(at(smooth, 0, 0), 0, 0, 0) && is(at(smooth, 15, 0), 255, 255, 255) && is(at(smooth, 0, 15), 255, 255, 255) && is(at(smooth, 15, 15), 0, 0, 0),
          "A stretched picture's edge texels reach its edges");
    check(is(at(smooth, 7, 0), 112, 112, 112, 6) && at(smooth, 5, 0)[0] < at(smooth, 7, 0)[0] && at(smooth, 7, 0)[0] < at(smooth, 9, 0)[0],
          "and between two texels it is blended");
    image.x = 4;
    image.y = 4;
    image.w = image.h = 8;
    image.alpha = 0.5f;
    stretched.steps = {fill(red), image};
    const auto faded = made(stretched, "faded.png");
    check(is(at(faded, 2, 2), 255, 0, 0) && is(at(faded, 13, 8), 255, 0, 0) && is(at(faded, 4, 4), 128, 0, 0, 3) && is(at(faded, 11, 4), 255, 128, 128, 3),
          "A picture in a box is drawn only there, over what is under it by its alpha");

    picture::Request halves;
    halves.width = 4;
    halves.height = 2;
    halves.steps = {fill(red), poly({{2, 0}, {4, 0}, {4, 2}, {2, 2}}, green)};
    made(halves, "halves.png");
    picture::Request filled;
    filled.width = filled.height = 16;
    picture::Step whole;
    whole.kind = Kind::image;
    whole.picture = named("halves.png");
    whole.cover = true;
    whole.focus = 0;
    filled.steps = {fill(blue), whole};
    check(is(at(made(filled, "left.png"), 8, 8), 255, 0, 0) && is(at(made(filled, "left.png"), 4, 12), 255, 0, 0),
          "A picture filling the canvas keeps its shape: with its focus at 0 its left shows");
    filled.steps[1].focus = 1;
    check(is(at(made(filled, "right.png"), 8, 8), 0, 255, 0) && is(at(made(filled, "right.png"), 12, 4), 0, 255, 0), "and at 1 its right");

    // A picture that is not there draws nothing, and a polygon naming one falls back to its colour.
    picture::Request absent;
    absent.width = absent.height = 8;
    tile.picture = named("nothing.png");
    auto patch = poly({{0, 0}, {4, 0}, {4, 4}, {0, 4}}, yellow);
    patch.picture = named("nothing.png");
    absent.steps = {fill(blue), tile, patch};
    const auto without = made(absent, "absent.png");
    check(is(at(without, 6, 6), 0, 0, 255) && is(at(without, 2, 2), 255, 255, 0), "A step whose picture cannot be read draws nothing, or its colour");

    // A gradient: the colour over each row, its alpha going evenly from one end to the other.
    picture::Request shaded;
    shaded.width = 8;
    shaded.height = 200;
    picture::Step shade;
    shade.kind = Kind::gradient;
    shade.colour = white;
    shade.y0 = 50;
    shade.y1 = 150;
    shade.a0 = 0;
    shade.a1 = 1;
    shaded.steps = {fill(black), shade};
    const auto ramp = made(shaded, "shaded.png");
    check(is(at(ramp, 4, 20), 0, 0, 0) && is(at(ramp, 4, 180), 0, 0, 0) && is(at(ramp, 4, 100), 129, 129, 129, 3) && is(at(ramp, 4, 149), 254, 254, 254, 3) &&
              is(at(ramp, 4, 50), 1, 1, 1, 2),
          "A gradient shades its own rows and no others");

    // What is not a request to draw is refused whole.
    std::string png;
    picture::Request wrong = shapes;
    wrong.width = 0;
    check(!picture::render(wrong, png).empty(), "A picture has a size");
    wrong = shapes;
    wrong.steps[3].steps.push_back(layer);
    check(!picture::render(wrong, png).empty(), "A layer holds no layer");
    wrong = shapes;
    wrong.steps[1].points[0][0] = std::numeric_limits<float>::infinity();
    check(!picture::render(wrong, png).empty(), "A point is finite");
    check(!picture::make(shapes, folder / "no such folder" / "a.png").empty() && !std::filesystem::exists(folder / "no such folder"),
          "A picture that cannot be written leaves nothing");

    // Text, in a font Windows reads from a file of ours: where there is one to copy.
    const std::filesystem::path arial = "C:/Windows/Fonts/arial.ttf";
    std::error_code error;
    if (!std::filesystem::is_regular_file(arial, error)) return;
    std::filesystem::copy_file(arial, folder / "plain.ttf", std::filesystem::copy_options::overwrite_existing);
    const auto font = folder / "plain.ttf";
    const auto width = picture::text_width(font, "12,340", 64);
    const auto doubled = picture::text_width(font, "12,340", 128);
    check(width && *width > 100 && *width < 400 && doubled && std::abs(*doubled - *width * 2) < 1, "A text has a width, and twice it at twice the size");
    check(picture::text_width(font, "12,340 12,340", 64).value_or(0) > *width * 2 && picture::text_width(font, "", 64) == 0.0,
          "A longer text is wider, and none has no width");
    check(!picture::text_width(folder / "notes.png", "x", 64) && !picture::text_width(folder / "nothing.ttf", "x", 64), "What is not a font measures nothing");
    picture::Request card;
    card.width = 600;
    card.height = 200;
    picture::Step number;
    number.kind = Kind::text;
    number.font = named("plain.ttf");
    number.text = "12,340";
    number.size = 64;
    number.x = 580;
    number.y = 130;
    number.colour = white;
    number.align = picture::Align::right;
    card.steps = {fill(black), number};
    const auto bright = [&](const picture::Pixels& pixels, int left, int right) {
        int count = 0;
        for (int y = 0; y < 200; ++y)
            for (int x = std::max(left, 0); x < std::min(right, 600); ++x) count += at(pixels, x, y)[0] > 128;
        return count;
    };
    const int start = 580 - static_cast<int>(*width);
    const auto right = made(card, "right.png");
    check(bright(right, start - 2, 582) > 500 && !bright(right, 0, start - 4) && !bright(right, 584, 600), "A text set to the right ends at its x");
    int lowest = 0;
    for (int y = 0; y < 200; ++y)
        if (at(right, 560, y)[0] > 128) lowest = y;
    check(lowest >= 126 && lowest <= 131, "and its digits stand on its baseline");
    card.steps[1].align = picture::Align::left;
    card.steps[1].x = 20;
    const auto left = made(card, "left-text.png");
    check(bright(left, 18, 22 + static_cast<int>(*width)) > 500 && !bright(left, 0, 16) && !bright(left, 26 + static_cast<int>(*width), 600),
          "one set to the left starts there");
    card.steps[1].align = picture::Align::center;
    card.steps[1].x = 300;
    const auto middle = made(card, "middle-text.png");
    check(bright(middle, 300 - static_cast<int>(*width) / 2 - 2, 300 + static_cast<int>(*width) / 2 + 2) > 500 &&
              !bright(middle, 0, 300 - static_cast<int>(*width) / 2 - 4) && !bright(middle, 300 + static_cast<int>(*width) / 2 + 4, 600),
          "and one centred has its middle there");
    card.steps[1].font = named("nothing.ttf");
    check(!bright(made(card, "no-font.png"), 0, 600), "A text whose font cannot be read draws nothing");
}

// A plugin's own font as Windows reads it (plugin_font.h): the metrics its texts are measured by, and
// the glyphs they are drawn with. Where there is a font file to copy.
void windows_reads_a_fonts_metrics_and_glyphs() {
    namespace font = dingosdk::plugins::font;
    const auto folder = scratch / "fonts";
    std::filesystem::create_directories(folder);
    write(folder / "notes.ttf", "not a font at all");
    check(!font::metrics(folder / "notes.ttf") && !font::metrics(folder / "nothing.ttf") && !font::known_metrics(folder / "notes.ttf"),
          "What is not a font has no metrics");
    std::vector<font::Coverage> glyphs;
    const std::uint32_t capital[] = {'H'};
    check(!font::rasterise(folder / "notes.ttf", 32, capital, 1022, glyphs) && glyphs.empty(), "and no glyphs");
    const std::filesystem::path arial = "C:/Windows/Fonts/arial.ttf";
    std::error_code error;
    if (!std::filesystem::is_regular_file(arial, error)) return;
    const auto file = folder / "plain.ttf";
    std::filesystem::copy_file(arial, file, std::filesystem::copy_options::overwrite_existing);
    const auto m = font::metrics(file);
    check(m && m->own && m->size == 1 && m->ascent > 0.6f && m->ascent < 1, "A font's metrics are at size 1, a line's height, its baseline most of the way down");
    const auto of = [](char c) { return static_cast<std::size_t>(c - 32); };
    check(!m->lacks[of('H')] && !m->lacks[of(' ')] && m->lacks[127 - 32], "It has its letters and its space, and no glyph for what has none");
    check(m->advance[of('H')] > 0.5f && m->advance[of('H')] < 0.8f && m->advance[of('i')] < m->advance[of('W')] && m->advance[of(' ')] > 0.1f,
          "Each character has its own advance");
    for (char c = '0'; c <= '9'; ++c) check(m->advance[of(c)] == m->advance[of('0')], "and the digits one advance, so a number does not shuffle");
    const auto& box = m->box[of('H')];
    check(box[0] > 0 && box[2] < m->advance[of('H')] && box[1] > 0.05f && box[1] < 0.4f && std::abs(box[3] - m->ascent) < 0.005f,
          "A capital's box is inside its advance and stands on the baseline");
    check(m->box[of(' ')] == std::array<float, 4>{} && m->box[of('g')][3] > m->ascent + 0.1f && m->box[of('g')][1] > box[1],
          "A space has no box, and a g starts lower and hangs below the line");
    check(plugin_ui::has_glyph(*m, 0xe9) && plugin_ui::has_glyph(*m, 0x416) && !plugin_ui::has_glyph(*m, 0xe000) && !plugin_ui::has_glyph(*m, 9) &&
              std::is_sorted(m->wide.begin(), m->wide.end()) && m->wide.size() > 500,
          "The characters past ASCII it has are known: accents and Cyrillic, not a private one");
    check(font::known_metrics(file) == m && font::metrics(file) == m, "A file is read once");
    float sum = 0;
    for (const char c : std::string_view("12,340")) sum += m->advance[of(c)] * 64;
    check(std::abs(plugin_ui::measure(*m, 64, "12,340", 0)[0] - sum) < 0.01f && plugin_ui::measure(*m, 64, "12,340", 0)[1] == 64,
          "A text is as wide as its characters' advances, with no kerning, and a line high");

    // Its glyphs: grey coverage, each in the box its metrics gave.
    const std::uint32_t codes[] = {'H', ' ', 0xe000, 'g', 0x416};
    check(font::rasterise(file, 100, codes, 1022, glyphs) && glyphs.size() == 5, "Glyphs are made for the characters asked for, in their order");
    using Kind = font::Coverage::Kind;
    const auto h = glyphs[0];
    check(h.kind == Kind::drawn && h.code == 'H' && h.grey.size() == static_cast<std::size_t>(h.width) * static_cast<std::size_t>(h.height), "A letter is a box of greys");
    check(std::abs(static_cast<float>(h.left) - box[0] * 100) < 1.5f && std::abs(static_cast<float>(h.top) - (box[1] - m->ascent) * 100) < 1.5f &&
              std::abs(static_cast<float>(h.width) - (box[2] - box[0]) * 100) < 2.5f && std::abs(static_cast<float>(h.height) - (box[3] - box[1]) * 100) < 2.5f,
          std::format("drawn where its metrics said it would be: {} {} {}x{} against {} {} {}x{}", h.left, h.top, h.width, h.height, box[0] * 100,
                      (box[1] - m->ascent) * 100, (box[2] - box[0]) * 100, (box[3] - box[1]) * 100));
    const auto inked = std::count_if(h.grey.begin(), h.grey.end(), [](std::uint8_t grey) { return grey > 200; });
    const auto clear = std::count(h.grey.begin(), h.grey.end(), std::uint8_t{0});
    check(inked * 5 > static_cast<long long>(h.grey.size()) && clear * 5 > static_cast<long long>(h.grey.size()), "its strokes solid and the space between them clear");
    check(glyphs[1].kind == Kind::blank && glyphs[1].grey.empty(), "A space has nothing to draw");
    check(glyphs[2].kind == Kind::missing && glyphs[2].code == 0xe000, "A character the font lacks is missing");
    check(glyphs[3].kind == Kind::drawn && glyphs[3].top + glyphs[3].height > 10 && glyphs[4].kind == Kind::drawn, "A g hangs below the baseline, and a Cyrillic letter is drawn");
    check(font::rasterise(file, 100, capital, 20, glyphs) && glyphs.size() == 1 && glyphs[0].kind == Kind::missing, "A glyph too large to keep is missing");
    check(font::rasterise(file, 50, capital, 1022, glyphs) && std::abs(glyphs[0].height * 2 - h.height) <= 2, "At half the size it is half as tall");

    // The file written again is another font: read again, and measured as that one.
    const std::filesystem::path mono = "C:/Windows/Fonts/consola.ttf";
    if (!std::filesystem::is_regular_file(mono, error)) return;
    std::filesystem::copy_file(mono, file, std::filesystem::copy_options::overwrite_existing);
    const auto again = font::metrics(file);
    check(again && again != m && again->advance[of('i')] == again->advance[of('W')] && font::known_metrics(file) == again,
          "A font file written again is read again");
}
#endif

// Web requests with what a plugin that talks to a service needs: its own headers, the other methods,
// an answer written to a file and gone on with, a longer wait, redirects to its own hosts, the answer's
// headers, how far a download has come, and giving one up. A leaderboard, a route sharing site, a
// pack of pictures: nothing here is one plugin's.
void plugins_make_richer_web_requests() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const std::string main = R"(
        local sent = {}
        local function ask(request)
            return reskate.http.request(request, function(status, body, err, headers)
                table.insert(sent, {status = status, body = body, err = err, headers = headers})
            end)
        end
        local upload = ask({url = "https://api.example.com/v1/routes/7", method = "PUT", body = "{}", content_type = "application/json",
                            headers = {["X-Api-Version"] = "2", Accept = "application/json"}, timeout = 120})
        local download = ask({url = "https://cdn.example.com/packs/7.zip", to_file = "downloads/7.zip", resume = true, most = 200 * 1048576, follow = true})
        assert(upload == 1 and download == 2)
        for _, bad in {
            {url = "https://api.example.com/x", method = "CONNECT"},
            {url = "https://api.example.com/x", headers = {Host = "evil.example.org"}},
            {url = "https://api.example.com/x", headers = {Cookie = "a=b"}},
            {url = "https://api.example.com/x", headers = {["Proxy-Authorization"] = "x"}},
            {url = "https://api.example.com/x", headers = {["Bad Name"] = "x"}},
            {url = "https://api.example.com/x", headers = {Accept = "a\r\nHost: evil"}},
            {url = "https://api.example.com/x", to_file = "../outside.zip"},
            {url = "https://api.example.com/x", resume = true},
            {url = "https://api.example.com/x", most = 64 * 1048576},
            {url = "https://api.example.com/x", timeout = 0},
        } do
            assert(not pcall(ask, bad), "refused: " .. reskate.json.encode(bad))
        end
        reskate.console.command("going", "x", function()
            local p = reskate.http.progress(2)
            reskate.log(p and string.format("going %d of %d", p.received, p.total) or "not going")
            assert(reskate.http.progress(99) == nil)
            reskate.http.cancel(2)
            reskate.http.cancel(99)
        end)
        reskate.console.command("answers", "x", function()
            for _, a in sent do reskate.log(string.format("answer %d %q %s etag %s", a.status, a.body, tostring(a.err), tostring(a.headers.etag))) end
        end)
    )";
    host.apply({package("client", {{manifest_file, R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["net.http", "files", "console"],
                                                     "domains": ["api.example.com", "cdn.example.com"]})"}, {"plugin/main.lua", main}}),
                package("fileless", {{manifest_file, R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["net.http"], "domains": ["api.example.com"]})"},
                                     {"plugin/main.lua", R"(
        local ok, why = pcall(reskate.http.request, {url = "https://api.example.com/x", to_file = "x.bin"}, function() end)
        assert(not ok and why:find("files"), why)
        reskate.log("no file")
    )"}})});
    running(host, "client", environment);
    check(environment.logged("[fileless] no file"), "An answer goes to a file only with the files permission\n" + environment.all());
    check(environment.requests.size() == 2, "The requests it may make are made\n" + environment.all());
    const auto& upload = environment.requests[0];
    check(upload.method == "PUT" && upload.body == "{}" && upload.timeout_ms == 120000 && upload.headers.size() == 2 && upload.to_file.empty() &&
              upload.most == (1u << 20) && !upload.follow && upload.domains.empty(),
          "A request carries its method, its headers and its wait");
    const bool versioned = std::any_of(upload.headers.begin(), upload.headers.end(), [](const auto& h) { return h.first == "X-Api-Version" && h.second == "2"; });
    check(versioned, "its headers as it wrote them");
    const auto& download = environment.requests[1];
    check(download.to_file == std::filesystem::path("data") / "client" / "downloads/7.zip" && download.resume && download.follow &&
              download.most == 200ull * 1048576 && download.domains == std::vector<std::string>{"api.example.com", "cdn.example.com"},
          "An answer goes to a file of its data folder, and redirects only to its own hosts");
    environment.http_going[2] = {1500, 4000};
    host.run_console("going");
    check(environment.logged("[client] going 1500 of 4000") && environment.http_cancelled == std::vector<std::uint64_t>{2},
          "It reads how far a download has come and gives up its own only\n" + environment.all());
    environment.responses.push_back({1, "client", 200, "saved", {}, {{"etag", "\"v7\""}, {"content-type", "application/json"}}});
    environment.responses.push_back({2, "client", 0, {}, "it was given up", {}});
    ticks(host, environment, 1);
    host.run_console("answers");
    check(environment.logged("[client] answer 200 \"saved\" nil etag \"v7\"") && environment.logged("[client] answer 0 \"\" it was given up etag nil"),
          "An answer comes with its headers\n" + environment.all());
}

void plugins_draw_the_games_map() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    const std::string main = R"(
        reskate.on("draw", function(d)
            local map = reskate.world.map()
            reskate.log(map and string.format("map %d %d, %d x %d", map.centre.x, map.centre.z, map.size.x, map.size.z) or "no map")
            d:game_map(vector.create(10, 20, 0), vector.create(210, 220, 0), {centre = vector.create(5, 1, -7), range = 60, heading = 0.5, colour = 0xffffffc0})
            d:game_map(vector.create(0, 0, 0), vector.create(100, 100, 0), {centre = vector.create(0, 0, 0), range = 2000})
            assert(not pcall(d.game_map, d, vector.create(0, 0, 0), vector.create(1, 1, 0), {centre = vector.create(0, 0, 0)}), "a range")
            assert(not pcall(d.game_map, d, vector.create(0, 0, 0), vector.create(1, 1, 0), {centre = vector.create(0, 0, 0), range = 3}), "5 metres at least")
            assert(not pcall(d.game_map, d, vector.create(0, 0, 0), vector.create(1, 1, 0), {centre = vector.create(0, 0, 0), range = 2001}), "2000 at most")
            assert(not pcall(d.game_map, d, vector.create(0, 0, 0), vector.create(1, 1, 0), {range = 60}), "a centre")
            assert(not pcall(d.game_map, d, vector.create(0, 0, 0), vector.create(1, 1, 0)), "its options")
            reskate.log("drew")
        end)
    )";
    host.apply({package("radar", {{manifest_file, manifest(R"("overlay")")}, {"plugin/main.lua", main}}),
                package("blind", {{manifest_file, manifest(R"("console")")}, {"plugin/main.lua", R"(
        local ok, why = pcall(reskate.world.map)
        reskate.log(ok and "read the map" or tostring(why))
    )"}})});
    ticks(host, environment, 1);
    running(host, "radar", environment);
    check(environment.logged("[radar] no map") && environment.logged("[radar] drew"), "A level with no map has none, and drawing it is no error\n" + environment.all());
    check(environment.layers.size() == 1 && environment.layers[0].shapes.size() == 2, "The map is drawn as a shape\n" + environment.all());
    if (environment.layers.size() == 1 && environment.layers[0].shapes.size() == 2) {
        const auto& map = environment.layers[0].shapes[0];
        check(map.kind == plugin_ui::Shape::Kind::game_map && map.points.size() == 3 && map.points[0][0] == 10 && map.points[1][1] == 220 &&
                  map.points[2][0] == 5 && map.points[2][2] == -7 && map.radius == 60 && map.turn == 0.5f && map.style.colour == 0xc0ffffff,
              "in its box, about a place in the world, so many metres to its side, turned and tinted");
        check(environment.layers[0].shapes[1].turn == 0 && environment.layers[0].shapes[1].radius == 2000, "unturned when no heading is given");
    }
    check(environment.logged("[blind]") && !environment.logged("[blind] read the map") && environment.all().find("\"overlay\"") != std::string::npos,
          "A plugin without the overlay permission is not told the map\n" + environment.all());
    environment.fake.the_map = Game::MapArea{{300, 0, 1444}, {2206, 0, 1241}};
    ticks(host, environment, 1);
    check(environment.logged("[radar] map 300 1444, 2206 x 1241"), "Its middle and size, where the level has one\n" + environment.all());
}

// A copy of the local skater, drawn somewhere else, with "skater.copy".
void plugins_show_a_skater_copy() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        assert(not reskate.skater.copy_shown())
        assert(reskate.skater.copy({position = vector.create(1, -70, -5)}))
        assert(reskate.skater.copy_shown())
        assert(reskate.skater.copy({position = vector.create(1, -70, -5), board = true}))
        assert(not pcall(reskate.skater.copy, {position = "here"}), "a position is a vector")
        assert(not pcall(reskate.skater.copy, {position = vector.create(0, 0, 0), board = 1}), "board is true or false")
        assert(reskate.skater.copy(nil))
        assert(not reskate.skater.copy_shown())
        reskate.log("copied")
    )";
    host.apply({package("booth", {{manifest_file, manifest(R"("skater.copy")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    running(host, "booth", environment);
    check(environment.logged("[booth] copied"), "A plugin shows a copy of the skater\n" + environment.all());
    check(game.called("copy booth at 1 -70 -5 board false") && game.called("copy booth at 1 -70 -5 board true") && game.called("copy booth gone"),
          "The copy is placed, with its board or not, and let go\n" + environment.all());
    host.apply({package("sneaky", {{manifest_file, manifest(R"("skater.read")")}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.skater.copy, {position = vector.create(0, 0, 0)})) .. " " .. tostring(pcall(reskate.skater.copy_shown)))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false false"), "skater.copy is needed\n" + environment.all());
}

// The game's own set pieces: a light rig or a backdrop prefab, placed with "world.sets".
void plugins_place_set_pieces() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        local rig = "lighting/prefabs/character/pf_lightrig_cas_character"
        assert(reskate.entities.piece(1, rig, {position = vector.create(1, 2, 3)}))
        assert(reskate.entities.piece_ready(1) and not reskate.entities.piece_ready(2))
        local ok, why = reskate.entities.piece(2, "gameplay/store/prefab_store", {position = vector.create(0, 0, 0)})
        assert(ok == false and string.find(why, "light rigs", 1, true), "only the game's lights and backdrops: " .. tostring(why))
        assert(not pcall(reskate.entities.piece, 0, rig, {position = vector.create(0, 0, 0)}), "ids start at 1")
        assert(not pcall(reskate.entities.piece, 3, rig, {position = vector.create(0 / 0, 0, 0)}), "a finite position")
        reskate.entities.piece_remove(1)
        reskate.log("lit")
    )";
    host.apply({package("studio", {{manifest_file, manifest(R"("world.sets")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    running(host, "studio", environment);
    check(environment.logged("[studio] lit"), "A plugin places a set piece\n" + environment.all());
    check(game.called("piece studio 1 lighting/prefabs/character/pf_lightrig_cas_character at 1 2 3") && game.called("piece remove studio 1"),
          "The piece is placed and taken away\n" + environment.all());
    host.apply({});
    check(game.called("entities clear studio"), "A plugin's set pieces go with it, with its objects\n" + environment.all());
    // Without the permission, refused: world.entities is not enough.
    host.apply({package("props", {{manifest_file, manifest(R"("world.entities")")}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.entities.piece, 1, "lighting/prefabs/character/pf_lightrig_cas_character", {position = vector.create(0, 0, 0)})) .. " " ..
                    tostring(pcall(reskate.entities.piece_remove, 1)) .. " " .. tostring(pcall(reskate.entities.piece_ready, 1)))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[props] false false false"), "world.sets is needed\n" + environment.all());
}

void plugins_place_world_entities() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        local gate = "World/Props/Gate_Arch"
        assert(reskate.entities.set(1, gate, {position = vector.create(1, 2, 3), yaw = 90, scale = 2}))
        assert(reskate.entities.set(2, gate, {position = vector.create(0, 0, 0), right = vector.create(1, 0, 0),
            up = vector.create(0, 1, 0), forward = vector.create(0, 0, 3)}))
        assert(reskate.entities.set(3, gate, {matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 7,8,9,1}}, true))
        assert(reskate.entities.status(1) == "ready" and reskate.entities.status(3) == "hidden" and reskate.entities.status(9) == nil)
        assert(not pcall(reskate.entities.set, 0, gate, {position = vector.create(0, 0, 0)}), "ids start at 1")
        assert(not pcall(reskate.entities.set, 4, gate, {position = vector.create(0 / 0, 0, 0)}), "a finite position")
        assert(not pcall(reskate.entities.set, 4, gate, {matrix = {1, 2, 3}}), "16 numbers")
        -- On a skater's joint: yours by name, another player's by number.
        assert(reskate.entities.set(4, "Props/Camcorder", {position = vector.create(0, 0.1, 0)}, false, {joint = "right_hand"}))
        assert(reskate.entities.set(5, "Props/Camcorder", {position = vector.create(0, 0, 0)}, false, {player = "76561198000000001", joint = 103}))
        assert(not pcall(reskate.entities.set, 6, gate, {position = vector.create(0, 0, 0)}, false, {joint = 395}), "joints end at 394")
        assert(not pcall(reskate.entities.set, 6, gate, {position = vector.create(0, 0, 0)}, false, {joint = "elbow"}), "a joint's name")
        assert(not pcall(reskate.entities.set, 6, gate, {position = vector.create(0, 0, 0)}, false, {player = "me", joint = 1}), "a player")
        reskate.entities.remove(2)
        assert(reskate.entities.status(2) == nil and reskate.entities.ready())
        reskate.log("placed")
    )";
    host.apply({package("props", {{manifest_file, manifest(R"("world.entities")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    running(host, "props", environment);
    check(environment.logged("[props] placed"), "A plugin places world objects\n" + environment.all());
    // Yaw 90: forward is +x (sin, 0, cos), scaled 2; the axes and the matrix come as given.
    check(game.called("entity props 1 World/Props/Gate_Arch 1.00 2.00 3.00 2.00"), "a yaw and a scale\n" + environment.all());
    check(game.called("entity props 2 World/Props/Gate_Arch 0.00 0.00 0.00 0.00"), "its own axes");
    check(game.called("entity props 3 World/Props/Gate_Arch 7.00 8.00 9.00 0.00 hidden"), "a matrix, hidden");
    check(game.called("entity props 4 Props/Camcorder 0.00 0.10 0.00 0.00 on 0 49") &&
              game.called("entity props 5 Props/Camcorder 0.00 0.00 0.00 0.00 on 76561198000000001 103"),
          "on your right hand, on another player's head\n" + environment.all());
    host.apply({});
    check(game.called("entities clear props"), "A plugin's objects go with it");
    // Without the permission, refused.
    host.apply({package("sneaky", {{manifest_file, manifest()}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.entities.set, 1, "x", {position = vector.create(0, 0, 0)})))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false"), "world.entities is needed\n" + environment.all());
}

// What a prop hunt needs: the props this map has, the one a ray meets (to become it), wearing it
// upright on another player, and your own skater hidden on your screen.
void plugins_have_what_a_prop_hunt_needs() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        local props = reskate.entities.blueprints("World/Props/")
        assert(#props == 2 and props[1] == "World/Props/Bench_01", "the props loaded, by their names' start")
        assert(#reskate.entities.blueprints() == 3, "all of them")
        local shown = reskate.entities.blueprints("", true)
        assert(#shown == 1 and shown[1] == "World/Props/TrashCan_01", "only those with a mesh")
        local low, high = reskate.entities.bounds("World/Props/TrashCan_01")
        assert(low.y == 0 and math.abs(high.y - 0.9) < 1e-5, "its box")
        assert(reskate.entities.bounds("World/Vehicles/Car_01") == nil, "none without a mesh")
        local hit = reskate.world.pick(vector.create(0, 2, 0), vector.create(0, -2, 0))
        assert(hit and hit.position.z == 2 and hit.object.blueprint == "World/Props/TrashCan_01", "the object a ray meets")
        assert(math.abs(hit.object.heading - math.pi / 2) < 1e-4 and hit.object.position.z == 3, "where it is and how it is turned")
        assert(#hit.object.transform.matrix == 16)
        assert(reskate.world.pick(vector.create(0, 60, 0), vector.create(0, 70, 0)) == nil, "nothing met")
        assert(reskate.entities.set(1, hit.object.blueprint, hit.object.transform, false,
            {player = "76561198000000001", joint = "root", upright = true}), "worn upright, at the feet")
        assert(reskate.entities.set(2, hit.object.blueprint, {position = vector.create(0, 0, 0), yaw = 45}, false,
            {joint = "root", turn = false}), "turned as the mod says, whatever the skater does")
        assert(reskate.entities.set(3, hit.object.blueprint, {position = vector.create(0, 0, 0)}, false,
            {joint = "hips", turn = false, ground = 1.5}, {solid = false}), "on the floor, not solid")
        assert(not pcall(reskate.entities.set, 4, hit.object.blueprint, {position = vector.create(0, 0, 0)}, false, {joint = "hips", ground = 80}),
            "the floor within 50 m")
        assert(reskate.skater.hide(true))
        reskate.log("disguised")
    )";
    host.apply({package("prophunt", {{manifest_file, manifest(R"("world.entities", "world.raycast", "skater.control")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    running(host, "prophunt", environment);
    check(environment.logged("[prophunt] disguised"), "A prop hunt finds, becomes and wears a prop\n" + environment.all());
    check(game.called("entity prophunt 1 World/Props/TrashCan_01 1.00 0.00 3.00 1.00 on 76561198000000001 1 upright"),
          "the prop on the hider's hips, upright\n" + environment.all());
    check(game.called("hide skater prophunt on"), "your own skater hidden");
    check(game.called("entity prophunt 2 World/Props/TrashCan_01 0.00 0.00 0.00 0.71 on 0 1 held"), "a prop the skater does not turn\n" + environment.all());
    check(game.called("entity prophunt 3 World/Props/TrashCan_01 0.00 0.00 0.00 0.00 on 0 7 held ground 1.5 ghost"), "on the floor and not solid\n" + environment.all());
}

// A viewmodel (a shooter's arms and gun): loaded from the package, shown with a placement, its clips
// played, a bone read back; one plugin's at a time, given up when it stops, and only with the permission.
void plugins_show_a_viewmodel() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        assert(reskate.viewmodel.info() == nil, "nothing loaded yet")
        assert(not reskate.viewmodel.show({}), "nothing to show")
        assert(not pcall(reskate.viewmodel.load, "plugin/main.lua"), "a viewmodel is a .rsvm file")
        assert(not pcall(reskate.viewmodel.load, "../other/gun.rsvm"), "inside the package")
        assert(not pcall(reskate.viewmodel.load, "missing.rsvm"), "a file that is there")
        assert(reskate.viewmodel.load("gun/pistol.rsvm"))
        local info = reskate.viewmodel.info()
        assert(info.status == "ready" and #info.clips == 2 and info.clips[1].name == "FPS_Pistol_Fire" and info.clips[1].duration == 0.25, "its clips")
        assert(info.bones[2] == "Rif_end", "its bones")
        assert(reskate.viewmodel.show({offset = vector.create(0.02, -0.01, 0.03), pitch = 4, roll = -2, fov = 55, ambient = 0.8, brightness = 1.2}))
        assert(not pcall(reskate.viewmodel.show, {fov = 170}), "a lens of 20 to 120 degrees")
        assert(not pcall(reskate.viewmodel.show, {offset = vector.create(0, 0, 9)}), "within 5 m of the eye")
        assert(not pcall(reskate.viewmodel.show, {sky = 2}), "colours are vectors")
        assert(reskate.viewmodel.play("FPS_Pistol_Fire", {speed = 1.5, fade = 0.05}))
        assert(reskate.viewmodel.play("FPS_Pistol_Idle", {loop = true}))
        local ok, why = reskate.viewmodel.play("Dance")
        assert(not ok and why:find("no clip"), "a clip it does not have")
        assert(not pcall(reskate.viewmodel.play, "FPS_Pistol_Fire", {speed = 20}), "speed 0 to 10")
        local s = reskate.viewmodel.state()
        assert(s.clip == "FPS_Pistol_Fire" and s.duration == 0.25 and not s.done, "what plays")
        local muzzle = reskate.viewmodel.bone("Rif_end")
        assert(muzzle and math.abs(muzzle.position.z - 0.4) < 1e-6 and muzzle.z.z == 1, "a bone in the view")
        assert(reskate.viewmodel.bone("Nope") == nil)
        reskate.viewmodel.stop()
        assert(reskate.viewmodel.hide())
        reskate.log("armed")
    )";
    host.apply({package("shooter", {{manifest_file, manifest(R"("viewmodel")")}, {"plugin/main.lua", main}, {"gun/pistol.rsvm", "RSVM"}})});
    ticks(host, environment, 1);
    running(host, "shooter", environment);
    check(environment.logged("[shooter] armed"), "A viewmodel's calls work\n" + environment.all());
    check(game.called("viewmodel load shooter pistol.rsvm") && game.called("viewmodel show shooter 0.02 -0.01 0.03 0 4 -2 55 0.8 1.2") &&
              game.called("viewmodel play shooter FPS_Pistol_Fire false 1.5 0.05") && game.called("viewmodel play shooter FPS_Pistol_Idle true 1.0 0.10") &&
              game.called("viewmodel stop shooter") && game.called("viewmodel hide shooter"),
          "Loaded, shown where asked, played, stopped and hidden\n" + environment.all());
    // Another plugin cannot take it while the first has it.
    host.apply({package("shooter", {{manifest_file, manifest(R"("viewmodel")")}, {"plugin/main.lua", main}, {"gun/pistol.rsvm", "RSVM"}}),
                package("rival", {{manifest_file, manifest(R"("viewmodel")")}, {"plugin/main.lua", R"(
        local ok, why = reskate.viewmodel.load("gun.rsvm")
        reskate.log(tostring(ok) .. " " .. tostring(why))
    )"}, {"gun.rsvm", "RSVM"}})});
    ticks(host, environment, 1);
    check(environment.logged("[rival] false shooter has the viewmodel"), "One plugin's viewmodel at a time\n" + environment.all());
    // Without the permission, none of it.
    host.apply({package("sneaky", {{manifest_file, manifest()}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.viewmodel.load, "gun.rsvm")) .. " " .. tostring(pcall(reskate.viewmodel.show, {})) .. " " ..
            tostring(pcall(reskate.viewmodel.play, "FPS_Pistol_Fire")) .. " " .. tostring(pcall(reskate.viewmodel.bone, "Rif_end")) .. " " ..
            tostring(pcall(reskate.viewmodel.info)))
    )"}, {"gun.rsvm", "RSVM"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false false false false false"), "viewmodel is needed\n" + environment.all());
    check(game.viewmodeler.empty(), "Given up when its plugin stops\n" + environment.all());
}

// A flip mode's speed: flip tricks in the air turned slower or faster for a moment, 1 or nothing to let go.
void plugins_set_the_flip_speed() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    host.apply({package("flipper", {{manifest_file, manifest(R"("skater.control")")}, {"plugin/main.lua", R"(
        assert(reskate.skater.flip_speed(0.3) and reskate.skater.flip_speed(2) and reskate.skater.flip_speed() and reskate.skater.flip_speed(1))
        assert(reskate.skater.fly(vector.create(3, -4.5, 9)) and reskate.skater.fly(), "the held skater flown, and let go of")
        assert(not pcall(reskate.skater.fly, vector.create(0, -151, 0)), "150 m/s an axis")
        assert(reskate.skater.held() == true, "the skater is held")
        assert(not pcall(reskate.skater.fly, vector.create(0 / 0, 0, 0)), "a number")
        assert(not pcall(reskate.skater.flip_speed, 0.01) and not pcall(reskate.skater.flip_speed, 11), "0.05 to 10")
        assert(not pcall(reskate.skater.flip_speed, 0 / 0), "a number")
        reskate.log("flipped")
    )"}})});
    ticks(host, environment, 1);
    running(host, "flipper", environment);
    check(environment.logged("[flipper] flipped"), "flip_speed takes 0.05 to 10\n" + environment.all());
    check(game.called("flip speed flipper 0.30") && game.called("flip speed flipper 2.00") && game.called("flip speed flipper 1.00"),
          "A slower flip, a faster one, and let go\n" + environment.all());
    check(game.called("fly flipper 3.0 -4.5 9.0") && game.called("fly flipper none"), "The held skater flown, and let go of\n" + environment.all());
    // Without skater.control, not.
    host.apply({package("sneaky", {{manifest_file, manifest()}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.skater.flip_speed, 0.5)))
        reskate.log("fly " .. tostring(pcall(reskate.skater.fly, vector.create(0, 1, 0))))
        reskate.log("held " .. tostring(pcall(reskate.skater.held)))
        reskate.log("skydive " .. tostring(pcall(reskate.skater.skydive)))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false"), "skater.control is needed\n" + environment.all());
    check(!game.called("flip speed sneaky 0.50"), "Nothing reached the game");
    check(environment.logged("[sneaky] held false"), "Asking whether the skater is held needs skater.control\n" + environment.all());
    check(environment.logged("[sneaky] fly false") && !game.called("fly sneaky 0.0 1.0 0.0"), "Flying the skater needs skater.control too\n" + environment.all());
    check(environment.logged("[sneaky] skydive false") && !game.called("skydive"), "And so does the game's skydive\n" + environment.all());
}

// What a camera mode needs (the Filmer): an arm that reaches, where the hand is, cruise control, the
// mouse, and the right stick and stick clicks kept from the game.

void plugins_write_normalized_controllers() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("writer", R"(
        local p = {left_x = -1, left_y = 0.25, right_x = 1, right_y = -0.5,
            left_trigger = 0.3, right_trigger = 1, buttons = {a = true, rb = true, b = false}}
        assert(reskate.input.write(p, "playstation"))
        assert(not pcall(reskate.input.write, {left_x = 1.1}))
        assert(not pcall(reskate.input.write, {right_trigger = -0.1}))
        assert(not pcall(reskate.input.write, {left_y = 0/0}))
        assert(not pcall(reskate.input.write, {right_x = math.huge}))
        assert(not pcall(reskate.input.write, {left_x = "1"}))
        assert(not pcall(reskate.input.write, {buttons = {a = 1}}))
        assert(not pcall(reskate.input.write, p, 0.5))
        assert(not pcall(reskate.input.write, p, 4))
        assert(not pcall(reskate.input.write, p, "all"))
    )", R"("input.write")")});
    running(host, "writer", environment);
    const auto pad = environment.fake.written_pad;
    check(pad && pad->left_x == -1 && pad->right_y == -0.5f && pad->buttons == 0x1200 &&
        environment.fake.written_controller == 4, "normalized pad reaches the selected controller");
    host.apply({script("reader", R"(
        local ok, why = pcall(reskate.input.write, {})
        assert(not ok and why:find('input.write'), "reading input does not grant writing")
    )", R"("input")")});
    running(host, "reader", environment);
    host.apply({script("release", "assert(reskate.input.write(nil, 2))", R"("input.write")")});
    running(host, "release", environment);
    check(!environment.fake.written_pad && environment.fake.written_controller == 2, "nil releases the caller's output");

    using namespace dingosdk::input;
    PadOutputs outputs;
    PadOutput value{}; value.right_y = -20000; value.buttons = 0x1000;
    check(outputs.write("one", 0, value, 1000).empty(), "first writer owns slot");
    check(!outputs.write("two", 0, value, 1001).empty(), "another writer cannot steal a live slot");
    check(outputs.write("two", 1, value, 1001).empty(), "slots are independent");
    outputs.write("two", 0, std::nullopt, 1002);
    check(outputs.read(0, 1099, true)->right_y == -20000, "another writer cannot release it");
    check(!outputs.read(0, 1100, true), "expires exactly at 100 ms");
    check(outputs.write("two", 0, value, 1101).empty(), "an expired slot can be claimed");
    check(!outputs.read(0, 1102, false), "UI ownership discards output");
    check(!outputs.read(0, 1103, true) && !outputs.read(1, 1103, true), "no stale output after UI closes");
    outputs.write("one", 4, value, 1200); outputs.release("one");
    check(!outputs.read(4, 1201, true), "stop releases native pads too");
}

void controller_reports_preserve_non_controls() {
    using namespace dingosdk;
    for (int variant = 0; variant < 5; ++variant) {
        const auto kind = variant < 2 ? PlayStationPad::dualsense : PlayStationPad::dualshock4;
        const bool bluetooth = variant == 1 || variant == 3 || variant == 4;
        std::array<std::uint8_t,78> bytes{}; bytes.fill(0x5a);
        bytes[0] = variant == 1 ? 0x31 : variant == 3 ? 0x11 : 0x01;
        const auto before = bytes;
        PlayStationGamepad pad{0xf3f9, 200, 10, -32768, 32767, 16000, -12000};
        check(replace_playstation_report(kind, bluetooth, bytes.data(), bytes.size(), pad), "a valid report is replaced");
        const auto after = parse_playstation_report(kind, bluetooth, bytes.data(), bytes.size());
        check(after && after->buttons == pad.buttons && after->left_trigger == 200 && after->right_trigger == 10 &&
            after->left_x == -32768 && std::abs(static_cast<int>(after->left_y) - 32767) <= 1 &&
            std::abs(static_cast<int>(after->right_x) - 16000) <= 129 &&
            std::abs(static_cast<int>(after->right_y) + 12000) <= 129, "all controls round trip within HID precision");
        const unsigned sticks = variant == 1 ? 2 : variant == 3 ? 3 : 1;
        const bool sense = variant < 2;
        const unsigned face = sense ? sticks + 7 : sticks + 4;
        const unsigned triggers = sense ? sticks + 4 : sticks + 7;
        for (unsigned i = 0; i < bytes.size(); ++i) {
            const bool control = (i >= sticks && i < sticks + 4) || i == face || i == face + 1 || i == triggers || i == triggers + 1;
            if (!control) check(bytes[i] == before[i], "system, sequence, touch and sensor bytes stay untouched");
        }
        check(!replace_playstation_report(kind, bluetooth, bytes.data(), bytes.size(), *after), "unchanged controls do not rewrite a report");
        pad.left_trigger = 1; pad.right_trigger = 0;
        replace_playstation_report(kind, bluetooth, bytes.data(), bytes.size(), pad);
        check((bytes[face + 1] & 0x0c) == 0, "low triggers clear their digital bits");
    }
    // The current host reads padded DS4 Bluetooth reports and can apply a
    // menu-preview hold after a plugin replacement. Keep both paths composable.
    std::array<std::uint8_t, dualshock4_bluetooth_read_length> padded{}; padded.fill(0x5a); padded[0] = 0x11;
    const auto padding_before = padded;
    const PlayStationGamepad replacement{0x1000, 200, 0, -32768, 16000, 12000, -8000};
    check(replace_playstation_report(PlayStationPad::dualshock4, true, padded.data(), padded.size(), replacement) &&
          refresh_playstation_bluetooth_crc(padded.data(), padded.size()), "replacement reaches a padded DS4 Bluetooth report with a fresh CRC");
    for (std::size_t i = playstation_bluetooth_report_length; i < padded.size(); ++i)
        check(padded[i] == padding_before[i], "Windows report padding is retained");
    check(set_playstation_stick(PlayStationPad::dualshock4, true, padded.data(), padded.size(), 0, 16000),
          "upstream menu preview can override its one stick after a replacement");
    const auto combined = parse_playstation_report(PlayStationPad::dualshock4, true, padded.data(), padded.size());
    check(combined && combined->buttons == replacement.buttons && combined->right_trigger == 0 &&
          std::abs(static_cast<int>(combined->left_x) - 16000) <= 258 &&
          std::abs(static_cast<int>(combined->right_x) - replacement.right_x) <= 129,
          "menu preview preserves the replacement's other controls");
    std::array<std::uint8_t,2> short_report{0x31, 0}; const auto before = short_report;
    check(!replace_playstation_report(PlayStationPad::dualsense, true, short_report.data(), short_report.size(), {}) && short_report == before,
        "truncated reports stay untouched");
}

#ifdef _WIN32
void animation_values_require_occupied_typed_local_pages() {
    namespace av = dingosdk::animation_values;
    namespace layout = av::layout;
    alignas(8) std::array<std::uint8_t, 0x700> core{};
    alignas(8) std::array<std::uint8_t, 0x80> provider{}, model{};
    alignas(8) std::array<std::uint8_t, 0x500> graph{};
    alignas(8) std::array<std::uint8_t, 0x120> node{};
    alignas(8) std::array<std::uint8_t, 0x740> page{};
    const auto address = [](const auto& v) { return reinterpret_cast<std::uintptr_t>(v.data()); };
    const auto put = [](auto& bytes, std::size_t offset, auto value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
    const std::uintptr_t base = 0x140000000;
    put(core, layout::provider, address(provider)); put(core, layout::model, address(model));
    put(provider, layout::output, address(graph)); put(graph, 0, address(node));
    put(node, layout::type, base + layout::float_type); put(node, layout::key, std::uint32_t{0x04004000});
    const std::string name = "Animation/Dingo/Float.Physics.Example";
    std::memcpy(node.data() + layout::float_name, name.c_str(), name.size() + 1);
    put(model, layout::pages, address(page)); put(page, 4, 0.5f);
    check(av::read(base, address(core)).empty(), "descriptor defaults are never read");
    put(page, layout::occupied, std::uint32_t{1 << 4});
    auto values = av::read(base, address(core));
    check(values.size() == 1 && values.at(name).number == 0.5 && av::writable(name, values.at(name)), "occupied typed physics output is available");
    put(provider, layout::output, std::uintptr_t{}); put(provider, layout::input, address(graph));
    values = av::read(base, address(core));
    check(values.size() == 1 && !av::writable(name, values.at(name)), "animation inputs are read-only");
    put(page, 4, std::numeric_limits<float>::infinity());
    check(av::read(base, address(core)).empty(), "non-finite native values are refused");
    put(page, 4, 0.5f); put(node, layout::type, std::uintptr_t{});
    check(av::read(base, address(core)).empty(), "wrong descriptor types are refused");
}
#endif

void plugins_read_flip_state() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("missing_flip", R"(
        assert(reskate.skater.flip_state() == nil, "no local state is not a false underflip")
    )", R"("skater.read")")});
    running(host, "missing_flip", environment);

    environment.fake.flip_state_result = Game::FlipState{2, false, true, std::nullopt};
    host.apply({script("known_flip", R"(
        local f = reskate.skater.flip_state()
        assert(f.trick == 2 and f.underflip_anim == false and f.underflip_state == true)
        assert(f.underflip_window == nil, "an unavailable flag is not false")
        local count = 0
        for key in pairs(f) do
            assert(key == "trick" or key == "underflip_anim" or key == "underflip_state")
            count = count + 1
        end
        assert(count == 3, "only public values, never native addresses")
    )", R"("skater.read")")});
    running(host, "known_flip", environment);

    environment.fake.flip_state_result = Game::FlipState{0, true, false, false};
    host.apply({script("idle_flip", R"(
        local f = reskate.skater.flip_state()
        assert(f.trick == 0 and f.underflip_anim == true)
        assert(f.underflip_state == false and f.underflip_window == false)
    )", R"("skater.read")")});
    running(host, "idle_flip", environment);

    environment.fake.flip_state_result = Game::FlipState{2, std::nullopt, std::nullopt, std::nullopt};
    host.apply({script("unknown_flags", R"(
        local f = reskate.skater.flip_state()
        assert(f.trick == 2 and f.underflip_anim == nil and f.underflip_state == nil and f.underflip_window == nil)
    )", R"("skater.read")")});
    running(host, "unknown_flags", environment);

    const auto reads = environment.fake.flip_state_reads;
    host.apply({script("denied_flip", R"(
        assert(not pcall(reskate.skater.flip_state), "skater.read is required")
    )")});
    running(host, "denied_flip", environment);
    check(environment.fake.flip_state_reads == reads, "permission denial happens before reading native state");
}

void plugins_read_frames_and_hold_gameplay_options() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("observations", R"(
        assert(reskate.settings.gameplay("example") == true)
        assert(reskate.settings.gameplay("absent") == nil)
        assert(reskate.settings.hold_gameplay("example", false))
        assert(reskate.settings.hold_gameplay("example", nil))
        assert(not reskate.settings.hold_gameplay("absent", true))
        assert(not pcall(reskate.settings.hold_gameplay, "example", 1))
        assert(not pcall(reskate.settings.gameplay, ""))
        assert(reskate.skater.rail_target().targeting)
        assert(reskate.skater.select_rail_target(2, true))
        assert(reskate.skater.select_rail_target(nil))
        assert(not pcall(reskate.skater.select_rail_target, -1))
        assert(not pcall(reskate.skater.select_rail_target, 7))
        assert(not pcall(reskate.skater.select_rail_target, 1.5))
        assert(not pcall(reskate.skater.select_rail_target, 0/0))
        assert(not pcall(reskate.skater.select_rail_target, 2, 1))
        local f = reskate.skater.frame()
        assert(f.serial == 42 and f.mirrored and not f.fakie and f.forward.z == 1)
        local a = reskate.skater.animation_values()
        assert(a["Animation/Dingo/Bool.Anim.DoingTrick"].value == true)
        assert(a["Animation/Dingo/Float.Physics.Example"].output)
        assert(reskate.skater.set_animation_value("Animation/Dingo/Float.Physics.Example", 1.6))
        assert(not reskate.skater.set_animation_value("unknown", 1))
        assert(not pcall(reskate.skater.set_animation_value, "example", 0/0))
        assert(not pcall(reskate.skater.set_animation_value, "example", math.huge))
        assert(not pcall(reskate.skater.set_animation_value, "example", "1"))
        reskate.input.gamepad(2)
        assert(not pcall(reskate.input.gamepad, 4))
        assert(not pcall(reskate.input.gamepad, 0.5))
    )", R"("settings.read", "settings.write", "skater.read", "skater.animation", "skater.control", "input")")});
    running(host, "observations", environment);
    check(environment.fake.read_controller == 2, "physical pad selection reaches the game");
    host.apply({script("denied", R"(
        assert(not pcall(reskate.settings.gameplay, "example"))
        assert(not pcall(reskate.settings.hold_gameplay, "example", false))
        assert(not pcall(reskate.skater.rail_target))
        assert(not pcall(reskate.skater.select_rail_target, 2))
        assert(not pcall(reskate.skater.frame))
        assert(not pcall(reskate.skater.animation_values))
        assert(not pcall(reskate.skater.set_animation_value, "example", 1))
    )")});
    running(host, "denied", environment);
    dingosdk::input::GameplayOptions options;
    check(options.observe("example", true, 1000, false), "observe the player's preference");
    check(options.hold("one", "example", false, 1001).empty(), "hold an observed option");
    check(!options.observe("example", true, 1002, false), "hold affects gameplay");
    check(options.observe("example", true, 1002, true), "settings UI keeps the player's preference");
    check(options.get("example") == true, "reading returns the saved preference");
    check(!options.hold("two", "example", false, 1003).empty(), "another owner cannot steal the option");
    options.hold("two", "example", std::nullopt, 1004);
    check(!options.observe("example", true, 1005, false), "another owner cannot release it");
    check(options.observe("example", true, 1301, false), "holds expire at 300 ms");
    check(!options.hold("one", "unknown", true, 1400).empty(), "unknown options cannot be created");
    options.hold("one", "example", false, 1400); options.release("one");
    check(options.observe("example", true, 1401, false), "stop releases preferences");
}

// A compiled shader container as a compiler writes one: "DXBC", a hash (zero: not signed), its size, then one part of
// shader code whose first word is the program's kind and version (pixel shaders are kind 0).
std::string shader_container(std::string_view code, std::uint32_t version, bool signed_hash = true) {
    std::string bytes = "DXBC";
    const auto word = [&](std::uint32_t value) { bytes.append(reinterpret_cast<const char*>(&value), 4); };
    for (int i = 0; i < 4; ++i) word(signed_hash ? 0x1234567u + static_cast<std::uint32_t>(i) : 0);
    word(1);  // the container's version
    word(0);  // its size, below
    word(1);  // one part
    word(36); // at 36
    bytes += code;
    word(8);  // the part's size: its program header
    word(version);
    word(0);
    const auto size = static_cast<std::uint32_t>(bytes.size());
    std::memcpy(bytes.data() + 24, &size, 4);
    return bytes;
}

void screen_shader_files_are_checked() {
    using namespace dingosdk::screen_shader;
    Kind kind{};
    check(check_file("shaders/look.hlsl", "float4 main() : SV_Target { return 1; }", kind).empty() && kind == Kind::hlsl, "HLSL source is taken");
    check(check_file("shaders/LOOK.HLSL", "x", kind).empty(), "an extension in capitals too");
    check(!check_file("shaders/look.hlsl", "", kind).empty(), "not an empty one");
    check(!check_file("shaders/look.hlsl", std::string("a\0b", 3), kind).empty(), "source is text");
    check(!check_file("shaders/look.hlsl", std::string(most_source_bytes + 1, 'x'), kind).empty(), "at most 256 KB of source");
    check(!check_file("shaders/look.txt", "x", kind).empty(), "only .hlsl, .cso, .dxbc or .dxil");
    check(check_file("shaders/look.dxil", shader_container("DXIL", 0x00000060), kind).empty() && kind == Kind::dxil, "signed DXIL, a pixel shader");
    check(check_file("shaders/look.cso", shader_container("SHEX", 0x00000050), kind).empty() && kind == Kind::dxbc, "DXBC, a pixel shader");
    check(check_file("shaders/look.dxil", shader_container("DXIL", 0x00010060), kind).find("not a pixel shader") != std::string::npos,
          "a vertex shader is refused");
    check(check_file("shaders/look.dxil", shader_container("DXIL", 0x00000060, false), kind).find("not signed") != std::string::npos,
          "unsigned DXIL is refused");
    check(!check_file("shaders/look.cso", shader_container("RDEF", 0x00000050), kind).empty(), "a container with no shader code is refused");
    auto short_one = shader_container("DXIL", 0x00000060);
    short_one.pop_back();
    check(!check_file("shaders/look.dxil", short_one, kind).empty(), "a container whose size is not the file's is refused");
    check(!check_file("shaders/look.dxil", "not a shader at all, but long enough to be read", kind).empty(), "not a container");
    auto past = shader_container("DXIL", 0x00000060);
    const std::uint32_t beyond = 4096; // (not "far": a Windows macro)
    std::memcpy(past.data() + 32, &beyond, 4);
    check(!check_file("shaders/look.dxil", past, kind).empty(), "a part past the file's end is refused");
}

// reskate.screen.shader: the plugin's file read from its package and checked, its values given, nil ending it; refused
// without "screen.shader".
void plugins_run_a_screen_shader() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::map<std::string, std::string> files{
        {manifest_file, manifest(R"("screen.shader")")},
        {"shaders/tint.hlsl", "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return reskate_frame.Sample(reskate_linear, uv) * reskate_values[0]; }"},
        {"shaders/slow.hlsl", "float4 main() : SV_Target { return 0; }"},
        {"shaders/tint.dxil", shader_container("DXIL", 0x00000060)},
        {"shaders/vertex.dxil", shader_container("DXIL", 0x00010060)},
        {"shaders/notes.txt", "not a shader"},
        {"plugin/main.lua", R"(
        local values = {}
        for i = 1, 48 do values[i] = i / 4 end
        assert(reskate.screen.shader("shaders/tint.hlsl", values))
        assert(reskate.screen.shader("shaders/tint.dxil", {0.5}))
        assert(reskate.screen.shader("shaders/tint.hlsl"))
        assert(reskate.screen.shader(nil))
        local ok, why = reskate.screen.shader("shaders/slow.hlsl")
        assert(ok == false and why:find("on the GPU"), tostring(why))
        local too_many = {}
        for i = 1, 49 do too_many[i] = 0 end
        assert(not pcall(reskate.screen.shader, "shaders/tint.hlsl", too_many), "at most 48 values")
        assert(not pcall(reskate.screen.shader, "shaders/tint.hlsl", {"half"}), "values are numbers")
        assert(not pcall(reskate.screen.shader, "shaders/tint.hlsl", {0 / 0}), "values are finite")
        assert(not pcall(reskate.screen.shader, "shaders/vertex.dxil"), "a pixel shader")
        assert(not pcall(reskate.screen.shader, "shaders/notes.txt"), "a shader file")
        assert(not pcall(reskate.screen.shader, "shaders/missing.hlsl"), "a file in the package")
        assert(not pcall(reskate.screen.shader, "../outside.hlsl"), "inside the package")
        reskate.log("shaded")
    )"}};
    host.apply({package("shader", files)});
    ticks(host, environment, 1);
    running(host, "shader", environment);
    check(environment.logged("[shader] shaded"), "A screen shader's calls work\n" + environment.all());
    const auto hlsl_size = files.at("shaders/tint.hlsl").size(), dxil_size = files.at("shaders/tint.dxil").size();
    check(game.called(std::format("shader shader shaders/tint.hlsl 0 {} 0.25 0.50 12.00", hlsl_size)),
          "HLSL reaches the game with its 48 values\n" + environment.all());
    check(game.called(std::format("shader shader shaders/tint.dxil 2 {} 0.50 0.00 0.00", dxil_size)),
          "DXIL reaches the game as DXIL, values not given are 0\n" + environment.all());
    check(game.called("shader shader none"), "nil ends it");
    check(!game.called("shaders/vertex.dxil") && !game.called("shaders/notes.txt"), "files that are not screen shaders never reach the game");
    // It needs its permission: "overlay" is not enough.
    host.apply({package("sneaky", {{manifest_file, manifest(R"("overlay")")}, {"shaders/tint.hlsl", files.at("shaders/tint.hlsl")}, {"plugin/main.lua", R"(
        reskate.log("shader " .. tostring(pcall(reskate.screen.shader, "shaders/tint.hlsl")) .. " " .. tostring(pcall(reskate.screen.shader, nil)))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] shader false false"), "screen.shader is needed\n" + environment.all());
    check(!game.called("shader sneaky"), "Nothing of a plugin without it reaches the game");
}

// reskate.world.shader: the plugin's file checked as a screen shader's is, its values and heights, its data as a buffer
// of float4s, its anchor and its cost; refused without "world.shader" ("screen.shader" is not enough).
void plugins_run_a_world_shader() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::map<std::string, std::string> files{
        {manifest_file, manifest(R"("world.shader")")},
        {"shaders/tint.hlsl", "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return reskate_picture.Sample(reskate_point, uv); }"},
        {"shaders/tint.dxil", shader_container("DXIL", 0x00000060)},
        {"shaders/vertex.dxil", shader_container("DXIL", 0x00010060)},
        {"plugin/main.lua", R"(
        local values = {}
        for i = 1, 48 do values[i] = i / 2 end
        assert(reskate.world.shader("shaders/tint.hlsl", {values = values, heights = true}))
        assert(reskate.world.shader("shaders/tint.dxil"))
        local data = buffer.create(32)
        buffer.writef32(data, 0, 1.5)
        assert(reskate.world.shader_data(data))
        assert(reskate.world.shader_data(nil))
        reskate.world.shader_anchor(vector.create(1, 2, 3))
        reskate.world.shader_anchor(nil)
        assert(reskate.world.shader_cost() == 4.5)
        assert(reskate.world.shader_clock() == 123.25)
        assert(not pcall(reskate.world.shader_data, buffer.create(20)), "whole float4s")
        assert(not pcall(reskate.world.shader_data, buffer.create(65536 + 16)), "at most 64 KB")
        assert(not pcall(reskate.world.shader_data, "not a buffer"), "a buffer")
        assert(not pcall(reskate.world.shader, "shaders/tint.hlsl", {heights = "yes"}), "heights is true or false")
        assert(not pcall(reskate.world.shader, "shaders/tint.hlsl", {values = {"a"}}), "values are numbers")
        assert(not pcall(reskate.world.shader, "shaders/vertex.dxil"), "a pixel shader")
        assert(not pcall(reskate.world.shader_anchor, "here"), "an anchor is a position")
        assert(reskate.world.shader(nil))
        reskate.log("snowed")
    )"}};
    host.apply({package("world", files)});
    ticks(host, environment, 1);
    running(host, "world", environment);
    check(environment.logged("[world] snowed"), "A world shader's calls work\n" + environment.all());
    check(game.called(std::format("world world shaders/tint.hlsl 0 {} true 0.50 24.00", files.at("shaders/tint.hlsl").size())),
          "HLSL reaches the game with its values and the heights asked for\n" + environment.all());
    check(game.called(std::format("world world shaders/tint.dxil 2 {} false 0.00 0.00", files.at("shaders/tint.dxil").size())),
          "DXIL reaches the game as DXIL, with no heights unless asked\n" + environment.all());
    check(game.called("world data world 32 1.50") && game.called("world data world none"), "Its data as float4s, and none");
    check(game.called("world anchor world 1.0 2.0 3.0") && game.called("world anchor world none"), "Its anchor, and none");
    check(game.called("world world none"), "nil ends it");
    check(!game.called("world data world 20") && !game.called("shaders/vertex.dxil"), "Data and files that are not right never reach the game");
    host.apply({package("sneaky", {{manifest_file, manifest(R"("screen.shader")")}, {"shaders/tint.hlsl", files.at("shaders/tint.hlsl")}, {"plugin/main.lua", R"(
        reskate.log("world " .. tostring(pcall(reskate.world.shader, "shaders/tint.hlsl")) .. " " .. tostring(pcall(reskate.world.shader_data, nil)) .. " " ..
            tostring(pcall(reskate.world.shader_anchor, nil)) .. " " .. tostring(pcall(reskate.world.shader_cost)) .. " " ..
            tostring(pcall(reskate.world.shader_clock)))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] world false false false false false"), "world.shader is needed\n" + environment.all());
    check(!game.called("world sneaky") && !game.called("world data sneaky") && !game.called("world anchor sneaky"),
          "Nothing of a plugin without it reaches the game");
}

void plugins_reach_cruise_and_read_the_mouse() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        assert(reskate.skater.reach({at = vector.create(1, 2, 3)}))
        assert(reskate.skater.reach({hand = "left", at = vector.create(4, 5, 6), rotation = {0, 0, 0, 2}, weight = 0.5}))
        assert(reskate.skater.reach(nil))
        assert(not pcall(reskate.skater.reach, {hand = "foot", at = vector.create(0, 0, 0)}), "a hand is right or left")
        assert(not pcall(reskate.skater.reach, {at = vector.create(0, 0, 0), weight = 2}), "a weight is 0 to 1")
        assert(not pcall(reskate.skater.reach, {}), "a reach needs a point")
        local h = reskate.skater.hand()
        assert(h and h.position.x == 1 and h.position.z == 3 and h.rotation[4] == 1, "the right hand")
        assert(reskate.skater.hand("left") == nil and not pcall(reskate.skater.hand, "foot"))
        assert(reskate.skater.cruise(8, true) and reskate.skater.cruise(0))
        assert(reskate.skater.set_velocity(vector.create(1, 5, -2)) and reskate.skater.set_velocity(vector.create(0, 3, 0), true))
        assert(not pcall(reskate.skater.set_velocity, vector.create(0, 101, 0)), "100 m/s an axis")
        assert(not pcall(reskate.skater.cruise, 0.5) and not pcall(reskate.skater.cruise, 31), "1 to 30 m/s")
        local m = reskate.input.mouse()
        -- A digit's name is its key, not a key code: "1" is the 1 key, 1 the left mouse button.
        assert(reskate.input.key("1") == true and reskate.input.key(1) == false and reskate.input.key(0x31) == true and reskate.input.key("A") == false)
        -- (Held: the pointer kept in the window while the plugin reads the mouse.)
        assert(reskate.input.mouse({hold = true}) and not pcall(reskate.input.mouse, {hold = 1}), "hold is true or false")
        assert(m and m.x == 3 and m.y == -2 and m.wheel == 1, "the mouse")
        assert(reskate.input.hide({right_stick = true, right_stick_click = true}))
        -- Buttons kept from the game by name; Start never is.
        assert(reskate.input.hide({buttons = {"y", "b"}}))
        assert(not pcall(reskate.input.hide, {buttons = {"start"}}), "Start always reaches the game")
        assert(not pcall(reskate.input.hide, {buttons = "y"}), "buttons is a list")
        assert(reskate.input.hide({right_stick = true, right_stick_click = true}))
        -- Keys kept from the game: the mouse's buttons are keys too, Escape never is.
        reskate.input.game_keys({"R", "LeftMouse", "RightMouse", "MiddleMouse"})
        assert(not pcall(reskate.input.game_keys, {"R", "Escape"}), "Escape always reaches the game")
        reskate.input.game_keys(nil)
        assert(reskate.screen.look({bend = 0.8, fov = 110, bars = true, hood = true, grade = true, grain = true, age = 0.35, iris = true,
            rolling = 2, text = {"#rec", "0:00:02", nil, "4:05 PM", "OCT.20 2026"}}))
        assert(reskate.screen.slap() and reskate.screen.look(nil))
        assert(not pcall(reskate.screen.look, {bend = 2}), "a bend is 0 to 1")
        -- Soft towards the edges, by itself or with the rest.
        assert(reskate.screen.look({edge_blur = 0.7}) and reskate.screen.look({edge_blur = 0.5, edge_start = 0.25, hood = true}))
        assert(not pcall(reskate.screen.look, {edge_blur = 1.5}), "an edge blur is 0 to 1")
        -- The picture by its brightness alone, between two colours: whatever the plugin makes of it.
        assert(reskate.screen.look({tone = {}}) and reskate.screen.look({tone = {dark = 0x000033, bright = 0xff8000, curve = 1.4, noise = 0.3}, hood = true}))
        assert(not pcall(reskate.screen.look, {tone = {dark = "black"}}), "a colour is a number")
        assert(not pcall(reskate.screen.look, {tone = {bright = 0x1000000}}), "0xRRGGBB")
        assert(not pcall(reskate.screen.look, {tone = {curve = 9}}), "a curve is 0.2 to 5")
        assert(not pcall(reskate.screen.look, {tone = {noise = 2}}), "a noise is 0 to 1")
        assert(not pcall(reskate.screen.look, {edge_blur = 0.5, edge_start = 1}), "it starts short of the corners")
        assert(not pcall(reskate.screen.look, {text = {"far too long a line"}}), "12 characters")
        assert(not pcall(reskate.screen.look, {text = {"{}"}}), "the camcorder's characters")
        assert(not pcall(reskate.screen.look, {text = {"a", "b", "c", "d", "e", "f"}}), "5 lines")
        assert(reskate.screen.look({grade = true, grain = true, strengths = {grade = 0.6, grain = 0.25, smear = 0}}))
        assert(not pcall(reskate.screen.look, {grade = true, strengths = {grade = 1.5}}), "a strength is 0 to 1")
        assert(not pcall(reskate.screen.look, {strengths = {hood = "half"}}), "a strength is a number")
        reskate.log("filmed")
    )";
    host.apply({package("filmer", {{manifest_file, manifest(R"("skater.animation", "skater.read", "skater.control", "input", "overlay")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    running(host, "filmer", environment);
    check(environment.logged("[filmer] filmed"), "A camera mode's calls work\n" + environment.all());
    check(game.called("reach filmer right 1.0 2.0 3.0 false 1.00") && game.called("reach filmer left 4.0 5.0 6.0 true 0.50") &&
              game.called("reach filmer none"),
          "The arm reaches, either hand, and lets go\n" + environment.all());
    check(game.called("cruise filmer 8.0 true") && game.called("cruise filmer 0.0 false"), "Cruise control on and off");
    check(game.called("velocity filmer 1.0 5.0 -2.0") && game.called("velocity filmer 0.0 3.0 0.0 add"), "A velocity set, and one added");
    check(game.called("hide filmer false false false stick true false true"), "The right stick and its click kept from the game");
    check(game.called("hide filmer false false false buttons a000"), "Y and B kept from the game");
    check(game.called("look filmer 0.80 110 0.35 truetruetruefalsefalsetruefalsefalsefalsefalsetrue 2.0 [#REC|0:00:02||4:05 PM|OCT.20 2026]") &&
              game.called("slap filmer") && game.called("look filmer none"),
          "A camcorder look on the screen, its text in capitals, a slap, and off\n" + environment.all());
    check(game.called("look edge filmer 0.70 0.45") && game.called("look edge filmer 0.50 0.25"),
          "A look soft towards its edges reaches the game, from 0.45 of the way out when not said" + std::string(1, 10) + environment.all());
    check(game.called("look strengths filmer 1.00 1.00 0.60 1.00 1.00 0.25 1.00 1.00 0.00"),
          "A look's effects at part strength reach the game, 1 for those not given\n" + environment.all());
    // Each needs its permission.
    host.apply({package("sneaky", {{manifest_file, manifest()}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.skater.reach, {at = vector.create(0, 0, 0)})) .. " " .. tostring(pcall(reskate.skater.hand)) .. " " ..
            tostring(pcall(reskate.skater.cruise, 5)) .. " " .. tostring(pcall(reskate.input.mouse)) .. " " .. tostring(pcall(reskate.screen.look, {})))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false false false false false"),
          "skater.animation, skater.read, skater.control, input and overlay are needed\n" + environment.all());
}

// What the Filmer's port needs besides: something carried (seen by the others only), a pose only you
// see, a step in the camera height cycle and its event, the nametags held off, the board hidden from
// everyone, and a camcorder in the hand wear slot.
void plugins_carry_and_take_a_step_in_the_camera_cycle() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        local steps = {}
        reskate.on("camera_cycle", function(step) table.insert(steps, step) end)
        assert(reskate.skater.carry({at = vector.create(1, 2, 3), rotation = {0, 1, 0, 0}, both = true, front = vector.create(0, -0.143, -0.248),
            grip = {rotation = {0.5, -0.5, 0.5, 0.5}, offset = vector.create(0.12, 0.04, -0.03)}, crouch = 0.22, hunch = 1.0, turn = 0.5,
            step = 0.5, step_phase = 1.25, still = true, push_straightens = true, weight = 0.75, shown = "others"}))
        assert(reskate.skater.carry({hand = "left", at = vector.create(0, 1, 0), rotation = {0, 0, 0, 1}}))
        assert(reskate.skater.carry({at = vector.create(0, 1, 0), rotation = {0, 0, 0, 1}, both = true, support = vector.create(0.02, -0.05, 0.1)}))
        assert(not pcall(reskate.skater.carry, {at = vector.create(0, 1, 0), rotation = {0, 0, 0, 1}, support = vector.create(0, 0.5, 0)}),
            "a support is at most 0.3 m each way")
        assert(reskate.skater.carry({at = vector.create(0, 1, 0), rotation = {0, 0, 0, 1}, exact = true, look_at = vector.create(0, 1.2, -0.1)}))
        assert(not pcall(reskate.skater.carry, {at = vector.create(0, 1, 0), rotation = {0, 0, 0, 1}, look_at = vector.create(0, 9, 0)}),
            "a look_at is within 3 m")
        assert(reskate.skater.carry(nil))
        assert(not pcall(reskate.skater.carry, {at = vector.create(0, 1, 0)}), "a carry needs a rotation")
        assert(not pcall(reskate.skater.carry, {at = vector.create(0, 1, 0), rotation = {0, 0, 0, 1}, crouch = 1}), "a crouch is 0 to 0.6 m")
        assert(not pcall(reskate.skater.carry, {at = vector.create(0, 1, 0), rotation = {0, 0, 0, 1}, shown = "them"}), "shown is everyone, others or me")
        assert(reskate.skater.pose({[48] = {scale = 0}}, 1, {shown = "me"}))
        -- A pose for the others beside it, in the same tick: a whole body for the others, the arms for me.
        assert(reskate.skater.pose({[47] = {angles = vector.create(0, 90, 0)}}, 1, {shown = "others"}))
        assert(reskate.skater.pose_others({[276] = {angles = vector.create(0, 90, 0)}}), "the call of its own for it")
        assert(not pcall(reskate.skater.pose_others, {[1] = {scale = 0}}), "the same joints as pose takes")
        -- The game's animation held still under a pose.
        assert(reskate.skater.pose({[48] = {scale = 0}}, 1, {still = true}))
        assert(not pcall(reskate.skater.pose, {[48] = {scale = 0}}, 1, {still = "yes"}), "still is true or false")
        assert(reskate.skater.reach({at = vector.create(1, 2, 3), shown = "others"}))
        assert(reskate.camera.cycle("ready") and reskate.camera.cycle("active") and reskate.camera.cycle(nil))
        assert(not pcall(reskate.camera.cycle, "on"), "ready or active")
        reskate.session.nametags(false)
        reskate.session.nametags(true)
        assert(reskate.board.hide(2, {others = true}))
        assert(reskate.outfit.set("handwear_l", "Own_HandwearGlove_Gen_GloveFingerless_00001"))
        local ok, why = reskate.outfit.set("shoes", "Own_Shoes_00001")
        assert(not ok and why == "only tattoo, moles, eyewear and hand wear slots", why)
        assert(reskate.input.bind("toggle") and reskate.input.bind("slap", "Pad.LB"), "an action with no key of its own, and one with")
        reskate.on("tick", function()
            if #steps > 0 then reskate.log("steps " .. table.concat(steps, " ")) steps = {} end
        end)
        reskate.log("carried")
    )";
    host.apply({package("filmer", {{manifest_file, manifest(R"("skater.animation", "camera", "session.nametags", "board", "outfit", "input")")},
                                   {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    running(host, "filmer", environment);
    check(environment.logged("[filmer] carried"), "The calls work\n" + environment.all());
    check(game.called("carry filmer right both 1.0 2.0 3.0 front -0.25 grip true w 0.75 crouch 0.22 hunch 1.00 turn 0.50 step 0.50 still push others") &&
              game.called("carry filmer left 0.0 1.0 0.0 front 0.00 grip false w 1.00 crouch 0.00 hunch 0.00 turn 0.00 step 0.00 everyone") &&
              game.called("carry filmer none"),
          "Carried in both hands for the others, in one, and let go\n" + environment.all());
    check(game.called("carry support filmer 0.02 -0.05 0.10"), "The other hand moved where it takes the item\n" + environment.all());
    check(game.called("carry held filmer exact look 0.0 1.2 -0.1"), "Held where asked, the face on it\n" + environment.all());
    check(game.called("pose filmer 1.00 me 48:0.00,0.00,0.00,1.00x0.00") && game.called("reach filmer right 1.0 2.0 3.0 false 1.00 others"),
          "A pose only you see, a reach only the others see\n" + environment.all());
    check(game.called("pose filmer 1.00 others 47:0.00,0.71,0.00,0.71x1.00"), "And a pose only the others see, asked for beside it\n" + environment.all());
    check(game.called("pose filmer 1.00 others 276:0.00,0.71,0.00,0.71x1.00"), "By the call of its own as well\n" + environment.all());
    check(game.called("cycle filmer ready") && game.called("cycle filmer active") && game.called("cycle filmer out"), "In the cycle, on, and out");
    check(game.called("nametags filmer off") && game.called("nametags filmer on"), "The nametags held off and given back");
    check(game.called("board hide filmer 2 everyone"), "The board hidden from everyone");
    check(game.called("outfit filmer handwear_l Own_HandwearGlove_Gen_GloveFingerless_00001 0 false"), "A camcorder in the hand wear slot");
    check(game.binds.contains("filmer/toggle") && game.binds["filmer/toggle"] == 0 && game.binds["filmer/slap"] == combo_of("Pad.LB"),
          "An action the player puts on a key themselves");
    // The button's steps reach the plugin in the cycle, and nobody else.
    game.cycle_news = {{"filmer", true}, {"someone", false}, {"filmer", false}};
    ticks(host, environment, 2);
    check(environment.logged("[filmer] steps enter leave"), "The camera cycle's steps as events\n" + environment.all());
    // Each needs its permission.
    host.apply({package("sneaky", {{manifest_file, manifest()}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.skater.carry, {at = vector.create(0, 0, 0), rotation = {0, 0, 0, 1}})) .. " " ..
            tostring(pcall(reskate.camera.cycle, "ready")) .. " " .. tostring(pcall(reskate.session.nametags, false)) .. " " ..
            tostring(pcall(reskate.on, "camera_cycle", function() end)))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false false false false"), "skater.animation, camera and session.nametags are needed\n" + environment.all());
}

// Reading a skater's joints, and the physics tuning read and set.
void plugins_read_joints_and_tune_physics() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        local head = reskate.skater.joint("head")
        assert(head and head.position.x == 0 and head.position.y == 103 and head.rotation[4] == 1 and head.scale == 1, "your head")
        assert(reskate.skater.joint(300) == nil, "not known yet")
        local theirs = reskate.session.joint("42", 49)
        assert(theirs and theirs.position.x == 42 and theirs.position.y == 49, "their right hand")
        assert(reskate.session.joint({id = "7"}, "hips") == nil)
        assert(not pcall(reskate.skater.joint, 395) and not pcall(reskate.skater.joint, "elbow") and not pcall(reskate.session.joint, "me", 1))
        local jump = reskate.physics.tuning("PhysicsMode.JumpMaxHeight")
        assert(jump and jump.value == 1.2 and jump.stock == 1.2 and jump.kind == "number" and jump.label == "Jump height" and jump.tuner == nil)
        local none, why = reskate.physics.tuning("nothing.here")
        assert(none == nil and why:find("no tuning value"), why)
        assert(#reskate.physics.find("physicsmode") == 2)
        assert(reskate.physics.tune("PhysicsMode.JumpMaxHeight", 2.5) and reskate.physics.tune("PhysicsMode.AutoPushEnabled", true))
        assert(reskate.physics.tune("PhysicsMode.JumpMaxHeight", nil))
        assert(not pcall(reskate.physics.tune, "x", 0 / 0) and not pcall(reskate.physics.tune, "", 1))
        assert(reskate.skater.pose({head = {scale = 0}, [44] = {angles = vector.create(0, 0, 90)}, [8] = {rotation = {0, 0, 0, 2}}}, 0.5))
        assert(reskate.skater.pose(nil))
        assert(not pcall(reskate.skater.pose, {[1] = {scale = 0}}), "the skater's placement")
        assert(not pcall(reskate.skater.pose, {head = {scale = 11}}), "scale 0 to 10")
        assert(reskate.skater.pose({head = {scale = vector.create(1, 2, 0.5)}}, 0.25), "a scale for each axis")
        assert(not pcall(reskate.skater.pose, {head = {scale = vector.create(1, 11, 1)}}), "0 to 10 on each axis")
        assert(not pcall(reskate.skater.pose, {head = {scale = "big"}}), "a number or a vector")
        assert(not pcall(reskate.skater.pose, {head = {}}, 2), "weight 0 to 1")
        assert(not pcall(reskate.skater.pose, {head = {}, [103] = {}}), "a joint once")
        local many = {}
        for j = 2, 394 do many[j] = {scale = 1} end
        assert(reskate.skater.pose(many), "every joint that can be posed")
        assert(reskate.skater.pose(nil))
        -- A scale that is set, not multiplied: with a place, and on its own.
        assert(reskate.skater.pose({head = {place = {position = vector.create(0, 0.1, 0), scale = 2}}, [44] = {place = {scale = vector.create(1, 2, 3)}}}))
        assert(not pcall(reskate.skater.pose, {head = {place = {scale = 11}}}), "a place's scale is 0 to 10")
        assert(not pcall(reskate.skater.pose, {head = {place = {scale = vector.create(1, -1, 1)}}}), "on each axis")
        -- The whole pose as the game animated it, and a whole pose set: a photo mode holds the skater as he
        -- stood, his head turned to the camera.
        local pose = reskate.skater.read_pose()
        assert(pose and buffer.len(pose) == 395 * 40, "395 joints of 40 bytes")
        assert(math.abs(buffer.readf32(pose, 7 * 40 + 28) - 0.07) < 1e-6 and buffer.readf32(pose, 7 * 40) == 1 and buffer.readf32(pose, 7 * 40 + 24) == 1)
        buffer.writef32(pose, 7 * 40 + 12, 0)  -- joint 7: turned half round about y, twice as tall, a little higher
        buffer.writef32(pose, 7 * 40 + 16, 2)  -- (a rotation of any length is brought to 1)
        buffer.writef32(pose, 7 * 40 + 20, 0)
        buffer.writef32(pose, 7 * 40 + 24, 0)
        buffer.writef32(pose, 7 * 40 + 4, 2)
        buffer.writef32(pose, 7 * 40 + 32, 0.5)
        assert(reskate.skater.pose(pose, 0.75, {shown = "me"}))
        -- The pose as it went to the screen, after the game's body physics: how far the physics lifted the
        -- hips since the animation, for a plugin that shows it; and the pose a delta is worked out against.
        local drawn = reskate.skater.read_pose("drawn")
        assert(drawn and buffer.len(drawn) == 395 * 40, "the drawn pose is 395 joints too")
        assert(math.abs(buffer.readf32(drawn, 7 * 40 + 32) - buffer.readf32(reskate.skater.read_pose("animated"), 7 * 40 + 32) - 0.05) < 1e-6,
            "the hips stand 5 cm higher drawn than animated")
        assert(not pcall(reskate.skater.read_pose, "tomorrow"), "animated or drawn")
        local short = buffer.create(3 * 40)
        for j = 0, 2 do buffer.writef32(short, j * 40 + 24, 1) end
        assert(reskate.skater.pose(short), "a pose of the first joints only")
        assert(not pcall(reskate.skater.pose, buffer.create(41)), "40 bytes a joint")
        assert(not pcall(reskate.skater.pose, buffer.create(396 * 40)), "at most 395 joints")
        assert(not pcall(reskate.skater.pose, buffer.create(10 * 40)), "a rotation of no length")
        local broken = reskate.skater.read_pose()
        buffer.writef32(broken, 9 * 40 + 28, 0 / 0)
        assert(not pcall(reskate.skater.pose, broken), "a number that is not finite")
        buffer.writef32(broken, 9 * 40 + 28, 101)
        assert(not pcall(reskate.skater.pose, broken), "within 100 m of its parent")
        buffer.writef32(broken, 9 * 40 + 28, 0)
        buffer.writef32(broken, 9 * 40, 11)
        assert(not pcall(reskate.skater.pose, broken), "a scale of 0 to 10")
        assert(reskate.skater.pose(nil))
        -- Set to a place: a link of a chain relative to another joint, one in the skater's frame keeping
        -- its children where they were, one in the world.
        assert(reskate.skater.pose({[49] = {place = {position = vector.create(0, 0, -0.1), relative_to = 278}},
            [278] = {place = {angles = vector.create(0, 0, 90), relative_to = "skater"}, keep_children = true},
            head = {place = {position = vector.create(1, 2, 3), relative_to = "world"}}, [42] = {place = {position = vector.create(0, 0.2, 0)}}}))
        assert(not pcall(reskate.skater.pose, {[49] = {place = {relative_to = 278}}}), "a place has a position or a rotation")
        assert(not pcall(reskate.skater.pose, {[49] = {place = {position = vector.create(0, 0, 0), relative_to = "moon"}}}), "relative to a joint, the skater or the world")
        assert(not pcall(reskate.skater.pose, {head = {keep_children = 1}}), "keep_children is true or false")
        local _, itself = pcall(reskate.skater.pose, {[49] = {place = {position = vector.create(0, 0, 0), relative_to = 49}}})
        assert(itself:find("relative to itself"), itself)
        local _, below = pcall(reskate.skater.pose, {hips = {place = {position = vector.create(0, 0, 0), relative_to = "spine"}}})
        assert(below:find("hangs from it"), below)
        local _, circle = pcall(reskate.skater.pose, {[49] = {place = {position = vector.create(0, 0, 0), relative_to = 278}},
            [278] = {place = {position = vector.create(0, 0, 0), relative_to = 49}}})
        assert(circle:find("in a circle"), circle)
        assert(reskate.settings.set("Camera.Height", 1.5) and reskate.settings.set("Chat.Show", true) and
            reskate.settings.set("Render.Mode", "fast") and reskate.settings.set("Camera.Height", nil))
        -- How loud the game is stays the player's: a plugin that turned every volume up to scare them is why.
        for _, name in ipairs({"Audio.MasterVolume", "Sound.SfxVolume", "Music.Volume", "Audio.Mute", "VoIP.Enabled"}) do
            local ok, why = reskate.settings.set(name, 1)
            assert(not ok and why:find("sound settings"), name)
        end
        reskate.log("tuned")
    )";
    host.apply({package("tuner", {{manifest_file, manifest(R"("skater.read", "skater.control", "session.skaters", "settings.write", "skater.animation")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    running(host, "tuner", environment);
    check(environment.logged("[tuner] tuned"), "A plugin reads joints and tunes physics\n" + environment.all());
    check(game.called("tune tuner PhysicsMode.JumpMaxHeight 2.50") && game.called("tune tuner PhysicsMode.AutoPushEnabled 1.00") &&
              game.called("tune tuner PhysicsMode.JumpMaxHeight none"),
          "A value set, a flag set, and let go\n" + environment.all());
    check(game.called("pose tuner 0.25 103:0.00,0.00,0.00,1.00x1.00,2.00,0.50"), "A joint is scaled on each axis\n" + environment.all());
    check(game.called("pose tuner 0.50 8:0.00,0.00,0.00,1.00x1.00 44:0.00,0.00,0.71,0.71x1.00 103:0.00,0.00,0.00,1.00x0.00") &&
              game.called("pose tuner 1.00"),
          "Joints posed: a head shrunk, one turned 90 degrees about Z, and off\n" + environment.all());
    check(game.called("pose tuner 1.00 42:0.00,0.00,0.00,1.00x1.00 place@parent p0.00,0.20,0.00 49:0.00,0.00,0.00,1.00x1.00 place@278 p0.00,0.00,-0.10 "
                      "103:0.00,0.00,0.00,1.00x1.00 place@world p1.00,2.00,3.00 278:0.00,0.00,0.00,1.00x1.00 place@skater r0.00,0.00,0.71,0.71 keep") &&
              game.called("pose order tuner 42 103 278 49"),
          "Joints set to places: in their parent's frame, another joint's, the world and the skater's, the children kept; "
          "each posed after the joint it is placed relative to\n" + environment.all());
    check(game.called("pose tuner 1.00 44:0.00,0.00,0.00,1.00x1.00 s=1.00,2.00,3.00 103:0.00,0.00,0.00,1.00x1.00 place@parent p0.00,0.10,0.00 s=2.00,2.00,2.00"),
          "A scale that is set rather than multiplied, with a place and on its own\n" + environment.all());
    check(game.called("pose tuner 0.75 whole 393 from 2 to 394: 7 p0.07,0.50,0.00 r0.00,1.00,0.00,0.00 s1.00,2.00,1.00"),
          "The whole pose read, changed and set: every joint from 2, its rotation brought to a length of 1\n" + environment.all());
    check(game.called("pose tuner 1.00 2:0.00,0.00,0.00,1.00x1.00 place@parent p0.00,0.00,0.00 r0.00,0.00,0.00,1.00 s=0.00,0.00,0.00"),
          "A pose of the first joints sets only those\n" + environment.all());
    check(game.called("setting tuner Camera.Height 1.5") && game.called("setting tuner Chat.Show true") &&
              game.called("setting tuner Render.Mode fast") && game.called("setting tuner Camera.Height none"),
          "Game settings held and let go\n" + environment.all());
    host.apply({package("sneaky", {{manifest_file, manifest(R"("skater.read")")}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.physics.tune, "PhysicsMode.JumpMaxHeight", 2)) .. " " .. tostring(pcall(reskate.session.joint, "42", 1)) ..
            " " .. tostring(pcall(reskate.settings.set, "Camera.Height", 1)))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false false false"), "skater.control, session.skaters and settings.write are needed\n" + environment.all());
}

// Touching another player, hiding one on this screen, and a ray at players.
void plugins_touch_hide_and_ray_at_players() {
    // A body from its joints: the limbs between theirs, the head on from the neck, the board along its Z.
    const auto body = dingosdk::plugin_feed::body_of(9, [](std::uint16_t joint) -> std::optional<std::array<float, 3>> {
        if (joint == 103) return std::array<float, 3>{0, 1.6f, 0};
        if (joint == 101) return std::array<float, 3>{0, 1.5f, 0};
        return std::array<float, 3>{0, static_cast<float>(joint) / 1000, 0};
    }, std::pair{std::array<float, 3>{0, 0, 0}, std::array<float, 4>{0, 0.7071068f, 0, 0.7071068f}});
    const auto head = std::find_if(body.capsules.begin(), body.capsules.end(), [](const auto& c) { return c.part == 0; });
    const auto deck = std::find_if(body.capsules.begin(), body.capsules.end(), [](const auto& c) { return c.part == 6; });
    check(body.capsules.size() == 11 && head != body.capsules.end() && std::abs(head->b[1] - 1.76f) < 1e-4f && deck != body.capsules.end() &&
              std::abs(deck->a[0] + 0.3f) < 1e-4f && std::abs(deck->b[0] - 0.3f) < 1e-4f && std::abs(deck->a[1] - 0.08f) < 1e-4f,
          "A body: nine limb capsules, the head 16 cm on from the neck, the deck along its length turned 90 degrees");
    const auto gone = dingosdk::plugin_feed::body_of(9, [](std::uint16_t) -> std::optional<std::array<float, 3>> { return std::array<float, 3>{0, NAN, 0}; },
                                           std::nullopt);
    check(gone.capsules.empty(), "No body from a pose that is not finite");

    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const auto upright = [](std::uint64_t player, float x, float y) {
        return Body{player, {{1, {x, y, 0}, {x, y + 1, 0}, 0.16f}, {6, {x, y, -0.3f}, {x, y, 0.3f}, 0.12f}}};
    };
    game.body_list = {upright(0, 0, 0), upright(9, 1, 0), upright(12, 5, 0.5f), upright(13, 8, -3)};
    const std::string main = R"(
        local hit = reskate.session.raycast(vector.create(0, 1, 0), vector.create(10, 1, 0))
        assert(hit and hit.player == "9" and hit.part == "body" and math.abs(hit.distance - 0.84) < 1e-4 and math.abs(hit.position.x - 0.84) < 1e-4,
            "the nearest player, at their body's surface: " .. tostring(hit and hit.distance))
        assert(reskate.session.raycast(vector.create(0, 3, 2), vector.create(10, 3, 2)) == nil, "over everyone")
        assert(reskate.session.raycast(vector.create(8, 3, 0), vector.create(8, -5, 0)) == nil, "the ground in the way")
        local under = reskate.session.raycast(vector.create(8, 3, 0), vector.create(8, -5, 0), {world = false})
        assert(under and under.player == "13", "through the world")
        -- From inside a player's capsule the hit is where the ray starts, and the world, checked
        -- along no length at all, is not in the way.
        local inside = reskate.session.raycast(vector.create(1, 0.5, 0), vector.create(1, 0.5, 5))
        assert(inside and inside.player == "9" and inside.distance == 0 and inside.position == vector.create(1, 0.5, 0),
            "from inside a player's capsule, that player: " .. tostring(inside and inside.distance))
        local near = reskate.session.raycast(vector.create(5, -0.2, 0), vector.create(5, 3, 0))
        assert(near and near.player == "12" and math.abs(near.distance - 0.54) < 1e-4, "the world within 0.4 m before a player is not in the way")
        assert(not pcall(reskate.session.raycast, vector.create(0, 0, 0), vector.create(2000, 0, 0)), "too long")
        assert(reskate.session.hide("12") and reskate.session.hidden()[1] == "12")
        assert(reskate.session.hide({id = "12"}, false) and #reskate.session.hidden() == 0)
        -- A player's skater drawn somewhere else: where it stands, or one of its joints, put at a place.
        assert(reskate.session.place("12", {position = vector.create(1, 2, 3)}))
        assert(reskate.session.place({id = "12"}, {position = vector.create(4, 5, 6), rotation = {0, 0, 0, 2}, joint = "hips"}))
        assert(reskate.session.place("12", {position = vector.create(7, 8, 9), board = false}))
        assert(reskate.session.place("12", nil))
        assert(not pcall(reskate.session.place, "12", {}), "a place has a position")
        assert(not pcall(reskate.session.place, "12", {position = vector.create(0 / 0, 0, 0)}), "a finite position")
        assert(not pcall(reskate.session.place, "12", {position = vector.create(0, 0, 0), rotation = {0, 0, 0, 0}}), "a rotation has a length")
        assert(not pcall(reskate.session.place, "12", {position = vector.create(0, 0, 0), joint = 999}), "a joint of the skeleton")
        assert(not pcall(reskate.session.place, "nobody", {position = vector.create(0, 0, 0)}), "a player")
        assert(not pcall(reskate.session.touch, true, 2), "a reach is 0 to 1 m")
        reskate.session.touch(true)
        reskate.on("touch", function(e)
            reskate.log(string.format("touch %s %s %s %.2f %d", e.player, e.part, e.their_part, e.distance, #reskate.session.touching()))
        end)
        reskate.on("touch_end", function(e) reskate.log("apart " .. e.player .. " " .. #reskate.session.touching()) end)
        reskate.log("rayed")
    )";
    host.apply({package("tag", {{manifest_file, manifest(R"("session.skaters")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 2);
    running(host, "tag", environment);
    check(environment.logged("[tag] rayed") && game.called("hide tag 12 on") && game.called("hide tag 12 off"),
          "Rays at players and a player hidden and shown\n" + environment.all());
    check(game.called("place tag 12 at 1.0 2.0 3.0 turned no joint none") && game.called("place tag 12 at 4.0 5.0 6.0 turned 1.00 joint 7") &&
              game.called("place tag 12 at 7.0 8.0 9.0 turned no joint none no board") && game.called("place tag 12 back"),
          "A player's skater placed, by a joint and turned, and put back\n" + environment.all());
    check(!environment.logged("[tag] touch "), "Nobody within reach\n" + environment.all());
    const auto count = [&](std::string_view part) {
        return std::count_if(environment.logs.begin(), environment.logs.end(), [&](const auto& line) { return line.find(part) != std::string::npos; });
    };
    game.body_list[1] = upright(9, 0.3f, 0);
    ticks(host, environment, 2);
    check(environment.logged("[tag] touch 9 body body 0.00 1") && count("[tag] touch ") == 1, "Touching: once, as they come into contact\n" + environment.all());
    game.body_list[1] = upright(9, 0.4f, 0);
    ticks(host, environment, 2);
    check(count("[tag] touch ") == 1 && !environment.logged("[tag] apart"), "Still touching a little further apart\n" + environment.all());
    game.body_list[1] = upright(9, 0.6f, 0);
    ticks(host, environment, 1);
    check(environment.logged("[tag] apart 9 0"), "Apart once 10 cm further than the reach\n" + environment.all());
    game.body_list[1] = upright(9, 0.3f, 0);
    ticks(host, environment, 1);
    game.body_list.erase(game.body_list.begin() + 1);
    ticks(host, environment, 1);
    check(count("[tag] touch ") == 2 && count("[tag] apart 9") == 2, "Touching again, and apart when they leave\n" + environment.all());
    host.apply({package("sneaky", {{manifest_file, manifest(R"("skater.read")")}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.session.touch, true)) .. " " .. tostring(pcall(reskate.session.hide, "9")) .. " " ..
            tostring(pcall(reskate.session.raycast, vector.create(0, 1, 0), vector.create(1, 1, 0))) .. " " ..
            tostring(pcall(reskate.on, "touch", function() end)) .. " " ..
            tostring(pcall(reskate.session.place, "9", {position = vector.create(0, 0, 0)})))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false false false false false") && !game.called("place sneaky 9 at 0.0 0.0 0.0 turned no joint none"),
          "session.skaters is needed\n" + environment.all());
}

// Menu pose observation is permission-gated and never writes board bones.
void plugins_use_background_capture() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    host.apply({package("background", {{manifest_file, manifest(R"("capture")")}, {"plugin/main.lua", R"(
        assert(reskate.capture.start({background=true,depth=true}))
        reskate.capture.stop()
        assert(reskate.capture.start({depth=true}))
        reskate.on("tick",function()
            local status=reskate.capture.state()
            assert(type(status.pending)=="number" and type(status.dropped)=="number")
            if status.pending==2 and status.dropped==7 then reskate.log("queue metrics") end
        end)
        reskate.log("background options")
    )"}})});
    ticks(host, environment, 1);
    running(host, "background", environment);
    check(game.capture_starts.size() == 2 && game.capture_starts[0].background && !game.capture_starts[1].background,
          "background processing is opt-in; existing callers stay synchronous\n" + environment.all());
    game.capture_now.pending = 2;
    game.capture_now.dropped = 7;
    ticks(host, environment, 1);
    check(environment.logged("[background] queue metrics"), "pending and dropped frame metrics reach the plugin");
    const auto calls = game.capture_starts.size();
    host.apply({package("denied", {{manifest_file, manifest(R"("room")")}, {"plugin/main.lua", R"(
        assert(not pcall(function()reskate.capture.start({background=true})end))
        reskate.log("capture denied")
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[denied] capture denied") && game.capture_starts.size() == calls,
          "background capture still requires capture permission");
}

void plugins_read_menu_pose() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    game.preview = Game::RoomPose{{1, -69, -5}, {0, 0, 0, 1}, 0.01, 42};
    host.apply({package("observer", {{manifest_file, manifest(R"("room")")}, {"plugin/main.lua", R"(
        local pose = reskate.room.pose("board")
        assert(pose.position.y == -69 and pose.rotation[4] == 1)
        assert(pose.age == 0.01 and pose.sequence == 42)
        assert(reskate.room.pose("skater"))
        assert(not pcall(reskate.room.pose, "other"))
        reskate.log("observed")
    )"}})});
    ticks(host, environment, 1);
    running(host, "observer", environment);
    check(environment.logged("[observer] observed"), "Menu pose available with room permission\n" + environment.all());
    check(game.called("pose board") && game.called("pose skater"), "Both subjects are observed");
    game.preview.reset();
    host.apply({package("empty", {{manifest_file, manifest(R"("room")")}, {"plugin/main.lua", R"(
        assert(reskate.room.pose() == nil)
        reskate.log("no pose")
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[empty] no pose"), "An unavailable pose is nil");
    host.apply({package("denied", {{manifest_file, manifest(R"("capture")")}, {"plugin/main.lua", R"(
        assert(not pcall(reskate.room.pose, "board"))
        reskate.log("denied")
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[denied] denied"), "room permission is required");
}

// Typing into a field a plugin draws (only while its screen is open), and the character room.
void plugins_type_and_use_the_room() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        reskate.ui.screen("Editor", function() end, {panel = false})
        assert(not pcall(reskate.ui.screen, "Docked", function() end, {panel = false, dock = "left"}), "no panel, no dock")
        assert(reskate.input.text(true) == "", "nothing without its screen open")
        reskate.ui.open("Editor")
        local typed = reskate.input.text(true)
        assert(typed == "ab", "what was typed: " .. typed)
        assert(reskate.input.text(true) == "", "once")
        assert(reskate.room.state() == "idle")
        assert(reskate.room.open())
        assert(reskate.room.state() == "open")
        reskate.room.leave()
        assert(reskate.room.state() == "idle")
        assert(reskate.room.open({quiet = true}))
        assert(not pcall(reskate.room.open, {quiet = 1}), "quiet is true or false")
        reskate.room.leave()
        assert(reskate.room.show(true))
        assert(reskate.room.show(true, {skater = false}))
        assert(reskate.room.show(false))
        assert(not pcall(reskate.room.show), "on or off is said")
        assert(not pcall(reskate.room.show, true, {skater = 1}), "skater is true or false")
        local at = reskate.room.place()
        assert(math.abs(at.x - 1) < 1e-3 and math.abs(at.y + 69.99) < 1e-3 and math.abs(at.z + 5.25) < 1e-3, "where the room is")
        reskate.log("typed")
    )";
    game.typed = "ab";
    host.apply({package("editor", {{manifest_file, manifest(R"("ui.screen", "input", "room")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    running(host, "editor", environment);
    check(environment.logged("[editor] typed"), "A plugin takes typing and the room\n" + environment.all());
    check(game.called("room open editor") && game.called("room leave editor"), "The room is walked to and from\n" + environment.all());
    check(game.called("room quiet editor"), "The room is opened with the game's interface hidden\n" + environment.all());
    check(game.called("room show editor true true") && game.called("room show editor true false") && game.called("room show editor false true"),
          "The room's set is shown without the menu, with the skater or not, and taken away\n" + environment.all());
    host.apply({package("sneaky", {{manifest_file, manifest(R"("input")")}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.room.open)) .. " " .. tostring(pcall(reskate.room.state)) .. " " ..
                    tostring(pcall(reskate.room.show, true)) .. " " .. tostring(pcall(reskate.room.place)))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false false false false"), "room is needed\n" + environment.all());
}

// The game's own clips: listed once they are read, each a "game:" clip like any other.
void plugins_use_the_games_clips() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        local none, why = reskate.animation.game_clips("trick")
        assert(none == nil and why == "the game's animations are still being read", tostring(why))
        reskate.on("tick", function()
            local names = reskate.animation.game_clips("kickflip")
            if not names or done then return end
            done = true
            assert(#names == 2 and names[1] == "animation/dingo/c_proto_onb_trick_o_kickflip_high", "the names that contain it")
            assert(#reskate.animation.game_clips() == 3, "all of them")
            local info = reskate.animation.info("game:" .. names[1])
            assert(info.frames == 3 and info.joints == 395 and math.abs(info.takeoff - 0.05) < 1e-6 and math.abs(info.landing - 0.15) < 1e-6, "a clip")
            assert(info.stance == "regular", "its stance: " .. tostring(info.stance))
            local mirrored = reskate.animation.mirror("game:" .. names[1])
            assert(mirrored and mirrored:sub(1, 5) == "clip:" and reskate.animation.info(mirrored).stance == "goofy", "mirrored, the other stance")
            assert(reskate.animation.play("game:" .. names[1], {floor = true}))
            local ok, err = pcall(reskate.animation.info, "game:" .. names[2])
            assert(not ok and err:find("SequenceContainerAsset, not a clip"), err)
            assert(not pcall(reskate.animation.game_clips, string.rep("x", 65)), "a part of a name is at most 64 characters")
            reskate.log("game clips")
        end)
    )";
    host.apply({package("clips", {{manifest_file, manifest(R"("skater.animation")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    game.game_clips_ready = true;
    ticks(host, environment, 2);
    running(host, "clips", environment);
    check(environment.logged("[clips] game clips"), "A plugin lists and plays the game's clips\n" + environment.all());
    check(game.called("game clip animation/dingo/c_proto_onb_trick_o_kickflip_high") &&
              game.called("play clips 3 frames 1.0 0.15 false floor"),
          "read once, and played on the floor\n" + environment.all());
    host.apply({package("sneaky", {{manifest_file, manifest(R"("files")")}, {"plugin/main.lua", R"(
        reskate.log(tostring(pcall(reskate.animation.game_clips)))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false"), "skater.animation is needed\n" + environment.all());
}

// A clip read, sampled, cut, keyed, saved and read back from the data folder, and played held.
void plugins_edit_clips() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    const std::string main = R"(
        assert(reskate.animation.record(2))
        local rec = reskate.animation.recorded()
        local i = reskate.animation.info(rec)
        assert(i.frames == 3 and i.duration == 0.5 and i.joints == 395 and not i.board, "a recording's info")
        assert(math.abs(i.takeoff - 0.1) < 1e-6 and math.abs(i.landing - 0.4) < 1e-6, "with its takeoff and landing")
        local cut = reskate.animation.trim(rec, 0.2, 0.5)
        local c = reskate.animation.info(cut)
        assert(cut == "clip:2" and c.frames == 2 and math.abs(c.duration - 0.25) < 1e-6, "a trim from its first frame")
        assert(c.takeoff == nil and math.abs(c.landing - 0.15) < 1e-6, "its marks moved with it")
        local work = reskate.animation.edit(rec, {{joint = "hips", keys = {{time = 0, angles = vector.create(0, 0, 90)},
            {time = 0.5, angles = vector.create(0, 0, 90), offset = vector.create(0, 0.5, 0), ease = "linear"}}}}, {name = "work"})
        assert(work == "edit:work", work)
        local s = reskate.animation.sample(work, 7, 0.5)
        assert(math.abs(s.rotation[3] - 0.70711) < 1e-3 and math.abs(s.rotation[4] - 0.70711) < 1e-3, "the hips turned 90 degrees")
        assert(math.abs(s.position.y - 0.5) < 1e-4 and math.abs(reskate.animation.sample(work, "hips", 0.25).position.y - 0.25) < 1e-4, "and moved, blended")
        assert(reskate.animation.sample(rec, 7, 0.5).rotation[4] == 1, "the recording stays as it was")
        local many = reskate.animation.sample(work, {7, "hips", 8, 394, "board"}, 0.25)
        local one = reskate.animation.sample(work, 8, 0.25)
        assert(math.abs(many[7].position.y - 0.25) < 1e-4 and math.abs(many.hips.position.y - 0.25) < 1e-4, "joints sampled together")
        assert(many[8].rotation[4] == one.rotation[4] and many[8].scale == one.scale and many[394] ~= nil, "as one at a time")
        assert(many.board == nil and next(reskate.animation.sample(work, {}, 0)) == nil, "the board left out of a clip without it")
        assert(not pcall(reskate.animation.sample, work, {7, "nose"}, 0), "a joint that is not one")
        assert(reskate.animation.edit(rec, {}, {name = "work"}) == "edit:work", "the same name again")
        assert(reskate.animation.sample("edit:work", 7, 0.5).rotation[4] == 1, "replaces it")
        local list = reskate.animation.joints()
        assert(#list == 25 and list[1].joint == "board" and list[2].joint == 7 and list[2].label == "Hips" and list[2].moves, "the editable joints")
        assert(reskate.animation.save(work, "anims/work.fas"))
        local keyed = reskate.animation.edit(rec, {{joint = 7, keys = {{time = 0.5, offset = vector.create(0, 0.5, 0)}}}})
        assert(reskate.animation.save(keyed, "anims/keyed.fas"))
        local back = reskate.animation.info("data:anims/keyed.fas")
        assert(back.frames == 3 and math.abs(reskate.animation.sample("data:anims/keyed.fas", "hips", 0.5).position.y - 0.5) < 1e-4, "saved and read back")
        assert(reskate.animation.play(work, {at = 0.25, paused = true, floor = true}))
        reskate.animation.seek(0.4)
        reskate.animation.pause(false)
        assert(reskate.animation.time() == 0.25)
        assert(not pcall(reskate.animation.edit, rec, {{joint = 1, keys = {}}}), "the placement joint")
        assert(not pcall(reskate.animation.edit, rec, {{joint = 7, keys = {{time = 0, ease = "bouncy"}}}}), "an ease")
        assert(not pcall(reskate.animation.edit, rec, {{joint = 7, keys = {}}, {joint = "hips", keys = {}}}), "a joint once")
        assert(not pcall(reskate.animation.edit, rec, {}, {name = "../x"}), "a name")
        assert(not pcall(reskate.animation.trim, rec, 0.3, 0.2) and not pcall(reskate.animation.trim, rec, 0.26, 0.3), "a trim keeps two frames")
        assert(not pcall(reskate.animation.info, "data:none.fas") and not pcall(reskate.animation.info, "edit:none"), "clips there are not")
        assert(not pcall(reskate.animation.play, work, {at = 9}), "at is within the clip")
        reskate.log("edited")
    )";
    host.apply({package("editor", {{manifest_file, manifest(R"("skater.animation", "files")")}, {"plugin/main.lua", main}})});
    ticks(host, environment, 1);
    running(host, "editor", environment);
    check(environment.logged("[editor] edited"), "A plugin edits clips\n" + environment.all());
    check(game.called("play editor 3 frames 1.0 0.15 false at 0.25 paused floor") && game.called("seek editor 0.40") && game.called("pause editor false"),
          "and plays one held, moved and let run\n" + environment.all());
    host.apply({package("sneaky", {{manifest_file, manifest(R"("skater.animation")")}, {"plugin/main.lua", R"(
        assert(reskate.animation.record(2))
        local rec = reskate.animation.recorded()
        reskate.log(tostring(pcall(reskate.animation.save, rec, "x.fas")) .. " " .. tostring(pcall(reskate.animation.info, "data:x.fas")))
    )"}})});
    ticks(host, environment, 1);
    check(environment.logged("[sneaky] false false"), "files is needed for data clips\n" + environment.all());
}

void plugins_draw_on_the_overlay() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("painter", R"(
        local saved = nil
        reskate.on("draw", function(d)
            saved = d
            d:text(vector.create(10, 20, 0), "hello", {colour = "#ff8000", size = 24, align = "centre"})
            d:rect(vector.create(0, 0, 0), vector.create(100, 50, 0), {colour = 0x00ff00ff, fill = true, rounding = 4})
            d:line(vector.create(0, 0, 0), vector.create(5, 5, 0))
            d:circle(vector.create(50, 50, 0), 10)
            d:poly({vector.create(0, 0, 0), vector.create(10, 0, 0), vector.create(0, 10, 0)}, {fill = true})
            d:world_text(vector.create(0, 1, -10), "over there")
            d:world_line(vector.create(0, 0, -5), vector.create(1, 0, -5), {thickness = 3})
            d:world_circle(vector.create(0, 0, -5), 2)
            local on = d:world_to_screen(vector.create(1, 0, -10))
            assert(on and math.abs(on.x - 1060) < 0.01 and math.abs(on.z - 10) < 0.01, "world_to_screen")
            assert(d:world_to_screen(vector.create(0, 0, 5)) == nil, "behind the camera")
            -- Just in front of the camera and off to the side the point is millions of pixels
            -- out, which d:poly would refuse.
            assert(d:world_to_screen(vector.create(1000, 0, -0.06)) == nil, "far off the screen")
            assert(d:world_to_screen(vector.create(0, 1000, -0.06)) == nil, "far above the screen")
            assert(d:world_to_screen(vector.create(1, 0, -0.06)), "off the screen but drawable")
            assert(d:size().x == 1920 and d:size().y == 1080)
            assert(not pcall(d.text, d, vector.create(0, 0, 0), "x", {colour = "red"}), "a colour by name")
            assert(not pcall(d.text, d, vector.create(0 / 0, 0, 0), "x"), "NaN")
            assert(not pcall(d.poly, d, {vector.create(0, 0, 0)}), "a polygon of one point")
            assert(not pcall(d.text, d, vector.create(0, 0, 0), string.rep("x", 600)), "a long text")
            reskate.log("drawn")
        end)
        reskate.console.command("outside", "x", function()
            local ok, err = pcall(saved.text, saved, vector.create(0, 0, 0), "x")
            reskate.log("outside " .. tostring(ok) .. " " .. tostring(err):match("only inside a draw handler"))
        end)
        reskate.console.command("toasts", "x", function()
            local shown = 0
            for i = 1, 10 do if reskate.ui.toast("toast " .. i) then shown += 1 end end
            reskate.log("toasts " .. shown)
        end)
    )", R"("console", "overlay")"), script("unseen", R"(
        local ok, err = pcall(reskate.on, "draw", function() end)
        assert(not ok and err:find("overlay"), err)
        ok, err = pcall(reskate.ui.toast, "x")
        assert(not ok and err:find("overlay"), err)
        reskate.log("unseen refused")
    )")});
    running(host, "painter", environment);
    check(environment.logged("unseen refused"), "Drawing needs overlay\n" + environment.all());
    ticks(host, environment, 1);
    running(host, "painter", environment);
    check(environment.layers.size() == 1 && environment.layers[0].plugin == "painter" && environment.layers[0].shapes.size() == 8,
          "Its shapes are handed to the overlay each tick\n" + environment.all());
    check(environment.logged("drawn"), "and the draw handler ran to its end\n" + environment.all());
    const auto& first = environment.layers[0].shapes[0];
    check(first.kind == plugin_ui::Shape::Kind::text && first.text == "hello" && first.style.colour == 0xff0080ff &&
          first.style.size == 24 && first.style.align == 1, "with their style (colours as ImGui keeps them)");
    check(environment.layers[0].shapes[1].style.colour == 0xff00ff00 && environment.layers[0].shapes[1].style.fill, "a colour number too");
    host.run_console("outside");
    check(environment.logged("outside false only inside a draw handler"), "Drawing outside a draw handler is refused\n" + environment.all());
    host.run_console("toasts");
    check(environment.logged("toasts 3") && environment.toasts.size() == 3 && environment.toasts[0] == "painter: toast 1", "Three toasts in 5 s");
    environment.now += 5000;
    host.run_console("toasts");
    check(environment.toasts.size() == 6, "and three more after");
}

void plugins_have_menu_panels() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("panels", R"(
        local state = {on = false, speed = 5, mode = 1, presses = 0}
        reskate.menu.panel("Settings", function(ui)
            ui:heading("Settings")
            ui:label("Some words")
            ui:separator()
            if ui:button("go", "Go") then state.presses += 1 end
            state.on = ui:toggle("on", "Switch", state.on)
            state.speed = ui:slider("speed", "Speed", state.speed, 0, 10)
            state.mode = ui:dropdown("mode", "Mode", {"one", "two", "three"}, state.mode)
            ui:bar("Throttle", 0.25, 0, 1)
            assert(not pcall(ui.bar, ui, "Bad", 0, 1, 0), "a bar's range")
            assert(not pcall(ui.button, ui, "go", "Again"), "two widgets with one id")
        end)
        assert(not pcall(reskate.menu.panel, "Settings", function() end), "two panels with one title")
        reskate.console.command("state", "x", function()
            reskate.log(string.format("state %d %s %.1f %d", state.presses, tostring(state.on), state.speed, state.mode))
        end)
    )", R"("console", "menu")")});
    running(host, "panels", environment);
    ticks(host, environment, 2);
    check(environment.panels.empty(), "Panels are built only while the menu shows them");
    environment.panels_open = true;
    ticks(host, environment, 10);
    check(environment.panels.size() == 1 && environment.panels[0].title == "Settings" && environment.panels[0].widgets.size() == 8 &&
              environment.panels[0].widgets[7].kind == plugin_ui::Widget::Kind::bar && environment.panels[0].widgets[7].value == 0.25f,
          "Then its panel is built\n" + environment.all());
    using A = plugin_ui::Action;
    environment.actions = {A{"panels", "Settings", "go"}, A{"panels", "Settings", "on", true}, A{"panels", "Settings", "speed", false, 99},
                           A{"panels", "Settings", "mode", false, 0, 3}, A{"someone", "Settings", "go"}};
    ticks(host, environment, 10);
    host.run_console("state");
    check(environment.logged("state 1 true 10.0 3"), "What the player did reaches it, the slider held in its range\n" + environment.all());
    const auto& widgets = environment.panels[0].widgets;
    check(widgets[4].on && widgets[5].value == 10 && widgets[6].chosen == 3 && widgets[6].choices.size() == 3, "and shows in the next build");
}

// A plugin's own page in the menu's PLUGINS section: named in the section, built only while the menu
// shows it, page first and its panels after, with cards, tabs, info lines and help.
void plugins_have_pages() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("pages", R"(
        local tab, on, strength = 1, false, 50
        local size, name, tint, count, open, pick = 2, "Board", {r = 1, g = 0, b = 0}, 3, true, 0
        reskate.menu.page("Tools", function(ui)
            tab = ui:tabs("tabs", {"MAIN", "MORE"}, tab)
            ui:card("SWITCHES", "What it does")
            on = ui:toggle("on", "Switch", on, "Turns it on")
            strength = ui:slider("strength", "Strength", strength, 10, 100, "%d%%")
            ui:info("Status", "fine")
            ui:note("A note")
            ui:warn("A warning")
            ui:button("go", "GO", true)
            size = ui:buttons("sizes", {"50 MM", "54 MM", "58 MM"}, size) or size
            name = ui:text("name", "Name", name, "Type a name")
            tint = ui:colour("tint", "Tint", tint)
            count = ui:stepper("count", "Count", count, 0, 10, 0.5, "%.1f")
            open = ui:section("more", "MORE", "Folds away", open)
            pick = ui:list("pick", "Pick", {"a", "b", "c", "d"}, pick)
            assert(not pcall(ui.buttons, ui, "bad3", {}), "buttons are 1 to 32 names")
            assert(not pcall(ui.stepper, ui, "bad4", "Bad", 1, 0, 2, 0), "a step is above 0")
            assert(not pcall(ui.list, ui, "bad5", "Bad", {}), "a list has items")
            assert(not pcall(ui.colour, ui, "bad6", "Bad", {r = "x"}), "a colour is three numbers")
            assert(not pcall(ui.colour, ui, "bad8", "Bad", {0/0, 0, 0}), "and finite ones")
            assert(not pcall(ui.text, ui, "bad7", "Bad", string.rep("x", 201)), "a text field holds at most 200 characters")
            assert(not pcall(ui.slider, ui, "bad", "Bad", 1, 0, 2, "%s"), "a slider's format is one number")
            assert(not pcall(ui.slider, ui, "bad2", "Bad", 1, 0, 2, "%f %f"), "and only one")
        end)
        assert(not pcall(reskate.menu.page, "Again", function() end), "one page")
        assert(not pcall(reskate.menu.panel, "Tools", function() end), "a panel cannot share the page's title")
        reskate.menu.panel("Extra", function(ui) ui:label("more") end)
        reskate.console.command("state", "x", function()
            reskate.log(string.format("state %d %s %.0f %d %s %.1f %.1f %s %d", tab, tostring(on), strength, size, name, tint.g, count,
                tostring(open), pick))
        end)
    )", R"("console", "menu")"), script("others", R"(
        reskate.menu.panel("Theirs", function(ui) ui:label("theirs") end)
    )", R"("menu")")});
    running(host, "pages", environment);
    running(host, "others", environment);
    ticks(host, environment, 10);
    check(environment.pages.size() == 2 && environment.pages[0] == plugin_ui::PageEntry{"others", "others"} &&
              environment.pages[1] == plugin_ui::PageEntry{"pages", "Tools"},
          "The PLUGINS section names each plugin's page: its title, or its id with panels only");
    check(environment.panels.empty(), "Nothing is built while no page shows");
    environment.panels_open = true;
    environment.shown = "pages";
    ticks(host, environment, 10);
    check(environment.panels.size() == 2 && environment.panels[0].page && environment.panels[0].title == "Tools" && !environment.panels[1].page &&
              environment.panels[1].title == "Extra",
          "The shown plugin's page is built, then its panels, and no other plugin's\n" + environment.all());
    using K = plugin_ui::Widget::Kind;
    const auto& w = environment.panels[0].widgets;
    check(w.size() == 14 && w[0].kind == K::tabs && w[0].choices.size() == 2 && w[1].kind == K::card && w[1].help == "What it does" &&
              w[2].help == "Turns it on" && w[3].format == "%.0f%%" && w[4].kind == K::info && w[4].help == "fine" && w[5].kind == K::note &&
              w[6].kind == K::warn && w[7].on,
          "with its tabs, cards, help, the slider's format, info lines, notes and a main button");
    check(w[8].kind == K::buttons && w[8].choices.size() == 3 && w[8].chosen == 2 && w[9].kind == K::text && w[9].input == "Board" &&
              w[9].help == "Type a name" && w[10].kind == K::colour && w[10].colour[0] == 1 && w[10].colour[1] == 0 && w[11].kind == K::stepper &&
              w[11].step == 0.5f && w[11].value == 3 && w[11].format == "%.1f" && w[12].kind == K::section && w[12].on &&
              w[12].help == "Folds away" && w[13].kind == K::list && w[13].choices.size() == 4 && w[13].chosen == 0 && w[13].rows == 6,
          "and a row of buttons with one lit, a text field, a colour, a stepper, a section and a list");
    using A = plugin_ui::Action;
    environment.actions = {A{"pages", "Tools", "tabs", false, 0, 2}, A{"pages", "Tools", "on", true}, A{"pages", "Tools", "strength", false, 75},
                           A{"pages", "Tools", "sizes", false, 0, 3}, A{"pages", "Tools", "name", false, 0, 0, "Deck"},
                           A{"pages", "Tools", "tint", false, 0, 0, "", {0, 0.5f, 1}}, A{"pages", "Tools", "count", false, 7.3f},
                           A{"pages", "Tools", "more", false}, A{"pages", "Tools", "pick", false, 0, 2}};
    ticks(host, environment, 10);
    host.run_console("state");
    check(environment.logged("state 2 true 75 3 Deck 0.5 7.5 false 2"),
          "What the player did on it reaches the plugin, the stepper on its steps\n" + environment.all());
}

// A 16-bit mono WAVE file of `samples` at `rate`.
std::string wave_file(unsigned rate, std::size_t samples) {
    std::string out = "RIFF";
    const auto put = [&](std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    put(static_cast<std::uint32_t>(36 + samples * 2), 4);
    out += "WAVEfmt ";
    put(16, 4);
    put(1, 2);
    put(1, 2);
    put(rate, 4);
    put(rate * 2, 4);
    put(2, 2);
    put(16, 2);
    out += "data";
    put(static_cast<std::uint32_t>(samples * 2), 4);
    for (std::size_t i = 0; i < samples; ++i) put(static_cast<std::uint32_t>(static_cast<std::int16_t>((i % 20) * 1000)) & 0xffff, 2);
    return out;
}

// An IMA ADPCM WAVE file of these blocks, as ffmpeg's adpcm_ima_wav writes one (its fact chunk when `frames`).
std::string adpcm_file(unsigned channels, unsigned bits, unsigned block_align, const std::vector<std::uint8_t>& blocks, std::uint32_t frames = 0) {
    std::string body = "WAVEfmt ";
    const auto put = [&](std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) body.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    put(20, 4);
    put(0x11, 2);
    put(channels, 2);
    put(wave_rate, 4);
    put(wave_rate * block_align / 9, 4);
    put(block_align, 2);
    put(bits, 2);
    put(2, 2);
    put(1 + (block_align - 4 * channels) * 2 / channels, 2); // its frames a block
    if (frames) {
        body += "fact";
        put(4, 4);
        put(frames, 4);
    }
    body += "data";
    put(static_cast<std::uint32_t>(blocks.size()), 4);
    body.append(blocks.begin(), blocks.end());
    std::string out = "RIFF";
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((body.size() >> (8 * i)) & 0xff));
    return out + body;
}

void plugins_play_ima_adpcm_sounds() {
    const auto decode = [](const std::string& bytes, float most_seconds, std::string& error) {
        return decode_wave(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), most_seconds, error);
    };
    const auto as_played = [](int sample) { return static_cast<std::int16_t>(static_cast<float>(sample) / 32768.0f * 32767.0f); };
    // Two mono blocks of 9 frames: a header (predictor, step index) and 8 nibbles, low first. The fact chunk
    // keeps 13 of the 18. The values are those of the IMA ADPCM reference decoder.
    const std::vector<std::uint8_t> mono{0xe8, 0x03, 0, 0, 0x77, 0x07, 0x9b, 0x10, 0x30, 0xf8, 20, 0, 0xf3, 0x48, 0x00, 0x8c};
    const int expected[13] = {1000, 1011, 1041, 1104, 1113, 1056, 1034, 1040, 1058, -2000, -1957, -2040, -2052};
    std::string error;
    auto wave = decode(adpcm_file(1, 4, 8, mono, 13), 30, error);
    check(wave && wave->samples.size() == 13, "An IMA ADPCM sound plays, as long as its fact chunk says: " + error);
    bool same = wave != nullptr;
    for (std::size_t i = 0; same && i < 13; ++i) same = wave->samples[i] == as_played(expected[i]);
    check(same, "decoded as the reference decoder does, block by block");
    wave = decode(adpcm_file(1, 4, 8, mono), 30, error);
    check(wave && wave->samples.size() == 18, "every frame of its blocks without one");
    // Two channels, each the other's mirror (every nibble's sign flipped): mixed to mono they cancel.
    const std::vector<std::uint8_t> stereo{0xe8, 0x03, 0, 0, 0x18, 0xfc, 0, 0, 0x77, 0x07, 0x9b, 0x10, 0xff, 0x8f, 0x13, 0x98};
    wave = decode(adpcm_file(2, 4, 16, stereo), 30, error);
    same = wave && wave->samples.size() == 9;
    for (std::size_t i = 0; same && i < 9; ++i) same = wave->samples[i] == 0;
    check(same, "and its channels are mixed to mono, each from its own bytes: " + error);
    check(!decode(adpcm_file(1, 4, 8, mono, 13), 0.0002f, error) && error.find("longer than") != std::string::npos,
          "A long IMA ADPCM sound is refused");
    check(!decode(adpcm_file(1, 3, 8, mono), 30, error) && !decode(adpcm_file(1, 4, 6, mono), 30, error) &&
              error == "its format is not 8 to 32-bit PCM, 32-bit float or IMA ADPCM",
          "and so are other bits a sample, and blocks that are not a header and whole 4-byte groups");
}

void plugins_play_their_own_sounds() {
    std::string error;
    const auto bytes = wave_file(8000, 4000);
    const auto wave = decode_wave(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), 30, error);
    check(wave && wave->samples.size() == 24000, "Half a second at 8 kHz is half a second at 48 kHz: " + error);
    check(!decode_wave(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), 0.25f, error),
          "and a sound longer than allowed is refused");
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({package("noisy", {{manifest_file, manifest(R"("console", "audio")")}, {"sounds/beep.wav", bytes}, {"notes.txt", "x"},
                                  {"plugin/main.lua", R"(
        reskate.console.command("noise", "x", function()
            local beep = assert(reskate.audio.play("sounds/beep.wav", {volume = 0.5, pitch = 2, loop = true, position = vector.create(1, 2, 3)}))
            local motor = assert(reskate.audio.play("motor", {throttle = 0.4, speed = 10}))
            -- A sound of the player's own: at the listener, not a place in the world.
            assert(reskate.audio.play("sounds/beep.wav", {volume = 0.25, position = "listener"}))
            assert(not pcall(reskate.audio.play, "sounds/beep.wav", {position = "nowhere"}), "a position is a vector, skater or listener")
            assert(reskate.audio.set(motor, {throttle = 0.9, position = "skater"}))
            assert(reskate.audio.playing(beep))
            reskate.audio.stop(beep)
            assert(not pcall(reskate.audio.play, "notes.txt"), "a wave")
            assert(not pcall(reskate.audio.play, "../x/beep.wav"), "its own package")
            assert(not pcall(reskate.audio.play, "sounds/beep.wav", {volume = 3}), "a sane volume")
            reskate.log("sounded")
            -- A burst (automatic fire, each shot several layers): sixty in a second, and the next is refused.
            local played, why = 0, nil
            for _ = 1, 80 do
                local id, reason = reskate.audio.play("sounds/beep.wav")
                if id then played += 1 else why = reason break end
            end
            assert(played >= 50 and played <= 60, "played " .. played)
            reskate.log("burst: " .. tostring(why))
        end)
    )"}})});
    running(host, "noisy", environment);
    host.run_console("noise");
    check(environment.logged("[noisy] sounded"), "A plugin plays its own sounds\n" + environment.all());
    check(environment.logged("[noisy] burst: more than 60 sounds in a second"), "sixty a second, and the next is refused\n" + environment.all());
    for (const auto* call : {"sound noisy 24000 0.50 2.00 true at 1 2 3", "sound noisy motor 1.00 1.00 false", "sound noisy 24000 0.25 1.00 false at listener",
                             "change noisy 2 0.9 skater", "stop sound noisy 1"})
        check(environment.fake.called(call), std::string("The game was asked: ") + call + "\n" + environment.all());
}

void plugins_play_any_number_of_different_sounds() {
    // "play <round> <names...>" logs "<round> played <name>" or "<round> refused <name>: <why>".
    const auto play = R"(
        reskate.console.command("play", "round names", function(args)
            for i = 2, #args do
                local ok, why = pcall(reskate.audio.play, "sounds/" .. args[i] .. ".wav")
                reskate.log(args[1] .. (ok and " played " or " refused ") .. args[i] .. (ok and "" or ": " .. tostring(why)))
            end
        end)
    )";
    const std::map<std::string, std::string> files{{manifest_file, manifest(R"("console", "audio")")}, {"sounds/a.wav", wave_file(wave_rate, 1000)},
                                                   {"sounds/b.wav", wave_file(wave_rate, 2000)}, {"sounds/c.wav", wave_file(wave_rate, 3000)},
                                                   {"plugin/main.lua", play}};
    {
        TestEnvironment environment;
        Limits limits;
        limits.sounds = 2;
        Host host(environment, Side::client, limits);
        host.apply({package("dj", files)});
        running(host, "dj", environment);
        // Holding two, a third lets go of the one played longest ago, and a wave let go plays again.
        host.run_console("play 1 a b c a b");
        check(environment.logged("1 played c") && environment.logged("1 played b") && !environment.logged("refused"),
              "Past the limit the oldest idle wave is let go\n" + environment.all());
        check(environment.fake.called("sound dj 1000 1.00 1.00 false") && environment.fake.called("sound dj 3000 1.00 1.00 false"),
              "and every sound reached the game\n" + environment.all());
        // While every wave it holds is still playing, a new one is refused; once one ends it plays.
        environment.fake.hold_waves = true;
        host.run_console("play 2 a b c");
        check(environment.logged("2 played b") && environment.logged("2 refused c: ") && environment.logged("all it holds are playing"),
              "Only waves no sound plays are let go\n" + environment.all());
        environment.fake.held_waves.clear();
        environment.fake.hold_waves = false;
        host.run_console("play 3 c");
        check(environment.logged("3 played c"), "A wave whose sound ended is let go\n" + environment.all());
    }
    {
        // The bytes held count too: room for b (2000 samples) or c (3000), not both.
        TestEnvironment environment;
        Limits limits;
        limits.sound_bytes = 5000 * sizeof(std::int16_t) - 1;
        Host host(environment, Side::client, limits);
        host.apply({package("dj", files)});
        running(host, "dj", environment);
        environment.fake.hold_waves = true;
        host.run_console("play 1 b c");
        check(environment.logged("1 played b") && environment.logged("1 refused c: "), "A plugin holds at most so many bytes of waves\n" + environment.all());
        environment.fake.held_waves.clear();
        environment.fake.hold_waves = false;
        host.run_console("play 2 c b");
        check(environment.logged("2 played c") && environment.logged("2 played b"), "and lets go of idle ones to stay under it\n" + environment.all());
    }
}

void plugins_spatialize_the_station() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("speakers", R"(
        reskate.console.command("place", "x", function()
            assert(reskate.spatialize_music({left = 2, right = 0.5, lpf = 0.5, lpf_r = 0.25, itd = 12.7, env = 0.3}))
            assert(reskate.spatialize_music({left = 9, right = -1, lpf = 0, itd = 400, env = 2, mute_loops = false}), "held to the ranges")
            assert(not reskate.spatialize_music({active = false}) and not reskate.spatialize_music(false) and not reskate.spatialize_music())
            assert(not pcall(reskate.spatialize_music, {left = 0/0}), "a finite number")
            assert(not pcall(reskate.spatialize_music, 3), "a table, or nothing")
            reskate.log("placed")
        end)
    )", R"("console", "music")"),
                script("deaf", R"(
        local ok, why = pcall(reskate.spatialize_music, {left = 2})
        assert(not ok and why:find("permission"), tostring(why))
        reskate.log("refused")
    )", R"("console")")});
    running(host, "speakers", environment);
    host.run_console("place");
    check(environment.logged("[speakers] placed") && environment.logged("[deaf] refused"), "A plugin with music spatializes the station; one without cannot\n" + environment.all());
    for (const auto* call : {"spatial 2.00 0.50 0.50 0.25 12 0.30 true", "spatial 4.00 0.00 0.04 0.04 32 0.65 false", "spatial off"})
        check(environment.fake.called(call), std::string("The game was asked: ") + call + "\n" + environment.all());
}

void plugins_play_sounds_and_read_the_mods() {
    TestEnvironment environment;
    environment.mod_list = {{"Owner-Map", "A Map", true}, {"OldThing", "Old Thing", false}};
    Host host(environment, Side::client);
    host.apply({script("listener", R"(
        reskate.console.command("sounds", "x", function()
            local played = 0
            for i = 1, 20 do if reskate.audio.play_ui("select") then played += 1 end end
            reskate.log("played " .. played)
            assert(not pcall(reskate.audio.play_ui, "explosion"), "a sound there is not")
        end)
        reskate.console.command("mods", "x", function()
            reskate.log(string.format("mods %s %s %s %s %d", tostring(reskate.mods.installed("a map")), tostring(reskate.mods.installed("OWNER-MAP")),
                tostring(reskate.mods.installed("Old Thing")), tostring(reskate.mods.installed("nothing")), #reskate.mods.list()))
        end)
    )", R"("console", "audio.ui", "mods.read")")});
    host.run_console("sounds");
    check(environment.logged("played 10") && environment.sounds.size() == 10, "Ten sounds a second\n" + environment.all());
    host.run_console("mods");
    check(environment.logged("mods on on off nil 2"), "Mods by folder or title, on or off\n" + environment.all());
}

void plugins_read_item_origins_and_favorites() {
    TestEnvironment environment;
    environment.mod_list = {{"Author_Pack", "Author Pack", true, "Ada", "1.4.2"}, {"Bare", "Bare", false}};
    auto& game = environment.fake;
    game.origins_ready = false;
    game.origin_rows = {
        {"own_hat", true, false, "Author_Pack", "Author Pack", "Ada", "1.4.2"},
        {"deck_stock", true, true, "Author_Pack", "Author Pack", "Ada", "1.4.2"},
        {"grip_plain", true, false, "Author_Pack", "Author Pack", "", ""},
        {"shoe_plain", false, true, {}, {}, {}, {}},
    };
    game.favorites_ready = false;
    Host host(environment, Side::client);
    host.apply({script("picker", R"(
        local n = 0
        reskate.on("favorites_changed", function()
            n += 1
            reskate.log("favorites changed " .. n)
        end)
        local function fields(row)
            local names = {}
            for key in pairs(row) do table.insert(names, key) end
            table.sort(names)
            return table.concat(names, ",")
        end
        reskate.console.command("origins_closed", "x", function()
            local rows, ready = reskate.mods.item_origins({"own_hat", "shoe_plain"})
            assert(ready == false, "catalogues not ready")
            assert(rows.own_hat.source == "unknown" and fields(rows.own_hat) == "source", "a mod is not reported while the catalogues are unread")
            assert(rows.shoe_plain.source == "unknown" and fields(rows.shoe_plain) == "source", "unknown is not the game")
            local favs, why = reskate.menu.favorites()
            assert(favs == nil and why == "not loaded", "favorites not loaded: " .. tostring(why))
            reskate.log("closed origins")
        end)
        reskate.console.command("origins_open", "x", function()
            local rows, ready = reskate.mods.item_origins({"Own_Hat", "deck_stock", "grip_plain", "shoe_plain", "no_such"})
            assert(ready == true, "catalogues ready")
            local hat = rows.Own_Hat
            assert(hat.source == "mod" and hat.folder == "Author_Pack" and hat.title == "Author Pack"
                and hat.author == "Ada" and hat.version == "1.4.2", "mod origin")
            assert(fields(hat) == "author,folder,source,title,version", "a mod origin has those fields")
            local deck = rows.deck_stock
            assert(deck.source == "changed" and deck.folder == "Author_Pack" and deck.author == "Ada", "a game item a mod changes")
            local grip = rows.grip_plain
            assert(grip.source == "mod" and grip.folder == "Author_Pack" and grip.title == "Author Pack"
                and grip.author == nil and grip.version == nil, "an undeclared author is left out")
            local shoe = rows.shoe_plain
            assert(shoe.source == "game" and fields(shoe) == "source", "a game item no mod touches")
            assert(rows.no_such.source == "unknown" and fields(rows.no_such) == "source", "a key nowhere is unknown")
            local S = reskate.Enums.ItemSource
            assert(S.mod == "mod" and S.changed == "changed" and S.game == "game" and S.unknown == "unknown", "Enums.ItemSource")
            assert(fields(S) == "changed,game,mod,unknown", "Enums.ItemSource holds the four sources")
            assert(hat.source == S.mod and deck.source == S.changed and shoe.source == S.game and rows.no_such.source == S.unknown,
                "a source is an Enums.ItemSource word")
            assert(not pcall(function() S.other = "other" end), "Enums.ItemSource is read-only")
            local empty, still = reskate.mods.item_origins({})
            local count = 0
            for _ in pairs(empty) do count += 1 end
            assert(still == true and count == 0, "an empty ask is ready and empty")
            reskate.log("open origins")
        end)
        reskate.console.command("favorites", "x", function()
            local favs, why = reskate.menu.favorites()
            assert(why == nil, tostring(why))
            assert(favs.own_hat == true and favs.deck_stock == true and favs.shoe_plain == nil, "the starred keys")
            local count = 0
            for _ in pairs(favs) do count += 1 end
            assert(count == 2, "only the starred keys")
            reskate.log("favorites read")
        end)
        reskate.console.command("bounds", "x", function()
            local too = {}
            for i = 1, 4001 do too[i] = "k" end
            local ok, err = pcall(reskate.mods.item_origins, too)
            assert(not ok and tostring(err):find("at most 4000"), tostring(err))
            ok, err = pcall(reskate.mods.item_origins, {string.rep("a", 256)})
            assert(not ok and tostring(err):find("longer than 255"), tostring(err))
            ok, err = pcall(reskate.mods.item_origins, "own_hat")
            assert(not ok, tostring(err))
            reskate.log("bounds held")
        end)
        reskate.console.command("mods", "x", function()
            local pack, bare
            for _, row in ipairs(reskate.mods.list()) do
                if row.folder == "Author_Pack" then pack = row else bare = row end
            end
            assert(pack.title == "Author Pack" and pack.on == true and pack.author == "Ada" and pack.version == "1.4.2"
                and pack.installed == nil, "author and version, and no invented install time")
            assert(bare.folder == "Bare" and bare.on == false and bare.author == nil and bare.version == nil, "a mod that names none")
            reskate.log("mod list")
        end)
    )", R"("console", "mods.read", "menu")"),
        script("blocked", R"(
        reskate.console.command("blocked", "x", function()
            local ok, err = pcall(reskate.mods.item_origins, {"own_hat"})
            assert(not ok and tostring(err):find("needs the \"mods.read\" permission"), tostring(err))
            ok, err = pcall(reskate.menu.favorites)
            assert(not ok and tostring(err):find("needs the \"menu\" permission"), tostring(err))
            ok, err = pcall(reskate.on, "favorites_changed", function() end)
            assert(not ok and tostring(err):find("needs the \"menu\" permission"), tostring(err))
            reskate.log("blocked")
        end)
    )", R"("console")")});
    running(host, "picker", environment);
    running(host, "blocked", environment);
    ticks(host, environment, 1);
    check(!environment.logged("favorites changed"), "The first look does not pretend the set changed\n" + environment.all());
    check(host.run_console("origins_closed") && environment.logged("closed origins"), "Unread catalogues stay unknown\n" + environment.all());
    game.origins_ready = true;
    check(host.run_console("origins_open") && environment.logged("open origins"), "Origins follow the merge and the catalogues\n" + environment.all());
    game.favorites_ready = true;
    game.favorite_generation = 1;
    game.favorite_keys = {"own_hat", "deck_stock"};
    ticks(host, environment, 1);
    check(environment.logged("favorites changed 1"), "Loading the favorites file is a change\n" + environment.all());
    check(host.run_console("favorites") && environment.logged("favorites read"), "Favorites read back\n" + environment.all());
    ticks(host, environment, 1);
    check(!environment.logged("favorites changed 2"), "The same set does not fire again\n" + environment.all());
    game.favorite_keys = {"own_hat"};
    game.favorite_generation = 2;
    ticks(host, environment, 1);
    check(environment.logged("favorites changed 2"), "Starring an item fires once\n" + environment.all());
    check(host.run_console("bounds") && environment.logged("bounds held"), "Key limits match the grid\n" + environment.all());
    check(host.run_console("mods") && environment.logged("mod list"), "The mod list names author and version\n" + environment.all());
    check(host.run_console("blocked") && environment.logged("blocked"), "Without the permission the calls raise\n" + environment.all());
}

// Two games in one session, each with the same plugin, passing messages through their hosts.
void plugins_follow_the_session_and_message_each_other() {
    TestEnvironment a, b;
    a.session_now = {true, false, {{1, "Ann", true, true}, {2, "Bob", false, false}}};
    b.session_now = {true, false, {{2, "Bob", true, false}, {1, "Ann", false, true}}};
    const auto main = R"(
        reskate.on("player_joined", function(p) reskate.log("joined " .. p.name .. " " .. p.id .. (p.me and " me" or "") .. (p.host and " host" or "")) end)
        reskate.on("player_left", function(p) reskate.log("left " .. p.name) end)
        reskate.on("message", function(from, text) reskate.log("from " .. from.name .. ": " .. text) end)
        reskate.console.command("hello", "x", function(args)
            local s = reskate.session
            reskate.log(string.format("session %s %s %d me %s host %s", tostring(s.active()), tostring(s.is_dedicated()), #s.players(),
                s.me().name, s.host().name))
            assert(reskate.net.send(args[1] or "all", "hi from " .. s.me().name))
        end)
        reskate.console.command("alone", "x", function()
            reskate.log("alone " .. tostring(reskate.net.send("all", "x")) .. " " .. tostring(reskate.session.active()) .. " " .. #reskate.session.players())
        end)
        reskate.console.command("flood", "x", function()
            local ok, err = pcall(function() for i = 1, 10 do reskate.net.send("all", string.rep("x", 4000)) end end)
            reskate.log("flood " .. tostring(err):match("more than %d+ KB") )
            assert(not pcall(reskate.net.send, "all", string.rep("x", 5000)), "too big")
            assert(not pcall(reskate.net.send, "nobody", "x"), "not a player")
        end)
    )";
    // Each game's folder for it is named differently: the messages go by its manifest.json name.
    auto first = package("chat-a", {{manifest_file, manifest(R"("console", "session.read", "net.session")")}, {"manifest.json", R"({"name": "Chatter"})"}, {"plugin/main.lua", main}});
    auto second = package("chat-b", {{manifest_file, manifest(R"("console", "session.read", "net.session")")}, {"manifest.json", R"({"name": "Chatter"})"}, {"plugin/main.lua", main}});
    auto other = package("other", {{manifest_file, manifest(R"("net.session")")}, {"manifest.json", R"({"name": "Other"})"}, {"plugin/main.lua",
        R"(reskate.on("message", function(from, text) reskate.log("other heard " .. text) end))"}});
    Host host_a(a, Side::client), host_b(b, Side::client);
    host_a.apply({first});
    host_b.apply({second, other});
    ticks(host_a, a, 1);
    ticks(host_b, b, 1);
    check(a.logged("[chat-a] joined Ann 1 me host") && a.logged("[chat-a] joined Bob 2"), "Players in the session are heard\n" + a.all());
    host_a.run_console("hello");
    check(a.logged("session true false 2 me Ann host Ann"), "The session reads\n" + a.all());
    check(a.sent.size() == 1 && wire::is_plugin(a.sent[0]), "A message goes out as a plugin message");
    for (auto& bytes : a.sent) b.incoming.push_back({1, bytes});
    a.sent.clear();
    ticks(host_b, b, 1);
    check(b.logged("[chat-b] from Ann: hi from Ann") && !b.logged("other heard"), "and reaches the same plugin only\n" + b.all());
    host_a.run_console("hello 3");
    for (auto& bytes : a.sent) b.incoming.push_back({1, bytes});
    a.sent.clear();
    ticks(host_b, b, 1);
    check(std::count_if(b.logs.begin(), b.logs.end(), [](const auto& l) { return l.find("from Ann") != std::string::npos; }) == 1,
          "A message for another player is not delivered here");
    b.incoming.push_back({1, {0xD1, 1, 1, 2}});
    b.incoming.push_back({1, {7, 7, 7}});
    ticks(host_b, b, 1);
    running(host_b, "chat-b", b);
    host_a.run_console("flood");
    check(a.logged("flood more than 16 KB"), "16 KB a second\n" + a.all());
    a.session_now.players.pop_back();
    ticks(host_a, a, 1);
    check(a.logged("[chat-a] left Bob"), "A player who leaves is heard");
    a.session_now = {};
    ticks(host_a, a, 1);
    a.now += 1000; // past the flood's second
    host_a.run_console("alone");
    check(a.logged("alone false false 0"), "Without a session nothing is sent\n" + a.all());
    running(host_a, "chat-a", a);
}

// A game mode's rules in the plugin's server copy: players' copies talk to it, it answers everyone.
void plugins_server_copies_are_the_authority() {
    TestEnvironment server, player;
    server.session_now = {true, false, {{1, "Ann", true, true}, {2, "Bob", false, false}}};
    player.session_now = {true, false, {{2, "Bob", true, false}, {1, "Ann", false, true}}};
    const std::string main = R"(
        reskate.log("side " .. reskate.side)
        local E = reskate.Enums
        assert(E.Side.server == "server" and E.Event.tick == "tick" and E.Key.Enter == "Enter" and E.LogLevel.warn == "warn"
            and E.Target.all == "all" and E.EntityStatus.ready == "ready" and E.Throwdown.spot_battle == "spot_battle")
        assert(not pcall(function() E.Pop.huge = "huge" end), "an enum is read-only")
        reskate.log("enum check", E.LogLevel.warn)
        if reskate.side == E.Side.server then
            reskate.on("message", function(from, text)
                reskate.log("server heard " .. text .. " from " .. from.name)
                assert(reskate.net.send(reskate.Enums.Target.all, "state " .. text))
                assert(reskate.server.kick(from, "testing"))
                assert(reskate.server.say("round over"))
                assert(not pcall(reskate.net.send, "server", "x"), "it is the server")
            end)
        else
            reskate.on("message", function(from, text) reskate.log("player heard " .. text .. " server " .. tostring(from.server)) end)
            reskate.console.command("join", "x", function()
                assert(reskate.net.send("server", "hello"))
                local ok, err = pcall(reskate.server.kick, "1")
                assert(not ok and err:find("server copy"), err)
            end)
        end
    )";
    const auto mode = [&](const char* id) {
        return package(id, {{manifest_file, manifest(R"("console", "net.session", "server")", "both")}, {"manifest.json", R"({"name": "Mode"})"},
                            {"plugin/main.lua", main}});
    };
    Host server_host(server, Side::server), player_host(player, Side::client);
    server_host.apply({mode("mode-s")});
    player_host.apply({mode("mode-p")});
    ticks(server_host, server, 1);
    ticks(player_host, player, 1);
    check(server.logged("[mode-s] side server") && player.logged("[mode-p] side client"), "Each copy knows its side\n" + server.all() + player.all());
    player_host.run_console("join");
    check(player.sent.size() == 1 && wire::to_of(player.sent[0]) == wire::authority && !wire::from_authority(player.sent[0]) &&
          !wire::relayed(player.sent[0]), "A player's copy speaks to the authority, and no relay passes that on\n" + player.all());
    player_host.apply({});
    player_host.apply({mode("mode-p")});
    ticks(player_host, player, 1);
    player.incoming.push_back({2, player.sent[0]});
    ticks(player_host, player, 1);
    check(!player.logged("player heard hello"), "What is for the authority is not heard by players' copies");
    server.incoming.push_back({2, player.sent[0]});
    ticks(server_host, server, 1);
    check(server.logged("[mode-s] server heard hello from Bob"), "The server copy hears it, from the player\n" + server.all());
    check(server.fake.called("kick 2 testing") && server.fake.called("announce Mode round over"), "and rules: a kick, a line to everyone");
    check(server.sent.size() == 1 && wire::from_authority(server.sent[0]) && !wire::relayed(server.sent[0]), "Its answer is the authority's");
    player.incoming.push_back({1, server.sent[0]});
    ticks(player_host, player, 1);
    check(player.logged("[mode-p] player heard state hello server true"), "and players' copies hear it from the server\n" + player.all());
    running(server_host, "mode-s", server);
    running(player_host, "mode-p", player);
}

// A dedicated server's park mods, for a server copy with server.parks: listed, spawned and removed
// as the server's park-mod does. Not for a copy without the permission, nor for a player's copy.
// One player hidden from others, for a server copy with "server": wholly (skater, name, voice,
// sound) or only their voice or sound, from a player, a list of them or "all", and shown again.
// Not for a copy without the permission, nor for a player's copy.
void plugins_server_copies_hide_players() {
    TestEnvironment server, player, blind;
    const std::string main = R"(
        reskate.console.command("hide", "x", function()
            assert(reskate.server.hide("7", "all"))
            assert(reskate.server.hide({id = "7"}, {"8", {id = "5"}}))
            assert(reskate.server.hide("7", "8", {skater = false, voice = true, sound = false}))
            assert(reskate.server.hide("7", "8", {skater = false, sound = true}))
            assert(reskate.server.hide("7", "8", {skater = true, name = true}))
            local ok, why = reskate.server.hide("9", "8")
            assert(not ok and why == "that player is not on the server", tostring(why))
            assert(reskate.server.unhide("7", "8"))
            assert(reskate.server.unhide("7"))
            assert(not pcall(reskate.server.hide, "7"), "who it is hidden from")
            assert(not pcall(reskate.server.hide, "7", {}), "at least one viewer")
            assert(not pcall(reskate.server.hide, "7", "8", {skater = false, voice = false, sound = false}), "it hides something")
            assert(not pcall(reskate.server.hide, "7", "8", {skater = true, voice = false}), "a hidden skater's voice goes too")
            assert(not pcall(reskate.server.hide, "7", "8", {skater = false, name = true}), "the name goes with the skater")
            assert(not pcall(reskate.server.hide, "7", "8", {colour = true}), "only skater, name, voice and sound")
            assert(not pcall(reskate.server.hide, "7", "8", {skater = "yes"}), "true or false")
            assert(not pcall(reskate.server.unhide, "nobody"), "a player")
            reskate.log("hide done")
        end)
    )";
    const auto hider = [&](const char* id, const char* permissions, const char* side) {
        return package(id, {{manifest_file, manifest(permissions, side)}, {"plugin/main.lua", main}});
    };
    Host server_host(server, Side::server), player_host(player, Side::client), blind_host(blind, Side::server);
    server_host.apply({hider("hide-s", R"("console", "server")", "server")});
    player_host.apply({hider("hide-p", R"("console", "server")", "client")});
    blind_host.apply({hider("hide-b", R"("console")", "server")});
    for (auto* host : {&server_host, &player_host, &blind_host}) host->run_console("hide");
    check(server.logged("[hide-s] hide done"), "A server copy could not hide players\n" + server.all());
    for (const auto* call : {"hide hide-s 7 from all skater true voice true sound true", "hide hide-s 7 from 8,5 skater true voice true sound true",
                             "hide hide-s 7 from 8 skater false voice true sound false", "hide hide-s 7 from 8 skater false voice true sound true",
                             "hide hide-s 9 from 8 skater true voice true sound true", "unhide hide-s 7 from 8", "unhide hide-s 7 from all"})
        check(server.fake.called(call), std::string("The server was asked: ") + call + "\n" + server.all());
    check(!server.fake.called("hide hide-s 7 from 8 skater false voice false sound false") && !server.fake.called("unhide hide-s 0"),
          "A refused call reached the server");
    check(!player.logged("hide done") && player.all().find("server copy") != std::string::npos, "A player's copy hid players\n" + player.all());
    check(!blind.logged("hide done") && blind.all().find("\"server\"") != std::string::npos, "A plugin without server hid players\n" + blind.all());
    running(server_host, "hide-s", server);
}

void plugins_server_copies_spawn_park_mods() {
    TestEnvironment server, player, blind;
    const std::string main = R"(
        reskate.console.command("parks", "x", function()
            local list = reskate.server.parks()
            assert(#list == 2 and list[1].name == "street" and list[1].here and list[1].spawned and list[1].objects == 12 and list[1].minutes_left == 5)
            assert(list[2].map == "Isle of Grom" and not list[2].here and not list[2].spawned)
            assert(reskate.server.park_spawn("street", 30))
            assert(reskate.server.park_spawn("street"))
            local ok, why = reskate.server.park_spawn("plaza")
            assert(not ok and why:find("belongs on Isle of Grom"), why)
            assert(reskate.server.park_remove("street"))
            assert(not pcall(reskate.server.park_spawn, "Street"), "a park mod's name, lower case")
            assert(not pcall(reskate.server.park_spawn, "street 5; map grom"), "one name and nothing else")
            assert(not pcall(reskate.server.park_spawn, "street", 0), "minutes 1 to 1440")
            assert(not pcall(reskate.server.park_spawn, "street", 1441), "minutes 1 to 1440")
            reskate.log("parks done")
        end)
    )";
    const auto parks = [&](const char* id, const char* permissions, const char* side) {
        return package(id, {{manifest_file, manifest(permissions, side)}, {"plugin/main.lua", main}});
    };
    Host server_host(server, Side::server), player_host(player, Side::client), blind_host(blind, Side::server);
    server_host.apply({parks("parks-s", R"("console", "server.parks")", "server")});
    player_host.apply({parks("parks-p", R"("console", "server.parks")", "client")});
    blind_host.apply({parks("parks-b", R"("console", "server")", "server")});
    for (auto* host : {&server_host, &player_host, &blind_host}) host->run_console("parks");
    check(server.logged("[parks-s] parks done"), "A server copy could not use the park mods\n" + server.all());
    check(server.fake.called("park spawn parks-s street 30") && server.fake.called("park spawn parks-s street 0") &&
              server.fake.called("park spawn parks-s plaza 0") && server.fake.called("park remove parks-s street"),
          "The spawns and removal did not reach the server, by the plugin's id");
    check(!server.fake.called("park spawn parks-s Street") && !server.fake.called("park spawn parks-s street 1441"), "A refused call reached the server");
    check(!player.logged("parks done") && player.all().find("server copy") != std::string::npos,
          "A player's copy used the server's park mods\n" + player.all());
    check(!blind.logged("parks done") && blind.all().find("server.parks") != std::string::npos,
          "A plugin without server.parks used the park mods\n" + blind.all());
}

// A player's role, for a server copy with server: read-only, nil for one with none. Not for a copy
// without the permission, nor for a player's copy.
void plugins_server_copies_see_roles() {
    TestEnvironment server, player, blind;
    for (auto* environment : {&server, &player, &blind}) environment->session_now = {true, false, {{1, "Ann", false, false}, {2, "Bob", false, false}}};
    const std::string main = R"(
        reskate.console.command("roles", "x", function()
            local ann, bob = reskate.session.players()[1], reskate.session.players()[2]
            local role = ann.role
            assert(role and role.name == "moderator" and role.rank == 50 and role.staff == true)
            assert(not pcall(function() role.rank = 1000 end), "read-only")
            assert(bob.role == nil, "nil with no role")
            reskate.log("roles done")
        end)
    )";
    const auto roles = [&](const char* id, const char* permissions, const char* side) {
        return package(id, {{manifest_file, manifest(permissions, side)}, {"plugin/main.lua", main}});
    };
    Host server_host(server, Side::server), player_host(player, Side::client), blind_host(blind, Side::server);
    server_host.apply({roles("roles-s", R"("console", "session.read", "server")", "server")});
    player_host.apply({roles("roles-p", R"("console", "session.read", "server")", "client")});
    blind_host.apply({roles("roles-b", R"("console", "session.read")", "server")});
    // a tick first: the host takes the session's players then (Host::follow_session), so they are there to ask
    ticks(server_host, server, 1);
    ticks(player_host, player, 1);
    ticks(blind_host, blind, 1);
    for (auto* host : {&server_host, &player_host, &blind_host}) host->run_console("roles");
    check(server.logged("[roles-s] roles done"), "A server copy could not read the players' roles\n" + server.all());
    check(!player.logged("roles done") && player.all().find("server copy") != std::string::npos,
          "A player's copy read the players' roles\n" + player.all());
    check(!blind.logged("roles done") && blind.all().find("\"server\"") != std::string::npos,
          "A plugin without server read the players' roles\n" + blind.all());
}

// Cards and polls on players' screens, for a server copy with "server": for a player, a list of
// them or "all", with checked text, and each answer or close coming back as an event to the plugin
// that asked. Not for a copy without the permission, nor for a player's copy.
void plugins_server_copies_show_cards_and_polls() {
    TestEnvironment server, player, blind;
    const std::string main = R"(
        local heard = pcall(reskate.on, "poll_answer", function(e)
            reskate.log(string.format("answer %d %s %d %s", e.id, e.player, e.answer, e.text))
        end)
        pcall(reskate.on, "poll_closed", function(e) reskate.log(string.format("closed %d %s %s", e.id, e.player, e.why)) end)
        if not heard then reskate.log("no poll events") end
        reskate.console.command("cards", "x", function()
            assert(reskate.server.card("all", "Race in 30 s"))
            assert(reskate.server.card({"7", {id = "8"}}, "Ready?", 5))
            assert(reskate.server.card({id = "7"}, "Go"))
            local id = assert(reskate.server.poll("all", "Next track?", {"Docks", "Hill"}, {seconds = 20}))
            assert(id == 41, tostring(id))
            assert(reskate.server.poll("7", "Again?", {"Yes", "No", "Later"}) == 42)
            local none, why = reskate.server.poll("9", "Anyone?", {"a", "b"})
            assert(none == nil and why == "that player is not on the server", tostring(why))
            assert(reskate.server.poll_close(41))
            assert(not pcall(reskate.server.card, "all", ""), "a card says something")
            assert(not pcall(reskate.server.card, "all", string.rep("x", 121)), "120 characters at most")
            assert(not pcall(reskate.server.card, "all", "x", 0), "1 to 60 s")
            assert(not pcall(reskate.server.card, "all", "x", 61), "1 to 60 s")
            assert(not pcall(reskate.server.card, "everyone", "x"), "a player, a list of them or all")
            assert(not pcall(reskate.server.card, {}, "x"), "at least one player")
            assert(not pcall(reskate.server.poll, "all", "", {"a", "b"}), "a question")
            assert(not pcall(reskate.server.poll, "all", "q?", {"only one"}), "2 to 6 answers")
            assert(not pcall(reskate.server.poll, "all", "q?", {"1", "2", "3", "4", "5", "6", "7"}), "2 to 6 answers")
            assert(not pcall(reskate.server.poll, "all", "q?", {"a", string.rep("x", 49)}), "48 characters an answer at most")
            assert(not pcall(reskate.server.poll, "all", "q?", {"a", ""}), "an answer says something")
            assert(not pcall(reskate.server.poll, "all", "q?", {"a", "b"}, {seconds = 4}), "5 to 300 s")
            assert(not pcall(reskate.server.poll_close, "x"), "a poll's id")
            reskate.log("cards done")
        end)
    )";
    const auto cards = [&](const char* id, const char* permissions, const char* side) {
        return package(id, {{manifest_file, manifest(permissions, side)}, {"plugin/main.lua", main}});
    };
    Host server_host(server, Side::server), player_host(player, Side::client), blind_host(blind, Side::server);
    server_host.apply({cards("cards-s", R"("console", "server")", "server")});
    player_host.apply({cards("cards-p", R"("console", "server")", "client")});
    blind_host.apply({cards("cards-b", R"("console")", "server")});
    for (auto* host : {&server_host, &player_host, &blind_host}) host->run_console("cards");
    check(server.logged("[cards-s] cards done"), "A server copy could not show cards and polls\n" + server.all());
    for (const auto* call : {"card cards-s all Race in 30 s 0", "card cards-s 7,8 Ready? 5", "card cards-s 7 Go 0",
                             "poll cards-s all Next track? Docks|Hill 20", "poll cards-s 7 Again? Yes|No|Later 0", "poll close cards-s 41"})
        check(server.fake.called(call), std::string("The server was asked: ") + call + "\n" + server.all());
    check(!server.fake.called("card cards-s all  0") && !server.fake.called("poll cards-s all q? only one 0"), "A refused call reached the server");
    // Answers and closes, each to the plugin that asked.
    server.fake.poll_news = {{"cards-s", 41, 7, 2, "Hill", ""}, {"cards-s", 42, 7, 0, "", "timeout"}, {"someone", 50, 8, 1, "Docks", ""}};
    ticks(server_host, server, 1);
    check(server.logged("[cards-s] answer 41 7 2 Hill") && server.logged("[cards-s] closed 42 7 timeout"),
          "The plugin hears its polls answered and closed\n" + server.all());
    check(!server.logged("answer 50"), "and only its own");
    check(!player.logged("cards done") && player.all().find("server copy") != std::string::npos,
          "A player's copy showed cards\n" + player.all());
    check(!blind.logged("cards done") && blind.all().find("\"server\"") != std::string::npos && blind.logged("no poll events"),
          "A plugin without server showed cards or heard polls\n" + blind.all());
    running(server_host, "cards-s", server);
}

// Clips on the skater's body: from the package or recorded, played, replacing a trick.
void plugins_animate_the_skater() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    dingosdk::anim_studio::Clip clip;
    clip.joints = dingosdk::anim_studio::skeleton_joints;
    clip.times = {0, 0.5f};
    clip.frames.resize(clip.times.size() * clip.joints);
    clip.takeoff = 0.1f;
    clip.landing = 0.4f;
    const auto bytes = dingosdk::anim_studio::clip_bytes(clip);
    const std::string main = R"(
        reskate.console.command("go", "x", function()
            assert(reskate.animation.play("clips/spin.fas", {speed = 2, blend = 0.3, loop = true}))
            assert(not pcall(reskate.animation.play, "clips/missing.fas"), "a clip in the package")
            assert(not pcall(reskate.animation.play, "notes.txt"), "a clip file")
            assert(not pcall(reskate.animation.play, "../other/spin.fas"), "its own package only")
            assert(not pcall(reskate.animation.play, "clips/spin.fas", {speed = 99}), "a sane speed")
            assert(reskate.animation.replace("Kickflip", "clips/spin.fas", {pop = "high"}))
            assert(not pcall(reskate.animation.replace, "kick;flip", "clips/spin.fas"), "a trick's name")
            assert(not pcall(reskate.animation.replace, "ollie", "clips/spin.fas", {pop = "huge"}), "a pop")
            assert(reskate.animation.record(2))
            assert(not pcall(reskate.animation.record, 60), "at most 10 s")
            local rec = reskate.animation.recorded()
            assert(rec == "rec:1", tostring(rec))
            assert(reskate.animation.recorded() == nil, "one recording, once")
            for i = 1, 19 do reskate.animation.play(rec) end
            local over, why = reskate.animation.play(rec)
            assert(not over and why:find("20 times"), "20 a second")
            assert(reskate.animation.trick() == "kickflip")
            reskate.animation.forget(rec)
            assert(not pcall(reskate.animation.play, rec), "a forgotten recording is gone")
            reskate.animation.stop()
            reskate.log("danced")
        end)
    )";
    host.apply({package("dancer", {{manifest_file, manifest(R"("console", "skater.animation")")}, {"plugin/main.lua", main},
                                   {"clips/spin.fas", std::string(bytes.begin(), bytes.end())}, {"notes.txt", "x"}})});
    running(host, "dancer", environment);
    host.run_console("go");
    check(environment.logged("[dancer] danced"), "A plugin animates the skater\n" + environment.all());
    for (const auto* call : {"play dancer 2 frames 2.0 0.30 true", "replace dancer kickflip high clip", "record dancer 2.0", "play dancer 3 frames 1.0 0.15 false",
                             "stop dancer"})
        check(environment.fake.called(call), std::string("The game was asked: ") + call + "\n" + environment.all());
    running(host, "dancer", environment);
}

// Clips and sounds read off the plugin's thread (reskate.animation.preload, reskate.audio.preload):
// fn comes in a later tick, and once it says they're held, play finds them with their files gone. One
// that can't be read or isn't one says why, at most limits_.preloads go at once, only the plugin's
// own package is preloaded, and without its permission a plugin can't preload at all.
void plugins_preload_clips_and_sounds() {
    TestEnvironment environment;
    Limits limits;
    limits.preloads = 5;
    Host host(environment, Side::client, limits);
    dingosdk::anim_studio::Clip clip;
    clip.joints = dingosdk::anim_studio::skeleton_joints;
    clip.times = {0, 0.5f};
    clip.frames.resize(clip.times.size() * clip.joints);
    const auto bytes = dingosdk::anim_studio::clip_bytes(clip);
    const std::string main = R"(
        local function told(what) return function(ok, why) reskate.log(what .. " " .. tostring(ok) .. " " .. tostring(why)) end end
        reskate.console.command("warm", "x", function()
            assert(reskate.animation.preload("clips/spin.fas", told("spin")))
            assert(reskate.audio.preload("sounds/beep.wav", told("beep")))
            assert(reskate.animation.preload("clips/spin.fas", told("again")))
            assert(reskate.animation.preload("notes.txt", told("notes")))
            assert(reskate.audio.preload("sounds/missing.wav", told("missing")))
            local ok, why = reskate.animation.preload("clips/other.fas")
            assert(not ok and why:find("5 clips and sounds"), tostring(why))
            assert(not pcall(reskate.animation.preload, "game:kickflip"), "a game clip isn't preloaded")
            assert(not pcall(reskate.audio.preload, "motor"), "nor the motor")
            assert(not pcall(reskate.animation.preload, "../other/spin.fas"), "its own package only")
            reskate.log("asked")
        end)
        reskate.console.command("use", "x", function()
            assert(reskate.animation.play("clips/spin.fas"))
            assert(reskate.audio.play("sounds/beep.wav"))
            reskate.log("used")
        end)
    )";
    host.apply({package("preloader", {{manifest_file, manifest(R"("console", "skater.animation", "audio")")}, {"plugin/main.lua", main},
                                      {"clips/spin.fas", std::string(bytes.begin(), bytes.end())}, {"sounds/beep.wav", wave_file(wave_rate, 1000)},
                                      {"notes.txt", "x"}})});
    running(host, "preloader", environment);
    host.run_console("warm");
    check(environment.logged("[preloader] asked") && !environment.logged("spin "), "Nothing is called back in the call that asks\n" + environment.all());
    const auto start = std::chrono::steady_clock::now();
    const auto all_told = [&] {
        for (const auto* line : {"spin ", "beep ", "again ", "notes ", "missing "})
            if (!environment.logged(line)) return false;
        return true;
    };
    while (!all_told() && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        ticks(host, environment, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(environment.logged("spin true nil") && environment.logged("beep true nil") && environment.logged("again true nil"),
          "Preloads are called back once held\n" + environment.all());
    check(environment.logged("notes false it is not one") && environment.logged("missing false it cannot be read"),
          "A preload that can't be held says why\n" + environment.all());
    const auto directory = scratch / "mods" / "preloader";
    std::filesystem::remove(directory / "clips" / "spin.fas");
    std::filesystem::remove(directory / "sounds" / "beep.wav");
    host.run_console("use");
    check(environment.logged("[preloader] used"), "Preloaded clips and sounds play without their files\n" + environment.all());
    running(host, "preloader", environment);

    host.apply({script("no-preload", R"(
        local ok, why = pcall(reskate.animation.preload, "clips/spin.fas")
        assert(not ok and why:find("skater.animation"), tostring(why))
        ok, why = pcall(reskate.audio.preload, "sounds/beep.wav")
        assert(not ok and why:find("\"audio\""), tostring(why))
        reskate.log("preloads refused")
    )")});
    running(host, "no-preload", environment);
    check(environment.logged("preloads refused"), "Without the permission a plugin can't preload\n" + environment.all());
}

void plugins_call_and_hear_each_other() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({package("library", {{manifest_file, manifest()}, {"manifest.json", R"({"name": "Lib"})"}, {"plugin/main.lua", R"(
        reskate.expose({
            add = function(a, b) return a + b end,
            echo = function(t) return {got = t, n = #t.list} end,
            fail = function() error("library failure") end,
            back = function() return reskate.require_plugin("user").poke() end,
        })
        reskate.on_plugin("user", "hello", function(from, data) reskate.log("heard " .. data.word .. " from " .. from) end)
        reskate.on_plugin("*", "anyone", function(from, data) reskate.log("anyone from " .. from) end)
    )"}}), script("user", R"(
        reskate.expose({poke = function() return 1 end})
        reskate.console.command("use", "x", function()
            local lib = assert(reskate.require_plugin("lib"), "by its manifest name")
            assert(lib.add(2, 3) == 5)
            local echoed = lib.echo({list = {1, 2, 3}, word = "hi"})
            assert(echoed.n == 3 and echoed.got.word == "hi")
            local ok, err = pcall(lib.fail)
            assert(not ok and err:find("raised an error"), err)
            assert(not pcall(lib.add, print, 1), "a function does not cross")
            ok, err = pcall(lib.back)
            assert(not ok, "a call back into a plugin in a call is refused")
            local none, why = reskate.require_plugin("nobody")
            assert(none == nil and why:find("no running plugin"), why)
            assert(reskate.emit("hello", {word = "hey"}))
            assert(reskate.emit("anyone"))
            assert(not pcall(reskate.emit, "bad name!"))
            reskate.log("library fine")
        end)
    )", R"("console")")});
    running(host, "library", environment);
    host.run_console("use");
    check(environment.logged("[user] library fine"), "Libraries work\n" + environment.all());
    running(host, "library", environment);
    ticks(host, environment, 1);
    check(environment.logged("[library] heard hey from user") && environment.logged("[library] anyone from user"), "Events reach the next tick\n" + environment.all());
}

void web_requests_go_only_to_listed_domains() {
    TestEnvironment environment;
    check([] {
        try { parse_manifest(R"({"api": 1, "entry": "a.lua", "domains": ["api.example.com"]})"); } catch (const std::exception&) { return true; }
        return false;
    }(), "domains need net.http");
    check([] {
        try { parse_manifest(R"({"api": 1, "entry": "a.lua", "permissions": ["net.http"], "domains": ["*.example.com"]})"); } catch (const std::exception&) { return true; }
        return false;
    }(), "and are plain host names");
    Host host(environment, Side::client);
    host.apply({package("web", {{manifest_file, R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["console", "net.http"], "domains": ["API.example.com"]})"},
        {"plugin/main.lua", R"(
        reskate.console.command("fetch", "x", function()
            assert(reskate.http.request({url = "https://api.example.com/v1/x?y=1"}, function(status, body, err)
                reskate.log("answer " .. status .. " " .. body .. " " .. tostring(err))
            end))
            assert(reskate.http.request({url = "https://api.example.com:443/post", method = "POST", body = "{}", content_type = "application/json"}, function() end))
            for _, bad in {"http://api.example.com/", "https://other.com/", "https://user@api.example.com/", "https://api.example.com:8080/",
                           "https://api.example.com/a b", "ftp://api.example.com/"} do
                assert(not pcall(reskate.http.request, {url = bad}, function() end), bad)
            end
            assert(not pcall(reskate.http.request, {url = "https://api.example.com/", method = "CONNECT"}, function() end))
            reskate.log("web fine")
        end)
        reskate.console.command("many", "x", function()
            local ok, err = pcall(function() for i = 1, 20 do reskate.http.request({url = "https://api.example.com/"}, function() end) end end)
            reskate.log("many " .. tostring(err):match("more than %d+ web requests [%a ]+"))
        end)
    )"}})});
    host.run_console("fetch");
    check(environment.logged("[web] web fine") && environment.requests.size() == 2, "Requests go out\n" + environment.all());
    check(environment.requests[0].host == "api.example.com" && environment.requests[0].path == "/v1/x?y=1" && environment.requests[1].method == "POST",
          "with their host, path and method");
    environment.responses.push_back({environment.requests[0].id, "web", 200, "hello", ""});
    ticks(host, environment, 1);
    check(environment.logged("[web] answer 200 hello nil"), "and the answer comes back\n" + environment.all());
    host.run_console("many");
    check(environment.logged("many more than 8 web requests waiting"), "8 waiting at most\n" + environment.all());
}


void preview_board_material_scope() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("preview_material", R"(
        assert(reskate.board.parameter_scopes()[2] == "preview")
        assert(#reskate.board.parameters("grip", "preview") == 4)
        assert(reskate.board.set_parameter("grip", "graphic", {r=.3, g=.4, b=.5}, "preview"))
        reskate.board.clear_parameter(nil, nil, "preview")
        assert(not pcall(reskate.board.parameters, "deck", "other"))
        assert(not pcall(reskate.board.set_parameter, "deck", "base", 1, "other"))
        assert(not pcall(reskate.board.clear_parameter, nil, nil, "other"))
        reskate.log("preview material routed")
    )", R"("board", "room")"), script("preview_denied", R"(
        local ok, why = pcall(reskate.board.parameters, "deck", "preview")
        assert(not ok and why:find('needs the "room" permission'), tostring(why))
        ok, why = pcall(reskate.board.set_parameter, "deck", "base", {r=0,g=0,b=0}, "preview")
        assert(not ok and why:find('needs the "room" permission'), tostring(why))
        ok, why = pcall(reskate.board.clear_parameter, nil, nil, "preview")
        assert(not ok and why:find('needs the "room" permission'), tostring(why))
        reskate.log("preview room permission enforced")
    )", R"("board")"), script("preview_no_board", R"(
        assert(not pcall(reskate.board.parameter_scopes))
        assert(not pcall(reskate.board.parameters, "deck", "preview"))
        assert(not pcall(reskate.board.set_parameter, "deck", "base", 0, "preview"))
        assert(not pcall(reskate.board.clear_parameter, nil, nil, "preview"))
        reskate.log("preview board permission enforced")
    )", R"("room")")});
    check(environment.logged("preview material routed") && environment.logged("preview room permission enforced") &&
          environment.logged("preview board permission enforced"), "Preview scope permissions\n" + environment.all());
    const auto& calls = environment.fake.calls;
    for (const auto* expected : {"preview parameters", "preview set", "preview clear"})
        check(std::count(calls.begin(), calls.end(), expected) == 1, "Preview scope dispatch once");
}

void declared_board_colours_through_the_api() {
    TestEnvironment environment;
    environment.fake.declared_rules = true;
    environment.fake.declared_deck = true;
    Host host(environment, Side::client);
    host.apply({script("materialapi", R"(
        reskate.console.command("materialapi", "x", function(args)
            local deck = reskate.board.parameters("deck")
            assert(#deck == 7 + 9, "seven selectors and nine declared inputs, not " .. #deck)
            local inherited = 0
            for i, p in deck do
                if i <= 7 then
                    assert(not p.inherited and (p.type == "integer" or p.type == "switch"), "a live selector")
                else
                    assert(p.inherited == true, "a declared input is inherited")
                    if p.type == "colour" then
                        inherited += 1
                        assert(p.value.r == 0.5 and p.value.g == 0.5 and p.value.b == 0.5, "at its declared default")
                    end
                end
            end
            assert(inherited == 8, "eight declared colours")
            for _, key in {"5b2d035c7746f31a", "51745c0fd2d117d3", "5c1058b72fe158ed", "5a4e08ac201c369f",
                           "57fc15cf6583c0c7", "5a2f52ddbd12f283", "55fda1e1521b9097", "580bde15aa576f12"} do
                assert(reskate.board.set_parameter("deck", key, {r = 0.25, g = 0.75, b = 0.375}), key)
            end
            local ok, why = reskate.board.set_parameter("deck", "0000000000005001", 3)
            assert(not ok and why:find("only read"), "a selector stays read-only: " .. tostring(why))
            ok, why = reskate.board.set_parameter("deck", "5b2d035c7746f31a", 0.5)
            assert(not ok and why:find("colour"), "a declared colour is not a number: " .. tostring(why))
            ok, why = reskate.board.set_parameter("deck", "5b2d035c7746f31a", {r = 2, g = 0, b = 0})
            assert(not ok and why:find("0 to 1"), "its channels are 0 to 1")
            ok, why = reskate.board.set_parameter("deck", "7777777777777777", 0.5)
            assert(not ok and why:find("only a declared colour"), "a declared number with no live value is not added: " .. tostring(why))
            ok, why = reskate.board.set_parameter("deck", "1234567812345678", {r = 0, g = 0, b = 0})
            assert(not ok and why:find("no parameter"), "nor anything undeclared")
            reskate.log("materialapi fine")
        end)
        reskate.console.command("materialapi_one", "x", function() reskate.board.clear_parameter("deck", "5b2d035c7746f31a") end)
    )", R"("board", "console")"),
                script("thief", R"(
        reskate.console.command("thief", "x", function()
            local ok, why = reskate.board.set_parameter("deck", "5b2d035c7746f31a", {r = 1, g = 1, b = 1})
            assert(not ok and why:find("materialapi holds"), "another plugin's colour is not taken: " .. tostring(why))
            reskate.log("thief refused")
        end)
    )", R"("board", "console")"),
                script("nopermission", R"(
        local ok, err = pcall(reskate.board.parameters, "deck")
        assert(not ok and err:find("needs the \"board\" permission"), err)
        ok, err = pcall(reskate.board.set_parameter, "deck", "5b2d035c7746f31a", {r = 1, g = 1, b = 1})
        assert(not ok and err:find("needs the \"board\" permission"), err)
        ok, err = pcall(reskate.board.clear_parameter, "deck")
        assert(not ok and err:find("needs the \"board\" permission"), err)
        reskate.log("nopermission refused")
    )")});
    running(host, "materialapi", environment);
    running(host, "nopermission", environment);
    check(environment.logged("nopermission refused"), "A plugin without the board permission reaches no board parameter\n" + environment.all());
    check(environment.fake.declared_holds.empty(), "Nothing is held before a plugin asks");
    check(host.run_console("materialapi") && environment.logged("materialapi fine"), "Declared colours list and set\n" + environment.all());
    check(environment.fake.declared_held("materialapi") == 8, "The plugin holds the eight declared colours");
    check(host.run_console("thief") && environment.logged("thief refused"), "Another plugin cannot take them\n" + environment.all());
    check(environment.fake.declared_held("thief") == 0, "and holds none");
    check(host.run_console("materialapi_one") && environment.fake.declared_held("materialapi") == 7, "Clearing one key lets go of that one");
    host.apply({});
    check(environment.fake.declared_holds.empty(), "Unloading lets go of every one (the game puts them back to their defaults)");
}

void game_menu_distinguishes_unknown_from_closed() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    host.apply({script("menu-reader", R"(
        local function snapshot(known, open, skaters, count)
            local row = reskate.menu.game()
            local fields = 0
            for key in pairs(row) do
                assert(key == "known" or key == "open" or key == "skaters" or key == "buttons", key)
                fields += 1
            end
            assert(fields == 4 and type(row.known) == "boolean" and type(row.open) == "boolean"
                and type(row.skaters) == "boolean" and type(row.buttons) == "table")
            assert(row.known == known and row.open == open and row.skaters == skaters and #row.buttons == count)
            return row
        end
        reskate.console.command("menu_unknown", "x", function()
            snapshot(false, false, false, 0)
            reskate.log("unknown menu preserved")
        end)
        reskate.console.command("menu_closed", "x", function()
            snapshot(true, false, false, 0)
            reskate.log("known closed menu preserved")
        end)
        reskate.console.command("menu_open", "x", function()
            local row = snapshot(true, true, true, 2)
            assert(row.buttons[1] == "Skaters" and row.buttons[2] == "Board")
            reskate.log("known open menu preserved")
        end)
        reskate.console.command("menu_buttons", "x", function()
            local row = snapshot(true, true, false, 64)
            assert(row.buttons[1] == "1" and row.buttons[64] == "64")
            reskate.log("menu button bound preserved")
        end)
    )", R"("menu", "console")"), script("menu-denied", R"(
        local ok, why = pcall(reskate.menu.game)
        assert(not ok and why:find('needs the "menu" permission', 1, true), tostring(why))
        reskate.log("menu permission denied")
    )")});
    running(host, "menu-reader", environment);
    running(host, "menu-denied", environment);
    check(environment.logged("menu permission denied") && game.menu_reads == 0,
          "Without menu permission the backend is not entered\n" + environment.all());
    check(host.run_console("menu_unknown") && environment.logged("unknown menu preserved"),
          "An unknown menu retains every old field\n" + environment.all());
    game.menu_now = {false, true, {}, true};
    check(host.run_console("menu_closed") && environment.logged("known closed menu preserved"),
          "A known closed menu still suppresses Skaters\n" + environment.all());
    game.menu_now = {true, true, {"Skaters", "Board"}, true};
    check(host.run_console("menu_open") && environment.logged("known open menu preserved"),
          "A known open menu retains the page and button order\n" + environment.all());
    game.menu_now = {true, false, {}, true};
    for (unsigned i = 1; i <= 65; ++i) game.menu_now.buttons.push_back(std::to_string(i));
    check(host.run_console("menu_buttons") && environment.logged("menu button bound preserved") && game.menu_reads == 4,
          "The old button limit and call count remain unchanged\n" + environment.all());
}

void the_wider_set_reaches_the_game() {
    TestEnvironment environment;
    auto& game = environment.fake;
    environment.local_skater = SkaterState{{1, 0, 1}, 0.25f, 3, 0, false, 0};
    Host host(environment, Side::client);
    host.apply({script("wide", R"(
        reskate.on("skater_state", function(now, before) reskate.log("state " .. before .. " to " .. now) end)
        reskate.on("bail", function(e) reskate.log("bail " .. e.count .. " force " .. tostring(e.force) .. " cause " .. tostring(e.cause)) end)
        reskate.on("board_landed", function(e) reskate.log(string.format("landed %.1f onto %s", e.down_speed, e.onto)) end)
        reskate.on("body_hit", function(e) reskate.log(string.format("hit %s %.0f", e.bone, e.impact)) end)
        reskate.on("bail_end", function(e) reskate.log(string.format("fall %.1f s left %.0f thigh %.0f", e.seconds, e.left, e.bones["Left thigh"].impact)) end)
        reskate.console.command("outfit", "x", function()
            assert(reskate.outfit.find("roadrash_leg1") == "RoadRash_Leg1" and reskate.outfit.find("nothing") == nil)
            local slots = reskate.outfit.slots()
            assert(#slots == 2 and slots[1].slot == "tattoo_legl" and slots[1].parameters[1] == 0.5)
            assert(reskate.outfit.set("tattoo_legl", "RoadRash_Leg1", {placement = {0.1, 0.2, 0.3, 0.4, 0.5}, covers = true}))
            assert(not pcall(reskate.outfit.set, "tattoo_legl", "x", {placement = {1, 2, 3, 4, 5, 6, 7, 8, 9}}), "at most 8 numbers")
            reskate.outfit.clear("tattoo_legl")
            reskate.log("outfit fine")
        end)
        reskate.console.command("deck", "x", function()
            local bones = reskate.board.bones()
            assert(math.abs(bones[3].position.y - 0.1) < 1e-6 and bones[0] and bones[15] and #bones[3].rotation == 4, "the board's bones")
            assert(reskate.board.set(12, {position = vector.create(0, 2, 0), rotation = {0, 0, 0, 2}, space = "world"}))
            assert(not pcall(reskate.board.set, 1, {position = vector.create(0, 0, 0)}), "bone 1 places the board")
            assert(reskate.board.hide(3))
            -- The whole board somewhere: held out in front of the skater, turned on its side, as a plugin that
            -- has him carry it would; in the world, halfway there; and let go.
            assert(reskate.board.mount({position = vector.create(0, 1.1, 0.4), rotation = {0, 0, 2, 0}, relative_to = "skater"}))
            assert(reskate.board.mount({position = vector.create(5, -69, 2)}, 0.5))
            assert(reskate.board.mount(nil))
            -- A board laid on a stand with its front truck turned: a bone under the mount placed with it.
            assert(reskate.board.mount({position = vector.create(1, 2, 3), bones = {[4] = {rotation = {0, 2, 0, 0}}, [9] = {position = vector.create(0, -0.05, 0.2)}}}))
            assert(reskate.board.mount(nil))
            assert(not pcall(reskate.board.mount, {position = vector.create(0, 0, 0), bones = {[2] = {rotation = {0, 0, 0, 1}}}}), "bones 3 to 15")
            assert(not pcall(reskate.board.mount, {position = vector.create(0, 0, 0), bones = {[16] = {rotation = {0, 0, 0, 1}}}}), "bones 3 to 15")
            assert(not pcall(reskate.board.mount, {position = vector.create(0, 0, 0), bones = {[4] = {}}}), "a position or a rotation")
            assert(not pcall(reskate.board.mount, {position = vector.create(0, 0, 0), bones = {[4] = {position = vector.create(0, 5, 0)}}}), "within 2 m")
            assert(not pcall(reskate.board.mount, {position = vector.create(0, 0, 0), relative_to = "moon"}), "the world or the skater")
            assert(not pcall(reskate.board.mount, {position = vector.create(0, 30, 0), relative_to = "skater"}), "within 20 m of him")
            assert(not pcall(reskate.board.mount, {position = vector.create(0, 0, 0)}, 2), "a weight is 0 to 1")
            assert(not pcall(reskate.board.mount, {rotation = {0, 0, 0, 1}}), "a mount has a position")
            reskate.board.park({12, 13, 14, 15})
            assert(reskate.board.scale(6, 1.5) and reskate.board.scale(7, vector.create(1, 2, 3)) and reskate.board.scale(10))
            assert(not pcall(reskate.board.scale, 1, 2), "bone 1 places the board")
            assert(not pcall(reskate.board.scale, 6, "big"), "a size is a number or a vector")
            assert(reskate.board.toss({12, 4}, {velocity = vector.create(1, 3, 0), spin = vector.create(0, 5, 0), ground = -2}))
            local grip = reskate.board.parameters("grip")
            assert(#grip == 4 and grip[1].key == "873f48af36e50d59" and grip[1].name == "graphic" and grip[1].type == "colour", "the grip's parameters")
            assert(math.abs(grip[1].value.g - 0.3) < 1e-6 and grip[2].value == 1 and grip[2].name == nil, "a colour and a number")
            assert(grip[3].type == "integer" and grip[3].value == 13 and grip[4].type == "switch" and grip[4].value == true, "an integer and a switch")
            assert(#reskate.board.parameters("wheels") == 0)
            assert(reskate.board.set_parameter("grip", "graphic", {r = 1, g = 0, b = 0}))
            assert(reskate.board.set_parameter("deck", "841C7D74837F1D39", 2.5))
            assert(not pcall(reskate.board.parameters, "rail"), "a part is one of four")
            assert(not pcall(reskate.board.set_parameter, "grip", "123", 1), "a key is 16 hex digits")
            assert(not pcall(reskate.board.set_parameter, "grip", "graphic", {r = 1}), "a colour has three channels")
            assert(not pcall(reskate.board.set_parameter, "grip", "graphic", math.huge), "a value is finite")
            reskate.board.clear_parameter("grip", "base")
            reskate.board.clear_parameter()
            reskate.log("deck fine")
        end)
        reskate.console.command("physics", "x", function()
            assert(reskate.board.motion().velocity.y == -2)
            local m = reskate.board.motion()
            assert(m.spin.y == 4, "the board's spin")
            assert(m.place.position.y == 0 and m.place.rotation[4] == 1, "where the board is")
            assert(math.abs(m.deck.position.y - 0.1) < 1e-6, "and its deck, on the board's place")
            assert(reskate.skater.wipeout(2))
            local again, two = reskate.skater.wipeout(2)
            assert(not again and two:find("four wipeouts a second"), "four wipeouts a second at most")
            assert(not pcall(reskate.skater.wipeout, 10), "a gentle force")
            assert(reskate.skater.skydive(), "the game's skydive")
            assert(reskate.skater.skydive(), "asked every tick: no limit of its own")
            reskate.log("physics fine")
        end)
        reskate.on("chat", function(line) reskate.log("chat " .. line.name .. ": " .. line.text) end)
        reskate.on("track_changed", function(t) reskate.log("track " .. t.title .. " by " .. t.artist) end)
        reskate.on("throwdown", function(e) reskate.log("throwdown " .. e.phase) end)
        reskate.on("action", function(name) reskate.log("action " .. name) end)
        reskate.on("screen_closed", function(title) reskate.log("closed " .. title) end)
        assert(reskate.input.bind("jump", "Ctrl+K"))
        assert(not pcall(reskate.input.bind, "bad", "Esc"), "Esc is not bindable")
        reskate.ui.screen("Big", function(ui) ui:heading("A screen") if ui:button("ok", "OK") then reskate.log("screen ok") end end)
        reskate.console.command("do", "x", function()
            assert(reskate.skater.state() == "riding")
            assert(reskate.skater.noclip(true) and reskate.skater.boost("up"))
            assert(not pcall(reskate.skater.boost, "sideways"))
            assert(reskate.trainer.set_point() and reskate.trainer.back())
            reskate.hud.hide(true)
            assert(#reskate.objects.catalog() == 1 and reskate.objects.list()[1].id == "42")
            assert(reskate.objects.place("bench_01", vector.create(5, 0, 5), 1.0))
            assert(reskate.objects.move("42", vector.create(6, 0, 6)))
            assert(reskate.objects.remove("42"))
            assert(not pcall(reskate.objects.remove, "not a number"))
            assert(reskate.world.set_time("night") and reskate.world.set_control("fog", 1) and reskate.world.set_park("historic", "skatepark_02"))
            local controls = reskate.world.controls()
            assert(controls and controls.fog == 0 and controls.fog_distance == 1.5 and controls.clouds == -1, "the world controls as they are")
            local ok, why = reskate.world.set_time("never")
            assert(not ok and why == "refused")
            assert(#reskate.travel.locations() == 1 and #reskate.travel.levels() == 1)
            assert(reskate.travel.go("bam"))
            local again, wait = reskate.travel.load("Levels/Game/X/X")
            assert(not again and wait:find("5 s"), "one trip every 5 s")
            assert(reskate.ai.spawn(2) and reskate.ai.count() == 3 and reskate.ai.clear())
            assert(not pcall(reskate.ai.spawn, 20))
            local s = reskate.session.skaters()[1]
            assert(s.id == "7" and s.position.y == 2 and s.heading == 0.5)
            reskate.session.tag("7", "IT", 0xff0000ff)
            assert(reskate.voice.set_volume("7", 0))
            assert(reskate.chat.send("hello all"))
            local soon, three = reskate.chat.send("again")
            assert(not soon and three:find("3 s"))
            assert(not pcall(reskate.chat.send, "/kick someone"), "no commands")
            for i = 1, 5 do assert(reskate.chat.notice("note " .. i, 0x00ff00ff)) end
            local sixth, five = reskate.chat.notice("too many")
            assert(not sixth and five:find("5 lines"), "5 lines a second")
            assert(not pcall(reskate.chat.notice, "two\nlines"), "one line")
            assert(reskate.camera.look(vector.create(0, 5, 0), vector.create(0, 5, 10), 70))
            assert(not pcall(reskate.camera.look, vector.create(1, 1, 1), vector.create(1, 1, 1)), "a view needs a direction")
            assert(not pcall(reskate.camera.look, vector.create(0, 0, 0), vector.create(0, 0, 1), 400), "a sane field of view")
            assert(reskate.camera.get().fov == 60 and reskate.camera.get().position.y == 2)
            assert(reskate.camera.set(vector.create(0, 5, 0), vector.create(0, 0, -1), vector.create(1, 1, 0), 80), "a rolled view")
            assert(not pcall(reskate.camera.set, vector.create(0, 0, 0), vector.create(0, 1, 0), vector.create(0, 2, 0)), "up along forward")
            local f = reskate.input.flight()
            assert(f.forward == 0.5 and f.look_x == 3 and f.boost, "the flight keys and mouse")
            local pad = reskate.input.gamepad()
            assert(pad.buttons.a and not pad.buttons.b and pad.left_x == 0.25 and pad.right_trigger == 1, "the gamepad")
            local stick = reskate.input.joystick()
            assert(stick.axes[2] == -1 and stick.buttons[3] and not stick.buttons[1] and stick.name == "Radio", "the radio")
            assert(#stick.axes == 8 and stick.axes[7] == 0.25 and stick.axes[8] == -0.75, "a radio's channels 7 and 8")
            assert(stick.device == 0, "the radio's number")
            local list = reskate.input.joysticks()
            assert(#list == 2 and list[2].device == 1 and list[2].name == "Pad", "every controller")
            assert(reskate.input.joystick(1).name == "Pad" and reskate.input.joystick(5) == nil, "a chosen controller")
            assert(not pcall(reskate.input.joystick, -1) and not pcall(reskate.input.joystick, 1.5), "a controller's number")
            assert(reskate.input.capture(true))
            reskate.camera.release()
            assert(reskate.skater.freeze(true))
            assert(reskate.throwdowns.state().active == false)
            assert(reskate.throwdowns.host("skate", {"7", {id = "8"}, "7"}, {max = 4}))
            local again, five = reskate.throwdowns.host("jam", {"7"})
            assert(not again and five:find("5 s"), "one drop every 5 s")
            assert(not pcall(reskate.throwdowns.host, "bowling", {"7"}), "a linked mode")
            assert(not pcall(reskate.throwdowns.host, "jam", {}), "someone to play")
            assert(reskate.throwdowns.hosting() == "open")
            assert(reskate.music.next())
            local p = reskate.progression.get()
            assert(p.districts[1].rank == 4 and p.challenges[1].completed_goals == 1 and p.score == 1500)
            assert(reskate.settings.game("Render.Fov") == "90" and reskate.settings.game("Nope") == nil and #reskate.settings.names() == 1)
            reskate.ui.open("Big")
            reskate.log("wide fine")
        end)
    )", R"("console", "events.skater", "skater.read", "skater.control", "trainer", "hud", "world.objects", "world.environment", "travel",
          "world.ai", "session.skaters", "session.nametags", "voice.control", "chat.read", "chat.send", "chat.local", "camera", "throwdowns", "music", "audio", "outfit", "board",
          "progression.read", "settings.read", "input", "ui.screen")")});
    running(host, "wide", environment);
    game.state = 100;
    game.lines = {{1, 5, "Old", "before", false, false}};
    ticks(host, environment, 1);
    check(environment.logged("[wide] state none to riding"), "The skater's state is heard\n" + environment.all());
    host.run_console("do");
    check(environment.logged("[wide] wide fine"), "Every wider-set call works\n" + environment.all());
    for (const auto* call : {"control noclip_on", "control boost_up", "hud wide on", "place 0 bench_01 5.0", "move 42  6.0", "delete 42",
                             "env tod night", "env control fog 1", "env park historic skatepark_02", "travel location bam", "ai spawn 2",
                             "ai clear", "tag wide 7 IT ff0000ff", "voice 7 0.0", "chat hello all", "notice wide note 5 ff00ff00", "camera wide 0.0 5.0 0.0 back 0.00 0.00 -1.00 fov 70",
                             "camera wide 0.0 5.0 0.0 back -0.00 -0.00 1.00 fov 80", "capture wide on",
                             "camera release wide", "freeze wide on", "host wide ThrowdownSkate 2 players max 4", "music next"})
        check(game.called(call), std::string("The game was asked: ") + call);
    check(environment.teleports.size() == 1, "The save point teleports back");
    check(game.binds["wide/jump"] == combo_of("Ctrl+K"), "Its bind is held by the game");
    game.state = 401;
    game.wipe = 1;
    game.lines.push_back({2, 6, "Ann", "hi there", false, false});
    game.now_playing = {true, true, "Spotify", "Song", "Band", "Album", 9};
    game.throwdown.active = true;
    game.presses = {{"wide", "jump"}, {"someone", "jump"}};
    environment.now += 1000; // the music is looked at once a second
    ticks(host, environment, 1);
    check(environment.logged("[wide] state riding to grinding") && environment.logged("[wide] chat Ann: hi there") && !environment.logged("before") &&
          environment.logged("[wide] track Song by Band") && environment.logged("[wide] throwdown started") && environment.logged("[wide] action jump"),
          "And its events, but not chat from before it listened\n" + environment.all());
    ticks(host, environment, 1);
    game.wipe = 2;
    game.news.landings.push_back({4.5f, 6, true});
    game.news.hits.push_back({"Head", 9, false, false});
    game.news.falls.push_back({1.5f, 12, 3, {{"Left thigh", 8, 0.5f}}});
    ticks(host, environment, 1);
    check(environment.logged("[wide] bail 2 force 7.5 cause 12"), "A wipeout is a bail, with the slam's force and cause\n" + environment.all());
    check(environment.logged("[wide] landed 4.5 onto rail") && environment.logged("[wide] hit Head 9") &&
              environment.logged("[wide] fall 1.5 s left 12 thigh 8"),
          "and the board's landings, the body's hits and the fall are heard\n" + environment.all());
    host.run_console("outfit");
    check(environment.logged("[wide] outfit fine") && game.called("outfit wide tattoo_legl RoadRash_Leg1 5 true") &&
              game.called("outfit clear wide tattoo_legl"),
          "A plugin wears items over the outfit\n" + environment.all());
    host.run_console("deck");
    check(environment.logged("[wide] deck fine") && game.called("board wide 12 2.0 world") && game.called("board hide wide 3") &&
              game.called("board park wide 4") && game.called("board toss wide 2 3 -2") && game.called("board scale wide 6 1.5 1.5 1.5") &&
              game.called("board scale wide 7 1.0 2.0 3.0") && game.called("board scale wide 10 none"),
          "A plugin changes what is drawn of the board\n" + environment.all());
    check(game.called("board mount wide 0.00,1.10,0.40 r0.00,0.00,1.00,0.00 skater 1.00") &&
              game.called("board mount wide 5.00,-69.00,2.00 r0.00,0.00,0.00,1.00 world 0.50") && game.called("board mount wide none") &&
              game.called("board mount bone 4 held r0.00,1.00,0.00,0.00") && game.called("board mount bone 9 at 0.00,-0.05,0.20"),
          "The whole board placed under the skater, in the world at a weight, and let go\n" + environment.all());
    check(game.called("board parameter wide 1 873f48af36e50d59 colour 1.0") && game.called("board parameter wide 0 841c7d74837f1d39 number 2.5") &&
              game.called("board parameter clear wide 1 8c46cd354b18a9e2") && game.called("board parameter clear wide -1 all"),
          "and holds its shader parameters\n" + environment.all());
    host.run_console("physics");
    check(environment.logged("[wide] physics fine") && game.called("wipeout 2.0"), "The board's motion reads, and a wipeout is asked for\n" + environment.all());
    check(game.called("skydive"), "The game's skydive is asked for\n" + environment.all());
    ticks(host, environment, 10);
    const auto screen = std::find_if(environment.panels.begin(), environment.panels.end(), [](const auto& p) { return p.screen; });
    check(screen != environment.panels.end() && screen->title == "Big" && screen->widgets.size() == 2, "Its open screen is built, without the menu");
    environment.actions = {plugin_ui::Action{"wide", "Big", "ok"}};
    ticks(host, environment, 10);
    check(environment.logged("[wide] screen ok"), "and answers its widgets");
    environment.actions = {plugin_ui::Action{"wide", "Big", plugin_ui::close_widget}};
    ticks(host, environment, 10);
    check(environment.logged("[wide] closed Big") && std::none_of(environment.panels.begin(), environment.panels.end(), [](const auto& p) { return p.screen; }),
          "Esc closes it, which it hears");
    host.apply({});
    check(std::find(game.released.begin(), game.released.end(), "wide") != game.released.end(), "Unloading gives back what it held of the game");
}

void key_names_become_binds() {
    check(combo_of("F7") == (0x80000000u | 0x76u), "F7");
    check(combo_of("k") == (0x80000000u | 'K'), "a letter, any case");
    check(combo_of("Ctrl+K") != 0 && (combo_of("Ctrl+K") & 0xc0000000u) == 0x40000000u, "a chord");
    check(combo_of("Ctrl+K") == combo_of("k+ctrl"), "in any order");
    check(combo_of("Pad.LB+Pad.RB") == 0x300u && combo_of("pad.a") == 0x1000u && combo_of("Pad.RT") == 0x20000u, "pad combos");
    for (const auto* bad : {"", "Esc", "F13", "Ctrl+", "Pad.Z", "Ctrl+Pad.A", "A+B+C+D+E+F", "Num1"})
        check(combo_of(bad) == 0, std::string("not a bind: ") + bad);
}

void level_events_reach_plugins() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("levels", R"(
        assert(reskate.level.current() == nil)
        reskate.on("level_loaded", function(name) reskate.log("loaded " .. name .. " / " .. reskate.level.current().name) end)
        reskate.on("level_unloading", function(name) reskate.log("leaving " .. name) end)
    )")});
    host.set_level("sanvan");
    host.set_level("park");
    host.set_level("");
    check(environment.logged("loaded sanvan / sanvan") && environment.logged("leaving sanvan") &&
          environment.logged("loaded park / park") && environment.logged("leaving park"), "Level events in order\n" + environment.all());
}

// The pause menu, ReSkate's menu (its window or one of its pages in the pause menu) and the replay
// editor: read with menu.state, and heard as they come and go, once each change. A plugin without
// the menu permission gets neither.
void plugins_know_what_menu_is_up() {
    TestEnvironment environment;
    auto& game = environment.fake;
    game.menu_state_now = {.pause = true, .known = true}; // already up when the plugin loads
    Host host(environment, Side::client);
    host.apply({script("menus", R"(
        local now = reskate.menu.state()
        reskate.log(string.format("at load known %s pause %s reskate %s replay %s page %s", tostring(now.known), tostring(now.pause),
            tostring(now.reskate), tostring(now.replay_editor), tostring(now.page)))
        assert(reskate.Enums.MenuPage.mod_manager == "mod_manager")
        local function shown(s)
            local up = {}
            if not s.known then table.insert(up, "unknown") end
            if s.pause then table.insert(up, "pause") end
            if s.reskate then table.insert(up, "reskate") end
            if s.replay_editor then table.insert(up, "replay") end
            if s.page then table.insert(up, s.page) end
            return #up > 0 and table.concat(up, "+") or "nothing"
        end
        reskate.on("menu_changed", function(state, previous) reskate.log("menu " .. shown(previous) .. " to " .. shown(state)) end)
    )", R"("menu")"), script("blind", R"(
        local ok, err = pcall(reskate.menu.state)
        assert(not ok and err:find("needs the \"menu\" permission"), err)
        ok, err = pcall(reskate.on, "menu_changed", function() end)
        assert(not ok and err:find("needs the \"menu\" permission"), err)
        reskate.log("blind refused")
    )")});
    running(host, "menus", environment);
    running(host, "blind", environment);
    check(environment.logged("at load known true pause true reskate false replay false page nil") && environment.logged("blind refused"),
          "The state reads at load, and not without the permission\n" + environment.all());
    ticks(host, environment, 3);
    check(!environment.logged("[menus] menu "), "What was up before is not news\n" + environment.all());

    game.menu_state_now = {.pause = true, .reskate = true, .page = "mod_manager", .known = true};
    ticks(host, environment, 3);
    game.menu_state_now = {.known = true};
    ticks(host, environment, 1);
    game.menu_state_now.reskate = true; // the windowed menu, over the game
    ticks(host, environment, 1);
    game.menu_state_now = {.replay_editor = true, .known = true};
    ticks(host, environment, 2);
    game.menu_state_now.known = false; // the game's screen model is lost: a closed pause menu is no longer known
    ticks(host, environment, 1);
    check(environment.logged("[menus] menu pause to pause+reskate+mod_manager") &&
              environment.logged("[menus] menu pause+reskate+mod_manager to nothing") &&
              environment.logged("[menus] menu nothing to reskate") && environment.logged("[menus] menu reskate to replay") &&
              environment.logged("[menus] menu replay to unknown+replay"),
          "Each change is heard with what was up before\n" + environment.all());
    const auto heard = std::count_if(environment.logs.begin(), environment.logs.end(),
                                     [](const std::string& line) { return line.find("[menus] menu ") != std::string::npos; });
    check(heard == 5, std::format("Once each, not every tick ({} heard)\n", heard) + environment.all());
}

void timers_run_when_due() {
    TestEnvironment environment;
    Host host(environment, Side::client);
    host.apply({script("clock", R"(
        local count = 0
        reskate.time.after(100, function() reskate.log("once") end)
        local id = reskate.time.every(50, function() count += 1 end)
        reskate.time.after(260, function() reskate.time.cancel(id) reskate.log("every ran " .. count) end)
        local cancelled = reskate.time.after(10, function() reskate.log("never") end)
        reskate.time.cancel(cancelled)
    )")});
    ticks(host, environment, 30);
    check(environment.logged("[clock] once") && environment.logged("every ran 5") && !environment.logged("never"),
        "Timers run once, repeat, and stop when cancelled\n" + environment.all());
}

void a_package_with_a_program_or_on_the_wrong_side_is_not_loaded() {
    const auto clean = package("clean", {{manifest_file, manifest()}, {"plugin/main.lua", ""}, {"readme.txt", "A plain file"}});
    check(check_package_files(clean.directory).empty(), "A plain package passes");
    const auto named = package("named", {{manifest_file, manifest()}, {"plugin/main.lua", ""}, {"tools/helper.DLL", "x"}});
    check(check_package_files(named.directory).find("tools/helper.dll") != std::string::npos, "A DLL by its name is refused");
    const auto hidden = package("hidden", {{manifest_file, manifest()}, {"plugin/main.lua", ""}, {"data/picture.png", "MZ\x90\x00"}});
    check(!check_package_files(hidden.directory).empty(), "and a program by its first bytes, whatever its name");
    const auto script_file = package("batch", {{manifest_file, manifest()}, {"plugin/main.lua", ""}, {"run.bat", "echo"}});
    check(!check_package_files(script_file.directory).empty(), "and a script Windows runs");

    TestEnvironment environment;
    Host host(environment, Side::client);
    auto refused = hidden;
    refused.refusal = check_package_files(hidden.directory);
    host.apply({refused, package("server_only", {{manifest_file, manifest("", "server")}, {"plugin/main.lua", ""}}),
        package("broken", {{manifest_file, "{"}, {"plugin/main.lua", ""}}),
        package("syntax", {{manifest_file, manifest()}, {"plugin/main.lua", "local = 1"}})});
    stopped(host, "hidden", "a program");
    check(status_of(host, "server_only").state == "not loaded", "A server plugin waits for a server");
    stopped(host, "broken", "not valid JSON");
    stopped(host, "syntax", "plugin/main.lua");
}

void discovery_finds_the_plugins_among_the_mods_left_on() {
    const auto root = scratch / "game";
    const auto mods = root / "Mods";
    write(mods / "Alpha" / manifest_file, manifest());
    write(mods / "Alpha" / "plugin" / "main.lua", "");
    write(mods / "Alpha" / "manifest.json", R"({"name":"Alpha Pack","author":"Ada","version_number":"1.4.2"})");
    write(mods / "Beta" / manifest_file, manifest());
    write(mods / "Beta" / "plugin" / "main.lua", "");
    write(mods / "Beta" / "bin" / "tool.exe", "x");
    write(mods / "Gamma" / manifest_file, manifest());
    write(mods / "Gamma" / "plugin" / "main.lua", "");
    write(mods / "JustAMap" / "manifest.json", R"({"name": "Just a map"})");
    check(dingosdk::mods::set_mod_enabled(mods, "Gamma", false), "mods.json switches a mod off");
    FileChecks checks;
    std::vector<ModInfo> listed;
    auto found = discover(root, checks, &listed);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    check(found.size() == 2 && found[0].id == "Alpha" && found[1].id == "Beta", "Only the plugins left on are found");
    const auto alpha = std::find_if(listed.begin(), listed.end(), [](const ModInfo& mod) { return mod.folder == "Alpha"; });
    check(alpha != listed.end() && alpha->title == "Alpha Pack" && alpha->author == "Ada" && alpha->version == "1.4.2",
          "A mod's manifest author and version are what reskate.mods.list reports");
    check(found[0].refusal.empty() && found[1].refusal.find("bin/tool.exe") != std::string::npos, "with what their files say");
    const auto before = found[0].fingerprint;
    write(mods / "Alpha" / "plugin" / "main.lua", "-- changed");
    auto again = discover(root, checks);
    std::sort(again.begin(), again.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    check(again[0].fingerprint != before, "A changed file changes the fingerprint, which reloads it");
}

// A plugin a dedicated server brought here is the server's: it runs only on servers that require it.
void a_servers_plugin_runs_only_on_servers_that_require_it() {
    namespace scope = server_scope;
    const auto mods = scratch / "server-scope-game" / "Mods";
    std::filesystem::create_directories(mods);
    check(scope::read(mods).empty(), "No plugin is a server's before one brings any");
    check(scope::set(mods, "Team-Drone", true) && scope::set(mods, "neekgate-FPV_Drone", true) && scope::set(mods, "Team-Drone", true),
          "A plugin a server brought is marked as the server's");
    const auto marked = scope::read(mods);
    check(marked == scope::Folders{"team-drone", "neekgate-fpv_drone"}, "The mark is kept by mod folder, whatever its case, once each");

    // The player's own plugins run everywhere, in a session or not.
    check(scope::runs(marked, {}, "Team-Filmer", "Filmer", "Team") && scope::runs(marked, {"Team-Drone"}, "Team-Filmer", "Filmer", "Team"),
          "A plugin of the player's own was held back");
    // A server's own: nowhere but on a server that requires it.
    check(!scope::runs(marked, {}, "Team-Drone", "Drone", "Team"), "A server's plugin ran outside any server");
    check(!scope::runs(marked, {"Team-Filmer", "Other-Drone"}, "Team-Drone", "Drone", "Team"),
          "A server's plugin ran on a server that does not require it");
    check(scope::runs(marked, {"Team-Filmer", "team-drone"}, "Team-Drone", "Drone", "Team") &&
              scope::runs(marked, {"Team-Drone-1.2.0"}, "TEAM-DRONE", "", ""),
          "A server's plugin did not run on a server that requires it");
    // A package is the mod in its folder, or the one with its name and author (map_download.h).
    check(scope::names("neekgate-FPV_Drone-2.0.1", "neekgate-FPV_Drone", "", "") && scope::names("neekgate-FPV_Drone", "drone-dev", "FPV_Drone", "Neekgate"),
          "A required package did not name its plugin");
    check(!scope::names("neekgate-FPV_Drone", "drone-dev", "FPV_Drone", "") && !scope::names("neekgate-FPV_Drone", "neekgate-FPV", "FPV", "other") &&
              !scope::names("", "", "", ""),
          "A required package named a plugin that is not its own");
    check(scope::runs(marked, {"neekgate-FPV_Drone"}, "neekgate-FPV_Drone", "FPV Drone", "neekgate"), "A marked plugin is known by its folder");

    // The player makes one their own; a mod that is taken out is forgotten.
    check(scope::set(mods, "TEAM-DRONE", false) && scope::read(mods) == scope::Folders{"neekgate-fpv_drone"}, "A plugin made the player's own stayed the server's");
    check(scope::runs(scope::read(mods), {}, "Team-Drone", "Drone", "Team"), "and runs everywhere again");
    scope::keep_only(mods, {"Neekgate-FPV_Drone", "Team-Drone"});
    check(scope::read(mods).size() == 1, "A plugin still in Mods was forgotten");
    scope::keep_only(mods, {"Team-Drone"});
    check(scope::read(mods).empty() && !std::filesystem::exists(mods / scope::file_name), "A plugin taken out of Mods is forgotten, and the file with the last");

    // A file that is not the list marks nothing, so every plugin runs as before.
    write(mods / scope::file_name, R"({"Team-Drone": true})");
    check(scope::read(mods).empty(), "A file that is no list marked a plugin");
    write(mods / scope::file_name, "not json");
    check(scope::read(mods).empty() && scope::set(mods, "Team-Drone", true) && scope::read(mods) == scope::Folders{"team-drone"},
          "A broken file is replaced by the next mark");
}

void discovery_can_recheck_sources_when_mod_metadata_is_unchanged() {
    const auto root = scratch / "same-metadata-game";
    const auto mods = root / "Mods";
    const auto directory = mods / "Snapshot";
    const auto source = directory / "plugin" / "main.lua";
    write(directory / manifest_file, manifest());
    write(source, "old-code");
    check(mods::set_mod_enabled(mods, "Snapshot", true), "mods.json records the plugin as enabled");
    FileChecks checks;
    const auto found = discover(root, checks);
    check(found.size() == 1 && found.front().sources, "Discovery captures the source files with the candidate");
    const auto old = found.front();
    const auto timestamp = std::filesystem::last_write_time(source);
    write(source, "new-code");
    std::filesystem::last_write_time(source, timestamp);
    check(mods::mod_fingerprint(directory) == old.fingerprint, "Same-size bytes with the old timestamp keep the mod metadata fingerprint");
    const auto cached = discover(root, checks);
    check(cached.front().plugin_fingerprint == old.plugin_fingerprint && cached.front().sources == old.sources,
          "The normal discovery cache keeps its already-approved snapshot");
    checks.forget_plugin(old);
    const auto refreshed = discover(root, checks);
    check(refreshed.front().plugin_fingerprint != old.plugin_fingerprint &&
              refreshed.front().sources->at(plugin_source_key("plugin/main.lua")) == "new-code",
          "A manual reload can request a fresh snapshot even when size and timestamp match");
    auto same_bytes = plugin_snapshot(directory);
    auto verified = refreshed.front();
    verified.sources = same_bytes.sources;
    check(verified.sources != refreshed.front().sources && verified == refreshed.front(),
          "A fresh pointer to identical source bytes is not a plugin change");
}

void an_approved_plugin_runs_only_the_source_snapshot_it_was_shown() {
    const auto directory = scratch / "snapshot-plugin";
    const auto entry = directory / "plugin" / "main.lua";
    const auto module = directory / "plugin" / "Helper.lua";
    std::filesystem::remove_all(directory);
    write(directory / manifest_file, manifest());
    write(entry, R"(reskate.log("entry-old") reskate.on("tick", function() reskate.log(require("helper").version) end))");
    write(module, R"(return {version = "module-old"})");
    auto snapshot = plugin_snapshot(directory);
    check(snapshot.error.empty() && snapshot.sources, "The complete source snapshot is ready to load");

    struct ApprovalEnvironment final : Environment {
        std::string approved_fingerprint;
        bool allowed{true};
        std::vector<std::string> logs;
        void log(LogLevel, std::string_view text) override { logs.emplace_back(text); }
        void console(std::string_view) override {}
        double now_ms() override { return 1000; }
        std::filesystem::path storage_root() override { return scratch / "snapshot-storage"; }
        bool builtin_command(std::string_view) override { return false; }
        std::optional<bool> approved(const Candidate& candidate) override {
            return allowed && candidate.plugin_fingerprint == approved_fingerprint;
        }
        void approve(const Candidate& candidate, bool on) override {
            approved_fingerprint = candidate.plugin_fingerprint;
            allowed = on;
        }
        bool logged(std::string_view text) const {
            return std::any_of(logs.begin(), logs.end(), [&](const auto& line) { return line.find(text) != std::string::npos; });
        }
    } environment;
    environment.approved_fingerprint = snapshot.fingerprint;

    Candidate candidate{"snapshot", directory, "mod-fingerprint", {}, snapshot.fingerprint, snapshot.sources};
    Host host(environment, Side::client);
    host.apply({candidate});
    check(environment.logged("entry-old"), "The approved entry source runs");
    const auto entry_time = std::filesystem::last_write_time(entry);
    const auto module_time = std::filesystem::last_write_time(module);
    const auto original_entry_size = std::filesystem::file_size(entry);
    const auto original_module_size = std::filesystem::file_size(module);
    write(entry, R"(reskate.log("entry-new") reskate.on("tick", function() reskate.log(require("helper").version) end))");
    write(module, R"(return {version = "module-new"})");
    std::filesystem::last_write_time(entry, entry_time);
    std::filesystem::last_write_time(module, module_time);
    check(std::filesystem::file_size(entry) == original_entry_size && std::filesystem::file_size(module) == original_module_size,
          "The changed entry and lazy module keep their original sizes");
    check(plugin_fingerprint(directory) != candidate.plugin_fingerprint, "The actual changed source bytes have a new approval fingerprint");

    host.tick(16);
    check(environment.logged("module-old") && !environment.logged("module-new"),
          "A first-time require reads the approved snapshot after its disk file changes");
    check(host.reload("snapshot").find("reloaded") != std::string::npos, "The unchanged approved snapshot can still be reloaded");
    host.tick(16);
    check(environment.logged("entry-old") && !environment.logged("entry-new"),
          "Reload does not execute changed entry bytes under the old approval");
    check(!environment.logged("module-new"), "Reload also keeps a lazy module on the approved snapshot");

    auto changed_snapshot = plugin_snapshot(directory);
    auto changed = candidate;
    changed.plugin_fingerprint = changed_snapshot.fingerprint;
    changed.sources = changed_snapshot.sources;
    host.apply({changed});
    check(status_of(host, "snapshot").state == "switched off" && !environment.logged("entry-new"),
          "A changed snapshot stays held until the player approves its new fingerprint");
    check(host.switch_on("snapshot").find("started") != std::string::npos, "A new explicit approval starts the changed snapshot");
    check(environment.logged("entry-new"), "The new entry runs only after the new approval");
    host.shutdown();
}

// reskate.strings: the text the game shows for its ids, by id or by #hash; checked before the game
// gets it, and given back when the plugin unloads.
void plugins_change_the_games_text() {
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    host.apply({script("words", R"(
        reskate.log("hash " .. reskate.strings.hash("ID_MENU_PLAY"))
        assert(reskate.strings.set("ID_MENU_PLAY", "Oyna"))
        assert(reskate.strings.set("#00ab12CD", "\u{c7}\u{131}k"))
        local n = reskate.strings.set_many({_about = "notes", ID_A = "Bir", ["#ff"] = "\u{130}ki"})
        assert(n == 2, n)
        assert(reskate.strings.exists("ID_A") == true and reskate.strings.exists("ID_NONE") == false, "exists")
        assert(not pcall(reskate.strings.set, "#xyz", "a"), "a bad hash")
        assert(not pcall(reskate.strings.set, "#123456789", "a"), "a hash of 9 digits")
        assert(not pcall(reskate.strings.set, "two words", "a"), "an id has no spaces")
        assert(not pcall(reskate.strings.set, "ID_B", "\xff"), "not UTF-8")
        assert(not pcall(reskate.strings.set, "ID_B", "a\0b"), "a NUL")
        assert(not pcall(reskate.strings.set, "ID_B", string.rep("a", 512)), "512 units are too many")
        assert(reskate.strings.set("ID_B", string.rep("\u{15f}", 511)), "511 two-byte characters fit")
        assert(not pcall(reskate.strings.set_many, {ID_C = 1}), "a text is a string")
        assert(not pcall(reskate.strings.set_many, {"x"}), "keys are ids")
        assert(not pcall(reskate.strings.set_many, {ID_C = "c", ["bad id"] = "d"}), "one bad pair refuses all")
        assert(reskate.strings.set_case_rules("tr"), "Turkish casing")
        assert(reskate.strings.set_case_rules(), "the game's casing again")
        assert(not pcall(reskate.strings.set_case_rules, "de"), "only tr")
        reskate.log("strings fine")
    )", R"("strings")")});
    running(host, "words", environment);
    check(environment.logged("[words] strings fine"), "The strings API checks what it is given\n" + environment.all());
    check(environment.logged(std::format("[words] hash #{:08x}", dingosdk::text_overrides::string_hash("ID_MENU_PLAY"))),
          "A hash is the game's id hash\n" + environment.all());
    check(game.called("strings words 1 ID_MENU_PLAY=Oyna") && game.called("strings words 1 #00ab12CD="), "Each text reaches the game");
    check(game.called("strings words 2 ") && !game.strings.contains("words _about"), "set_many leaves notes out");
    check(!game.strings.contains("words ID_C"), "and takes nothing of a table with a bad pair");
    std::vector<std::string> rules;
    std::copy_if(game.calls.begin(), game.calls.end(), std::back_inserter(rules), [](const std::string& c) { return c.starts_with("case rules "); });
    check(rules == std::vector<std::string>{"case rules words tr", "case rules words "},
          "Casing rules reach the game, and only known ones");
    using dingosdk::text_overrides::turkish_case;
    static_assert(turkish_case(u'i', u'I', 2) == char16_t{0x130} && turkish_case(u'I', u'i', 1) == char16_t{0x131});
    static_assert(turkish_case(char16_t{0x131}, u'I', 2) == u'I' && turkish_case(char16_t{0x130}, u'i', 1) == u'i', "the game maps these already");
    static_assert(turkish_case(u'a', u'A', 2) == u'A' && turkish_case(u'i', u'i', 0) == u'i' && turkish_case(u'I', u'I', 2) == u'I');
    check(dingosdk::text_overrides::string_hash("") == 0xffffffffU && dingosdk::text_overrides::string_hash("a") == 0xffffffffU * 33 + 'a',
          "The hash starts from 0xffffffff and takes h * 33 + byte");
    host.apply({});
    check(std::find(game.released.begin(), game.released.end(), "words") != game.released.end(), "Unloading gives its text back");
}

// Shared content: a plugin names a folder in its manifest's "content" and reads it in every enabled
// mod; other folders, and the field without mods.read, are refused.
void a_plugin_reads_the_content_folder_it_names() {
    for (const auto* bad : {R"({"api": 1, "entry": "a.lua", "content": ["parks"]})",
                            R"({"api": 1, "entry": "a.lua", "permissions": ["mods.read"], "content": ["../up"]})",
                            R"({"api": 1, "entry": "a.lua", "permissions": ["mods.read"], "content": "parks"})"}) {
        bool refused = false;
        try { parse_manifest(bad); } catch (const std::exception&) { refused = true; }
        check(refused, std::string("A bad content field is refused: ") + bad);
    }
    check(parse_manifest(R"({"api": 1, "entry": "a.lua", "permissions": ["mods.read"], "content": ["Parks", "parks", "routes"]})").content ==
              std::vector<std::string>{"parks", "routes"},
          "Content names are lower-case, each once");
    TestEnvironment environment;
    auto& game = environment.fake;
    game.content_files = {{"Someone-Park_Pack", "Park Pack", "sanvan/bridge.json", 12}, {"Map_Mod", "Map Mod", "red.json", 7}};
    game.content_texts["Someone-Park_Pack/parks/sanvan/bridge.json"] = R"({"id":"bridge"})";
    Host host(environment, Side::client);
    host.apply({package("reader", {{manifest_file, R"({"api": 1, "entry": "plugin/main.lua", "permissions": ["console", "mods.read"], "content": ["parks"]})"},
                                   {"plugin/main.lua", R"(
        local files = assert(reskate.mods.content("Parks"))
        assert(#files == 2 and files[1].mod == "Someone-Park_Pack" and files[1].path == "sanvan/bridge.json" and files[1].size == 12
               and files[2].title == "Map Mod", "listed")
        assert(reskate.mods.read_content("Someone-Park_Pack", "parks", "sanvan/bridge.json") == '{"id":"bridge"}', "read")
        local text, why = reskate.mods.read_content("Map_Mod", "parks", "missing.json")
        assert(text == nil and why == "not found", "a missing file")
        local ok, err = pcall(reskate.mods.content, "routes")
        assert(not ok and err:find("in the manifest's \"content\""), err)
        ok, err = pcall(reskate.mods.read_content, "Map_Mod", "parks", "../../secret.txt")
        assert(not ok and err:find("a path inside"), err)
        reskate.console.print("content ok")
    )"}})});
    running(host, "reader", environment);
    check(environment.printed("content ok") && game.called("content list parks"), "The plugin reads its content folder\n" + environment.all());
}

// What the game and a dedicated server read for mods.content (plugin_content.h), from real folders:
// every mod folder in a Mods folder by name, each one's files under the folder asked for, and the
// limits a plugin cannot get past.
void content_is_read_from_the_mod_folders() {
    namespace content = dingosdk::plugins::content;
    const auto mods = scratch / "content mods";
    const auto write = [](const std::filesystem::path& file, const std::string& text) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    };
    write(mods / "b_track" / "parks" / "start.json", "{}");
    write(mods / "b_track" / "parks" / "deep" / "a" / "b" / "c" / "level4.json", "{}");
    write(mods / "b_track" / "parks" / "deep" / "a" / "b" / "c" / "d" / "level5.json", "{}");
    write(mods / "b_track" / "parks" / "half.tmp", "");
    write(mods / "b_track" / "parks" / "big.json", std::string((1u << 20) + 1, ' '));
    write(mods / "b_track" / "manifest.json", R"({"name": "B Track", "version_number": "1.0.0"})");
    write(mods / "A_Park" / "parks" / "z.json", "zz");
    write(mods / "A_Park" / "reskate-levels.json", "{}");
    write(mods / "no parks" / "manifest.json", "not JSON");
    write(mods / ".reskate_plugin_data" / "parks" / "secret.json", "{}");
    write(mods / "loose.json", "{}");

    const auto found = content::mod_folders(mods);
    check(found.size() == 3 && found[0].name == "A_Park" && found[1].name == "b_track" && found[2].name == "no parks",
          "Every mod folder, by name, none hidden");
    check(found[0].title == "A_Park" && found[1].title == "B Track" && found[2].title == "no parks", "titled as their manifest names them");
    check(content::mod_folders(scratch / "no such folder").empty(), "No Mods folder, no mods");

    std::vector<Game::ContentFile> files;
    content::list(found, "parks", files);
    std::string listed;
    for (const auto& file : files) listed += std::format("{}:{}:{} ", file.mod, file.path, file.size);
    check(listed == std::format("A_Park:z.json:2 b_track:big.json:{} b_track:deep/a/b/c/level4.json:2 b_track:start.json:2 ", (1u << 20) + 1),
          "Mod by mod, sorted, four folders deep at most, no .tmp: " + listed);

    std::string text;
    check(content::read(found, "B_TRACK", "parks", "start.json", text).empty() && text == "{}", "A file is read, the mod named in any case");
    check(content::read(found, "b_track", "parks", "missing.json", text) == "not found", "A missing file");
    check(content::read(found, "b_track", "parks", "big.json", text) == "the file is larger than 1 MB", "A file over 1 MB");
    check(content::read(found, "nobody", "parks", "start.json", text) == "no such mod is on", "A mod that is not there");
    check(content::read(found, ".reskate_plugin_data", "parks", "secret.json", text) == "no such mod is on", "A hidden folder is no mod");

    for (int i = 0; i < 1005; ++i) write(mods / "c_many" / "parks" / std::format("{:04}.json", i), "");
    files.clear();
    content::list(content::mod_folders(mods), "parks", files);
    check(files.size() == content::most_files, std::format("At most {} files, {} listed", content::most_files, files.size()));
}


void plugins_drive_the_throwdown_kit() {
    namespace kit = dingosdk::throwdowns::kit;
    TestEnvironment environment;
    auto& game = environment.fake;
    Host host(environment, Side::client);
    host.apply({script("kit", R"(
        assert(reskate.throwdowns.mode{id = "lap", name = "Lap", description = "Through the gates.", card = "spot", order = 2,
            flag = "World/ReSkate/Throwdowns/Flags/lap_Placement", color = 0xff3434ff, players = {min = 1, max = 6, default = 4}})
        assert(not pcall(reskate.throwdowns.mode, {id = "x", name = "X", card = "bowling"}), "a stock card")
        assert(reskate.throwdowns.ui{
            setup = {mode = "lap", title = "LAP", players = {caption = "Players", min = 1, max = 6, step = 1, value = 4},
                     timer = {caption = "Finish deadline", min = 30, max = 600, step = 30, value = 180},
                     rows = {{id = "laps", kind = "choice", caption = "Laps", options = {"1", "2", "3"}, value = 2},
                             {id = "area", kind = "button", caption = "Area", label = "Set optional area"}}},
            waiting = {mode = "lap", title = "LAP", status = "Waiting for host", enrolled = 1, capacity = 6, can_start = true},
            hud = {theme = "lap", layout = "side", counters = {{text = "LEADER 1,200"}},
                   clock = {remaining_ms = 60000, total_ms = 90000, sampled_ms = 5, ticking = true},
                   board = {title = "LEADERBOARD", columns = {"POS", "NAME", "SCORE"},
                            rows = {{rank = 1, name = "Me", value = 1200, self = true, marks = {{text = "S", on = true}}}}},
                   timers = {{caption = "LAP", text = {base_ms = 1500, sampled_ms = 5, running = true, cap_ms = 600000}}},
                   markers = {{at = vector.create(1, 2, 3), text = "1", distance = true}}},
            panel = {title = "LAP", settings = {{icon = "players", caption = "Players", value = "4"}},
                     prompts = {{input = "UI_ShoulderL", caption = "Area", command = "area"}}, players = {{id = "76561198000000001", name = "Me", self = true}}},
            flags = {{id = "f1", mode = "lap", leader = "76561198000000001", game = 7, at = vector.create(1, 2, 3), yaw = 90,
                      stats = {{title = "LAPS", value = "2"}}}},
            match = {teleports = true, hidden = {"76561198000000002"}},
        })
        assert(not pcall(reskate.throwdowns.ui, "text"), "a document is a table")
        reskate.on("throwdown_ui", function(e)
            reskate.log("ui " .. e.action .. " " .. e.mode .. " " .. table.concat(e.args, ","))
            if e.action == "open" then
                assert(reskate.throwdowns.flag.begin("lap", tonumber(e.args[1]), {players = 4, seconds = 90}))
                local state = reskate.throwdowns.flag.state()
                assert(state.mine and state.mode == "lap" and state.game == tonumber(e.args[1]) and state.placing, "the flag is the plugin's")
            elseif e.action == "leave" then
                reskate.throwdowns.flag.leave()
            end
        end)
        assert(reskate.files.write("routes/a.json", "{\"gates\": 3}"))
        assert(reskate.files.read("routes/a.json") == "{\"gates\": 3}")
        local names = reskate.files.list("routes")
        assert(#names == 1 and names[1] == "a.json", "the folder lists its file")
        assert(reskate.files.list()[1] == "routes/", "and the top its folder")
        assert(not pcall(reskate.files.read, "../escape.json"), "no way out")
        assert(not pcall(reskate.files.write, "C:/escape.json", ""), "no drive")
        assert(not pcall(reskate.files.write, "routes/con.json", ""), "no device")
        assert(reskate.files.write("old.json", "x") and reskate.files.remove("old.json"))
        local missing, why = reskate.files.read("old.json")
        assert(missing == nil and why == "not found", "a removed file is gone")
        assert(reskate.ui.dialog("save", {title = "SAVE ROUTE", field = {text = "My route", max = 32},
            list = {rows = {{text = "Route A", sub = "3 gates", buttons = {"rename"}}}, empty = "No routes"},
            buttons = {"Save", {id = "cancel", label = "Cancel"}}, file = {label = "Load .json", filter = "*.json"}}))
        assert(not pcall(reskate.ui.dialog, "x", {buttons = {"1", "2", "3", "4", "5"}}), "at most 4 buttons")
        assert(reskate.skater.push(vector.create(1, 0, 2)))
        assert(reskate.skater.push_wait() and reskate.skater.push_reset())
        reskate.skater.hold(true)
        assert(reskate.modes == nil, "reskate.modes is gone")
        -- The general services: physics, the session's world, the trick feed and flick diagrams,
        -- the pad flight and the Bone Cam.
        reskate.physics.competition(true)
        assert(reskate.physics.stock_ready())
        reskate.physics.prepare()
        local world = reskate.session.world()
        assert(world.world == "77" and world.map == "test_map" and world.barred == false, "the session's world")
        reskate.tricks.feed(true)
        assert(#reskate.tricks.gesture("Kickflip") == 2 and #reskate.tricks.gesture("Boardslide") == 0, "flick diagrams")
        assert(reskate.camera.pad_flight(true))
        assert(reskate.skater.bone_cam() == "game" and reskate.skater.bone_cam("ringing") == "1")
        reskate.skater.bone_cam("game", "on")
    )", R"("throwdowns", "files", "ui.screen", "skater.control", "session.read", "events.tricks", "camera")")});
    running(host, "kit", environment);
    check(game.called("mode kit lap Lap") && game.modes.size() == 1, "The mode reaches the game\n" + environment.all());
    const auto& mode = game.modes.front();
    check(mode.card == "spot" && mode.order == 2 && mode.flag == "World/ReSkate/Throwdowns/Flags/lap_Placement" && mode.color == 0xff3434ffU,
          "with its card, order, flag prefab and colour");
    const auto documents = kit::documents_for("kit/lap");
    check(documents && documents->plugin == "kit" && game.kit_documents == 1, "Its documents are in the kit's store");
    const auto& setup = *documents->setup;
    check(setup.mode == "kit/lap" && setup.title == "LAP" && setup.players.present && setup.players.value == 4 && setup.timer.present &&
              setup.timer.value == 180 && setup.hide_native == std::vector<std::string>{"rounds"},
          "Its setup page, its mode the SDK's id");
    check(setup.rows.size() == 2 && setup.rows[0].kind == "choice" && setup.rows[0].selected == 1 && setup.rows[1].kind == "button" &&
              setup.rows[1].label == "Set optional area",
          "Its setup rows, a choice counted from 0");
    check(documents->waiting && documents->waiting->mode == "kit/lap" && documents->waiting->can_start && documents->waiting->capacity == 6, "Its waiting card");
    const auto& hud = documents->hud;
    check(hud.active && hud.theme == "lap" && hud.counters == std::vector<std::string>{"LEADER 1,200"} && hud.clock.present && hud.clock.remaining_ms == 60000,
          "Its HUD");
    check(hud.board.present && hud.board.rows.size() == 1 && hud.board.rows[0].rank == "1" && hud.board.rows[0].value == "1200" && hud.board.rows[0].self &&
              hud.board.rows[0].marks.size() == 1 && hud.board.rows[0].marks[0].on && hud.board.columns.size() == 3,
          "Its board, numbers as labels");
    check(hud.timers.size() == 1 && hud.timers[0].clock.present && hud.timers[0].clock.up && hud.timers[0].clock.remaining_ms == 1500 &&
              hud.timers[0].clock.total_ms == 600000 && hud.markers.size() == 1 && hud.markers[0].at[2] == 3,
          "A stopwatch timer and a marker");
    check(documents->panel.present && documents->panel.prompts.size() == 1 && documents->panel.prompts[0].glyph == "area" &&
              documents->panel.players.size() == 1 && documents->panel.players[0].id == 76561198000000001ULL,
          "Its match panel");
    check(documents->flags.size() == 1 && documents->flags[0].mode == "kit/lap" && documents->flags[0].leader == 76561198000000001ULL &&
              documents->flags[0].game == 7 && documents->flags[0].at[0] == 1 && documents->flags[0].stats.size() == 1,
          "Its flags");
    check(kit::match().teleports && kit::hidden(76561198000000002ULL) && !kit::hidden(76561198000000001ULL) && kit::active() && kit::active()->plugin == "kit",
          "What its match asks of the game");
    check(game.files.contains("kit/routes/a.json") && !game.files.contains("kit/old.json"), "Its files are in its own folder");
    check(game.called("dialog kit save rows 1 buttons 2") && game.dialog && game.dialog->field.text == "My route" && game.dialog->file.filter == "*.json" &&
              game.dialog->buttons[1].id == "cancel" && game.dialog->rows[0].buttons[0] == "rename",
          "Its dialog");
    check(game.called("push kit 1 0 2") && game.called("push wait kit") && game.called("push reset kit") && game.called("hold kit true"),
          "Push, its wait and reset, and hold reach the game");
    check(game.called("competition kit true") && game.called("prepare physics") && game.called("trick feed kit true") &&
              game.called("pad flight kit true") && game.called("bone cam kit game on"),
          "The general services reach the game\n" + environment.all());
    // The native UI: a card clicked, a dialog answered.
    kit::post("kit/lap", "open", {"7"});
    kit::post_to("kit", "dialog", "", {"save", "Save", "My route", "0", ""});
    kit::post("other/lap", "open", {"8"});
    ticks(host, environment, 1);
    check(environment.logged("[kit] ui open lap 7") && game.called("flag begin kit/lap 7 4 90 true 0.0"),
          "A card clicked reaches the plugin by its own id, which takes the flag\n" + environment.all());
    check(environment.logged("[kit] ui dialog  save,Save,My route,0,"), "A dialog's answer reaches it\n" + environment.all());
    check(!environment.logged("ui open lap 8") && kit::take("other").size() == 1, "Another plugin's events stay its own");
    kit::post("kit/lap", "leave");
    ticks(host, environment, 1);
    check(game.called("flag leave kit") && !game.flag.busy, "Leave lets the flag go");
    host.switch_off("kit");
    check(!kit::documents_for("kit/lap") && !kit::match().teleports && !kit::hidden(76561198000000002ULL), "Its documents go when it stops");
}

// The Throwdowns page's cards: the stock three (one replaced) and the plugin modes by order.
void the_throwdowns_page_lists_the_stock_cards_and_plugin_modes() {
    namespace custom = dingosdk::multiplayer::custom_throwdowns;
    using dingosdk::throwdowns::Adapter;
    custom::Mode skate;
    skate.plugin = "p";
    skate.id = "skate";
    skate.name = "Letters";
    skate.card = "skate";
    skate.replaces = "skate";
    custom::Mode late = skate;
    late.id = "late";
    late.name = "Late";
    late.card = "jam";
    late.replaces.clear();
    late.order = 20;
    late.color = 0xff00ff00U;
    custom::Mode early = late;
    early.id = "early";
    early.name = "Early";
    early.card = "spot";
    early.order = 10;
    early.color = 0;
    check(custom::set_mode(skate).empty() && custom::set_mode(late).empty() && custom::set_mode(early).empty(), "The modes register");
    check(!custom::set_mode([&] { auto bad = early; bad.card = "bowling"; return bad; }()).empty(), "A card is a stock one");
    const auto& registry = dingosdk::throwdowns::registry();
    std::vector<std::string> ids;
    for (const auto& d : registry.definitions()) ids.push_back(d.id);
    check(ids == std::vector<std::string>{"p/skate", "native.spot", "native.jam", "p/early", "p/late"}, "In the grid's order: " + [&] {
        std::string all;
        for (const auto& id : ids) all += id + " ";
        return all;
    }());
    const auto* late_card = registry.find("p/late");
    check(late_card && late_card->adapter == Adapter::plugin && late_card->donor == 2 && late_card->palette.primary == 0xff00ff00U && late_card->plugin == "p",
          "A plugin card: its donor and colour");
    check(registry.find("p/early")->palette.primary == dingosdk::throwdowns::palette_for("p/early").primary, "a colour picked from its id");
    check(registry.find("native.spot")->donor == 1 && registry.find("native.jam")->adapter == Adapter::native_jam, "The stock cards");
    const auto before = registry.revision();
    custom::remove_plugin("p");
    const auto& again = dingosdk::throwdowns::registry();
    check(again.revision() > before && again.definitions().size() == 3 && again.definitions()[0].id == "native.skate", "S.K.A.T.E. is back when it goes");
}


}

// A mod's plugin runs only from a package Thunderstore lists as a plugin: one disguised as a
// cosmetics pack does not, nor one taken off Thunderstore since; a folder that is not a package, or
// a package Thunderstore never had, is the player's own and runs. Until a package has been looked
// up, its plugin waits, unless Thunderstore cannot be reached: then the check is skipped until it
// answers. The answers are kept in a file and asked again after six hours.
void thunderstore_says_which_packages_run_plugins() {
    const auto file = scratch / "listings.json";
    std::filesystem::remove(file);
    const auto answer = [](std::string owner, std::string name, std::vector<std::string> categories, std::string review = "approved") {
        std::string list;
        for (const auto& category : categories) list += (list.empty() ? "\"" : ",\"") + category + "\"";
        return ListingAnswer{ListingAnswer::Kind::listed,
                             R"({"namespace":")" + owner + R"(","name":")" + name + R"(","community_listings":[{"community":"reskate","categories":[)" +
                                 list + R"(],"review_status":")" + review + R"("}]})"};
    };
    std::int64_t now = 1'800'000'000;
    {
        Listings listings(file);
        check(listings.verdict("My Plugin", now).runs && listings.verdict("myplugin", now).runs, "A folder that is not a package runs its plugin");
        const auto waiting = listings.verdict("kokomorning2-Walmart_Cosmetics-1.0.0", now);
        check(!waiting.runs && waiting.why.find("waiting") != std::string::npos, "A package not looked up yet waits");
        (void)listings.verdict("Team-Speedometer", now);
        (void)listings.verdict("Team-Taken_Down", now);
        (void)listings.verdict("Team-Not_Yet_Uploaded", now);
        (void)listings.verdict("Team-Rejected", now);
        auto wanted = listings.take_wanted(now);
        std::sort(wanted.begin(), wanted.end());
        check(wanted == std::vector<std::string>{"Team-Not_Yet_Uploaded", "Team-Rejected", "Team-Speedometer", "Team-Taken_Down",
                                                 "kokomorning2-Walmart_Cosmetics"},
              "Each package is looked up once, under the folder's spelling and without its version");
        check(listings.take_wanted(now + 1).empty(), "and not again at once");
        listings.record("kokomorning2-Walmart_Cosmetics", answer("kokomorning2", "Walmart_Cosmetics", {"Cosmetics", "Skateboards"}), now);
        listings.record("Team-Speedometer", answer("Team", "Speedometer", {"Plugins", "Misc"}), now);
        listings.record("Team-Taken_Down", answer("Team", "Taken_Down", {"Plugins"}), now);
        listings.record("Team-Not_Yet_Uploaded", {ListingAnswer::Kind::missing, {}}, now);
        listings.record("Team-Rejected", answer("Team", "Rejected", {"Plugins"}, "rejected"), now);
        const auto disguised = listings.verdict("kokomorning2-Walmart_Cosmetics-1.0.0", now);
        check(!disguised.runs && disguised.why.find("not list") != std::string::npos && disguised.why.find("Cosmetics, Skateboards") != std::string::npos,
              "A cosmetics package's plugin does not run, and the player is told what it is listed as: " + disguised.why);
        check(listings.verdict("Team-Speedometer", now).runs, "A package listed as a plugin runs it");
        check(listings.verdict("Team-Not_Yet_Uploaded", now).runs, "A package Thunderstore never had runs, as a plugin of the player's own");
        check(!listings.verdict("Team-Rejected", now).runs, "A package the moderators rejected does not");
        listings.record("Team-Speedometer", {ListingAnswer::Kind::failed, {}}, now + 10);
        check(listings.verdict("Team-Speedometer", now + 10).runs, "A failed lookup keeps what was known");
        listings.record("Team-Speedometer", answer("Someone", "Else", {"Plugins"}), now + 10);
        check(listings.verdict("Team-Speedometer", now + 10).runs, "An answer about another package changes nothing");
        listings.record("Team-Taken_Down", {ListingAnswer::Kind::missing, {}}, now + 20);
        const auto gone = listings.verdict("Team-Taken_Down", now + 20);
        check(!gone.runs && gone.why.find("taken off") != std::string::npos, "A plugin taken off Thunderstore stops");
        check(listings.take_wanted(now + 100).empty(), "Known answers are not asked again soon");
        check(listings.take_wanted(now + Listings::refresh_seconds + 30).size() == 5, "but are after six hours");
    }
    {
        Listings again(file);
        check(!again.verdict("kokomorning2-Walmart_Cosmetics", now).runs && again.verdict("Team-Speedometer", now).runs &&
                  !again.verdict("Team-Taken_Down", now).runs && again.verdict("Team-Not_Yet_Uploaded", now).runs,
              "The answers are kept across launches");
    }
    {
        // Offline: a package never answered for runs without the check, until Thunderstore answers.
        Listings offline;
        check(!offline.verdict("Team-Offline_Pack", now).runs && offline.take_wanted(now).size() == 1, "A package waits for its first lookup");
        offline.record("Team-Offline_Pack", {ListingAnswer::Kind::failed, {}}, now);
        check(offline.verdict("Team-Offline_Pack", now).runs, "When Thunderstore cannot be reached, the check is skipped");
        check(offline.take_wanted(now + Listings::retry_seconds).size() == 1, "and it is asked again a minute later");
        offline.record("Team-Offline_Pack", answer("Team", "Offline_Pack", {"Cosmetics"}), now + Listings::retry_seconds);
        check(!offline.verdict("Team-Offline_Pack", now + Listings::retry_seconds).runs, "Once it answers, its answer holds");
        offline.record("Team-Offline_Pack", {ListingAnswer::Kind::failed, {}}, now + Listings::refresh_seconds * 2);
        check(!offline.verdict("Team-Offline_Pack", now + Listings::refresh_seconds * 2).runs,
              "and going offline later does not undo a refusal");
    }
    check(listing_package("Owner-Name-1.2.3") == std::optional<std::string>("Owner-Name") && listing_package("Owner-Name") &&
              !listing_package("Owner-Name-latest") && !listing_package("Owner") && !listing_package("Own er-Name"),
          "A folder is named as a package, or not");
}
#include "measured_contact_host_fixture.inc"
#include "body_prepare_host_fixture.inc"
#include "shape_categories_host_fixture.inc"
#include "exclusion_facts_host_fixture.inc"

int main() {
    const auto run = [](void (*test)()) { test(); };
    try {
        scratch = std::filesystem::temp_directory_path() / "reskate-plugin-tests";
        std::filesystem::remove_all(scratch);
        run(a_manifest_says_what_a_plugin_is);
        run(a_variant_can_wait_for_the_next_launch);
        run(a_plugin_holds_a_trainer_preset);
        run(the_skater_surface_sound_is_set_for_some_fields);
        run(the_time_of_day_and_world_controls_are_read);
        run(ui_value_reservations_enforce_limits_and_collisions);
        run(plugins_publish_only_their_own_ui_values);
        run(plugin_approval_follows_the_plugins_own_files);
        run(an_approved_plugin_runs_only_the_source_snapshot_it_was_shown);
        run(traffic_vehicles_are_permissioned_bounded_and_released);
        run(measured_contact_host_fixture_v1);
        run(plugins_own_typed_bodies_and_acknowledge_measured_results);
        run(plugins_pipeline_native_steps_and_select_owned_receipts);
        run(plugins_can_inspect_later_partial_failure_without_acknowledging_unmeasured_head);
        run(plugins_read_how_many_commands_a_body_holds_pending);
        run(plugins_command_every_step_when_the_tick_precedes_the_synchronizer);
        run(plugins_reuse_their_own_parked_bodies);
        run(native_body_permission_and_client_boundary_are_enforced);
        run(body_prepare_host_fixture_v1);
        run(owned_body_batches_are_permissioned_opaque_and_transactional);
        run(body_contact_details_are_known_and_epoch_bound);
        run(owned_body_batches_select_a_named_query_preset);
        run(shape_categories_host_fixture_v1);
        run(exclusion_facts_host_fixture_v1);
        run(owned_body_batches_share_actual_and_logical_query_limits);
        run(effects_require_resident_roots);
        run(expired_effects_return_their_quota);
        run(game_effects_need_their_own_permission);
        run(plugins_render_private_effects);
        run(failed_plugin_startup_releases_effects);
        run(plugins_track_effect_lifetimes);
        run(physics_steps_include_optional_geometry);
        run(physics_claims_are_bounded_and_exclusive);
        run(marker_consumers_do_not_replay_or_steal_operations);
        run(plugins_control_only_claimed_object_physics);
        run(moving_edges_keep_their_authored_identity);
        run(object_contact_queries_are_owned_and_budgeted);
        run(a_plugin_loads_ticks_runs_its_commands_and_unloads);
        run(a_permission_must_be_declared);
        run(a_score_line_needs_its_own_permission);
        run(a_score_line_is_checked_before_the_game_sees_it);
        run(a_score_line_refusal_comes_back_to_the_plugin);
        run(one_plugins_score_line_shows_at_a_time);
        run(plugins_choose_stand_ins);
        run(plugins_read_and_hide_other_players_items);
        run(plugins_add_chat_commands);
        run(thunderstore_says_which_packages_run_plugins);
        run(command_names_are_checked);
        run(storage_is_kept_across_loads_and_has_a_quota);
        run(packages_json_and_modules);
        run(the_sandbox_holds);
        run(a_runaway_plugin_is_stopped_and_the_rest_go_on);
        run(a_slow_call_is_said_and_a_hung_one_stopped);
        run(a_steam_id_reaches_the_game_exactly);
        run(reskates_own_reading_is_not_a_plugins_time);
        run(a_plugin_over_its_time_budget_is_stopped);
        run(the_player_sets_how_strict_the_limits_are);
        run(a_plugin_reloads_when_its_files_change);
        run(the_skater_can_be_read_moved_and_the_world_probed);
        run(teleport_equipment_choices_keep_permissions_and_host_limits);
        run(plugins_distinguish_clear_unavailable_and_hit_world_probes);
        run(tricks_reach_plugins_that_ask_for_them);
        run(plugins_draw_on_the_overlay);
        run(plugins_draw_pictures);
        run(plugins_draw_meshes_marks_and_data_pictures);
        run(a_manifest_names_its_fonts);
        run(plugins_draw_text_in_their_own_fonts);
        run(a_plugin_sets_a_speedometer_in_its_own_digits);
        run(glyph_pages_are_sized_filled_and_let_go);
        run(plugins_manage_files_and_granted_folders);
        run(plugins_compose_pictures_from_steps);
        run(plugins_list_and_unpack_zip_files);
        run(the_zip_reader_lists_and_unpacks_a_file);
#ifdef _WIN32
        run(the_picture_renderer_draws_what_it_is_told);
        run(windows_reads_a_fonts_metrics_and_glyphs);
#endif
        run(plugins_make_richer_web_requests);
        run(plugins_draw_the_games_map);
        run(plugins_place_world_entities);
        run(plugins_place_set_pieces);
        run(plugins_show_a_skater_copy);
        run(plugins_have_what_a_prop_hunt_needs);
        run(plugins_write_normalized_controllers);
        run(controller_reports_preserve_non_controls);
        run(plugins_read_flip_state);
        run(plugins_read_frames_and_hold_gameplay_options);
#ifdef _WIN32
        run(animation_values_require_occupied_typed_local_pages);
#endif
        run(plugins_reach_cruise_and_read_the_mouse);
        run(plugins_set_the_flip_speed);
        run(plugins_show_a_viewmodel);
        run(plugins_carry_and_take_a_step_in_the_camera_cycle);
        run(screen_shader_files_are_checked);
        run(plugins_run_a_screen_shader);
        run(plugins_run_a_world_shader);
        run(plugins_read_joints_and_tune_physics);
        run(plugins_type_and_use_the_room);
        run(plugins_read_menu_pose);
        run(plugins_use_background_capture);
        run(plugins_touch_hide_and_ray_at_players);
        run(plugins_use_the_games_clips);
        run(plugins_edit_clips);
        run(plugins_server_copies_are_the_authority);
        run(plugins_server_copies_spawn_park_mods);
        run(plugins_server_copies_hide_players);
        run(plugins_server_copies_see_roles);
        run(plugins_server_copies_show_cards_and_polls);
        run(plugins_animate_the_skater);
        run(plugins_preload_clips_and_sounds);
        run(plugins_play_their_own_sounds);
        run(plugins_play_ima_adpcm_sounds);
        run(plugins_play_any_number_of_different_sounds);
        run(plugins_have_menu_panels);
        run(plugins_have_pages);
        run(plugins_spatialize_the_station);
        run(plugins_play_sounds_and_read_the_mods);
        run(plugins_read_item_origins_and_favorites);
        run(plugins_follow_the_session_and_message_each_other);
        run(plugins_call_and_hear_each_other);
        run(web_requests_go_only_to_listed_domains);
        run(game_menu_distinguishes_unknown_from_closed);
        run(the_wider_set_reaches_the_game);
        run(preview_board_material_scope);
        run(declared_board_colours_through_the_api);
        run(key_names_become_binds);
        run(level_events_reach_plugins);
        run(plugins_know_what_menu_is_up);
        run(timers_run_when_due);
        run(a_package_with_a_program_or_on_the_wrong_side_is_not_loaded);
        run(discovery_finds_the_plugins_among_the_mods_left_on);
        run(discovery_can_recheck_sources_when_mod_metadata_is_unchanged);
        run(a_servers_plugin_runs_only_on_servers_that_require_it);
        run(plugins_change_the_games_text);
        run(a_plugin_reads_the_content_folder_it_names);
        run(content_is_read_from_the_mod_folders);
        run(plugins_drive_the_throwdown_kit);
        run(the_throwdowns_page_lists_the_stock_cards_and_plugin_modes);
        std::filesystem::remove_all(scratch);
        std::cout << "plugin tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "plugin test failed: " << error.what() << '\n';
        return 1;
    }
}
