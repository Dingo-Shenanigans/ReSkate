#include "capture_recorder.h"
#include "capture_matte.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace dingosdk::capture {
namespace {
// `area` of a frame, copied out in the frame's own channel order.
std::vector<std::uint8_t> copy_area(const PixelView& frame, Rect area) {
    std::vector<std::uint8_t> out(std::size_t{area.width} * area.height * 4);
    for (std::uint32_t y = 0; y < area.height; ++y)
        std::copy_n(frame.data + std::size_t{area.y + y} * frame.pitch + std::size_t{area.x} * 4, std::size_t{area.width} * 4,
                    out.data() + std::size_t{y} * area.width * 4);
    return out;
}

Image opaque_image(const PixelView& view) {
    Image out(view.width, view.height);
    for (std::uint32_t y = 0; y < view.height; ++y)
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const auto rgb = view.rgb(x, y);
            auto* p = out.at(x, y);
            p[0] = rgb[0], p[1] = rgb[1], p[2] = rgb[2], p[3] = 255;
        }
    return out;
}
} // namespace

// What turns, by the game's depth: nearer than the backdrop behind it. The backdrop's distance for
// each row is that of the area's outer columns (the backdrop, or the floor where the row meets it:
// the camera is level, so the floor is about as far all along a row), and what is 8% nearer
// than that is the board or skater. The game's UI is never in the depth, and neither are a colour's
// likeness to the backdrop or the floor's reflection. Into `picture`'s alpha; false when the depth
// shows nothing there.
bool depth_mask(const PixelView& frame, Rect area, Image& picture) {
    if (!frame.depth || picture.width != area.width || picture.height != area.height) return false;
    const auto w = area.width, h = area.height;
    const auto margin = std::max<std::uint32_t>(2, w * 6 / 100);
    std::vector<float> edge;
    float behind = 0;
    std::size_t found = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        edge.clear();
        for (std::uint32_t x = 0; x < w; x = x + 1 == margin ? w - margin : x + 1) {
            const float d = frame.depth_at(area.x + x, area.y + y);
            if (d > 1e-7f) edge.push_back(1.0f / d);
        }
        if (!edge.empty()) {
            std::nth_element(edge.begin(), edge.begin() + static_cast<std::ptrdiff_t>(edge.size() / 2), edge.end());
            behind = edge[edge.size() / 2];
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            const float d = frame.depth_at(area.x + x, area.y + y);
            const bool in_front = d > 1e-7f && (behind <= 0 || 1.0f / d < behind * 0.92f);
            picture.at(x, y)[3] = in_front ? 255 : 0;
            found += in_front;
        }
    }
    // The soles: where the feet meet the floor the depth cannot tell them from it. Carried down from
    // the lowest part of the skater in each column while the colour is unlike the floor's along that
    // row (the area's outer columns again), a little way at most.
    std::vector<std::array<int, 3>> floor_colour(h);
    std::vector<int> channel;
    for (std::uint32_t y = 0; y < h; ++y)
        for (int c = 0; c < 3; ++c) {
            channel.clear();
            for (std::uint32_t x = 0; x < w; x = x + 1 == margin ? w - margin : x + 1) channel.push_back(picture.at(x, y)[c]);
            std::nth_element(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(channel.size() / 2), channel.end());
            floor_colour[y][static_cast<std::size_t>(c)] = channel[channel.size() / 2];
        }
    const auto reach = std::max<std::uint32_t>(2, h / 25);
    for (std::uint32_t x = margin; x + margin < w; ++x) {
        std::uint32_t lowest = h;
        for (std::uint32_t y = h; y-- > 0;)
            if (picture.at(x, y)[3]) { lowest = y; break; }
        if (lowest >= h) continue;
        for (std::uint32_t y = lowest + 1; y < h && y <= lowest + reach; ++y) {
            auto* p = picture.at(x, y);
            int off = 0;
            for (int c = 0; c < 3; ++c) off = std::max(off, std::abs(p[c] - floor_colour[y][static_cast<std::size_t>(c)]));
            if (off <= 24) break;
            p[3] = 255;
        }
    }
    return found > 64;
}

