#include "turnaround_encode.h"
#include "turnaround_vp8.h"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string_view>

// WebM (Matroska) with VP8 video, and transparency the way Chrome, Firefox and ffmpeg read it: the
// alpha as a second VP8 picture (its luma) in each frame's BlockAdditional, the track marked with
// AlphaMode 1.
namespace dingosdk::turnaround {
namespace {
// EBML element ids and sizes.
void put_id(std::vector<std::uint8_t>& out, std::uint32_t id) {
    int bytes = id > 0xffffff ? 4 : id > 0xffff ? 3 : id > 0xff ? 2 : 1;
    while (bytes--) out.push_back(static_cast<std::uint8_t>(id >> (bytes * 8)));
}
// Always 8 bytes: sizes are known, and fixed-width ones keep this simple.
void put_size(std::vector<std::uint8_t>& out, std::uint64_t size) {
    out.push_back(0x01);
    for (int i = 6; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(size >> (i * 8)));
}
void element(std::vector<std::uint8_t>& out, std::uint32_t id, const std::vector<std::uint8_t>& body) {
    put_id(out, id);
    put_size(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
}
void uint_element(std::vector<std::uint8_t>& out, std::uint32_t id, std::uint64_t value) {
    std::vector<std::uint8_t> body;
    int bytes = 1;
    while (bytes < 8 && (value >> (bytes * 8))) ++bytes;
    for (int i = bytes - 1; i >= 0; --i) body.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    element(out, id, body);
}
void string_element(std::vector<std::uint8_t>& out, std::uint32_t id, std::string_view text) {
    element(out, id, std::vector<std::uint8_t>(text.begin(), text.end()));
}
void float_element(std::vector<std::uint8_t>& out, std::uint32_t id, double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, 8);
    std::vector<std::uint8_t> body;
    for (int i = 7; i >= 0; --i) body.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    element(out, id, body);
}

// BT.601 limited range, the colour VP8 decoders assume.
vp8::Planes to_planes(const Image& image, bool alpha) {
    vp8::Planes planes;
    planes.width = image.width;
    planes.height = image.height;
    const std::size_t cw = (image.width + 1) / 2, ch = (image.height + 1) / 2;
    planes.y.resize(std::size_t{image.width} * image.height);
    planes.u.assign(cw * ch, 128);
    planes.v.assign(cw * ch, 128);
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto* p = image.at(x, y);
            planes.y[std::size_t{y} * image.width + x] =
                alpha ? p[3] : static_cast<std::uint8_t>(std::lround(16 + 0.2568 * p[0] + 0.5041 * p[1] + 0.0979 * p[2]));
        }
    if (alpha) return planes;
    for (std::size_t cy = 0; cy < ch; ++cy)
        for (std::size_t cx = 0; cx < cw; ++cx) {
            double r = 0, g = 0, b = 0;
            int n = 0;
            for (std::size_t dy = 0; dy < 2; ++dy)
                for (std::size_t dx = 0; dx < 2; ++dx) {
                    const auto x = cx * 2 + dx, y = cy * 2 + dy;
                    if (x >= image.width || y >= image.height) continue;
                    const auto* p = image.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                    r += p[0], g += p[1], b += p[2], ++n;
                }
            r /= n, g /= n, b /= n;
            planes.u[cy * cw + cx] = static_cast<std::uint8_t>(std::clamp(std::lround(128 - 0.1482 * r - 0.2910 * g + 0.4392 * b), 16L, 240L));
            planes.v[cy * cw + cx] = static_cast<std::uint8_t>(std::clamp(std::lround(128 + 0.4392 * r - 0.3678 * g - 0.0714 * b), 16L, 240L));
        }
    return planes;
}

// Fully transparent pixels take the colour of the nearest opaque ones along their row, so VP8's
// blocks do not smear an arbitrary hidden colour into the board's edge.
Image bleed_colour(const Image& image) {
    Image out = image;
    for (std::uint32_t y = 0; y < out.height; ++y) {
        std::int64_t last = -1;
        for (std::uint32_t x = 0; x < out.width; ++x) {
            if (out.at(x, y)[3]) {
                if (last < 0) for (std::uint32_t k = 0; k < x; ++k) std::copy_n(out.at(x, y), 3, out.at(k, y));
                last = x;
            } else if (last >= 0) {
                std::copy_n(out.at(static_cast<std::uint32_t>(last), y), 3, out.at(x, y));
            }
        }
    }
    return out;
}
} // namespace

