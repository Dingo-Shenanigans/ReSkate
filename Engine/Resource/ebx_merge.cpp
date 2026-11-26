#include "ebx_merge.h"

#include "ebx_boxed.h"
#include "ebx_carry.h"
#include "ebx_writer.h"

#include <cstdint>
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace dingosdk::frostbite::ebx {
namespace {

// An exported instance carries its own identity in the low bits of `Flags`:
// every sublevel the game ships has `Flags == guid[0..3] & 0x01ffffff`. An
// authoring tool that clones a donor instance gives the copy a fresh guid but
// leaves the donor's id behind, which nothing notices until a second mod clones
// the same donor and the two copies collide. Re-deriving the id from the guid
// restores the invariant and is a no-op for an instance that already holds it.
constexpr std::uint32_t objectIdMask = 0x01ffffffU;

std::uint32_t object_id(const Guid& guid) {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[0]))) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(guid.bytes[3])) << 24U);
}

bool renumber(Object& object, const Guid& guid, const Document& source) {
    for (auto& field : object.fields) {
        if (field.name != "Flags") continue;
        // The reader keeps a signed field as int64 and an unsigned one as
        // uint64; `Flags` is unsigned in this schema but not in every schema.
        const auto* signedValue = std::get_if<std::int64_t>(&field.value.data);
        const auto* unsignedValue = std::get_if<std::uint64_t>(&field.value.data);
        if (!signedValue && !unsignedValue) return false;
        const auto current = signedValue ? static_cast<std::uint32_t>(*signedValue)
                                         : static_cast<std::uint32_t>(*unsignedValue);
        const auto renumbered = (current & ~objectIdMask) | (object_id(guid) & objectIdMask);
        if (renumbered == current) return false;
        // On most types `Flags` is a genuine flag word. It is only an id to
        // re-derive when it holds the id of some exported instance, which is
        // what a clone inherits from its donor.
        bool cloned{};
        for (const auto& instance : source.instances)
            if (instance.exported &&
                (object_id(instance.instanceGuid) & objectIdMask) == (current & objectIdMask)) {
                cloned = true;
                break;
            }
        if (!cloned) return false;
        if (signedValue) field.value.data = static_cast<std::int64_t>(renumbered);
        else field.value.data = static_cast<std::uint64_t>(renumbered);
        return true;
    }
    return false;
}

std::string type_name(const Document& document, std::int32_t descriptor) {
    if (descriptor < 0 || static_cast<std::size_t>(descriptor) >= document.types.size())
        throw std::runtime_error("EBX type descriptor is out of range");
    return document.types[static_cast<std::size_t>(descriptor)].name;
}

Value::Array* root_array(Document& document, std::string_view name) {
    auto* root = const_cast<InstanceRecord*>(document.root());
    if (!root || !root->object) return nullptr;
    for (auto& field : root->object->fields)
        if (field.name == name) return std::get_if<Value::Array>(&field.value.data);
    return nullptr;
}

const Value::Array* root_array(const Document& document, std::string_view name) {
    const auto* root = document.root();
    if (!root || !root->object) return nullptr;
    if (const auto* field = root->object->find(name)) return std::get_if<Value::Array>(&field->value.data);
    return nullptr;
}

// A type's name, which is what two documents that number their types
// independently have in common. Empty for a descriptor that names none.
std::string type_or_none(const Document& document, std::int32_t descriptor) {
    return descriptor >= 0 && static_cast<std::size_t>(descriptor) < document.types.size()
        ? document.types[static_cast<std::size_t>(descriptor)].name : std::string{};
}

constexpr auto unmatched = static_cast<std::size_t>(-1);

// Which of an edit's instances each of the base's is: the root is the root, an
// exported one is found by its guid, and an internal one, which has nothing
// else to go by, by its place among the internal ones. An edit that added or
// dropped an internal instance has moved the rest along, so then none of them
// is matched. Two that are not of one type are not a match either.
std::vector<std::size_t> match_instances(const Document& base, const Document& edit) {
    std::vector<std::size_t> match(base.instances.size(), unmatched);
    if (base.instances.empty() || edit.instances.empty()) return match;
    match[0] = 0;
    std::map<Guid, std::size_t> exported;
    std::vector<std::size_t> internal, baseInternal;
    for (std::size_t index = 1; index < edit.instances.size(); ++index) {
        if (edit.instances[index].exported) exported.emplace(edit.instances[index].instanceGuid, index);
        else internal.push_back(index);
    }
    for (std::size_t index = 1; index < base.instances.size(); ++index) {
        if (!base.instances[index].exported) { baseInternal.push_back(index); continue; }
        if (const auto found = exported.find(base.instances[index].instanceGuid); found != exported.end())
            match[index] = found->second;
    }
    if (internal.size() == baseInternal.size())
        for (std::size_t at = 0; at < internal.size(); ++at) match[baseInternal[at]] = internal[at];
    for (std::size_t index = 0; index < match.size(); ++index)
        if (match[index] != unmatched && type_or_none(base, base.instances[index].descriptor) !=
                                             type_or_none(edit, edit.instances[match[index]].descriptor))
            match[index] = unmatched;
    return match;
}

bool same(double left, double right) { return std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right); }
bool same(const ResourceReference& left, const ResourceReference& right) { return left.id == right.id; }
template <typename Type> bool same(const Type& left, const Type& right) { return left == right; }