// Keeps only what turned, and says where it is. What moves over the turn (opaque and unlike the
// first frame) is the subject; what is solid in any frame, changes at all, and is joined to that is
// the rest of it (the nose and tail sit on the axis the board turns about and change little);
// everything else in the area is the game's UI, and is cleared from every frame. A rim two pixels wide is kept round it
// for the soft edges.
// The pixels the game drew the same in every frame, as captured (before the backdrop is keyed out,
// which can shade a still pixel differently from frame to frame): the game's UI (a prompt, its
// leader line) stays exactly as it is, while every part of what turns, its nose and tail too,
// takes the light differently as it goes round.
std::vector<std::uint8_t> unchanging(const std::vector<Image>& frames) {
    const auto& first = frames.front();
    std::vector<std::uint8_t> low(first.rgba), high(first.rgba), out(first.rgba.size() / 4);
    for (const auto& frame : frames)
        for (std::size_t i = 0; i < frame.rgba.size(); ++i)
            low[i] = std::min(low[i], frame.rgba[i]), high[i] = std::max(high[i], frame.rgba[i]);
    for (std::size_t p = 0; p < out.size(); ++p) {
        int spread = 0;
        for (std::size_t c = 0; c < 3; ++c) spread = std::max(spread, high[p * 4 + c] - low[p * 4 + c]);
        out[p] = spread <= 3;
    }
    return out;
}

