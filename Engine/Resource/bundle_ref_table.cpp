#include "bundle_ref_table.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace dingosdk::frostbite::bundle_ref {
namespace {
// Layout (all little-endian), as the game ships it:
//   header: 0 suffix string ptr, 8 lookups ptr, 16 path pool ptr (128),
//           24 bundle list ptr, 32 empty string ptr, 72 lookup count,
//           76 bundle count, 92 version (1)
//   pool:   path descriptors {u32 parent token, inline text or u32 offset}
//   bundles: {u32 descriptor token, u32 flags} per bundle, index 0 a sentinel
//   lookups: {u64 hash, u32 bundle index, u32 path token}, sorted by hash
//   trailer: 5 relocations (the header pointers), 20 bytes
// Resource meta is 16 bytes: payload size, trailer size (20), zero.
constexpr std::size_t maximum_bytes = 8 * 1024 * 1024;
constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

std::uint32_t u32(std::span<const std::byte> data, std::size_t at) {
    if (at > data.size() || data.size() - at < 4) throw std::runtime_error("truncated bundle-reference table");
    std::uint32_t value{};
    std::memcpy(&value, data.data() + at, 4);
    return value;
}
std::uint64_t u64(std::span<const std::byte> data, std::size_t at) {
    if (at > data.size() || data.size() - at < 8) throw std::runtime_error("truncated bundle-reference table");
    std::uint64_t value{};
    std::memcpy(&value, data.data() + at, 8);
    return value;
}
void put32(std::vector<std::byte>& data, std::size_t at, std::uint32_t value) { std::memcpy(data.data() + at, &value, 4); }
void put64(std::vector<std::byte>& data, std::size_t at, std::uint64_t value) { std::memcpy(data.data() + at, &value, 8); }

std::uint64_t hash(std::string_view name) {
    std::uint64_t value = 0xCBF29CE484222325ULL;
    for (auto c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        value = value * 0x100000001B3ULL ^ static_cast<unsigned char>(c);
    }
    return value;
}
std::string_view leaf(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

struct Row {
    std::uint64_t hash{};
    std::uint32_t bundle{}, token{};
};
struct Parsed {
    std::size_t pool{}, bundles{}, lookups{}, payload{};
    std::uint32_t bundle_count{};
    std::vector<Row> rows;
    std::map<std::string, std::uint32_t> presets; // path -> bundle index
};

class Reader {
public:
    Reader(std::span<const std::byte> data, std::span<const std::byte> meta) : data_(data) {
        if (data.size() < 0x100 || data.size() > maximum_bytes || meta.size() != 16)
            throw std::runtime_error("not a bundle-reference table");
        out.payload = data.size() - 20;
        if (u32(meta, 0) != out.payload || u32(meta, 4) != 20 || u64(meta, 8) != 0)
            throw std::runtime_error("bundle-reference table metadata differs");
        for (std::size_t i = 0; i < 5; ++i)
            if (u32(data, out.payload + i * 4) != i * 8) throw std::runtime_error("bundle-reference relocations differ");
        out.pool = pointer(16);
        out.bundles = pointer(24);
        out.lookups = pointer(8);
        if (out.pool != 128 || out.bundles <= out.pool || out.lookups <= out.bundles || (out.bundles & 15U) ||
            (out.lookups & 15U) || u32(data, 92) != 1)
            throw std::runtime_error("bundle-reference table layout differs");
        const auto count = u32(data, 72);
        out.bundle_count = u32(data, 76);
        if (count > 200000 || out.bundle_count < 2 || out.bundle_count > 100000 ||
            out.bundles + std::uint64_t{out.bundle_count} * 8 != out.lookups ||
            out.lookups + std::uint64_t{count} * 16 != out.payload)
            throw std::runtime_error("bundle-reference table counts differ");
        out.rows.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto at = out.lookups + std::size_t{i} * 16;
            Row row{u64(data, at), u32(data, at + 8), u32(data, at + 12)};
            if ((!out.rows.empty() && out.rows.back().hash >= row.hash) || row.bundle >= out.bundle_count)
                throw std::runtime_error("bundle-reference lookups are not sorted or name no bundle");
            out.rows.push_back(row);
            const auto path = text(row.token, 0);
            if (path.empty()) throw std::runtime_error("bundle-reference lookup has no path");
            out.presets.emplace(path, row.bundle);
        }
    }
    Parsed out;

private:
    std::size_t pointer(std::size_t at) const {
        const auto value = u64(data_, at);
        if (value >= out.payload) throw std::runtime_error("bundle-reference pointer out of range");
        return static_cast<std::size_t>(value);
    }
    std::string text(std::uint32_t token, unsigned depth) {
        if (token == none) return {};
        if (depth > 128) throw std::runtime_error("bundle-reference path is cyclic");
        if (const auto found = cache_.find(token); found != cache_.end()) return found->second;
        const auto at = out.pool + (token & 0x7FFFFFU);
        const auto length = token >> 24;
        if (!length || at > out.bundles || out.bundles - at < 8) throw std::runtime_error("bad bundle-reference path");
        const auto fragment = (token & 0x800000U) ? at + 4 : out.pool + u32(data_, at + 4);
        if (fragment < out.pool || fragment > out.bundles || out.bundles - fragment < length)
            throw std::runtime_error("bundle-reference path outside its pool");
        auto result = text(u32(data_, at), depth + 1);
        for (std::size_t i = 0; i < length; ++i) {
            const auto c = std::to_integer<unsigned char>(data_[fragment + i]);
            if (c < 32 || c > 126) throw std::runtime_error("bundle-reference path is not text");
            result.push_back(static_cast<char>(c));
        }
        cache_.emplace(token, result);
        return result;
    }
    std::span<const std::byte> data_;
    std::unordered_map<std::uint32_t, std::string> cache_;
};

// A table being added to: read once, each preset added in memory, written once. (Reading
// the whole table again for every preset made a merge of many cosmetic mods quadratic.)
class Builder {
public:
    Builder(std::span<const std::byte> base, const Parsed& parsed)
        : base_(base), table_(parsed), rows_(parsed.rows), size_(base.size()) {
        for (const auto& row : rows_) hashes_.insert(row.hash);
        // In path order, so each leaf name keeps the first preset that answers to it.
        for (const auto& preset : parsed.presets) leaves_.emplace(hash(leaf(preset.first)), preset.first);
    }
    // Appends `path` to the pool and a full/leaf row pair for it in `bundle`. A
    // leaf name another preset already answers to stays that preset's: the table
    // holds one row per hash, and mods number their presets alike (1_ap, 2_ap ...)
    // in different folders. Returns that preset's path, empty when the leaf row
    // went in too.
    std::string insert(const std::string& path, std::uint32_t bundle) {
        // What reading the table back at this point would refuse.
        if (size_ > maximum_bytes) throw std::runtime_error("not a bundle-reference table");
        if (rows_.size() > 200000) throw std::runtime_error("bundle-reference table counts differ");
        const auto full = hash(path), short_name = hash(leaf(path));
        if (hashes_.contains(full)) throw std::runtime_error(path + " collides with an existing lookup");
        // A path with no folder is its own leaf and has the one row.
        const bool with_leaf = full != short_name && !hashes_.contains(short_name);
        std::string holder;
        if (full != short_name && !with_leaf) {
            const auto found = leaves_.find(short_name);
            holder = found != leaves_.end() ? found->second : std::string("another lookup");
        }
        if (path.size() > 255) throw std::runtime_error(path + " is too long for a lookup");
        const auto growth = (4 + path.size() + 15) & ~std::size_t{15};
        const auto offset = table_.bundles + pool_.size() - table_.pool;
        if (offset > 0x7FFFFF) throw std::runtime_error("bundle-reference path pool is full");
        const auto token = static_cast<std::uint32_t>(offset) | 0x800000U | (static_cast<std::uint32_t>(path.size()) << 24);
        const auto at = pool_.size();
        pool_.resize(at + growth);
        put32(pool_, at, none);
        std::memcpy(pool_.data() + at + 4, path.data(), path.size());
        rows_.push_back({full, bundle, token});
        hashes_.insert(full);
        if (with_leaf) {
            rows_.push_back({short_name, bundle, token});
            hashes_.insert(short_name);
        }
        // The preset a later one with this leaf name is told about: the first in path order.
        if (const auto [held, fresh] = leaves_.emplace(short_name, path); !fresh && path < held->second) held->second = path;
        size_ += growth + (with_leaf ? 32 : 16);
        changed_ = true;
        return holder;
    }
    // The table as it now is; untouched when nothing was added.
    void write(std::vector<std::byte>& data, std::vector<std::byte>& meta) {
        if (!changed_) return;
        const auto bundles = table_.bundles + pool_.size(), lookups = table_.lookups + pool_.size();
        const auto payload = lookups + rows_.size() * 16;
        std::vector<std::byte> out(payload + 20);
        std::copy_n(base_.begin(), table_.bundles, out.begin());
        std::copy(pool_.begin(), pool_.end(), out.begin() + static_cast<std::ptrdiff_t>(table_.bundles));
        std::copy(base_.begin() + static_cast<std::ptrdiff_t>(table_.bundles), base_.begin() + static_cast<std::ptrdiff_t>(table_.lookups),
                  out.begin() + static_cast<std::ptrdiff_t>(bundles));
        std::ranges::sort(rows_, {}, &Row::hash);
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            put64(out, lookups + i * 16, rows_[i].hash);
            put32(out, lookups + i * 16 + 8, rows_[i].bundle);
            put32(out, lookups + i * 16 + 12, rows_[i].token);
        }
        std::copy(base_.begin() + static_cast<std::ptrdiff_t>(table_.payload), base_.end(), out.begin() + static_cast<std::ptrdiff_t>(payload));
        put64(out, 8, lookups);
        put64(out, 24, bundles);
        put32(out, 72, static_cast<std::uint32_t>(rows_.size()));
        put32(meta, 0, static_cast<std::uint32_t>(payload));
        data = std::move(out);
    }

private:
    std::span<const std::byte> base_;
    const Parsed& table_;
    std::vector<Row> rows_;
    std::unordered_set<std::uint64_t> hashes_;
    std::unordered_map<std::uint64_t, std::string> leaves_; // a leaf name's hash -> the preset that answers to it
    std::vector<std::byte> pool_;                           // the path descriptors added after the base's
    std::size_t size_{};                                    // the table's size in bytes with what was added
    bool changed_{};
};
} // namespace

