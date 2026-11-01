#include "Extension/Rendering/Capture/capture_encode.h"
#include "Extension/Rendering/Capture/capture_matte.h"
#include "Extension/Rendering/Capture/capture_panel.h"
#include "Extension/Rendering/Capture/capture_recorder.h"
#include "Extension/Rendering/Capture/capture_parallel.h"
#include "Extension/Rendering/Capture/capture_readback.h"
#include <stdexcept>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <array>
#include <string>
#include <thread>

// Captures without the game (reskate.capture): matting, resizing, the card, the file formats, and a
// recording of a turning board or skater found, cut out and saved.
// Run with the folder of the menu fonts (External/fonts) as the first argument to test the card too,
// and a folder as the second to save a picture of it there.
namespace {
using namespace dingosdk::capture;
int failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}

std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3]; }

// The chunk types of a PNG, in order.
std::vector<std::string> chunks(const std::vector<std::uint8_t>& png) {
    std::vector<std::string> out;
    for (std::size_t at = 8; at + 12 <= png.size();) {
        const auto size = be32(png.data() + at);
        out.emplace_back(reinterpret_cast<const char*>(png.data() + at + 4), 4);
        at += 12 + size;
    }
    return out;
}

// A board-ish test picture: an opaque rounded bar with a soft edge, a see-through middle band and a
// fully transparent background.
Image board(std::uint32_t size, float turn) {
    Image image(size, size);
    const auto n = static_cast<float>(size);
    const float c = n / 2.0f, half_w = n * 0.18f * std::abs(std::cos(turn)) + 2, half_h = n * 0.42f;
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float dx = std::abs(fx - c) - half_w, dy = std::abs(fy - c) - half_h;
            const float edge = std::clamp(0.5f - std::max(dx, dy), 0.0f, 1.0f);
            auto* p = image.at(x, y);
            p[0] = static_cast<std::uint8_t>(40 + y * 200 / size);
            p[1] = 140;
            p[2] = static_cast<std::uint8_t>(220 - x * 100 / size);
            const bool band = std::abs(fy - c) < n * 0.05f;
            p[3] = static_cast<std::uint8_t>(std::lround(edge * (band ? 120 : 255)));
        }
    return image;
}

// `image` over a background, then "graded" the way the game's post-processing shifts both captures.
std::vector<std::uint8_t> composite(const Image& image, std::array<float, 3> background) {
    std::vector<std::uint8_t> out(image.rgba.size());
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        const float a = image.rgba[i + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) {
            const float v = image.rgba[i + c] * a + background[c] * (1 - a);
            out[i + 2 - c] = static_cast<std::uint8_t>(std::lround(std::clamp(v * 0.9f + 6.0f, 0.0f, 255.0f))); // BGRA
        }
        out[i + 3] = 255;
    }
    return out;
}

void test_matte() {
    const auto truth = board(96, 0.3f);
    const auto dark = composite(truth, {10, 12, 14}), light = composite(truth, {235, 236, 240});
    const PixelView d{dark.data(), 96, 96, 96 * 4, true}, l{light.data(), 96, 96, 96 * 4, true};
    const Rect all{0, 0, 96, 96};
    const auto backgrounds = measure_backgrounds(d, l, all);
    expect(std::abs(backgrounds.dark[0] - (10 * 0.9f + 6)) < 1 && std::abs(backgrounds.light[2] - (240 * 0.9f + 6)) < 1,
           "backgrounds are measured from the edge");
    const auto cut = matte_pair(d, l, all, backgrounds, 0.0f);
    int worst_alpha = 0, worst_colour = 0;
    for (std::uint32_t y = 0; y < 96; ++y)
        for (std::uint32_t x = 0; x < 96; ++x) {
            const auto* want = truth.at(x, y);
            const auto* got = cut.at(x, y);
            worst_alpha = std::max(worst_alpha, std::abs(want[3] - got[3]));
            if (want[3] > 60)
                for (int c = 0; c < 3; ++c)
                    worst_colour = std::max(worst_colour, std::abs(static_cast<int>(want[c] * 0.9f + 6) - got[c]));
        }
    expect(worst_alpha <= 3, "matting recovers alpha, soft edges and the see-through band included");
    expect(worst_colour <= 6, "matting recovers the board's colour without the background in it");
    expect(content_bounds(cut).width < 96, "the cut-out has transparent space around the board");

    const auto same = matte_pair(d, d, all, measure_backgrounds(d, d, all));
    expect(same.at(0, 0)[3] == 255, "identical captures are taken as opaque, not empty");
}

// A whole turn of a board with a black grip over a dark radial vignette, the Board screen's worst
// case: where the board has been, the backdrop must come back see-through, not as a dark ghost.
void test_backdrop_key(bool light_both_sides) {
    constexpr std::uint32_t w = 300, h = 500, turn_frames = 120;
    std::vector<Image> frames, truth;
    for (std::uint32_t f = 0; f < turn_frames; ++f) {
        const float angle = 6.2831853f * static_cast<float>(f) / turn_frames;
        const float half_w = 55 * std::abs(std::cos(angle)) + 3, half_h = 200;
        const bool grip = std::cos(angle) < 0;
        Image frame(w, h), want(w, h);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                const float rx = (fx - 120) / 300, ry = (fy - 230) / 400;
                // A vignette no simple surface fits: a soft spot, a kink and a slow ripple.
                const float bg = 24 + 30 * std::exp(-(rx * rx + ry * ry) * 5) + 8 * std::max(0.0f, 0.4f - std::abs(rx)) +
                                 5 * std::sin(fy / 37);
                const float dx = std::abs(fx - 150) - half_w, dy = std::abs(fy - 250) - half_h;
                const float cover = std::clamp(0.5f - std::max(dx, dy), 0.0f, 1.0f);
                // A white deck with a black grip; or one light on both sides (a white grip), which covers
                // the middle for most of the turn: there the backdrop is seen least.
                const float back = light_both_sides ? 200.0f : 12.0f;
                const float deck[3] = {grip ? back : 226.0f, grip ? back : 220.0f, grip ? back + 1 : 208.0f};
                auto* p = frame.at(x, y);
                for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(std::lround(deck[c] * cover + (bg + static_cast<float>(c)) * (1 - cover)));
                p[3] = 255;
                want.at(x, y)[3] = static_cast<std::uint8_t>(std::lround(cover * 255));
            }
        frames.push_back(std::move(frame));
        truth.push_back(std::move(want));
    }
    key_against_backdrop(frames);
    std::uint64_t ghost = 0, lost = 0, background = 0, board = 0;
    for (std::uint32_t f = 0; f < turn_frames; ++f)
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto want = truth[f].at(x, y)[3], got = frames[f].at(x, y)[3];
                if (want == 0) ++background, ghost += got > 64;
                if (want == 255) ++board, lost += got < 192;
            }
    std::printf("backdrop key (%s): %.3f%% ghost, %.3f%% lost\n", light_both_sides ? "light both sides" : "black grip", 100.0 * static_cast<double>(ghost) / static_cast<double>(background),
                100.0 * static_cast<double>(lost) / static_cast<double>(board));
    expect(ghost * 1000 < background, "where the board has been comes back see-through");
    expect(lost * 100 < board, "a black grip on the dark backdrop stays board");
}

