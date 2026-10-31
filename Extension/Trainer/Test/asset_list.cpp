// Lists the data assets of the game's level root bundle, or the values of one of them.
//   dingosdk_trainer_asset_list <Skate folder> <word> [<word> ...]   names containing any word
//   dingosdk_trainer_asset_list <Skate folder> --dump <asset name>   every plain value, by name
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/mesh_set.h"
#include "Engine/Vfs/game_bundles.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <string_view>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#pragma warning(disable : 4996) // std::getenv in a command line tool
namespace ebx = dingosdk::frostbite::ebx;
namespace {
std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
std::size_t value_size(ebx::FieldType type) {
    switch (type) {
    case ebx::FieldType::boolean: case ebx::FieldType::int8: case ebx::FieldType::uint8: return 1;
    case ebx::FieldType::int16: case ebx::FieldType::uint16: return 2;
    case ebx::FieldType::int32: case ebx::FieldType::uint32: case ebx::FieldType::enumeration:
    case ebx::FieldType::float32: return 4;
    case ebx::FieldType::int64: case ebx::FieldType::uint64: case ebx::FieldType::float64: return 8;
    default: return 0;
    }
}
void walk(const ebx::Document &document, const ebx::TypeDescriptor &type, std::span<const std::byte> image, std::size_t start, int depth,
          const std::string &path) {
    if (depth > 16) return;
    for (std::size_t i = 0; i < type.fieldCount; ++i) {
        const auto index = static_cast<std::size_t>(type.fieldIndex) + i;
        if (index >= document.fields.size()) return;
        const auto &field = document.fields[index];
        const auto kind = field.type();
        const auto name = path.empty() ? field.name : field.name.empty() ? path : path + "." + field.name;
        if (kind == ebx::FieldType::inherited || (kind == ebx::FieldType::structure && field.category() != ebx::FieldCategory::array)) {
            if (field.classRef >= document.types.size()) continue;
            const bool inherited = kind == ebx::FieldType::inherited;
            walk(document, document.types[field.classRef], image, inherited ? start : start + field.dataOffset, depth + 1,
                 inherited ? path : name);
            continue;
        }
        const auto offset = start + field.dataOffset;
        if (field.category() == ebx::FieldCategory::array) {
            std::cout << name << "\t0x" << std::hex << offset << std::dec << "\tarray\n";
            continue;
        }
        if (kind == ebx::FieldType::pointer) {
            std::cout << name << "\t0x" << std::hex << offset << std::dec << "\tpointer\n";
            continue;
        }
        const auto size = value_size(kind);
        if (!size || offset + size > image.size()) continue;
        std::cout << name << "\t0x" << std::hex << offset << std::dec << '\t';
        if (kind == ebx::FieldType::float32) {
            float value;
            std::memcpy(&value, image.data() + offset, 4);
            std::cout << "real\t" << value;
        } else if (kind == ebx::FieldType::boolean) {
            std::cout << "flag\t" << static_cast<int>(image[offset]);
        } else {
            std::int64_t value{};
            std::memcpy(&value, image.data() + offset, size);
            std::cout << "int" << size * 8 << '\t' << value;
        }
        std::cout << '\n';
    }
}
void tree(const ebx::Value &value, int depth, const std::string &indent);
void tree(const ebx::Object &object, int depth, const std::string &indent) {
    for (const auto &field : object.fields) {
        std::cout << indent << (field.name.empty() ? "?" : field.name) << " = ";
        tree(field.value, depth, indent);
    }
}
void tree(const ebx::Value &value, int depth, const std::string &indent) {
    if (depth > 12) { std::cout << "...\n"; return; }
    if (const auto *b = std::get_if<bool>(&value.data)) std::cout << (*b ? "true" : "false") << '\n';
    else if (const auto *i = std::get_if<std::int64_t>(&value.data)) std::cout << *i << '\n';
    else if (const auto *u = std::get_if<std::uint64_t>(&value.data)) std::cout << *u << '\n';
    else if (const auto *d = std::get_if<double>(&value.data)) std::cout << *d << '\n';
    else if (const auto *s = std::get_if<std::string>(&value.data)) std::cout << '"' << *s << "\"\n";
    else if (const auto *r = std::get_if<ebx::PointerReference>(&value.data))
        std::cout << (r->kind == ebx::PointerKind::null ? "null" : r->kind == ebx::PointerKind::internal ? "-> instance " : "-> import ") << r->index << '\n';
    else if (const auto *x = std::get_if<ebx::BoxedReference>(&value.data)) std::cout << "boxed type 0x" << std::hex << x->encodedType << std::dec << " at " << x->dataOffset << '\n';
    else if (const auto *y = std::get_if<ebx::TypeReference>(&value.data)) std::cout << "type " << y->descriptor << " 0x" << std::hex << y->encoded << std::dec << '\n';
    else if (const auto *o = std::get_if<std::shared_ptr<ebx::Object>>(&value.data)) {
        std::cout << "{\n";
        if (*o) tree(**o, depth + 1, indent + "  ");
        std::cout << indent << "}\n";
    } else if (const auto *a = std::get_if<ebx::Value::Array>(&value.data)) {
        std::cout << "[" << a->size() << "]\n";
        std::size_t shown{};
        for (const auto &element : *a) {
            if (++shown > 40) { std::cout << indent << "  ...\n"; break; }
            std::cout << indent << "  - ";
            tree(element, depth + 1, indent + "    ");
        }
    } else std::cout << "(other)\n";
}
} // namespace

