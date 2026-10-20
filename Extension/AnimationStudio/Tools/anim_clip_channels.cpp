// Offline adapter for the Studio CLI's expanded transform channels. Never accesses game memory.
#include "Engine/Core/Json/json.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/ebx_fields.h"
#include "Engine/Resource/ebx_writer.h"
#include "Extension/AnimationStudio/anim_core.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace ebx = dingosdk::frostbite::ebx;
using dingosdk::Json;
std::vector<std::byte> read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() <= 0 || in.tellg() > 64 * 1024 * 1024) throw std::runtime_error("Invalid input size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0); in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!in) throw std::runtime_error("Cannot read input");
    return bytes;
}
ebx::Value& field(ebx::Object& object, std::string_view name) {
    for (auto& f : object.fields) if (f.name == name) return f.value;
    throw std::runtime_error("Missing field: " + std::string(name));
}
void write(const std::filesystem::path& path, const ebx::Document& doc) {
    const auto bytes = ebx::write_document(doc, true);
    (void)ebx::read_document(bytes); // Refuse an invalid serialized result before publishing it.
    std::ofstream out(path, std::ios::binary); out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--sample-edits") {
            namespace studio = dingosdk::anim_studio;
            const auto bytes = read(argv[2]), query = read(argv[3]);
            const auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            const auto animation = studio::from_json(text);
            const auto profile = Json::parse(text);
            const auto times = Json::parse(std::string_view(reinterpret_cast<const char*>(query.data()), query.size()));
            if (animation.source.kind != "native_asset" || animation.source.rig != "animation/dingo/animbase_default_skeleton" ||
                (animation.stance != "" && animation.stance != "regular") ||
                animation.switch_stance.value_or(false) || animation.speed != 1 || !times.is_array() || times.size() > 20000)
                throw std::runtime_error("Native baking needs a canonical native source, original speed and bounded sample times");
            const auto duration = profile.at("duration").get<float>();
            Json tracks = Json::array();
            for (const auto& track : animation.tracks) {
                if (track.hidden || track.keys.empty()) continue;
                Json samples = Json::array();
                float previous = -1;
                for (const auto& value : times) {
                    const auto time = value.get<float>();
                    if (!std::isfinite(time) || time < previous || time < 0 || time > duration + 0.001f)
                        throw std::runtime_error("Invalid source sample time");
                    previous = time;
                    const auto edit = time >= animation.start && (animation.end < 0 || time <= animation.end)
                        ? studio::evaluate(track, time) : studio::Offset{};
                    samples.push_back({{"r", edit.rotation}, {"p", edit.offset}});
                }
                tracks.push_back({{"joint", track.joint}, {"samples", std::move(samples)}});
            }
            std::ofstream out(argv[4]); out.exceptions(std::ios::badbit | std::ios::failbit);
            out << Json{{"source_asset", animation.source.asset}, {"duration", duration}, {"tracks", std::move(tracks)}}.dump(2);
            return 0;
        }
        if (argc != 6) throw std::runtime_error("Usage: anim_clip_channels channel.ebx controller.ebx selection.json out-channel.ebx out-controller.ebx");
        auto channels = ebx::read_document(read(argv[1]));
        auto controller = ebx::read_document(read(argv[2]));
        const auto request = read(argv[3]);
        const auto selection = Json::parse(std::string_view(reinterpret_cast<const char*>(request.data()), request.size()));
        if (channels.rootType != "ChannelToDofAsset" || controller.rootType != "ClipControllerAsset" || channels.instances.size() != 1)
            throw std::runtime_error("Unsupported animation documents");
        auto& ids = field(*channels.instances[0].object, "ChannelToDofIds");
        const auto original = ebx::as<ebx::Value::Array>(ids, "Invalid channel IDs");
        ebx::Value::Array filtered;
        std::set<std::uint64_t> seen;
        for (const auto& value : selection.at("ids")) {
            const auto wanted = value.get<std::uint64_t>();
            if (!seen.insert(wanted).second) throw std::runtime_error("Duplicate requested channel");
            const ebx::Value* found = nullptr;
            for (const auto& item : original) if (ebx::as<std::uint64_t>(item, "Invalid channel ID") == wanted) {
                if (found) throw std::runtime_error("Duplicate imported channel");
                found = &item;
            }
            if (!found) throw std::runtime_error("Requested channel is missing");
            filtered.push_back(*found);
        }
        const auto q = selection.at("q").get<std::uint32_t>(), v = selection.at("v").get<std::uint32_t>(), f = selection.at("f").get<std::uint32_t>();
        if (filtered.empty() || filtered.size() != std::size_t(q) + v + f) throw std::runtime_error("Channel count mismatch");
        ids.data = std::move(filtered);
        unsigned animations = 0;
        for (auto& instance : controller.instances) {
            if (!instance.object || controller.types.at(instance.object->descriptor).name != "EclipseAnimationAsset") continue;
            ++animations;
            // Keep resource identity, codec, tags, trajectory and timing exactly as imported.
            for (const auto& [name, value] : {std::pair{"QuaternionChannelSize", q}, {"VectorChannelSize", v}, {"FloatChannelSize", f}}) {
                auto& item = field(*instance.object, name);
                (void)ebx::as<std::uint64_t>(item, "Invalid channel count"); item.data = std::uint64_t(value);
            }
        }
        if (animations != 1) throw std::runtime_error("Expected one animation payload");
        write(argv[4], channels); write(argv[5], controller);
        std::cout << "Preserved " << q << " quaternion, " << v << " vector, " << f << " scalar channels\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