void test_geometry() {
    const auto square = padded_square({10, 20, 30, 50}, 5, 200, 100);
    expect(square.width == 60 && square.height == 60, "padded_square grows and squares");
    expect(square.x + square.width <= 200 && square.y + square.height <= 100, "padded_square stays inside");
    const auto u = united({0, 0, 10, 10}, {20, 5, 5, 20});
    expect(u.x == 0 && u.y == 0 && u.width == 25 && u.height == 25, "united covers both");
    const auto small = fitted(board(200, 0), 50);
    expect(small.width == 50 && small.height == 50, "fitted keeps a square square");
    expect(small.at(25, 2)[3] == 0 && small.at(25, 15)[3] > 200, "fitted keeps the background clear and the board solid");
    // Premultiplied filtering: an opaque red pixel next to transparent green does not turn olive.
    Image edge(2, 1);
    edge.rgba = {255, 0, 0, 255, 0, 255, 0, 0};
    const auto halved = fitted(edge, 1);
    expect(halved.at(0, 0)[0] == 255 && halved.at(0, 0)[1] == 0 && halved.at(0, 0)[3] == 128, "fitted does not bleed transparent colour");
    const auto flat = over_matte(halved, {0, 0, 255});
    expect(flat.at(0, 0)[3] == 255 && flat.at(0, 0)[0] == 128 && flat.at(0, 0)[2] == 127, "over_matte flattens");
}

void test_png() {
    Animation animation;
    animation.fps = 24;
    for (int i = 0; i < 6; ++i) animation.frames.push_back(board(64, static_cast<float>(i) * 0.5f));
    const auto apng = encode_apng(animation);
    expect(apng.size() > 8 && std::memcmp(apng.data(), "\x89PNG\r\n\x1a\n", 8) == 0, "APNG signature");
    const auto types = chunks(apng);
    const auto count = [&](const char* t) { return std::count(types.begin(), types.end(), std::string(t)); };
    expect(types.front() == "IHDR" && types[1] == "acTL" && types.back() == "IEND", "APNG chunk order");
    expect(count("fcTL") == 6 && count("IDAT") == 1 && count("fdAT") == 5, "APNG has one control and data chunk per frame");
    const auto still = encode_png(animation.frames.front());
    expect(chunks(still).size() == 3, "PNG is IHDR, IDAT, IEND");
}

// Decodes a GIF's first frame's LZW data back to indices, to check the encoder's code-size steps.
std::vector<std::uint8_t> first_frame_indices(const std::vector<std::uint8_t>& gif, std::size_t pixels) {
    std::size_t at = 13 + 768;
    while (at < gif.size() && gif[at] == 0x21) { // extensions
        at += 2;
        while (gif[at]) at += gif[at] + 1u;
        ++at;
    }
    at += 10; // image descriptor
    const int min_bits = gif[at++];
    std::vector<std::uint8_t> data;
    while (gif[at]) {
        data.insert(data.end(), gif.begin() + static_cast<std::ptrdiff_t>(at + 1), gif.begin() + static_cast<std::ptrdiff_t>(at + 1 + gif[at]));
        at += gif[at] + 1u;
    }
    const int clear = 1 << min_bits, end = clear + 1;
    std::vector<std::vector<std::uint8_t>> table;
    const auto reset = [&] {
        table.assign(static_cast<std::size_t>(end) + 1, {});
        for (int i = 0; i < clear; ++i) table[static_cast<std::size_t>(i)] = {static_cast<std::uint8_t>(i)};
    };
    reset();
    int bits = min_bits + 1, previous = -1;
    std::size_t bit = 0;
    std::vector<std::uint8_t> out;
    while (bit + static_cast<std::size_t>(bits) <= data.size() * 8 && out.size() < pixels) {
        int code = 0;
        for (int k = 0; k < bits; ++k, ++bit) code |= ((data[bit / 8] >> (bit % 8)) & 1) << k;
        if (code == clear) { reset(), bits = min_bits + 1, previous = -1; continue; }
        if (code == end) break;
        std::vector<std::uint8_t> entry;
        if (code < static_cast<int>(table.size())) entry = table[static_cast<std::size_t>(code)];
        else if (previous >= 0) entry = table[static_cast<std::size_t>(previous)], entry.push_back(table[static_cast<std::size_t>(previous)][0]);
        else return {};
        out.insert(out.end(), entry.begin(), entry.end());
        if (previous >= 0 && table.size() < 4096) {
            auto added = table[static_cast<std::size_t>(previous)];
            added.push_back(entry[0]);
            table.push_back(std::move(added));
        }
        if (static_cast<int>(table.size()) >= (1 << bits) && bits < 12) ++bits;
        previous = code;
    }
    return out;
}

void test_gif() {
    // Noise fills the LZW table several times over, so clears and every code width get used.
    Image noise(300, 200);
    std::uint32_t seed = 7;
    for (std::size_t i = 0; i < noise.rgba.size(); i += 4) {
        seed = seed * 1664525u + 1013904223u;
        noise.rgba[i] = static_cast<std::uint8_t>(seed >> 24), noise.rgba[i + 1] = static_cast<std::uint8_t>(seed >> 16);
        noise.rgba[i + 2] = static_cast<std::uint8_t>(seed >> 8), noise.rgba[i + 3] = (seed >> 7) & 1 ? 255 : 0;
    }
    Animation animation;
    animation.frames = {noise, noise};
    const auto gif = encode_gif(animation, {.fringe = {0, 0, 0}, .dither = false});
    expect(std::memcmp(gif.data(), "GIF89a", 6) == 0 && gif.back() == 0x3b, "GIF header and trailer");
    const auto indices = first_frame_indices(gif, 300 * 200);
    expect(indices.size() == 300 * 200, "GIF LZW data decodes to every pixel");
    bool transparent_right = indices.size() == 300 * 200;
    for (std::size_t i = 0; transparent_right && i < indices.size(); ++i)
        transparent_right = (indices[i] == 255) == (noise.rgba[i * 4 + 3] < 128);
    expect(transparent_right, "GIF cuts out exactly the transparent pixels");

    Animation timing;
    timing.fps = 30;
    timing.frames.assign(3, board(16, 0));
    const auto timed = encode_gif(timing);
    int total = 0;
    for (std::size_t i = 0; i + 5 < timed.size(); ++i)
        if (timed[i] == 0x21 && timed[i + 1] == 0xf9) total += timed[i + 4] | timed[i + 5] << 8;
    expect(total == 10, "GIF delays add up to the clip's length");
}

