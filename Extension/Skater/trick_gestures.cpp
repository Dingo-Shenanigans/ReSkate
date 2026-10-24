#include "trick_gestures.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dingosdk::tricks {
namespace {
namespace ebx = frostbite::ebx;
// Game build 20260929: the core level carries every gesture.
constexpr std::string_view gesture_toc = "Win32/levels/game/bam_levelroot/bam_levelroot.toc";
constexpr std::string_view gesture_bundle = "win32/levels/game/bam_levelroot/bam_levelroot";
constexpr std::string_view gesture_prefix = "gameplay/input/gestures/fliptricks/gesture_fliptrick_";

using Gestures = std::map<std::string, StickPath, std::less<>>;
struct Loader {
    std::once_flag started;
    std::atomic<std::shared_ptr<const Gestures>> gestures;
};
Loader &loader() {
    static auto *value = new Loader;
    return *value;
}

std::string lower(std::string_view text) {
    std::string out;
    for (const char c : text) out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return out;
}
double number(const ebx::Value &value) {
    return std::visit([](const auto &v) -> double {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t> || std::is_same_v<T, double>) return static_cast<double>(v);
        else if constexpr (std::is_same_v<T, bool>) return v ? 1.0 : 0.0;
        else return 0.0;
    }, value.data);
}
double field(const ebx::Object &object, std::string_view name) {
    const auto *found = object.find(name);
    return found ? number(found->value) : 0.0;
}
const ebx::Value::Array *array(const ebx::Object &object, std::string_view name) {
    const auto *found = object.find(name);
    return found ? std::get_if<ebx::Value::Array>(&found->value.data) : nullptr;
}
const ebx::Object *object_of(const ebx::Value &value) {
    const auto *pointer = std::get_if<std::shared_ptr<ebx::Object>>(&value.data);
    return pointer && *pointer ? pointer->get() : nullptr;
}
// A gesture's path: its first pattern of points (Mode 0), or else its first one of arcs (Mode 1),
// each arc's middle at its middle reach. Angle 0 is the stick pulled back, 90 to the left.
StickPath path_of(const ebx::Object &gesture) {
    const auto *patterns = array(gesture, "Patterns");
    if (!patterns) return {};
    StickPath arcs_path;
    for (const auto &item : *patterns) {
        const auto *pattern = object_of(item);
        if (!pattern) continue;
        if (field(*pattern, "Mode") == 0) {
            StickPath path;
            if (const auto *points = array(*pattern, "Points"))
                for (const auto &p : *points)
                    if (const auto *point = object_of(p)) path.push_back({static_cast<float>(field(*point, "x")), static_cast<float>(field(*point, "y"))});
            if (path.size() >= 2) return path;
        } else if (arcs_path.empty()) {
            if (const auto *arcs = array(*pattern, "Arcs"))
                for (const auto &a : *arcs)
                    if (const auto *arc = object_of(a)) {
                        const double start = field(*arc, "StartAngle"), end = field(*arc, "EndAngle");
                        const double sweep = std::fmod(end - start + 360.0, 360.0);
                        const double angle = (start + sweep / 2.0) * 3.14159265358979 / 180.0;
                        const double reach = std::clamp((field(*arc, "MinMagnitude") + field(*arc, "MaxMagnitude")) / 2.0, 0.0, 1.0);
                        arcs_path.push_back({static_cast<float>(std::sin(angle) * reach), static_cast<float>(std::cos(angle) * reach)});
                    }
        }
    }
    return arcs_path.size() >= 2 ? arcs_path : StickPath{};
}
std::filesystem::path game_folder() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("the game's folder is unknown");
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}
Gestures read_gestures(const std::filesystem::path &game_root) {
    const vfs::GameData data(game_root);
    const auto toc = data.read_toc(gesture_toc);
    const auto bundle = data.read_bundle(toc, gesture_bundle);
    if (!bundle) throw std::runtime_error("the gestures' bundle is missing");
    Gestures gestures;
    for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i) {
        const auto name = lower(bundle->manifest.ebx[i].name);
        if (!name.starts_with(gesture_prefix)) continue;
        const auto key = name.substr(gesture_prefix.size());
        // The variants out of a grind or an underflip are the same flick done from elsewhere.
        if (key.find("fromgrind") != std::string::npos || key.find("underflip") != std::string::npos) continue;
        try {
            const auto *payload = bundle->payload(frostbite::AssetKind::ebx, i);
            if (!payload) continue;
            const auto document = ebx::read_document(data.read(*payload));
            if (const auto *root = document.root(); root && root->object)
                if (auto path = path_of(*root->object); !path.empty()) gestures[key] = std::move(path);
        } catch (...) {}
    }
    if (gestures.empty()) throw std::runtime_error("no flip trick gestures were found");
    return gestures;
}

// skate.'s name for a trick, as the gestures' keys spell it: "Nollie Kickflip" is n_kickflip,
// "FS 360 Pop Shove-it" fs360popshuvit.
std::string gesture_key(std::string_view trick) {
    std::string text;
    for (const char c : lower(trick))
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) text += c;
    for (const auto &[from, to] : {std::pair{"frontside", "fs"}, std::pair{"backside", ""}, std::pair{"shoveit", "shuvit"},
                                   std::pair{"shuveit", "shuvit"}}) {
        for (auto at = text.find(from); at != std::string::npos; at = text.find(from)) text.replace(at, std::string_view(from).size(), to);
    }
    if (text.starts_with("bs")) text.erase(0, 2);
    for (const std::string_view stance : {"fakie", "switch"})
        if (text.starts_with(stance) && text.size() > stance.size()) text.erase(0, stance.size());
    if (text.starts_with("nollie") && text.size() > 6) text = "n_" + text.substr(6);
    return text;
}
} // namespace

void prepare_trick_gestures() noexcept {
    try {
        std::call_once(loader().started, [] {
            std::thread([] {
                const auto started = GetTickCount64();
                try {
                    auto read = std::make_shared<const Gestures>(read_gestures(game_folder()));
                    logging::log(logging::Level::info, logging::Channel::runtime, "Trick gestures: read {} flip trick gestures in {} ms.",
                                 read->size(), GetTickCount64() - started);
                    loader().gestures.store(std::move(read));
                } catch (const std::exception &failure) {
                    logging::log(logging::Level::warning, logging::Channel::runtime, "Trick gestures: none shown: {}.", failure.what());
                }
            }).detach();
        });
    } catch (...) {}
}

std::size_t load_trick_gestures(const std::filesystem::path &game_root) {
    auto read = std::make_shared<const Gestures>(read_gestures(game_root));
    const auto count = read->size();
    loader().gestures.store(std::move(read));
    return count;
}

StickPath trick_gesture(std::string_view trick) {
    const auto gestures = loader().gestures.load();
    if (!gestures) return {};
    const auto found = gestures->find(gesture_key(trick));
    return found != gestures->end() ? found->second : StickPath{};
}
} // namespace dingosdk::tricks