// Which of the base's entries each entry of one of the result's lists is (-1 for one an edit
// put in), for every list inside the document an edit has resized so far, by its path: the
// instance, the fields down to the list and the base's place of each list entry on the way.
// Paths in the base's terms stay the same however the edits move the result's entries about.
using Origins = std::map<std::string, std::vector<std::ptrdiff_t>, std::less<>>;
// The references the edits so far put into each such list, by path.
using Inserted = std::map<std::string, std::set<std::pair<PointerKind, std::int32_t>>, std::less<>>;

// One step from a list of the base to the same list in an edit.
struct Step {
    enum class Kind { keep, remove, insert } kind{};
    std::size_t base{};   // keep, remove: the base's entry
    std::size_t edit{};   // keep, insert: the edit's entry
    std::size_t gap{};    // insert: the base entry it goes in front of (the base's length for the end)
};

// Finds what one edit changed in place. The base and the edit are walked side
// by side, along with the result: it began as a copy of the base, so it has the
// same shape, and may already hold what an earlier edit changed. Nothing is
// written until the whole edit has been seen, so an edit either has all its
// values taken or none of them.
class InPlace final {
public:
    InPlace(const Document& base, const Document& edit, Document& result, const std::vector<std::size_t>& match,
            Origins& origins)
        : base_(base), edit_(edit), result_(result), match_(match), origins_(origins) {}

    // The instance whose fields come next.
    void at(std::size_t instance) { path_ = "#" + std::to_string(instance); }

    void fields(const Object& base, const Object& edit, Object& result, bool root) {
        if (base.fields.size() != edit.fields.size() || base.fields.size() != result.fields.size()) { uncarried = true; return; }
        for (std::size_t index = 0; index < base.fields.size(); ++index) {
            if (base.fields[index].name != edit.fields[index].name) { uncarried = true; return; }
            const auto length = path_.size();
            path_ += '/';
            path_ += base.fields[index].name;
            value(base.fields[index].value, edit.fields[index].value, result.fields[index].value, root);
            path_.resize(length);
        }
    }

    // Writes what the edit changed. `carrier` brings the entries it put into lists across.
    void apply(detail::Carrier& carrier, Inserted& inserted, std::size_t& repeated) {
        for (const auto& [to, from] : changes_) to->data = from->data;
        for (const auto& patch : patches_) {
            if (patch.text) write_boxed_text(result_, patch.offset, *patch.text);
            else write_boxed_bytes(result_, patch.offset, patch.bytes);
        }
        // Inner lists were recorded before the lists that hold them, so each is still
        // where it was found when it is rebuilt.
        for (auto& change : lists_) {
            auto& old = std::get<Value::Array>(change.list->data);
            std::ranges::stable_sort(change.inserts, {}, &std::pair<std::size_t, const Value*>::first);
            const std::set<std::size_t> removed(change.removed.begin(), change.removed.end());
            auto& earlier = inserted[change.path];
            std::set<std::pair<PointerKind, std::int32_t>> mine;
            Value::Array next;
            std::vector<std::ptrdiff_t> origin;
            std::size_t pending{};
            // The entries this edit put in front of base entry `gap` and those before it.
            const auto place = [&](std::size_t gap) {
                for (; pending < change.inserts.size() && change.inserts[pending].first <= gap; ++pending) {
                    auto carried = carrier.value(*change.inserts[pending].second);
                    // Two edits that put the same reference in one list mean it once.
                    if (const auto* reference = std::get_if<PointerReference>(&carried.data);
                        reference && reference->kind != PointerKind::null) {
                        const std::pair key{reference->kind, reference->index};
                        if (earlier.contains(key)) { ++repeated; continue; }
                        mine.insert(key);
                    }
                    next.push_back(std::move(carried));
                    origin.push_back(-1);
                }
            };
            for (std::size_t index = 0; index < old.size(); ++index) {
                const auto from = change.origin[index];
                if (from >= 0) {
                    place(static_cast<std::size_t>(from));
                    if (removed.contains(static_cast<std::size_t>(from))) continue;
                }
                next.push_back(std::move(old[index]));
                origin.push_back(from);
            }
            place(static_cast<std::size_t>(-1));
            old = std::move(next);
            origins_[change.path] = std::move(origin);
            earlier.merge(mine);
        }
    }

    std::size_t values{};     // values the edit changed from the base
    std::size_t contested{};  // of those, ones the result already had changed to something else
    std::size_t entries{};    // list entries the edit put in or took out, inside the document
    bool uncarried{};         // a change was seen that cannot be taken
    bool unread{};            // somewhere a change could not be looked for

private:
    enum class Side { edit, result };

    template <typename Type> bool plain(const Value& base, const Value& edit, Value& result) {
        const auto* before = std::get_if<Type>(&base.data);
        if (!before) return false;
        const auto& after = std::get<Type>(edit.data);
        if (same(*before, after)) return true;
        ++values;
        const auto& current = std::get<Type>(result.data);
        if (same(current, after)) return true;
        if (!same(current, *before)) ++contested;
        changes_.emplace_back(&result, &edit);
        return true;
    }

