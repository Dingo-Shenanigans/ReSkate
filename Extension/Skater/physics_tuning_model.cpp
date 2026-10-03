#include "physics_tuning_model.h"
#include "Engine/Game/Build/20260929/physics_tuning.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace dingosdk::physics_tuning {
namespace {
namespace fb = frostbite;
namespace ebx = frostbite::ebx;
using namespace game::build::v20260929::physics_tuning;
constexpr std::string_view bundle_name = "win32/levels/game/bam_levelroot/bam_levelroot";
constexpr std::string_view bundle_toc = "Win32/levels/game/bam_levelroot/bam_levelroot.toc";
constexpr std::string_view asset_name = "gameplay/skatephysicstuning";
constexpr std::size_t max_curve_points = 256;

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
std::uint16_t value_size(ebx::FieldType type) noexcept {
    switch (type) {
    case ebx::FieldType::boolean: case ebx::FieldType::int8: case ebx::FieldType::uint8: return 1;
    case ebx::FieldType::int16: case ebx::FieldType::uint16: return 2;
    case ebx::FieldType::int32: case ebx::FieldType::uint32: case ebx::FieldType::enumeration:
    case ebx::FieldType::float32: return 4;
    case ebx::FieldType::int64: case ebx::FieldType::uint64: case ebx::FieldType::float64: return 8;
    default: return 0;
    }
}
// Every plain value and curve pointer of `type` placed at `start`, nested structures included.
void walk(const ebx::Document &document, const ebx::TypeDescriptor &type, std::uint32_t start, Model &model, int depth,
          const std::string &prefix) {
    if (depth > 16) throw std::runtime_error("the tuning's types nest too deeply");
    for (std::size_t i = 0; i < type.fieldCount; ++i) {
        const auto index = static_cast<std::size_t>(type.fieldIndex) + i;
        if (index >= document.fields.size()) throw std::runtime_error("a tuning field is missing");
        const auto &field = document.fields[index];
        const auto kind = field.type();
        if (kind == ebx::FieldType::inherited || (kind == ebx::FieldType::structure && field.category() != ebx::FieldCategory::array)) {
            if (field.classRef >= document.types.size()) throw std::runtime_error("a tuning structure type is missing");
            walk(document, document.types[field.classRef], kind == ebx::FieldType::inherited ? start : start + field.dataOffset,
                 model, depth + 1, kind == ebx::FieldType::inherited ? prefix : prefix + field.name + ".");
            continue;
        }
        if (field.category() == ebx::FieldCategory::array) continue;
        const auto offset = start + field.dataOffset;
        if (offset < asset_fields_start) continue; // the object header and the asset's name
        if (kind == ebx::FieldType::pointer) {
            if (offset + 8 <= asset_size) model.curve_slots.push_back(static_cast<std::uint16_t>(offset));
            continue;
        }
        const auto size = value_size(kind);
        if (!size || offset + size > asset_size) continue;
        model.fields.push_back({static_cast<std::uint16_t>(offset), size, kind == ebx::FieldType::float32,
                                kind == ebx::FieldType::boolean, prefix + field.name});
    }
}
template<class T> T at(std::span<const std::byte> bytes, std::size_t position) {
    if (position > bytes.size() || bytes.size() - position < sizeof(T)) throw std::runtime_error("a tuning curve is cut short");
    T value;
    std::memcpy(&value, bytes.data() + position, sizeof(T));
    return value;
}
// The curve a slot of the asset points at, straight from the EBX bytes (the file keeps the
// runtime's layout for FloatCurve and its points).
std::optional<Curve> read_curve(std::span<const std::byte> bytes, const ebx::Document &document,
                                const ebx::InstanceRecord &root, std::uint16_t slot) {
    const auto encoded = at<std::int32_t>(bytes, document.dataStart + root.dataOffset + slot);
    if (!encoded || (encoded & 1)) return std::nullopt; // no curve, or one in another file
    const auto target = static_cast<std::int64_t>(root.dataOffset) + slot + encoded;
    const auto found = std::ranges::find_if(document.instances, [&](const ebx::InstanceRecord &instance) {
        return static_cast<std::int64_t>(instance.dataOffset) == target;
    });
    if (found == document.instances.end() || found->descriptor < 0 ||
        static_cast<std::size_t>(found->descriptor) >= document.types.size() ||
        document.types[static_cast<std::size_t>(found->descriptor)].name != "FloatCurve")
        return std::nullopt;
    const auto start = document.dataStart + found->dataOffset;
    Curve curve;
    curve.min = at<float>(bytes, start + curve_min_offset);
    curve.max = at<float>(bytes, start + curve_max_offset);
    const auto points_at = static_cast<std::int64_t>(start + curve_points_offset) +
                           at<std::int32_t>(bytes, start + curve_points_offset);
    if (points_at < 4 || static_cast<std::uint64_t>(points_at) > bytes.size()) throw std::runtime_error("a tuning curve's points are misplaced");
    const auto count = at<std::uint32_t>(bytes, static_cast<std::size_t>(points_at) - 4);
    if (count > max_curve_points) throw std::runtime_error("a tuning curve has too many points");
    const auto size = count * curve_point_size;
    if (static_cast<std::uint64_t>(points_at) + size > bytes.size()) throw std::runtime_error("a tuning curve's points are cut short");
    const auto *first = reinterpret_cast<const std::uint8_t *>(bytes.data()) + points_at;
    curve.points.assign(first, first + size);
    return curve;
}

void put(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void put(std::vector<std::uint8_t> &out, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    bool failed{};
    std::span<const std::uint8_t> take(std::size_t size) {
        if (failed || bytes.size() - at < size) { failed = true; return {}; }
        const auto result = bytes.subspan(at, size);
        at += size;
        return result;
    }
    std::uint16_t u16() {
        const auto b = take(2);
        return b.empty() ? 0 : static_cast<std::uint16_t>(b[0] | (b[1] << 8));
    }
    std::uint8_t u8() { const auto b = take(1); return b.empty() ? 0 : b[0]; }
    float f32() {
        const auto b = take(4);
        if (b.empty()) return 0;
        return std::bit_cast<float>(static_cast<std::uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16)) |
                                    (static_cast<std::uint32_t>(b[3]) << 24));
    }
};
bool finite_at(std::span<const std::uint8_t> bytes, std::size_t offset) {
    float value;
    std::memcpy(&value, bytes.data() + offset, 4);
    return std::isfinite(value);
}
bool valid_points(std::span<const std::uint8_t> points) {
    if (points.size() % curve_point_size) return false;
    for (std::size_t p = 0; p < points.size(); p += curve_point_size) {
        for (std::size_t f = 0; f < 0x18; f += 4)
            if (!finite_at(points, p + f)) return false;
        std::uint32_t type;
        std::memcpy(&type, points.data() + p + 0x18, 4);
        if (type > 3) return false; // FloatCurveType has four values
    }
    return true;
}
} // namespace

