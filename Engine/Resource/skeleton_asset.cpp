#include "skeleton_asset.h"
#include <stdexcept>
#include <string_view>

namespace dingosdk::frostbite {
namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(std::string("SkeletonAsset: ") + message);
}
template <class T> const T& as(const ebx::Value& value, const char* what) {
    const auto* result = std::get_if<T>(&value.data);
    require(result != nullptr, what);
    return *result;
}
const ebx::Value& field(const ebx::Object& object, std::string_view name) {
    const auto* found = object.find(name);
    require(found != nullptr, "a field is missing");
    return found->value;
}
}

Skeleton read_skeleton(const ebx::Object& asset) {
    const auto& names = as<ebx::Value::Array>(field(asset, "BoneNames"), "BoneNames is not a list");
    const auto& hierarchy = as<ebx::Value::Array>(field(asset, "Hierarchy"), "Hierarchy is not a list");
    const auto count = names.size();
    require(count > 0 && hierarchy.size() == count, "its lists disagree on the bone count");
    Skeleton result;
    for (std::size_t bone = 0; bone < count; ++bone) {
        result.names.push_back(as<std::string>(names[bone], "a bone name is not text"));
        const auto parent = as<std::int64_t>(hierarchy[bone], "a parent is not a number");
        require(parent >= -1 && parent < static_cast<std::int64_t>(bone), "a bone's parent does not come before it");
        result.parents.push_back(static_cast<int>(parent));
    }
    return result;
}
}
