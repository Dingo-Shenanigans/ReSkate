#include "game_faces.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/ui_textures.h"
#include "Engine/Vfs/content_cache.h"
#include "Engine/Vfs/game_bundles.h"
#include "Extension/Plugins/plugin_ui.h"
#include "Extension/Skater/xray_skeleton.h"
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace dingosdk::overlay::game_faces {
namespace {
namespace ui = addr::ui_textures;
namespace fb = dingosdk::frostbite;
namespace fs = std::filesystem;
static_assert(ui::faces.size() == plugin_ui::game_faces.size());
std::atomic<bool> started{};

// A TrueType file (its version 1.0) or an OpenType one ("OTTO").
bool font_file(const std::vector<std::byte>& bytes) {
    return bytes.size() >= 12 && ((bytes[0] == std::byte{0} && bytes[1] == std::byte{1}) || (bytes[0] == std::byte{'O'} && bytes[1] == std::byte{'T'}));
}
bool kept(const fs::path& file) {
    std::error_code error;
    return fs::is_regular_file(file, error) && fs::file_size(file, error) > 12;
}
void read_faces() {
    std::vector<fs::path> files;
    bool all = true;
    for (std::size_t face = 0; face < ui::faces.size(); ++face) {
        files.push_back(file(face));
        if (files.back().empty()) return;
        all = all && kept(files.back());
    }
    if (all) return;
    vfs::GameData data(xray::game_folder());
    const auto toc = data.read_toc(ui::face_toc);
    const auto bundle = data.read_bundle(toc, ui::face_bundle);
    if (!bundle) throw std::runtime_error("the bundle of its faces is missing");
    std::error_code error;
    fs::create_directories(files.front().parent_path(), error);
    for (std::size_t face = 0; face < ui::faces.size(); ++face) {
        if (kept(files[face])) continue;
        std::size_t index{};
        const auto* asset = bundle->find(fb::AssetKind::resource, ui::faces[face], &index);
        const auto* payload = asset ? bundle->payload(fb::AssetKind::resource, index) : nullptr;
        if (!payload) {
            logging::log(logging::Level::warning, logging::Channel::graphics, "The game's face {} is missing.", ui::faces[face]);
            continue;
        }
        const auto bytes = data.read(*payload);
        if (!font_file(bytes)) {
            logging::log(logging::Level::warning, logging::Channel::graphics, "The game's face {} is not a font file.", ui::faces[face]);
            continue;
        }
        // Written beside it first, then put in its place: a font half written is never read.
        auto part = files[face];
        part += L".part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out) throw std::runtime_error("its file could not be written");
        }
        fs::rename(part, files[face], error);
        if (error) {
            fs::remove(part, error);
            throw std::runtime_error("its file could not be put in place");
        }
    }
    logging::log(logging::Level::info, logging::Channel::graphics, "The game's faces are kept for plugins in {}.", files.front().parent_path().string());
}
}

fs::path file(std::size_t face) {
    if (face >= plugin_ui::game_faces.size()) return {};
    try {
        const auto folder = content_cache::directory();
        if (folder.empty()) return {};
        return folder / "fonts" / (std::string(plugin_ui::game_faces[face]) + ".ttf");
    } catch (...) {
        return {};
    }
}

void prepare() noexcept {
    if (started.exchange(true)) return;
    try {
        std::thread([] {
            // (Nothing may leave a thread of its own: the game would close.)
            try {
                read_faces();
            } catch (const std::exception& error) {
                logging::log(logging::Level::warning, logging::Channel::graphics, "The game's faces are unavailable: {}.", error.what());
            } catch (...) {
            }
        }).detach();
    } catch (...) {
    }
}
}
