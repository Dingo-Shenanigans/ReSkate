#pragma once
#include "placement_geometry.h"
#include <map>
#include <set>
#include <vector>
namespace dingosdk::placement_adapters {
using placement_geometry::Pose;
using placement_geometry::Vec3;
struct Body {
    std::uintptr_t world{};
    std::uint32_t index{UINT32_MAX}, generation{};
    auto operator<=>(const Body&) const = default;
};
static_assert(sizeof(Body) == 16);
struct Published {
    std::uint64_t id{};
    Body body{};
    bool surface{};
};
struct Edit {
    std::uint64_t id{};
    Pose pose{};
    bool committed{};
};
struct Inbox {
    std::vector<Edit> edits;
    std::set<std::uint64_t> selected;
};
bool start(std::uintptr_t base);
// Exact validated server/client root allowlist. No game lock is taken by hooks.
void publish(const std::map<std::uint64_t, Published>& roots);
Inbox drain();
void support(std::uintptr_t context, bool enabled, Body body = {}, Vec3 point = {});
void reset();
void edited(std::uint64_t entity, const Pose& pose, bool committed);
} // namespace dingosdk::placement_adapters