    void value(const Value& base, const Value& edit, Value& result, bool rootList) {
        if (base.data.index() != edit.data.index() || base.data.index() != result.data.index()) { uncarried = true; return; }
        if (std::holds_alternative<std::monostate>(base.data)) return;
        if (plain<bool>(base, edit, result) || plain<std::int64_t>(base, edit, result) ||
            plain<std::uint64_t>(base, edit, result) || plain<double>(base, edit, result) ||
            plain<std::string>(base, edit, result) || plain<Guid>(base, edit, result) ||
            plain<Sha1>(base, edit, result) || plain<ResourceReference>(base, edit, result)) return;
        if (const auto* before = std::get_if<PointerReference>(&base.data)) {
            reference(*before, std::get<PointerReference>(edit.data));
            return;
        }
        if (const auto* before = std::get_if<std::shared_ptr<Object>>(&base.data)) {
            const auto& after = std::get<std::shared_ptr<Object>>(edit.data);
            const auto& mine = std::get<std::shared_ptr<Object>>(result.data);
            if (!*before && !after) return;
            if (!*before || !after || !mine) { uncarried = true; return; }
            fields(**before, *after, *mine, false);
            return;
        }
        if (const auto* before = std::get_if<Value::Array>(&base.data)) {
            const auto& after = std::get<Value::Array>(edit.data);
            if (!rootList) {
                list(*before, after, result);
                return;
            }
            // A list on the root grows at its end, and what was appended comes
            // across on its own (merge_documents). One that shrank is not taken.
            auto& mine = std::get<Value::Array>(result.data);
            if (after.size() < before->size() || mine.size() < before->size()) { uncarried = true; return; }
            for (std::size_t index = 0; index < before->size(); ++index) {
                const auto length = path_.size();
                path_ += '[' + std::to_string(index) + ']';
                value((*before)[index], after[index], mine[index], false);
                path_.resize(length);
            }
            return;
        }
        if (const auto* before = std::get_if<TypeReference>(&base.data)) {
            uncarried |= !same_type(*before, std::get<TypeReference>(edit.data), edit_);
            return;
        }
        if (const auto* before = std::get_if<BoxedReference>(&base.data))
            boxed(*before, std::get<BoxedReference>(edit.data), std::get<BoxedReference>(result.data));
    }

    bool same_type(const TypeReference& before, const TypeReference& after, const Document& other) const {
        if (before.primitive != after.primitive) return false;
        if (before.primitive) return before.primitiveType == after.primitiveType;
        return type_or_none(base_, before.descriptor) == type_or_none(other, after.descriptor);
    }

    // A boxed value is read field by field from the bytes each document holds, and a change to
    // one of its numbers or texts is written over the result's bytes (ebx_boxed.h). Its shape
    // has to stay as the base has it: what it holds is laid out where the document was written.
    void boxed(const BoxedReference& before, const BoxedReference& after, const BoxedReference& current) {
        if ((before.encodedType == 0) != (after.encodedType == 0)) { uncarried = true; return; }
        if (before.encodedType == 0) return;
        const auto was = read_boxed(base_, before), now = read_boxed(edit_, after), mine = read_boxed(result_, current);
        if (!was || !now || !mine || mine->size() != was->size()) { unread = true; return; }
        if (now->size() != was->size()) { uncarried = true; return; }
        for (std::size_t index = 0; index < was->size(); ++index) {
            const auto& from = (*was)[index];
            const auto& to = (*now)[index];
            const auto& held = (*mine)[index];
            if (from.kind != to.kind || held.kind != from.kind) { uncarried = true; return; }
            switch (from.kind) {
            case BoxedLeaf::Kind::shape:
            case BoxedLeaf::Kind::type:
                if (from.text != to.text) { uncarried = true; return; }
                break;
            case BoxedLeaf::Kind::pointer:
                reference(from.pointer, to.pointer);
                break;
            case BoxedLeaf::Kind::bytes:
                if (from.bytes == to.bytes) break;
                ++values;
                if (held.bytes == to.bytes) break;
                if (held.bytes != from.bytes) ++contested;
                patches_.push_back({held.offset, to.bytes, std::nullopt});
                break;
            case BoxedLeaf::Kind::text:
                if (from.text == to.text) break;
                ++values;
                if (held.text == to.text) break;
                if (held.text != from.text) ++contested;
                patches_.push_back({held.offset, {}, to.text});
                break;
            }
        }
    }

    // A list inside the document that an edit resized: its entries are lined up against the
    // base's, so what the edit put in and took out is known by place, as Studio's array editing
    // does, and an entry both have is compared as any other value. Entries go into the result's
    // list where they stand among the base's entries; an earlier edit may already have moved
    // those, which `origins_` keeps track of.
    void list(const Value::Array& before, const Value::Array& after, Value& result) {
        auto& mine = std::get<Value::Array>(result.data);
        std::vector<std::ptrdiff_t> origin;
        if (const auto found = origins_.find(path_); found != origins_.end()) {
            origin = found->second;
        } else {
            origin.resize(before.size());
            for (std::size_t index = 0; index < origin.size(); ++index) origin[index] = static_cast<std::ptrdiff_t>(index);
        }
        if (origin.size() != mine.size()) { unread = true; return; }
        std::vector<std::ptrdiff_t> where(before.size(), -1);
        for (std::size_t index = 0; index < origin.size(); ++index)
            if (origin[index] >= 0 && static_cast<std::size_t>(origin[index]) < before.size())
                where[static_cast<std::size_t>(origin[index])] = static_cast<std::ptrdiff_t>(index);

        // A list of the same length is lined up too: one entry taken from the front and one put
        // at the end is that, and not every entry changed. One with more than a few hundred
        // entries changed is compared place by place, as before: that is one comparison for
        // each entry, where lining it up would be one for each pair.
        std::vector<Step> steps;
        const auto limit = after.size() == before.size() ? std::size_t{1} << 16 : std::size_t{1} << 22;
        if (auto aligned = align(before, after, limit)) {
            steps = std::move(*aligned);
        } else if (after.size() == before.size()) {
            for (std::size_t index = 0; index < before.size(); ++index) steps.push_back({Step::Kind::keep, index, index, 0});
        } else {
            uncarried = true;
            return;
        }
        ListChange change{&result, path_, origin, {}, {}};
        for (const auto& step : steps) {
            switch (step.kind) {
            case Step::Kind::keep: {
                const auto at = where[step.base];
                if (at < 0) {
                    // An earlier edit took the entry out; this edit's change to it is lost.
                    if (!equal(before[step.base], after[step.edit], Side::edit)) { ++values; ++contested; }
                    break;
                }
                const auto length = path_.size();
                path_ += '[' + std::to_string(step.base) + ']';
                value(before[step.base], after[step.edit], mine[static_cast<std::size_t>(at)], false);
                path_.resize(length);
                break;
            }
            case Step::Kind::remove: {
                ++entries;
                const auto at = where[step.base];
                if (at < 0) break;   // an earlier edit took it out as well
                if (!equal(before[step.base], mine[static_cast<std::size_t>(at)], Side::result)) ++contested;
                change.removed.push_back(step.base);
                break;
            }
            case Step::Kind::insert:
                ++entries;
                change.inserts.emplace_back(step.gap, &after[step.edit]);
                break;
            }
        }
        if (!change.removed.empty() || !change.inserts.empty()) lists_.push_back(std::move(change));
    }

