#pragma once
#include "Engine/Game/World/network_objects.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>

namespace dingosdk::multiplayer {
inline constexpr std::size_t max_owned_objects = 1024, object_chunk_entries = 64, max_object_parts = 32;
struct ObjectChunk {
    std::uint64_t base{}, revision{}; // base=0 replaces the owner's complete layout.
    std::uint16_t part{}, parts{1};
    std::vector<NetworkObject> objects;
    std::vector<std::uint64_t> removed;
};
inline bool valid_network_object(const NetworkObject &object) {
    if (!object.id || object.item.size() > 256 || !object.item.starts_with("own_bk") ||
        !std::all_of(object.item.begin(), object.item.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }))
        return false;
    for (float value : object.position)
        if (!std::isfinite(value) || std::abs(value) > 100000)
            return false;
    if (!std::isfinite(object.scale) || object.scale < .01f || object.scale > 100.0f)
        return false;
    float norm{};
    for (float value : object.rotation) {
        if (!std::isfinite(value))
            return false;
        norm += value * value;
    }
    return std::isfinite(norm) && norm > .98f && norm < 1.02f;
}
inline bool valid_object_chunk(const ObjectChunk &chunk) {
    if (!chunk.revision || chunk.base >= chunk.revision || !chunk.parts || chunk.parts > max_object_parts ||
        chunk.part >= chunk.parts || chunk.objects.size() + chunk.removed.size() > object_chunk_entries ||
        (!chunk.base && !chunk.removed.empty()))
        return false;
    std::set<std::uint64_t> ids;
    for (const auto &object : chunk.objects)
        if (!valid_network_object(object) || !ids.insert(object.id).second)
            return false;
    for (const auto id : chunk.removed)
        if (!id || !ids.insert(id).second)
            return false;
    return true;
}
// What of an owner's uploaded `layout` everyone else is shown under a limit of `limit` objects
// (0: all of it). The objects already shown (`shared`) keep their place, so placing one too
// many never takes away one that is there; the rest fill what room is left, oldest ID first.
inline std::vector<NetworkObject> limited_layout(std::vector<NetworkObject> layout, const std::map<std::uint64_t, NetworkObject> &shared,
                                                 std::size_t limit) {
    if (!limit || layout.size() <= limit) return layout;
    std::stable_partition(layout.begin(), layout.end(), [&](const NetworkObject &object) { return shared.contains(object.id); });
    layout.resize(limit);
    return layout;
}
// Per-owner state. A multipart replacement/delta becomes visible only after all
// parts validate. Reliable delivery keeps parts ordered; stale revisions cannot
// resurrect deleted objects. No wire IDs are native entity handles.
class ObjectState {
    using Layout = std::map<std::uint64_t, NetworkObject>;
    Layout objects_;
    std::uint64_t revision_{};
    ObjectChunk latest_;
    struct Pending {
        ObjectChunk change;
        Layout layout;
        std::set<std::uint64_t> touched;
        unsigned next{};
    };
    std::optional<Pending> pending_;

  public:
    std::uint64_t revision() const {
        return revision_;
    }
    const Layout &objects() const {
        return objects_;
    }
    std::vector<NetworkObject> layout() const {
        std::vector<NetworkObject> result;
        result.reserve(objects_.size());
        for (const auto &[id, object] : objects_) {
            (void)id;
            result.push_back(object);
        }
        return result;
    }
    void replace(std::span<const NetworkObject> objects) {
        if (objects.size() > max_owned_objects)
            throw std::invalid_argument("Too many owned objects");
        Layout next;
        for (const auto &object : objects)
            if (!valid_network_object(object) || !next.emplace(object.id, object).second)
                throw std::invalid_argument("Invalid owned object");
        if (revision_ && next == objects_)
            return;
        if (revision_ == UINT64_MAX)
            throw std::runtime_error("Object revision exhausted");
        latest_ = {};
        latest_.base = revision_;
        latest_.revision = ++revision_;
        for (const auto &[id, object] : next) {
            const auto found = objects_.find(id);
            if (found == objects_.end() || found->second != object)
                latest_.objects.push_back(object);
        }
        for (const auto &[id, object] : objects_) {
            (void)object;
            if (!next.contains(id))
                latest_.removed.push_back(id);
        }
        objects_ = std::move(next);
    }
    std::vector<ObjectChunk> updates(std::uint64_t since) const {
        if (!revision_ || since == revision_)
            return {};
        ObjectChunk change;
        if (since && since == latest_.base)
            change = latest_;
        else {
            change.revision = revision_;
            change.objects = layout();
        }
        const auto count = change.objects.size() + change.removed.size();
        const auto parts =
            std::max<std::size_t>(1, (count + object_chunk_entries - 1) / object_chunk_entries);
        std::vector<ObjectChunk> result(parts);
        for (std::size_t part = 0; part < parts; ++part) {
            auto &chunk = result[part];
            chunk.base = change.base;
            chunk.revision = revision_;
            chunk.part = static_cast<std::uint16_t>(part);
            chunk.parts = static_cast<std::uint16_t>(parts);
            for (auto index = part * object_chunk_entries;
                 index < std::min(count, (part + 1) * object_chunk_entries); ++index)
                if (index < change.objects.size())
                    chunk.objects.push_back(change.objects[index]);
                else
                    chunk.removed.push_back(change.removed[index - change.objects.size()]);
        }
        return result;
    }
    enum class Result { ignored, pending, applied, invalid };
    Result receive(const ObjectChunk &chunk) {
        if (!valid_object_chunk(chunk))
            return Result::invalid;
        if (chunk.revision <= revision_)
            return Result::ignored;
        if (!chunk.part) {
            if (chunk.base && chunk.base != revision_)
                return Result::invalid;
            Pending next;
            next.change.base = chunk.base;
            next.change.revision = chunk.revision;
            next.change.parts = chunk.parts;
            if (chunk.base)
                next.layout = objects_;
            pending_ = std::move(next);
        }
        if (!pending_ || pending_->next != chunk.part || pending_->change.base != chunk.base ||
            pending_->change.revision != chunk.revision || pending_->change.parts != chunk.parts)
            return Result::invalid;
        auto &next = *pending_;
        for (const auto &object : chunk.objects) {
            if (!next.touched.insert(object.id).second) {
                pending_.reset();
                return Result::invalid;
            }
            next.layout[object.id] = object;
            next.change.objects.push_back(object);
        }
        for (const auto id : chunk.removed) {
            if (!next.touched.insert(id).second) {
                pending_.reset();
                return Result::invalid;
            }
            next.layout.erase(id);
            next.change.removed.push_back(id);
        }
        if (next.touched.size() > max_owned_objects * 2) {
            pending_.reset();
            return Result::invalid;
        }
        if (++next.next != chunk.parts)
            return Result::pending;
        if (next.layout.size() > max_owned_objects) {
            pending_.reset();
            return Result::invalid;
        }
        objects_ = std::move(next.layout);
        latest_ = std::move(next.change);
        revision_ = chunk.revision;
        pending_.reset();
        return Result::applied;
    }
};
} // namespace dingosdk::multiplayer