std::vector<std::uint8_t> encode_webm(const Animation& animation, const WebmOptions& options) {
    if (animation.frames.empty() || animation.frames.front().empty()) throw std::invalid_argument("No frames to save.");
    const auto width = animation.frames.front().width, height = animation.frames.front().height;
    for (const auto& frame : animation.frames)
        if (frame.width != width || frame.height != height) throw std::invalid_argument("The frames are not all the same size.");
    bool transparent = false;
    for (const auto& frame : animation.frames)
        for (std::size_t i = 3; i < frame.rgba.size() && !transparent; i += 4) transparent = frame.rgba[i] != 255;
    const auto fps = std::clamp<std::uint32_t>(animation.fps, 1, 120);

    std::vector<std::uint8_t> out;
    {
        std::vector<std::uint8_t> ebml;
        uint_element(ebml, 0x4286, 1);  // EBMLVersion
        uint_element(ebml, 0x42f7, 1);  // EBMLReadVersion
        uint_element(ebml, 0x42f2, 4);  // EBMLMaxIDLength
        uint_element(ebml, 0x42f3, 8);  // EBMLMaxSizeLength
        string_element(ebml, 0x4282, "webm");
        uint_element(ebml, 0x4287, 4);  // DocTypeVersion (BlockAdditions, AlphaMode)
        uint_element(ebml, 0x4285, 2);  // DocTypeReadVersion
        element(out, 0x1a45dfa3, ebml);
    }
    std::vector<std::uint8_t> segment;
    {
        std::vector<std::uint8_t> info;
        uint_element(info, 0x2ad7b1, 1000000); // TimestampScale: milliseconds
        float_element(info, 0x4489, 1000.0 * static_cast<double>(animation.frames.size()) / fps); // Duration
        string_element(info, 0x4d80, "ReSkate Fit Check");
        string_element(info, 0x5741, "ReSkate Fit Check");
        element(segment, 0x1549a966, info);
    }
    {
        std::vector<std::uint8_t> video, track, tracks;
        uint_element(video, 0xb0, width);
        uint_element(video, 0xba, height);
        if (transparent) uint_element(video, 0x53c0, 1); // AlphaMode
        uint_element(track, 0xd7, 1);          // TrackNumber
        uint_element(track, 0x73c5, 1);        // TrackUID
        uint_element(track, 0x83, 1);          // TrackType: video
        uint_element(track, 0x9c, 0);          // FlagLacing
        uint_element(track, 0x23e383, 1000000000ull / fps); // DefaultDuration (ns)
        string_element(track, 0x86, "V_VP8");
        element(track, 0xe0, video);
        element(tracks, 0xae, track);
        element(segment, 0x1654ae6b, tracks);
    }
    // Clusters of at most 20 seconds, so the 16-bit block timestamps stay in range.
    std::vector<std::uint8_t> cluster;
    std::uint64_t cluster_start = 0;
    const auto close_cluster = [&] {
        if (!cluster.empty()) element(segment, 0x1f43b675, cluster);
        cluster.clear();
    };
    for (std::size_t i = 0; i < animation.frames.size(); ++i) {
        const std::uint64_t time = i * 1000 / fps;
        if (cluster.empty() || time - cluster_start >= 20000) {
            close_cluster();
            cluster_start = time;
            uint_element(cluster, 0xe7, cluster_start); // Timestamp
        }
        const auto& frame = animation.frames[i];
        const auto picture = vp8::encode_keyframe(to_planes(transparent ? bleed_colour(frame) : frame, false), options.quality);
        const auto relative = static_cast<std::int16_t>(time - cluster_start);
        std::vector<std::uint8_t> block{0x81, static_cast<std::uint8_t>(static_cast<std::uint16_t>(relative) >> 8),
                                        static_cast<std::uint8_t>(relative), 0x00};
        block.insert(block.end(), picture.begin(), picture.end());
        if (!transparent) {
            // A SimpleBlock, flagged as a key frame.
            block[3] = 0x80;
            element(cluster, 0xa3, block);
            continue;
        }
        std::vector<std::uint8_t> group, additions, more;
        element(group, 0xa1, block); // Block
        uint_element(more, 0xee, 1); // BlockAddID
        element(more, 0xa5, vp8::encode_keyframe(to_planes(frame, true), options.alpha_quality)); // BlockAdditional
        element(additions, 0xa6, more);
        element(group, 0x75a1, additions);
        element(cluster, 0xa0, group);
    }
    close_cluster();
    element(out, 0x18538067, segment);
    return out;
}
} // namespace dingosdk::turnaround
