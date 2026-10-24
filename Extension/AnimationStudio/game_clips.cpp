#include "game_clips.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace dingosdk::anim_studio::game_clips {
namespace {
namespace ebx = frostbite::ebx;
namespace fb = frostbite;
// Game build 20260929: the level root carries the clips and their channel maps (each named
// "_af/<its guid's first 18 characters>/toolchanneltodofassetdata"); the game configuration carries
// the skater's rig, its DOF sets and its skeleton.
constexpr std::string_view level_toc = "Win32/levels/game/bam_levelroot/bam_levelroot.toc";
constexpr std::string_view level_bundle = "win32/levels/game/bam_levelroot/bam_levelroot";
constexpr std::string_view config_toc = "Win32/globals.toc";
constexpr std::string_view config_bundle = "win32/configurations/gameconfigurations/delmargameconfiguration";
constexpr std::string_view rig_name = "animation/dingo/animbase_default_rig";
constexpr std::uint32_t eclipse_resource_type = 0x4088BF7E;

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return out;
}

// ---- EBX values ------------------------------------------------------------------------------------
const ebx::Value& field(const ebx::Object& object, std::string_view name) {
    const auto* found = object.find(name);
    if (!found) throw std::runtime_error("its " + std::string(name) + " is missing");
    return found->value;
}
std::optional<double> number_of(const ebx::Value& value) {
    if (const auto* v = std::get_if<std::int64_t>(&value.data)) return static_cast<double>(*v);
    if (const auto* v = std::get_if<std::uint64_t>(&value.data)) return static_cast<double>(*v);
    if (const auto* v = std::get_if<double>(&value.data)) return *v;
    if (const auto* v = std::get_if<bool>(&value.data)) return *v ? 1.0 : 0.0;
    return std::nullopt;
}
double number(const ebx::Object& object, std::string_view name) {
    const auto n = number_of(field(object, name));
    if (!n || !std::isfinite(*n)) throw std::runtime_error("its " + std::string(name) + " is not a number");
    return *n;
}
std::uint32_t index_of(const ebx::Value& value) {
    const auto n = number_of(value);
    if (!n || *n < 0 || *n > 4294967295.0 || *n != std::floor(*n)) throw std::runtime_error("an index or id is not one");
    return static_cast<std::uint32_t>(*n);
}
const ebx::Value::Array& array(const ebx::Object& object, std::string_view name) {
    const auto* a = std::get_if<ebx::Value::Array>(&field(object, name).data);
    if (!a) throw std::runtime_error("its " + std::string(name) + " is not a list");
    return *a;
}
const ebx::Object* object_of(const ebx::Value& value) {
    const auto* p = std::get_if<std::shared_ptr<ebx::Object>>(&value.data);
    return p && *p ? p->get() : nullptr;
}
std::string type_of(const ebx::Document& doc, const ebx::InstanceRecord& instance) {
    return instance.descriptor >= 0 && static_cast<std::size_t>(instance.descriptor) < doc.types.size()
               ? doc.types[static_cast<std::size_t>(instance.descriptor)].name
               : std::string{};
}

// An instance a pointer names: in the same document, or exported by another found with `find`.
struct Ref {
    std::shared_ptr<const ebx::Document> document;
    std::size_t instance{};
    const ebx::Object& object() const {
        const auto& o = document->instances.at(instance).object;
        if (!o) throw std::runtime_error("an instance has no object");
        return *o;
    }
    std::string type() const { return type_of(*document, document->instances.at(instance)); }
};
template <class Find>
Ref resolve(const std::shared_ptr<const ebx::Document>& doc, const ebx::Value& value, Find&& find, std::string_view what) {
    const auto* pointer = std::get_if<ebx::PointerReference>(&value.data);
    if (!pointer || pointer->kind == ebx::PointerKind::null) throw std::runtime_error("its " + std::string(what) + " is not set");
    if (pointer->kind == ebx::PointerKind::internal) {
        if (pointer->index < 0 || static_cast<std::size_t>(pointer->index) >= doc->instances.size())
            throw std::runtime_error("its " + std::string(what) + " points nowhere");
        return {doc, static_cast<std::size_t>(pointer->index)};
    }
    const auto& import = doc->imports.at(static_cast<std::size_t>(pointer->index));
    const std::shared_ptr<const ebx::Document> target = find(import.fileGuid);
    if (!target) throw std::runtime_error("its " + std::string(what) + " (" + import.fileGuid.string() + ") is not in the game's data");
    for (std::size_t i = 0; i < target->instances.size(); ++i)
        if (target->instances[i].exported && target->instances[i].instanceGuid == import.classGuid) return {target, i};
    throw std::runtime_error("its " + std::string(what) + " is not in its partition");
}

// ---- the Eclipse codec (ReSkate Studio's sdk/resources/eclipse_animation.cpp) -------------------------
struct Channel {
    std::vector<std::uint16_t> frames;
    std::vector<std::array<float, 4>> values;
};
struct Eclipse {
    std::vector<Channel> rotations, vectors, scalars;
};
template <class T> T read(std::span<const std::byte> bytes, std::size_t at) {
    if (at > bytes.size() || sizeof(T) > bytes.size() - at) throw std::runtime_error("its animation is cut short");
    T value;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}