    // Lines a list of the edit up against the base's: the entries both have in common, by
    // their longest common run (after the ones at either end that agree), and between two of
    // those the base's entries taken out and the edit's put in. As many of each between the
    // same two common entries is the same entries changed, and those are compared in place.
    std::optional<std::vector<Step>> align(const Value::Array& before, const Value::Array& after, std::size_t limit) const {
        const auto n = before.size(), m = after.size();
        std::size_t head{}, tail{};
        while (head < n && head < m && equal(before[head], after[head], Side::edit)) ++head;
        while (tail < n - head && tail < m - head && equal(before[n - 1 - tail], after[m - 1 - tail], Side::edit)) ++tail;
        const auto rows = n - head - tail, columns = m - head - tail;
        // Past `limit` comparisons the list is not lined up.
        if (rows && columns && rows > limit / columns) return std::nullopt;
        const auto width = columns + 1;
        std::vector<std::uint32_t> common((rows + 1) * width);
        std::vector<bool> alike(rows * columns);
        for (auto row = rows; row-- > 0;)
            for (auto column = columns; column-- > 0;) {
                const bool same_ = equal(before[head + row], after[head + column], Side::edit);
                alike[row * columns + column] = same_;
                common[row * width + column] = same_ ? common[(row + 1) * width + column + 1] + 1
                                                     : std::max(common[(row + 1) * width + column], common[row * width + column + 1]);
            }
        std::vector<Step> raw;
        for (std::size_t index = 0; index < head; ++index) raw.push_back({Step::Kind::keep, index, index, 0});
        for (std::size_t row = 0, column = 0; row < rows || column < columns;) {
            if (row < rows && column < columns && alike[row * columns + column]) {
                raw.push_back({Step::Kind::keep, head + row, head + column, 0});
                ++row, ++column;
            } else if (column < columns && (row == rows || common[row * width + column + 1] >= common[(row + 1) * width + column])) {
                raw.push_back({Step::Kind::insert, 0, head + column, head + row});
                ++column;
            } else {
                raw.push_back({Step::Kind::remove, head + row, 0, 0});
                ++row;
            }
        }
        for (std::size_t index = 0; index < tail; ++index) raw.push_back({Step::Kind::keep, n - tail + index, m - tail + index, 0});

        std::vector<Step> steps;
        for (std::size_t index = 0; index < raw.size();) {
            if (raw[index].kind == Step::Kind::keep) { steps.push_back(raw[index++]); continue; }
            std::vector<Step> taken, put;
            for (; index < raw.size() && raw[index].kind != Step::Kind::keep; ++index)
                (raw[index].kind == Step::Kind::remove ? taken : put).push_back(raw[index]);
            if (taken.size() == put.size()) {
                for (std::size_t pair = 0; pair < taken.size(); ++pair)
                    steps.push_back({Step::Kind::keep, taken[pair].base, put[pair].edit, 0});
            } else {
                steps.insert(steps.end(), taken.begin(), taken.end());
                steps.insert(steps.end(), put.begin(), put.end());
            }
        }
        return steps;
    }