Model read_game_tuning(const std::filesystem::path &game_root) {
    const vfs::GameData data(game_root);
    std::optional<vfs::GameBundle> bundle;
    try {
        bundle = data.read_bundle(data.read_toc(bundle_toc), bundle_name);
    } catch (const std::exception &) {}
    // A game update that moves the bundle costs a scan of the TOCs.
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(game_root / L"Data" / L"Win32", error), end;
         it != end && !error && !bundle; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != L".toc") continue;
        try {
            bundle = data.read_bundle(data.read_toc(std::filesystem::relative(it->path(), game_root / L"Data").generic_string()),
                                      bundle_name);
        } catch (const std::exception &) {}
    }
    if (!bundle) throw std::runtime_error("the game's level root bundle was not found");
    std::size_t index = bundle->manifest.ebx.size();
    for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i)
        if (lower(bundle->manifest.ebx[i].name) == asset_name) { index = i; break; }
    const auto *payload = bundle->payload(fb::AssetKind::ebx, index);
    if (!payload) throw std::runtime_error("Gameplay/SkatePhysicsTuning is not in the level root bundle");
    const auto bytes = data.read(*payload);
    const auto document = ebx::read_document(bytes);
    const auto *root = document.root();
    if (!root || document.rootType != "SkatePhysicsTuningAsset" || root->rawImage.size() != asset_size ||
        root->descriptor < 0 || static_cast<std::size_t>(root->descriptor) >= document.types.size())
        throw std::runtime_error("Gameplay/SkatePhysicsTuning is not the SkatePhysicsTuningAsset this build expects");
    Model model;
    model.image.resize(asset_size);
    std::memcpy(model.image.data(), root->rawImage.data(), asset_size);
    walk(document, document.types[static_cast<std::size_t>(root->descriptor)], 0, model, 0, {});
    std::ranges::sort(model.fields, {}, &Field::offset);
    for (std::size_t i = 1; i < model.fields.size(); ++i)
        if (model.fields[i].offset < model.fields[i - 1].offset + model.fields[i - 1].size)
            throw std::runtime_error("the tuning's fields overlap");
    std::ranges::sort(model.curve_slots);
    model.curve_slots.erase(std::unique(model.curve_slots.begin(), model.curve_slots.end()), model.curve_slots.end());
    if (model.fields.empty()) throw std::runtime_error("the tuning has no values");
    // Only the pointers the game's own data shows as FloatCurves are synced. The others (keyed
    // tables, sub-objects, imports) keep other layouts: writing a curve's points or range into
    // one overwrote a guest's pointer with the host's, and the skater crashed on a coping.
    std::vector<std::uint16_t> curve_slots;
    for (const auto slot : model.curve_slots) {
        if (auto curve = read_curve(bytes, document, *root, slot)) {
            curve_slots.push_back(slot);
            model.curves.push_back(std::move(curve));
        }
    }
    model.curve_slots = std::move(curve_slots);
    return model;
}