std::uint16_t big16(std::span<const std::byte> bytes, std::size_t at) {
    return static_cast<std::uint16_t>((read<std::uint8_t>(bytes, at) << 8) | read<std::uint8_t>(bytes, at + 1));
}
// The smallest-three quaternion: three components and the lane of the largest, which is made up.
std::array<float, 4> quaternion(std::uint32_t largest, std::array<float, 3> c) {
    constexpr std::array<std::array<unsigned, 3>, 4> lanes{{{1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}}};
    std::array<float, 4> q{};
    double sum{};
    for (std::size_t i = 0; i < 3; ++i) {
        q[lanes[largest & 3][i]] = c[i];
        sum += double(c[i]) * c[i];
    }
    if (!std::isfinite(sum) || sum > 1.0001) throw std::runtime_error("its animation has a broken rotation");
    q[largest & 3] = static_cast<float>(std::sqrt(std::max(0.0, 1 - sum)));
    return q;
}
std::array<float, 4> quaternion64(std::uint64_t p) {
    return quaternion(static_cast<std::uint32_t>(p & 3), {(static_cast<float>(p >> 43) - 1048576.0F) * 6.743499056938163e-7F,
                                                          (static_cast<float>((p >> 22) & 0x1FFFFF) - 1048576.0F) * 6.743499056938163e-7F,
                                                          (static_cast<float>((p >> 2) & 0xFFFFF) - 524288.0F) * 1.3487003798218211e-6F});
}
Eclipse read_eclipse(std::span<const std::byte> bytes) {
    if (bytes.size() < 80) throw std::runtime_error("its animation is cut short");
    const auto data = read<std::uint32_t>(bytes, 0x3C), size = read<std::uint32_t>(bytes, 0x38);
    const auto q_format = read<std::uint32_t>(bytes, 0x24), v_format = read<std::uint32_t>(bytes, 0x28);
    const auto f_format = read<std::uint32_t>(bytes, 0x2C), time_format = read<std::uint32_t>(bytes, 0x30);
    if (data > bytes.size() || size > bytes.size() - data || data < 80 || q_format > 3 || v_format > 2 || f_format > 2 || time_format > 1)
        throw std::runtime_error("its animation's layout is not one this reads");
    const auto payload = bytes.subspan(data, size);
    Eclipse out;
    const auto table = [&](std::size_t count_at, std::size_t offset_at, std::size_t stride, std::vector<Channel>& output, unsigned kind) {
        const auto count = read<std::uint32_t>(bytes, count_at), start = read<std::uint32_t>(bytes, offset_at);
        if (count > 65535 || (count && (start < 80 || start > data || count > (data - start) / stride)))
            throw std::runtime_error("its animation's channels are out of bounds");
        for (std::uint32_t c = 0; c < count; ++c) {
            const std::size_t at = start + c * stride;
            const auto keys = read<std::uint32_t>(bytes, at);
            if (!keys || keys > 65536) throw std::runtime_error("an animation channel has no keys");
            Channel channel;
            if (kind == 0 && keys == 1) {
                channel.frames.push_back(0);
                channel.values.push_back(quaternion64(read<std::uint64_t>(bytes, at + 4)));
            } else if (kind && keys == 2) {
                const auto first = read<std::uint32_t>(bytes, at + 4), last = read<std::uint32_t>(bytes, at + 8);
                if (first > 65535 || last > 65535) throw std::runtime_error("an animation key's time is broken");
                channel.frames = {static_cast<std::uint16_t>(first), static_cast<std::uint16_t>(last)};
                for (unsigned k = 0; k < 2; ++k) {
                    std::array<float, 4> value{};
                    for (unsigned d = 0; d < (kind == 1 ? 3U : 1U); ++d) value[d] = read<float>(bytes, at + (kind == 1 ? 16 + k * 16 : 12 + k * 4) + d * 4);
                    channel.values.push_back(value);
                }
            } else {
                const auto times = read<std::uint32_t>(bytes, at + 4), values = read<std::uint32_t>(bytes, at + 8);
                for (std::uint32_t k = 0; k < keys; ++k)
                    channel.frames.push_back(time_format ? big16(payload, times + k * 2ULL) : read<std::uint8_t>(payload, times + k));
                for (std::uint32_t k = 0; k < keys; ++k) {
                    std::array<float, 4> value{};
                    if (kind == 0) {
                        if (q_format == 0) {
                            const std::size_t p = values + k * 4ULL;
                            const unsigned a = read<std::uint8_t>(payload, p), b = read<std::uint8_t>(payload, p + 1);
                            const unsigned c0 = read<std::uint8_t>(payload, p + 2), d = read<std::uint8_t>(payload, p + 3);
                            value = quaternion(d & 3, {(static_cast<float>(a | (b >> 6) << 8) - 512.0F) * 0.0013824179768562317F,
                                                       (static_cast<float>((b & 63) | (c0 >> 4) << 6) - 512.0F) * 0.0013824179768562317F,
                                                       (static_cast<float>((c0 & 15) | (d >> 2) << 4) - 512.0F) * 0.0013824179768562317F});
                        } else if (q_format == 1) {
                            const std::size_t p = values + k * 6ULL;
                            const unsigned a = big16(payload, p), b = big16(payload, p + 2), c0 = big16(payload, p + 4);
                            value = quaternion(((a & 1) << 1) | (b & 1), {(static_cast<float>(a & ~1U) - 32768.0F) * 2.157951530534774e-5F,
                                                                          (static_cast<float>(b & ~1U) - 32768.0F) * 2.157951530534774e-5F,
                                                                          (static_cast<float>(c0) - 32768.0F) * 2.157951530534774e-5F});
                        } else if (q_format == 2) value = quaternion64(read<std::uint64_t>(payload, values + k * 8ULL));
                        else
                            for (unsigned d = 0; d < 4; ++d) value[d] = read<float>(payload, values + k * 16ULL + d * 4);
                    } else {
                        const auto format = kind == 1 ? v_format : f_format;
                        const unsigned dimensions = kind == 1 ? 3U : 1U, width = format == 0 ? 1U : format == 1 ? 2U : 4U;
                        for (unsigned d = 0; d < dimensions; ++d) {
                            const std::size_t p = values + (std::size_t(k) * dimensions + d) * width;
                            if (format == 2) value[d] = read<float>(payload, p);
                            else {
                                const auto least = read<float>(bytes, at + (kind == 1 ? 16 : 12) + d * 4);
                                const auto most = read<float>(bytes, at + (kind == 1 ? 32 : 16) + d * 4);
                                const float share = format ? big16(payload, p) / 65535.0F : read<std::uint8_t>(payload, p) / 255.0F;
                                value[d] = least + share * (most - least);
                            }
                        }
                    }
                    channel.values.push_back(value);
                }
            }
            for (std::size_t k = 1; k < channel.frames.size(); ++k)
                if (channel.frames[k] <= channel.frames[k - 1]) throw std::runtime_error("an animation channel's keys are out of order");
            output.push_back(std::move(channel));
        }
    };
    table(0, 4, 12, out.rotations, 0);
    table(12, 16, 48, out.vectors, 1);
    table(0x18, 0x1C, 20, out.scalars, 2);
    return out;
}
// A channel at `frame`: between its two keys around it, the rotation slerped.
std::array<float, 4> sample(const Channel& c, double frame, bool rotation) {
    const auto next = std::upper_bound(c.frames.begin(), c.frames.end(), frame, [](double f, std::uint16_t k) { return f < k; });
    if (next == c.frames.begin()) return c.values.front();
    if (next == c.frames.end()) return c.values.back();
    const auto i = static_cast<std::size_t>(next - c.frames.begin());
    const auto t = static_cast<float>((frame - c.frames[i - 1]) / (c.frames[i] - c.frames[i - 1]));
    const auto &a = c.values[i - 1], &b = c.values[i];
    if (rotation) return normalised(slerp(a, b, t));
    auto out = a;
    for (unsigned k = 0; k < 4; ++k) out[k] += (b[k] - a[k]) * t;
    return out;
}

