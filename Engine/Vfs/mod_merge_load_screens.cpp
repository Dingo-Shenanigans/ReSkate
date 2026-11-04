#include "mod_merge_internal.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Core/Platform/path_text.h"

#include <cstring>
#include <stdexcept>

namespace dingosdk::mods::detail {
namespace {
// Where the game keeps its loading screens: one configuration whose
// LevelOverrides rows each name a level and point at a RimeLoadScreenInfo.
// Studio appends a row per map (maps/ui/loading_screen.cpp).
constexpr std::string_view configurationToc = "win32/globals.toc";
constexpr std::string_view configurationBundle = "win32/default_settings";
constexpr std::string_view configuration = "ui/foundations/loading/loadscreen/dingoloadscreen_configuration";

const fb::ebx::Object* object_of(const fb::ebx::Value& value, const fb::ebx::Document& document) {
    if (const auto* inline_object = std::get_if<std::shared_ptr<fb::ebx::Object>>(&value.data)) return inline_object->get();
    const auto* pointer = std::get_if<fb::ebx::PointerReference>(&value.data);
    if (!pointer || pointer->kind != fb::ebx::PointerKind::internal || pointer->index < 0 ||
        static_cast<std::size_t>(pointer->index) >= document.instances.size())
        return nullptr;
    return document.instances[static_cast<std::size_t>(pointer->index)].object.get();
}
const std::string* text_of(const fb::ebx::Object& object, std::string_view name) {
    const auto* field = object.find(name);
    return field ? std::get_if<std::string>(&field->value.data) : nullptr;
}
}

std::vector<MergeReport::LoadScreen> read_load_screens(const std::vector<const Mod*>& mods,
                                                       const std::map<const Mod*, RelativeFiles>& modFiles,
                                                       const CasStore& store, const fs::path& baseRoot,
                                                       const fs::path& gameRoot, MergeReport& report) {
    std::vector<MergeReport::LoadScreen> result;
    for (const auto* mod : mods) {
        const auto modName = path_utf8(mod->directory.filename());
        for (const auto& relative : modFiles.at(mod).tocs) {
            if (ascii_lower(relative) != configurationToc) continue;
            try {
                const auto toc = fb::read_toc(read_file(mod->directory / fs::path(relative)));
                for (const auto& bundle : toc.bundles) {
                    if (ascii_lower(bundle.name) != configurationBundle) continue;
                    const auto listing = list_bundle(store, mod->directory, baseRoot, bundle, gameRoot);
                    if (!listing) break;
                    for (std::size_t index = 0; index < listing->manifest.ebx.size(); ++index) {
                        if (ascii_lower(listing->manifest.ebx[index].name) != configuration) continue;
                        const auto document =
                            fb::ebx::read_document(read_asset(store, mod->directory, baseRoot, *listing, index, gameRoot));
                        const auto* root = document.root();
                        const auto* rows = root && root->object ? root->object->find("LevelOverrides") : nullptr;
                        const auto* list = rows ? std::get_if<fb::ebx::Value::Array>(&rows->value.data) : nullptr;
                        if (!list) break;
                        for (const auto& value : *list) {
                            const auto* row = object_of(value, document);
                            const auto* level = row ? text_of(*row, "LevelName") : nullptr;
                            const auto* screen = row ? row->find("LoadScreen") : nullptr;
                            const auto* info = screen ? object_of(screen->value, document) : nullptr;
                            const auto* name = info ? text_of(*info, "BundleName") : nullptr;
                            const auto* widget = info ? info->find("WidgetAssetGuid") : nullptr;
                            const auto* guid = widget ? std::get_if<fb::Guid>(&widget->value.data) : nullptr;
                            if (!level || level->empty() || !name || !guid) continue;
                            MergeReport::LoadScreen entry{modName, *level, *name};
                            std::memcpy(entry.widget.data(), guid->bytes.data(), entry.widget.size());
                            result.push_back(std::move(entry));
                        }
                        break;
                    }
                    break;
                }
            } catch (const std::exception& failure) {
                report.notes.push_back(modName + ": its loading screens could not be read (" + failure.what() + ")");
            }
        }
    }
    return result;
}
}