Rect keep_moving(std::vector<Image>& frames, const std::vector<std::uint8_t>& fixed) {
    const auto& first = frames.front();
    const auto w = first.width, h = first.height;
    std::vector<std::uint8_t> moved(std::size_t{w} * h), solid(std::size_t{w} * h);
    for (const auto& frame : frames)
        for (std::size_t p = 0, i = 0; p < moved.size(); ++p, i += 4) {
            const auto alpha = frame.rgba[i + 3];
            if (alpha >= 64 && !fixed[p]) solid[p] = 1;
            if (moved[p] || fixed[p] || alpha < 16) continue;
            int change = 0;
            for (std::size_t c = 0; c < 4; ++c) change = std::max(change, std::abs(frame.rgba[i + c] - first.rgba[i + c]));
            if (change > 24) moved[p] = 1;
        }
    // Flood from what moved over what is solid, one piece at a time: each piece's size (in pixels
    // that moved) and bounds.
    std::vector<std::uint32_t> piece(moved.size()); // 0: none, else the piece's number + 1
    struct Piece { std::size_t moved{}; std::uint32_t x0{}, y0{}, x1{}, y1{}; };
    std::vector<Piece> pieces;
    std::vector<std::uint32_t> todo;
    for (std::uint32_t start = 0; start < moved.size(); ++start) {
        if (!moved[start] || piece[start]) continue;
        pieces.push_back({0, w, h, 0, 0});
        const auto label = static_cast<std::uint32_t>(pieces.size());
        auto& info = pieces.back();
        piece[start] = label;
        todo.push_back(start);
        while (!todo.empty()) {
            const auto p = todo.back();
            todo.pop_back();
            const auto x = static_cast<std::int64_t>(p % w), y = static_cast<std::int64_t>(p / w);
            info.moved += moved[p];
            info.x0 = std::min(info.x0, static_cast<std::uint32_t>(x)), info.x1 = std::max(info.x1, static_cast<std::uint32_t>(x + 1));
            info.y0 = std::min(info.y0, static_cast<std::uint32_t>(y)), info.y1 = std::max(info.y1, static_cast<std::uint32_t>(y + 1));
            for (std::int64_t dy = -1; dy <= 1; ++dy)
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const auto nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const auto q = static_cast<std::uint32_t>(ny * w + nx);
                    if (!piece[q] && (solid[q] || moved[q])) piece[q] = label, todo.push_back(q);
                }
        }
    }
    // What turns is the biggest piece, with any piece centred within its bounds (a board held
    // apart from the hand); pieces elsewhere are the scene's own movement (a light, its haze).
    std::vector<std::uint8_t> kept(moved.size());
    if (!pieces.empty()) {
        const auto biggest = static_cast<std::size_t>(std::max_element(pieces.begin(), pieces.end(), [](const Piece& l, const Piece& r) {
                              return l.moved < r.moved;
                          }) - pieces.begin());
        const auto& big = pieces[biggest];
        std::vector<std::uint8_t> keep(pieces.size());
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const auto cx = (pieces[i].x0 + pieces[i].x1) / 2, cy = (pieces[i].y0 + pieces[i].y1) / 2;
            keep[i] = i == biggest || (cx >= big.x0 && cx < big.x1 && cy >= big.y0 && cy < big.y1);
        }
        for (std::size_t p = 0; p < kept.size(); ++p) kept[p] = piece[p] && keep[piece[p] - 1];
    }
    // The rim (of what changes: never a still prompt touching the edge).
    for (int pass = 0; pass < 2; ++pass) {
        auto grown = kept;
        const auto add = [&](std::size_t q) { if (!fixed[q]) grown[q] = 1; };
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                if (!kept[std::size_t{y} * w + x]) continue;
                if (x > 0) add(std::size_t{y} * w + x - 1);
                if (x + 1 < w) add(std::size_t{y} * w + x + 1);
                if (y > 0) add(std::size_t{y - 1} * w + x);
                if (y + 1 < h) add(std::size_t{y + 1} * w + x);
            }
        kept = std::move(grown);
    }
    std::uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            if (kept[std::size_t{y} * w + x] && solid[std::size_t{y} * w + x])
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
    for (auto& frame : frames)
        for (std::size_t p = 0; p < kept.size(); ++p)
            if (!kept[p]) std::fill_n(frame.rgba.data() + p * 4, 4, std::uint8_t{0});
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}
// A 24x48 grey thumbnail of a frame's area, for telling how far round the board is and whether it
// has stopped.
std::vector<float> thumbnail(const PixelView& frame, Rect area) {
    constexpr std::uint32_t tw = 24, th = 48;
    std::vector<float> out(tw * th);
    for (std::uint32_t ty = 0; ty < th; ++ty)
        for (std::uint32_t tx = 0; tx < tw; ++tx) {
            const auto x0 = tx * area.width / tw, x1 = std::max(x0 + 1, (tx + 1) * area.width / tw);
            const auto y0 = ty * area.height / th, y1 = std::max(y0 + 1, (ty + 1) * area.height / th);
            float sum = 0;
            for (auto y = y0; y < y1; y += 2)
                for (auto x = x0; x < x1; x += 2) {
                    const auto rgb = frame.rgb(area.x + x, area.y + y);
                    sum += 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
                }
            out[ty * tw + tx] = sum / static_cast<float>(((x1 - x0 + 1) / 2) * ((y1 - y0 + 1) / 2));
        }
    return out;
}
float unlike(const std::vector<float>& a, const std::vector<float>& b) noexcept {
    float sum = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) sum += std::abs(a[i] - b[i]);
    return sum / static_cast<float>(std::max<std::size_t>(a.size(), 1));
}
Rect area_on(const Area& a, std::uint32_t width, std::uint32_t height) noexcept {
    const double frame_width = a.menu ? std::min<double>(width, height * 21.0 / 9.0) : width;
    const double frame_left = (width - frame_width) / 2;
    const auto clamp01 = [](float v) { return std::clamp(static_cast<double>(v), 0.0, 1.0); };
    const auto x0 = static_cast<std::uint32_t>(frame_left + frame_width * clamp01(a.left));
    const auto x1 = static_cast<std::uint32_t>(frame_left + frame_width * clamp01(a.right));
    const auto y0 = static_cast<std::uint32_t>(height * clamp01(a.top)), y1 = static_cast<std::uint32_t>(height * clamp01(a.bottom));
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, std::min(x1, width) - x0, std::min(y1, height) - y0};
}

const char* extension(Format format) noexcept {
    switch (format) {
    case Format::apng:
    case Format::png: return "png";
    case Format::webm: return "webm";
    default: return "gif";
    }
}

bool Recorder::start(const RecordRequest& request, std::string& why) {
    if (saving_.load()) {
        why = "a capture is being saved";
        return false;
    }
    std::lock_guard lock(mutex_);
    request_ = request;
    request_.most = std::clamp<std::uint32_t>(request.most, 1, 20000);
    area_ = {};
    skip_ = request.skip;
    stride_ = 1;
    started_ = std::chrono::steady_clock::now();
    pictures_.clear(), kept_at_.clear(), kept_depth_.clear(), thumbnails_.clear();
    status_ = {State::recording, 0, 0, false, {}, {}};
    return true;
}