// ---- the rig ---------------------------------------------------------------------------------------
// Where each DOF of the rig goes: a joint's rotation ('q'), position ('t') or scale ('s').
struct Dof {
    std::uint16_t joint{};
    char part{};
};
struct Rig {
    fb::Guid guid;
    std::map<std::uint32_t, Dof> dofs;
    std::vector<Joint> rest;           // each joint's default (parent-local)
    std::vector<std::int16_t> parents; // the skeleton's own: anim_core's table hangs Skateboard_Root elsewhere
    std::vector<std::string> names;    // each joint's, as the skeleton has them
    std::uint16_t board{};             // Skateboard_Root
    std::vector<std::uint16_t> mirror_of; // each joint's counterpart (Left* and Right* trade), else itself
    std::uint16_t left_foot{}, right_foot{}, front_truck{}, back_truck{};
};
// `child` in `parent`'s frame, put into the frame `parent` is in.
Joint compose(const Joint& parent, const Joint& child) {
    const auto moved = rotate(parent.rotation, {child.position[0] * parent.scale[0], child.position[1] * parent.scale[1], child.position[2] * parent.scale[2]});
    Joint out = child;
    out.position = {parent.position[0] + moved[0], parent.position[1] + moved[1], parent.position[2] + moved[2]};
    out.rotation = normalised(multiply(parent.rotation, child.rotation));
    return out;
}
std::string string_at(std::span<const std::byte> bytes, std::size_t at, std::size_t end) {
    if (end > bytes.size() || at >= end) throw std::runtime_error("a rig name is out of bounds");
    const auto* start = reinterpret_cast<const char*>(bytes.data() + at);
    const auto* nul = static_cast<const char*>(std::memchr(start, 0, end - at));
    if (!nul) throw std::runtime_error("a rig name has no end");
    return {start, nul};
}
} // namespace

struct Library::Data {
    vfs::GameData data;
    vfs::GameBundle level, config;
    std::map<std::string, std::size_t, std::less<>> level_names; // lower case: its ebx index
    std::map<std::string, std::size_t, std::less<>> channel_maps; // "_af/" partitions by their guid's start
    std::map<fb::Guid, std::size_t> config_guids;
    Rig rig;
    explicit Data(const std::filesystem::path& root) : data(root) {}