bool is_table(std::string_view name) {
    constexpr std::string_view suffix = "_bundlereftable";
    if (name.size() < suffix.size()) return false;
    return std::equal(suffix.begin(), suffix.end(), name.end() - static_cast<std::ptrdiff_t>(suffix.size()),
                      [](char a, char b) { return a == (b >= 'A' && b <= 'Z' ? static_cast<char>(b + 32) : b); });
}

MergedTable merge(const Table& base, std::span<const Table> edits) {
    const auto original = Reader(base.resource, base.resourceMeta).out;
    const std::span<const std::byte> base_bundles = base.resource.subspan(original.bundles, original.lookups - original.bundles);
    MergedTable result{{base.resource.begin(), base.resource.end()}, {base.resourceMeta.begin(), base.resourceMeta.end()}};
    std::map<std::string, std::uint32_t> added;
    Builder builder(base.resource, original);
    for (std::size_t index = 0; index < edits.size(); ++index) {
        const auto& edit = edits[index];
        const auto copy = Reader(edit.resource, edit.resourceMeta).out;
        // Additions only: the same bundle list, and every base preset where it was.
        const auto bundles = edit.resource.subspan(copy.bundles, copy.lookups - copy.bundles);
        if (!std::ranges::equal(bundles, base_bundles))
            throw std::runtime_error("an edit changes the bundle list");
        for (const auto& [path, bundle] : original.presets) {
            const auto found = copy.presets.find(path);
            if (found == copy.presets.end() || found->second != bundle)
                throw std::runtime_error("an edit moves or drops " + path);
        }
        for (const auto& [path, bundle] : copy.presets) {
            if (original.presets.contains(path)) continue;
            if (const auto found = added.find(path); found != added.end()) {
                if (found->second != bundle) ++result.conflicts;
                continue;
            }
            if (auto holder = builder.insert(path, bundle); !holder.empty())
                result.shadowed.push_back({index, path, std::move(holder)});
            added.emplace(path, bundle);
            ++result.added;
        }
    }
    builder.write(result.resource, result.resourceMeta);
    return result;
}

} // namespace dingosdk::frostbite::bundle_ref