void test_webm() {
    Animation animation;
    animation.fps = 30;
    for (int i = 0; i < 4; ++i) animation.frames.push_back(board(48, static_cast<float>(i) * 0.4f));
    const auto webm = encode_webm(animation);
    const auto has = [&](std::initializer_list<std::uint8_t> bytes) {
        return std::search(webm.begin(), webm.end(), bytes.begin(), bytes.end()) != webm.end();
    };
    expect(webm.size() > 4 && webm[0] == 0x1a && webm[1] == 0x45 && webm[2] == 0xdf && webm[3] == 0xa3, "WebM starts with an EBML header");
    expect(has({'V', '_', 'V', 'P', '8'}), "WebM track is VP8");
    expect(has({0x53, 0xc0}), "a see-through WebM marks its track AlphaMode");
    expect(has({0x9d, 0x01, 0x2a}), "WebM frames are VP8 key frames");
    Animation opaque;
    opaque.frames = {over_matte(board(48, 0), {0, 0, 0})};
    const auto flat = encode_webm(opaque);
    const std::array<std::uint8_t, 2> alpha_mode{0x53, 0xc0};
    expect(std::search(flat.begin(), flat.end(), alpha_mode.begin(), alpha_mode.end()) == flat.end(),
           "an opaque WebM has no alpha");
}