    std::shared_ptr<const ebx::Document> document(const vfs::GameBundle& bundle, std::size_t index) const {
        const auto* payload = bundle.payload(fb::AssetKind::ebx, index);
        if (!payload) throw std::runtime_error("an asset's payload is missing");
        return std::make_shared<const ebx::Document>(ebx::read_document(data.read(*payload)));
    }
    std::shared_ptr<const ebx::Document> config_document(const fb::Guid& guid) const {
        const auto found = config_guids.find(guid);
        return found == config_guids.end() ? nullptr : document(config, found->second);
    }
    std::shared_ptr<const ebx::Document> level_document(const fb::Guid& guid) const {
        const auto found = channel_maps.find(guid.string().substr(0, 18));
        if (found == channel_maps.end()) return nullptr;
        auto doc = document(level, found->second);
        return doc->fileGuid == guid ? doc : nullptr;
    }
    void read_rig();
};

void Library::Data::read_rig() {
    const auto found = std::find_if(config.manifest.ebx.begin(), config.manifest.ebx.end(), [](const auto& a) { return lower(a.name) == rig_name; });
    if (found == config.manifest.ebx.end()) throw std::runtime_error("the skater's rig is not in the game's data");
    const auto doc = document(config, static_cast<std::size_t>(found - config.manifest.ebx.begin()));
    const auto* root_instance = doc->root();
    if (!root_instance || !root_instance->object || doc->rootType != "RigAsset") throw std::runtime_error("the skater's rig is not a rig");
    const auto& root = *root_instance->object;
    rig.guid = doc->fileGuid;
    const auto find = [&](const fb::Guid& g) { return config_document(g); };
    // The skeleton: the joints' names, in the order the skater's pose has them.
    const auto skeleton = resolve(doc, field(root, "SkeletonAsset"), find, "skeleton");
    if (skeleton.type() != "AntSkeletonAsset") throw std::runtime_error("the rig's skeleton is not one");
    std::vector<std::byte> table;
    for (const auto& c : array(skeleton.object(), "NameTable")) table.push_back(static_cast<std::byte>(index_of(c) & 0xff));
    const auto& joints = array(skeleton.object(), "Joints");
    if (joints.size() != skeleton_joints) throw std::runtime_error("the skater's skeleton has " + std::to_string(joints.size()) + " joints");
    std::map<std::string, std::uint16_t, std::less<>> joint_of;
    for (std::size_t i = 0; i < joints.size(); ++i) {
        const auto* joint = object_of(joints[i]);
        if (!joint) throw std::runtime_error("a skeleton joint is not one");
        rig.names.push_back(string_at(table, index_of(field(*joint, "Name")), table.size()));
        joint_of.emplace(rig.names.back(), static_cast<std::uint16_t>(i));
        const auto parent = number(*joint, "Parent");
        if (!(parent >= -1 && parent < static_cast<double>(i))) throw std::runtime_error("the skeleton's joints are out of order");
        rig.parents.push_back(static_cast<std::int16_t>(parent));
    }
    const auto board = joint_of.find("Skateboard_Root");
    if (board == joint_of.end()) throw std::runtime_error("the skeleton has no Skateboard_Root");
    rig.board = board->second;
    rig.mirror_of.resize(rig.names.size());
    for (std::size_t i = 0; i < rig.names.size(); ++i) {
        rig.mirror_of[i] = static_cast<std::uint16_t>(i);
        auto other = rig.names[i];
        if (const auto at = other.find("Left"); at != std::string::npos) other.replace(at, 4, "Right");
        else if (const auto at2 = other.find("Right"); at2 != std::string::npos) other.replace(at2, 5, "Left");
        else continue;
        if (const auto pair = joint_of.find(other); pair != joint_of.end()) rig.mirror_of[i] = pair->second;
    }
    const auto named = [&](std::string_view name) {
        const auto found = joint_of.find(name);
        if (found == joint_of.end()) throw std::runtime_error("the skeleton has no " + std::string(name));
        return found->second;
    };
    rig.left_foot = named("LeftFoot");
    rig.right_foot = named("RightFoot");
    rig.front_truck = named("Truck_Front");
    rig.back_truck = named("Truck_Back");
    // Each DOF set's names (its LayoutData: a header, 32 bytes a DOF, then the names), against the ids.
    const auto& sets = array(root, "DofSets");
    const auto& starts = array(root, "DofSetIdIndices");
    const auto& ids = array(root, "DofIds");
    if (sets.size() != starts.size()) throw std::runtime_error("the rig's DOF lists disagree");
    for (std::size_t s = 0; s < sets.size(); ++s) {
        const auto set = resolve(doc, sets[s], find, "DOF set");
        const auto& words = array(set.object(), "LayoutData");
        std::vector<std::byte> bytes(words.size() * 8);
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto* w = std::get_if<std::uint64_t>(&words[i].data);
            if (!w) throw std::runtime_error("a DOF set's layout is not one this reads");
            std::memcpy(bytes.data() + i * 8, w, 8);
        }
        const auto word = [&](std::size_t at) { return read<std::uint32_t>(bytes, at); };
        const std::size_t start = index_of(starts[s]), end = s + 1 < starts.size() ? index_of(starts[s + 1]) : ids.size();
        const std::size_t count = std::size_t(word(4)) + 1, strings = word(0x10), string_end = strings + word(0x14);
        if (start > end || end > ids.size() || count != end - start || strings != 0x20 + count * 0x20 || string_end > bytes.size())
            throw std::runtime_error("a DOF set's layout is not one this reads");
        for (std::size_t i = 0; i < count; ++i) {
            const auto name = string_at(bytes, strings + word(0x20 + i * 0x20 + 12), string_end);
            if (name.size() < 3 || name[name.size() - 2] != '.') continue;
            const char part = name.back();
            if (part != 'q' && part != 't' && part != 's') continue;
            if (const auto j = joint_of.find(std::string_view(name).substr(0, name.size() - 2)); j != joint_of.end())
                rig.dofs.try_emplace(index_of(ids[start + i]), Dof{j->second, part});
        }
    }
    // The default pose: every joint's rest rotation, position and scale.
    const auto pose = resolve(doc, field(root, "DefaultPoseAsset"), find, "default pose");
    std::map<std::uint32_t, std::array<float, 4>> defaults;
    for (const auto& [prefix, dimensions] : {std::pair{"Vector3", 3}, std::pair{"Vector4", 4}}) {
        const auto* id_field = pose.object().find(std::string(prefix) + "DofIds");
        const auto* value_field = pose.object().find(std::string(prefix) + "Values");
        if (!id_field || !value_field) continue;
        const auto* dof_ids = std::get_if<ebx::Value::Array>(&id_field->value.data);
        const auto* values = std::get_if<ebx::Value::Array>(&value_field->value.data);
        if (!dof_ids || !values || dof_ids->size() != values->size()) throw std::runtime_error("the rig's default pose disagrees with itself");
        for (std::size_t i = 0; i < dof_ids->size(); ++i) {
            const auto* v = object_of((*values)[i]);
            if (!v) continue;
            std::array<float, 4> value{};
            constexpr std::array axes{"x", "y", "z", "w"};
            for (int d = 0; d < dimensions; ++d) value[static_cast<std::size_t>(d)] = static_cast<float>(number(*v, axes[static_cast<std::size_t>(d)]));
            defaults[index_of((*dof_ids)[i])] = value;
        }
    }
    rig.rest.assign(skeleton_joints, Joint{});
    for (const auto& [id, dof] : rig.dofs) {
        const auto value = defaults.find(id);
        if (value == defaults.end()) continue;
        auto& j = rig.rest[dof.joint];
        const auto& v = value->second;
        if (dof.part == 'q') j.rotation = normalised({v[0], v[1], v[2], v[3]});
        else if (dof.part == 't') j.position = {v[0], v[1], v[2]};
        else j.scale = {v[0], v[1], v[2]};
    }
}

