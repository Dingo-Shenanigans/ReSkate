#include "Engine/Game/World/body_shape_categories.h"
#include "Engine/Game/Build/20260929/park_editor.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace dingosdk::body_shape_categories;
namespace {
namespace q = dingosdk::body_query;
namespace r = dingosdk::addr::rigid_bodies;
namespace pb = dingosdk::addr::physics_bodies;
unsigned checks{};
void check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
constexpr std::uintptr_t maximum = std::numeric_limits<std::uintptr_t>::max();

// Sparse fake process memory. Every read is recorded, and after() may change
// memory between the helper's own paired reads.
struct Memory {
    std::map<std::uintptr_t, unsigned char> bytes;
    std::vector<std::uintptr_t> reads;
    std::function<void(std::uintptr_t)> after;
    template<class T> void put(std::uintptr_t at, const T& value) {
        std::array<unsigned char, sizeof(T)> raw{};
        std::memcpy(raw.data(), &value, sizeof(T));
        for (std::size_t i = 0; i < raw.size(); ++i) bytes[at + i] = raw[i];
    }
    template<class T> bool operator()(std::uintptr_t at, T& value) {
        reads.push_back(at);
        std::array<unsigned char, sizeof(T)> raw{};
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const auto found = bytes.find(at + i);
            if (found == bytes.end()) return false;
            raw[i] = found->second;
        }
        std::memcpy(&value, raw.data(), sizeof(T));
        if (after) after(at);
        return true;
    }
    std::size_t count(std::uintptr_t at) const { return static_cast<std::size_t>(std::count(reads.begin(), reads.end(), at)); }
    bool touched(std::uintptr_t begin, std::uintptr_t end) const {
        return std::any_of(reads.begin(), reads.end(), [&](std::uintptr_t at) { return at >= begin && at < end; });
    }
};

