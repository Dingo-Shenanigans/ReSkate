#include "skeleton_asset.h"
#include "ebx_fields.h"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dingosdk::frostbite {
namespace {
using ebx::as;
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(std::string("SkeletonAsset: ") + message);
}
const ebx::Value& field(const ebx::Object& object, std::string_view name) {
    return ebx::field(object, name, "SkeletonAsset: a field is missing");
}
}

Skeleton read_skeleton(const ebx::Object& asset) {
    const auto& names = as<ebx::Value::Array>(field(asset, "BoneNames"), "SkeletonAsset: BoneNames is not a list");
    const auto& hierarchy = as<ebx::Value::Array>(field(asset, "Hierarchy"), "SkeletonAsset: Hierarchy is not a list");
    const auto count = names.size();
    require(count > 0 && hierarchy.size() == count, "its lists disagree on the bone count");
    Skeleton result;
    for (std::size_t bone = 0; bone < count; ++bone) {
        result.names.push_back(as<std::string>(names[bone], "SkeletonAsset: a bone name is not text"));
        const auto parent = as<std::int64_t>(hierarchy[bone], "SkeletonAsset: a parent is not a number");
        require(parent >= -1 && parent < static_cast<std::int64_t>(bone), "a bone's parent does not come before it");
        result.parents.push_back(static_cast<int>(parent));
    }
    return result;
}

Skeleton read_ant_skeleton(const ebx::Object& asset) {
    const auto& joints = as<ebx::Value::Array>(field(asset, "Joints"), "SkeletonAsset: Joints is not a list");
    const auto& table = as<ebx::Value::Array>(field(asset, "NameTable"), "SkeletonAsset: NameTable is not a list");
    require(!joints.empty(), "it has no joints");
    std::string names;
    names.reserve(table.size());
    for (const auto& value : table) {
        const auto byte = as<std::int64_t>(value, "SkeletonAsset: a name table entry is not a number");
        require(byte >= -128 && byte <= 127, "a name table entry is not a byte");
        names.push_back(static_cast<char>(byte));
    }
    Skeleton result;
    for (const auto& value : joints) {
        const auto& joint = as<std::shared_ptr<ebx::Object>>(value, "SkeletonAsset: a joint is not an object");
        require(joint != nullptr, "a joint is empty");
        const auto start = as<std::uint64_t>(field(*joint, "Name"), "SkeletonAsset: a joint's name is not a number");
        require(start < names.size(), "a joint's name is outside the name table");
        const auto stop = names.find('\0', static_cast<std::size_t>(start));
        require(stop != std::string::npos, "a joint's name has no end");
        const auto parent = as<std::int64_t>(field(*joint, "Parent"), "SkeletonAsset: a joint's parent is not a number");
        require(parent >= -1 && parent < static_cast<std::int64_t>(result.names.size()), "a joint's parent does not come before it");
        result.names.push_back(names.substr(static_cast<std::size_t>(start), stop - static_cast<std::size_t>(start)));
        result.parents.push_back(static_cast<int>(parent));
    }
    return result;
}
}
