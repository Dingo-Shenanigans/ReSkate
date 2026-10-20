// Offline inspection only: native EBX fields and references, never game memory or asset writes.
#include "Engine/Core/Json/json.h"
#include "Engine/Game/Build/20260929/skater_skeleton.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

using dingosdk::Json;
namespace fb = dingosdk::frostbite;
namespace ebx = fb::ebx;
using Names = std::map<std::string, std::string>;
namespace {
std::string lower(std::string name) {
    for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return name;
}
std::string type_name(const ebx::Document& doc, std::int32_t descriptor) {
    return descriptor >= 0 && static_cast<std::size_t>(descriptor) < doc.types.size() ? doc.types[descriptor].name : "unknown";
}
Json value_json(const ebx::Value& value, const ebx::Document& doc, const Names& names, unsigned depth = 0) {
    if (depth > 64) throw std::runtime_error("EBX object nesting exceeds inspection limit");
    return std::visit([&](const auto& held) -> Json {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) return nullptr;
        else if constexpr (std::is_same_v<T, ebx::PointerReference>) {
            if (held.kind == ebx::PointerKind::null) return nullptr;
            if (held.index < 0) throw std::runtime_error("Negative EBX pointer index");
            const auto i = static_cast<std::size_t>(held.index);
            if (held.kind == ebx::PointerKind::internal) {
                if (i >= doc.instances.size()) throw std::runtime_error("Internal EBX pointer outside instance table");
                const auto& target = doc.instances[i];
                return {{"kind", "internal"}, {"instance", held.index},
                        {"type", target.object ? type_name(doc, target.object->descriptor) : "unknown"}};
            }
            if (i >= doc.imports.size()) throw std::runtime_error("External EBX pointer outside import table");
            const auto& target = doc.imports[i];
            const auto id = target.fileGuid.string();
            const auto found = names.find(id);
            return {{"kind", "external"}, {"partition", id}, {"instance", target.classGuid.string()},
                    {"asset", found == names.end() ? Json(nullptr) : Json(found->second)}};
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ebx::Object>>) {
            if (!held) return nullptr;
            Json fields = Json::object();
            for (const auto& field : held->fields) fields[field.name] = value_json(field.value, doc, names, depth + 1);
            return {{"type", type_name(doc, held->descriptor)}, {"fields", std::move(fields)}};
        } else if constexpr (std::is_same_v<T, ebx::Value::Array>) {
            Json items = Json::array();
            for (const auto& item : held) items.push_back(value_json(item, doc, names, depth + 1));
            return items;
        } else if constexpr (std::is_same_v<T, fb::Guid>) return held.string();
        else if constexpr (std::is_same_v<T, fb::Sha1>) {
            std::string hex;
            constexpr char digits[] = "0123456789abcdef";
            for (const auto byte : held.bytes) { const auto v = std::to_integer<unsigned>(byte); hex += digits[v >> 4]; hex += digits[v & 15]; }
            return hex;
        } else if constexpr (std::is_same_v<T, ebx::ResourceReference>) return {{"resource_id", held.id}};
        else if constexpr (std::is_same_v<T, ebx::TypeReference>) return {{"type_reference", held.descriptor}, {"encoded", held.encoded}};
        else if constexpr (std::is_same_v<T, ebx::BoxedReference>) return {{"boxed_type", held.encodedType}, {"offset", held.dataOffset}};
        else if constexpr (std::is_same_v<T, double>) return std::isfinite(held) ? Json(held) : Json{{"nonfinite", true}};
        else return held;
    }, value.data);
}
Json document_json(const std::string& name, const ebx::Document& doc, const Names& names) {
    Json instances = Json::array();
    for (std::size_t i = 0; i < doc.instances.size(); ++i) {
        const auto& instance = doc.instances[i];
        const ebx::Value wrapped{instance.object, {}};
        instances.push_back({{"index", i}, {"guid", instance.instanceGuid.string()}, {"object", value_json(wrapped, doc, names)}});
    }
    return {{"asset", name}, {"partition", doc.fileGuid.string()}, {"root_type", doc.rootType}, {"instances", std::move(instances)}};
}
void write(const std::filesystem::path& path, const Json& data) {
    std::ofstream out(path, std::ios::binary); out.exceptions(std::ios::badbit | std::ios::failbit);
    out << data.dump(2) << '\n';
}
void self_test() {
    ebx::Document doc;
    doc.imports.push_back({{}, {}});
    const Names names{{fb::Guid{}.string(), "animation/dingo/fixture"}};
    const auto ref = value_json(ebx::Value{ebx::PointerReference{ebx::PointerKind::external, 0}, {}}, doc, names);
    if (ref.at("asset").get<std::string>() != "animation/dingo/fixture") throw std::runtime_error("External pointer identity was lost");
    const auto unresolved = value_json(ebx::Value{ebx::PointerReference{ebx::PointerKind::external, 0}, {}}, doc, {});
    if (!unresolved.at("asset").is_null()) throw std::runtime_error("Missing reference must stay unresolved");
    bool rejected = false;
    try { (void)value_json(ebx::Value{ebx::PointerReference{ebx::PointerKind::internal, 0}, {}}, doc, names); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Out-of-range reference was accepted");
    std::cout << "animation_graph_probe: self-test passed\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") { self_test(); return 0; }
        if (argc == 4 && std::string_view(argv[1]) == "--document") {
            std::ifstream in(argv[2], std::ios::binary | std::ios::ate);
            if (!in || in.tellg() <= 0 || in.tellg() > 64 * 1024 * 1024) throw std::runtime_error("Invalid offline EBX size");
            std::vector<std::byte> bytes(static_cast<std::size_t>(in.tellg()));
            in.seekg(0); in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
            if (!in) throw std::runtime_error("Cannot read offline EBX");
            write(argv[3], document_json(argv[2], ebx::read_document(bytes), {}));
            return 0;
        }
        const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        if (argc == 5 && mode == "--motion-refs") {
            if (std::filesystem::file_size(argv[3]) > 4 * 1024 * 1024) throw std::runtime_error("PTM inspection input exceeds limit");
            std::ifstream input(argv[3], std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(input), {}};
            const auto source = Json::parse(text);
            std::map<std::string,std::string> targets;
            for (const auto& instance : source.at("instances")) {
                const auto& object = instance.at("object");
                if (object.at("type").get<std::string>() != "PTMDatabaseAsset") continue;
                for (const auto& ref : object.at("fields").at("MotionClips")) {
                    const auto guid = lower(ref.at("partition").get<std::string>());
                    targets.emplace("_af/" + guid.substr(0,18) + "/", guid);
                }
            }
            if (targets.empty()) throw std::runtime_error("No PTM motion references");
            const dingosdk::vfs::GameData data(std::filesystem::absolute(argv[2]));
            Json results = Json::array();
            const auto toc = data.read_toc("Win32/levels/game/bam_levelroot/bam_levelroot.toc");
            for (const auto& entry : toc.bundles) {
                const auto bundle = data.read_bundle(toc,entry.name);
                if (!bundle) continue;
                for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i) {
                    const auto name = lower(bundle->manifest.ebx[i].name);
                    const auto slash = name.find('/',4);
                    if (slash == std::string::npos) continue;
                    const auto found = targets.find(name.substr(0,slash+1));
                    if (found == targets.end()) continue;
                    const auto* payload = bundle->payload(fb::AssetKind::ebx,i);
                    if (!payload) throw std::runtime_error("Missing motion payload");
                    const auto doc = ebx::read_document(data.read(*payload));
                    if (lower(doc.fileGuid.string()) != found->second || doc.rootType != "MotionClip") continue;
                    const auto* root = doc.root();
                    if (!root || !root->object) throw std::runtime_error("Missing motion root");
                    Json fields = Json::object();
                    for (const auto& field : root->object->fields) {
                        if (field.name == "FeaturesBlob" || field.name == "FrameInfo" || field.name == "MetaDataBlob") continue;
                        fields[field.name] = value_json(field.value,doc,{});
                    }
                    results.push_back({{"asset",name},{"partition",doc.fileGuid.string()},{"fields",std::move(fields)}});
                    targets.erase(found);
                }
                if (targets.empty()) break;
            }
            write(argv[4],{{"motions",results},{"unresolved",targets.size()}});
            std::cout << "Resolved " << results.size() << " PTM motions; missing " << targets.size() << '\n';
            return targets.empty() ? 0 : 2;
        }
        const bool layered = argc == 6 && (mode == "--layer-asset" || mode == "--layer-resource");
        if (layered || (argc == 5 && (mode == "--asset" || mode == "--resource"))) {
            const auto data = layered ? dingosdk::vfs::GameData(std::filesystem::absolute(argv[2]), std::filesystem::absolute(argv[3]))
                                      : dingosdk::vfs::GameData(std::filesystem::absolute(argv[2]));
            const auto wanted = lower(argv[layered ? 4 : 3]);
            const bool guid_lookup = wanted.size() == 36 && wanted[8] == '-' && wanted[13] == '-' && wanted[18] == '-' && wanted[23] == '-';
            const auto output = argv[layered ? 5 : 4];
            const bool resource = mode == "--resource" || mode == "--layer-resource";
            const bool resource_id_lookup = resource && wanted.starts_with("0x");
            const auto resource_id = resource_id_lookup ? std::stoull(wanted,nullptr,16) : 0;
            for (const auto toc_name : {"Win32/levels/game/bam_levelroot/bam_levelroot.toc", "Win32/globals.toc",
                                       "Win32/levels/game/dingolevel_root/dingolevel_root.toc"}) {
                const auto toc = data.read_toc(toc_name);
                for (const auto& entry : toc.bundles) {
                    const auto bundle = data.read_bundle(toc, entry.name);
                    if (!bundle) continue;
                    const auto& assets = resource ? bundle->manifest.resources : bundle->manifest.ebx;
                    for (std::size_t i = 0; i < assets.size(); ++i) {
                        const auto name = lower(assets[i].name);
                        if (name != wanted && !(guid_lookup && name.starts_with("_af/" + wanted.substr(0,18) + "/")) &&
                            !(resource_id_lookup && assets[i].resourceId == resource_id)) continue;
                        const auto* payload = bundle->payload(resource ? fb::AssetKind::resource : fb::AssetKind::ebx, i);
                        if (!payload) throw std::runtime_error("Native EBX payload missing");
                        const auto bytes = data.read(*payload);
                        if (resource) {
                            std::ofstream out(output, std::ios::binary); out.exceptions(std::ios::badbit | std::ios::failbit);
                            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                            Json meta = Json::array();
                            for (const auto byte : assets[i].resourceMeta) meta.push_back(std::to_integer<unsigned>(byte));
                            write(std::string(output)+".meta.json",{{"asset",name},{"resource_id",assets[i].resourceId},
                                  {"resource_type",assets[i].resourceType},{"resource_meta",meta}});
                        } else {
                            const auto doc = ebx::read_document(bytes);
                            if (guid_lookup && lower(doc.fileGuid.string()) != wanted) continue;
                            write(output, document_json(name, doc, {}));
                        }
                        return 0;
                    }
                }
            }
            throw std::runtime_error("Asset not found in the inspected native bundles");
        }
        if (argc < 4) throw std::runtime_error("Usage: dingosdk_anim_graph_probe game-root output-directory asset [asset ...]");
        namespace skeleton = dingosdk::game::build::v20260929::skater_skeleton;
        const dingosdk::vfs::GameData data(std::filesystem::absolute(argv[1]));
        std::set<std::string> wanted;
        for (int i = 3; i < argc; ++i) wanted.insert(lower(argv[i]));
        Names names;
        std::map<std::string, ebx::Document> selected;
        Json index = Json::array(), errors = Json::array();
        std::set<std::string> seen;
        std::size_t scanned = 0;
        const auto scan = [&](const dingosdk::vfs::GameBundle& bundle, const std::string& toc, const std::string& bundle_name) {
          for (std::size_t i = 0; i < bundle.manifest.ebx.size(); ++i) {
            const auto name = lower(bundle.manifest.ebx[i].name);
            if (!name.starts_with("animation/dingo/") && !wanted.contains(name)) continue;
            if (seen.contains(name)) continue;
            try {
                const auto* payload = bundle.payload(fb::AssetKind::ebx, i);
                if (!payload) throw std::runtime_error("EBX payload is missing");
                auto document = ebx::read_document(data.read(*payload));
                const auto id = document.fileGuid.string();
                const auto [found, fresh] = names.emplace(id, name);
                if (!fresh && found->second != name) errors.push_back({{"asset", name}, {"error", "Duplicate partition GUID"}, {"other", found->second}});
                index.push_back({{"asset", name}, {"partition", id}, {"root_type", document.rootType}, {"instances", document.instances.size()},
                                 {"toc", toc}, {"bundle", bundle_name}, {"payload_sha1", value_json(ebx::Value{bundle.manifest.ebx[i].sha1, {}}, document, {})}});
                if (wanted.contains(name)) selected.emplace(name, std::move(document));
                seen.insert(name);
            } catch (const std::exception& e) { errors.push_back({{"asset", name}, {"error", e.what()}}); }
            if (++scanned % 2000 == 0) std::cout << "Inspected " << scanned << " native assets\n" << std::flush;
          }
        };
        const auto level = data.read_toc(skeleton::skeleton_toc);
        const auto root_bundle = data.read_bundle(level, skeleton::skeleton_bundle);
        if (!root_bundle) throw std::runtime_error("Native level root bundle is missing");
        scan(*root_bundle, std::string(skeleton::skeleton_toc), std::string(skeleton::skeleton_bundle));
        for (const auto& entry : level.bundles) {
            if (entry.name == skeleton::skeleton_bundle) continue;
            const auto bundle = data.read_bundle(level, entry.name);
            if (bundle) scan(*bundle, std::string(skeleton::skeleton_toc), entry.name);
        }
        const auto globals = data.read_toc("Win32/globals.toc");
        for (const auto& entry : globals.bundles) {
            const auto bundle = data.read_bundle(globals, entry.name);
            if (bundle) scan(*bundle, "Win32/globals.toc", entry.name);
        }
        const std::string base_toc = "Win32/levels/game/dingolevel_root/dingolevel_root.toc";
        const auto base = data.read_toc(base_toc);
        for (const auto& entry : base.bundles) {
            const auto bundle = data.read_bundle(base, entry.name);
            if (bundle) scan(*bundle, base_toc, entry.name);
        }
        Json assets = Json::array();
        for (const auto& [name, document] : selected) assets.push_back(document_json(name, document, names));
        Json missing = Json::array();
        for (const auto& name : wanted) if (!selected.contains(name)) missing.push_back(name);
        const auto output = std::filesystem::absolute(argv[2]);
        std::filesystem::create_directories(output);
        write(output / "native-asset-types.json", index);
        write(output / "native-graph.json", {{"schema", 1}, {"scope", "animation/dingo EBX in bam_levelroot, dingolevel_root and globals plus explicitly requested assets; first copy per name"},
              {"interpretation", "Static authoring data only; field offsets are not live memory addresses. Unresolved references remain null."},
              {"assets", assets}, {"missing", missing}, {"errors", errors}});
        std::cout << "Decoded " << index.size() << " assets; selected " << selected.size() << "; missing " << missing.size() << "; errors " << errors.size() << '\n';
        return missing.empty() ? 0 : 2;
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
