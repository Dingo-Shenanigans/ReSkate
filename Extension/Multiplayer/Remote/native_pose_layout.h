#pragma once
#include <cstddef>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <array>
#include <string_view>
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_skater.h"

namespace dingosdk::multiplayer {
struct NativePoseLayout {
    std::uintptr_t buffer{};
    std::uint32_t count{};
};

namespace native_pose_detail {
constexpr std::uintptr_t highest = 0x00007fffffffffffULL;
inline void require(bool valid, const char *message) {
    if (!valid)
        throw std::runtime_error(message);
}
inline std::uintptr_t add(std::uintptr_t address, std::int64_t delta) {
    require(address >= 0x10000 && address <= highest, "Animation base address is invalid.");
    require(delta >= -static_cast<std::int64_t>(address - 0x10000) &&
                delta <= static_cast<std::int64_t>(highest - address),
            "Animation address exceeds bounds.");
    return static_cast<std::uintptr_t>(static_cast<std::int64_t>(address) + delta);
}
template <class T, class Read> T value(Read &read, std::uintptr_t address, const char *field) {
    T result{};
    if (address < 0x10000 || address > highest - sizeof(T) || !read(address, &result, sizeof(T)))
        throw std::runtime_error(std::format("Animation {} is unreadable at {:#x}.", field, address));
    return result;
}
} // namespace native_pose_detail

// The skater entity caches this pointer at +0x628 only during post-construction.
// Resolve the owned factory collection so physics can be disabled immediately.
template <class Read>
std::uintptr_t read_native_component(Read &&read, std::uintptr_t entity, std::uintptr_t vtable) {
    using namespace native_pose_detail;
    const auto collection = value<std::uintptr_t>(read, add(entity, 0x70), "component collection");
    require(collection && value<std::uintptr_t>(read, collection, "component owner") == entity,
            "Skater component collection owner differs.");
    const auto count = value<std::uint8_t>(read, add(collection, 8), "component count");
    require(count <= 128, "Skater component count exceeds bounds.");
    std::uintptr_t result{};
    for (unsigned i = 0; i < count; ++i) {
        const auto component = value<std::uintptr_t>(read, add(collection, 0x20 + i * 0x20), "component");
        if (!component || value<std::uintptr_t>(read, component, "component type") != vtable)
            continue;
        require(!result &&
                    value<std::uintptr_t>(read, add(component, 0x18), "component backlink") == collection,
                "Skater component ownership differs.");
        result = component;
    }
    require(result != 0, "Factory did not publish a skater animation component.");
    return result;
}
template <class Read>
std::uintptr_t read_native_skater_component(Read &&read, std::uintptr_t base, std::uintptr_t entity) {
    using namespace native_pose_detail;
    const auto result = read_native_component(read, entity, base + addr::engine::skater_component_vtable);
    const auto cached = value<std::uintptr_t>(read, add(entity, 0x628), "cached component");
    require(!cached || cached == result, "Skater component cache differs from its collection.");
    return result;
}

struct NativeBoard {
    std::uintptr_t entity{}, component{}, holder{};
};
struct NativeBoardVisual {
    std::uintptr_t holder{}, mesh{};
    std::uint32_t meshes{};
    bool initialized{}, enabled{};
};
// A valid pose buffer can exist before the AnimationSkeletonEntity has been
// initialized or its render resources enabled. Keep these checks independent.
template <class Read>
NativeBoardVisual read_native_board_visual(Read &&read, std::uintptr_t base, std::uintptr_t entity) {
    using namespace native_pose_detail;
    require(value<std::uintptr_t>(read, entity, "board visual entity") == base + addr::engine::board_entity_vtable,
            "Skateboard visual entity type differs.");
    const auto holder = value<std::uintptr_t>(read, add(entity, 0xf0), "board visual holder");
    require(holder &&
                value<std::uintptr_t>(read, holder, "board holder type") ==
                    base + addr::engine::board_holder_vtable &&
                value<std::uintptr_t>(read, add(holder, 0x20), "board holder context") ==
                    value<std::uintptr_t>(read, add(entity, 0x20), "board visual context") &&
                value<std::uintptr_t>(read, add(holder, 0x30), "board holder parent") ==
                    value<std::uintptr_t>(read, add(entity, 0x40), "board visual parent"),
            "Skateboard animation resource ownership differs.");
    const auto mesh = read_native_component(read, entity, base + addr::engine::skater_appearance_vtable);
    const auto begin = value<std::uintptr_t>(read, add(mesh, 0x90), "board mesh begin");
    const auto end = value<std::uintptr_t>(read, add(mesh, 0x98), "board mesh end");
    const auto capacity = value<std::uintptr_t>(read, add(mesh, 0xa0), "board mesh capacity");
    require(begin <= end && end <= capacity && (end - begin) % 8 == 0 && (capacity - begin) % 8 == 0 &&
                (capacity - begin) / 8 <= 64 && (begin || !capacity),
            "Skateboard mesh collection exceeds bounds.");
    const auto flags = value<std::uint64_t>(read, add(holder, 0x28), "board resource flags");
    require((flags & 0x40) == 0, "Skateboard animation resource cannot use normal initialization.");
    return {holder, mesh, static_cast<std::uint32_t>((end - begin) / 8), (flags & 8) != 0,
            value<std::uint8_t>(read, add(holder, 0x94), "board resources enabled") != 0};
}
// Recover a render callback's board only after proving the entity owns this exact holder.
template <class Read>
std::uintptr_t read_native_board_render_owner(Read&& read, std::uintptr_t base, std::uintptr_t holder) {
    using namespace native_pose_detail;
    const auto entity = add(holder, -static_cast<std::int64_t>(addr::native_skater::board_embedded_holder_offset));
    require(read_native_board_visual(read, base, entity).holder == holder,
            "Skateboard render holder backlink differs.");
    return entity;
}
// The native board lookup (addr::native_skater::board_from_component) follows the
// skater component's +0x60 backlink to a separate ClientSkateboardEntity. Its
// animation holder is +0xf0 (addr::native_skater::board_animation_holder).
template <class Read> NativeBoard read_native_board(Read &&read, std::uintptr_t base, std::uintptr_t skater) {
    using namespace native_pose_detail;
    const auto skater_component = read_native_skater_component(read, base, skater);
    const auto component = value<std::uintptr_t>(read, add(skater_component, 0x60), "board component");
    if (!component)
        return {};
    require(value<std::uintptr_t>(read, component, "board component type") ==
                    base + addr::engine::board_component_vtable &&
                value<std::uintptr_t>(read, add(component, 0x58), "board skater backlink") ==
                    skater_component,
            "Local skateboard association differs.");
    const auto collection = value<std::uintptr_t>(read, add(component, 0x18), "board collection");
    const auto entity = value<std::uintptr_t>(read, collection, "board owner");
    require(value<std::uintptr_t>(read, entity, "board entity type") == base + addr::engine::board_entity_vtable &&
                value<std::uintptr_t>(read, add(entity, 0x20), "board context") ==
                    value<std::uintptr_t>(read, add(skater, 0x20), "skater context") &&
                read_native_component(read, entity, base + addr::engine::board_component_vtable) == component,
            "Local skateboard ownership differs.");
    return {entity, component, value<std::uintptr_t>(read, add(entity, 0xf0), "board animation holder")};
}

template <class Read>
std::uintptr_t read_native_board_blueprint(Read &&read, std::uintptr_t base, std::uintptr_t entity) {
    using namespace native_pose_detail;
    const auto data = value<std::uintptr_t>(read, add(entity, 0x48), "board entity data");
    // In the supported Gameplay/Skateboard/Skateboard EBX the root data follows
    // its blueprint header. Check all identities and the root link; never use
    // proximity alone as proof that an address is an asset.
    const auto blueprint = add(data, -0x70);
    require(value<std::uintptr_t>(read, data, "board data type") == base + addr::engine::board_data_vtable &&
                value<std::uintptr_t>(read, blueprint, "board blueprint type") ==
                    base + addr::engine::blueprint_vtable &&
                value<std::uintptr_t>(read, add(blueprint, 8), "board blueprint descriptor") ==
                    base + addr::engine::board_blueprint_type &&
                (value<std::uintptr_t>(read, add(blueprint, 0x68), "board blueprint root") &
                 ~std::uintptr_t{4}) == data,
            "Loaded skateboard blueprint layout differs.");
    constexpr std::string_view expected = "Gameplay/Skateboard/Skateboard";
    std::array<char, expected.size() + 1> name{};
    const auto name_address = value<std::uintptr_t>(read, add(blueprint, 0x18), "board blueprint name");
    require(read(name_address, name.data(), name.size()) && name.back() == 0 &&
                std::string_view(name.data(), expected.size()) == expected,
            "Loaded skateboard blueprint name differs.");
    return blueprint;
}

// Read-only layout shared by in-game capture and the standalone live probe.
// The supported build's count getter (addr::engine::animation_count_getter) reads
// definition+0x1a0 -> +0xc. The native pose resolver (addr::native_skater::pose_output_resolve)
// resolves the current output pose using a *signed* table index;
// the live 395-bone skater uses -1, selecting table word 18.
template <class Read>
NativePoseLayout read_native_pose_layout(Read &&read, std::uintptr_t base, std::uintptr_t holder,
                                         std::size_t bound) {
    using namespace native_pose_detail;
    if (!holder)
        return {};
    const auto rig = value<std::uintptr_t>(read, add(holder, 0x78), "rig");
    if (!rig)
        return {};
    const auto definition = value<std::uintptr_t>(read, add(rig, 0x18), "definition");
    if (!definition)
        return {};
    const auto table_v = value<std::uintptr_t>(read, definition, "definition type");
    const auto method = value<std::uintptr_t>(read, add(table_v, 0x78), "count getter");
    require(method == base + addr::engine::animation_count_getter,
            "Animation count getter differs from the supported build.");
    const auto resource = value<std::uintptr_t>(read, add(definition, 0x1a0), "skeleton resource");
    if (!resource)
        return {};
    const auto count = value<std::uint32_t>(read, add(resource, 0xc), "bone count");
    if (!count)
        return {};
    require(count <= bound, "Skeleton exceeds multiplayer bone limit.");
    const auto header = value<std::uintptr_t>(read, add(rig, 0x20), "output header");
    if (!header)
        return {};
    const auto tagged = value<std::uint64_t>(read, add(header, 0x10), "tagged offset table");
    const auto tag = tagged >> 60;
    require(tag <= 1, "Unsupported animation offset table tag.");
    const auto low = static_cast<std::int64_t>(tagged << 4) >> 4;
    const auto table = tag ? add(add(header, 0x10), low) : static_cast<std::uintptr_t>(low);
    const auto index = value<std::int32_t>(read, table, "signed table index");
    require(index >= -4096 && index <= 4096, "Animation offset table index exceeds bounds.");
    const auto relative = (std::int64_t{10} - std::int64_t{index} * 8) * 4;
    const auto offset = value<std::uint32_t>(read, add(table, relative), "pose offset");
    const auto start = value<std::uint16_t>(read, add(header, 0x1c), "data offset");
    const auto address = start ? add(add(header, start), offset) : static_cast<std::uintptr_t>(offset);
    require(address >= 0x10000 && address <= highest - count * 0x30ULL, "Animation pose address is invalid.");
    return {address, count};
}
} // namespace dingosdk::multiplayer