Encoded encode_differences(const Model &model, const Values &live, std::size_t limit) {
    Encoded result;
    if (live.image.size() != model.image.size()) return result;
    std::vector<std::uint8_t> runs;
    std::size_t run_count{};
    const auto differs = [&](const Field &f) {
        return std::memcmp(model.image.data() + f.offset, live.image.data() + f.offset, f.size) != 0;
    };
    // Runs of adjacent changed fields, so a changed group costs one header.
    for (std::size_t i = 0; i < model.fields.size();) {
        if (!differs(model.fields[i])) { ++i; continue; }
        const auto first = model.fields[i].offset;
        std::size_t end = first + model.fields[i].size;
        for (++i; i < model.fields.size() && model.fields[i].offset == end && differs(model.fields[i]) &&
                  end + model.fields[i].size - first <= 0xffff; ++i)
            end += model.fields[i].size;
        put(runs, first);
        put(runs, static_cast<std::uint16_t>(end - first));
        runs.insert(runs.end(), live.image.begin() + first, live.image.begin() + static_cast<std::ptrdiff_t>(end));
        ++run_count;
    }
    std::vector<std::uint8_t> curves;
    std::size_t curve_count{};
    const auto fixed = 2 + 2 + runs.size() + 2;
    if (fixed > limit) { result.left_out = model.curve_slots.size(); return result; }
    for (std::size_t i = 0; i < model.curve_slots.size() && i < live.curves.size(); ++i) {
        const auto &mine = live.curves[i];
        if (!mine || (i < model.curves.size() && model.curves[i] == mine) ||
            mine->points.size() > max_curve_points * curve_point_size || mine->points.size() % curve_point_size)
            continue;
        const auto size = 12 + mine->points.size();
        if (fixed + curves.size() + size > limit) { ++result.left_out; continue; }
        put(curves, model.curve_slots[i]);
        put(curves, static_cast<std::uint16_t>(mine->points.size() / curve_point_size));
        put(curves, mine->min);
        put(curves, mine->max);
        curves.insert(curves.end(), mine->points.begin(), mine->points.end());
        ++curve_count;
    }
    if (!run_count && !curve_count) return result;
    result.bytes = {'T', 1};
    put(result.bytes, static_cast<std::uint16_t>(run_count));
    result.bytes.insert(result.bytes.end(), runs.begin(), runs.end());
    put(result.bytes, static_cast<std::uint16_t>(curve_count));
    result.bytes.insert(result.bytes.end(), curves.begin(), curves.end());
    result.runs = run_count;
    result.curves = curve_count;
    return result;
}

std::optional<Values> apply_differences(const Model &model, std::span<const std::uint8_t> differences) {
    // Curves the differences leave out are the game's.
    Values values{model.image, model.curves};
    values.curves.resize(model.curve_slots.size());
    if (differences.empty()) return values;
    std::vector<bool> sent(model.curve_slots.size());
    Reader r{differences};
    if (r.u8() != 'T' || r.u8() != 1) return std::nullopt;
    const auto runs = r.u16();
    for (unsigned n = 0; n < runs && !r.failed; ++n) {
        const auto offset = r.u16(), size = r.u16();
        const auto bytes = r.take(size);
        if (r.failed || !size) return std::nullopt;
        // A run starts at a field and covers adjacent fields only.
        auto field = std::ranges::lower_bound(model.fields, offset, {}, &Field::offset);
        std::size_t end = offset;
        while (field != model.fields.end() && field->offset == end && end < offset + size) end += (field++)->size;
        if (end != static_cast<std::size_t>(offset) + size) return std::nullopt;
        std::memcpy(values.image.data() + offset, bytes.data(), size);
    }
    const auto curves = r.u16();
    for (unsigned n = 0; n < curves && !r.failed; ++n) {
        const auto slot = r.u16(), count = r.u16();
        Curve curve;
        curve.min = r.f32();
        curve.max = r.f32();
        const auto points = r.take(static_cast<std::size_t>(count) * curve_point_size);
        const auto found = std::ranges::lower_bound(model.curve_slots, slot);
        if (r.failed || count > max_curve_points || found == model.curve_slots.end() || *found != slot ||
            !std::isfinite(curve.min) || !std::isfinite(curve.max) || !valid_points(points))
            return std::nullopt;
        const auto slot_index = static_cast<std::size_t>(found - model.curve_slots.begin());
        if (sent[slot_index]) return std::nullopt; // the same curve twice
        sent[slot_index] = true;
        curve.points.assign(points.begin(), points.end());
        values.curves[slot_index] = std::move(curve);
    }
    if (r.failed || r.at != differences.size()) return std::nullopt;
    for (const auto &field : model.fields) {
        const bool bad = (field.real && !finite_at(values.image, field.offset)) ||
                         (field.flag && values.image[field.offset] > 1);
        if (bad) std::memcpy(values.image.data() + field.offset, model.image.data() + field.offset, field.size);
    }
    return values;
}
} // namespace dingosdk::physics_tuning