    // Whether a value of the base and one of the edit (or of the result) are the same value:
    // references by what they lead to, since each document numbers its instances and imports.
    bool equal(const Value& left, const Value& right, Side side) const {
        const auto& other = side == Side::edit ? edit_ : static_cast<const Document&>(result_);
        if (left.data.index() != right.data.index()) return false;
        if (std::holds_alternative<std::monostate>(left.data)) return true;
        if (const auto* value = std::get_if<double>(&left.data)) return same(*value, std::get<double>(right.data));
        if (const auto* value = std::get_if<ResourceReference>(&left.data)) return same(*value, std::get<ResourceReference>(right.data));
        if (const auto* value = std::get_if<PointerReference>(&left.data))
            return same_target(*value, std::get<PointerReference>(right.data), side);
        if (const auto* value = std::get_if<std::shared_ptr<Object>>(&left.data)) {
            const auto& with = std::get<std::shared_ptr<Object>>(right.data);
            if (!*value || !with) return !*value && !with;
            if ((*value)->fields.size() != with->fields.size()) return false;
            for (std::size_t index = 0; index < with->fields.size(); ++index)
                if ((*value)->fields[index].name != with->fields[index].name ||
                    !equal((*value)->fields[index].value, with->fields[index].value, side))
                    return false;
            return true;
        }
        if (const auto* value = std::get_if<Value::Array>(&left.data)) {
            const auto& with = std::get<Value::Array>(right.data);
            if (value->size() != with.size()) return false;
            for (std::size_t index = 0; index < with.size(); ++index)
                if (!equal((*value)[index], with[index], side)) return false;
            return true;
        }
        if (const auto* value = std::get_if<TypeReference>(&left.data)) return same_type(*value, std::get<TypeReference>(right.data), other);
        if (const auto* value = std::get_if<BoxedReference>(&left.data)) {
            const auto& with = std::get<BoxedReference>(right.data);
            if (value->encodedType == 0 || with.encodedType == 0) return value->encodedType == with.encodedType;
            const auto was = read_boxed(base_, *value), now = read_boxed(other, with);
            if (!was || !now || was->size() != now->size()) return false;
            for (std::size_t index = 0; index < was->size(); ++index) {
                const auto& from = (*was)[index];
                const auto& to = (*now)[index];
                if (from.kind != to.kind || from.text != to.text || from.bytes != to.bytes ||
                    (from.kind == BoxedLeaf::Kind::pointer && !same_target(from.pointer, to.pointer, side)))
                    return false;
            }
            return true;
        }
        return std::visit([&](const auto& held) {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, bool> || std::is_same_v<Held, std::int64_t> ||
                          std::is_same_v<Held, std::uint64_t> || std::is_same_v<Held, std::string> ||
                          std::is_same_v<Held, Guid> || std::is_same_v<Held, Sha1>)
                return held == std::get<Held>(right.data);
            else
                return false;
        }, left.data);
    }

    bool same_target(const PointerReference& left, const PointerReference& right, Side side) const {
        if (left.kind != right.kind) return false;
        if (left.kind == PointerKind::null) return true;
        if (left.index < 0 || right.index < 0) return left.index == right.index;
        const auto from = static_cast<std::size_t>(left.index), to = static_cast<std::size_t>(right.index);
        if (left.kind == PointerKind::external) {
            const auto& imports = side == Side::edit ? edit_.imports : result_.imports;
            return from < base_.imports.size() && to < imports.size() &&
                   base_.imports[from].fileGuid == imports[to].fileGuid &&
                   base_.imports[from].classGuid == imports[to].classGuid;
        }
        if (side == Side::result) return from == to;
        return from < match_.size() && match_[from] == to;
    }

    // A reference still has to lead to the same place; one pointed elsewhere
    // cannot be taken, because what it points at may not be in the result.
    void reference(const PointerReference& before, const PointerReference& after) {
        if (before.kind != after.kind) { uncarried = true; return; }
        if (before.kind == PointerKind::null) return;
        if (before.index < 0 || after.index < 0) { uncarried |= before.index != after.index; return; }
        const auto from = static_cast<std::size_t>(before.index), to = static_cast<std::size_t>(after.index);
        if (before.kind == PointerKind::external) {
            uncarried |= from >= base_.imports.size() || to >= edit_.imports.size() ||
                         base_.imports[from].fileGuid != edit_.imports[to].fileGuid ||
                         base_.imports[from].classGuid != edit_.imports[to].classGuid;
            return;
        }
        if (from >= match_.size()) { uncarried = true; return; }
        // An internal instance that could not be matched is not known to have moved.
        if (match_[from] == unmatched) (base_.instances[from].exported ? uncarried : unread) = true;
        else uncarried |= match_[from] != to;
    }

    struct Patch {
        std::uint32_t offset{};
        std::vector<std::byte> bytes;
        std::optional<std::string> text;
    };
    struct ListChange {
        Value* list{};
        std::string path;
        std::vector<std::ptrdiff_t> origin;   // as the list was when the edit was looked at
        std::vector<std::size_t> removed;     // base entries
        std::vector<std::pair<std::size_t, const Value*>> inserts;   // (gap, the edit's entry)
    };

    const Document& base_;
    const Document& edit_;
    Document& result_;
    const std::vector<std::size_t>& match_;
    Origins& origins_;
    std::string path_;
    std::vector<std::pair<Value*, const Value*>> changes_;
    std::vector<Patch> patches_;
    std::vector<ListChange> lists_;
};

template <typename Visit> void each_pointer(Value& value, const Visit& visit) {
    if (auto* pointer = std::get_if<PointerReference>(&value.data)) { visit(*pointer); return; }
    if (auto* nested = std::get_if<std::shared_ptr<Object>>(&value.data)) {
        if (*nested) for (auto& field : (*nested)->fields) each_pointer(field.value, visit);
        return;
    }
    if (auto* list = std::get_if<Value::Array>(&value.data))
        for (auto& element : *list) each_pointer(element, visit);
}

} // namespace

