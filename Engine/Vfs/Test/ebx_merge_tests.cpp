// The document merge, on documents made in memory, so it needs no game files.
//
// Several mods each ship their own copy of one of the game's assets. merge_documents puts the
// copies together over the game's: what each added, and what each changed in place. The cases:
//   * values two edits changed in place both arrive, and one both changed is the later edit's;
//   * a list grows at its end wherever it is, and what each edit appended comes across;
//   * an edit that also changed something that cannot be taken (a reference pointed elsewhere, a
//     list that shrank) has none of its values taken, and the summary names it, unless that change
//     is in an exported instance no other edit changed: then the instance is taken whole; or in an
//     entry of a list below the root's own that no other edit changed: then the entry is;
//   * an exported instance is found by its guid wherever the edit keeps it, and internal
//     instances that do not line up are reported as not looked at;
//   * a reference two edits both append is listed once, and one edit's own repeat is left alone;
//   * drop_root_references takes entries out of the root's lists with the imports they used;
//   * a boxed value read through its type merges like any other value: a change inside it comes
//     across, a list inside it grows at its end, an added instance brings its boxes along, and a
//     box written afresh is marked so; one read only as bytes is reported as not looked at.
#include "Engine/Resource/ebx_carry.h"
#include "Engine/Resource/ebx_merge.h"

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace ebx = dingosdk::frostbite::ebx;
using dingosdk::frostbite::Guid;

