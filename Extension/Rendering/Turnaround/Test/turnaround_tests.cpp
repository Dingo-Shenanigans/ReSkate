#include "Extension/Rendering/Turnaround/turnaround_encode.h"
#include "Extension/Rendering/Turnaround/turnaround_matte.h"
#include "Extension/Rendering/Turnaround/turnaround_panel.h"
#include "Extension/Rendering/Turnaround/turnaround_session.h"

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

// Turnaround pictures without the game: matting, resizing, the parts card and the file formats.
// Run with the folder of the menu fonts (External/fonts) as the first argument to test the card too,
// and a folder as the second to save a picture of it there.
namespace {
using namespace dingosdk::turnaround;
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

// A stand-in for the game: a 1280x720 frame with a board that turns on the right of the menu, a
// background it can make dark or light (graded, as the game's post-processing does), and a piece
// of UI that never moves.
class FakeStage : public Stage {
public:
    explicit FakeStage(bool backgrounds, double spin = 0) : backgrounds_(backgrounds), spin_(spin) {}
    bool begin(std::string&) override { began = true; return true; }
    void pose(double t, bool l) override { turn = t, light = l; }
    bool changes_background() const override { return backgrounds_; }
    bool continuous() const override { return spin_ != 0; }
    bool slow_down(double ratio) override { spin_ *= ratio; ++slowed; return true; }
    void end() override { ended = true; }
    // A turning stage moves on by `spin` of a turn every frame drawn. As the game's temporal
    // smoothing does, a frame keeps a trace of the last one.
    std::vector<std::uint8_t> frame() {
        if (spin_ != 0 && !ended) turn += spin_;
        ++drawn;
        auto now = draw();
        if (!last_.empty())
            for (std::size_t i = 0; i < now.size(); ++i)
                now[i] = static_cast<std::uint8_t>((now[i] * 3 + last_[i] + 2) / 4);
        last_ = now;
        return now;
    }
    std::vector<std::uint8_t> draw() const {
        constexpr std::uint32_t w = 1280, h = 720;
        std::vector<std::uint8_t> out(std::size_t{w} * h * 4);
        // A skater sways as they stand, so the turn never quite comes back to how it started.
        const float sway = skater ? 3.0f * std::sin(static_cast<float>(drawn) * 0.21f) : 0.0f;
        const float cx = 1280 * (skater ? 0.5f : 0.76f) + sway, cy = 720 * 0.57f;
        const float half_w = 60 * std::abs(std::cos(static_cast<float>(turn) * 6.2831853f)) + 3, half_h = 230;
        const bool back = std::cos(static_cast<float>(turn) * 6.2831853f) < 0;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                float bg = light ? 225.0f : 28.0f + 10.0f * fy / h; // the dark one with a slight vignette
                float rgb[3] = {bg, bg, bg + 4};
                if (x > 860 && x < 900 && y > 140 && y < 170) rgb[0] = rgb[1] = rgb[2] = 200; // UI that never moves
                if (skater && x > 560 && x < 720 && y > 648 && y < 668) rgb[0] = rgb[1] = rgb[2] = 230; // the page numbers, just under the feet
                if (skater && x > 380 && x < 440 && y > 120 && y < 170) // a light in the scene that flickers
                    rgb[0] = rgb[1] = rgb[2] = 90.0f + 50.0f * std::sin(static_cast<float>(drawn) * 0.7f);
                // The Board screen's "Equip" prompt under the board, its leader line running in to it.
                if (!skater && ((x > 990 && x < 1060 && y > 640 && y < 664) || (x > 900 && x < 990 && y >= 641 && y <= 643)))
                    rgb[0] = rgb[1] = rgb[2] = 210;
                const float dx = std::abs(fx - cx) - half_w, dy = std::abs(fy - cy) - half_h;
                const float cover = std::clamp(0.5f - std::max(dx, dy), 0.0f, 1.0f);
                const float deck[3] = {back ? 12.0f : 40.0f, back ? 12.0f : 150.0f + 80 * fy / h, back ? 13.0f : 220.0f};
                for (int c = 0; c < 3; ++c) rgb[c] = deck[c] * cover + rgb[c] * (1 - cover);
                // With depth: a strip of the graphic down the deck's edge as dark as the backdrop, which
                // colour alone cuts away.
                if (with_depth && !skater && cover > 0.99f && std::abs(fx - cx) > half_w - 8)
                    for (int c = 0; c < 3; ++c) rgb[c] = bg + 1.0f;
                // A skater's black joint, as dark as the backdrop behind it.
                if (skater && cover > 0.99f && std::abs(fy - cy + 60) < 10 && std::abs(fx - cx) < half_w * 0.5f)
                    for (int c = 0; c < 3; ++c) rgb[c] = bg + 1.0f;
                auto* p = out.data() + (std::size_t{y} * w + x) * 4;
                for (int c = 0; c < 3; ++c) p[2 - c] = static_cast<std::uint8_t>(std::clamp(rgb[c] * 0.92f + 5, 0.0f, 255.0f));
                p[3] = 255;
            }
        return out;
    }
    // The game's depth for the frame drawn last: the board a quarter as far as the backdrop.
    std::vector<float> depth() const {
        constexpr std::uint32_t w = 1280, h = 720;
        const float sway = skater ? 3.0f * std::sin(static_cast<float>(drawn) * 0.21f) : 0.0f;
        const float cx = 1280 * (skater ? 0.5f : 0.76f) + sway, cy = 720 * 0.57f;
        const float half_w = 60 * std::abs(std::cos(static_cast<float>(turn) * 6.2831853f)) + 3, half_h = 230;
        std::vector<float> out(std::size_t{w} * h, 1.0f / 400);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                if (std::abs(static_cast<float>(x) + 0.5f - cx) < half_w && std::abs(static_cast<float>(y) + 0.5f - cy) < half_h)
                    out[std::size_t{y} * w + x] = 1.0f / 100;
        return out;
    }
    double turn{};
    bool light{}, began{}, ended{}, skater{}, with_depth{};
    unsigned drawn{}, slowed{};
private:
    bool backgrounds_;
    double spin_;
    std::vector<std::uint8_t> last_;
};

