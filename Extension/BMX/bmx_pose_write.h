#pragma once
#include <Windows.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace dingosdk::bmx::rider::pose_write {
inline constexpr std::uintptr_t highest = 0x00007fffffffffffULL;
inline constexpr std::size_t bone_stride = 48, scale_offset = 0, rotation_offset = 16, position_offset = 32;
enum class Lane { rotation, position, board_scale };

// SDK-only revision join. The enclosing rider mutex protects this state and
// its corresponding Frame/contact payload. A successful receipt establishes
// all requested prop setter calls completed, never a native/GPU lifetime.
class PlacementGate {
    std::uint64_t revision_{};std::uintptr_t entity_{};
    bool candidate_{},active_{},admitted_{};
    std::uint64_t advance() noexcept {
        candidate_=active_=admitted_=false;entity_=0;
        if(revision_==std::numeric_limits<std::uint64_t>::max())return 0;
        return ++revision_;
    }
public:
    explicit PlacementGate(std::uint64_t initial=0) noexcept:revision_(initial){}
    std::uint64_t begin_tick() noexcept {return advance();}
    std::uint64_t publish(bool active,std::uintptr_t entity) noexcept {
        const auto revision=advance();if(!revision)return 0;
        candidate_=true;active_=active;entity_=entity;return revision;
    }
    bool has_candidate() const noexcept {return candidate_;}
    bool ready(std::uint64_t revision) const noexcept {return revision&&revision==revision_&&admitted_;}
    bool complete(std::uint64_t revision,std::uintptr_t entity,bool all_parts,bool fresh) noexcept {
        if(!revision||revision!=revision_||!candidate_)return false;
        admitted_=active_&&entity_&&entity==entity_&&all_parts&&fresh;return admitted_;
    }
};

// The shared native publisher places the selected output pointer at
// RenderPoseData+0x10. Compare only during that borrowed callback invocation.
// This grants neither ownership nor a write lease; the caller must independently
// prove local owner, holder, output count and current pose before every write.
template<class Read>
inline bool published_buffer_matches(Read&& read,std::uintptr_t render_data,std::uintptr_t expected) noexcept {
    if(render_data<0x10000||render_data>highest-0x10-sizeof(std::uintptr_t)||
       expected<0x10000||expected>highest||expected%alignof(float))return false;
    std::uintptr_t published{};
    return read(render_data+0x10,&published,sizeof(published))&&published==expected;
}

// Revalidate the native transforms used to convert world-space hips targets to
// parent-local space. Read one bone at a time; neither metadata W lane belongs
// to the transform. This is a bounded pre-write check, not an atomic pose lock.
template<class Bone, class Read>
inline bool ancestors_unchanged(Read&& read, std::uintptr_t buffer, std::size_t count,
                               std::span<const Bone> original, std::span<const int> parents,
                               int hips) noexcept {
    static_assert(sizeof(Bone) == bone_stride && offsetof(Bone, scale) == scale_offset &&
        offsetof(Bone, rotation) == rotation_offset && offsetof(Bone, position) == position_offset);
    if (!count || count > 512 || parents.empty() || parents.size() > count ||
        original.size() < parents.size() || hips < 0 || static_cast<std::size_t>(hips) >= parents.size() ||
        buffer < 0x10000 || buffer % alignof(float) || buffer > highest - count * bone_stride) return false;
    int child = hips;
    for (std::size_t step = 0; step < parents.size(); ++step) {
        const int parent = parents[static_cast<std::size_t>(child)];
        if (parent == -1) return child != hips;
        // The solved hierarchy is topologically ordered. Strict descent also
        // rejects a cycle without allocating a visited set or following it.
        if (parent < 0 || parent >= child) return false;
        Bone live{};
        if (!read(buffer + static_cast<std::size_t>(parent) * bone_stride, &live, sizeof(live))) return false;
        const auto& before = original[static_cast<std::size_t>(parent)];
        for (unsigned axis = 0; axis < 3; ++axis)
            if (!std::isfinite(live.scale[axis]) || !std::isfinite(live.position[axis]) ||
                live.scale[axis] != before.scale[axis] || live.position[axis] != before.position[axis]) return false;
        for (unsigned axis = 0; axis < 4; ++axis)
            if (!std::isfinite(live.rotation[axis]) || live.rotation[axis] != before.rotation[axis]) return false;
        child = parent;
    }
    return false;
}

// Only the current, independently validated rider pose may call this helper.
// It does not discover ownership or grant page access. Stage the small source
// first, then copy exactly one changed lane under the same-process SEH boundary.
inline bool write(std::uintptr_t buffer, std::size_t count, std::size_t index,
                  Lane lane, const float* values) noexcept {
    if (!count || count > 512 || index >= count || buffer < 0x10000 || buffer % alignof(float) ||
        buffer > highest - count * bone_stride || !values ||
        (lane != Lane::rotation && lane != Lane::position && lane != Lane::board_scale)) return false;
    // The supported board has two native trajectory/root channels. Its scale
    // writes are exclusively the14 render descendants, XYZ only.
    if (lane == Lane::board_scale && (count != 16 || index < 2)) return false;
    const auto components = lane == Lane::rotation ? 4u : 3u;
    const auto bytes = components * sizeof(float);
    const auto source = reinterpret_cast<std::uintptr_t>(values);
    if (source < 0x10000 || source % alignof(float) || source > highest - bytes) return false;
    const auto destination = buffer + index * bone_stride +
        (lane == Lane::rotation ? rotation_offset : lane == Lane::position ? position_offset : scale_offset);
    // A generic cross-page memcpy can partly write before faulting. Every
    // supported lane fits in one minimum Windows x64 page; reject a crossing
    // without querying or changing protection. No atomic-write claim is made.
    if ((destination & 0xfffu) > 4096 - bytes) return false;
    alignas(16) float staged[4]{};
    __try { std::memcpy(staged, values, bytes); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    for (unsigned i = 0; i < components; ++i) if (!std::isfinite(staged[i])) return false;
    __try { std::memcpy(reinterpret_cast<void*>(destination), staged, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace dingosdk::bmx::rider::pose_write