namespace {
class MeshMetadataOutput {
public:
    static constexpr std::size_t maximum_bytes = 1024 * 1024, maximum_records = 8192;
    static constexpr std::size_t maximum_text = 512, summary_reserve = 1024;

    explicit MeshMetadataOutput(std::ostream& output = std::cout) : output_(output) {}

    std::string text(std::string_view source) {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(maximum_text);
        for (const unsigned char byte : source) {
            const bool plain = byte >= 0x20 && byte <= 0x7e && byte != '\\';
            const std::size_t width = plain ? 1 : byte == '\\' ? 2 : 4;
            if (width > maximum_text - result.size()) { truncated_ = true; break; }
            if (plain) result += static_cast<char>(byte);
            else if (byte == '\\') result += "\\\\";
            else { result += "\\x"; result += digits[byte >> 4]; result += digits[byte & 15]; }
        }
        return result;
    }
    template <class... Values> bool record(const Values&... values) {
        if (stopped_ || finished_) return false;
        std::ostringstream line;
        (line << ... << values);
        const auto value = line.str();
        constexpr auto record_limit = maximum_bytes - summary_reserve;
        std::size_t encoded_size{};
        if (records_ >= maximum_records - 1 || bytes_ > record_limit ||
            !record_size(value, record_limit - bytes_, encoded_size)) {
            truncated_ = stopped_ = true;
            return false;
        }
        write(value, encoded_size);
        return true;
    }
    void mark_truncated() noexcept { truncated_ = true; }
    bool open() const noexcept { return !stopped_ && !finished_; }
    int finish(bool complete, int failure_code = 1) {
        const bool success = complete && !truncated_ && static_cast<bool>(output_);
        std::ostringstream line;
        line << "SUMMARY\tmode=mesh-metadata\tcomplete=" << (success ? 1 : 0)
             << "\toutput_truncated=" << (truncated_ ? 1 : 0) << "\trecords_before_summary=" << records_
             << "\tbytes_before_summary=" << bytes_ << "\tsuccess_scope=resource_metadata_only"
             << "\tgeometry_payload_read=0\tdependency_closure=unverified";
        const bool reported = summary(line.str(),
            "SUMMARY\tmode=mesh-metadata\tcomplete=0\toutput_truncated=1\terror=summary_budget_exceeded");
        return success && reported ? 0 : failure_code;
    }
private:
    // Include the newline appended by write(), then account for any LFs inside the formatted value.
    static bool record_size(std::string_view value, std::size_t available, std::size_t& size) noexcept {
#ifdef _WIN32
        constexpr std::size_t newline_bytes = 2;
#else
        constexpr std::size_t newline_bytes = 1;
#endif
        if (available < newline_bytes || value.size() > available - newline_bytes) return false;
        size = value.size() + newline_bytes;
#ifdef _WIN32
        for (const char byte : value) {
            if (byte != '\n') continue;
            if (size == available) return false;
            ++size;
        }
#endif
        return true;
    }
    void write(std::string_view value, std::size_t encoded_size) {
        // Unformatted output cannot add padding from the caller's stream width or flags.
        output_.write(value.data(), static_cast<std::streamsize>(value.size()));
        output_.put('\n');
        bytes_ += encoded_size;
        ++records_;
    }
    bool summary(std::string_view value, std::string_view fallback) {
        if (finished_) return false;
        finished_ = stopped_ = true;
        std::size_t encoded_size{};
        if (bytes_ > maximum_bytes || !record_size(value, summary_reserve, encoded_size) ||
            encoded_size > maximum_bytes - bytes_) {
            truncated_ = true;
            if (bytes_ <= maximum_bytes && record_size(fallback, summary_reserve, encoded_size) &&
                encoded_size <= maximum_bytes - bytes_) write(fallback, encoded_size);
            return false;
        }
        write(value, encoded_size);
        return static_cast<bool>(output_);
    }