// Literal layout, independent of the production constants: World+28 holds the
// state; state+F58/FF8/F98 the shape bits, byte generations and shape-to-body
// map; World+680 the body generations; state+FE8 the category array.
struct Fixture {
    static constexpr std::uintptr_t world = 0x10000, state = 0x20000;
    static constexpr std::uintptr_t shape_bits = 0x30000, shape_generations = 0x40000, mappings = 0x50000;
    static constexpr std::uintptr_t body_generations = 0x60000, categories = 0x70000, moved = 0x78000;
    // A native ray result buffer. Nothing may read it: the helper is given only
    // the copied shape handle, never result storage.
    static constexpr std::uintptr_t result_buffer = 0x90000;
    static constexpr NativeHandle mapped_body{world, 57, 4};
    Memory memory;
    bool world_current{true};
    unsigned identity_calls{};
    Fixture() {
        memory.put(world + 0x28, state);
        memory.put(state + 0xf58, shape_bits);
        memory.put(state + 0xff8, shape_generations);
        memory.put(state + 0xf98, mappings);
        memory.put(world + 0x680, body_generations);
        memory.put(state + 0xfe8, categories);
        memory.put(body_generations + 57 * sizeof(std::uint32_t), std::uint32_t{4});
        // Child shapes 9 and 10 share body 57. Element 57 is a decoy that a read
        // through the mapped body's index would return instead.
        seed(9, 4, 57, 0x80000000U);
        seed(10, 4, 57, 1);
        memory.put(categories + 57 * sizeof(std::uint32_t), std::uint32_t{0xdeadbeefU});
        for (std::uintptr_t i = 0; i < 64; ++i) memory.put(result_buffer + i, std::uint8_t{0xab});
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    void seed(std::uint32_t index, std::uint8_t generation, std::uint32_t target, std::uint32_t mask) {
        auto& bits = memory.bytes[shape_bits + (index >> 3)];
        bits = static_cast<unsigned char>(bits | (1U << (index & 7U)));
        memory.put(shape_generations + index, generation);
        memory.put(mappings + std::uintptr_t{index} * sizeof(std::uint32_t), target);
        memory.put(categories + std::uintptr_t{index} * sizeof(std::uint32_t), mask);
    }
    static NativeHandle hit_shape(std::uint32_t index) { return {world, index, 4}; }
    static std::uintptr_t element(std::uintptr_t array, std::uint32_t index) { return array + std::uintptr_t{index} * sizeof(std::uint32_t); }
    // The caller's qualified-hit oracle: the production shape-to-body resolver
    // over this memory, behind a World/state currentness gate the case controls.
    bool qualified(const NativeHandle& shape, const NativeHandle& expected) {
        ++identity_calls;
        NativeHandle resolved;
        return world_current && dingosdk::body_hit_identity::resolve(world, state, shape, resolved, memory,
            [&] { return world_current; }, [](const NativeHandle&) { return true; },
            [](std::uint32_t candidate) { return candidate < 64; }, [](const NativeHandle&) { return true; }) &&
            resolved == expected;
    }
    auto oracle() { return [this](const NativeHandle& s, const NativeHandle& b) { return qualified(s, b); }; }
    Capture capture(std::uint32_t index, Witness& out) {
        return dingosdk::body_shape_categories::capture(world, state, hit_shape(index), mapped_body, out, memory, oracle());
    }
};

void the_hit_shape_selects_its_own_element() {
    Fixture f;
    Witness first, second;
    check(f.capture(9, first) == Capture::available && first.value == 0x80000000U, "an actual hit shape lost its category");
    check(first.world == Fixture::world && first.state == Fixture::state && first.shape == Fixture::hit_shape(9) &&
        first.body == Fixture::mapped_body && first.categories == Fixture::categories,
        "the witness lost its World/state, original shape/body or array");
    check(f.memory.count(Fixture::element(Fixture::categories, 9)) == 2 && f.memory.count(Fixture::state + 0xfe8) == 2 &&
        f.identity_calls == 2, "the pointer/value pair was not read twice between two identity checks");
    check(!f.memory.count(Fixture::element(Fixture::categories, 57)), "the mapped body index was read as a shape index");
    check(f.capture(10, second) == Capture::available && second.value == 1 && second.body == first.body &&
        second.shape != first.shape, "a sibling child shape of the same body shared the first shape's value");
}

void boundary_masks_round_trip_and_zero_is_known() {
    for (const std::uint32_t mask : {0U, 1U, 0x80000000U, 0xffffffffU}) {
        Fixture f;
        Witness out;
        f.memory.put(Fixture::element(Fixture::categories, 9), mask);
        check(f.capture(9, out) == Capture::available && out.value == mask, "a boundary mask did not round trip exactly");
    }
}

void malformed_hits_and_field_spans_read_nothing() {
    for (unsigned variant = 0; variant < 6; ++variant) {
        Fixture f;
        Witness out{1, 2, {}, {}, 3, 4};
        auto world = Fixture::world, state = Fixture::state;
        auto shape = Fixture::hit_shape(9);
        auto body = Fixture::mapped_body;
        switch (variant) {
        case 0: shape.index = 0xffffffffU; break;
        case 1: shape.world += 8; break;
        case 2: body.world += 8; break;
        case 3: body.index = 0xffffffffU; break;
        case 4: world = 0; break;
        case 5: state = 0; break;
        }
        check(capture(world, state, shape, body, out, f.memory, f.oracle()) == Capture::unavailable,
            "a sentinel, foreign or null hit produced a category");
        check(!out.world && !out.value && !out.categories, "a refusal left a stale witness");
        check(f.memory.reads.empty() && !f.identity_calls, "a malformed hit reached the identity callback or memory");
    }
    // The last state whose complete pointer field is representable reaches the
    // callback and one field read; one byte further reaches neither.
    const auto limit = maximum - (sizeof(std::uintptr_t) - 1) - 0xfe8;
    for (const auto edge : {limit, limit + 1}) {
        unsigned calls{};
        std::vector<std::uintptr_t> at;
        Witness out;
        const auto result = capture(edge, edge, NativeHandle{edge, 9, 4}, NativeHandle{edge, 57, 4}, out,
            [&](std::uintptr_t address, auto& value) { at.push_back(address); value = {}; return true; },
            [&](const NativeHandle&, const NativeHandle&) { ++calls; return true; });
        check(result == Capture::unavailable && !out.world, "a null array produced a category");
        check(edge == limit ? calls == 2 && at.size() == 1 && at[0] == edge + 0xfe8 : !calls && at.empty(),
            "the state field span was checked off by one or after a read");
    }
}

void unreadable_or_overflowing_storage_is_only_absence() {
    for (unsigned variant = 0; variant < 5; ++variant) {
        Fixture f;
        Witness out;
        switch (variant) {
        case 0: f.memory.bytes.erase(Fixture::state + 0xfe8 + 3); break;
        case 1: f.memory.put(Fixture::state + 0xfe8, std::uintptr_t{0}); break;
        case 2: f.memory.put(Fixture::state + 0xfe8, maximum - 7); break;
        // One byte short of a complete element 9: the span would wrap.
        case 3: f.memory.put(Fixture::state + 0xfe8, maximum - (9 * sizeof(std::uint32_t) + 2)); break;
        case 4: f.memory.bytes.erase(Fixture::element(Fixture::categories, 9) + 3); break;
        }
        check(f.capture(9, out) == Capture::unavailable && !out.world, "unreadable or overflowing storage published a value");
        check(f.identity_calls == 2, "an optional read failure skipped or replaced the identity checks");
        check(!f.memory.touched(maximum - 64, maximum), "an overflowing element span reached memory");
    }
}

void the_getter_bytes_gate_the_diagnostic() {
    const auto& pinned = r::shape_in_categories;
    // The pins agree with the fingerprint's own displacements: World+28 is the
    // state, state+FE8 the array, handle+8 the index, and elements are DWORDs.
    const auto displacement = std::uint32_t{pinned.bytes[14]} | std::uint32_t{pinned.bytes[15]} << 8 |
        std::uint32_t{pinned.bytes[16]} << 16 | std::uint32_t{pinned.bytes[17]} << 24;
    check(std::uintptr_t{pinned.bytes[10]} == pb::store_offset && std::uintptr_t{displacement} == r::shape_categories &&
        std::size_t{pinned.bytes[6]} == offsetof(NativeHandle, index) && (pinned.bytes[21] >> 6) == 2,
        "the reused offsets are not the ones the pinned getter reads");
    constexpr std::uintptr_t base = 0x140000000;
    Memory memory;
    memory.put(base + pinned.rva, pinned.bytes);
    check(layout_qualified(base, memory), "exact pinned getter bytes were refused");
    for (std::size_t i = 0; i < pinned.bytes.size(); ++i) {
        auto changed = pinned.bytes;
        changed[i] = static_cast<unsigned char>(changed[i] ^ 0x01U);
        memory.put(base + pinned.rva, changed);
        check(!layout_qualified(base, memory), "a getter byte mismatch still enabled categories");
    }
    memory.put(base + pinned.rva, pinned.bytes);
    memory.bytes.erase(base + pinned.rva + 31);
    check(!layout_qualified(base, memory), "unreadable getter bytes enabled categories");
    memory.reads.clear();
    check(!layout_qualified(0, memory) && !layout_qualified(maximum - pinned.rva - 30, memory) && memory.reads.empty(),
        "a null or overflowing image base reached memory");
}

void drift_inside_one_capture_is_classified() {
    // Each change lands right after the first element read, between the paired reads.
    for (unsigned variant = 0; variant < 9; ++variant) {
        Fixture f;
        Witness out;
        bool changed{};
        f.memory.after = [&](std::uintptr_t at) {
            if (changed || at != Fixture::element(Fixture::categories, 9)) return;
            changed = true;
            switch (variant) {
            case 0: // An identical copy at another address is still a replacement.
                f.memory.put(Fixture::element(Fixture::moved, 9), std::uint32_t{0x80000000U});
                f.memory.put(Fixture::state + 0xfe8, Fixture::moved);
                break;
            case 1: f.memory.put(Fixture::element(Fixture::categories, 9), std::uint32_t{0x80000001U}); break;
            case 2: f.memory.bytes.erase(Fixture::state + 0xfe8); break;
            case 3: f.memory.put(Fixture::shape_generations + 9, std::uint8_t{5}); break;
            case 4: f.memory.bytes[Fixture::shape_bits + 1] = 0; break;
            case 5: f.memory.put(Fixture::element(Fixture::mappings, 9), std::uint32_t{9}); break;
            case 6: f.world_current = false; break;
            case 7: f.memory.put(Fixture::element(Fixture::body_generations, 57), std::uint32_t{5}); break;
            case 8: // Both at once: the identity change outranks the value change.
                f.memory.put(Fixture::element(Fixture::categories, 9), std::uint32_t{1});
                f.memory.put(Fixture::shape_generations + 9, std::uint8_t{5});
                break;
            }
        };
        const auto result = f.capture(9, out);
        const auto expected = variant < 3 ? Capture::unavailable : Capture::identity_changed;
        check(changed && result == expected, "drift inside one capture was misclassified");
        check(!out.world && !out.value, "a drifting capture published a witness");
    }
}

void closing_inspection_downgrades_for_good_and_identity_is_sticky() {
    using Batch = Sidecars<4>;
    {
        Fixture f;
        Batch sidecars;
        const auto add = [&](std::size_t ordinal, std::uint32_t index) {
            return sidecars.capture(ordinal, Fixture::world, Fixture::state, Fixture::hit_shape(index), Fixture::mapped_body,
                f.memory, f.oracle());
        };
        // Rays 0 and 2 hit shape 9; ray 1 hits its sibling shape 10.
        check(add(0, 9) && add(1, 10) && add(2, 9), "a current hit was refused");
        check(sidecars.available(0) && sidecars.available(1) && sidecars.available(2) && !sidecars.available(3) &&
            !sidecars.available(4), "sidecars were not bound to their ray ordinals");
        check(sidecars.current(f.memory, f.oracle()) && sidecars.available(0) && sidecars.available(1) && sidecars.available(2),
            "an unchanged closing inspection dropped a value");
        f.memory.put(Fixture::element(Fixture::categories, 10), std::uint32_t{2});
        check(sidecars.current(f.memory, f.oracle()) && sidecars.available(0) && !sidecars.available(1) && sidecars.available(2),
            "a sibling's value drift was not confined to its own ray");
        f.memory.put(Fixture::element(Fixture::categories, 10), std::uint32_t{1});
        check(sidecars.current(f.memory, f.oracle()) && !sidecars.available(1), "a restored value regained availability");
        f.memory.put(Fixture::element(Fixture::moved, 9), std::uint32_t{0x80000000U});
        f.memory.put(Fixture::element(Fixture::moved, 10), std::uint32_t{1});
        f.memory.put(Fixture::state + 0xfe8, Fixture::moved);
        check(sidecars.current(f.memory, f.oracle()) && !sidecars.available(0) && !sidecars.available(2) && !sidecars.identity_lost(),
            "a replaced array kept old values or refused the batch");
    }
    Fixture f;
    Batch sidecars;
    const auto add = [&](std::size_t ordinal, std::uint32_t index) {
        return sidecars.capture(ordinal, Fixture::world, Fixture::state, Fixture::hit_shape(index), Fixture::mapped_body,
            f.memory, f.oracle());
    };
    check(add(0, 9) && add(1, 10), "a current hit was refused");
    f.memory.put(Fixture::element(Fixture::mappings, 10), std::uint32_t{9});
    check(!sidecars.current(f.memory, f.oracle()) && sidecars.identity_lost(), "closing identity drift was only a missing value");
    f.memory.put(Fixture::element(Fixture::mappings, 10), std::uint32_t{57});
    check(!sidecars.current(f.memory, f.oracle()) && !add(2, 9), "a recovered identity cleared the batch refusal");
    q::Request request;
    request.include_shape_categories = true;
    q::Result result;
    result.available = true;
    result.count = 2;
    result.rays[0].status = result.rays[1].status = q::Status::hit;
    sidecars.publish(request, result);
    check(!result.rays[0].in_categories_available && !result.rays[1].in_categories_available,
        "an identity-lost batch published a category");
}

q::Request request(std::size_t count, bool categories) {
    q::Request out;
    out.observation.epoch = {7, 11, 100, 3, static_cast<float>(dingosdk::owned_body::native_seconds)};
    out.count = count;
    out.observation.mass = 12;
    out.observation.inertia = {2, 3, 4};
    out.observation.rotation = {0, 0, 0, 1};
    for (auto& ray : out.rays) ray = {{1, 2, 3}, {1, -2, 3}};
    out.include_shape_categories = categories;
    return out;
}
// Literal masks, independent of the production constants: the camera filter, the
// named actor categories and the Static and Terrain shape templates.
constexpr std::uint32_t camera = 0x40080303U, named = 0xB620000CU, static_template = 0x2F90U, terrain_template = 0xAD1U;
// The native adapter's order over fake memory: qualify the actual hit, capture
// beside it by ray ordinal, recheck inside the fence, judge the named-actor-category
// requirement after execute, then publish.
struct Adapter {
    Fixture& f;
    std::vector<std::uint32_t> shapes;
    bool layout{true}, closing_refused{};
    std::function<void()> closing;
    Sidecars<q::maximum_rays> sidecars;
    std::size_t ordinal{};
    std::uint32_t filter{camera};
    q::Result run(const q::Request& selected) {
        auto out = q::execute(selected, 64, 1000, [&](const q::Ray&, unsigned) {
            const auto index = ordinal++;
            const auto shape = Fixture::hit_shape(shapes[index]);
            if (!f.qualified(shape, Fixture::mapped_body)) return q::Probe{{}, 1};
            if ((selected.include_shape_categories || selected.require_named_actor_categories_absent) && layout &&
                !sidecars.capture(index, Fixture::world, Fixture::state, shape, Fixture::mapped_body, f.memory, f.oracle()))
                return q::Probe{{}, 1};
            return q::Probe{{q::Status::hit, {1, 0, 3}, .5f}, 1};
        }, [&] {
            if (ordinal == selected.count && closing) std::exchange(closing, {})();
            return !closing_refused && sidecars.current(f.memory, f.oracle());
        });
        if (out.available && selected.require_named_actor_categories_absent) {
            const auto failure = sidecars.require(out, filter, named);
            if (failure != q::Failure::none) q::refuse(out, failure);
            else out.named_actor_categories_absent = true;
        }
        sidecars.publish(selected, out);
        return out;
    }
};
void empty(const q::Result& out) {
    check(!out.available, "a refused batch was available");
    for (const auto& hit : out.rays)
        check(hit.status == q::Status::unavailable && !hit.in_categories_available && !hit.in_categories, "a refused batch kept a row or category");
}

void batches_publish_only_surviving_optional_values() {
    const std::vector<std::uint32_t> shapes{9, 10, 9};
    {
        Fixture f;
        Adapter a{f, shapes};
        const auto out = a.run(request(3, true));
        check(out.available && out.native_casts == 3 && out.rays[0].in_categories_available && out.rays[0].in_categories == 0x80000000U &&
            out.rays[1].in_categories_available && out.rays[1].in_categories == 1 &&
            out.rays[2].in_categories_available && out.rays[2].in_categories == 0x80000000U,
            "an opted-in batch lost or mixed its per-ray categories");
        check(!f.memory.touched(Fixture::result_buffer, Fixture::result_buffer + 64), "a category read touched native result storage");
    }
    for (const bool requested : {false, true}) {
        // Not requested, or requested with mismatched getter bytes: geometry is
        // unchanged and no category storage is read at all.
        Fixture f;
        Adapter a{f, shapes};
        a.layout = !requested;
        const auto out = a.run(request(3, requested));
        check(out.available && out.native_casts == 3 && out.rays[1].status == q::Status::hit, "the diagnostic changed geometry");
        for (const auto& hit : out.rays) check(!hit.in_categories_available, "an unrequested or unqualified batch carried a category");
        check(!f.memory.count(Fixture::state + 0xfe8), "category storage was read without an opt-in and matching bytes");
    }
    {
        // An optional read failure is only absence.
        Fixture f;
        Adapter a{f, shapes};
        f.memory.bytes.erase(Fixture::element(Fixture::categories, 10));
        const auto out = a.run(request(3, true));
        check(out.available && out.rays[0].in_categories_available && !out.rays[1].in_categories_available &&
            out.rays[1].status == q::Status::hit && out.rays[2].in_categories_available, "an unreadable value refused the batch or leaked");
    }
    {
        // A closing value change downgrades just the rays on that shape.
        Fixture f;
        Adapter a{f, shapes};
        a.closing = [&] { f.memory.put(Fixture::element(Fixture::categories, 9), std::uint32_t{0}); };
        const auto out = a.run(request(3, true));
        check(out.available && !out.rays[0].in_categories_available && out.rays[1].in_categories_available &&
            !out.rays[2].in_categories_available, "publication did not reflect the closing downgrade");
    }
    for (unsigned variant = 0; variant < 3; ++variant) {
        // Identity drift inside a capture or at the closing fence, or an unrelated
        // closing refusal, refuses the whole batch with no category at all.
        Fixture f;
        Adapter a{f, shapes};
        bool changed{};
        if (variant == 0) f.memory.after = [&](std::uintptr_t at) {
            if (changed || at != Fixture::element(Fixture::categories, 10)) return;
            changed = true;
            f.memory.put(Fixture::shape_generations + 10, std::uint8_t{5});
        };
        if (variant == 1) a.closing = [&] { f.memory.put(Fixture::element(Fixture::mappings, 10), std::uint32_t{9}); };
        if (variant == 2) a.closing = [&] { a.closing_refused = true; };
        const auto out = a.run(request(3, true));
        check(out.failure == q::Failure::observation_changed && out.native_casts == (variant == 0 ? 2U : 3U),
            "an identity or fence failure was relabelled or lost its casts");
        empty(out);
        check(variant != 0 || a.sidecars.identity_lost(), "identity drift inside a capture was not sticky");
    }
}

// The named-actor-category check: each hit row, in order, needs its own surviving
// value, overlapping the filter and carrying no named bit. Clear rows pass.
void the_named_check_judges_every_hit_in_order() {
    namespace pe = dingosdk::addr::park_editor;
    check(pe::camera_query_filter == camera && pe::terrain_query_filter == 1 && pe::all_query_filter == 0xffffffffU &&
        pe::named_actor_categories == named, "the Build layer's filters or named categories changed");
    static_assert((camera & named) == 0 && (1U & named) == 0, "a preset filter shares a bit with the named categories");
    const auto result = [](std::initializer_list<q::Status> rows) {
        q::Result out;
        out.available = true;
        out.failure = q::Failure::none;
        out.count = rows.size();
        std::size_t i{};
        for (const auto status : rows) out.rays[i++].status = status;
        return out;
    };
    const auto judged = [&](std::uint32_t first, std::uint32_t second, std::uint32_t filter) {
        Fixture f;
        Sidecars<4> sidecars;
        f.memory.put(Fixture::element(Fixture::categories, 9), first);
        f.memory.put(Fixture::element(Fixture::categories, 10), second);
        check(sidecars.capture(0, Fixture::world, Fixture::state, Fixture::hit_shape(9), Fixture::mapped_body, f.memory, f.oracle()) &&
            sidecars.capture(1, Fixture::world, Fixture::state, Fixture::hit_shape(10), Fixture::mapped_body, f.memory, f.oracle()) &&
            sidecars.available(0) && sidecars.available(1), "a current hit was refused or lost its value");
        return sidecars.require(result({q::Status::hit, q::Status::hit}), filter, named);
    };
    check(judged(static_template, static_template, camera) == q::Failure::none, "a Static-template hit was refused");
    for (const unsigned bit : {2U, 3U, 21U, 25U, 26U, 28U, 29U, 31U})
        check(judged(static_template, static_template | (1U << bit), camera) == q::Failure::hit_named_actor_category,
            "a hit carrying a named actor category passed");
    check(judged(static_template, 0x40U, camera) == q::Failure::hit_categories_inconsistent &&
        judged(static_template, 0, camera) == q::Failure::hit_categories_inconsistent,
        "a hit whose categories miss the filter was not called inconsistent");
    // Within one row the overlap rule comes first; across rows the first row that fails wins.
    check(judged(static_template, 0x4U, camera) == q::Failure::hit_categories_inconsistent, "the rules ran out of order in one row");
    check(judged(static_template | 0x4U, 0x40U, camera) == q::Failure::hit_named_actor_category, "a later row's failure was returned first");
    // The filter is the preset's own: Static lacks the Terrain bit.
    check(judged(terrain_template, terrain_template, 1) == q::Failure::none &&
        judged(static_template, static_template, 1) == q::Failure::hit_categories_inconsistent, "the check ignored the filter");
    {
        // Clear rows pass; a hit without its own value does not.
        Fixture f;
        Sidecars<4> sidecars;
        check(sidecars.require(result({q::Status::clear, q::Status::clear}), camera, named) == q::Failure::none, "clear rows were refused");
        check(sidecars.require(result({q::Status::hit}), camera, named) == q::Failure::hit_categories_unavailable, "an absent value passed");
        f.memory.put(Fixture::element(Fixture::categories, 9), static_template | 0x4U);
        check(sidecars.capture(0, Fixture::world, Fixture::state, Fixture::hit_shape(9), Fixture::mapped_body, f.memory, f.oracle()),
            "a current hit was refused");
        check(sidecars.require(result({q::Status::hit, q::Status::clear}), camera, named) == q::Failure::hit_named_actor_category &&
            sidecars.require(result({q::Status::clear, q::Status::hit}), camera, named) == q::Failure::hit_categories_unavailable,
            "a value was judged on another ray's row");
        q::Result refused;
        refused.failure = q::Failure::observation_changed;
        check(sidecars.require(q::Result{}, camera, named) == q::Failure::unavailable &&
            sidecars.require(refused, camera, named) == q::Failure::observation_changed, "a refused result was vouched for");
    }
    {
        // Unreadable at capture, or dropped at the closing fence: no value, so no claim.
        Fixture f;
        Sidecars<4> sidecars;
        f.memory.put(Fixture::element(Fixture::categories, 9), static_template);
        f.memory.put(Fixture::element(Fixture::categories, 10), static_template);
        f.memory.bytes.erase(Fixture::element(Fixture::categories, 10) + 1);
        check(sidecars.capture(0, Fixture::world, Fixture::state, Fixture::hit_shape(9), Fixture::mapped_body, f.memory, f.oracle()) &&
            sidecars.capture(1, Fixture::world, Fixture::state, Fixture::hit_shape(10), Fixture::mapped_body, f.memory, f.oracle()) &&
            sidecars.available(0) && !sidecars.available(1), "an unreadable value refused the hit or was kept");
        check(sidecars.require(result({q::Status::hit, q::Status::hit}), camera, named) == q::Failure::hit_categories_unavailable,
            "an unreadable value passed");
        f.memory.put(Fixture::element(Fixture::categories, 9), static_template | 1U);
        check(sidecars.current(f.memory, f.oracle()) && !sidecars.available(0), "a changed value survived the closing fence");
        check(sidecars.require(result({q::Status::hit, q::Status::clear}), camera, named) == q::Failure::hit_categories_unavailable,
            "a value dropped at the closing fence passed");
    }
    {
        // A batch whose identity was lost vouches for nothing.
        Fixture f;
        Sidecars<4> sidecars;
        f.memory.put(Fixture::element(Fixture::categories, 9), static_template);
        check(sidecars.capture(0, Fixture::world, Fixture::state, Fixture::hit_shape(9), Fixture::mapped_body, f.memory, f.oracle()),
            "a current hit was refused");
        f.memory.put(Fixture::element(Fixture::mappings, 9), std::uint32_t{9});
        check(!sidecars.current(f.memory, f.oracle()) &&
            sidecars.require(result({q::Status::hit}), camera, named) == q::Failure::hit_categories_unavailable,
            "an identity-lost batch passed the check");
    }
    const std::vector<std::uint32_t> shapes{9, 10, 9};
    const auto selected = [](bool categories) {
        auto out = request(3, categories);
        out.require_named_actor_categories_absent = true;
        return out;
    };
    for (const bool categories : {false, true}) {
        // Through the adapter: the fact is set only after the check, and values
        // appear only when they were also asked for.
        Fixture f;
        Adapter a{f, shapes};
        f.memory.put(Fixture::element(Fixture::categories, 9), static_template);
        f.memory.put(Fixture::element(Fixture::categories, 10), static_template);
        const auto out = a.run(selected(categories));
        check(out.available && out.named_actor_categories_absent && !out.local_actor_excluded && out.native_casts == 3,
            "a passing batch lost its fact or claimed an exclusion");
        for (const auto& hit : out.rays) {
            if (hit.status != q::Status::hit) continue;
            check(hit.in_categories_available == categories && (!categories || hit.in_categories == static_template),
                "the requirement published a value nobody asked for, or lost one");
        }
    }
    for (unsigned variant = 0; variant < 3; ++variant) {
        // A named bit, a missed filter or a closing change refuses the whole batch.
        Fixture f;
        Adapter a{f, shapes};
        f.memory.put(Fixture::element(Fixture::categories, 9), static_template);
        f.memory.put(Fixture::element(Fixture::categories, 10), variant == 0 ? static_template | (1U << 28) : variant == 1 ? 0x40U : static_template);
        if (variant == 2) a.closing = [&] { f.memory.put(Fixture::element(Fixture::categories, 9), static_template | (1U << 26)); };
        const auto out = a.run(selected(true));
        const auto expected = variant == 0 ? q::Failure::hit_named_actor_category : variant == 1 ? q::Failure::hit_categories_inconsistent :
            q::Failure::hit_categories_unavailable;
        check(out.failure == expected && out.native_casts == 3 && !out.named_actor_categories_absent && !out.local_actor_excluded,
            "a failing batch kept its fact, lost its casts or named the wrong rule");
        empty(out);
    }
}

// The owner slot follows the same rules through its own array: state+1008 at the
// hit shape's own element, paired reads between identity checks and a closing
// recheck. Only a request that asked for owner slots sees one.
void owner_slots_use_their_own_array() {
    check(pb::body_owner_offset == 0x1008 && r::unassigned_shape_owner == 0xffffffffU, "the owner slot layout changed");
    Fixture f;
    constexpr std::uintptr_t owners = 0x7c000;
    f.memory.put(Fixture::state + 0x1008, owners);
    f.memory.put(Fixture::element(owners, 9), std::uint32_t{0xffffffffU});
    f.memory.put(Fixture::element(owners, 10), std::uint32_t{0x2a});
    // A decoy at the mapped body's own index.
    f.memory.put(Fixture::element(owners, 57), std::uint32_t{0x99});
    Witness out;
    check(capture(Fixture::world, Fixture::state, Fixture::hit_shape(10), Fixture::mapped_body, out, f.memory, f.oracle(), pb::body_owner_offset) ==
        Capture::available && out.value == 0x2a && out.categories == owners && out.field == pb::body_owner_offset,
        "an owner slot was not read from its own array");
    check(f.memory.count(Fixture::state + 0x1008) == 2 && !f.memory.count(Fixture::state + 0xfe8) &&
        !f.memory.count(Fixture::element(owners, 57)), "an owner read touched categories or the mapped body's element");
    f.memory.reads.clear();
    const auto calls = f.identity_calls;
    check(capture(Fixture::world, Fixture::state, Fixture::hit_shape(10), Fixture::mapped_body, out, f.memory, f.oracle(), 0xff0) ==
        Capture::unavailable && f.memory.reads.empty() && f.identity_calls == calls && !out.world, "an unknown field reached memory");
    Sidecars<4> slots{pb::body_owner_offset}, masks;
    const auto add = [&](Sidecars<4>& sidecars, std::size_t ordinal, std::uint32_t index) {
        return sidecars.capture(ordinal, Fixture::world, Fixture::state, Fixture::hit_shape(index), Fixture::mapped_body, f.memory, f.oracle());
    };
    check(add(slots, 0, 9) && add(slots, 1, 10) && add(masks, 0, 9) && add(masks, 1, 10), "a current hit was refused");
    const auto rows = [] {
        q::Result result;
        result.available = true;
        result.failure = q::Failure::none;
        result.count = 3;
        result.rays[0].status = result.rays[1].status = q::Status::hit;
        result.rays[2].status = q::Status::clear;
        return result;
    };
    q::Request owners_only;
    owners_only.include_hit_owner = true;
    auto result = rows();
    slots.publish(owners_only, result);
    masks.publish(owners_only, result);
    check(result.rays[0].owner_available && !result.rays[0].owner_assigned && result.rays[0].owner == 0xffffffffU &&
        result.rays[1].owner_available && result.rays[1].owner_assigned && result.rays[1].owner == 0x2a &&
        !result.rays[2].owner_available && !result.rays[2].owner_assigned, "owner slots were lost, mixed or put on a clear row");
    check(!result.rays[0].in_categories_available && !result.rays[1].in_categories_available, "an owner-only request published categories");
    q::Request categories_only;
    categories_only.include_shape_categories = true;
    result = rows();
    slots.publish(categories_only, result);
    masks.publish(categories_only, result);
    check(!result.rays[0].owner_available && !result.rays[1].owner_available && result.rays[1].in_categories_available &&
        result.rays[1].in_categories == 1, "a categories-only request published owner slots");
    // Owner slots cannot vouch for the named-actor-category check.
    check(slots.require(rows(), camera, named) == q::Failure::hit_categories_unavailable, "an owner slot was judged as a category");
    // A closing change drops only that slot; an identity change refuses.
    f.memory.put(Fixture::element(owners, 10), std::uint32_t{0x2b});
    check(slots.current(f.memory, f.oracle()) && slots.available(0) && !slots.available(1), "a changed owner slot survived its fence");
    f.memory.put(Fixture::element(Fixture::mappings, 9), std::uint32_t{9});
    check(!slots.current(f.memory, f.oracle()) && slots.identity_lost(), "owner identity drift was only a missing slot");
    result = rows();
    slots.publish(owners_only, result);
    check(!result.rays[0].owner_available, "an identity-lost batch published an owner slot");
}
} // namespace

int main() {
    try {
        the_hit_shape_selects_its_own_element();
        boundary_masks_round_trip_and_zero_is_known();
        malformed_hits_and_field_spans_read_nothing();
        unreadable_or_overflowing_storage_is_only_absence();
        the_getter_bytes_gate_the_diagnostic();
        drift_inside_one_capture_is_classified();
        closing_inspection_downgrades_for_good_and_identity_is_sticky();
        batches_publish_only_surviving_optional_values();
        the_named_check_judges_every_hit_in_order();
        owner_slots_use_their_own_array();
        std::cout << "body shape categories: 10 cases, " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