void Recorder::stop() noexcept {
    std::lock_guard lock(mutex_);
    if (status_.state == State::recording) status_.state = State::stopped;
}

void Recorder::discard() noexcept {
    if (saving_.load()) return;
    std::lock_guard lock(mutex_);
    pictures_.clear(), pictures_.shrink_to_fit();
    kept_at_.clear(), kept_depth_.clear(), thumbnails_.clear(), thumbnails_.shrink_to_fit();
    status_ = {};
}

Status Recorder::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

float Recorder::compare(std::uint32_t a, std::uint32_t b) const {
    std::lock_guard lock(mutex_);
    if (a >= thumbnails_.size() || b >= thumbnails_.size()) return -1;
    return unlike(thumbnails_[a], thumbnails_[b]);
}

std::vector<std::uint32_t> Recorder::kept() const {
    std::lock_guard lock(mutex_);
    return kept_at_;
}

bool Recorder::active() const noexcept {
    std::lock_guard lock(mutex_);
    return status_.state == State::recording;
}

void Recorder::fail(std::string message) {
    status_.state = State::failed;
    status_.message = std::move(message);
}

bool Recorder::wants_frame() noexcept {
    std::lock_guard lock(mutex_);
    if (status_.state != State::recording) return false;
    // However it goes, a recording lets go within its time.
    if (std::chrono::steady_clock::now() - started_ > std::chrono::duration<double>(request_.seconds)) {
        status_.state = State::stopped;
        status_.message = "the recording reached its time";
        return false;
    }
    if (skip_) {
        --skip_;
        return false;
    }
    return true;
}

void Recorder::take_frame(const PixelView& frame) noexcept {
    try {
        Rect area;
        RecordRequest request;
        {
            std::lock_guard lock(mutex_);
            if (status_.state != State::recording) return;
            if (!frame.data || !frame.width || !frame.height) {
                fail("the game's picture could not be read");
                return;
            }
            if (area_.empty()) {
                area_ = area_on(request_.area, frame.width, frame.height);
                if (area_.empty()) {
                    fail("the area is not on the screen");
                    return;
                }
            }
            area = area_;
            request = request_;
        }
        // The heavy part outside the lock: the area copied, cut out, and made the size it is kept at.
        auto small = thumbnail(frame, area);
        const auto copy = copy_area(frame, area);
        auto picture = opaque_image({copy.data(), area.width, area.height, area.width * 4, frame.bgra});
        const bool by_depth = request.depth && depth_mask(frame, area, picture);
        if (!by_depth)
            for (std::size_t i = 3; i < picture.rgba.size(); i += 4) picture.rgba[i] = 255;
        if (request.height && request.height < picture.height)
            picture = resized(picture, std::max<std::uint32_t>(1, picture.width * request.height / picture.height), request.height);
        const auto most = std::clamp<std::size_t>(kept_bytes / std::max<std::size_t>(picture.rgba.size(), 1), 60, most_kept);
        std::lock_guard lock(mutex_);
        if (status_.state != State::recording) return;
        const auto n = static_cast<std::uint32_t>(thumbnails_.size());
        thumbnails_.push_back(std::move(small));
        if (n % stride_ == 0) {
            kept_at_.push_back(n);
            kept_depth_.push_back(by_depth);
            pictures_.push_back(std::move(picture));
            if (pictures_.size() > most) { // keep every other one, and from now on every other
                std::size_t w = 0;
                for (std::size_t r = 0; r < pictures_.size(); r += 2)
                    pictures_[w] = std::move(pictures_[r]), kept_depth_[w] = kept_depth_[r], kept_at_[w++] = kept_at_[r];
                pictures_.resize(w), kept_at_.resize(w), kept_depth_.resize(w);
                stride_ *= 2;
            }
        }
        status_.taken = n + 1;
        status_.kept = static_cast<std::uint32_t>(pictures_.size());
        status_.depth = !kept_depth_.empty() && std::all_of(kept_depth_.begin(), kept_depth_.end(), [](bool d) { return d; });
        if (status_.taken >= request_.most) status_.state = State::stopped;
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_);
        fail(error.what());
    } catch (...) {
        std::lock_guard lock(mutex_);
        fail("the frame could not be kept");
    }
}

