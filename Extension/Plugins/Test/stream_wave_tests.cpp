#include "../plugin_stream_wave.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>

using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto file = fs::temp_directory_path() / ("reskate-pcm-" + std::to_string(stamp) + ".wav");
    const Bytes stereo{'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,2,0,0x44,0xac,0,0,0x10,0xb1,2,0,4,0,16,0,'d','a','t','a',4,0,0,0,1,2,3,4};
    const auto open = [&](const Bytes& bytes) {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) throw std::runtime_error("writing fixture failed");
        std::string error;
        return dingosdk::plugins::open_stream_wave(file, error);
    };
    auto wave = open(stereo);
    bool ok = wave && wave->rate == 44100 && wave->channels == 2 && wave->frames() == 1 &&
        std::equal(wave->pcm.begin(), wave->pcm.end(), stereo.end() - 4, stereo.end());
    wave.reset();
    auto padded = stereo;
    padded.insert(padded.end(), {'J','U','N','K',1,0,0,0,42,0});
    padded[4] += 10;
    ok &= static_cast<bool>(open(padded));
    padded.pop_back();
    --padded[4];
    ok &= !open(padded);
    auto trailing = stereo;
    trailing.push_back(42);
    ++trailing[4];
    ok &= !open(trailing);
    auto duplicate = stereo;
    duplicate.insert(duplicate.begin() + 36, {'d','a','t','a',0,0,0,0});
    duplicate[4] += 8;
    ok &= !open(duplicate);
    ok &= fs::remove(file);
    if (!ok) std::cerr << "file-backed PCM regression\n";
    return ok ? 0 : 1;
}
