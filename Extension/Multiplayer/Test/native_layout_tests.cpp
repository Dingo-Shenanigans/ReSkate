#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_creation_list.h"
#include "Extension/Multiplayer/Hud/native_indicator_slots.h"
#include "Extension/Multiplayer/Hud/native_nametag_context.h"
#include "Extension/Multiplayer/Remote/native_cosmetics_layout.h"
#include "Extension/Multiplayer/Remote/native_audio_layout.h"
#include "Extension/Multiplayer/Remote/native_audio_binding.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_indicators.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace dingosdk::multiplayer;
namespace {
namespace engine = dingosdk::addr::engine;
namespace indicators = dingosdk::addr::native_indicators;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Memory {
    std::map<std::uintptr_t, std::uint8_t> bytes;
    template <class T> void put(std::uintptr_t address, T value) {
        std::array<std::uint8_t, sizeof(T)> data{};
        std::memcpy(data.data(), &value, sizeof(T));
        for (std::size_t i = 0; i < data.size(); ++i)
            bytes[address + i] = data[i];
    }
    bool operator()(std::uintptr_t address, void *output, std::size_t size) const {
        auto *data = static_cast<std::uint8_t *>(output);
        for (std::size_t i = 0; i < size; ++i) {
            const auto found = bytes.find(address + i);
            if (found == bytes.end())
                return false;
            data[i] = found->second;
        }
        return true;
    }
};
constexpr std::uintptr_t fixture_base = 0x140000000, fixture_holder = 0x20000, fixture_rig = 0x30000,
                         fixture_definition = 0x40000, fixture_vtable = 0x50000, fixture_resource = 0x60000,
                         fixture_header = 0x80000, fixture_table = 0x70000;
Memory fixture() {
    Memory m;
    m.put(fixture_holder + 0x78, fixture_rig);
    m.put(fixture_rig + 0x18, fixture_definition);
    m.put(fixture_definition, fixture_vtable);
    m.put(fixture_vtable + 0x78, fixture_base + engine::animation_count_getter);
    m.put(fixture_definition + 0x1a0, fixture_resource);
    m.put(fixture_resource + 0xc, std::uint32_t{395});
    m.put(fixture_rig + 0x20, fixture_header);
    m.put(fixture_header + 0x10, fixture_table);
    m.put(fixture_header + 0x1c, std::uint16_t{0x30});
    m.put(fixture_table, std::int32_t{-1});
    m.put(fixture_table + 18 * 4, std::uint32_t{0x8c0});
    return m;
}
void indicator_slot_regression() {
    Memory m;
    constexpr std::uintptr_t parent = 0x100000, metadata = 0x200000, slots = 0x300000, inputs = 0x400000,
                             outputs = 0x500000, data = 0x600000;
    m.put(parent + 0x44, std::uint32_t{});
    m.put(parent + 0x78, metadata);
    m.put(parent + 0x58, slots);
    m.put(metadata, inputs);
    m.put(metadata + 8, inputs + 40);
    m.put(metadata + 0x80, outputs);
    m.put(metadata + 0x88, outputs + 24);
    for (auto record : {inputs, outputs}) {
        m.put(record, data);
        m.put(record + 8, std::uint32_t{0x161286ea});
    }
    m.put(inputs + 36, std::uint32_t{3});
    m.put(slots, std::uintptr_t{0x700003});
    m.put(slots + 24, std::uintptr_t{0x800001});
    check(indicator_property(m, parent, data, 0x161286ea, true) == 0x700000,
          "Indicator output slots use ordered indices and clear pointer tags");
    check(indicator_property(m, parent, data, 0x161286ea, false) == 0x800000,
          "Indicator input slots use the connection's explicit index");
    check(indicator_property(m, parent, data + 8, 0x161286ea, true) == 0,
          "Indicator bindings must match the exact blueprint interface");
    constexpr std::uintptr_t child = 0x900000;
    m.put(child + 0x44, std::uint32_t{0x08000000});
    m.put(child + 0x10, parent);
    check(indicator_property(m, child, data, 0x161286ea, false) == 0x800000,
          "Delegating sublevels expose their parent bindings");
    auto rejected = [&](const Memory &bad, std::uintptr_t p, bool output) {
        try {
            indicator_property(bad, p, data, 0x161286ea, output);
        } catch (const std::exception &) {
            return true;
        }
        return false;
    };
    auto bad = m;
    bad.put(inputs + 36, std::uint32_t{UINT32_MAX});
    check(rejected(bad, parent, false), "Reject out-of-range native input slot indices");
    bad = m;
    bad.put(metadata + 0x88, outputs + 25);
    check(rejected(bad, parent, true), "Reject partial native property records");
    bad = m;
    bad.put(metadata + 0x88, outputs + 24 * 8193);
    check(rejected(bad, parent, true), "Bound native property scans");
    bad = m;
    bad.put(child + 0x10, child);
    check(rejected(bad, child, true), "Reject cyclic native property parent chains");
}
void indicator_asset_argument_regression() {
    // Mirrors the crash: a class slot stores an icon reference, but the native
    // setter consumes the icon object, not a pointer to that reference. A
    // primitive slot containing the same bits must keep its storage address.
    constexpr std::uintptr_t slot = 0x100000, type = 0x200000, metadata = 0x300000, storage = 0x400000,
                             icon = 0x500000;
    Memory m;
    m.put(slot, storage);
    m.put(slot + 8, type);
    m.put(slot + 0x24, std::uint32_t{0x68});
    m.put(type, metadata);
    m.put(metadata + 4, std::uint16_t{0x63});
    m.put(metadata + 0x88, fixture_base + indicators::class_reference_getter);
    m.put(storage, icon | 4);
    check(indicator_property_argument(m, fixture_base, slot, type) == icon,
          "Native class setter receives the asset object, with its reference tag removed");
    auto plain = m;
    plain.put(metadata + 4, std::uint16_t{0x300});
    check(indicator_property_argument(plain, fixture_base, slot, type) == storage,
          "Native scalar setter receives value storage, never the handle interpreted as a pointer");
    plain.put(metadata + 4, std::uint16_t{0x45});
    check(indicator_property_argument(plain, fixture_base, slot, type) == storage,
          "Native struct setter also receives value storage");
    auto empty = m;
    empty.put(storage, std::uintptr_t{});
    check(indicator_property_argument(empty, fixture_base, slot, type) == 0,
          "A null asset remains null rather than becoming an address of temporary storage");
    for (unsigned test = 0; test != 4; ++test) {
        auto bad = m;
        if (test == 0)
            bad.put(slot + 8, type + 8);
        if (test == 1)
            bad.put(slot + 0x24, std::uint32_t{0x28});
        if (test == 2)
            bad.put(slot, std::uintptr_t{});
        if (test == 3)
            bad.put(metadata + 0x88, fixture_base + indicators::class_reference_getter + 0x10);
        bool rejected{};
        try {
            indicator_property_argument(bad, fixture_base, slot, type);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "Reject unready/mistyped properties and unsupported class-reference layouts");
    }
}
void nametag_context_regression() {
    constexpr std::uintptr_t type = 0x100000, meta = 0x110000, fields = 0x120000, inherited = 0x130000,
                             inherited_meta = 0x140000, inherited_fields = 0x150000, value = 0x160000;
    constexpr std::uint64_t template_player = 0x17dc60000, remote_player = 0x200010000;
    Memory m;
    m.put(type, meta);
    m.put(meta, std::uint32_t{0xfc296756});
    m.put(meta + 6, std::uint16_t{0x5a0});
    m.put(meta + 0x2a, std::uint16_t{5});
    m.put(meta + 0x60, fields);
    m.put(fields, std::uint32_t{0xbd35f1ed});
    m.put(fields + 8, std::uint16_t{});
    m.put(fields + 16, inherited);
    m.put(inherited, inherited_meta);
    m.put(inherited_meta, std::uint32_t{0xf78df8b4});
    m.put(inherited_meta + 6, std::uint16_t{0x570});
    m.put(inherited_meta + 0x2a, std::uint16_t{11});
    m.put(inherited_meta + 0x60, inherited_fields);
    m.put(inherited_fields, std::uint32_t{0x81d5aa57});
    m.put(inherited_fields + 8, std::uint16_t{});
    m.put(inherited_fields + 16, fixture_base + engine::uint64_type);
    std::array<std::byte, 0x5a0> original;
    for (unsigned i = 0; i < original.size(); ++i)
        original[i] = std::byte((i * 17) & 255);
    std::memcpy(original.data(), &template_player, 8);
    m.put(value, original);
    const auto copy = nametag_context_copy(m, fixture_base, type, value, remote_player);
    std::uint64_t copied_player{};
    std::memcpy(&copied_player, copy.data(), 8);
    check(copied_player == remote_player, "Nametag context references the remote player's own record");
    check(std::memcmp(copy.data() + 8, original.data() + 8, original.size() - 8) == 0,
          "Preserve every authored style/string/model reference after the inherited player handle");
    std::array<std::byte, 0x5a0> unchanged;
    check(m(value, unchanged.data(), unchanged.size()) && unchanged == original,
          "Nametag preparation must not mutate the native session marker context");
    for (unsigned test = 0; test < 6; ++test) {
        auto bad = m;
        if (test == 0)
            bad.put(meta, std::uint32_t{0x5c53e5e2}); // Previous crashing UIPlayerInfo binding.
        if (test == 1)
            bad.put(meta + 6, std::uint16_t{0x108});
        if (test == 2)
            bad.put(inherited_fields + 8, std::uint16_t{8});
        if (test == 3)
            bad.put(inherited_fields + 16, fixture_base + engine::string_type);
        if (test == 4)
            bad.bytes.erase(value + 0x59f);
        bool rejected{};
        try {
            nametag_context_copy(bad, fixture_base, type, value, test == 5 ? 0 : remote_player);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "Reject wrong context type, layout, truncated data, or missing player handle");
    }
}
void creation_regression() {
    constexpr std::uintptr_t page = 0x90000, tail = 0xa0000;
    Memory m;
    NativeCreationList list{0, page, tail, 0, 2, 0};
    m.put(page, std::uintptr_t{0});
    m.put(page + 8, tail);
    for (unsigned i = 0; i < 60; ++i)
        m.put(page + 0x10 + i * 8, std::uintptr_t{0xb0000 + i * 0x100});
    m.put(tail, page | 1);
    m.put(tail + 8, std::uint32_t{2});
    m.put(tail + 0x10, std::uintptr_t{0xc0000});
    m.put(tail + 0x18, std::uintptr_t{0xd0000});
    const auto created = read_native_creation_list(m, list);
    check(created.pages == std::vector<std::uintptr_t>{page, tail} && created.entities.size() == 62 &&
              created.entities[59] == 0xb3b00 && created.entities[60] == 0xc0000 &&
              created.entities[61] == 0xd0000,
          "Blueprint initialization lost entities at a page boundary");
    check(read_native_creation_list(m, {0, tail, tail, 0, 1, 0}).entities.size() == 2,
          "Single blueprint creation page rejected");
    check(read_native_creation_list(m, {}).entities.empty(), "Empty creation list rejected");
    for (unsigned test = 0; test < 10; ++test) {
        auto invalid = m;
        auto bad_list = list;
        if (test == 0)
            invalid.put(tail + 8, std::uint32_t{61});
        if (test == 1)
            invalid.put(tail + 8, std::uint32_t{0});
        if (test == 2)
            invalid.put(page + 8, page);
        if (test == 3)
            invalid.put(page + 8, std::uintptr_t{});
        if (test == 4)
            invalid.put(tail, page);
        if (test == 5)
            invalid.put(tail + 0x18, std::uintptr_t{0xb0000});
        if (test == 6)
            invalid.bytes.erase(tail + 0x10);
        if (test == 7)
            bad_list.pages = 65;
        if (test == 8)
            bad_list.pages = 3;
        if (test == 9)
            bad_list.spare = tail;
        bool rejected{};
        try {
            (void)read_native_creation_list(invalid, bad_list);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "Invalid blueprint creation list reached the native initializer");
    }
}
void regression() {
    auto m = fixture();
    auto pose = read_native_pose_layout(m, fixture_base, fixture_holder, max_skater_bones);
    check(pose.count == 395 && pose.buffer == fixture_header + 0x8f0,
          "Live signed -1 fixture_table layout was rejected");
    const auto relative =
        static_cast<std::int64_t>(fixture_table) - static_cast<std::int64_t>(fixture_header + 0x10);
    m.put(fixture_header + 0x10,
          (std::uint64_t{1} << 60) | (static_cast<std::uint64_t>(relative) & 0x0fffffffffffffffULL));
    check(read_native_pose_layout(m, fixture_base, fixture_holder, max_skater_bones).buffer == pose.buffer,
          "Negative relative fixture_table address was not sign extended");
    m.put(fixture_header + 0x10, fixture_table);
    m.put(fixture_table, std::int32_t{1});
    m.put(fixture_table + 8, std::uint32_t{0x8c0});
    check(read_native_pose_layout(m, fixture_base, fixture_holder, max_skater_bones).buffer == pose.buffer,
          "Positive fixture_table index failed");
    for (unsigned test = 0; test < 5; ++test) {
        auto invalid = fixture();
        if (test == 0)
            invalid.put(fixture_table, std::int32_t{-4097});
        if (test == 1)
            invalid.put(fixture_table, std::int32_t{4097});
        if (test == 2)
            invalid.put(fixture_header + 0x10, std::uint64_t{2} << 60);
        if (test == 3)
            invalid.put(fixture_resource + 0xc, std::uint32_t{513});
        if (test == 4)
            invalid.bytes.erase(fixture_table + 18 * 4);
        bool rejected{};
        try {
            (void)read_native_pose_layout(invalid, fixture_base, fixture_holder, max_skater_bones);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "Invalid or unreadable animation layout accepted");
    }
    m = fixture();
    m.put(fixture_holder + 0x78, std::uintptr_t{});
    check(!read_native_pose_layout(m, fixture_base, fixture_holder, max_skater_bones).buffer,
          "Streaming fixture_rig not handled");
}
void component_regression() {
    Memory m;
    constexpr std::uintptr_t entity = 0x90000, collection = 0xa0000, component = 0xb0000;
    m.put(entity + 0x70, collection);
    m.put(collection, entity);
    m.put(collection + 8, std::uint8_t{1});
    m.put(collection + 0x20, component);
    m.put(component, fixture_base + engine::skater_component_vtable);
    m.put(component + 0x18, collection);
    m.put(entity + 0x628, std::uintptr_t{});
    check(read_native_skater_component(m, fixture_base, entity) == component,
          "Fresh factory component was rejected before its cache was populated");
    m.put(entity + 0x628, component);
    check(read_native_skater_component(m, fixture_base, entity) == component,
          "Initialized component rejected");
    for (unsigned test = 0; test < 3; ++test) {
        auto invalid = m;
        if (test == 0)
            invalid.put(entity + 0x628, component + 0x100);
        if (test == 1)
            invalid.put(component + 0x18, collection + 0x100);
        if (test == 2)
            invalid.put(collection + 8, std::uint8_t{129});
        bool rejected{};
        try {
            (void)read_native_skater_component(invalid, fixture_base, entity);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "Invalid component ownership accepted");
    }
}
void board_regression() {
    Memory m;
    constexpr std::uintptr_t skater = 0x90000, skater_collection = 0xa0000, skater_component = 0xb0000,
                             board = 0xc0000, collection = 0xd0000, component = 0xe0000, blueprint = 0xf0000,
                             data = blueprint + 0x70, name = 0x100000;
    m.put(skater + 0x70, skater_collection);
    m.put(skater_collection, skater);
    m.put(skater_collection + 8, std::uint8_t{1});
    m.put(skater_collection + 0x20, skater_component);
    m.put(skater_component, fixture_base + engine::skater_component_vtable);
    m.put(skater_component + 0x18, skater_collection);
    m.put(skater + 0x628, skater_component);
    m.put(skater_component + 0x60, component);
    m.put(component, fixture_base + engine::board_component_vtable);
    m.put(component + 0x18, collection);
    m.put(component + 0x58, skater_component);
    m.put(collection, board);
    m.put(collection + 8, std::uint8_t{1});
    m.put(collection + 0x20, component);
    m.put(board, fixture_base + engine::board_entity_vtable);
    m.put(board + 0x70, collection);
    m.put(board + 0x20, std::uintptr_t{0x110000});
    m.put(skater + 0x20, std::uintptr_t{0x110000});
    m.put(board + 0xf0, fixture_holder);
    m.put(board + 0x48, data);
    m.put(data, fixture_base + engine::board_data_vtable);
    m.put(blueprint, fixture_base + engine::blueprint_vtable);
    m.put(blueprint + 8, fixture_base + engine::board_blueprint_type);
    m.put(blueprint + 0x68, data | 4);
    m.put(blueprint + 0x18, name);
    constexpr char expected[] = "Gameplay/Skateboard/Skateboard";
    for (std::size_t i = 0; i < sizeof(expected); ++i)
        m.put(name + i, expected[i]);
    const auto found = read_native_board(m, fixture_base, skater);
    check(found.entity == board && found.component == component && found.holder == fixture_holder,
          "Owned skateboard association rejected");
    check(read_native_board_blueprint(m, fixture_base, board) == blueprint,
          "Typed skateboard blueprint rejected");
    for (unsigned test = 0; test < 6; ++test) {
        auto invalid = m;
        if (test == 0)
            invalid.put(component + 0x58, std::uintptr_t{});
        if (test == 1)
            invalid.put(board + 0x20, std::uintptr_t{0x120000});
        if (test == 2)
            invalid.put(collection, skater);
        if (test == 3)
            invalid.put(blueprint + 0x68, data + 8);
        if (test == 4)
            invalid.put(blueprint + 8, fixture_base + engine::board_blueprint_type + 8);
        if (test == 5)
            invalid.put(name, 'X');
        bool rejected{};
        try {
            const auto candidate = read_native_board(invalid, fixture_base, skater);
            (void)read_native_board_blueprint(invalid, fixture_base, candidate.entity);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "Foreign or invalid skateboard accepted");
    }
    m.put(skater_component + 0x60, std::uintptr_t{});
    check(!read_native_board(m, fixture_base, skater).entity, "Absent skateboard not handled");

    constexpr std::uintptr_t mesh = 0x130000, mesh_list = 0x140000, parent = 0x150000;
    m.put(collection + 8, std::uint8_t{2});
    m.put(collection + 0x40, mesh);
    m.put(mesh, fixture_base + engine::skater_appearance_vtable);
    m.put(mesh + 0x18, collection);
    m.put(mesh + 0x90, std::uintptr_t{});
    m.put(mesh + 0x98, std::uintptr_t{});
    m.put(mesh + 0xa0, std::uintptr_t{});
    m.put(board + 0x40, parent);
    m.put(fixture_holder, fixture_base + engine::board_holder_vtable);
    m.put(fixture_holder + 0x20, std::uintptr_t{0x110000});
    m.put(fixture_holder + 0x30, parent);
    m.put(fixture_holder + 0x28, std::uint64_t{0x8006});
    m.put(fixture_holder + 0x94, std::uint8_t{});
    auto visual = read_native_board_visual(m, fixture_base, board);
    check(!visual.initialized && !visual.enabled && !visual.meshes,
          "Uninitialized board incorrectly reported render-ready");
    m.put(fixture_holder + 0x28, std::uint64_t{0x900e});
    m.put(fixture_holder + 0x94, std::uint8_t{1});
    m.put(mesh + 0x90, mesh_list);
    m.put(mesh + 0x98, mesh_list + 3 * 8);
    m.put(mesh + 0xa0, mesh_list + 4 * 8);
    visual = read_native_board_visual(m, fixture_base, board);
    check(visual.initialized && visual.enabled && visual.meshes == 3,
          "Initialized board meshes not recognized");
    for (unsigned test = 0; test < 7; ++test) {
        auto invalid = m;
        if (test == 0)
            invalid.put(fixture_holder + 0x20, std::uintptr_t{0x120000});
        if (test == 1)
            invalid.put(fixture_holder + 0x30, parent + 8);
        if (test == 2)
            invalid.put(fixture_holder, fixture_base + engine::board_holder_vtable + 8);
        if (test == 3)
            invalid.put(mesh + 0x98, mesh_list - 8);
        if (test == 4)
            invalid.put(mesh + 0xa0, mesh_list + 65 * 8);
        if (test == 5)
            invalid.put(mesh + 0x98, mesh_list + 3);
        if (test == 6)
            invalid.put(mesh + 0x18, skater_collection);
        bool rejected{};
        try {
            (void)read_native_board_visual(invalid, fixture_base, board);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "Unsafe skateboard render resource accepted");
    }
}
void probe(DWORD pid, std::uintptr_t entity) {
    const auto process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    check(process != nullptr, "Cannot open game for read-only inspection");
    struct Close {
        HANDLE handle;
        ~Close() { CloseHandle(handle); }
    } close{process};
    auto read = [&](std::uintptr_t address, void *out, std::size_t size) {
        SIZE_T done{};
        return ReadProcessMemory(process, reinterpret_cast<const void *>(address), out, size, &done) &&
               done == size;
    };
    auto pointer = [&](std::uintptr_t address) {
        return native_pose_detail::value<std::uintptr_t>(read, address, "live pointer");
    };
    check(pointer(entity) == fixture_base + engine::skater_entity_vtable, "Live entity type differs");
    const auto component = read_native_skater_component(read, fixture_base, entity);
    check(pointer(component) == fixture_base + engine::skater_component_vtable, "Live skater component type differs");
    const auto pose =
        read_native_pose_layout(read, fixture_base, pointer(component + 0xa0), max_skater_bones);
    check(pose.buffer && pose.count, "Live pose not ready");
    struct Bone {
        std::array<float, 4> scale, rotation, position;
    };
    static_assert(sizeof(Bone) == 0x30);
    std::vector<Bone> bones(pose.count);
    check(read(pose.buffer, bones.data(), bones.size() * sizeof(Bone)), "Live pose is unreadable");
    for (const auto &bone : bones) {
        const Transform t{{bone.position[0], bone.position[1], bone.position[2]},
                          bone.rotation,
                          {bone.scale[0], bone.scale[1], bone.scale[2]}};
        check(valid_transform(t), "Live pose contains an invalid transform");
    }
    std::cout << "Read-only live probe: " << pose.count << " valid bone transforms; pose buffer=0x"
              << std::hex << pose.buffer << ". No game memory was changed.\n";
    const auto board = read_native_board(read, fixture_base, entity);
    check(board.entity != 0, "Live skateboard not ready");
    const auto blueprint = read_native_board_blueprint(read, fixture_base, board.entity);
    const auto board_pose = read_native_pose_layout(read, fixture_base, board.holder, max_board_bones - 1);
    check(board_pose.buffer && board_pose.count, "Live skateboard rig not ready");
    bones.resize(board_pose.count);
    check(read(board_pose.buffer, bones.data(), bones.size() * sizeof(Bone)),
          "Live skateboard pose unreadable");
    for (const auto &bone : bones)
        check(valid_transform({{bone.position[0], bone.position[1], bone.position[2]},
                               bone.rotation,
                               {bone.scale[0], bone.scale[1], bone.scale[2]}}),
              "Invalid live skateboard bone");
    std::cout << "Read-only skateboard probe: " << std::dec << board_pose.count << " valid bones; entity=0x"
              << std::hex << board.entity << "; blueprint=0x" << blueprint << ".\n";
    const auto visual = read_native_board_visual(read, fixture_base, board.entity);
    std::cout << "Skateboard render resources: initialized=" << visual.initialized
              << ", enabled=" << visual.enabled << ", meshes=" << std::dec << visual.meshes << ".\n";
    const CosmeticMemory cosmetics{read, fixture_base};
    Appearance appearance{cosmetics.capture(entity), cosmetics.capture(board.entity)};
    check(valid_appearance(appearance), "Live cosmetic recipes are invalid");
    Packet packet;
    packet.kind = PacketKind::cosmetics;
    packet.session = 1;
    packet.epoch = 1;
    packet.map = 1;
    packet.appearance = appearance;
    const auto bytes = encode(packet);
    const auto decoded = decode(bytes);
    check(decoded && decoded->appearance == appearance, "Live cosmetic packet does not round trip");
    std::cout << "Read-only cosmetics probe: " << appearance.skater.items.size() << " skater slots, "
              << appearance.board.items.size() << " board slots, " << bytes.size()
              << " wire bytes. Slot/category/parameter definitions validated against loaded assets.\n";
}
void cosmetic_layout_regression() {
    const CosmeticRecipe recipe{
        skater_recipe_key, 2, {0, 0x3f000000}, {{11, "Own_Shirt", {5}}, {12, "", {}}}};
    BorrowedCosmeticRecipe borrowed(recipe);
    const auto read_self = [](std::uintptr_t p, void *out, std::size_t size) {
        SIZE_T done{};
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(p), out, size, &done) &&
               done == size;
    };
    const CosmeticMemory live{read_self, fixture_base};
    check(live.array<std::uint32_t>(reinterpret_cast<std::uintptr_t>(borrowed.value.scalars), 64) ==
              recipe.scalars,
          "Borrowed native scalar header/layout differs");
    const auto items =
        live.array<NativeCosmeticItem>(reinterpret_cast<std::uintptr_t>(borrowed.value.items), 64);
    check(items.size() == 2 && items[0].hash == cosmetic_asset_hash("Own_Shirt") && items[0].slot == 11 &&
              items[1].asset && !*items[1].asset && items[1].parameters && !items[1].hash,
          "Borrowed native item layout or empty native values differ");
    check(live.array<std::uint32_t>(reinterpret_cast<std::uintptr_t>(items[0].parameters), 128) ==
                  recipe.items[0].parameters &&
              live.count(reinterpret_cast<std::uintptr_t>(borrowed.value.mask), 4, 64) == 0 &&
              !borrowed.value.word && !borrowed.value.flag,
          "Native transient state must not be copied from peers");
    Memory m;
    constexpr std::uintptr_t res = 0x100000, slots = 0x110000, slot = 0x120000, scalars = 0x130000,
                             parameters = 0x140000, manager = 0x150000, buckets = 0x160000, node = 0x170000,
                             asset = 0x180000, name = 0x190000, categories = 0x200000, category = 0x210000;
    m.put(res + 0x38, recipe.key);
    m.put(res + 0x3c, recipe.version);
    m.put(res + 0x28, slots);
    m.put(res + 0x30, scalars);
    m.put(slots - 4, std::uint32_t{1});
    m.put(scalars - 4, std::uint32_t{2});
    m.put(slots + 8, slot | 4);
    m.put(slot + 8, fixture_base + engine::cosmetic_slot_type);
    m.put(slot + 0x4c, std::uint32_t{11});
    m.put(slot + 0x18, parameters);
    m.put(parameters - 4, std::uint32_t{1});
    m.put(slot + 0x30, category | 4);
    m.put(category + 0x38, std::uint32_t{77});
    m.put(fixture_base + engine::cosmetics_manager, manager);
    m.put(manager, fixture_base + engine::cosmetics_manager_vtable);
    m.put(manager + 0xa8, std::uint8_t{1});
    m.put(manager + 0x30, buckets);
    m.put(manager + 0x38, std::uint32_t{1});
    m.put(manager + 0x3c, std::uint32_t{1});
    m.put(buckets, node);
    m.put(node, cosmetic_asset_hash("Own_Shirt"));
    m.put(node + 8, asset | 4);
    m.put(node + 0x10, std::uintptr_t{0});
    m.put(asset + 0x38, name);
    m.put(asset + 0x48, cosmetic_asset_hash("Own_Shirt"));
    m.put(asset + 0x20, categories);
    m.put(categories - 4, std::uint32_t{1});
    m.put(categories, category | 4);
    const std::string text = "Own_Shirt";
    for (std::size_t i = 0; i <= text.size(); ++i)
        m.put(name + i, text.c_str()[i]);
    auto one = recipe;
    one.items.resize(1);
    CosmeticMemory{std::cref(m), fixture_base}.validate(res, one);
    const auto reject = [&](const Memory &bad, const CosmeticRecipe &r) {
        try {
            CosmeticMemory{std::cref(bad), fixture_base}.validate(res, r);
        } catch (const std::exception &) {
            return true;
        }
        return false;
    };
    auto wrong = one;
    wrong.items[0].slot++;
    check(reject(m, wrong), "Native validation must reject wrong slot order");
    wrong = one;
    wrong.items[0].parameters.clear();
    check(reject(m, wrong), "Native validation must reject wrong parameter shape");
    wrong = one;
    wrong.scalars.clear();
    check(reject(m, wrong), "Native validation must reject wrong scalar shape");
    wrong = one;
    wrong.items[0].asset = "Unknown";
    check(reject(m, wrong), "Native validation must reject missing assets");
    // A peer's modded item becomes the first installed default for its slot, or nothing.
    const CosmeticMemory fake{std::cref(m), fixture_base};
    auto modded = wrong;
    const std::array<std::string_view, 2> fallbacks{"Own_NotInstalled", "Own_Shirt"};
    const auto swaps = fake.substitute_missing(res, modded, fallbacks);
    check(swaps.size() == 1 && swaps[0].first == "Unknown" && swaps[0].second == "Own_Shirt" &&
              modded.items[0].asset == "Own_Shirt" && modded.items[0].parameters == one.items[0].parameters &&
              !reject(m, modded),
          "A missing peer item must become the installed default for its slot");
    modded = wrong;
    const std::array<std::string_view, 1> none{"Own_NotInstalled"};
    fake.substitute_missing(res, modded, none);
    check(modded.items[0].asset.empty() && !reject(m, modded), "A missing peer item without a default must become empty");
    modded = one;
    check(fake.substitute_missing(res, modded, fallbacks).empty() && modded == one, "Installed peer items must stay");
    // A peer whose mods add slots (or who lacks this game's): items go to this game's slots by slot hash.
    const auto own = [](std::uint32_t, std::size_t size) { return std::vector<std::uint32_t>(size, 7); };
    modded = one;
    check(fake.fit_slots(res, modded, own) == std::pair<std::size_t, std::size_t>{} && modded == one,
          "A recipe with this game's slots must not be touched");
    modded.items = {{99, "Own_Sticker", {1, 2}}, one.items[0]};
    check(fake.fit_slots(res, modded, own) == std::pair<std::size_t, std::size_t>{0, 1} && modded == one && !reject(m, modded),
          "An item in a slot this game lacks must be left out, and the rest kept");
    modded.items = {{99, "Own_Sticker", {1, 2}}};
    check(fake.fit_slots(res, modded, own) == std::pair<std::size_t, std::size_t>{1, 1} && modded.items.size() == 1 &&
              modded.items[0] == CosmeticSlot{11, "", {7}} && !reject(m, modded),
          "A slot the peer lacks must stay empty with this player's parameters");
    modded = one;
    modded.items[0].parameters = {1, 2, 3};
    check(fake.fit_slots(res, modded, own) == std::pair<std::size_t, std::size_t>{} &&
              modded.items[0] == CosmeticSlot{11, "Own_Shirt", {7}} && !reject(m, modded),
          "An item whose slot takes other parameters here must keep its place");
    auto bad = m;
    bad.put(categories, category + 0x100);
    bad.put(category + 0x138, std::uint32_t{88});
    check(reject(bad, one), "Native validation must reject wrong asset categories");
    bad = m;
    bad.put(node, std::uint32_t{9});
    bad.put(node + 0x10, node);
    check(reject(bad, one), "Catalog cycles must terminate");
    bad = m;
    bad.put(slots - 4, std::uint32_t{0x7fffffff});
    check(reject(bad, one), "Native cosmetic arrays must be bounded");
}
void sound_binding_regression() {
    Memory m;
    constexpr std::uintptr_t local = 0x100000, remote = 0x110000, local_collection = 0x200000,
                             remote_collection = 0x210000, source = 0x300000, peer = 0x310000,
                             manager = 0x400000, context = 0x500000, data = 0x600000;
    for (const auto actor : {local, remote}) {
        const auto collection = actor == local ? local_collection : remote_collection;
        const auto component = actor == local ? source : peer;
        m.put(actor, fixture_base + engine::skater_entity_vtable);
        m.put(actor + 0x20, context);
        m.put(actor + 0xf8, actor == local ? std::uintptr_t{0x700000} : std::uintptr_t{});
        m.put(actor + 0x70, collection);
        m.put(collection, actor);
        m.put(collection + 8, std::uint8_t{1});
        m.put(collection + 0x20, component);
        m.put(component, fixture_base + engine::skater_audio_component_vtable);
        m.put(component + 8, data);
        m.put(component + 0x18, collection);
        m.put(component + 0x74, static_cast<std::uint16_t>(actor == local ? 3 : 5));
        m.put(component + 0x76, std::uint8_t{});
    }
    m.put(fixture_base + engine::audio_manager, manager);
    m.put(manager, fixture_base + engine::audio_manager_vtable);
    m.put(fixture_base + engine::audio_manager_vtable + 0x20, fixture_base + engine::audio_manager_update);
    const auto binding = read_native_audio_binding(m, fixture_base, local, remote, context);
    check(binding.component == peer && binding.manager == manager && binding.handle == 5,
          "Already-created native remote voice must be reused, not rejected");
    for (const auto handle : {std::uint16_t{0}, std::uint16_t{7}}) {
        m.put(peer + 0x74, handle);
        check(read_native_audio_binding(m, fixture_base, local, remote, context).handle == handle,
              "Native sound handle creation/replacement must be re-read from the component");
    }
    for (unsigned failure = 0; failure < 8; ++failure) {
        auto bad = m;
        if (failure == 0)
            bad.put(peer + 0x74, std::uint16_t{3});
        if (failure == 1)
            bad.put(peer + 0x74, std::uint16_t{1});
        if (failure == 2)
            bad.put(peer + 0x76, std::uint8_t{1});
        if (failure == 3)
            bad.put(remote + 0xf8, std::uintptr_t{0x700000});
        if (failure == 4)
            bad.put(remote + 0x20, context + 8);
        if (failure == 5)
            bad.put(peer + 0x18, local_collection);
        if (failure == 6)
            bad.put(peer + 8, data + 8);
        if (failure == 7)
            bad.put(manager, fixture_base + engine::audio_manager_vtable + 8);
        bool rejected = false;
        try {
            (void)read_native_audio_binding(bad, fixture_base, local, remote, context);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "Unsafe local/foreign/stale sound binding accepted");
    }
}
void sound_layout_regression() {
    AudioState s;
    for (std::size_t i = 0; i < s.values.size(); ++i)
        s.values[i] = static_cast<float>(i) * .125f;
    for (std::size_t i = 0; i < s.selectors.size(); ++i)
        s.selectors[i] = 0xf0000000U + static_cast<std::uint32_t>(i);
    for (std::size_t i = 1; i < s.flags.size(); ++i)
        s.flags[i] = i % 2;
    Pose p;
    p.root.position = {40, 50, 60};
    p.board.resize(1);
    p.board[0].position = {41, 49, 62};
    const auto f = audio_frame(s, p);
    check(audio_state(f) == s, "Native audio values do not round trip independently of padding");
    check(audio_value<std::array<float, 16>>(f, 0) == to_matrix(p.root) &&
              audio_value<std::array<float, 16>>(f, 0x40) == to_matrix(p.board[0]),
          "Native sound must follow the buffered skater and board positions");
    check(audio_value<std::uint8_t>(f, 0x1ec) == 0 && audio_value<std::uint8_t>(f, 0x217) == 0 &&
              audio_value<std::uint32_t>(f, 0xc0) == 0x07ffffff,
          "Sound local-player flag, padding or field masks differ");
    std::array<bool, 0x218> used{};
    const auto mark = [&](std::size_t offset, std::size_t count) {
        check(offset + count <= used.size(), "Native audio field out of bounds");
        for (std::size_t i = offset; i < offset + count; ++i) {
            check(!used[i], "Overlapping native sound fields");
            used[i] = true;
        }
    };
    for (const auto off : audio_float_offsets)
        mark(off, 4);
    for (const auto off : audio_selector_offsets)
        mark(off, 4);
    for (const auto off : {0xc0, 0x124, 0x130, 0x164})
        mark(off, 4);
    mark(0, 0xb0);
    mark(0x1ec, 43);
    auto invalid = s;
    invalid.values[0] = std::numeric_limits<float>::infinity();
    bool rejected = false;
    try {
        (void)audio_frame(invalid, p);
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, "Nonfinite values reached the native audio frame");
}
} // namespace
int main(int argc, char **argv) {
    try {
        regression();
        component_regression();
        board_regression();
        creation_regression();
        indicator_slot_regression();
        indicator_asset_argument_regression();
        nametag_context_regression();
        cosmetic_layout_regression();
        sound_layout_regression();
        sound_binding_regression();
        if (argc == 4 && std::string_view(argv[1]) == "--probe")
            probe(static_cast<DWORD>(std::stoul(argv[2])), std::stoull(argv[3], nullptr, 0));
        else
            check(argc == 1, "Usage: native_tests [--probe PID entity_address]");
        std::cout << "Native animation layout regressions passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