bool Recorder::save(SaveRequest request, std::string& why) {
    if (request.frames.empty() || request.frames.size() > most_saved) {
        why = "a capture saves 1 to " + std::to_string(most_saved) + " frames";
        return false;
    }
    if (saving_.exchange(true)) {
        why = "a capture is being saved";
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (pictures_.empty()) {
            saving_.store(false);
            why = "nothing was recorded";
            return false;
        }
        status_.state = State::saving;
        status_.message.clear();
        status_.file.clear();
    }
    std::thread([this, request = std::move(request)]() mutable { encode(std::move(request)); }).detach();
    return true;
}

void Recorder::encode(SaveRequest o) noexcept {
    try {
        std::vector<Image> frames;
        bool by_depth{};
        {
            std::lock_guard lock(mutex_);
            // Each frame asked for: the nearest one kept.
            for (const auto wanted : o.frames) {
                std::size_t best = 0;
                for (std::size_t k = 1; k < kept_at_.size(); ++k)
                    if (std::llabs(static_cast<long long>(kept_at_[k]) - wanted) < std::llabs(static_cast<long long>(kept_at_[best]) - wanted)) best = k;
                frames.push_back(pictures_[best]);
            }
            by_depth = std::all_of(kept_depth_.begin(), kept_depth_.end(), [](bool d) { return d; });
        }
        if (o.cutout == Cutout::depth && !by_depth) throw std::runtime_error("not every frame had the game's depth");
        const bool depth_cut = o.cutout == Cutout::depth || (o.cutout == Cutout::automatic && by_depth);
        const bool colour_cut = o.cutout == Cutout::colour || (o.cutout == Cutout::automatic && !by_depth);
        if (!depth_cut)
            for (auto& frame : frames)
                for (std::size_t i = 3; i < frame.rgba.size(); i += 4) frame.rgba[i] = 255;
        Rect through{0, 0, frames.front().width, frames.front().height};
        if (o.cutout != Cutout::none) {
            const auto fixed = depth_cut ? std::vector<std::uint8_t>(frames.front().rgba.size() / 4) : unchanging(frames);
            if (colour_cut) key_against_backdrop(frames, o.solid);
            // A still cut out by depth: the depth alone says what it is (nothing moves in one frame).
            if (depth_cut && frames.size() < 2) through = content_bounds(frames.front());
            else through = keep_moving(frames, fixed);
            if (through.empty()) throw std::runtime_error("nothing that moved was found in the area");
        }
        const auto margin = std::max(through.width, through.height) / 25 + 2;
        const auto size = std::clamp<std::uint32_t>(o.size, 64, 2048);
        std::optional<Image> card;
        if (!o.card.empty() && !o.fonts.heading.empty()) card = render_parts_panel(o.card_title, o.card, size, o.fonts);
        Animation animation;
        animation.fps = std::clamp<std::uint32_t>(o.fps, 1, 60);
        for (auto& frame : frames) {
            auto shown = fitted(square_around(frame, through, margin), size);
            frame = {};
            if (card) shown = side_by_side(shown, *card);
            if (o.background) shown = over_matte(shown, *o.background);
            animation.frames.push_back(std::move(shown));
        }
        std::vector<std::uint8_t> bytes;
        switch (o.format) {
        case Format::apng: bytes = encode_apng(animation); break;
        case Format::png: bytes = encode_png(animation.frames.front()); break;
        case Format::webm: bytes = encode_webm(animation, {.quality = 1, .alpha_quality = 0}); break; // the highest VP8 has
        default: bytes = encode_gif(animation, {.fringe = o.background.value_or(Rgb{24, 25, 28}), .dither = true}); break;
        }
        std::filesystem::create_directories(o.file.parent_path());
        std::ofstream out(o.file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("the file could not be written");
        std::lock_guard lock(mutex_);
        status_.state = State::saved;
        status_.file = o.file;
        status_.message = depth_cut ? "cut out by depth" : colour_cut ? "cut out by colour" : "not cut out";
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_);
        fail(std::string("not saved: ") + error.what());
    } catch (...) {
        std::lock_guard lock(mutex_);
        fail("not saved");
    }
    saving_.store(false);
}
} // namespace dingosdk::capture
