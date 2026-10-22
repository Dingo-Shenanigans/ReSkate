// Build a private articulated native model pack from the existing FPV template.
// Only the chosen output folder is written. The source game and template stay intact.
#include "Engine/Vfs/game_archives.h"
#include "Engine/Vfs/game_bundles.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_writer.h"
#include "Engine/Resource/mesh_set.h"
#include "Engine/Resource/toc.h"
#include "Engine/Resource/texture.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace fb = dingosdk::frostbite;
namespace ebx = fb::ebx;
namespace vfs = dingosdk::vfs;
namespace db = dingosdk::native_db;
namespace fs = std::filesystem;
using Bytes = std::vector<std::byte>;
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
Bytes read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    require(size >= 0 && size < 64*1024*1024, "Input is missing or exceeds builder bound");
    Bytes bytes(static_cast<std::size_t>(size)); file.seekg(0);
    require(static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), size)), "Cannot read input");
    return bytes;
}
void write(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path()); std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(file), "Cannot write pack artifact");
}
fb::Sha1 sha(std::span<const std::byte> bytes) {
    fb::Sha1 value;
    require(bytes.size() <= std::numeric_limits<ULONG>::max(), "Hash bound");
    const auto status = BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()),
        reinterpret_cast<PUCHAR>(value.bytes.data()), static_cast<ULONG>(value.bytes.size()));
    require(status >= 0, "Native SHA1 failed"); return value;
}
fb::Sha1 sha(std::string_view text) { return sha({reinterpret_cast<const std::byte*>(text.data()), text.size()}); }
fb::Guid guid(std::string_view key) {
    const auto hashed = sha("ReSkateBMXNative20261017/" + std::string(key));
    fb::Guid value; std::copy_n(hashed.bytes.begin(), 16, value.bytes.begin()); return value;
}
std::uint64_t id(std::string_view key) {
    const auto hashed = sha("ReSkateBMXNative20261017/" + std::string(key));
    std::uint64_t value{}; std::memcpy(&value, hashed.bytes.data(), sizeof(value)); return value;
}
std::uint64_t owner_hash(std::string_view name) {
    std::uint64_t value = 5381; for (const unsigned char c : name) value = value*33 ^ c; return value;
}
ebx::Value clone(const ebx::Value& from) {
    ebx::Value result = from;
    if (const auto* object = std::get_if<std::shared_ptr<ebx::Object>>(&from.data); object && *object) {
        auto next = std::make_shared<ebx::Object>(**object);
        for (auto& field : next->fields) field.value = clone(field.value);
        result.data = std::move(next);
    } else if (const auto* array = std::get_if<ebx::Value::Array>(&from.data)) {
        ebx::Value::Array next; for (const auto& item : *array) next.push_back(clone(item)); result.data = std::move(next);
    }
    return result;
}
ebx::FieldValue& field(ebx::Object& object, std::string_view name) {
    const auto found = std::find_if(object.fields.begin(), object.fields.end(), [&](const auto& item) { return item.name == name; });
    require(found != object.fields.end(), "Template field is missing"); return *found;
}
void remap_external(ebx::Value& value, std::int32_t offset) {
    if (auto* pointer = std::get_if<ebx::PointerReference>(&value.data); pointer && pointer->kind == ebx::PointerKind::external)
        pointer->index += offset;
    else if (auto* object = std::get_if<std::shared_ptr<ebx::Object>>(&value.data); object && *object)
        for (auto& item : (*object)->fields) remap_external(item.value, offset);
    else if (auto* array = std::get_if<ebx::Value::Array>(&value.data))
        for (auto& item : *array) remap_external(item, offset);
}
void bounds(ebx::Value& value, const std::array<float, 6>& box) {
    auto& array = std::get<ebx::Value::Array>(value.data); require(array.size() == 1, "Unexpected part bounds");
    auto& object = *std::get<std::shared_ptr<ebx::Object>>(array[0].data);
    for (unsigned end = 0; end < 2; ++end) {
        auto& point = *std::get<std::shared_ptr<ebx::Object>>(field(object, end ? "max" : "min").value.data);
        unsigned axis{}; for (const auto* name : {"x", "y", "z"}) field(point, name).value.data = double(box[end*3+axis++]);
    }
}
template<class T> void put(Bytes& bytes, std::size_t at, T value) {
    static_assert(std::is_integral_v<T>);
    require(at <= bytes.size() && sizeof(T) <= bytes.size()-at, "Header write bound");
    std::memcpy(bytes.data()+at, &value, sizeof(value));
}
struct TextureInput {
    std::uint32_t width{},height{},format{},count{};
    std::array<std::uint32_t,15> sizes{};
    Bytes pixels;
};
TextureInput texture_input(const fs::path& root, std::string_view name) {
    const auto info=read(root/(std::string(name)+".info"));
    require(info.size()==80, "Native texture input descriptor length");
    fb::BinaryReader in(info);
    require(in.fixed_string(4)=="BMXT", "Native texture input descriptor magic");
    TextureInput t;t.width=in.u32();t.height=in.u32();t.format=in.u32();t.count=in.u32();
    require(t.width>0 && t.width<=4096 && t.height>0 && t.height<=4096 &&
        t.count>0 && t.count<=15 && (t.format==55 || t.format==63), "Texture dimensions or format outside private contract");
    auto width=t.width,height=t.height;std::uint64_t total{};
    for(unsigned mip=0;mip<15;++mip) {
        t.sizes[mip]=in.u32();
        const auto expected=mip<t.count?std::max(1u,(width+3)/4)*std::max(1u,(height+3)/4)*(t.format==55?8u:16u):0u;
        require(t.sizes[mip]==expected, "Texture mip byte count mismatch");total+=t.sizes[mip];
        width=std::max(1u,width/2);height=std::max(1u,height/2);
    }
    t.pixels=read(root/(std::string(name)+".pixels"));require(total==t.pixels.size(), "Texture pixel payload mismatch");
    return t;
}
void texture_dimensions(ebx::Document& document, const TextureInput& input) {
    require(document.rootType=="TextureAsset" && document.instances.size()==1, "Unexpected donor texture document");
    auto& object=*document.instances[0].object;
    // These fields are signed s16/u16 in the donor. Preserve the parsed variant
    // representation so EBX serialization does not silently change field type.
    const auto assign=[](ebx::Value& value,std::uint32_t number) {
        if(std::holds_alternative<std::uint64_t>(value.data))value.data=std::uint64_t(number);
        else {require(std::holds_alternative<std::int64_t>(value.data), "Unexpected texture dimension scalar");value.data=std::int64_t(number);}
    };
    assign(field(object,"AuthoredWidth").value,input.width);assign(field(object,"AuthoredHeight").value,input.height);
    auto& crop=*std::get<std::shared_ptr<ebx::Object>>(field(object,"CropInfo").value.data);
    assign(field(crop,"X").value,0);assign(field(crop,"Y").value,0);
    assign(field(crop,"Z").value,input.width);assign(field(crop,"W").value,input.height);
}