void test_panel(const std::filesystem::path& fonts, const char* out) {
    const auto load = [&](const char* name) {
        std::ifstream in(fonts / name, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    const auto brush = load("PermanentMarker-Regular.ttf"), heading = load("Montserrat-ExtraBold.ttf"), body = load("Montserrat-SemiBold.ttf");
    const std::vector<Part> parts{{"Deck", "ReSkate Scrolling Pattern"},
                                  {"Grip tape", "A grip tape with a name long enough that it has to wrap onto a second line and then some more"},
                                  {"Trucks", "Silver"}, {"Wheels", ""}};
    const auto card = render_parts_panel("Board", parts, 512, {heading, body, brush});
    expect(card.height == 512 && card.width > 300, "the parts card is the height asked for");
    expect(card.at(0, 0)[3] < 40 && card.at(card.width / 2, card.height - 4)[3] > 200, "the card has rounded corners and a solid body");
    int bright = 0;
    for (std::size_t i = 0; i < card.rgba.size(); i += 4) bright += card.rgba[i] > 200;
    expect(bright > 500, "the card has text on it");
    const auto shown = side_by_side(fitted(board(300, 0.2f), 512), card);
    expect(shown.width == 512 + card.width, "side_by_side lays the card beside the board");
    if (out) { // a folder to save the card in, to look at it
        const auto png = encode_png(shown);
        std::ofstream(std::filesystem::path(out) / "parts_card.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
}
} // namespace

// A stand-in for the game: a 1280x720 frame with a board that turns a little every frame on the right
// of the menu (or a skater in the middle, swaying as they stand), a piece of UI that never moves, and
// the game's depth when asked for.
struct FakeGame {
    double turn{}, spin{1.0 / 37.3};
    bool skater{}, with_depth{};
    unsigned drawn{};
    std::vector<std::uint8_t> last_;
    std::vector<std::uint8_t> frame() {
        turn += spin;
        ++drawn;
        auto now = draw();
        // As the game's temporal smoothing does, a frame keeps a trace of the last one.
        if (!last_.empty())
            for (std::size_t i = 0; i < now.size(); ++i) now[i] = static_cast<std::uint8_t>((now[i] * 3 + last_[i] + 2) / 4);
        last_ = now;
        return now;
    }
    float centre_x() const { return 1280 * (skater ? 0.5f : 0.76f) + (skater ? 3.0f * std::sin(static_cast<float>(drawn) * 0.21f) : 0.0f); }
    float half_width() const { return 60 * std::abs(std::cos(static_cast<float>(turn) * 6.2831853f)) + 3; }
    std::vector<std::uint8_t> draw() const {
        constexpr std::uint32_t w = 1280, h = 720;
        std::vector<std::uint8_t> out(std::size_t{w} * h * 4);
        const float cx = centre_x(), cy = 720 * 0.57f, half_w = half_width(), half_h = 230;
        const bool back = std::cos(static_cast<float>(turn) * 6.2831853f) < 0;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                const float bg = 28.0f + 10.0f * fy / h;
                float rgb[3] = {bg, bg, bg + 4};
                if (x > 860 && x < 900 && y > 140 && y < 170) rgb[0] = rgb[1] = rgb[2] = 200; // UI that never moves
                const float dx = std::abs(fx - cx) - half_w, dy = std::abs(fy - cy) - half_h;
                const float cover = std::clamp(0.5f - std::max(dx, dy), 0.0f, 1.0f);
                const float deck[3] = {back ? 12.0f : 40.0f, back ? 12.0f : 150.0f + 80 * fy / h, back ? 13.0f : 220.0f};
                for (int c = 0; c < 3; ++c) rgb[c] = deck[c] * cover + rgb[c] * (1 - cover);
                auto* p = out.data() + (std::size_t{y} * w + x) * 4;
                for (int c = 0; c < 3; ++c) p[2 - c] = static_cast<std::uint8_t>(std::clamp(rgb[c] * 0.92f + 5, 0.0f, 255.0f));
                p[3] = 255;
            }
        return out;
    }
    // The game's depth for the frame drawn last: the board a quarter as far as the backdrop.
    std::vector<float> depth() const {
        constexpr std::uint32_t w = 1280, h = 720;
        const float cx = centre_x(), cy = 720 * 0.57f, half_w = half_width(), half_h = 230;
        std::vector<float> out(std::size_t{w} * h, 1.0f / 400);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                if (std::abs(static_cast<float>(x) + 0.5f - cx) < half_w && std::abs(static_cast<float>(y) + 0.5f - cy) < half_h)
                    out[std::size_t{y} * w + x] = 1.0f / 100;
        return out;
    }
};

// A recording of the board (or skater) turning, as a plugin makes one: its frames compared to find
// the turn, the frames evenly spaced through it saved.
void test_recording(const std::filesystem::path& fonts, Format format, bool skater, bool with_depth, std::uint32_t frames) {
    const auto load = [&](const char* name) {
        std::ifstream in(fonts / name, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    static const auto brush = load("PermanentMarker-Regular.ttf"), heading = load("Montserrat-ExtraBold.ttf"),
                      body = load("Montserrat-SemiBold.ttf");
    FakeGame game;
    game.skater = skater;
    game.with_depth = with_depth;
    Recorder recorder;
    RecordRequest request;
    request.area = skater ? Area{0.27f, 0.14f, 0.735f, 0.985f, true} : Area{0.655f, 0.185f, 0.86f, 0.985f, true};
    request.depth = with_depth;
    request.height = 480;
    request.skip = 3;
    request.most = 120;
    std::string why;
    expect(recorder.start(request, why), "a recording starts");
    for (int i = 0; i < 400 && recorder.status().state == State::recording; ++i) {
        const bool wanted = recorder.wants_frame();
        const auto pixels = game.frame();
        if (!wanted) continue;
        const auto depth = with_depth ? game.depth() : std::vector<float>{};
        recorder.take_frame({pixels.data(), 1280, 720, 1280 * 4, true, with_depth ? reinterpret_cast<const std::uint8_t*>(depth.data()) : nullptr,
                             1280 * 4});
    }
    const auto taken = recorder.status();
    expect(taken.state == State::stopped && taken.taken == 120, "it stops by itself at the frames asked for");
    expect(taken.depth == with_depth, "it says whether every frame was cut out by depth");
    // Comes round: about 37 frames on it looks as it started, half way it does not.
    const float round = recorder.compare(0, 37), half = recorder.compare(0, 19);
    expect(round >= 0 && half > 3 * round + 1, "a whole turn on, a frame looks like the first again; half way it does not");
    expect(recorder.compare(0, 500) < 0, "a frame not taken is not compared");
    const auto kept = recorder.kept();
    expect(kept.size() == 120 && kept.front() == 0 && kept.back() == 119, "every frame is kept while there is room");
    SaveRequest save;
    for (std::uint32_t j = 0; j < frames; ++j) save.frames.push_back(static_cast<std::uint32_t>(std::lround(37.3 * j / frames)));
    save.format = format;
    save.size = 256;
    save.solid = !skater;
    save.card_title = skater ? "Outfit" : "Board";
    save.card = {{"Deck", "Test Deck"}, {"Wheels", "Round"}};
    save.fonts = {heading, body, brush};
    save.file = std::filesystem::temp_directory_path() / "capture_tests" / (std::string("turn.") + extension(format));
    expect(recorder.save(save, why), "it saves");
    std::string second;
    expect(!recorder.save(save, second), "one save at a time");
    for (int i = 0; i < 600 && recorder.status().state == State::saving; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto saved = recorder.status();
    if (saved.state != State::saved)
        std::printf("recording (%s, %s, %s): %s\n", extension(format), skater ? "skater" : "board", with_depth ? "depth" : "colour", saved.message.c_str());
    expect(saved.state == State::saved, "the recording is saved");
    std::error_code error;
    expect(std::filesystem::file_size(saved.file, error) > 1000, "its file is written");
    if (with_depth) expect(saved.message.find("by depth") != std::string::npos, "cut out by the game's depth when it had one");
    else expect(saved.message.find("by colour") != std::string::npos, "else by colour");
    recorder.discard();
    expect(recorder.status().state == State::idle && recorder.kept().empty(), "discarded, nothing is held");
}

// The cut by the game's depth: a skater standing on a floor that curves away at the sides. Shoes only
// 3% nearer than the floor under them are kept (they went missing, 2026-10-24); the curved floor is
// not taken for the skater.
void test_depth_cut() {
    constexpr std::uint32_t w = 200, h = 150;
    std::vector<float> depth(std::size_t{w} * h);
    std::vector<std::uint8_t> pixels(std::size_t{w} * h * 4, 90);
    const auto floor_at = [&](std::uint32_t x) {
        const float t = (2.0f * x + 1) / w - 1;
        return 100.0f - 30.0f * t * t; // nearer at the sides, as a curved backdrop is
    };
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            float distance = floor_at(x);
            if (x >= 80 && x < 120 && y >= 20 && y < 130) distance *= 0.6f;             // the skater
            else if (x >= 85 && x < 115 && y >= 130 && y < 140) distance *= 0.97f;     // their shoes
            depth[std::size_t{y} * w + x] = 1.0f / distance;
        }
    const PixelView frame{pixels.data(), w, h, w * 4, true, reinterpret_cast<const std::uint8_t*>(depth.data()), w * 4};
    Image picture(w, h);
    expect(depth_mask(frame, {0, 0, w, h}, picture), "the depth shows something in front");
    std::size_t shoes = 0, floor = 0;
    for (std::uint32_t y = 130; y < 140; ++y)
        for (std::uint32_t x = 87; x < 113; ++x) shoes += picture.at(x, y)[3] == 255;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            if ((x < 70 || x >= 130) && picture.at(x, y)[3]) ++floor;
    expect(shoes == 26 * 10, "shoes just in front of the floor are kept");
    expect(floor == 0, "a backdrop curving away is not taken for the subject");
}

// The cut by the game's depth: a skater standing on a floor that curves away at the sides. Shoes only
// 0.5% nearer than the floor under them are kept (they went missing, 2026-10-24); the curved floor is
// not taken for the skater.
void test_depth_cut_contact() {
    constexpr std::uint32_t w = 200, h = 150;
    std::vector<float> depth(std::size_t{w} * h);
    std::vector<std::uint8_t> pixels(std::size_t{w} * h * 4, 90);
    const auto floor_at = [&](std::uint32_t x) {
        const float t = (2.0f * x + 1) / w - 1;
        return 100.0f - 30.0f * t * t; // nearer at the sides, as a curved backdrop is
    };
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            float distance = floor_at(x);
            if (x >= 80 && x < 120 && y >= 20 && y < 130) distance *= 0.6f;             // the skater
            else if (x >= 85 && x < 115 && y >= 130 && y < 134) distance *= 0.995f;     // their shoes
            depth[std::size_t{y} * w + x] = 1.0f / distance;
        }
    const PixelView frame{pixels.data(), w, h, w * 4, true, reinterpret_cast<const std::uint8_t*>(depth.data()), w * 4};
    Image picture(w, h);
    expect(depth_mask(frame, {0, 0, w, h}, picture), "the depth shows something in front");
    std::size_t shoes = 0, floor = 0;
    for (std::uint32_t y = 130; y < 134; ++y)
        for (std::uint32_t x = 87; x < 113; ++x) shoes += picture.at(x, y)[3] == 255;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            if ((x < 70 || x >= 130) && picture.at(x, y)[3]) ++floor;
    expect(shoes == 26 * 4, "near-contact soles extend the depth silhouette without colour contrast");
    expect(floor == 0, "a backdrop curving away is not taken for the subject");
}


// A wide shallow foreground and far depth outliers must not bias the floor fit,
// including the floor directly under the subject where sole recovery can grow.
void test_depth_cut_wide_and_far() {
    constexpr std::uint32_t w=800,h=200;
    for (bool far : {false,true}) {
        std::vector<float> depth(std::size_t{w}*h);
        std::vector<std::uint8_t> pixels(std::size_t{w}*h*4,90);
        for (std::uint32_t y=0;y<h;++y) for (std::uint32_t x=0;x<w;++x) {
            const double t=(2.0*x+1)/w-1;
            double d=100-25*t*t;
            if (x>=300 && x<500 && y>=20 && y<130) d*=.6;
            if (x>=200 && x<600 && y>=130 && y<140) d*=.97;
            if (far && x>=650 && x<660) d=9000000;
            depth[std::size_t{y}*w+x]=static_cast<float>(1/d);
        }
        Image picture(w,h);
        const PixelView frame{pixels.data(),w,h,w*4,true,reinterpret_cast<const std::uint8_t*>(depth.data()),w*4};
        expect(depth_mask(frame,{0,0,w,h},picture),"wide subject has a depth cut-out");
        for (std::uint32_t y=130;y<140;++y) for (std::uint32_t x=205;x<595;++x)
            expect(picture.at(x,y)[3]==255,"wide 3%-nearer shoes survive a curved floor fit");
        for (std::uint32_t y=140;y<h;++y) for (std::uint32_t x=0;x<w;++x)
            expect(picture.at(x,y)[3]==0,"floor directly beneath subject stays transparent");
        for (std::uint32_t y=0;y<h;++y) for (std::uint32_t x=650;x<660;++x)
            expect(picture.at(x,y)[3]==0,"far depth outliers never become foreground");
    }
}

// Background below wide cuffs must not be extended as though every column ended
// at a sole. Shadows can differ from the floor colour without being foreground.
void test_depth_cut_cuff_background() {
    constexpr std::uint32_t w=200,h=180;
    for (int background : {0,1,2}) {
        std::vector<float> depth(std::size_t{w}*h,1.0f/100.0f);
        std::vector<std::uint8_t> pixels(std::size_t{w}*h*4,90);
        Image picture(w,h);
        for (std::uint32_t y=0;y<h;++y) for (std::uint32_t x=0;x<w;++x) {
            const bool body=x>=65 && x<135 && y>=15 && y<90;
            const bool cuff=((x>=70 && x<95)||(x>=110 && x<135)) && y>=90 && y<120;
            const bool ankle=((x>=78 && x<87)||(x>=118 && x<127)) && y>=120 && y<137;
            const bool shoe=((x>=78 && x<93)||(x>=118 && x<133)) && y>=137 && y<143;
            const bool sole=((x>=79 && x<92)||(x>=119 && x<132)) && y>=143 && y<147;
            const bool cuff_gap=!ankle && ((x>=70 && x<95)||(x>=110 && x<135)) && y>=120 && y<127;
            const auto at=std::size_t{y}*w+x;
            if (body||cuff||ankle||shoe) depth[at]=1.0f/60.0f;
            if (sole) depth[at]=1.0f/99.5f;
            if (cuff_gap && background==1) depth[at]=0;
            if (cuff_gap && background==2) depth[at]=1.0f/99.0f;
            for (int c=0;c<3;++c) {
                const auto colour=static_cast<std::uint8_t>(cuff_gap?45:body||cuff||ankle||shoe||sole?150:90);
                pixels[at*4+static_cast<std::size_t>(c)]=colour;
                picture.at(x,y)[c]=colour;
            }
        }
        const PixelView frame{pixels.data(),w,h,w*4,false,reinterpret_cast<const std::uint8_t*>(depth.data()),w*4};
        expect(depth_mask(frame,{0,0,w,h},picture),"cuffed skater has a depth silhouette");
        for (std::uint32_t y=120;y<127;++y) for (std::uint32_t x=70;x<78;++x)
            expect(picture.at(x,y)[3]==0,"contrasting background below trouser cuff stays transparent");
        for (std::uint32_t y=143;y<147;++y) for (std::uint32_t x=80;x<91;++x)
            expect(picture.at(x,y)[3]==255,"near-contact soles remain intact after cuff cleanup");
        for (std::uint32_t y=147;y<155;++y) for (std::uint32_t x=70;x<95;++x)
            expect(picture.at(x,y)[3]==0,"floor beneath recovered sole remains transparent");
    }
}

void test_capture_background() {
    using namespace dingosdk::capture;
    constexpr std::uint32_t w = 512, h = 512;
    std::vector<std::uint8_t> pixels(std::size_t{w} * h * 4, 90);
    std::vector<float> depth(std::size_t{w} * h, 0.01f);
    for (std::uint32_t y = 65; y < 450; ++y)
        for (std::uint32_t x = 190; x < 325; ++x) {
            depth[std::size_t{y} * w + x] = 0.02f;
            auto* colour = pixels.data() + (std::size_t{y} * w + x) * 4;
            colour[0] = static_cast<std::uint8_t>(x);
            colour[1] = static_cast<std::uint8_t>(y);
            colour[2] = 180;
        }
    const auto original_pixels = pixels;
    const auto original_depth = depth;
    const auto view = [&] {
        return PixelView{pixels.data(), w, h, w * 4, true, reinterpret_cast<const std::uint8_t*>(depth.data()), w * 4};
    };
    RecordRequest request;
    request.height = 384;
    Recorder serial, background;
    std::string why;
    expect(serial.start(request, why), "serial comparison recording starts");
    serial.take_frame(view());
    serial.stop();
    request.background = true;
    expect(background.start(request, why), "background comparison recording starts");
    background.take_frame(view());
    // Simulate the readback mappings being reused immediately on return.
    std::fill(pixels.begin(), pixels.end(), std::uint8_t{0});
    std::fill(depth.begin(), depth.end(), 0.0f);
    background.stop();
    expect(background.status().taken == 1 && background.status().kept == 1 && background.status().depth && background.status().pending == 0,
           "stop drains owned colour/depth work after source buffers change");
    const auto root = std::filesystem::temp_directory_path() / "capture_background_tests";
    std::filesystem::create_directories(root);
    const auto save = [&](Recorder& recorder, const char* name, Format format = Format::png, std::uint32_t count = 1) {
        SaveRequest output;
        for (std::uint32_t n = 0; n < count; ++n)
            output.frames.push_back(n);
        output.format = format;
        output.fps = 60;
        output.size = 96;
        output.cutout = Cutout::depth;
        output.solid = false;
        output.file = root / name;
        expect(recorder.save(output, why), "background/serial PNG save starts");
        for (int n = 0; n < 1000 && recorder.status().state == State::saving; ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        expect(recorder.status().state == State::saved, "background/serial PNG save finishes");
        std::ifstream input(output.file, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
    };
    const auto expected = save(serial, "serial.png"), actual = save(background, "background.png");
    expect(!actual.empty() && actual == expected, "sparse background frames reconstruct pixel-identical encoded output");
    std::copy(original_pixels.begin(), original_pixels.end(), pixels.begin());
    std::copy(original_depth.begin(), original_depth.end(), depth.begin());
    Recorder moving_serial, moving_background;
    request.background = false;
    expect(moving_serial.start(request, why), "moving serial recording starts");
    request.background = true;
    expect(moving_background.start(request, why), "moving background recording starts");
    for (std::uint32_t n = 0; n < 6; ++n) {
        std::fill(depth.begin(), depth.end(), 0.01f);
        for (std::uint32_t y = 65; y < 450; ++y)
            for (std::uint32_t x = 190 + n * 4; x < 325 + n * 4; ++x)
                depth[std::size_t{y} * w + x] = 0.02f;
        moving_serial.take_frame(view());
        moving_background.take_frame(view());
        for (int wait = 0; wait < 1000 && moving_background.status().pending; ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    moving_serial.stop();
    moving_background.stop();
    const auto serial_animation = save(moving_serial, "serial-animation.png", Format::apng, 6);
    const auto background_animation = save(moving_background, "background-animation.png", Format::apng, 6);
    expect(!background_animation.empty() && serial_animation == background_animation,
           "moving sparse canvases preserve every APNG pixel and position");
    const auto animation_chunks = chunks(background_animation);
    expect(std::count(animation_chunks.begin(), animation_chunks.end(), "fcTL") == 6,
           "background output retains six real animation frames");
    for (std::size_t at = 8; at + 12 <= background_animation.size();) {
        const auto size = be32(background_animation.data() + at);
        if (std::memcmp(background_animation.data() + at + 4, "fcTL", 4) == 0 && size >= 26) {
            const auto* control = background_animation.data() + at + 8;
            expect(control[20] == 0 && control[21] == 1 && control[22] == 0 && control[23] == 60,
                   "background APNG uses an actual 1/60-second frame delay");
        }
        at += size + 12;
    }
    Recorder thinned;
    RecordRequest small;
    small.depth = false;
    small.most = 1000;
    expect(thinned.start(small, why), "frame-count thinning recording starts");
    std::vector<std::uint8_t> tiny_pixels(64 * 64 * 4, 150);
    for (int n = 0; n < 481; ++n)
        thinned.take_frame({tiny_pixels.data(), 64, 64, 64 * 4, false});
    thinned.stop();
    expect(thinned.kept().front() == 0 && thinned.kept().size() == 241, "frame-count thinning preserves its first frame without self-move");
    SaveRequest first;
    first.frames = {0};
    first.format = Format::png;
    first.size = 64;
    first.cutout = Cutout::none;
    first.file = root / "thinned.png";
    expect(thinned.save(first, why), "first thinned frame can still be encoded");
    for (int wait = 0; wait < 1000 && thinned.status().state == State::saving; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(thinned.status().state == State::saved, "thinning keeps its first image data intact");
    Recorder stressed;
    request.most = 1000;
    expect(stressed.start(request, why), "bounded background recording starts");
    for (int n = 0; n < 100; ++n) {
        stressed.take_frame(view());
        expect(stressed.status().pending <= 3, "background queue stays bounded");
    }
    stressed.stop();
    const auto state = stressed.status();
    const auto kept = stressed.kept();
    expect(state.taken + state.dropped == 100 && state.pending == 0 && kept.size() == state.taken,
           "every presented job is either retained or reported dropped, with no invented frames");
    expect(std::is_sorted(kept.begin(), kept.end()) && std::adjacent_find(kept.begin(), kept.end()) == kept.end(),
           "background frames commit in order");
    Recorder cancelled;
    expect(cancelled.start(request, why), "cancellable background recording starts");
    cancelled.take_frame(view());
    cancelled.discard();
    expect(cancelled.status().state == State::idle && cancelled.status().taken == 0 && cancelled.kept().empty(),
           "discard retires queued generations");
    expect(cancelled.start(request, why), "restart after cancellation");
    cancelled.take_frame(view());
    expect(cancelled.start(request, why), "restart while processing retires previous work");
    expect(cancelled.status().taken == 0 && cancelled.kept().empty(), "old pending frames never enter a new recording");
    std::uint8_t tiny = 0;
    cancelled.take_frame({&tiny, 8192, 8192, 8192 * 4, false, &tiny, 8192 * 4});
    expect(cancelled.status().state == State::failed && cancelled.status().message.find("memory limit") != std::string::npos,
           "oversized raw jobs fail before reading source pixels or allocating unbounded memory");
}

void test_capture_cropped_readback() {
    using namespace dingosdk::capture;
    for (const auto dimensions : std::array<std::array<std::uint32_t, 2>, 3>{{{800, 450}, {1920, 1080}, {2560, 720}}}) {
        const auto w = dimensions[0], h = dimensions[1];
        for (const bool skater : {false, true}) {
            Recorder full_recorder, cropped_recorder;
            RecordRequest request;
            request.area = skater ? Area{0.27f, 0.14f, 0.735f, 0.985f, true} : Area{0.655f, 0.185f, 0.86f, 0.985f, true};
            request.height = 480;
            std::string why;
            expect(full_recorder.start(request, why) && cropped_recorder.start(request, why), "cropped/full recordings start");
            const auto area = full_recorder.requested_area(w, h), wanted = area_on(request.area, w, h);
            expect(area.x == wanted.x && area.y == wanted.y && area.width == wanted.width && area.height == wanted.height,
                   "requested readback rectangle preserves menu and ultrawide mapping");
            std::vector<std::uint8_t> pixels(std::size_t{w} * h * 4);
            std::vector<float> depth(std::size_t{w} * h, 0.01f);
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < w; ++x) {
                    auto* out = pixels.data() + (std::size_t{y} * w + x) * 4;
                    out[0] = static_cast<std::uint8_t>(x);
                    out[1] = static_cast<std::uint8_t>(y);
                    out[2] = static_cast<std::uint8_t>(x + y);
                    out[3] = 255;
                    if (x > area.x + area.width / 3 && x < area.x + area.width * 2 / 3 && y > area.y + area.height / 10 &&
                        y < area.y + area.height * 9 / 10)
                        depth[std::size_t{y} * w + x] = 0.02f;
                }
            const PixelView full{pixels.data(), w, h, w * 4, true, reinterpret_cast<const std::uint8_t*>(depth.data()), w * 4};
            const auto pitch = (area.width * 4 + 255u) & ~255u;
            std::vector<std::uint8_t> copy(std::size_t{pitch} * area.height, 0xcd);
            for (std::uint32_t y = 0; y < area.height; ++y)
                std::copy_n(pixels.data() + (std::size_t{area.y + y} * w + area.x) * 4, std::size_t{area.width} * 4,
                            copy.data() + std::size_t{y} * pitch);
            PixelView cropped_view{copy.data(), area.width, area.height, pitch, true, full.depth, full.depth_pitch, area.x, area.y, w, h};
            const Rect local{0, 0, area.width, area.height};
            expect(thumbnail(full, area) == thumbnail(cropped_view, local),
                   "cropped colour thumbnails match full-screen colour coordinates");
            Image original(area.width, area.height), cropped_picture(area.width, area.height);
            expect(depth_mask(full, area, original) == depth_mask(cropped_view, local, cropped_picture) &&
                       original.rgba == cropped_picture.rgba,
                   "cropped colour origin selects the identical full-screen depth silhouette");
            full_recorder.take_frame(full);
            auto stale = cropped_view;
            ++stale.origin_x;
            cropped_recorder.take_frame(stale);
            expect(cropped_recorder.status().taken == 0, "stale readback region is ignored after an area change");
            cropped_recorder.take_frame(cropped_view);
            expect(full_recorder.status().taken == 1 && cropped_recorder.status().taken == 1 && cropped_recorder.status().depth,
                   "cropped recorder retains the same numbered depth frame");
            full_recorder.discard();
            cropped_recorder.discard();
        }
    }
}

void test_capture_hdr_pixels() {
    using namespace dingosdk::capture;
    constexpr std::uint32_t w = 1024, h = 259, pitch = w * 4 + 64;
    std::vector<std::uint8_t> packed(std::size_t{pitch} * h, 0xcd), actual;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t value = x | (y << 10) | (((x + y) & 1023) << 20) | (3u << 30);
            std::memcpy(packed.data() + std::size_t{y} * pitch + x * 4, &value, 4);
        }
    readback_rgba(packed.data(), w, h, pitch, ReadbackColour::rgb10, actual);
    bool same = true;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto* pixel = actual.data() + (std::size_t{y} * w + x) * 4;
            same = same && pixel[0] == x * 255 / 1023 && pixel[1] == y * 255 / 1023 && pixel[2] == ((x + y) & 1023) * 255 / 1023 &&
                   pixel[3] == 255;
        }
    expect(same, "parallel HDR10 conversion preserves all channel values and padded row strides");
    constexpr std::uint32_t half_pitch = w * 8 + 64;
    packed.assign(std::size_t{half_pitch} * h, 0xcd);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto n = static_cast<std::uint16_t>(y * w + x);
            const std::uint16_t channels[]{n, static_cast<std::uint16_t>(n + 7891), static_cast<std::uint16_t>(n + 23117), 0x3c00};
            std::memcpy(packed.data() + std::size_t{y} * half_pitch + x * 8, channels, 8);
        }
    readback_rgba(packed.data(), w, h, half_pitch, ReadbackColour::half_float, actual);
    same = true;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto n = static_cast<std::uint16_t>(y * w + x);
            const std::uint16_t values[]{n, static_cast<std::uint16_t>(n + 7891), static_cast<std::uint16_t>(n + 23117)};
            const auto* pixel = actual.data() + (std::size_t{y} * w + x) * 4;
            for (int c = 0; c < 3; ++c)
                same = same && pixel[c] == readback_srgb8(readback_half(values[c]));
            same = same && pixel[3] == 255;
        }
    expect(same, "HDR half lookup preserves the previous conversion for all 65536 encodings");
    expect(readback_srgb8(readback_half(0x3c00)) == 255 && readback_srgb8(readback_half(0x3800)) == 188 &&
               readback_srgb8(readback_half(0x7e00)) == 0 && readback_srgb8(readback_half(0xfc00)) == 0,
           "half conversion retains white, midpoint, NaN and negative infinity behavior");
}

void test_capture_parallel_rows() {
    using namespace dingosdk::capture;
    std::array<unsigned, 19> visits{};
    parallel_rows(19, 20000, [&](std::uint32_t first, std::uint32_t last) {
        for (auto row = first; row < last; ++row)
            ++visits[row];
    });
    expect(std::all_of(visits.begin(), visits.end(), [](unsigned count) { return count == 1; }),
           "parallel rows cover a non-divisible height exactly once");
    bool propagated = false;
    try {
        parallel_rows(19, 20000, [](std::uint32_t first, std::uint32_t last) {
            for (auto row = first; row < last; ++row)
                if (row == 17)
                    throw std::runtime_error("worker failure");
        });
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    expect(propagated, "worker failures reach capture cleanup after all workers join");
}

void test_capture_parallel_pixels() {
    using namespace dingosdk::capture;
    const std::array<std::array<std::uint32_t, 4>, 3> cases{{{97, 83, 171, 151}, {641, 479, 733, 597}, {1786, 1825, 939, 960}}};
    // Golden hashes from the serial cuff-fixed mask and original area filter.
    const std::array<std::uint64_t, 3> golden{167371848359352969ull, 16840209323894955362ull, 5533595409890215556ull};
    std::size_t at = 0;
    for (const auto& dimensions : cases) {
        const auto w = dimensions[0], h = dimensions[1];
        Image picture(w, h);
        std::uint32_t seed = 17;
        for (auto& byte : picture.rgba) {
            seed = seed * 1664525u + 1013904223u;
            byte = static_cast<std::uint8_t>(seed >> 24);
        }
        std::vector<float> depth(std::size_t{w} * h);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const float t = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
                float distance = 100.0f + 12.0f * t * t;
                if (x > w / 3 && x < w * 2 / 3 && y > h / 10 && y < h * 9 / 10)
                    distance *= 0.6f;
                if (y == h * 9 / 10 && x > w / 3 && x < w * 2 / 3)
                    distance *= 0.995f;
                const bool empty = (y < h / 12 || y % 13 == 0) && (x < w / 8 || x > w * 7 / 8);
                depth[std::size_t{y} * w + x] = empty ? 0 : 1.0f / distance;
            }
        const PixelView frame{picture.rgba.data(), w, h, w * 4, false, reinterpret_cast<const std::uint8_t*>(depth.data()), w * 4};
        const bool result = depth_mask(frame, {0, 0, w, h}, picture);
        expect(result, "golden depth fixture has a foreground silhouette");
        const auto actual = resized(picture, dimensions[2], dimensions[3]);

        std::uint64_t hash = 14695981039346656037ull;
        for (const auto byte : actual.rgba) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        expect(hash == golden[at++], "parallel depth and resize match original "
                                     "pixels including inherited backdrop rows");
    }
}

// Optional CPU profile, outside normal tests: same crop and output size as
// a large skater capture on a 4K screen. Reports costs, never asserts timing.
void profile_capture() {
    using namespace dingosdk::capture;
    constexpr std::uint32_t width = 3840, height = 2160;
    std::vector<std::uint8_t> pixels(std::size_t{width} * height * 4, 90);
    std::vector<float> depth(std::size_t{width} * height, 0.01f);
    const Rect area{1037, 302, 1786, 1825};
    for (std::uint32_t y = 400; y < 2000; ++y)
        for (std::uint32_t x = 1700; x < 2150; ++x)
            depth[std::size_t{y} * width + x] = 0.02f;
    const PixelView frame{pixels.data(), width, height, width * 4, false, reinterpret_cast<const std::uint8_t*>(depth.data()), width * 4};
    Image picture(area.width, area.height);
    for (std::size_t n = 3; n < picture.rgba.size(); n += 4)
        picture.rgba[n] = 255;
    const auto measure = [](const char* label, auto fn) {
        fn();
        const auto begin = std::chrono::steady_clock::now();
        for (int n = 0; n < 12; ++n)
            fn();
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / 12;
        std::printf("%s %.3f ms/frame\n", label, ms);
    };
    std::vector<std::uint8_t> converted(std::size_t{width} * height * 4);
    measure("serial HDR10 reference", [&] {
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint32_t value;
                std::memcpy(&value, pixels.data() + (std::size_t{y} * width + x) * 4, 4);
                auto* out = converted.data() + (std::size_t{y} * width + x) * 4;
                for (int c = 0; c < 3; ++c)
                    out[c] = static_cast<std::uint8_t>(((value >> (10 * c)) & 0x3ff) * 255 / 1023);
                out[3] = 255;
            }
    });
    measure("HDR10 conversion", [&] { readback_rgba(pixels.data(), width, height, width * 4, ReadbackColour::rgb10, converted); });
    measure("depth mask", [&] { (void)depth_mask(frame, area, picture); });
    measure("resize", [&] { (void)resized(picture, 939, 960); });
    Recorder recorder;
    RecordRequest request;
    request.area = {0.27f, 0.14f, 0.735f, 0.985f};
    request.height = 960;
    std::string why;
    if (!recorder.start(request, why)) {
        std::printf("profile failed: %s\n", why.c_str());
        return;
    }
    measure("whole recorder", [&] { recorder.take_frame(frame); });
    recorder.discard();
}

void profile_background() {
    using namespace dingosdk::capture;
    constexpr std::uint32_t w=3440,h=1440;
    std::vector<std::uint8_t> pixels(std::size_t{w}*h*4,90);
    std::vector<float> depth(std::size_t{w}*h,0.01f);
    for(std::uint32_t y=220;y<1380;++y) for(std::uint32_t x=1560;x<1950;++x) depth[std::size_t{y}*w+x]=0.02f;
    const PixelView frame{pixels.data(),w,h,w*4,false,reinterpret_cast<const std::uint8_t*>(depth.data()),w*4};
    Recorder recorder;RecordRequest request;request.area={0.27f,0.14f,0.735f,0.985f,true};request.background=true;request.height=960;
    std::string why;if(!recorder.start(request,why)){std::printf("profile start failed\n");return;}
    double total=0,maximum=0;
    const auto begin=std::chrono::steady_clock::now();
    for (int n=0;n<240;++n) {
        std::this_thread::sleep_until(begin+std::chrono::microseconds(n*1000000/60));
        const auto started=std::chrono::steady_clock::now();recorder.take_frame(frame);
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        total+=ms;maximum=std::max(maximum,ms);
    }
    recorder.stop();const auto state=recorder.status();
    std::printf("background 60 Hz: accepted %u, retained %u, dropped %u, pending %u; enqueue mean %.3f ms, max %.3f ms\n",
        state.taken,state.kept,state.dropped,state.pending,total/240,maximum);
}

int main(int argc, char** argv) {
    if(argc>1 && std::string(argv[1])=="--profile-background"){profile_background();return 0;}
    if (argc>1 && std::string(argv[1])=="--profile-capture") { profile_capture(); return 0; }
    test_capture_background();
    test_capture_cropped_readback();
    test_capture_hdr_pixels();
    test_capture_parallel_rows();
    test_capture_parallel_pixels();
    test_depth_cut_cuff_background();
    test_depth_cut_wide_and_far();
    test_depth_cut_contact();
    test_depth_cut();
    test_matte();
    test_backdrop_key(false);
    test_backdrop_key(true);
    test_geometry();
    test_png();
    test_gif();
    test_webm();
    if (argc > 1) {
        test_panel(argv[1], argc > 2 ? argv[2] : nullptr);
        test_recording(argv[1], Format::apng, false, false, 16);
        test_recording(argv[1], Format::gif, false, true, 24);
        test_recording(argv[1], Format::webm, true, false, 16);
        test_recording(argv[1], Format::png, true, true, 1);
    } else std::printf("skipped: the card and recordings (pass the fonts folder)\n");
    if (!failures) std::printf("capture tests passed\n");
    return failures ? 1 : 0;
}