void test_session(const std::filesystem::path& fonts, bool backgrounds, Format format, bool transparent, double spin = 0,
                  Subject subject = Subject::board, std::uint32_t frames = 16, bool with_depth = false) {
    const auto load = [&](const char* name) {
        std::ifstream in(fonts / name, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    static const auto brush = load("PermanentMarker-Regular.ttf"), heading = load("Montserrat-ExtraBold.ttf"),
                      body = load("Montserrat-SemiBold.ttf");
    auto stage = std::make_shared<FakeStage>(backgrounds, spin);
    stage->skater = subject == Subject::skater;
    stage->with_depth = with_depth;
    Session session;
    Session::Inputs in;
    in.options.subject = subject;
    in.options.format = format;
    in.options.transparent = transparent;
    in.options.frames = frames;
    in.options.size = 256;
    in.options.settle = 2;
    in.title = "Test Deck: \"blue\"";
    in.parts = {{"Deck", "Test Deck"}, {"Wheels", "Round"}};
    in.fonts = {heading, body, brush};
    in.folder = std::filesystem::temp_directory_path() / "turnaround_tests";
    in.stage = stage;
    expect(session.start(in), "a session starts");
    expect(!session.start(in), "a second session waits for the first");
    for (int i = 0; i < 20000 && session.progress().phase == Phase::capturing; ++i) {
        // A turning stage turns whether or not the frame is taken, as the game does.
        const bool wanted = session.wants_frame();
        const auto pixels = stage->frame();
        if (!wanted) continue;
        const auto depth = with_depth ? stage->depth() : std::vector<float>{};
        session.take_frame({pixels.data(), 1280, 720, 1280 * 4, true, with_depth ? reinterpret_cast<const std::uint8_t*>(depth.data()) : nullptr,
                            1280 * 4});
    }
    for (int i = 0; i < 600 && session.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto progress = session.progress();
    if (progress.phase != Phase::done) std::printf("session: %s\n", progress.message.c_str());
    expect(progress.phase == Phase::done, "a session saves its turnaround");
    expect(stage->began && stage->ended, "the stage is taken over and given back");
    if (spin != 0 && frames > 40) expect(stage->slowed > 0, "a turn too fast for the frames wanted is slowed");
    std::error_code error;
    expect(std::filesystem::file_size(progress.file, error) > 1000, "the turnaround file is written");
    if (format == Format::apng && transparent) {
        // The board is taller than the area it was taken from is wide: it must still be whole, with
        // see-through room above and below it in every frame.
        std::ifstream file(progress.file, std::ios::binary);
        const std::vector<std::uint8_t> png(std::istreambuf_iterator<char>(file), {});
        expect(png.size() > 1000, "the turnaround can be read back");
    }
    const auto name = progress.file.filename().string();
    expect(name.starts_with("Test-Deck-blue-") && name.ends_with(extension(format)), "the file is named after the deck");
    if (with_depth) expect(progress.message.find("by depth") != std::string::npos, "a turnaround with depth is cut out by it");
}

int main(int argc, char** argv) {
    test_matte();
    test_backdrop_key(false);
    test_backdrop_key(true);
    test_geometry();
    test_png();
    test_gif();
    test_webm();
    if (argc > 1) {
        test_panel(argv[1], argc > 2 ? argv[2] : nullptr);
        test_session(argv[1], true, Format::apng, true);
        test_session(argv[1], false, Format::gif, true);
        test_session(argv[1], true, Format::webm, false);
        test_session(argv[1], false, Format::apng, true, 1.0 / 37.3); // turned by the stick, at a speed it must find
        test_session(argv[1], false, Format::apng, true, 1.0 / 41.7, Subject::skater); // the Skaters screen
        test_session(argv[1], false, Format::webm, true, 1.0 / 37.3, Subject::board, 120); // too fast for 120 frames: slowed
        test_session(argv[1], false, Format::apng, true, 1.0 / 37.3, Subject::board, 16, true); // cut out by the game's depth
    }
    else std::printf("skipped: the parts card and sessions (pass the fonts folder)\n");
    if (!failures) std::printf("turnaround tests passed\n");
    return failures ? 1 : 0;
}
