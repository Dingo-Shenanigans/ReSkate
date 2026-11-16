#include "mod_merge_internal.h"

#include <array>
#include <cstring>
#include <optional>
#include <set>
#include <stdexcept>

namespace dingosdk::mods::detail {
namespace {
using Node = native_db::Node;
} // namespace

// The merged patch's layout: the installed game's, with each mod's own superbundles listed and
// put in their install chunks, and the patch's archives declared.
void merge_layout(native_db::Node& layout, const std::vector<const Mod*>& mods, const std::vector<const Mod*>& layoutSources,
                  const ArchiveUse& used, MergeReport& report) {
    auto* list = layout.field("superBundles");
    if (!list || list->type != 1) throw std::runtime_error("layout.toc has no superBundles list");
    std::set<std::string, std::less<>> known;
    for (const auto& row : list->children) {
        const auto* name = row.field("name");
        if (name && name->type == 7) known.emplace(ascii_lower(name->text));
    }
    // A mod's own layout.toc. A damaged one is that mod's problem: the merge runs again without it.
    const auto mod_layout = [&](const Mod& mod) -> std::optional<vfs::Layout> {
        try {
            return vfs::read_layout(mod.directory / L"layout.toc");
        } catch (const std::exception& error) {
            report.problems[mod.name].push_back(std::string("layout.toc could not be read: ") + error.what());
            return std::nullopt;
        }
    };
    for (const auto* mod : mods) {
        const auto ownLayout = mod_layout(*mod);
        if (!ownLayout) continue;
        const auto& own = ownLayout->root;
        const auto* ownManifest = own.field("installManifest");
        const auto* ownChunks = ownManifest ? ownManifest->field("installChunks") : nullptr;
        if (!ownChunks) continue;
        for (const auto& chunk : ownChunks->children) {
            const auto* chunkName = chunk.field("name");
            const auto* held = chunk.field("superbundles");
            if (!chunkName || !held) continue;
            for (const auto& entry : held->children)
                if (entry.type == 7) report.superbundle_chunks.emplace_back(entry.text, chunkName->text);
        }
    }
    for (const auto* mod : layoutSources) {
        const auto otherLayout = mod_layout(*mod);
        if (!otherLayout) continue;
        const auto& other = otherLayout->root;
        const auto* otherList = other.field("superBundles");
        if (!otherList) continue;
        for (const auto& row : otherList->children) {
            const auto* name = row.field("name");
            if (!name || name->type != 7 || !known.emplace(ascii_lower(name->text)).second) continue;
            // The list is ordered by name and the engine searches it that
            // way, so an entry appended at the end is never found.
            const auto key = ascii_lower(name->text);
            auto at = list->children.begin();
            for (; at != list->children.end(); ++at) {
                const auto* existing = at->field("name");
                if (existing && existing->type == 7 && ascii_lower(existing->text) > key) break;
            }
            list->children.insert(at, native_db::make_named_record("name", name->text));
            report.notes.push_back("Merged layout adds " + name->text + " from " + mod->name);
        }

        // The superbundle list is only half of it: each install chunk in the
        // manifest names the superbundles it holds, and that is what tells
        // the engine where a bundle's data lives. Without it the level is
        // requested and then waits for content that is never located.
        auto* chunks = layout.field("installManifest")
            ? layout.field("installManifest")->field("installChunks") : nullptr;
        const auto* otherManifest = other.field("installManifest");
        const auto* otherChunks = otherManifest ? otherManifest->field("installChunks") : nullptr;
        if (!chunks || !otherChunks) continue;
        for (const auto& source : otherChunks->children) {
            const auto* sourceName = source.field("name");
            const auto* sourceList = source.field("superbundles");
            if (!sourceName || !sourceList) continue;
            Node* target{};
            for (auto& candidate : chunks->children) {
                const auto* candidateName = candidate.field("name");
                if (candidateName && candidateName->text == sourceName->text) { target = &candidate; break; }
            }
            auto* targetList = target ? target->field("superbundles") : nullptr;
            if (!targetList) continue;
            for (const auto& entry : sourceList->children) {
                if (entry.type != 7) continue;
                const auto key = ascii_lower(entry.text);
                bool held{};
                for (const auto& existing : targetList->children)
                    if (existing.type == 7 && ascii_lower(existing.text) == key) { held = true; break; }
                if (held) continue;
                auto position = targetList->children.begin();
                for (; position != targetList->children.end(); ++position)
                    if (position->type == 7 && ascii_lower(position->text) > key) break;
                Node added;
                added.type = 7;
                added.text = entry.text;
                // A string node is written from its payload, terminator included.
                added.owned.assign(entry.text.begin(), entry.text.end());
                added.owned.push_back(0);
                targetList->children.insert(position, std::move(added));
                report.notes.push_back("Merged layout puts " + entry.text + " in " + sourceName->text);
            }
        }
    }
    // Declare the re-indexed archives, and only under the install chunks the
    // merged patch really reads them from: a record for an install chunk
    // that has no such file sends the engine looking for one that is absent.
    const auto declare = [&](const char* field) {
        auto* node = layout.field(field);
        if (!node || node->type != 19) return;
        const auto source = node->payload();
        if (source.size() % 8) throw std::runtime_error(std::string(field) + " is not a record table");
        std::vector<unsigned char> table(source.begin(), source.end());
        std::size_t added{};
        for (const auto& [installChunk, archive] : used) {
            const std::array<unsigned char, 4> id{
                static_cast<unsigned char>(installChunk & 0xFF),
                static_cast<unsigned char>((installChunk >> 8) & 0xFF),
                static_cast<unsigned char>((installChunk >> 16) & 0xFF),
                static_cast<unsigned char>((installChunk >> 24) & 0xFF)};
            // The table is grouped by install chunk and every group the game
            // ships is ordered by archive index, so a new archive goes inside
            // its own group and in that order, not after it and not last.
            std::size_t insertAt{};
            bool owns{}, already{}, placed{};
            for (std::size_t at = 0; at + 8 <= table.size(); at += 8) {
                if (std::memcmp(table.data() + at + 2, id.data(), id.size())) continue;
                owns = true;
                const auto current = static_cast<std::uint16_t>(table[at] | (table[at + 1] << 8));
                if (current == archive) already = true;
                if (!placed && current > archive) { insertAt = at; placed = true; }
                if (!placed) insertAt = at + 8;
            }
            if (!owns || already) continue;
            std::array<unsigned char, 8> record{};
            record[0] = static_cast<unsigned char>(archive & 0xFF);
            record[1] = static_cast<unsigned char>(archive >> 8);
            std::memcpy(record.data() + 2, id.data(), id.size());
            table.insert(table.begin() + static_cast<std::ptrdiff_t>(insertAt),
                         record.begin(), record.end());
            ++added;
        }
        if (!added) return;
        report.notes.push_back(std::string(field) + ": declared " + std::to_string(added) +
            " patch archive(s)");
        node->owned = std::move(table);
        node->bytes = {};
    };
    declare("layeredInstallChunkFiles");
    declare("unlayeredInstallChunkFiles");
}

} // namespace dingosdk::mods::detail
