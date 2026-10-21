#include "turnaround_vp8.h"
#include "turnaround_vp8_tables.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <stdexcept>

// Prediction and reconstruction follow libvpx's decoder exactly (vp8/decoder/decodeframe.c,
// vp8/common/idctllm.c, reconintra.c): the encoder predicts from what the decoder will have, so
// any difference would build up across the frame.
namespace dingosdk::turnaround::vp8 {
namespace {
// --- the boolean entropy coder (RFC 6386, section 7.3)
struct BoolEncoder {
    std::vector<std::uint8_t> out;
    std::uint32_t range = 255, bottom = 0;
    int bit_count = 24;
    void carry() {
        for (auto i = out.size(); i-- > 0;) {
            if (out[i] == 255) { out[i] = 0; continue; }
            ++out[i];
            break;
        }
    }
    void put(bool bit, int probability) {
        const std::uint32_t split = 1 + (((range - 1) * static_cast<std::uint32_t>(probability)) >> 8);
        if (bit) bottom += split, range -= split;
        else range = split;
        while (range < 128) {
            range <<= 1;
            if (bottom & (1u << 31)) carry();
            bottom <<= 1;
            if (!--bit_count) {
                out.push_back(static_cast<std::uint8_t>(bottom >> 24));
                bottom &= (1u << 24) - 1;
                bit_count = 8;
            }
        }
    }
    void literal(std::uint32_t value, int bits) {
        for (int b = bits - 1; b >= 0; --b) put((value >> b) & 1, 128);
    }
    std::vector<std::uint8_t> finish() {
        for (int i = 0; i < 32; ++i) put(false, 128); // as libvpx's vp8_stop_encode
        return std::move(out);
    }
};

enum Mode { dc_pred, v_pred, h_pred, tm_pred };
constexpr std::array<int, 16> zigzag{0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
constexpr std::array<int, 17> bands{0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
constexpr std::uint8_t pcat3[] = {173, 148, 140}, pcat4[] = {176, 155, 140, 135}, pcat5[] = {180, 157, 141, 134, 130},
                       pcat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129};
constexpr std::array<std::uint8_t, 4> kf_ymode_prob{145, 156, 163, 128};
constexpr std::array<std::uint8_t, 3> kf_uv_mode_prob{142, 114, 183};
enum BlockType { y_after_y2 = 0, y2 = 1, chroma = 2 };

const std::uint8_t* probabilities(int type, int band, int context) {
    return default_coef_probs + ((type * 8 + band) * 3 + context) * 11;
}

// One block's tokens, its quantized coefficients in raster order; the "has coefficients" flag
// the neighbours' contexts take.
bool put_tokens(BoolEncoder& e, const std::array<int, 16>& raster, int type, int first, int context) {
    int last = -1;
    for (int i = first; i < 16; ++i)
        if (raster[static_cast<std::size_t>(zigzag[static_cast<std::size_t>(i)])]) last = i;
    const auto* p = probabilities(type, bands[static_cast<std::size_t>(first)], context);
    if (last < 0) {
        e.put(false, p[0]);
        return false;
    }
    e.put(true, p[0]);
    for (int i = first; i <= last; ++i) {
        const int v = raster[static_cast<std::size_t>(zigzag[static_cast<std::size_t>(i)])], a = std::abs(v);
        const auto next = static_cast<std::size_t>(i + 1);
        if (!a) {
            e.put(false, p[1]);
            p = probabilities(type, bands[next], 0);
            continue;
        }
        e.put(true, p[1]);
        int next_context = 2;
        if (a == 1) {
            e.put(false, p[2]);
            next_context = 1;
        } else {
            e.put(true, p[2]);
            if (a <= 4) {
                e.put(false, p[3]);
                if (a == 2) e.put(false, p[4]);
                else e.put(true, p[4]), e.put(a == 4, p[5]);
            } else {
                e.put(true, p[3]);
                if (a <= 10) {
                    e.put(false, p[6]);
                    if (a <= 6) e.put(false, p[7]), e.put(a == 6, 159);
                    else {
                        e.put(true, p[7]);
                        const int extra = a - 7;
                        e.put((extra >> 1) & 1, 165);
                        e.put(extra & 1, 145);
                    }
                } else {
                    e.put(true, p[6]);
                    const int category = a <= 18 ? 0 : a <= 34 ? 1 : a <= 66 ? 2 : 3;
                    static constexpr int base[] = {11, 19, 35, 67}, bits[] = {3, 4, 5, 11};
                    static constexpr const std::uint8_t* table[] = {pcat3, pcat4, pcat5, pcat6};
                    e.put(category >> 1, p[8]);
                    e.put(category & 1, p[9 + (category >> 1)]);
                    const int extra = a - base[category];
                    for (int b = bits[category] - 1, k = 0; b >= 0; --b, ++k) e.put((extra >> b) & 1, table[category][k]);
                }
            }
        }
        e.put(v < 0, 128);
        if (next == 16) break;
        p = probabilities(type, bands[next], next_context);
        e.put(i != last, p[0]); // more to come, or the end of the block
    }
    return true;
}

// --- transforms (libvpx: vp8/encoder/dct.c, vp8/common/idctllm.c)
void forward_dct(const short* in, short* out) { // in: 4x4 raster
    short temp[16];
    for (int i = 0; i < 4; ++i) {
        const short* ip = in + i * 4;
        short* op = temp + i * 4;
        const int a1 = (ip[0] + ip[3]) * 8, b1 = (ip[1] + ip[2]) * 8, c1 = (ip[1] - ip[2]) * 8, d1 = (ip[0] - ip[3]) * 8;
        op[0] = static_cast<short>(a1 + b1);
        op[2] = static_cast<short>(a1 - b1);
        op[1] = static_cast<short>((c1 * 2217 + d1 * 5352 + 14500) >> 12);
        op[3] = static_cast<short>((d1 * 2217 - c1 * 5352 + 7500) >> 12);
    }
    for (int i = 0; i < 4; ++i) {
        const short* ip = temp + i;
        short* op = out + i;
        const int a1 = ip[0] + ip[12], b1 = ip[4] + ip[8], c1 = ip[4] - ip[8], d1 = ip[0] - ip[12];
        op[0] = static_cast<short>((a1 + b1 + 7) >> 4);
        op[8] = static_cast<short>((a1 - b1 + 7) >> 4);
        op[4] = static_cast<short>(((c1 * 2217 + d1 * 5352 + 12000) >> 16) + (d1 != 0));
        op[12] = static_cast<short>((d1 * 2217 - c1 * 5352 + 51000) >> 16);
    }
}
void forward_walsh(const short* in, short* out) {
    short temp[16];
    for (int i = 0; i < 4; ++i) {
        const short* ip = in + i * 4;
        short* op = temp + i * 4;
        const int a1 = (ip[0] + ip[2]) * 4, d1 = (ip[1] + ip[3]) * 4, c1 = (ip[1] - ip[3]) * 4, b1 = (ip[0] - ip[2]) * 4;
        op[0] = static_cast<short>(a1 + d1 + (a1 != 0));
        op[1] = static_cast<short>(b1 + c1);
        op[2] = static_cast<short>(b1 - c1);
        op[3] = static_cast<short>(a1 - d1);
    }
    for (int i = 0; i < 4; ++i) {
        const short* ip = temp + i;
        short* op = out + i;
        const int a1 = ip[0] + ip[8], d1 = ip[4] + ip[12], c1 = ip[4] - ip[12], b1 = ip[0] - ip[8];
        int a2 = a1 + d1, b2 = b1 + c1, c2 = b1 - c1, d2 = a1 - d1;
        a2 += a2 < 0, b2 += b2 < 0, c2 += c2 < 0, d2 += d2 < 0;
        op[0] = static_cast<short>((a2 + 3) >> 3);
        op[4] = static_cast<short>((b2 + 3) >> 3);
        op[8] = static_cast<short>((c2 + 3) >> 3);
        op[12] = static_cast<short>((d2 + 3) >> 3);
    }
}
void inverse_walsh(const int* in, int* out) {
    int temp[16];
    for (int i = 0; i < 4; ++i) {
        const int* ip = in + i;
        int* op = temp + i;
        const int a1 = ip[0] + ip[12], b1 = ip[4] + ip[8], c1 = ip[4] - ip[8], d1 = ip[0] - ip[12];
        op[0] = static_cast<short>(a1 + b1);
        op[4] = static_cast<short>(c1 + d1);
        op[8] = static_cast<short>(a1 - b1);
        op[12] = static_cast<short>(d1 - c1);
    }
    for (int i = 0; i < 4; ++i) {
        const int* ip = temp + i * 4;
        const int a1 = ip[0] + ip[3], b1 = ip[1] + ip[2], c1 = ip[1] - ip[2], d1 = ip[0] - ip[3];
        out[i * 4 + 0] = static_cast<short>((a1 + b1 + 3) >> 3);
        out[i * 4 + 1] = static_cast<short>((c1 + d1 + 3) >> 3);
        out[i * 4 + 2] = static_cast<short>((a1 - b1 + 3) >> 3);
        out[i * 4 + 3] = static_cast<short>((d1 - c1 + 3) >> 3);
    }
}
// Adds the inverse DCT of dequantized `in` (raster) to the prediction in place.
void inverse_dct_add(const int* in, std::uint8_t* dst, std::size_t stride) {
    constexpr int cospi8sqrt2minus1 = 20091, sinpi8sqrt2 = 35468;
    short temp[16];
    for (int i = 0; i < 4; ++i) {
        const short ip0 = static_cast<short>(in[i]), ip4 = static_cast<short>(in[4 + i]), ip8 = static_cast<short>(in[8 + i]),
                    ip12 = static_cast<short>(in[12 + i]);
        const int a1 = ip0 + ip8, b1 = ip0 - ip8;
        int t1 = (ip4 * sinpi8sqrt2) >> 16, t2 = ip12 + ((ip12 * cospi8sqrt2minus1) >> 16);
        const int c1 = t1 - t2;
        t1 = ip4 + ((ip4 * cospi8sqrt2minus1) >> 16);
        t2 = (ip12 * sinpi8sqrt2) >> 16;
        const int d1 = t1 + t2;
        temp[i] = static_cast<short>(a1 + d1);
        temp[12 + i] = static_cast<short>(a1 - d1);
        temp[4 + i] = static_cast<short>(b1 + c1);
        temp[8 + i] = static_cast<short>(b1 - c1);
    }
    for (int r = 0; r < 4; ++r) {
        const short* ip = temp + r * 4;
        const int a1 = ip[0] + ip[2], b1 = ip[0] - ip[2];
        int t1 = (ip[1] * sinpi8sqrt2) >> 16, t2 = ip[3] + ((ip[3] * cospi8sqrt2minus1) >> 16);
        const int c1 = t1 - t2;
        t1 = ip[1] + ((ip[1] * cospi8sqrt2minus1) >> 16);
        t2 = (ip[3] * sinpi8sqrt2) >> 16;
        const int d1 = t1 + t2;
        const int row[4] = {static_cast<short>((a1 + d1 + 4) >> 3), static_cast<short>((b1 + c1 + 4) >> 3),
                            static_cast<short>((b1 - c1 + 4) >> 3), static_cast<short>((a1 - d1 + 4) >> 3)};
        for (int c = 0; c < 4; ++c) {
            auto& d = dst[static_cast<std::size_t>(r) * stride + static_cast<std::size_t>(c)];
            d = static_cast<std::uint8_t>(std::clamp(row[c] + d, 0, 255));
        }
    }
}
void dc_only_add(int dc, std::uint8_t* dst, std::size_t stride) {
    const int a1 = (dc + 4) >> 3;
    for (std::size_t r = 0; r < 4; ++r)
        for (std::size_t c = 0; c < 4; ++c) dst[r * stride + c] = static_cast<std::uint8_t>(std::clamp(a1 + dst[r * stride + c], 0, 255));
}

int quantize(int coefficient, int step, bool dc) {
    // Rounding with a slight dead zone for AC coefficients: fewer stray small ones to code.
    const int a = std::abs(coefficient), bias = dc ? step / 2 : step * 3 / 8;
    const int q = std::min((a + bias) / step, 2048);
    return coefficient < 0 ? -q : q;
}

// A padded plane and its reconstruction (what the decoder will see), with VP8's frame borders:
// 127 above the frame (top-left corner included), 129 left of it.
struct Plane {
    std::size_t width{}, height{};
    std::vector<std::uint8_t> source, recon;
    Plane(const std::vector<std::uint8_t>& from, std::size_t w, std::size_t h, std::size_t padded_w, std::size_t padded_h)
        : width(padded_w), height(padded_h), source(padded_w * padded_h), recon(padded_w * padded_h) {
        for (std::size_t y = 0; y < padded_h; ++y)
            for (std::size_t x = 0; x < padded_w; ++x) source[y * padded_w + x] = from[std::min(y, h - 1) * w + std::min(x, w - 1)];
    }
    int above(std::size_t x, std::size_t y) const { return y ? recon[(y - 1) * width + x] : 127; }
    int left(std::size_t x, std::size_t y) const { return x ? recon[y * width + x - 1] : 129; }
    int corner(std::size_t x, std::size_t y) const { return !y ? 127 : !x ? 129 : recon[(y - 1) * width + x - 1]; }
    // The `size` x `size` prediction at (x, y).
    void predict(std::size_t x0, std::size_t y0, std::size_t size, Mode mode, std::uint8_t* out) const {
        for (std::size_t r = 0; r < size; ++r)
            for (std::size_t c = 0; c < size; ++c) {
                int v = 128;
                switch (mode) {
                case v_pred: v = above(x0 + c, y0); break;
                case h_pred: v = left(x0, y0 + r); break;
                case tm_pred: v = std::clamp(left(x0, y0 + r) + above(x0 + c, y0) - corner(x0, y0), 0, 255); break;
                case dc_pred: {
                    int sum = 0, count = 0;
                    if (y0) for (std::size_t i = 0; i < size; ++i) sum += above(x0 + i, y0), ++count;
                    if (x0) for (std::size_t i = 0; i < size; ++i) sum += left(x0, y0 + i), ++count;
                    v = count ? (sum + count / 2) / count : 128;
                    break;
                }
                }
                out[r * size + c] = static_cast<std::uint8_t>(v);
            }
    }
    // The mode whose prediction is closest to the source.
    Mode best_mode(std::size_t x0, std::size_t y0, std::size_t size) const {
        std::vector<std::uint8_t> guess(size * size);
        Mode best = dc_pred;
        std::uint64_t best_error = std::numeric_limits<std::uint64_t>::max();
        for (const Mode mode : {dc_pred, v_pred, h_pred, tm_pred}) {
            predict(x0, y0, size, mode, guess.data());
            std::uint64_t error = 0;
            for (std::size_t r = 0; r < size; ++r)
                for (std::size_t c = 0; c < size; ++c) {
                    const int d = source[(y0 + r) * width + x0 + c] - guess[r * size + c];
                    error += static_cast<std::uint64_t>(d * d);
                }
            if (error < best_error) best_error = error, best = mode;
        }
        return best;
    }
    // Writes the prediction into the reconstruction and returns the 4x4 residual blocks (raster
    // order of blocks, each raster order) of the source against it.
    std::vector<std::array<short, 16>> residual(std::size_t x0, std::size_t y0, std::size_t size, Mode mode) {
        std::vector<std::uint8_t> guess(size * size);
        predict(x0, y0, size, mode, guess.data());
        const std::size_t per_row = size / 4;
        std::vector<std::array<short, 16>> blocks(per_row * per_row);
        for (std::size_t r = 0; r < size; ++r)
            for (std::size_t c = 0; c < size; ++c) {
                recon[(y0 + r) * width + x0 + c] = guess[r * size + c];
                blocks[(r / 4) * per_row + c / 4][(r % 4) * 4 + c % 4] =
                    static_cast<short>(source[(y0 + r) * width + x0 + c] - guess[r * size + c]);
            }
        return blocks;
    }
    std::uint8_t* at(std::size_t x, std::size_t y) { return recon.data() + y * width + x; }
};

void put_y_mode(BoolEncoder& e, Mode mode) {
    e.put(true, kf_ymode_prob[0]); // not B_PRED
    const bool second = mode == h_pred || mode == tm_pred;
    e.put(second, kf_ymode_prob[1]);
    if (second) e.put(mode == tm_pred, kf_ymode_prob[3]);
    else e.put(mode == v_pred, kf_ymode_prob[2]);
}
void put_uv_mode(BoolEncoder& e, Mode mode) {
    e.put(mode != dc_pred, kf_uv_mode_prob[0]);
    if (mode == dc_pred) return;
    e.put(mode != v_pred, kf_uv_mode_prob[1]);
    if (mode != v_pred) e.put(mode == tm_pred, kf_uv_mode_prob[2]);
}

// Token contexts: whether the block above / to the left had coefficients. Per macroblock column
// (above) or for the current row (left): Y 0..3, U 4..5, V 6..7, Y2 8.
using Contexts = std::array<std::uint8_t, 9>;
} // namespace

std::vector<std::uint8_t> encode_keyframe(const Planes& planes, int quality) {
    const auto w = planes.width, h = planes.height;
    if (!w || !h || w > 16383 || h > 16383) throw std::invalid_argument("VP8 frame size out of range.");
    const std::size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (planes.y.size() < std::size_t{w} * h || planes.u.size() < cw * ch || planes.v.size() < cw * ch)
        throw std::invalid_argument("VP8 planes too small.");
    const std::size_t mb_cols = (w + 15) / 16, mb_rows = (h + 15) / 16;
    Plane Y(planes.y, w, h, mb_cols * 16, mb_rows * 16), U(planes.u, cw, ch, mb_cols * 8, mb_rows * 8),
        V(planes.v, cw, ch, mb_cols * 8, mb_rows * 8);
    const int qi = std::clamp(quality, 0, 127);
    const int y1_dc = dc_qlookup[qi], y1_ac = ac_qlookup[qi], y2_dc = dc_qlookup[qi] * 2,
              y2_ac = std::max((ac_qlookup[qi] * 101581) >> 16, 8), uv_dc = std::min<int>(dc_qlookup[qi], 132),
              uv_ac = ac_qlookup[qi];

    BoolEncoder header, tokens;
    header.put(false, 128); // colour space
    header.put(false, 128); // clamping required
    header.put(false, 128); // no segmentation
    header.put(false, 128); // normal loop filter...
    header.literal(0, 6);   // ...at level 0: off
    header.literal(0, 3);   // sharpness
    header.put(false, 128); // no loop filter deltas
    header.literal(0, 2);   // one token partition
    header.literal(static_cast<std::uint32_t>(qi), 7);
    for (int i = 0; i < 5; ++i) header.put(false, 128); // no quantizer deltas
    header.put(false, 128);                             // refresh_entropy_probs: keep the defaults
    for (const auto probability : coef_update_probs) header.put(false, probability); // no probability updates
    header.put(false, 128);                             // mb_no_coeff_skip off: every macroblock has tokens

    std::vector<Contexts> above(mb_cols);
    for (std::size_t my = 0; my < mb_rows; ++my) {
        Contexts left{};
        for (std::size_t mx = 0; mx < mb_cols; ++mx) {
            auto& top = above[mx];
            const std::size_t x = mx * 16, y = my * 16, cx = mx * 8, cy = my * 8;
            const Mode y_mode = Y.best_mode(x, y, 16);
            // Chroma: one mode for U and V, chosen on U and V together.
            Mode uv_mode = dc_pred;
            {
                std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
                std::array<std::uint8_t, 64> gu{}, gv{};
                for (const Mode mode : {dc_pred, v_pred, h_pred, tm_pred}) {
                    U.predict(cx, cy, 8, mode, gu.data());
                    V.predict(cx, cy, 8, mode, gv.data());
                    std::uint64_t error = 0;
                    for (std::size_t r = 0; r < 8; ++r)
                        for (std::size_t c = 0; c < 8; ++c) {
                            const int du = U.source[(cy + r) * U.width + cx + c] - gu[r * 8 + c];
                            const int dv = V.source[(cy + r) * V.width + cx + c] - gv[r * 8 + c];
                            error += static_cast<std::uint64_t>(du * du + dv * dv);
                        }
                    if (error < best) best = error, uv_mode = mode;
                }
            }
            put_y_mode(header, y_mode);
            put_uv_mode(header, uv_mode);

            // Luma: 16 blocks, their DCs gathered into the second-order (Y2) block.
            const auto y_res = Y.residual(x, y, 16, y_mode);
            std::array<std::array<short, 16>, 16> y_coeff{};
            std::array<short, 16> dcs{}, y2_coeff{};
            for (std::size_t b = 0; b < 16; ++b) {
                forward_dct(y_res[b].data(), y_coeff[b].data());
                dcs[b] = y_coeff[b][0];
            }
            forward_walsh(dcs.data(), y2_coeff.data());
            std::array<int, 16> y2_q{};
            for (std::size_t i = 0; i < 16; ++i) y2_q[i] = quantize(y2_coeff[i], i ? y2_ac : y2_dc, true);
            left[8] = top[8] = put_tokens(tokens, y2_q, y2, 0, top[8] + left[8]);
            // The DCs the decoder will have, from the quantized Y2.
            std::array<int, 16> y2_dq{}, block_dc{};
            bool y2_has_ac = false;
            for (std::size_t i = 1; i < 16; ++i) y2_has_ac |= y2_q[static_cast<std::size_t>(zigzag[i])] != 0;
            for (std::size_t i = 0; i < 16; ++i) y2_dq[i] = y2_q[i] * (i ? y2_ac : y2_dc);
            if (y2_has_ac) inverse_walsh(y2_dq.data(), block_dc.data());
            else block_dc.fill(static_cast<short>((y2_dq[0] + 3) >> 3));
            for (std::size_t b = 0; b < 16; ++b) {
                std::array<int, 16> q{};
                bool has_ac = false;
                for (std::size_t i = 1; i < 16; ++i) has_ac |= (q[i] = quantize(y_coeff[b][i], y1_ac, false)) != 0;
                const std::size_t col = b % 4, row = b / 4;
                left[row] = top[col] = put_tokens(tokens, q, y_after_y2, 1, top[col] + left[row]);
                auto* dst = Y.at(x + col * 4, y + row * 4);
                if (has_ac) {
                    std::array<int, 16> dq{};
                    dq[0] = block_dc[b];
                    for (std::size_t i = 1; i < 16; ++i) dq[i] = q[i] * y1_ac;
                    inverse_dct_add(dq.data(), dst, Y.width);
                } else {
                    dc_only_add(block_dc[b], dst, Y.width);
                }
            }
            (void)y1_dc; // luma DCs go through Y2; the first-order DC step is unused without B_PRED

            // Chroma: four 4x4 blocks each for U and V.
            for (int plane = 0; plane < 2; ++plane) {
                auto& P = plane ? V : U;
                const auto res = P.residual(cx, cy, 8, uv_mode);
                for (std::size_t b = 0; b < 4; ++b) {
                    std::array<short, 16> coeff{};
                    forward_dct(res[b].data(), coeff.data());
                    std::array<int, 16> q{};
                    bool beyond_first = false;
                    for (std::size_t i = 0; i < 16; ++i) q[i] = quantize(coeff[i], i ? uv_ac : uv_dc, i == 0);
                    for (std::size_t i = 1; i < 16; ++i) beyond_first |= q[static_cast<std::size_t>(zigzag[i])] != 0;
                    const std::size_t col = b % 2, row = b / 2, slot = 4 + static_cast<std::size_t>(plane) * 2;
                    left[slot + row] = top[slot + col] = put_tokens(tokens, q, chroma, 0, top[slot + col] + left[slot + row]);
                    auto* dst = P.at(cx + col * 4, cy + row * 4);
                    if (beyond_first) {
                        std::array<int, 16> dq{};
                        for (std::size_t i = 0; i < 16; ++i) dq[i] = q[i] * (i ? uv_ac : uv_dc);
                        inverse_dct_add(dq.data(), dst, P.width);
                    } else {
                        dc_only_add(q[0] * uv_dc, dst, P.width);
                    }
                }
            }
        }
    }

    const auto first = header.finish();
    const auto second = tokens.finish();
    if (first.size() >= (1u << 19)) throw std::runtime_error("VP8 header partition too large.");
    std::vector<std::uint8_t> out;
    out.reserve(10 + first.size() + second.size());
    const std::uint32_t tag = 0 /* key frame */ | (0 << 1) /* version */ | (1 << 4) /* shown */ |
                              (static_cast<std::uint32_t>(first.size()) << 5);
    out.insert(out.end(), {static_cast<std::uint8_t>(tag), static_cast<std::uint8_t>(tag >> 8), static_cast<std::uint8_t>(tag >> 16),
                           0x9d, 0x01, 0x2a, static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8),
                           static_cast<std::uint8_t>(h), static_cast<std::uint8_t>(h >> 8)});
    out.insert(out.end(), first.begin(), first.end());
    out.insert(out.end(), second.begin(), second.end());
    return out;
}
} // namespace dingosdk::turnaround::vp8
