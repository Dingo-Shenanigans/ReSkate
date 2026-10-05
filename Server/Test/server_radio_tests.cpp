#include "Server/server_radio.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <opus.h>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace dingosdk::server;
using namespace dingosdk::multiplayer;
namespace fs = std::filesystem;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
void frames() {
    RadioBatch batch{7, 40, {{1, 2, 3}, std::vector<std::uint8_t>(max_radio_frame, 9)}};
    const auto bytes = encode_radio_batch(batch);
    const auto back = decode_radio_batch(bytes);
    check(back && back->track == 7 && back->first == 40 && back->frames == batch.frames, "radio batch round trip");
    for (std::size_t cut = 0; cut < bytes.size(); ++cut)
        check(!decode_radio_batch(std::span(bytes).first(cut)), "truncated radio batch accepted at " + std::to_string(cut));
    auto longer = bytes;
    longer.push_back(0);
    check(!decode_radio_batch(longer), "radio batch with trailing bytes accepted");
    bool threw{};
    try { encode_radio_batch({0, 0, {{1}}}); } catch (const std::invalid_argument &) { threw = true; }
    check(threw, "radio batch without a track encoded");
}
void sources(const fs::path &root) {
    const auto folder = root / "Radio";
    fs::create_directories(folder / "albums");
    fs::create_directories(root / "outside");
    { std::ofstream(folder / "song.mp3") << "x"; }
    { std::ofstream(root / "outside" / "secret.mp3") << "x"; }
    // Windows only lets an administrator (or developer mode) make links: without one, no link to test.
    std::error_code no_link;
    fs::create_directory_symlink(root / "outside", folder / "escape", no_link);
    std::string error;
    check(Radio::check_source("https://example.com/live.mp3", folder, error) == "https://example.com/live.mp3", "https URL refused");
    check(Radio::check_source("HTTP://example.com/a", folder, error) == "HTTP://example.com/a", "upper-case scheme refused");
    check(Radio::check_source("song.mp3", folder, error) == fs::weakly_canonical(folder / "song.mp3").string(), "file refused");
    check(!Radio::check_source("albums", folder, error).empty(), "folder refused");
    for (const auto *bad : {"", "../outside/secret.mp3", "albums/../../outside/secret.mp3", "/etc/passwd", "escape/secret.mp3",
                            "-o", "--exec=id", "file:///etc/passwd", "tcp://10.0.0.1:80", "ftp://example.com/a.mp3",
                            "concat:song.mp3|song.mp3", "missing.mp3", "https://example.com/a b", "https://example.com/\n-x"})
        check(Radio::check_source(bad, folder, error).empty(), std::string("source accepted: ") + bad);
#ifdef _WIN32
    for (const auto *bad : {"..\\outside\\secret.mp3", "albums\\..\\..\\outside\\secret.mp3", "C:\\Windows\\win.ini",
                            "C:song.mp3", "\\\\server\\share\\a.mp3", "\\outside\\secret.mp3", "song.mp3:hidden", "NUL"})
        check(Radio::check_source(bad, folder, error).empty(), std::string("source accepted: ") + bad);
#endif
}
// Energy at `frequency` relative to the whole signal (Goertzel on the left channel).
double tone_share(const std::vector<float> &left, double frequency) {
    const double w = 2 * std::numbers::pi * frequency / radio_rate, coefficient = 2 * std::cos(w);
    double s1{}, s2{}, total{};
    for (const auto x : left) {
        const auto s = x + coefficient * s1 - s2;
        s2 = s1;
        s1 = s;
        total += x * x;
    }
    const auto power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return total ? power / (total * left.size() / 2) : 0;
}
struct Played {
    std::map<std::uint32_t, std::vector<float>> left; // decoded, per track
    std::vector<std::string> notices;
    std::size_t frames{};
    bool paced = true;
};
// Runs the radio on a clock that moves 20 ms per step, until it finishes or `steps` run out.
Played listen(Radio &radio, unsigned steps, const std::string &skip_after = {}) {
    Played played;
    std::map<std::uint32_t, OpusDecoder *> decoders;
    std::uint64_t now = 1000000000;
    const auto start = now;
    bool skipped{};
    for (unsigned step = 0; step < steps; ++step, now += 20000) {
        auto out = radio.poll(now);
        for (auto &notice : out.notices) played.notices.push_back(notice);
        for (const auto &batch : out.batches)
            for (const auto &frame : batch.frames) {
                auto &decoder = decoders[batch.track];
                int error{};
                if (!decoder) decoder = opus_decoder_create(radio_rate, radio_channels, &error);
                std::vector<float> pcm(radio_frame_samples * radio_channels);
                const auto count = opus_decode_float(decoder, frame.data(), static_cast<opus_int32>(frame.size()), pcm.data(),
                                                     static_cast<int>(radio_frame_samples), 0);
                check(count == static_cast<int>(radio_frame_samples), "an Opus frame did not decode to 20 ms");
                for (int i = 0; i < count; ++i) played.left[batch.track].push_back(pcm[2 * i]);
                ++played.frames;
            }
        // Never more than real time plus the lead (and the frame being released).
        if (played.frames > (now - start) / 20000 + 11) played.paced = false;
        if (!skip_after.empty() && !skipped && !played.notices.empty() && played.notices.back() == skip_after &&
            played.frames > 50) {
            radio.skip();
            skipped = true;
        }
        if (!played.notices.empty() && played.notices.back() == "Radio: the queue has finished.") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto &[track, decoder] : decoders) opus_decoder_destroy(decoder);
    return played;
}
// The command is fixed by the test, so going through the shell (cmd.exe on Windows) is fine here.
bool make_tone(const fs::path &file, int frequency, int seconds) {
#ifdef _WIN32
    const auto quoted = "\"" + file.string() + "\"";
#else
    const auto quoted = "'" + file.string() + "'";
#endif
    const auto command = "ffmpeg -nostdin -loglevel error -y -f lavfi -i sine=frequency=" + std::to_string(frequency) +
                         ":sample_rate=48000:duration=" + std::to_string(seconds) + " -ac 2 " + quoted;
    return std::system(command.c_str()) == 0;
}
void tones(const fs::path &root) {
    const auto folder = root / "Radio";
    fs::create_directories(folder / "set");
    if (!make_tone(folder / "tone.wav", 440, 3) || !make_tone(folder / "set" / "a.wav", 440, 4) ||
        !make_tone(folder / "set" / "b.wav", 880, 2)) {
        std::cerr << "ffmpeg is not installed: skipping the playback tests\n";
        return;
    }
    {
        Radio radio(folder);
        check(radio.play("tone.wav").starts_with("Radio: starting"), "play refused a file");
        const auto played = listen(radio, 2000);
        check(!played.notices.empty() && played.notices.front() == "Now playing: tone", "no now-playing notice");
        check(played.notices.back() == "Radio: the queue has finished.", "the radio did not finish");
        check(played.frames == 150, "3 s became " + std::to_string(played.frames) + " frames, not 150");
        check(played.paced, "frames left faster than real time");
        const auto &left = played.left.begin()->second;
        double sum{};
        for (const auto x : left) sum += x * x;
        const auto rms = std::sqrt(sum / left.size());
        check(rms > 0.05, "the tone came out silent (RMS " + std::to_string(rms) + ")");
        check(tone_share(left, 440) > 0.9, "the tone is not at 440 Hz");
        check(tone_share(left, 1000) < 0.01, "energy at 1000 Hz");
    }
    {
        // A folder plays in name order; skip ends the first song and the second plays whole.
        Radio radio(folder);
        radio.play("set");
        const auto played = listen(radio, 3000, "Now playing: a");
        check(played.notices.size() >= 3 && played.notices[1] == "Now playing: b", "skip did not move on to b");
        check(played.left.size() == 2, "expected two tracks");
        const auto &a = played.left.begin()->second, &b = played.left.rbegin()->second;
        check(a.size() < 4 * radio_rate * 3 / 4, "the skipped song played to the end");
        check(b.size() == 2 * radio_rate, "the second song did not play whole");
        check(tone_share(b, 880) > 0.9, "the second song is not at 880 Hz");
    }
    {
        Radio radio(folder);
        radio.play("set");
        listen(radio, 30);
        check(radio.stop() == "Radio stopped.", "stop");
        check(radio.status().starts_with("Nothing is playing"), "status after stop");
        check(radio.poll(2000000000).batches.empty(), "frames after stop");
    }
}
} // namespace

int main() {
    frames();
    const auto root = fs::temp_directory_path() / ("reskate-radio-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    sources(root);
    tones(root);
    fs::remove_all(root);
    std::cout << "server radio tests passed\n";
}
