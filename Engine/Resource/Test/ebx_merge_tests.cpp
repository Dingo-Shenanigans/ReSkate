// Standalone validation for the single-edit in-place merge change.
#include "Engine/Resource/ebx_merge.h"

#include <cstdio>
#include <cstring>

using namespace dingosdk::frostbite::ebx;

namespace {

Document make_document(double pose) {
    Document doc;
    doc.rootType = "SkeletonAsset";
    TypeDescriptor type{};
    type.name = "SkeletonAsset";
    doc.types.push_back(type);

    auto root = std::make_shared<Object>();
    root->descriptor = 0;

    // A root array of one struct holding an editable scalar.
    auto entry = std::make_shared<Object>();
    entry->descriptor = 0;
    FieldValue x; x.descriptor = 0; x.name = "x"; x.value.data = pose;
    entry->fields.push_back(x);

    Value v; v.data = std::shared_ptr<Object>(entry);
    Value::Array arr; arr.push_back(v);
    Value poses; poses.data = arr;
    FieldValue f; f.descriptor = 0; f.name = "Poses"; f.value = poses;
    root->fields.push_back(f);

    // A scalar field changed in place by the edit.
    FieldValue s; s.descriptor = 0; s.name = "Scale"; s.value.data = pose;
    root->fields.push_back(s);

    InstanceRecord rec;
    rec.exported = true;
    std::memset(rec.instanceGuid.bytes.data(), 0x11, 16);
    rec.object = root;
    doc.instances.push_back(rec);
    return doc;
}

double root_scalar(const Document& doc, std::string_view field) {
    for (const auto& f : doc.root()->object->fields)
        if (f.name == field) return std::get<double>(f.value.data);
    return -1.0;
}

double root_array_x(const Document& doc) {
    for (const auto& f : doc.root()->object->fields)
        if (f.name == "Poses") {
            const auto& arr = std::get<Value::Array>(f.value.data);
            const auto& obj = std::get<std::shared_ptr<Object>>(arr[0].data);
            return std::get<double>(obj->find("x")->value.data);
        }
    return -1.0;
}

} // namespace

int main() {
    const auto base = make_document(1.0);
    const auto edit = make_document(2.25);

    // One edit alone: in-place changes are honoured.
    {
        MergeSummary summary;
        const Document* edits[] = {&edit};
        const auto merged = merge_documents(base, edits, &summary);
        if (summary.rootReplacements != 1) { std::puts("FAIL: rootReplacements != 1"); return 1; }
        if (root_scalar(merged, "Scale") != 2.25) { std::puts("FAIL: scalar not replaced"); return 1; }
        if (root_array_x(merged) != 2.25) { std::puts("FAIL: array entry not replaced"); return 1; }
    }

    // Two edits: in-place changes stay as the base had them.
    {
        const auto edit2 = make_document(3.5);
        MergeSummary summary;
        const Document* edits[] = {&edit, &edit2};
        const auto merged = merge_documents(base, edits, &summary);
        if (summary.rootReplacements != 0) { std::puts("FAIL: rootReplacements != 0"); return 1; }
        if (root_scalar(merged, "Scale") != 1.0) { std::puts("FAIL: multi-edit scalar changed"); return 1; }
        if (root_array_x(merged) != 1.0) { std::puts("FAIL: multi-edit array changed"); return 1; }
    }

    std::puts("all single/multi edit merge checks passed");
    return 0;
}
