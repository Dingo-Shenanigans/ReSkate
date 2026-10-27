// Offline: builds the Animator Studio's trick library from an installed game exactly as the studio does in game
// (native_library.h), into a new folder, to check it against a library built by contrib/animator-studio. It reads the
// game's files and never writes into the game.
//
//     dingosdk_anim_library_build <game-root> <output-folder>
#include "Extension/AnimationStudio/native_library.h"
#include <chrono>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    namespace library = dingosdk::anim_studio::native_library;
    if (argc != 3) {
        std::cerr << "Usage: dingosdk_anim_library_build <game-root> <output-folder>\n";
        return 1;
    }
    try {
        const std::filesystem::path game = std::filesystem::absolute(argv[1]), output = std::filesystem::absolute(argv[2]);
        if (std::filesystem::exists(output) && !std::filesystem::is_empty(output)) throw std::runtime_error("Choose a new or empty output folder");
        const auto start = std::chrono::steady_clock::now();
        std::string last;
        const auto report = library::build(game, output, [&](std::string_view step, std::size_t done, std::size_t total) {
            if (step != last) {
                last = step;
                std::cout << '[' << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() << " s] " << step << '\n'
                          << std::flush;
            }
            (void)done;
            (void)total;
            return true;
        }, "offline build");
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        std::cout << "assets " << report.assets << ", clips " << report.clips << " (tricks " << report.tricks << "), unsupported " << report.unsupported
                  << ", errors " << report.errors << ", " << seconds << " s\n";
        if (!report.first_error.empty()) std::cout << "first error: " << report.first_error << '\n';
        return report.errors ? 2 : 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
