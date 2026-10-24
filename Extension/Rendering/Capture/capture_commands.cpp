#include "Extension/Console/commands.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <filesystem>
#include <sstream>
#include <string>

namespace dingosdk::console {
namespace {
std::filesystem::path game_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}
} // namespace

// Research aids for screen captures (reskate.capture), from Fit Check's `fitcheck depth`.
void register_capture_commands(Commands& registry) {
    auto arguments = argument("verb", Type::text, true);
    arguments.complete = [](const Model&, auto) { return std::vector<std::string>{"depth"}; };
    auto command = action("capture",
                          "Research aids for plugins' screen captures (reskate.capture): depth (the game's depth buffers over the next frames, "
                          "as pictures with depth-probe.txt in Captures\\depth beside Skate.exe). Answers go to the log.",
                          Group::console, {std::move(arguments)});
    command.run = [](const Model&, const Values& values, const Output& out) {
        std::istringstream words(values.empty() ? std::string{} : std::get<std::string>(values[0]));
        std::string verb;
        words >> verb;
        if (verb == "depth") {
            const auto folder = game_folder() / L"Captures" / L"depth";
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            overlay::request_depth_probe(folder.c_str());
            out("Taking the game's depth buffers over the next frames: depth-probe.txt and depth-*.pgm go to Captures\\depth.");
        } else {
            out("capture depth");
        }
    };
    registry.add(std::move(command));
}
} // namespace dingosdk::console