namespace detail {

Carrier::Carrier(const Document& from, Document& to) : from_(from), to_(to) {
    for (std::size_t index = 0; index < to.types.size(); ++index)
        typeByName_.emplace(to.types[index].name, static_cast<std::int32_t>(index));
    for (std::size_t index = 0; index < to.instances.size(); ++index)
        instanceByGuid_.emplace(to.instances[index].instanceGuid, index);
}

void Carrier::alias(std::size_t from, std::size_t to) { aliases_[from] = to; }

std::int32_t Carrier::type(std::int32_t descriptor) const {
    const auto name = type_name(from_, descriptor);
    const auto found = typeByName_.find(name);
    if (found == typeByName_.end())
        throw std::runtime_error("EBX merge needs a type the base does not define: " + name);
    return found->second;
}

std::size_t Carrier::instance(std::size_t index) {
    if (index >= from_.instances.size()) throw std::runtime_error("EBX pointer is out of range");
    if (const auto aliased = aliases_.find(index); aliased != aliases_.end()) return aliased->second;
    const auto& source = from_.instances[index];
    if (source.exported) {
        if (const auto found = instanceByGuid_.find(source.instanceGuid); found != instanceByGuid_.end())
            return found->second;
    } else if (const auto found = carriedInternal_.find(index); found != carriedInternal_.end()) {
        return found->second;
    }
    if (!source.object) throw std::runtime_error("EBX instance has no parsed object");
    // Reserve the slot before copying, so a cycle back to this instance
    // resolves instead of recursing for ever.
    const auto descriptor = type(source.descriptor);
    InstanceRecord carried;
    carried.fixupType = ensure_fixup_type(to_, descriptor);
    carried.descriptor = descriptor;
    carried.exported = source.exported;
    carried.instanceGuid = source.instanceGuid;
    carried.object = std::make_shared<Object>();
    carried.object->descriptor = descriptor;
    to_.instances.push_back(std::move(carried));
    const auto at = to_.instances.size() - 1;
    // Internal instances carry no guid, so only exported ones are keyed.
    if (source.exported) instanceByGuid_.emplace(source.instanceGuid, at);
    else carriedInternal_.emplace(index, at);
    // The writer lays an instance down over its original image and only
    // then writes the parsed fields, so padding and anything the field walk
    // does not cover has to travel with it.
    to_.instances[at].rawImage = source.rawImage;
    // Each pointer followed recurses: a mod's long pointer chain must fail, not overflow the stack.
    if (depth_ >= 512) throw std::runtime_error("EBX pointers chain too deeply to merge");
    ++depth_;
    struct Leave { unsigned& depth; ~Leave() { --depth; } } leave{depth_};
    auto copied = object(*source.object);
    to_.instances[at].object->fields = std::move(copied->fields);
    if (source.exported) renumbered += renumber(*to_.instances[at].object, source.instanceGuid, from_);
    ++instances;
    return at;
}

std::optional<std::uint16_t> Carrier::type_row(std::uint16_t row) {
    if (row >= from_.fixupTypeGuids.size()) return std::nullopt;
    const auto& guid = from_.fixupTypeGuids[row];
    const auto signedRow = row < from_.fixupTypeSignatures.size();
    for (std::size_t at = 0; at < to_.fixupTypeGuids.size() && at <= 0xFFFF; ++at) {
        if (to_.fixupTypeGuids[at] != guid) continue;
        const auto signedAt = at < to_.fixupTypeSignatures.size();
        if (signedRow && signedAt && to_.fixupTypeSignatures[at] != from_.fixupTypeSignatures[row]) continue;
        return static_cast<std::uint16_t>(at);
    }
    // A type the base describes but lists no row for gets one.
    for (std::size_t index = 0; index < from_.types.size(); ++index)
        if (from_.types[index].guid == guid && (!signedRow || from_.types[index].signature == from_.fixupTypeSignatures[row])) {
            const auto found = typeByName_.find(from_.types[index].name);
            if (found == typeByName_.end()) return std::nullopt;
            return ensure_fixup_type(to_, found->second);
        }
    return std::nullopt;
}

std::int32_t Carrier::import(std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= from_.imports.size())
        throw std::runtime_error("EBX import pointer is out of range");
    const auto& reference = from_.imports[static_cast<std::size_t>(index)];
    for (std::size_t at = 0; at < to_.imports.size(); ++at)
        if (to_.imports[at].fileGuid == reference.fileGuid &&
            to_.imports[at].classGuid == reference.classGuid)
            return static_cast<std::int32_t>(at);
    to_.imports.push_back(reference);
    return static_cast<std::int32_t>(to_.imports.size() - 1);
}

std::shared_ptr<Object> Carrier::object(const Object& source) {
    auto result = std::make_shared<Object>();
    result->descriptor = type(source.descriptor);
    result->fields.reserve(source.fields.size());
    for (const auto& field : source.fields)
        result->fields.push_back({field.descriptor, field.name, value(field.value)});
    return result;
}

Value Carrier::value(const Value& source) {
    Value result;
    if (const auto* pointer = std::get_if<PointerReference>(&source.data)) {
        auto copy = *pointer;
        if (pointer->kind == PointerKind::internal && pointer->index >= 0)
            copy.index = static_cast<std::int32_t>(instance(static_cast<std::size_t>(pointer->index)));
        else if (pointer->kind == PointerKind::external)
            copy.index = import(pointer->index);
        result.data = copy;
        return result;
    }
    if (const auto* nested = std::get_if<std::shared_ptr<Object>>(&source.data)) {
        result.data = *nested ? object(**nested) : nullptr;
        return result;
    }
    if (const auto* array = std::get_if<Value::Array>(&source.data)) {
        Value::Array copied;
        copied.reserve(array->size());
        for (const auto& element : *array) copied.push_back(value(element));
        result.data = std::move(copied);
        return result;
    }
    if (const auto* reference = std::get_if<TypeReference>(&source.data)) {
        auto copy = *reference;
        if (!reference->primitive && reference->descriptor >= 0) copy.descriptor = type(reference->descriptor);
        result.data = copy;
        return result;
    }
    if (const auto* boxed = std::get_if<BoxedReference>(&source.data)) {
        result.data = carry_boxed(from_, *boxed, to_, [this](std::uint16_t row) { return type_row(row); });
        return result;
    }
    result = source;
    return result;
}