    std::ostream& output_;
    std::size_t bytes_{}, records_{};
    bool truncated_{}, stopped_{}, finished_{};
};

std::string metadata_hex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}
bool mesh_metadata_lod(std::string_view text, std::size_t& value) {
    if (text.empty()) return false;
    value = 0;
    for (const unsigned char byte : text) {
        if (byte < '0' || byte > '9') return false;
        const auto digit = static_cast<std::size_t>(byte - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}
bool mesh_metadata_name(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) return false;
    return true;
}

void mesh_metadata_rows(MeshMetadataOutput& out, const dingosdk::frostbite::MeshLodMetadata& mesh) {
    if (mesh.lod_kind_supported) {
        out.record("LOD\tlayout=", mesh.skinned ? "skinned" : "static", "\tcount=", mesh.lod_count,
                   "\tindex=", mesh.lod_index, "\tresource_offset=", mesh.lod_offset, "\tkind_raw=", mesh.lod_kind,
                   "\tkind_supported=1\tindex_format_raw=", mesh.index_format, "\tindex_width=", mesh.index_format == 33 ? "2" : "unknown",
                   "\tvertex_bytes=", mesh.geometry.vertex_bytes, "\tindex_bytes=", mesh.geometry.index_bytes,
                   "\tchunk_guid=", mesh.geometry.chunk.string(), "\tsection_count=", mesh.section_count,
                   "\tsection_table_offset=", mesh.section_table, "\tstride_ambiguous=", mesh.ambiguous ? 1 : 0,
                   "\tparser_complete=", mesh.complete ? 1 : 0);
    } else {
        // Later fields have not been interpreted for this LOD kind; zero-initialized members are not observations.
        out.record("LOD\tlayout=", mesh.skinned ? "skinned" : "static", "\tcount=", mesh.lod_count,
                   "\tindex=", mesh.lod_index, "\tresource_offset=", mesh.lod_offset, "\tkind_raw=", mesh.lod_kind,
                   "\tkind_supported=0\tparser_complete=0\tinterpretation=unsupported_lod_kind");
    }
    for (const auto& diagnostic : mesh.diagnostics) {
        if (!out.open()) break;
        out.record("DIAGNOSTIC\tscope=lod\tmessage=", out.text(diagnostic));
    }
    if (!mesh.lod_kind_supported) {
        if (mesh.static_common_offsets) {
            const auto& observed = *mesh.static_common_offsets;
            out.record("STATIC_COMMON_OFFSET\tlod_relative_offset=8\tresource_offset=", mesh.lod_offset + 8,
                       "\twidth=4\traw_value=", observed.section_count_raw, "\tcandidate_role=section_count",
                       "\tbasis=existing_static_reader\tinterpretation=unverified_common_layout");
            out.record("STATIC_COMMON_OFFSET\tlod_relative_offset=12\tresource_offset=", mesh.lod_offset + 12,
                       "\twidth=8\traw_value=", observed.section_pointer_raw, "\tcandidate_role=section_pointer",
                       "\tbasis=existing_static_reader\tinterpretation=unverified_common_layout");
            out.record("STATIC_COMMON_OFFSET\tlod_relative_offset=84\tresource_offset=", mesh.lod_offset + 84,
                       "\twidth=4\traw_value=", observed.index_format_raw, "\tcandidate_role=index_format",
                       "\tbasis=existing_static_reader\tinterpretation=unverified_common_layout");
            out.record("STATIC_COMMON_OFFSET\tlod_relative_offset=88\tresource_offset=", mesh.lod_offset + 88,
                       "\twidth=4\traw_value=", observed.index_bytes_raw, "\tcandidate_role=index_bytes",
                       "\tbasis=existing_static_reader\tinterpretation=unverified_common_layout");
            out.record("STATIC_COMMON_OFFSET\tlod_relative_offset=92\tresource_offset=", mesh.lod_offset + 92,
                       "\twidth=4\traw_value=", observed.vertex_bytes_raw, "\tcandidate_role=vertex_bytes",
                       "\tbasis=existing_static_reader\tinterpretation=unverified_common_layout");
            out.record("STATIC_COMMON_OFFSET\tlod_relative_offset=116\tresource_offset=", mesh.lod_offset + 116,
                       "\twidth=16\traw_bytes_hex=", metadata_hex(observed.geometry_identity_raw), "\tcandidate_role=geometry_identity",
                       "\tbasis=existing_static_reader\tinterpretation=unverified_common_layout");
            out.record("STATIC_SECTION_LIMITS\tsection_count_limit=64\tcombined_candidate_span_bytes_limit=32768"
                       "\telement_count_limit=16\tstream_count_limit=16\tdiagnostics_per_candidate_limit=16\tlimits_provenance=authored");
            for (std::size_t candidate_index = 0; candidate_index < mesh.static_section_candidates.size() && out.open(); ++candidate_index) {
                const auto& candidate = mesh.static_section_candidates[candidate_index];
                const bool diagnostics_truncated = candidate.diagnostic_count > candidate.diagnostics.size();
                if (diagnostics_truncated) out.mark_truncated();
                out.record("STATIC_SECTION_CANDIDATE\tindex=", candidate_index, "\tsection_stride=", candidate.stride,
                           "\ttable_pointer_resolved=", candidate.table_pointer_resolved ? 1 : 0,
                           candidate.table_pointer_resolved ? "\ttable_offset=" + std::to_string(candidate.table_offset) : std::string{},
                           "\ttable_span_valid=", candidate.table_span_valid ? 1 : 0,
                           "\tcandidate_span_bytes_available=", candidate.table_span_valid ? 1 : 0,
                           candidate.table_span_valid ? "\tcandidate_span_bytes=" + std::to_string(candidate.span_bytes) : std::string{},
                           "\tobservation_complete=", candidate.observation_complete ? 1 : 0,
                           "\tsections_reported=", candidate.sections.size(), "\tdiagnostic_count=", candidate.diagnostic_count,
                           "\tdiagnostics_truncated=", diagnostics_truncated ? 1 : 0,
                           "\tbasis=existing_mesh_reader_offsets\tinterpretation=unverified_static_section_layout");
                for (const auto& diagnostic : candidate.diagnostics) {
                    if (!out.open()) break;
                    out.record("DIAGNOSTIC\tscope=static_section_candidate\tcandidate=", candidate_index,
                               "\tmessage=", out.text(diagnostic));
                }
                for (std::size_t section_index = 0; section_index < candidate.sections.size() && out.open(); ++section_index) {
                    const auto& section = candidate.sections[section_index];
                    out.record("STATIC_SECTION_RAW\tcandidate=", candidate_index, "\tindex=", section_index,
                               "\tresource_offset=", section.resource_offset,
                               "\tprimitive_relative_offset=31\tprimitive_raw=", static_cast<unsigned>(section.primitive_raw),
                               "\ttriangle_count_relative_offset=32\ttriangle_count_raw=", section.triangle_count_raw,
                               "\tfirst_index_relative_offset=36\tfirst_index_raw=", section.first_index_raw,
                               "\tvertex_start_relative_offset=40\tvertex_start_raw=", section.vertex_start_raw,
                               "\tvertex_count_relative_offset=44\tvertex_count_raw=", section.vertex_count_raw,
                               "\telement_count_relative_offset=208\telement_count_raw=", static_cast<unsigned>(section.element_count_raw),
                               "\tstream_count_relative_offset=209\tstream_count_raw=", static_cast<unsigned>(section.stream_count_raw),
                               "\tdeclarations_observed=", section.declarations_observed ? 1 : 0,
                               "\tbasis=existing_mesh_reader_offsets\tinterpretation=unverified_static_section_layout");
                    for (std::size_t element_index = 0; element_index < section.elements.size() && out.open(); ++element_index) {
                        const auto& element = section.elements[element_index];
                        const auto relative_offset = 112 + 4 * element_index;
                        out.record("STATIC_ELEMENT_RAW\tcandidate=", candidate_index, "\tsection=", section_index, "\tindex=", element_index,
                                   "\tsection_relative_offset=", relative_offset, "\tresource_offset=", section.resource_offset + relative_offset,
                                   "\tusage_raw=", static_cast<unsigned>(element.usage_raw), "\tformat_raw=", static_cast<unsigned>(element.format_raw),
                                   "\toffset_raw=", static_cast<unsigned>(element.offset_raw), "\tstream_raw=", static_cast<unsigned>(element.stream_raw),
                                   "\tbasis=existing_mesh_reader_offsets\tinterpretation=unverified_static_section_layout");
                    }
                    for (std::size_t stream_index = 0; stream_index < section.streams.size() && out.open(); ++stream_index) {
                        const auto& stream = section.streams[stream_index];
                        const auto relative_offset = 176 + 2 * stream_index;
                        out.record("STATIC_STREAM_RAW\tcandidate=", candidate_index, "\tsection=", section_index, "\tindex=", stream_index,
                                   "\tsection_relative_offset=", relative_offset, "\tresource_offset=", section.resource_offset + relative_offset,
                                   "\tstride_raw=", static_cast<unsigned>(stream.stride_raw), "\tclassification_raw=", static_cast<unsigned>(stream.classification_raw),
                                   "\tbasis=existing_mesh_reader_offsets\tinterpretation=unverified_static_section_layout");
                    }
                }
            }
        }
        return;
    }
    for (std::size_t candidate_index = 0; candidate_index < mesh.candidates.size() && out.open(); ++candidate_index) {
        const auto& candidate = mesh.candidates[candidate_index];
        if (candidate.diagnostic_count > candidate.diagnostics.size()) out.mark_truncated();
        out.record("CANDIDATE\tindex=", candidate_index, "\tsection_stride=", candidate.stride,
                   "\tstructurally_valid=", candidate.structurally_valid ? 1 : 0,
                   "\tdeclarations_supported=", candidate.declarations_supported ? 1 : 0,
                   "\tsections_reported=", candidate.sections.size(), "\tdiagnostic_count=", candidate.diagnostic_count,
                   "\tdiagnostics_truncated=", candidate.diagnostic_count > candidate.diagnostics.size() ? 1 : 0);
        for (const auto& diagnostic : candidate.diagnostics) {
            if (!out.open()) break;
            out.record("DIAGNOSTIC\tcandidate=", candidate_index, "\tmessage=", out.text(diagnostic));
        }
        for (std::size_t section_index = 0; section_index < candidate.sections.size() && out.open(); ++section_index) {
            const auto& section = candidate.sections[section_index];
            out.record("SECTION\tcandidate=", candidate_index, "\tindex=", section_index,
                       "\tresource_offset=", section.resource_offset, "\tprimitive_raw=", static_cast<unsigned>(section.primitive),
                       "\ttriangles=", section.triangle_count, "\tfirst_index=", section.first_index,
                       "\tvertex_start=", section.vertex_start, "\tvertex_count=", section.vertex_count,
                       "\telement_count=", static_cast<unsigned>(section.element_count),
                       "\tstream_count=", static_cast<unsigned>(section.stream_count),
                       "\tvertex_range_checked=", section.vertex_range_checked ? 1 : 0,
                       "\tindex_range_checked=", section.index_range_checked ? 1 : 0);
            for (std::size_t element_index = 0; element_index < section.elements.size() && out.open(); ++element_index) {
                const auto& element = section.elements[element_index];
                out.record("ELEMENT\tcandidate=", candidate_index, "\tsection=", section_index, "\tindex=", element_index,
                           "\tusage_raw=", static_cast<unsigned>(element.usage), "\tformat_raw=", static_cast<unsigned>(element.format),
                           "\toffset=", static_cast<unsigned>(element.offset), "\tstream=", static_cast<unsigned>(element.stream),
                           "\tknown_width=", static_cast<unsigned>(element.width), "\tsemantic_supported=", element.semantic_supported ? 1 : 0);
            }
            for (std::size_t stream_index = 0; stream_index < section.streams.size() && out.open(); ++stream_index) {
                const auto& stream = section.streams[stream_index];
                out.record("STREAM\tcandidate=", candidate_index, "\tsection=", section_index, "\tindex=", stream_index,
                           "\tstride=", static_cast<unsigned>(stream.stride),
                           "\tclassification_raw=", static_cast<unsigned>(stream.classification));
            }
        }
    }
}

int mesh_metadata_command(int argc, char** argv) {
    using namespace dingosdk::frostbite;
    MeshMetadataOutput out;
    std::size_t lod{};
    // A malformed request never opens GameData or a TOC. The exact resource still comes from the selected manifest.
    if (argc != 5 || std::string_view(argv[3]).empty() || std::string_view(argv[3]).size() > 4096 ||
        !mesh_metadata_lod(argv[4], lod)) {
        out.record("ERROR\tstage=arguments\tmessage=expected one exact RES name and an unsigned decimal LOD index");
        return out.finish(false, 2);
    }
    constexpr std::size_t resource_limit = 16 * 1024 * 1024;
    constexpr std::uint32_t mesh_set_type = 0x49b156d4; // Existing MeshSet RES type used by the VFS mesh readers.
    std::string_view stage = "context";
    try {
        const auto* toc_environment = std::getenv("ASSET_TOC");
        const auto* bundle_environment = std::getenv("ASSET_BUNDLE");
        const std::string_view toc_name = toc_environment ? toc_environment : "Win32/levels/game/bam_levelroot/bam_levelroot.toc";
        const std::string_view bundle_name = bundle_environment ? bundle_environment : "win32/levels/game/bam_levelroot/bam_levelroot";
        if (toc_name.empty() || bundle_name.empty() || toc_name.size() > 4096 || bundle_name.size() > 4096 || bundle_name == "?") {
            out.record("ERROR\tstage=arguments\tmessage=mesh metadata needs one bounded TOC and bundle selection, not a bundle listing");
            return out.finish(false, 2);
        }
        out.record("CONTEXT\ttoc=", out.text(toc_name), "\tbundle=", out.text(bundle_name),
                   "\tstore_classification=unknown\tresidency=manifest_only\tgeometry_payload_read=0\toutput_bytes_limit=1048576"
                   "\toutput_records_limit=8192\tescaped_text_bytes_limit=512\tresource_tool_limit_bytes=16777216"
                   "\tdecoder_output_limit_bytes=268435456\tlimits_provenance=authored");
        stage = "bundle_read";
        const dingosdk::vfs::GameData data{std::filesystem::path(argv[1])};
        const auto toc = data.read_toc(toc_name);
        const auto found = data.read_bundle(toc, bundle_name);
        if (!found) throw std::runtime_error("selected bundle was not found");
        const auto& bundle = *found;
        std::size_t matches{}, resource_index{};
        for (std::size_t i = 0; i < bundle.manifest.resources.size(); ++i)
            if (mesh_metadata_name(bundle.manifest.resources[i].name, argv[3])) { ++matches; resource_index = i; }
        out.record("SELECTION\texact_res_name=", out.text(argv[3]), "\tlod=", lod, "\tmatched_resources=", matches);
        if (matches != 1) {
            out.record("ERROR\tstage=resource_selection\tmessage=", matches ? "exact RES name is ambiguous" : "exact RES name was not found in this bundle");
            return out.finish(false);
        }
        const auto& asset = bundle.manifest.resources[resource_index];
        const auto* payload = bundle.payload(AssetKind::resource, resource_index);
        out.record("RESOURCE\tname=", out.text(asset.name), "\tresource_type_raw=", asset.resourceType,
                   "\tresource_id=", asset.resourceId, "\tsha1=", metadata_hex(asset.sha1.bytes),
                   "\tsha1_source=manifest\tpayload_sha1_verified=0\tadvertised_bytes=", asset.originalSize,
                   "\tpayload_location=", payload ? "present" : "missing");
        if (asset.resourceType != mesh_set_type || !payload) {
            out.record("ERROR\tstage=resource_selection\tmessage=", payload ? "selected RES is not a MeshSet" : "selected RES has no payload location record");
            return out.finish(false);
        }
        out.record("PAYLOAD\tcompressed_bytes=", payload->size,
                   "\tdecoder_policy=existing_GameData_256MiB_cap\ttool_size_checks=advertised_compressed_then_actual");
        if (!asset.originalSize || asset.originalSize > resource_limit || !payload->size || payload->size > resource_limit) {
            out.record("ERROR\tstage=resource_size\tmessage=advertised or compressed resource size is outside the tool limit");
            return out.finish(false);
        }
        stage = "resource_read";
        // GameData retains its existing 256 MiB decode cap; the manifest precheck is not a smaller decoder quota.
        const auto bytes = data.read(*payload);
        out.record("RESOURCE_READ\tactual_bytes=", bytes.size(), "\tgeometry_payload_read=0");
        if (bytes.size() > resource_limit || bytes.size() != asset.originalSize) {
            out.record("ERROR\tstage=resource_size\tmessage=decoded size exceeds the tool limit or differs from the manifest");
            return out.finish(false);
        }
        if (bytes.size() >= 16) {
            BinaryReader header(bytes);
            const auto first = header.u32(), second = header.u32(), third = header.u32(), fourth = header.u32();
            out.record("HEADER\tword0=", first, "\tword1=", second, "\tword2=", third, "\tword3=", fourth);
        }
        stage = "mesh_metadata";
        const auto mesh = read_mesh_lod_metadata(bytes, lod);
        mesh_metadata_rows(out, mesh);
        if (!mesh.lod_kind_supported) return out.finish(false);
        std::size_t chunk_matches{}, chunk_index{};
        for (std::size_t i = 0; i < bundle.manifest.chunks.size(); ++i)
            if (bundle.manifest.chunks[i].guid == mesh.geometry.chunk) { ++chunk_matches; chunk_index = i; }
        out.record("CHUNK_REF\tguid=", mesh.geometry.chunk.string(), "\tlocal_matches=", chunk_matches,
                   "\tresolution=", chunk_matches == 1 ? "selected_bundle_manifest" : chunk_matches ? "ambiguous" : "external_or_missing",
                   "\tpayload_read=0");
        bool chunk_location{};
        if (chunk_matches == 1) {
            const auto& chunk = bundle.manifest.chunks[chunk_index];
            chunk_location = bundle.payload(AssetKind::chunk, chunk_index) != nullptr;
            out.record("CHUNK_METADATA\tguid=", chunk.guid.string(), "\tsha1=", metadata_hex(chunk.sha1.bytes),
                       "\tsha1_source=manifest\tadvertised_bytes=", chunk.originalSize, "\tlogical_offset=", chunk.logicalOffset,
                       "\tlogical_size=", chunk.logicalSize, "\tpayload_location=", chunk_location ? "present" : "missing", "\tpayload_read=0");
        }
        if (chunk_matches != 1 || !chunk_location)
            out.record("DIAGNOSTIC\tscope=chunk_manifest\tmessage=unique local geometry location is unresolved; global absence is not established");
        return out.finish(mesh.complete && chunk_matches == 1 && chunk_location);
    } catch (const std::exception& error) {
        out.record("ERROR\tstage=", stage, "\tmessage=", out.text(error.what()));
        return out.finish(false);
    }
}
}