Library::Library(const std::filesystem::path& game_root) : data_(std::make_unique<Data>(game_root)) {
    auto& d = *data_;
    auto level = d.data.read_bundle(d.data.read_toc(level_toc), level_bundle);
    if (!level) throw std::runtime_error("the level root's bundle is missing");
    d.level = std::move(*level);
    auto config = d.data.read_bundle(d.data.read_toc(config_toc), config_bundle);
    if (!config) throw std::runtime_error("the game configuration's bundle is missing");
    d.config = std::move(*config);
    for (std::size_t i = 0; i < d.level.manifest.ebx.size(); ++i) {
        auto name = lower(d.level.manifest.ebx[i].name);
        if (name.starts_with("_af/") && name.size() > 22 && name[22] == '/') d.channel_maps.emplace(name.substr(4, 18), i);
        else if (name.starts_with("animation/")) d.level_names.emplace(std::move(name), i);
    }
    for (std::size_t i = 0; i < d.config.manifest.ebx.size(); ++i)
        if (const auto* payload = d.config.payload(fb::AssetKind::ebx, i)) try {
                d.config_guids.emplace(ebx::read_file_guid(d.data.read(*payload)), i);
            } catch (...) {}
    d.read_rig();
}
Library::~Library() = default;

namespace {
// A joint's place with no scale, for the mirror: a turn, then a move.
struct Rigid {
    Quat r{0, 0, 0, 1};
    Vec3 t{};
};
Rigid rigid(const Joint& j) { return {j.rotation, j.position}; }
Rigid then(const Rigid& a, const Rigid& b) {
    const auto m = rotate(a.r, b.t);
    return {normalised(multiply(a.r, b.r)), {a.t[0] + m[0], a.t[1] + m[1], a.t[2] + m[2]}};
}
Rigid inverse(const Rigid& a) {
    const auto c = conjugate(a.r);
    const auto t = rotate(c, a.t);
    return {c, {-t[0], -t[1], -t[2]}};
}
float dot3(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
// `a` reflected across the plane through the origin with normal `n`: S a S, still a turn and a move
// (a turn's axis is a pseudovector: it comes out reflected and reversed).
Rigid reflect(const Rigid& a, const Vec3& n) {
    const Vec3 v{a.r[0], a.r[1], a.r[2]};
    const float dv = 2 * dot3(v, n), dt = 2 * dot3(a.t, n);
    return {normalised({-(v[0] - dv * n[0]), -(v[1] - dv * n[1]), -(v[2] - dv * n[2]), a.r[3]}),
            {a.t[0] - dt * n[0], a.t[1] - dt * n[1], a.t[2] - dt * n[2]}};
}
// A board mirrored across `n` (placement frame) as its own geometry is: the board is symmetric across
// its length (its local Z) and up (local Y), so its turn becomes S * R * Sb, Sb reflecting its local X.
// A flat board keeps its nose ahead; a board rolling one way rolls the other.
Rigid mirror_board(const Rigid& b, const Vec3& n) {
    const auto& q = b.r;
    // The turn as a matrix (columns: where local X, Y, Z go).
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    float m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                     {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                     {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    // S * M (each column reflected), then * Sb (local X's column reversed).
    for (int c = 0; c < 3; ++c) {
        const float d = 2 * (n[0] * m[0][c] + n[1] * m[1][c] + n[2] * m[2][c]);
        for (int r = 0; r < 3; ++r) m[r][c] -= d * n[static_cast<std::size_t>(r)];
    }
    for (int r = 0; r < 3; ++r) m[r][0] = -m[r][0];
    // Back to a quaternion.
    Quat out;
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        out = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25f * s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        out = {0.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        out = {(m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const float s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        out = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s, (m[1][0] - m[0][1]) / s};
    }
    const float dt = 2 * dot3(b.t, n);
    return {normalised(out), {b.t[0] - dt * n[0], b.t[1] - dt * n[1], b.t[2] - dt * n[2]}};
}
// Every joint under the placement joint (1) in its frame, down the skeleton's own parents.
void in_placement(std::span<const Joint> local, const std::vector<std::int16_t>& parents, std::vector<Rigid>& out) {
    out.assign(local.size(), Rigid{});
    for (std::size_t j = 2; j < local.size(); ++j) {
        const auto p = parents[j];
        out[j] = p > static_cast<int>(placement_joint) ? then(out[static_cast<std::size_t>(p)], rigid(local[j])) : rigid(local[j]);
    }
}
}

std::optional<bool> Library::goofy(const Clip& clip) const {
    const auto& rig = data_->rig;
    if (clip.empty() || clip.joints != skeleton_joints) return std::nullopt;
    std::vector<Rigid> placed;
    in_placement(clip.frame(0), rig.parents, placed);
    const auto apart = [&](std::uint16_t a, std::uint16_t b) {
        const auto &p = placed[a].t, &q = placed[b].t;
        return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[2] - q[2]) * (p[2] - q[2]));
    };
    const float left = apart(rig.left_foot, rig.front_truck), right = apart(rig.right_foot, rig.front_truck);
    // Feet more than a board's length from the trucks are not on it.
    if (!(std::max(left, right) < 1.2f) || std::abs(left - right) < 0.05f) return std::nullopt;
    return right < left;
}

Clip Library::mirror(const Clip& clip) const {
    const auto& rig = data_->rig;
    if (clip.empty() || clip.joints != skeleton_joints) throw std::runtime_error("only a clip of the skater's 395 joints can be mirrored");
    std::vector<Rigid> rest, placed;
    in_placement(rig.rest, rig.parents, rest);
    in_placement(clip.frame(0), rig.parents, placed);
    // The plane: upright, along the board from its back truck to its front one at the first frame.
    Vec3 along{placed[rig.front_truck].t[0] - placed[rig.back_truck].t[0], 0, placed[rig.front_truck].t[2] - placed[rig.back_truck].t[2]};
    const float length = std::sqrt(dot3(along, along));
    if (!(length > 0.1f)) throw std::runtime_error("the clip has no board to mirror it along");
    const Vec3 n{along[2] / length, 0, -along[0] / length}; // along x up
    const Rigid first_place = rigid(clip.frame(0)[placement_joint]);
    const auto n_root = rotate(first_place.r, n); // the same plane, seen from the root
    Clip out = clip;
    if (!clip.turned_only.empty()) {
        for (std::size_t j = 0; j < clip.turned_only.size(); ++j) out.turned_only[rig.mirror_of[j]] = clip.turned_only[j];
    }
    // The board's joints (Skateboard_Root and everything under it): the board mirrored as itself, the rest
    // of it (trucks, wheels, the feet's deck effectors) in its frame, Left and Right trading there.
    std::vector<bool> on_board(skeleton_joints, false);
    for (std::size_t j = rig.board; j < skeleton_joints; ++j)
        for (int at = static_cast<int>(j); at >= 0; at = rig.parents[static_cast<std::size_t>(at)])
            if (at == rig.board) {
                on_board[j] = true;
                break;
            }
    const Vec3 board_side{1, 0, 0}; // the board's own mirror: across its local X
    std::vector<Rigid> mirrored(skeleton_joints);
    for (std::size_t f = 0; f < clip.times.size(); ++f) {
        const auto frame = clip.frame(f);
        in_placement(frame, rig.parents, placed);
        // Each joint's way from its rest, mirrored, from its counterpart's rest.
        for (std::size_t j = 2; j < skeleton_joints; ++j) {
            if (on_board[j]) continue;
            const auto m = rig.mirror_of[j];
            mirrored[m] = then(reflect(then(placed[j], inverse(rest[j])), n), rest[m]);
        }
        const auto board_now = placed[rig.board], board_mirrored = mirror_board(board_now, n);
        for (std::size_t j = rig.board; j < skeleton_joints; ++j) {
            if (!on_board[j]) continue;
            if (j == rig.board) {
                mirrored[j] = board_mirrored;
                continue;
            }
            mirrored[rig.mirror_of[j]] = then(board_mirrored, reflect(then(inverse(board_now), placed[j]), board_side));
        }
        auto* dst = out.frames.data() + f * skeleton_joints;
        for (std::size_t k = 2; k < skeleton_joints; ++k) {
            const auto p = rig.parents[k];
            const auto local = p > static_cast<int>(placement_joint) ? then(inverse(mirrored[static_cast<std::size_t>(p)]), mirrored[k]) : mirrored[k];
            dst[k].rotation = local.r;
            dst[k].position = local.t;
            dst[k].scale = frame[rig.mirror_of[k]].scale;
        }
        // The placement joint's turn and travel since the first frame go the other way round.
        const auto since = then(rigid(frame[placement_joint]), inverse(first_place));
        const auto place = then(reflect(since, n_root), first_place);
        dst[placement_joint].rotation = place.r;
        dst[placement_joint].position = place.t;
        // The board, under the placement joint as Skateboard_Root is.
        if (f < clip.board.size()) {
            const auto b = mirror_board(rigid(clip.board[f]), n);
            out.board[f].rotation = b.r;
            out.board[f].position = b.t;
        }
    }
    return out;
}
std::string_view Library::joint_name(std::uint16_t joint) const {
    return joint < data_->rig.names.size() ? std::string_view(data_->rig.names[joint]) : std::string_view{};
}

std::vector<std::string> Library::names(std::string_view containing, std::size_t most) const {
    const auto wanted = lower(containing);
    std::vector<std::pair<std::size_t, std::string>> found;
    for (const auto& [name, index] : data_->level_names)
        if (wanted.empty() || name.find(wanted) != std::string::npos) found.emplace_back(index, name);
    std::sort(found.begin(), found.end());
    std::vector<std::string> out;
    for (auto& [index, name] : found) {
        if (out.size() >= most) break;
        out.push_back(std::move(name));
    }
    return out;
}

Clip Library::load(std::string_view asked) const {
    const auto& d = *data_;
    const auto name = lower(asked);
    const auto at = d.level_names.find(name);
    if (at == d.level_names.end()) throw std::runtime_error("the game has no animation " + name);
    const auto doc = d.document(d.level, at->second);
    if (doc->rootType != "ClipControllerAsset" || !doc->root() || !doc->root()->object)
        throw std::runtime_error(name + " is a " + doc->rootType + ", not a clip");
    const auto& root = *doc->root()->object;
    const auto* rig = std::get_if<ebx::PointerReference>(&field(root, "RigRef").data);
    if (!rig || rig->kind != ebx::PointerKind::external || doc->imports.at(static_cast<std::size_t>(rig->index)).fileGuid != d.rig.guid)
        throw std::runtime_error(name + " is not on the skater's rig");
    const auto find = [&](const fb::Guid& g) { return d.level_document(g); };
    const auto anim = resolve(doc, field(root, "Anim"), find, "animation");
    if (anim.type() != "EclipseAnimationAsset") throw std::runtime_error(name + "'s animation is a " + anim.type() + ", which this does not read");
    const double fps = number(root, "FPS") * number(root, "TimeScale");
    if (!(fps > 0 && fps < 1000)) throw std::runtime_error(name + "'s frame rate is broken");
    const double trim = number(anim.object(), "TrimOffset"), duration = number(anim.object(), "TrimmedDuration");
    if (!(duration > 0 && duration < 36000)) throw std::runtime_error(name + " has no frames");
    // Its keys: the resource the animation names.
    const auto* reference = std::get_if<ebx::ResourceReference>(&field(anim.object(), "Resource").data);
    if (!reference) throw std::runtime_error(name + " has no animation data");
    std::size_t index = 0;
    const auto& resources = d.level.manifest.resources;
    while (index < resources.size() && resources[index].resourceId != reference->id) ++index;
    if (index == resources.size() || resources[index].resourceType != eclipse_resource_type)
        throw std::runtime_error(name + "'s animation data is not in the game's data");
    const auto* payload = d.level.payload(fb::AssetKind::resource, index);
    if (!payload) throw std::runtime_error(name + "'s animation data is missing");
    const auto eclipse = read_eclipse(d.data.read(*payload));
    // Which DOF each channel moves: rotations, then vectors, then floats.
    const auto map = resolve(anim.document, field(anim.object(), "ChannelToDofAsset"), find, "channel map");
    const auto& ids = array(map.object(), "ChannelToDofIds");
    if (ids.size() != eclipse.rotations.size() + eclipse.vectors.size() + eclipse.scalars.size())
        throw std::runtime_error(name + "'s channel map disagrees with its animation");
    struct Bound {
        const Channel* channel;
        Dof dof;
    };
    std::vector<Bound> bound;
    std::size_t next = 0;
    for (const auto* list : {&eclipse.rotations, &eclipse.vectors}) {
        const char kind = list == &eclipse.rotations ? 'q' : 'v';
        for (const auto& channel : *list) {
            const auto found = d.rig.dofs.find(index_of(ids[next++]));
            if (found == d.rig.dofs.end()) continue;
            if ((kind == 'q') != (found->second.part == 'q')) continue;
            bound.push_back({&channel, found->second});
        }
    }
    Clip clip;
    clip.joints = skeleton_joints;
    // A joint the clip has no position for keeps the skater's own (his build), as the game does.
    clip.turned_only.assign(skeleton_joints, 1);
    for (const auto& b : bound)
        if (b.dof.part == 't') clip.turned_only[b.dof.joint] = 0;
    const auto frames = static_cast<std::size_t>(std::lround(duration)) + 1;
    clip.times.reserve(frames);
    clip.frames.reserve(frames * skeleton_joints);
    std::vector<float> board_y;
    for (std::size_t k = 0; k < frames; ++k) {
        const double frame = trim + static_cast<double>(k);
        clip.times.push_back(static_cast<float>(k / fps));
        const auto first = clip.frames.size();
        clip.frames.insert(clip.frames.end(), d.rig.rest.begin(), d.rig.rest.end());
        const std::span<Joint> pose(clip.frames.data() + first, skeleton_joints);
        for (const auto& b : bound) {
            const auto v = sample(*b.channel, frame, b.dof.part == 'q');
            auto& j = pose[b.dof.joint];
            if (b.dof.part == 'q') j.rotation = {v[0], v[1], v[2], v[3]};
            else if (b.dof.part == 't') j.position = {v[0], v[1], v[2]};
            else j.scale = {v[0], v[1], v[2]};
        }
        // The board under the placement joint, as a recording has it: Skateboard_Root, from the
        // placement joint down through the skeleton's own parents.
        Joint rel = pose[d.rig.board];
        for (auto up_to = d.rig.parents[d.rig.board]; up_to > placement_joint; up_to = d.rig.parents[static_cast<std::size_t>(up_to)]) rel = compose(pose[static_cast<std::size_t>(up_to)], rel);
        clip.board.push_back(rel);
        Joint up = rel;
        for (int up_to = placement_joint; up_to >= 0; up_to = d.rig.parents[static_cast<std::size_t>(up_to)]) up = compose(pose[static_cast<std::size_t>(up_to)], up);
        board_y.push_back(up.position[1]);
    }
    // The pop is the board's first low point (the tail strikes); the landing its deepest low point
    // between a quarter and about 0.85 s later (make_trick_library.py; at 60 a second). Its height is
    // smoothed over five frames, as numpy's convolve "same" does (nothing past the ends).
    std::vector<float> smooth(board_y.size());
    for (std::size_t i = 0; i < board_y.size(); ++i) {
        float sum = 0;
        for (std::size_t j = i >= 2 ? i - 2 : 0; j <= std::min(i + 2, board_y.size() - 1); ++j) sum += board_y[j];
        smooth[i] = sum / 5.0f;
    }
    std::vector<std::size_t> lows;
    for (std::size_t i = 2; i + 2 < smooth.size(); ++i)
        if (smooth[i] <= smooth[i - 1] && smooth[i] < smooth[i + 1]) lows.push_back(i);
    if (!lows.empty()) {
        const std::size_t takeoff = lows.front();
        const auto per_frame = fps / 60.0;
        const std::size_t from = takeoff + static_cast<std::size_t>(15 * per_frame), to = std::min(frames - 1, takeoff + static_cast<std::size_t>(51 * per_frame));
        std::optional<std::size_t> landing;
        for (const auto i : lows)
            if (i >= from && i <= to && (!landing || smooth[i] < smooth[*landing])) landing = i;
        clip.takeoff = clip.times[takeoff];
        clip.landing = clip.times[landing.value_or(std::min(frames - 1, takeoff + static_cast<std::size_t>(27 * per_frame)))];
    }
    return clip;
}

namespace {
struct Loader {
    std::once_flag started;
    std::atomic<std::shared_ptr<const Library>> library;
    std::mutex mutex;
    std::string failure;
};
Loader& loader() {
    static auto* l = new Loader;
    return *l;
}
std::filesystem::path game_folder() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("the game's folder is unknown");
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}
} // namespace

void prepare() noexcept {
    try {
        std::call_once(loader().started, [] {
            std::thread([] {
                try {
                    loader().library.store(std::make_shared<const Library>(game_folder()));
                } catch (const std::exception& e) {
                    std::lock_guard lock(loader().mutex);
                    loader().failure = e.what();
                }
            }).detach();
        });
    } catch (...) {}
}
std::shared_ptr<const Library> library() noexcept {
    prepare();
    return loader().library.load();
}
std::string failure() {
    std::lock_guard lock(loader().mutex);
    return loader().failure;
}
}