int main(int argc, char** argv) { try {
    if (argc == 5 && std::string_view(argv[1]) == "--extract-template") {
        const fs::path root = fs::absolute(argv[2]), output = fs::absolute(argv[3]), game = fs::absolute(argv[4]);
        require(root != output && !fs::exists(output), "Template inspection needs a new separate output directory");
        auto layout = vfs::read_layout(root / "layout.toc"); vfs::GameArchives archives(root, layout.root);
        const auto toc = fb::read_toc(read(root / "Win32/levels/game/dingolevel_root/dingolevel_root.toc"));
        require(toc.bundles.size() == 1, "Expected one native template bundle");
        const auto region = fb::read_bundle_region(toc.bundles[0].region);
        require(!region.files.empty(), "Template bundle is empty");
        const auto first = region.files[0];
        const auto bundle = archives.read_manifest(root, first.location, first.offset, first.size, game);
        const auto found = std::find_if(bundle.resources.begin(), bundle.resources.end(), [](const auto& item) { return item.name == "reskate/drone/fpv_drone_mesh"; });
        require(found != bundle.resources.end(), "Private native mesh template is absent");
        const auto resource_index = static_cast<std::size_t>(found-bundle.resources.begin());
        const auto file = region.files.at(1+bundle.ebx.size()+resource_index);
        require(file.location.patch, "Template resource is not local");
        const auto resource = fb::decode_cas(archives.read(root,file.location,file.offset,file.size),{game});
        const auto mesh = fb::any_mesh_lod_geometry(resource,0);
        const auto chunk = std::find_if(bundle.chunks.begin(),bundle.chunks.end(),[&](const auto& item) { return item.guid == mesh.chunk; });
        require(chunk != bundle.chunks.end(), "Template geometry chunk is absent");
        const auto chunk_file = region.files.at(1+bundle.ebx.size()+bundle.resources.size()+static_cast<std::size_t>(chunk-bundle.chunks.begin()));
        require(chunk_file.location.patch, "Template geometry is not local");
        const auto geometry = fb::decode_cas(archives.read(root,chunk_file.location,chunk_file.offset,chunk_file.size),{game});
        require(fb::check_mesh_lod(resource,0,geometry).empty(), "Invalid original native mesh template");
        write(output/"native-template.mesh",resource); write(output/"native-template.geom",geometry);
        std::cout << "Validated native template declaration and geometry exported.\n";
        return 0;
    }
    require(argc == 5 || argc == 6, "Usage: builder TEMPLATE PARTS OUTPUT SUPPORTED_GAME [TEXTURES]");
    const fs::path root = fs::absolute(argv[1]), parts = fs::absolute(argv[2]), output = fs::absolute(argv[3]), game = fs::absolute(argv[4]);
    require(root != output && !fs::exists(output), "Output must be a new separate directory");
    std::vector<std::string> part_names{"frame","fork","rear_wheel","front_wheel","cranks","handlebars"};
    if(fs::exists(parts/"part-list.txt")) {
        auto expected=part_names;expected.push_back("left_pedal");expected.push_back("right_pedal");
        std::ifstream list(parts/"part-list.txt");std::vector<std::string> actual;std::string line;
        while(std::getline(list,line)) {if(!line.empty() && line.back()=='\r')line.pop_back();actual.push_back(line);}
        require(actual==expected, "Unexpected private articulated part list");part_names=std::move(actual);
    }
    require(argc==5 || part_names.size()==8, "Supplied-model textures require the eight-part geometry contract");
    constexpr auto toc_path = "Win32/levels/game/dingolevel_root/dingolevel_root.toc";
    auto layout = vfs::read_layout(root / "layout.toc"); vfs::GameArchives archives(root, layout.root);
    auto toc = fb::read_toc(read(root / toc_path)); require(toc.bundles.size() == 1, "Expected one template bundle");
    auto region = fb::read_bundle_region(toc.bundles[0].region); require(!region.files.empty(), "Template manifest missing");
    const auto manifest_file = region.files[0];
    auto bundle = archives.read_manifest(root, manifest_file.location, manifest_file.offset, manifest_file.size, game);
    const auto old_e = bundle.ebx.size(), old_r = bundle.resources.size(), old_c = bundle.chunks.size();
    require(region.files.size() == 1+old_e+old_r+old_c, "Template asset ordering");
    const auto find = [](const auto& list, std::string_view name) {
        const auto at = std::find_if(list.begin(), list.end(), [&](const auto& item) { return item.name == name; });
        require(at != list.end(), "Template asset missing"); return static_cast<std::size_t>(at-list.begin());
    };
    const auto mesh_i = find(bundle.ebx, "reskate/drone/fpv_drone_mesh");
    const auto bp_i = find(bundle.ebx, "reskate/drone/fpv_drone");
    const auto db_i = find(bundle.ebx, "levels/game/dingolevel_root/dingolevel_root/meshvariationdb_win32");
    const auto res_i = find(bundle.resources, "reskate/drone/fpv_drone_mesh");
    const auto payload = [&](std::size_t index) {
        const auto& file = region.files.at(index+1); require(file.location.patch, "Expected private template payload");
        return fb::decode_cas(archives.read(root, file.location, file.offset, file.size), {game});
    };
    const auto mesh_bytes = payload(mesh_i), bp_bytes = payload(bp_i);
    auto variations = ebx::read_document(payload(db_i));
    auto& entries = std::get<ebx::Value::Array>(field(*variations.instances[0].object, "Entries").value.data);
    require(entries.size() == 1, "Expected one private template mesh variation");
    const auto prototype_entry = clone(entries[0]);
    const auto original_imports = variations.imports;
    auto metadata = db::read({reinterpret_cast<const unsigned char*>(bundle.chunkMetadata.data()), bundle.chunkMetadata.size()},
        "Private native pack chunk metadata", nullptr, {.unique_fields=false, .max_entries=bundle.chunkMetadata.size()});
    require(metadata.children.size() == old_c, "Chunk metadata count");
    fs::create_directories(output);
    for (const auto* name : {"layout.toc", "initfs_win32", ".reskate-studio-patch"}) fs::copy_file(root/name, output/name);
    const auto archive_path = fs::path("Win32") / archives.directory(manifest_file.location.installChunk) / vfs::archive_file(manifest_file.location.archive);
    fs::create_directories((output/archive_path).parent_path()); fs::copy_file(root/archive_path, output/archive_path);
    std::ofstream cas(output/archive_path, std::ios::binary | std::ios::app);
    struct StoredPayload { fb::BundleFileInfo file; fb::Sha1 digest; bool encoded{}; };
    std::vector<StoredPayload> written_payloads;
    const auto append_stored = [&](std::span<const std::byte> stored, fb::Sha1 digest, bool encoded) {
        // MSVC's app mode seeks for each write but initially reports position0.
        // Resolve the actual end before recording the first payload's offset.
        cas.seekp(0, std::ios::end);
        const auto offset = cas.tellp(); require(offset >= 0 && offset < std::numeric_limits<std::uint32_t>::max(), "CAS size bound");
        cas.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
        require(static_cast<bool>(cas) && stored.size() < std::numeric_limits<std::uint32_t>::max(), "CAS append failed");
        const fb::BundleFileInfo file{manifest_file.location, static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(stored.size())};
        written_payloads.push_back({file, digest, encoded});
        return file;
    };
    const auto append = [&](std::span<const std::byte> bytes, bool encode=true) {
        // Use the same native Kraken path as the mod merger.
        const auto stored = encode ? fb::encode_cas(bytes, {game}) : Bytes(bytes.begin(), bytes.end());
        return append_stored(stored,sha(bytes),encode);
    };
    std::vector<fb::BundleFileInfo> ef(region.files.begin()+1, region.files.begin()+1+old_e);
    std::vector<fb::BundleFileInfo> rf(region.files.begin()+1+old_e, region.files.begin()+1+old_e+old_r);
    std::vector<fb::BundleFileInfo> cf(region.files.begin()+1+old_e+old_r, region.files.end());
    if(argc==6) for(const auto* slot:{"albedo","normal"}) {
        const bool colour=std::string_view(slot)=="albedo";
        const std::string donor="reskate/drone/fpv_drone/textures/frame_2_"+std::string(colour?"basecolor":"normal");
        // The compiled shader depot binds these TextureAsset identities,
        // not imports in MeshVariationDatabase. Replace their payloads only
        // in this NEW private pack. Original template files remain intact.
        const std::string name=donor;
        const auto donor_e=find(bundle.ebx,donor),donor_r=find(bundle.resources,donor);
        auto document=ebx::read_document(payload(donor_e));
        const auto input=texture_input(fs::absolute(argv[5]),slot);
        require(input.format==(colour?55u:63u), "Colour/normal texture format mismatch");
        auto resource=bundle.resources[donor_r];auto header=payload(old_e+donor_r);
        const auto original=fb::read_texture_header(header,resource.resourceMeta);
        require(header.size()==180 && resource.resourceMeta.size()>=4, "Unexpected extended texture header");
        fb::BinaryReader version(resource.resourceMeta);
        const auto texture_version=version.u32();
        std::cout<<donor<<" header version="<<texture_version<<" type="<<original.type<<" format="<<original.format
            <<" dimensions="<<original.width<<'x'<<original.height<<'\n';
        require(texture_version>=12 && texture_version<=13 && original.type==0 && original.format==input.format &&
            header[32]==header[31] && header[33]==header[30], "Supported texture header contract changed");
        std::uint64_t old_owner{};std::memcpy(&old_owner,header.data()+120,8);
        require(old_owner==owner_hash(donor), "Donor texture owner hash differs");
        // Each mip ends on a CAS block boundary. The first two words identify
        // COMPRESSED stream ends, not decoded pixel offsets (Frosty Utils.cs).
        Bytes encoded;std::array<std::uint32_t,2> mip_ends{};std::size_t pixel_offset{};
        for(unsigned mip=0;mip<input.count;++mip) {
            const auto block=fb::encode_cas(std::span<const std::byte>(input.pixels).subspan(pixel_offset,input.sizes[mip]),{game});
            encoded.insert(encoded.end(),block.begin(),block.end());pixel_offset+=input.sizes[mip];
            if(mip<2 && input.count>1)mip_ends[mip]=static_cast<std::uint32_t>(encoded.size());
        }
        require(fb::decode_cas(encoded,{game})==input.pixels, "Texture mip CAS round trip failed");
        const auto chunk_id=original.chunk;
        require(std::get<ebx::ResourceReference>(field(*document.instances[0].object,"Resource").value.data).id==resource.resourceId,
            "Compiled texture binding resource differs from its header");
        put(header,0,mip_ends[0]);put(header,4,mip_ends[1]);
        put(header,20,static_cast<std::uint16_t>(input.count==1?original.flags&~1u:original.flags));
        put(header,22,static_cast<std::uint16_t>(input.width));put(header,24,static_cast<std::uint16_t>(input.height));
        put(header,30,static_cast<std::uint8_t>(input.count));put(header,31,std::uint8_t{0});
        // All 15 retained donor headers duplicate first/count here. Preserve
        // all six unexplained bytes34..39; this extended-header duplication remains
        // an observed contract, to be checked again by the native loader.
        put(header,32,std::uint8_t{0});put(header,33,static_cast<std::uint8_t>(input.count));
        std::copy(chunk_id.bytes.begin(),chunk_id.bytes.end(),header.begin()+40);
        for(unsigned mip=0;mip<15;++mip)put(header,56+mip*4,input.sizes[mip]);
        put(header,116,static_cast<std::uint32_t>(input.pixels.size()));put(header,120,owner_hash(name));
        const auto checked=fb::read_texture_header(header,resource.resourceMeta);
        require(checked.chunk==chunk_id && checked.width==input.width && checked.height==input.height &&
            checked.first_mip==0 && checked.mip_count==input.count && checked.chunk_size==input.pixels.size(), "Generated texture header mismatch");
        texture_dimensions(document,input);
        const auto bytes=ebx::write_document(document);
        require(ebx::read_document(bytes).fileGuid==document.fileGuid, "Texture EBX round trip failed");
        auto& asset=bundle.ebx[donor_e];asset.sha1=sha(bytes);asset.originalSize=bytes.size();ef[donor_e]=append(bytes);
        resource.sha1=sha(header);resource.originalSize=header.size();bundle.resources[donor_r]=resource;rf[donor_r]=append(header);
        const auto chunk_it=std::find_if(bundle.chunks.begin(),bundle.chunks.end(),[&](const auto& item){return item.guid==chunk_id;});
        require(chunk_it!=bundle.chunks.end(), "Bound texture chunk is missing");
        const auto chunk_index=static_cast<std::size_t>(chunk_it-bundle.chunks.begin());
        auto& chunk=*chunk_it;
        chunk.sha1=sha(input.pixels);chunk.originalSize=input.pixels.size();chunk.logicalSize=static_cast<std::uint32_t>(input.pixels.size());
        chunk.logicalOffset=0;
        const auto placement=append_stored(encoded,sha(input.pixels),true);cf[chunk_index]=placement;
        unsigned toc_matches{};
        for(auto& entry:toc.chunks)if(entry.guid==chunk_id){entry.location=placement.location;entry.offset=placement.offset;entry.size=placement.size;++toc_matches;}
        require(toc_matches==1, "Bound texture requires one whole TOC chunk");
        unsigned owner_matches{};
        for(auto& row:metadata.children) {
            const auto* owner=row.field("h64");std::uint64_t hash{};
            if(!owner || owner->payload().size()!=8)continue;
            std::memcpy(&hash,owner->payload().data(),8);if(hash!=owner_hash(name))continue;
            auto* meta=row.field("meta");require(meta!=nullptr, "Texture chunk metadata missing meta object");
            auto* mip=meta->field("firstMip");require(mip!=nullptr && mip->payload().size()==4, "Texture first mip metadata missing");
            mip->bytes={};mip->owned.assign(4,0);++owner_matches;
        }
        require(owner_matches==1, "Texture metadata owner must resolve uniquely");
        write(output/"texture-audit"/(std::string(slot)+".header"),header);
        write(output/"texture-audit"/(std::string(slot)+".pixels"),input.pixels);
        std::cout<<name<<" "<<input.width<<'x'<<input.height<<" mips="<<input.count<<" compressed mip ends="<<mip_ends[0]<<','<<mip_ends[1]<<'\n';
    }
    // The donor imports this group from the city bundle. Root-domain meshes
    // cannot resolve an asset loaded only in a child level domain. Preserve
    // the original authored dependency, but load it beside these meshes.
    constexpr auto lod_name = "common/utilities/lodgroups/worldobject/medium/worldobject_mediumvista_2lod";
    const vfs::GameData game_data(game);
    const auto city_toc = game_data.read_toc("Win32/levels/game/bam_levelroot/bam_levelroot.toc");
    const auto city = game_data.read_bundle(city_toc, "win32/levels/game/bam_levelroot/bam_levelroot");
    require(city.has_value(), "Supported city bundle with donor LOD group is absent");
    std::size_t lod_index{};
    const auto* lod_asset = city->find(fb::AssetKind::ebx, lod_name, &lod_index);
    require(lod_asset != nullptr, "Donor LOD group is absent");
    const auto* lod_payload = city->payload(fb::AssetKind::ebx, lod_index);
    require(lod_payload != nullptr, "Donor LOD group has no payload");
    const auto lod_bytes = game_data.read(*lod_payload);
    const auto lod = ebx::read_document(lod_bytes);
    const auto donor_mesh = ebx::read_document(mesh_bytes);
    require(lod_bytes.size() == 2240 && lod.rootType == "MeshLodGroup" && lod.imports.empty() &&
        lod.instances.size() == 1, "Donor LOD shape differs");
    require(!donor_mesh.imports.empty() &&
        lod.fileGuid == donor_mesh.imports[0].fileGuid && lod.instances[0].instanceGuid == donor_mesh.imports[0].classGuid &&
        lod.fileGuid.string() == "7a5d035c-d46c-4cc2-8417-9839b66383d5", "Donor LOD import pair differs");
    // Validate the decoded source bytes against the independently extracted
    // supported-build dependency, rather than assuming the manifest digest
    // describes the decoded representation.
    constexpr std::array<unsigned char, 20> lod_sha1{0x16,0x6f,0x68,0xb7,0x13,0xc7,0x92,0x60,0xa4,0x7f,
        0x6a,0x3a,0x9e,0x62,0xc1,0x08,0x74,0x2a,0x46,0xfc};
    const auto lod_digest = sha(lod_bytes);
    require(std::memcmp(lod_digest.bytes.data(), lod_sha1.data(), lod_sha1.size()) == 0,
        "Donor LOD decoded source SHA1 differs");
    require(std::none_of(bundle.ebx.begin(), bundle.ebx.end(), [&](const auto& item) { return item.name == lod_name; }),
        "Template already contains the donor LOD group");
    // Insert before the donor and aliases, preserving manifest/payload order.
    bundle.ebx.insert(bundle.ebx.begin()+mesh_i, *lod_asset);
    ef.insert(ef.begin()+mesh_i, append(lod_bytes));
    const auto effective_db_i = db_i + (mesh_i <= db_i ? 1 : 0);
    for (const auto& part : part_names) {
        const std::string name = "reskate/bmx/" + std::string(part), mesh_name = name + "_mesh";
        auto mesh = ebx::read_document(mesh_bytes), blueprint = ebx::read_document(bp_bytes);
        const auto old_mesh_file = mesh.fileGuid, old_mesh_root = mesh.instances[0].instanceGuid;
        mesh.fileGuid = guid(std::string(part)+"/mesh"); mesh.instances[0].instanceGuid = guid(std::string(part)+"/mesh_root");
        field(*mesh.instances[0].object, "Name").value.data = mesh_name;
        // Native ResourceRef stores an immediate resource ID with bit0 set.
        // Even values can be interpreted as live/refcounted pointers while the
        // loader replaces the reference. Keep the same tag as donor resources.
        const auto resource_id = id(std::string(part)+"/resource") | std::uint64_t{1};
        field(*mesh.instances[0].object, "MeshSetResource").value.data = ebx::ResourceReference{resource_id};
        const auto hash = static_cast<std::uint32_t>(owner_hash(mesh_name));
        field(*mesh.instances[0].object, "NameHash").value.data = std::uint64_t(hash);
        auto mesh_resource = read(parts/(std::string(part)+".mesh"));
        std::memcpy(mesh_resource.data()+120, &hash, sizeof(hash));
        // Native setup resolves MeshAsset by this embedded full name. Reusing
        // the donor name binds the new resource to the donor's asset settings.
        std::uint64_t full_name_offset{};
        std::memcpy(&full_name_offset, mesh_resource.data()+104, sizeof(full_name_offset));
        require(full_name_offset < mesh_resource.size()-16, "Mesh name pointer is outside its resource");
        const auto full_name_at = static_cast<std::size_t>(full_name_offset)+16;
        const auto name_end = std::find(mesh_resource.begin()+full_name_at, mesh_resource.end(), std::byte{});
        require(name_end != mesh_resource.end() && mesh_name.size() <= static_cast<std::size_t>(name_end-(mesh_resource.begin()+full_name_at)), "New mesh name exceeds the donor string slot");
        std::fill(mesh_resource.begin()+full_name_at, name_end, std::byte{});
        std::memcpy(mesh_resource.data()+full_name_at, mesh_name.data(), mesh_name.size());
        const auto short_name_offset = full_name_offset+std::string_view("reskate/bmx/").size();
        std::memcpy(mesh_resource.data()+112, &short_name_offset, sizeof(short_name_offset));
        const auto geometry = read(parts/(std::string(part)+".geom"));
        require(fb::check_mesh_lod(mesh_resource, 0, geometry).empty(), "Generated geometry is outside its declarations");
        const auto chunk_id = guid(std::string(part)+"/chunk");
        require(fb::any_mesh_lod_geometry(mesh_resource, 0).chunk == chunk_id, "Geometry chunk GUID mismatch");
        std::array<float,6> box{}; std::memcpy(box.data(), mesh_resource.data()+16, 12); std::memcpy(box.data()+3, mesh_resource.data()+32, 12);
        blueprint.fileGuid = guid(std::string(part)+"/blueprint"); blueprint.instances[0].instanceGuid = guid(std::string(part)+"/blueprint_root");
        field(*blueprint.instances[0].object, "Name").value.data = name;
        require(blueprint.imports.size() == 1 && blueprint.imports[0].fileGuid == old_mesh_file, "Template mesh import mismatch");
        blueprint.imports[0] = {mesh.fileGuid, mesh.instances[0].instanceGuid};
        bounds(field(*blueprint.instances[1].object, "PartBoundingBoxes").value, box);
        auto variation = clone(prototype_entry); const auto import_offset = static_cast<std::int32_t>(variations.imports.size());
        remap_external(variation, import_offset);
        for (auto imported : original_imports) {
            if (imported.fileGuid == old_mesh_file) {
                imported.fileGuid = mesh.fileGuid;
                if (imported.classGuid == old_mesh_root) imported.classGuid = mesh.instances[0].instanceGuid;
            }
            variations.imports.push_back(imported);
        }
        entries.push_back(std::move(variation));
        for (const auto& pair : {std::pair{&mesh, mesh_name}, std::pair{&blueprint, name}}) {
            const auto bytes = ebx::write_document(*pair.first);
            require(ebx::read_document(bytes).fileGuid == pair.first->fileGuid, "EBX round trip failed");
            auto asset = bundle.ebx[mesh_i+1]; asset.name = pair.second; asset.sha1 = sha(bytes); asset.originalSize = bytes.size();
            bundle.ebx.push_back(asset); ef.push_back(append(bytes));
        }
        auto resource = bundle.resources[res_i]; resource.name = mesh_name; resource.resourceId = resource_id;
        resource.sha1 = sha(mesh_resource); resource.originalSize = mesh_resource.size(); bundle.resources.push_back(resource); rf.push_back(append(mesh_resource));
        fb::BundleAsset chunk; chunk.kind = fb::AssetKind::chunk; chunk.guid = chunk_id; chunk.name = chunk_id.string();
        chunk.sha1 = sha(geometry); chunk.originalSize = geometry.size(); chunk.logicalSize = static_cast<std::uint32_t>(geometry.size());
        const auto placement = append(geometry); bundle.chunks.push_back(chunk); cf.push_back(placement);
        toc.chunks.push_back({chunk_id, placement.location, placement.offset, placement.size, false});
        db::Node row, hash_node, meta; row.type=2; row.terminated=true; hash_node.type=9; hash_node.named=true; hash_node.name="h64";
        const auto owner = owner_hash(mesh_name); for (unsigned i=0;i<8;++i) hash_node.owned.push_back(static_cast<unsigned char>(owner>>(i*8)));
        meta.type=2; meta.named=true; meta.name="meta"; meta.terminated=true;
        row.children.push_back(std::move(hash_node)); row.children.push_back(std::move(meta)); metadata.children.push_back(std::move(row));
        std::cout << name << " native mesh bytes=" << mesh_resource.size() << " geometry=" << geometry.size() << " chunk=" << chunk_id.string() << '\n';
    }
    const auto db_bytes = ebx::write_document(variations); (void)ebx::read_document(db_bytes);
    bundle.ebx[effective_db_i].sha1 = sha(db_bytes); bundle.ebx[effective_db_i].originalSize = db_bytes.size(); ef[effective_db_i] = append(db_bytes);
    const auto meta_bytes = db::write(metadata);
    bundle.chunkMetadata.assign(reinterpret_cast<const std::byte*>(meta_bytes.data()), reinterpret_cast<const std::byte*>(meta_bytes.data()+meta_bytes.size()));
    const auto manifest = fb::write_binary_bundle(bundle); const auto first = append(manifest, false);
    std::vector<fb::BundleFileInfo> files{first}; files.insert(files.end(),ef.begin(),ef.end()); files.insert(files.end(),rf.begin(),rf.end()); files.insert(files.end(),cf.begin(),cf.end());
    toc.bundles[0].region = fb::write_bundle_region(files);
    const auto packed_toc = fb::write_patch_toc(toc.bundles,toc.chunks,toc.flags); fb::verify_toc(fb::read_toc(packed_toc));
    write(output/toc_path, packed_toc); cas.close();
    // Validate the bytes through the exact archive locations recorded in the
    // emitted pack, rather than only validating the in-memory inputs.
    const vfs::GameArchives emitted_archives(output, layout.root);
    for (const auto& item : written_payloads) {
        const auto stored = emitted_archives.read(output, item.file.location, item.file.offset, item.file.size);
        const auto decoded = item.encoded ? fb::decode_cas(stored, {game}) : stored;
        require(sha(decoded) == item.digest, "Emitted CAS payload differs at its recorded offset");
    }
    const auto emitted_bundle = emitted_archives.read_manifest(output, first.location, first.offset, first.size, game);
    require(emitted_bundle.ebx.size() == bundle.ebx.size() && emitted_bundle.resources.size() == bundle.resources.size() &&
        emitted_bundle.chunks.size() == bundle.chunks.size(), "Emitted manifest asset counts differ");
    std::ofstream credits(output/"BMX-NATIVE.txt"); credits << "Private local BMX prototype geometry; not a redistribution pack.\nNative template/materials: FPV Drone by PinaPipe (CC BY 4.0), prepared by ReSkate.\nhttps://sketchfab.com/3d-models/fpv-drone-92152c31ec9a4a87b2a0478bd114d293\nhttps://creativecommons.org/licenses/by/4.0/\n"<<part_names.size()<<" articulated render parts, no borrowed collision volumes.\n";
    if(argc==6)credits<<"Geometry/colour: user-supplied Downloads/bmx-free/source/BMX_sf.fbx; kept private.\nSource normals retained, flat tangent normal; donor packed mask retained.\n";
    std::cout << "Created " << output.string() << " with "<<part_names.size()<<" native bike blueprints.\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