std::shared_ptr<Object> clone_object(const Object& source) {
    auto result = std::make_shared<Object>();
    result->descriptor = source.descriptor;
    result->fields.reserve(source.fields.size());
    for (const auto& field : source.fields)
        result->fields.push_back({field.descriptor, field.name, clone_value(field.value)});
    return result;
}

Value clone_value(const Value& source) {
    Value result;
    if (const auto* nested = std::get_if<std::shared_ptr<Object>>(&source.data)) {
        result.data = *nested ? clone_object(**nested) : nullptr;
        return result;
    }
    if (const auto* array = std::get_if<Value::Array>(&source.data)) {
        Value::Array copied;
        copied.reserve(array->size());
        for (const auto& element : *array) copied.push_back(clone_value(element));
        result.data = std::move(copied);
        return result;
    }
    result = source;
    return result;
}

Document clone_document(const Document& source) {
    Document result = source;
    for (auto& instance : result.instances)
        if (instance.object) instance.object = clone_object(*instance.object);
    return result;
}

// Every document the game ships keeps its exported instances, after the root,
// ascending by stored guid, and the engine binary-searches that table to resolve
// a reference into the partition. An instance appended at the end leaves the
// table unsorted: one appended guid is usually still found by luck, two are not,
// and the reference that goes missing comes back as a null object. So carried
// instances are put where they belong and every internal pointer is remapped.
void sort_instances(Document& document) {
    if (document.instances.size() < 3) return;
    // The root keeps slot 0 and is not part of the searched range. Exported
    // instances follow it in guid order; internal ones, which have no guid and
    // must come last, keep their relative order behind them.
    std::vector<std::size_t> exported;
    std::vector<std::size_t> internal;
    for (std::size_t index = 1; index < document.instances.size(); ++index)
        (document.instances[index].exported ? exported : internal).push_back(index);
    std::stable_sort(exported.begin(), exported.end(), [&](std::size_t left, std::size_t right) {
        return std::memcmp(document.instances[left].instanceGuid.bytes.data(),
                           document.instances[right].instanceGuid.bytes.data(), 16) < 0;
    });
    std::vector<std::size_t> order{0};
    order.insert(order.end(), exported.begin(), exported.end());
    order.insert(order.end(), internal.begin(), internal.end());
    bool moved{};
    for (std::size_t index = 0; index < order.size(); ++index) moved |= order[index] != index;
    if (!moved) return;

    std::vector<std::int32_t> moveTo(order.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        moveTo[order[index]] = static_cast<std::int32_t>(index);

    std::vector<InstanceRecord> sorted;
    sorted.reserve(order.size());
    for (const auto from : order) sorted.push_back(std::move(document.instances[from]));
    document.instances = std::move(sorted);

    const auto remap = [&](auto&& self, Value& value) -> void {
        if (auto* pointer = std::get_if<PointerReference>(&value.data)) {
            if (pointer->kind == PointerKind::internal && pointer->index >= 0 &&
                static_cast<std::size_t>(pointer->index) < moveTo.size())
                pointer->index = moveTo[static_cast<std::size_t>(pointer->index)];
            return;
        }
        if (auto* nested = std::get_if<std::shared_ptr<Object>>(&value.data)) {
            if (*nested) for (auto& field : (*nested)->fields) self(self, field.value);
            return;
        }
        if (auto* list = std::get_if<Value::Array>(&value.data))
            for (auto& element : *list) self(self, element);
    };
    for (auto& instance : document.instances)
        if (instance.object)
            for (auto& field : instance.object->fields) remap(remap, field.value);
}


} // namespace detail

