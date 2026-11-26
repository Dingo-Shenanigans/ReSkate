// Boxed values in the game's own documents: read field by field, merged, written and read back.
//
// Every cosmetic's appearance preset keeps its parameters in boxed values (ReSkate Studio,
// docs/ebx.md, "Boxed values"). Until 2026-11-18 the merge did not read what a boxed value
// holds: two mods that each changed a parameter of one preset had the higher mod's copy kept
// whole, and the other mod's parameter was not in the game. This reads the boxed values of the
// game's documents (every one has to read, or the merge would not see changes in it), then takes
// one document with numbers in two boxed values, changes one in each of two copies, merges them,
// writes the result as the launcher does and reads it back:
//   * both changed numbers are in the written document, and every other boxed value is as the
//     game's;
//   * a text in a boxed value, when the document has one, is changed through the writer too.
//
// Argument: <Skate folder>; skipped when the game is not there.
#include "Engine/Resource/ebx_boxed.h"
#include "Engine/Resource/ebx_carry.h"
#include "Engine/Resource/ebx_merge.h"
#include "Engine/Resource/ebx_writer.h"
#include "Engine/Vfs/game_bundles.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
namespace ebx = dingosdk::frostbite::ebx;

namespace {
int failures = 0;
void expect(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

void references_in(const ebx::Value& value, std::vector<ebx::BoxedReference>& found) {
    if (const auto* boxed = std::get_if<ebx::BoxedReference>(&value.data)) {
        if (boxed->encodedType != 0) found.push_back(*boxed);
    } else if (const auto* nested = std::get_if<std::shared_ptr<ebx::Object>>(&value.data)) {
        if (*nested) for (const auto& entry : (*nested)->fields) references_in(entry.value, found);
    } else if (const auto* entries = std::get_if<ebx::Value::Array>(&value.data)) {
        for (const auto& entry : *entries) references_in(entry, found);
    }
}
// Every boxed reference of the document's objects, in the order of the objects and their fields.
std::vector<ebx::BoxedReference> references_of(const ebx::Document& document) {
    std::vector<ebx::BoxedReference> found;
    for (const auto& instance : document.instances)
        if (instance.object)
            for (const auto& entry : instance.object->fields) references_in(entry.value, found);
    return found;
}
// The leaves of every boxed value, as one list.
std::optional<std::vector<ebx::BoxedLeaf>> leaves_of(const ebx::Document& document) {
    std::vector<ebx::BoxedLeaf> all;
    for (const auto& reference : references_of(document)) {
        auto leaves = ebx::read_boxed(document, reference);
        if (!leaves) return std::nullopt;
        all.insert(all.end(), leaves->begin(), leaves->end());
    }
    return all;
}

struct Candidate {
    std::string name;
    ebx::Document document;
    std::uint32_t first{}, second{};   // four-byte numbers in two different boxed values
    std::optional<std::uint32_t> text; // a text slot in a boxed value
};
} // namespace

int main(int argc, char** argv) try {
    if (argc < 2 || !fs::exists(fs::path(argv[1]) / L"Data" / L"Win32" / L"customization.toc")) {
        std::cout << "No game given; skipped.\n";
        return 0;
    }
    const dingosdk::vfs::GameData game(argv[1]);
    const auto toc = game.read_toc("Win32/customization.toc");
    std::size_t documents{}, withBoxed{}, read{}, unread{};
    std::string firstUnread;
    std::optional<Candidate> candidate;
    // Every document of the customization data, where the presets are.
    for (const auto& entry : toc.bundles) {
        const auto bundle = game.read_bundle(toc, entry.name);
        if (!bundle) continue;
        for (std::size_t index = 0; index < bundle->manifest.ebx.size(); ++index) {
            const auto* payload = bundle->payload(fb::AssetKind::ebx, index);
            if (!payload) continue;
            ebx::Document document;
            try {
                document = ebx::read_document(game.read(*payload));
            } catch (const std::exception&) {
                continue;
            }
            ++documents;
            if (document.boxedValues.empty()) continue;
            ++withBoxed;
            std::vector<std::uint32_t> numbers;
            std::optional<std::uint32_t> text;
            for (const auto& reference : references_of(document)) {
                const auto leaves = ebx::read_boxed(document, reference);
                if (!leaves) {
                    if (!unread++) firstUnread = bundle->manifest.ebx[index].name;
                    continue;
                }
                ++read;
                bool number{};
                for (const auto& leaf : *leaves) {
                    if (!number && leaf.kind == ebx::BoxedLeaf::Kind::bytes && leaf.bytes.size() == 4) {
                        numbers.push_back(leaf.offset);
                        number = true;
                    }
                    if (!text && leaf.kind == ebx::BoxedLeaf::Kind::text) text = leaf.offset;
                }
            }
            if (numbers.size() >= 2 && (!candidate || (!candidate->text && text)))
                candidate = Candidate{bundle->manifest.ebx[index].name, std::move(document), numbers[0], numbers[1], text};
        }
    }
    std::cout << documents << " documents, " << withBoxed << " with boxed values, " << read << " boxed values read\n";
    expect(read > 0 && unread == 0, std::to_string(unread) + " boxed value(s) of the game do not read, e.g. in " + firstUnread);
    expect(candidate.has_value(), "a document with numbers in two boxed values is found");
    if (!candidate) return 1;

    // Two mods' copies, each with one number changed, read back from the bytes a mod would ship.
    const auto& base = candidate->document;
    const auto number_at = [](const ebx::Document& document, std::uint32_t offset) {
        std::uint32_t value{};
        std::memcpy(&value, document.payload.data() + offset, sizeof(value));
        return value;
    };
    const auto upperValue = number_at(base, candidate->first) ^ 0x00010000U;
    const auto underValue = number_at(base, candidate->second) ^ 0x00020000U;
    const auto copy_with = [&](std::uint32_t offset, std::uint32_t value, const std::string* text) {
        auto copy = ebx::detail::clone_document(base);
        std::byte bytes[4];
        std::memcpy(bytes, &value, sizeof(bytes));
        ebx::write_boxed_bytes(copy, offset, bytes);
        if (text) ebx::write_boxed_text(copy, *candidate->text, *text);
        return ebx::read_document(ebx::write_document(copy));
    };
    const std::string newText = "reskate merge test";
    const auto upperCopy = copy_with(candidate->first, upperValue, nullptr);
    const auto underCopy = copy_with(candidate->second, underValue, candidate->text ? &newText : nullptr);
    const std::vector<const ebx::Document*> edits{&underCopy, &upperCopy};
    ebx::MergeSummary summary;
    const auto merged = ebx::merge_documents(base, edits, &summary);
    const auto expected = 2U + (candidate->text ? 1U : 0U);
    expect(summary.exact() && summary.values == expected && !summary.contested,
           candidate->name + ": every change is taken (" + std::to_string(summary.values) + " of " + std::to_string(expected) +
           ", " + std::to_string(summary.uncarried.size()) + " uncarried, " + std::to_string(summary.unread.size()) + " unread)");

    // Written as the launcher writes it, and read back as the game would get it.
    const auto written = ebx::read_document(ebx::write_document(merged));
    const auto before = leaves_of(base), after = leaves_of(written);
    expect(before && after && before->size() == after->size(), candidate->name + ": the written document's boxed values read");
    if (before && after && before->size() == after->size()) {
        std::size_t numbers{}, texts{}, others{};
        for (std::size_t index = 0; index < before->size(); ++index) {
            const auto& was = (*before)[index];
            const auto& now = (*after)[index];
            if (was.kind == ebx::BoxedLeaf::Kind::bytes && (was.offset == candidate->first || was.offset == candidate->second)) {
                std::uint32_t value{};
                if (now.bytes.size() == 4) std::memcpy(&value, now.bytes.data(), 4);
                numbers += value == (was.offset == candidate->first ? upperValue : underValue);
            } else if (was.kind == ebx::BoxedLeaf::Kind::text && candidate->text && was.offset == *candidate->text) {
                texts += now.text == newText;
            } else if (was.kind != now.kind || was.bytes != now.bytes || was.text != now.text ||
                       was.pointer.kind != now.pointer.kind) {
                ++others;
            }
        }
        expect(numbers == 2, candidate->name + ": both mods' numbers are in the written document");
        expect(!candidate->text || texts == 1, candidate->name + ": the changed text is in the written document");
        expect(others == 0, candidate->name + ": every other boxed value is as the game's (" + std::to_string(others) + " differ)");
    }
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "boxed values in the game's documents: ok (" << candidate->name << (candidate->text ? ", with a text" : "") << ")\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