namespace {
int failures = 0;
void expect(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

Guid guid(std::uint8_t seed) {
    Guid value;
    value.bytes.fill(static_cast<std::byte>(seed));
    return value;
}

template <typename Type> ebx::Value value_of(Type data) {
    ebx::Value value;
    value.data = std::move(data);
    return value;
}
ebx::Value number(std::int64_t value) { return value_of(value); }
ebx::Value real(double value) { return value_of(value); }
ebx::Value text(std::string value) { return value_of(std::move(value)); }
// A reference to another document, by the place of its import, and one to an instance of this one.
ebx::Value outside(std::int32_t import) { return value_of(ebx::PointerReference{ebx::PointerKind::external, import}); }
ebx::Value inside(std::int32_t instance) { return value_of(ebx::PointerReference{ebx::PointerKind::internal, instance}); }
ebx::Value list(ebx::Value::Array entries) { return value_of(std::move(entries)); }

std::shared_ptr<ebx::Object> object(std::int32_t type, std::vector<std::pair<std::string, ebx::Value>> fields) {
    auto result = std::make_shared<ebx::Object>();
    result->descriptor = type;
    for (auto& [name, value] : fields) result->fields.push_back({result->fields.size(), std::move(name), std::move(value)});
    return result;
}
ebx::InstanceRecord instance(std::int32_t type, std::shared_ptr<ebx::Object> fields, const Guid* exported = nullptr) {
    ebx::InstanceRecord record;
    record.descriptor = type;
    record.exported = exported != nullptr;
    if (exported) record.instanceGuid = *exported;
    record.object = std::move(fields);
    return record;
}

constexpr std::int32_t list_type = 0, entry_type = 1, detail_type = 2;
const Guid entry_guid = guid(0xA1);

// The game's asset: a root with plain values, a list of two other documents and a nested
// structure; an exported instance the root points at; and an internal one that one points at.
ebx::Document game() {
    ebx::Document document;
    document.fileGuid = guid(1);
    document.rootType = "ListAsset";
    document.types.resize(3);
    document.types[list_type].name = "ListAsset";
    document.types[entry_type].name = "Entry";
    document.types[detail_type].name = "Detail";
    document.imports = {{guid(0x10), guid(0xC0)}, {guid(0x11), guid(0xC0)}};
    document.instances.push_back(instance(list_type, object(list_type, {
        {"Volume", number(5)}, {"Gain", real(1.0)}, {"Title", text("game")},
        {"Items", list({outside(0), outside(1)})}, {"First", inside(1)},
        {"Tuning", value_of(object(detail_type, {{"Weights", list({number(1), number(2)})}, {"Scale", real(2.0)}}))}})));
    document.instances.push_back(instance(entry_type, object(entry_type, {{"Cost", number(10)}, {"Detail", inside(2)}}), &entry_guid));
    document.instances.push_back(instance(detail_type, object(detail_type, {{"Depth", number(3)}})));
    return document;
}
ebx::Document copy(const ebx::Document& document) { return ebx::detail::clone_document(document); }

ebx::Value& field(ebx::Object& from, std::string_view name) {
    for (auto& entry : from.fields)
        if (entry.name == name) return entry.value;
    throw std::runtime_error("no field " + std::string(name));
}
ebx::Value& field(ebx::Document& document, std::size_t at, std::string_view name) { return field(*document.instances.at(at).object, name); }
template <typename Type> const Type& read(const ebx::Document& document, std::size_t at, std::string_view name) {
    return std::get<Type>(field(*document.instances.at(at).object, name).data);
}
ebx::Object& tuning(ebx::Document& document) { return *std::get<std::shared_ptr<ebx::Object>>(field(document, 0, "Tuning").data); }
ebx::Value::Array& items(ebx::Document& document) { return std::get<ebx::Value::Array>(field(document, 0, "Items").data); }
// The documents a list names, in order.
std::vector<Guid> named(const ebx::Document& document) {
    std::vector<Guid> result;
    for (const auto& entry : std::get<ebx::Value::Array>(field(*document.instances.front().object, "Items").data))
        result.push_back(document.imports.at(static_cast<std::size_t>(std::get<ebx::PointerReference>(entry.data).index)).fileGuid);
    return result;
}
void append(ebx::Document& document, const Guid& target) {
    document.imports.push_back({target, guid(0xC0)});
    items(document).push_back(outside(static_cast<std::int32_t>(document.imports.size() - 1)));
}
// Where the exported instance with this guid is.
std::size_t find(const ebx::Document& document, const Guid& wanted) {
    for (std::size_t index = 0; index < document.instances.size(); ++index)
        if (document.instances[index].exported && document.instances[index].instanceGuid == wanted) return index;
    throw std::runtime_error("no such exported instance");
}

ebx::Document merge(const ebx::Document& base, std::initializer_list<const ebx::Document*> edits, ebx::MergeSummary& summary) {
    const std::vector<const ebx::Document*> documents(edits);
    return ebx::merge_documents(base, documents, &summary);
}

void values_from_every_edit() {
    const auto base = game();
    auto first = copy(base), second = copy(base);
    field(first, 0, "Volume") = number(9);
    field(first, 0, "Title") = text("first");
    field(second, 0, "Gain") = real(0.5);
    field(tuning(second), "Scale") = real(4.0);
    std::get<ebx::Value::Array>(field(tuning(second), "Weights").data)[1] = number(7);
    field(second, 1, "Cost") = number(20);
    field(second, 2, "Depth") = number(8);
    ebx::MergeSummary summary;
    auto merged = merge(base, {&first, &second}, summary);
    expect(read<std::int64_t>(merged, 0, "Volume") == 9 && read<std::string>(merged, 0, "Title") == "first",
           "values: the first edit's values are in the result");
    expect(read<double>(merged, 0, "Gain") == 0.5 && std::get<double>(field(tuning(merged), "Scale").data) == 4.0 &&
           std::get<std::int64_t>(std::get<ebx::Value::Array>(field(tuning(merged), "Weights").data)[1].data) == 7,
           "values: the second edit's values are in the result, the nested ones too");
    expect(read<std::int64_t>(merged, 1, "Cost") == 20 && read<std::int64_t>(merged, 2, "Depth") == 8,
           "values: values in an exported and in an internal instance are in the result");
    expect(summary.values == 7 && summary.contested == 0 && summary.exact() && !summary.instances && !summary.arrayEntries,
           "values: seven values taken, none disagreed, nothing added (" + std::to_string(summary.values) + " taken)");
    expect(named(merged) == named(base) && merged.instances.size() == base.instances.size(), "values: nothing else moved");
}

void one_value_from_two_edits(bool same) {
    const std::string label = same ? "same value twice" : "contested value";
    const auto base = game();
    auto lower = copy(base), higher = copy(base);
    field(lower, 0, "Volume") = number(9);
    field(higher, 0, "Volume") = number(same ? 9 : 11);
    ebx::MergeSummary summary;
    const auto merged = merge(base, {&lower, &higher}, summary);
    expect(read<std::int64_t>(merged, 0, "Volume") == (same ? 9 : 11), label + ": the later edit's value is the one kept");
    expect(summary.values == 2 && summary.contested == (same ? 0u : 1u) && summary.exact(),
           label + ": " + (same ? "no" : "one") + " disagreement is counted");
}

// What cannot be taken: the edit that made such a change keeps all its values out, so the result
// never has half of what a mod did to the game's own content.
void a_change_that_cannot_be_taken(int kind) {
    static constexpr std::array labels{"retargeted reference", "shorter root list", "shorter nested list", "instance gone"};
    const std::string label = labels[static_cast<std::size_t>(kind)];
    const auto base = game();
    auto reshaped = copy(base), plain = copy(base);
    field(reshaped, 0, "Volume") = number(9);
    if (kind == 0) items(reshaped)[0] = outside(1);
    else if (kind == 1) items(reshaped).pop_back();
    else if (kind == 2) std::get<ebx::Value::Array>(field(tuning(reshaped), "Weights").data).pop_back();
    else {
        // The exported instance is not in the edit: the root points at the internal one instead.
        reshaped.instances.erase(reshaped.instances.begin() + 1);
        field(reshaped, 0, "First") = inside(1);
    }
    field(plain, 0, "Title") = text("plain");
    ebx::MergeSummary summary;
    const auto merged = merge(base, {&reshaped, &plain}, summary);
    expect(read<std::int64_t>(merged, 0, "Volume") == 5, label + ": none of that edit's values are taken");
    expect(read<std::string>(merged, 0, "Title") == "plain", label + ": the other edit's value is");
    expect(summary.uncarried == std::vector<std::size_t>{0} && !summary.exact() && summary.values == 1,
           label + ": the summary names the edit");
    expect(named(merged) == named(base), label + ": the game's list is as it was");
}

// A change that cannot be combined, made to an exported instance no other edit changed, is taken
// with the whole instance: nothing of another edit's is lost by it. When another edit changed that
// instance too, the edit's changes are all kept out, as above.
void an_instance_only_one_edit_reshaped(bool alsoChangedElsewhere) {
    const std::string label = alsoChangedElsewhere ? "reshaped and contested" : "reshaped alone";
    const auto base = game();
    auto reshaped = copy(base), plain = copy(base);
    field(reshaped, 0, "Volume") = number(9);
    // The game's entry now points at an instance the edit added, not at the game's detail.
    const auto added = guid(0xD0);
    reshaped.instances.push_back(instance(detail_type, object(detail_type, {{"Depth", number(6)}}), &added));
    field(reshaped, 1, "Detail") = inside(static_cast<std::int32_t>(reshaped.instances.size() - 1));
    field(plain, 0, "Title") = text("plain");
    if (alsoChangedElsewhere) field(plain, 1, "Cost") = number(11);
    ebx::MergeSummary summary;
    auto merged = merge(base, {&reshaped, &plain}, summary);
    const auto entry = find(merged, entry_guid);
    expect(read<std::string>(merged, 0, "Title") == "plain", label + ": the other edit's value is in");
    if (!alsoChangedElsewhere) {
        const auto detail = read<ebx::PointerReference>(merged, entry, "Detail");
        expect(detail.kind == ebx::PointerKind::internal && static_cast<std::size_t>(detail.index) == find(merged, added),
               label + ": the entry is the edit's, pointing at the instance it added");
        expect(read<std::int64_t>(merged, find(merged, added), "Depth") == 6, label + ": the added instance came across");
        expect(read<std::int64_t>(merged, 0, "Volume") == 9, label + ": the edit's other values are taken");
        expect(summary.taken == 1 && summary.exact() && summary.changed_in_place(), label + ": one instance taken whole, nothing left out");
    } else {
        expect(read<std::int64_t>(merged, 0, "Volume") == 5 && read<std::int64_t>(merged, entry, "Cost") == 11,
               label + ": none of the reshaping edit's values are taken, the other edit's are");
        const auto detail = read<ebx::PointerReference>(merged, entry, "Detail");
        expect(detail.kind == ebx::PointerKind::internal && merged.instances.at(static_cast<std::size_t>(detail.index)).exported == false,
               label + ": the entry still points at the game's detail");
        expect(summary.taken == 0 && summary.uncarried == std::vector<std::size_t>{0}, label + ": the summary names the edit");
    }
}

// A list below the root's own grows at its end as one on the root does: an authored recipe keeps its
// equips in a list inside a struct on the root, and every mod that adds a slot to the recipe's
// template appends one there.
void a_nested_list_grows() {
    const auto base = game();
    auto first = copy(base), second = copy(base), third = copy(base);
    auto weights = [](ebx::Document& document) -> ebx::Value::Array& { return std::get<ebx::Value::Array>(field(tuning(document), "Weights").data); };
    weights(first).push_back(number(3));
    weights(second).push_back(number(4));
    weights(second).push_back(number(3));   // the first edit's as well, after its own
    field(third, 0, "Volume") = number(9);
    ebx::MergeSummary summary;
    auto merged = merge(base, {&first, &second, &third}, summary);
    std::vector<std::int64_t> got;
    for (const auto& weight : weights(merged)) got.push_back(std::get<std::int64_t>(weight.data));
    expect((got == std::vector<std::int64_t>{1, 2, 3, 4}), "nested list: what each edit appended is in, a repeat once (" +
           std::to_string(got.size()) + " entries)");
    expect(read<std::int64_t>(merged, 0, "Volume") == 9 && summary.exact() && summary.arrayEntries == 2 && summary.repeated == 1,
           "nested list: the other edit's value is in and nothing is left out");
}

// An entry of a list below the root's own that one edit changed in a way that cannot be combined (a
// reference pointed elsewhere) comes across whole, beside what another edit appended to the same list,
// in either order: a mod that moves buttons between the skater page's tile lists does that. When the
// other edit pointed the same entry elsewhere too, the later of the two is kept out.
void a_list_entry_only_one_edit_changed(bool reversed, bool contested) {
    const std::string label = std::string("entry taken whole") + (reversed ? ", other order" : "") + (contested ? ", contested" : "");
    auto base = game();
    base.instances[1].object->fields.push_back({2, "Links", list({outside(0), outside(1)})});
    auto links = [](ebx::Document& document) -> ebx::Value::Array& {
        return std::get<ebx::Value::Array>(field(document, find(document, entry_guid), "Links").data);
    };
    auto moved = copy(base), grown = copy(base);
    moved.imports.push_back({guid(0x30), guid(0xC0)});
    links(moved)[0] = outside(2);
    field(moved, 0, "Volume") = number(9);
    grown.imports.push_back({guid(0x31), guid(0xC0)});
    links(grown).push_back(outside(2));
    if (contested) {
        grown.imports.push_back({guid(0x32), guid(0xC0)});
        links(grown)[0] = outside(3);
    }
    ebx::MergeSummary summary;
    auto merged = reversed ? merge(base, {&grown, &moved}, summary) : merge(base, {&moved, &grown}, summary);
    std::vector<Guid> got;
    for (const auto& link : links(merged))
        got.push_back(merged.imports.at(static_cast<std::size_t>(std::get<ebx::PointerReference>(link.data).index)).fileGuid);
    if (!contested) {
        expect((got == std::vector<Guid>{guid(0x30), guid(0x11), guid(0x31)}), label + ": the moved entry and the appended one are both in");
        expect(read<std::int64_t>(merged, 0, "Volume") == 9 && summary.entries == 1 && summary.taken == 0 && summary.exact() &&
               summary.arrayEntries == 1, label + ": the moving edit's other value is in and nothing is left out");
    } else {
        const auto expected = reversed ? std::vector<Guid>{guid(0x32), guid(0x11), guid(0x31)} : std::vector<Guid>{guid(0x30), guid(0x11)};
        expect(got == expected, label + ": the earlier edit's entry is the one kept, with that edit's own changes");
        expect(read<std::int64_t>(merged, 0, "Volume") == (reversed ? 5 : 9) && summary.uncarried == std::vector<std::size_t>{1},
               label + ": the later edit's changes are all kept out, and the summary names it");
    }
}

// The instance an earlier edit took whole no longer looks like the base's in the result. A later
// edit that left it alone still has its own changes taken.
void a_later_edit_beside_an_instance_taken_whole() {
    const auto base = game();
    auto reshaped = copy(base), later = copy(base);
    field(reshaped, 1, "Detail") = value_of(ebx::PointerReference{});
    field(*reshaped.instances[1].object, "Cost") = number(12);
    reshaped.instances[1].object->fields.push_back({2, "Extra", number(1)});   // a field the base's lacks
    field(later, 0, "Volume") = number(7);
    ebx::MergeSummary summary;
    auto merged = merge(base, {&reshaped, &later}, summary);
    const auto entry = find(merged, entry_guid);
    expect(read<std::int64_t>(merged, entry, "Cost") == 12 && read<std::int64_t>(merged, 0, "Volume") == 7,
           "beside a taken instance: both edits' changes are in");
    expect(summary.taken == 1 && summary.exact(), "beside a taken instance: nothing left out");
}

// An edit that added an exported instance has moved the game's along; they are still found.
void instances_found_by_guid() {
    const auto base = game();
    auto edit = copy(base);
    const auto added = guid(0x05);   // sorts ahead of the game's
    edit.instances.insert(edit.instances.begin() + 1,
                          instance(entry_type, object(entry_type, {{"Cost", number(1)}, {"Detail", value_of(ebx::PointerReference{})}}), &added));
    field(edit, 0, "First") = inside(2);
    field(edit, 2, "Detail") = inside(3);
    field(edit, 2, "Cost") = number(30);
    ebx::MergeSummary summary;
    const auto merged = merge(base, {&edit}, summary);
    expect(summary.uncarried.empty() && summary.unread.empty() && summary.values == 1 && summary.instances == 1,
           "by guid: one value taken and one instance added, nothing left behind");
    expect(read<std::int64_t>(merged, find(merged, entry_guid), "Cost") == 30, "by guid: the game's instance has the edit's value");
    expect(read<std::int64_t>(merged, find(merged, added), "Cost") == 1, "by guid: the added instance came across");
    expect(find(merged, added) < find(merged, entry_guid), "by guid: exported instances are in guid order");
    expect(static_cast<std::size_t>(read<ebx::PointerReference>(merged, 0, "First").index) == find(merged, entry_guid),
           "by guid: the root still points at the game's instance");
}

// An internal instance has no guid. With one more of them in the edit the places say nothing,
// so they are not compared, and the summary says the edit was not looked at in full.
void internal_instances_that_do_not_line_up() {
    const auto base = game();
    auto edit = copy(base);
    edit.instances.push_back(instance(detail_type, object(detail_type, {{"Depth", number(4)}})));
    field(edit, 0, "Volume") = number(9);
    ebx::MergeSummary summary;
    const auto merged = merge(base, {&edit}, summary);
    expect(read<std::int64_t>(merged, 0, "Volume") == 9, "internal: the value on the root is taken");
    expect(summary.uncarried.empty() && summary.unread == std::vector<std::size_t>{0} && !summary.exact(),
           "internal: the edit is reported as not looked at in full");
}

void appended_references() {
    const auto base = game();
    auto first = copy(base), second = copy(base), third = copy(base);
    append(first, guid(0x20));
    append(first, guid(0x21));
    append(second, guid(0x21));   // the first edit's as well
    append(second, guid(0x22));
    append(third, guid(0x23));    // twice in one edit: its author's doing
    append(third, guid(0x23));
    ebx::MergeSummary summary;
    const auto merged = merge(base, {&first, &second, &third}, summary);
    const std::vector<Guid> expected{guid(0x10), guid(0x11), guid(0x20), guid(0x21), guid(0x22), guid(0x23), guid(0x23)};
    expect(named(merged) == expected, "appended: each reference once, in the order of the edits, one edit's own repeat kept (" +
           std::to_string(named(merged).size()) + " entries)");
    expect(summary.arrayEntries == 5 && summary.repeated == 1 && summary.exact(), "appended: five entries taken, one repeat left out");
}

void dropped_references() {
    auto document = game();
    append(document, guid(0x20));
    append(document, guid(0x21));
    const auto gone = ebx::drop_root_references(document, [](const ebx::ImportReference& reference) {
        return reference.fileGuid != guid(0x11) && reference.fileGuid != guid(0x20); });
    expect(gone == 2, "dropped: two entries went (" + std::to_string(gone) + ")");
    expect((named(document) == std::vector<Guid>{guid(0x10), guid(0x21)}), "dropped: the others are left, in order");
    expect(document.imports.size() == 2, "dropped: the imports nothing uses went with them (" +
           std::to_string(document.imports.size()) + " left)");

    // A boxed value is kept as bytes with its import numbers in them, so the imports stay put.
    auto boxed = game();
    boxed.boxedValues.emplace_back();
    expect(ebx::drop_root_references(boxed, [](const ebx::ImportReference& reference) { return reference.fileGuid != guid(0x10); }) == 1 &&
           boxed.imports.size() == 2 && named(boxed) == std::vector<Guid>{guid(0x11)},
           "dropped: a document with a boxed value keeps its imports where they are");
    auto untouched = game();
    expect(ebx::drop_root_references(untouched, [](const ebx::ImportReference&) { return true; }) == 0 &&
           untouched.imports.size() == 2, "dropped: nothing goes when every entry is kept");
}

// Boxed values, read through their type: a UI panel boxed on the root. Its type is one the
// document only names in its EFIX list (no signature), as a UI type defined in another asset is.
// Slot 0 is never a box's type: a box whose code is 0 is an empty one.
const Guid panel_type = guid(0xB0);

ebx::Value box(std::shared_ptr<ebx::Object> content, std::uint32_t slot) {
    auto boxed = std::make_shared<ebx::BoxedValue>();
    boxed->kind = ebx::BoxedValue::Kind::structure;
    boxed->type = panel_type;
    boxed->descriptor = detail_type;
    boxed->value = value_of(std::move(content));
    ebx::BoxedReference reference;
    reference.encodedType = slot << 2;
    reference.dataOffset = 0;
    reference.content = std::move(boxed);
    return value_of(std::move(reference));
}
std::shared_ptr<ebx::Object> panel(ebx::Value::Array rows, double scale, std::int32_t link) {
    return object(detail_type, {{"Rows", list(std::move(rows))}, {"Scale", real(scale)}, {"Link", outside(link)}});
}
// Its EFIX list: the entry type it describes (with a signature), then two it only names.
ebx::Document boxed_game() {
    auto document = game();
    document.types[entry_type].guid = guid(0xE1);
    document.types[entry_type].signature = 7;
    document.fixupTypeGuids = {guid(0xE1), guid(0xAF), panel_type};
    document.fixupTypeSignatures = {7};
    document.instances[0].object->fields.push_back({document.instances[0].object->fields.size(), "Panel", box(panel({number(1), number(2)}, 1.0, 0), 2)});
    return document;
}
ebx::BoxedReference& panel_box(ebx::Document& document, std::size_t at = 0, std::string_view name = "Panel") {
    return std::get<ebx::BoxedReference>(field(document, at, name).data);
}
ebx::Object& panel_of(ebx::Document& document, std::size_t at = 0, std::string_view name = "Panel") {
    return *std::get<std::shared_ptr<ebx::Object>>(panel_box(document, at, name).content->value.data);
}
std::vector<std::int64_t> rows(ebx::Object& content) {
    std::vector<std::int64_t> result;
    for (const auto& row : std::get<ebx::Value::Array>(field(content, "Rows").data)) result.push_back(std::get<std::int64_t>(row.data));
    return result;
}

// A value changed inside a box comes across like any other, a list inside one grows at its end
// with what each edit appended (once, when two append the same), and the box is then written afresh.
void boxed_values_change_and_grow() {
    auto base = boxed_game();
    auto first = copy(base), second = copy(base), third = copy(base);
    field(panel_of(first), "Scale") = real(2.0);
    std::get<ebx::Value::Array>(field(panel_of(second), "Rows").data).push_back(number(3));
    std::get<ebx::Value::Array>(field(panel_of(third), "Rows").data).push_back(number(3));
    std::get<ebx::Value::Array>(field(panel_of(third), "Rows").data).push_back(number(4));
    ebx::MergeSummary summary;
    auto merged = merge(base, {&first, &second, &third}, summary);
    expect(std::get<double>(field(panel_of(merged), "Scale").data) == 2.0, "boxed: a value changed inside a box is taken");
    expect((rows(panel_of(merged)) == std::vector<std::int64_t>{1, 2, 3, 4}), "boxed: rows appended inside a box come across, a repeat once");
    expect(panel_box(merged).rewritten, "boxed: the changed box is written afresh");
    expect(summary.values == 1 && summary.arrayEntries == 2 && summary.repeated == 1 && summary.exact(),
           "boxed: one value and two entries taken, one repeat left out (" + std::to_string(summary.arrayEntries) + " entries)");
    expect(rows(panel_of(base)).size() == 2 && !panel_box(base).rewritten, "boxed: the game's copy is untouched");
    auto quiet = copy(base);
    const auto unchanged = merge(base, {&quiet}, summary);
    expect(!std::get<ebx::BoxedReference>(field(*unchanged.instances[0].object, "Panel").data).rewritten,
           "boxed: a box nobody changed keeps its bytes");
}

// An instance an edit added comes across with its box: the box's type found again by guid in the
// result's EFIX list, and what the box points at numbered as the result numbers it.
void boxed_value_carried() {
    auto base = boxed_game();
    auto edit = copy(base);
    edit.fixupTypeGuids = {guid(0xE1), guid(0xAF), guid(0xB1), panel_type};   // the edit numbers the panel type 3
    edit.imports.push_back({guid(0x30), guid(0xC0)});
    const auto added = guid(0x40);
    auto content = object(entry_type, {{"Cost", number(1)}, {"Detail", value_of(ebx::PointerReference{})},
                                        {"Look", box(panel({number(5)}, 3.0, 2), 3)}});
    edit.instances.push_back(instance(entry_type, std::move(content), &added));
    ebx::MergeSummary summary;
    auto merged = merge(base, {&edit}, summary);
    const auto at = find(merged, added);
    auto& carried = panel_box(merged, at, "Look");
    expect(summary.instances == 1 && summary.uncarried.empty(), "carried: the added instance came across");
    expect(carried.rewritten && carried.encodedType == (2U << 2), "carried: its box is the result's slot for the panel type (code " +
           std::to_string(carried.encodedType) + (carried.rewritten ? ", rewritten)" : ")"));
    expect((rows(panel_of(merged, at, "Look")) == std::vector<std::int64_t>{5}), "carried: the box's content came across");
    const auto link = std::get<ebx::PointerReference>(field(panel_of(merged, at, "Look"), "Link").data);
    expect(link.kind == ebx::PointerKind::external && merged.imports.at(static_cast<std::size_t>(link.index)).fileGuid == guid(0x30),
           "carried: what the box points at is numbered as the result numbers it");
    expect(merged.fixupTypeGuids.size() == 3 && panel_box(merged).encodedType == (2U << 2),
           "carried: the game's own box still names the panel type");

    // A carried instance of a type the base does not describe would move the named types: refused.
    auto undescribed = boxed_game();
    undescribed.fixupTypeGuids = {guid(0xAF), panel_type};
    undescribed.fixupTypeSignatures.clear();
    std::get<ebx::BoxedReference>(field(undescribed, 0, "Panel").data).encodedType = 1U << 2;
    auto adds = copy(undescribed);
    adds.instances.push_back(instance(entry_type, object(entry_type, {{"Cost", number(1)}}), &added));
    bool refused = false;
    try {
        static_cast<void>(merge(undescribed, {&adds}, summary));
    } catch (const std::exception&) {
        refused = true;
    }
    expect(refused, "carried: a type that would move the named ones is refused");
}

// A reference pointed elsewhere inside a box cannot be taken, as outside one; a box that could not
// be read on either side is only reported as not looked at.
void boxed_values_that_cannot_be_taken() {
    auto base = boxed_game();
    auto edit = copy(base);
    field(panel_of(edit), "Link") = outside(1);
    field(panel_of(edit), "Scale") = real(9.0);
    ebx::MergeSummary summary;
    auto merged = merge(base, {&edit}, summary);
    expect(std::get<double>(field(panel_of(merged), "Scale").data) == 1.0 && summary.uncarried == std::vector<std::size_t>{0},
           "boxed: a retargeted reference inside a box keeps all the edit's values out");

    auto unread = game();
    ebx::BoxedReference bytes;
    bytes.encodedType = 4;
    bytes.dataOffset = 16;
    unread.instances[0].object->fields.push_back({unread.instances[0].object->fields.size(), "Raw", value_of(bytes)});
    auto other = copy(unread);
    std::get<ebx::BoxedReference>(field(other, 0, "Raw").data).dataOffset = 32;
    const auto kept = merge(unread, {&other}, summary);
    expect(summary.unread == std::vector<std::size_t>{0} && summary.uncarried.empty(), "boxed: a box read as bytes only is not looked at");
}
} // namespace

int main() try {
    values_from_every_edit();
    one_value_from_two_edits(false);
    one_value_from_two_edits(true);
    for (int kind = 0; kind < 4; ++kind) a_change_that_cannot_be_taken(kind);
    an_instance_only_one_edit_reshaped(false);
    an_instance_only_one_edit_reshaped(true);
    a_later_edit_beside_an_instance_taken_whole();
    for (const bool reversed : {false, true})
        for (const bool contested : {false, true}) a_list_entry_only_one_edit_changed(reversed, contested);
    a_nested_list_grows();
    instances_found_by_guid();
    internal_instances_that_do_not_line_up();
    appended_references();
    dropped_references();
    boxed_values_change_and_grow();
    boxed_value_carried();
    boxed_values_that_cannot_be_taken();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ebx merge: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