Document merge_documents(const Document& base, const std::span<const Document* const> edits,
                         MergeSummary* summary, const CarryImport& carry) {
    auto result = detail::clone_document(base);
    const auto* baseRoot = base.root();
    if (!baseRoot || !baseRoot->object) throw std::runtime_error("EBX base has no root object");

    // Snapshot how long each root array started out, so every edit is measured
    // against the base rather than against the edits applied before it.
    std::map<std::string, std::size_t, std::less<>> baseLengths;
    for (const auto& field : baseRoot->object->fields)
        if (const auto* array = std::get_if<Value::Array>(&field.value.data))
            baseLengths.emplace(field.name, array->size());

    MergeSummary totals;
    Origins origins;
    Inserted inserted;
    // The references the edits so far appended to each root array.
    std::map<std::string, std::set<std::pair<PointerKind, std::int32_t>>, std::less<>> appended;
    for (std::size_t index = 0; index < edits.size(); ++index) {
        const auto* edit = edits[index];
        if (!edit) continue;
        const auto* editRoot = edit->root();
        if (!editRoot || !editRoot->object) throw std::runtime_error("EBX edit has no root object");

        // What the edit changed in place. The result's first instances are still
        // the base's, in the base's order: carried ones are added behind them,
        // and nothing is sorted until every edit is in.
        const auto match = match_instances(base, *edit);
        InPlace changes(base, *edit, result, match, origins);
        for (std::size_t at = 0; at < base.instances.size(); ++at) {
            const auto& before = base.instances[at];
            if (match[at] == unmatched) {
                // An exported instance is found whenever the edit still has it.
                (before.exported ? changes.uncarried : changes.unread) = true;
                continue;
            }
            const auto& after = edit->instances[match[at]];
            if (!before.object || !after.object || !result.instances[at].object) {
                changes.unread |= before.object || after.object;
                continue;
            }
            changes.at(at);
            changes.fields(*before.object, *after.object, *result.instances[at].object, at == 0);
        }
        detail::Carrier mapper(*edit, result);
        // What an entry the edit put into a list points at among the base's own internal
        // instances is that instance in the result, not a copy of it.
        for (std::size_t at = 0; at < match.size(); ++at)
            if (match[at] != unmatched && !base.instances[at].exported) mapper.alias(match[at], at);
        if (changes.uncarried) {
            totals.uncarried.push_back(index);
        } else {
            changes.apply(mapper, inserted, totals.repeated);
            totals.values += changes.values;
            totals.contested += changes.contested;
            totals.entries += changes.entries;
        }
        if (changes.unread) totals.unread.push_back(index);

        std::map<std::string, std::set<std::pair<PointerKind, std::int32_t>>, std::less<>> mine;

        // Whatever an edit appended to one of the root's arrays comes across,
        // pulling in the instances those entries point at.
        for (const auto& field : editRoot->object->fields) {
            const auto* editArray = std::get_if<Value::Array>(&field.value.data);
            if (!editArray) continue;
            const auto known = baseLengths.find(field.name);
            const auto had = known == baseLengths.end() ? 0 : known->second;
            if (editArray->size() <= had) continue;
            auto* target = root_array(result, field.name);
            if (!target)
                throw std::runtime_error("EBX merge cannot find the base array " + field.name);
            for (auto at = had; at < editArray->size(); ++at) {
                const auto& entry = (*editArray)[at];
                const auto* pointer = carry ? std::get_if<PointerReference>(&entry.data) : nullptr;
                if (pointer && pointer->kind == PointerKind::external && pointer->index >= 0 &&
                    static_cast<std::size_t>(pointer->index) < edit->imports.size()) {
                    const auto& reference = edit->imports[static_cast<std::size_t>(pointer->index)];
                    if (!carry(reference)) {
                        totals.dropped.push_back({index, reference});
                        continue;
                    }
                }
                auto carried = mapper.value(entry);
                // Two edits that append the same thing mean it once. What one edit
                // lists twice on its own is left as its author wrote it.
                if (const auto* reference = std::get_if<PointerReference>(&carried.data);
                    reference && reference->kind != PointerKind::null) {
                    const std::pair key{reference->kind, reference->index};
                    if (const auto earlier = appended.find(field.name);
                        earlier != appended.end() && earlier->second.contains(key)) {
                        ++totals.repeated;
                        continue;
                    }
                    mine[field.name].insert(key);
                }
                target->push_back(std::move(carried));
                ++totals.arrayEntries;
            }
        }
        for (auto& [name, references] : mine) appended[name].merge(references);

        // An instance an edit added but never linked from the root still has to
        // exist, or the asset it belongs to goes missing.
        for (const auto& instance : edit->instances) {
            // Internal instances are only carried when something the edit added
            // points at them, which the array pass above has already done.
            if (!instance.exported) continue;
            bool known{};
            for (const auto& existing : result.instances)
                if (existing.exported && existing.instanceGuid == instance.instanceGuid) { known = true; break; }
            if (known) continue;
            static_cast<void>(mapper.instance(
                static_cast<std::size_t>(&instance - edit->instances.data())));
        }
        totals.instances += mapper.instances;
        totals.renumbered += mapper.renumbered;
    }
    detail::sort_instances(result);
    if (summary) *summary = totals;
    return result;
}

std::size_t drop_root_references(Document& document, const CarryImport& keep) {
    auto* root = const_cast<InstanceRecord*>(document.root());
    if (!keep || !root || !root->object) return 0;
    std::size_t dropped{};
    for (auto& field : root->object->fields) {
        auto* array = std::get_if<Value::Array>(&field.value.data);
        if (!array) continue;
        dropped += std::erase_if(*array, [&](const Value& entry) {
            const auto* pointer = std::get_if<PointerReference>(&entry.data);
            return pointer && pointer->kind == PointerKind::external && pointer->index >= 0 &&
                   static_cast<std::size_t>(pointer->index) < document.imports.size() &&
                   !keep(document.imports[static_cast<std::size_t>(pointer->index)]);
        });
    }
    // A boxed value is kept as the bytes it was read as, import numbers and
    // all, so a document with one keeps its imports where they are.
    if (!dropped || !document.boxedValues.empty()) return dropped;

    std::vector<bool> used(document.imports.size());
    for (auto& instance : document.instances)
        if (instance.object)
            for (auto& field : instance.object->fields)
                each_pointer(field.value, [&](const PointerReference& pointer) {
                    if (pointer.kind == PointerKind::external && pointer.index >= 0 &&
                        static_cast<std::size_t>(pointer.index) < used.size())
                        used[static_cast<std::size_t>(pointer.index)] = true;
                });
    std::vector<std::int32_t> moveTo(used.size(), -1);
    std::vector<ImportReference> kept;
    for (std::size_t index = 0; index < used.size(); ++index) {
        if (!used[index]) continue;
        moveTo[index] = static_cast<std::int32_t>(kept.size());
        kept.push_back(document.imports[index]);
    }
    if (kept.size() == document.imports.size()) return dropped;
    for (auto& instance : document.instances)
        if (instance.object)
            for (auto& field : instance.object->fields)
                each_pointer(field.value, [&](PointerReference& pointer) {
                    if (pointer.kind == PointerKind::external && pointer.index >= 0 &&
                        static_cast<std::size_t>(pointer.index) < moveTo.size())
                        pointer.index = moveTo[static_cast<std::size_t>(pointer.index)];
                });
    document.imports = std::move(kept);
    return dropped;
}

} // namespace dingosdk::frostbite::ebx