int main(int argc, char **argv) {
    if (argc >= 3 && std::string_view(argv[2]) == "--mesh-metadata") return mesh_metadata_command(argc, argv);
    if (argc < 3) {
        std::cerr << "usage: dingosdk_trainer_asset_list <Skate folder> <word>... | --dump <asset name> | --types <word>...\n";
        return 2;
    }
    try {
        const std::filesystem::path game_root(argv[1]);
        const dingosdk::vfs::GameData data(game_root);
        // ASSET_TOC / ASSET_BUNDLE: another superbundle and bundle than the level root; ASSET_BUNDLE=? lists the names.
        const auto *toc_name = std::getenv("ASSET_TOC");
        const auto *bundle_name = std::getenv("ASSET_BUNDLE");
        const auto toc = data.read_toc(toc_name ? toc_name : "Win32/levels/game/bam_levelroot/bam_levelroot.toc");
        if (bundle_name && std::string_view(bundle_name) == "?") {
            for (const auto &entry : toc.bundles) std::cout << entry.name << std::endl;
            return 0;
        }
        const auto found = data.read_bundle(toc, bundle_name ? bundle_name : "win32/levels/game/bam_levelroot/bam_levelroot");
        if (!found) throw std::runtime_error("the level root bundle was not found");
        const auto &bundle = *found;
        const std::string mode = argv[2];
        if (mode == "--floats" && argc >= 4) {
            // Every aligned four bytes of the asset's data that read as a plausible number.
            const auto wanted = lower(argv[3]);
            for (std::size_t i = 0; i < bundle.manifest.ebx.size(); ++i) {
                if (lower(bundle.manifest.ebx[i].name) != wanted) continue;
                const auto *payload = bundle.payload(dingosdk::frostbite::AssetKind::ebx, i);
                if (!payload) break;
                const auto bytes = data.read(*payload);
                const auto document = ebx::read_document(bytes);
                std::cerr << document.rootType << ": data " << document.dataStart << ".." << document.dataEnd << " of " << bytes.size() << "\n";
                for (std::size_t at = document.dataStart & ~std::size_t{3}; at + 4 <= bytes.size(); at += 4) {
                    float value;
                    std::memcpy(&value, bytes.data() + at, 4);
                    const float size = value < 0 ? -value : value;
                    if (size > 0.0009f && size < 100000.0f) std::cout << "0x" << std::hex << at << std::dec << "\t" << value << "\n";
                }
                return 0;
            }
            return 1;
        }
        if (mode == "--tree" && argc >= 4) {
            const auto wanted = lower(argv[3]);
            for (std::size_t i = 0; i < bundle.manifest.ebx.size(); ++i) {
                if (lower(bundle.manifest.ebx[i].name) != wanted) continue;
                const auto *payload = bundle.payload(dingosdk::frostbite::AssetKind::ebx, i);
                if (!payload) break;
                const auto bytes = data.read(*payload);
                const auto document = ebx::read_document(bytes);
                std::size_t number{};
                for (const auto &instance : document.instances) {
                    const auto type = instance.descriptor >= 0 && static_cast<std::size_t>(instance.descriptor) < document.types.size()
                        ? document.types[static_cast<std::size_t>(instance.descriptor)].name : std::string("?");
                    std::cout << "# instance " << number++ << " " << type << "\n";
                    if (instance.object) tree(*instance.object, 0, "  ");
                }
                number = 0;
                for (const auto &boxed : document.boxedValues) {
                    std::cout << "# boxed " << number++ << ": type 0x" << std::hex << boxed.encodedType << std::dec << " class " << boxed.classRef
                              << " count " << boxed.count << " offset " << boxed.offset << " bytes " << boxed.rawBytes.size() << " :";
                    for (std::size_t at = 0; at + 4 <= boxed.rawBytes.size() && at < 256; at += 4) {
                        float real; std::int32_t whole;
                        std::memcpy(&real, boxed.rawBytes.data() + at, 4);
                        std::memcpy(&whole, boxed.rawBytes.data() + at, 4);
                        const float size = real < 0 ? -real : real;
                        if (size > 0.0001f && size < 1.0e6f) std::cout << ' ' << real; else std::cout << " #" << whole;
                    }
                    std::cout << "\n";
                }
                for (const auto &import : document.imports) (void)import;
                std::cout << "# imports " << document.imports.size() << ", arrays " << document.arrays.size() << ", types";
                for (const auto &type : document.types) std::cout << ' ' << type.name;
                std::cout << "\n";
                return 0;
            }
            return 1;
        }
        if (mode == "--export" && argc >= 5) {
            // The raw bytes of every asset whose name contains a word, as <folder>/<number>.ebx,
            // with one line each: number, name, file id as hex, the ids it imports.
            const std::filesystem::path folder(argv[3]);
            std::filesystem::create_directories(folder);
            std::vector<std::string> wanted;
            for (int i = 4; i < argc; ++i) wanted.push_back(lower(argv[i]));
            const auto hex = [](const dingosdk::frostbite::Guid &guid) {
                static const char digits[] = "0123456789abcdef";
                std::string text;
                for (const auto byte : guid.bytes) {
                    text += digits[static_cast<unsigned>(byte) >> 4];
                    text += digits[static_cast<unsigned>(byte) & 15];
                }
                return text;
            };
            for (std::size_t i = 0; i < bundle.manifest.ebx.size(); ++i) {
                const auto name = lower(bundle.manifest.ebx[i].name);
                if (std::ranges::none_of(wanted, [&](const std::string &w) { return name.find(w) != std::string::npos; })) continue;
                try {
                    const auto *payload = bundle.payload(dingosdk::frostbite::AssetKind::ebx, i);
                    if (!payload) continue;
                    const auto bytes = data.read(*payload);
                    const auto document = ebx::read_document(bytes);
                    std::ofstream(folder / (std::to_string(i) + ".ebx"), std::ios::binary)
                        .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    std::cout << i << '\t' << name << '\t' << hex(document.fileGuid) << '\t' << document.rootType << '\t';
                    for (const auto &import : document.imports) std::cout << hex(import.fileGuid) << ' ';
                    std::cout << '\n';
                } catch (const std::exception &) {}
            }
            return 0;
        }
        if (mode == "--dump" && argc >= 4) {
            const auto wanted = lower(argv[3]);
            for (std::size_t i = 0; i < bundle.manifest.ebx.size(); ++i) {
                if (lower(bundle.manifest.ebx[i].name) != wanted) continue;
                const auto *payload = bundle.payload(dingosdk::frostbite::AssetKind::ebx, i);
                if (!payload) break;
                const auto bytes = data.read(*payload);
                const auto document = ebx::read_document(bytes);
                std::cerr << bundle.manifest.ebx[i].name << ": root type " << document.rootType << ", " << document.instances.size()
                          << " instances\n";
                // Every instance: an asset keeps its lists and sub-objects as separate ones.
                std::size_t number{};
                for (const auto &instance : document.instances) {
                    if (instance.descriptor < 0 || static_cast<std::size_t>(instance.descriptor) >= document.types.size()) continue;
                    const auto &type = document.types[static_cast<std::size_t>(instance.descriptor)];
                    std::cout << "# instance " << number++ << " " << type.name << " (" << instance.rawImage.size() << " bytes)\n";
                    walk(document, type, instance.rawImage, 0, 0, {});
                }
                return 0;
            }
            std::cerr << "no such asset\n";
            return 1;
        }
        std::vector<std::string> words;
        for (int i = (mode == "--types" ? 3 : 2); i < argc; ++i) words.push_back(lower(argv[i]));
        std::size_t shown{};
        for (std::size_t i = 0; i < bundle.manifest.ebx.size(); ++i) {
            const auto name = lower(bundle.manifest.ebx[i].name);
            std::string type;
            bool match = mode != "--types" && std::ranges::any_of(words, [&](const std::string &w) { return name.find(w) != std::string::npos; });
            if (mode == "--types") {
                // Slower: opens every asset to learn its root type.
                try {
                    if (const auto *payload = bundle.payload(dingosdk::frostbite::AssetKind::ebx, i)) {
                        type = ebx::read_document(data.read(*payload)).rootType;
                        const auto low = lower(type);
                        match = std::ranges::any_of(words, [&](const std::string &w) { return low.find(w) != std::string::npos; });
                    }
                } catch (const std::exception &) {}
            }
            if (!match) continue;
            std::cout << bundle.manifest.ebx[i].name << (type.empty() ? "" : "\t" + type) << '\n';
            ++shown;
        }
        std::cerr << shown << " of " << bundle.manifest.ebx.size() << " assets\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
