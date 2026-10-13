// By hand: captures what an application is playing for a few seconds and says how loud it
// was, first as it is and then with the application's own output muted (does the capture
// still hear it?). app_audio_probe [Spotify.exe]
#include "Extension/Music/app_audio_capture.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace dingosdk;

int main(int argc, char** argv) {
    std::wstring name = L"Spotify.exe";
    if (argc > 1) name.assign(argv[1], argv[1] + std::strlen(argv[1]));
    app_audio::start(name.c_str());
    std::vector<float> left(480), right(480);
    const auto listen = [&](const char* what, int seconds) {
        float loudest = 0;
        unsigned real = 0, asked = 0;
        // Read as the game would: 480 frames a hundred times a second, at 48 kHz.
        for (int i = 0; i < seconds * 100; ++i) {
            real += app_audio::read(1, left.data(), right.data(), 480, 48000.0f);
            asked += 480;
            for (const float sample : left) loudest = std::max(loudest, std::fabs(sample));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto state = app_audio::state();
        std::cout << what << ": capturing " << state.capturing << ", process " << state.process << ", captured " << state.frames << " frames, read "
                  << real << " of " << asked << ", loudest read " << loudest << ", level " << state.level << ", muted " << state.muted
                  << (state.problem.empty() ? "" : " (" + state.problem + ")") << std::endl;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    listen("as it is", 3);
    app_audio::mute_source(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    listen("with its own output muted", 3);
    app_audio::mute_source(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    listen("unmuted again", 1);
    app_audio::stop();
    return 0;
}
