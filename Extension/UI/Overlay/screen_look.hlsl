// A camcorder look over the finished frame (screen_look.cpp, reskate.screen.look), made from the
// Filmer's VX1000 look (neekgate/ReSkate, filmer-zee). Built into the DLL as text (cmake/UI.cmake) and
// compiled when the overlay first draws it; the build checks it with fxc when the Windows SDK has one.
//
// Order, as on a real camcorder: the world through a fisheye adapter (bend), a hand in front of the
// lens, the camera's colour and iris, the hood's dark edge, then the electronics burn in the text,
// and the tape softens, interlaces and adds grain to all of it.
cbuffer Look : register(b0)
{
    float4 L0; // bend 0..1, half angle the bend covers at the frame's top edge (rad), tan(half the game's FOV), frame half width (half heights)
    float4 L1; // screen aspect, 4:3 bars, hood, seconds
    float4 L2; // grade, scanlines, grain, softness
    float4 L3; // transfer (0 SDR), exposure, output width, output height
    float4 L4; // hand centre x, y (half heights, y down), angle (rad), size (0: no hand)
    float4 L5; // hand mirrored, hand blur (hand units), unused, unused
    float4 L6; // text pixel (half heights), text white, text on, unused
    float4 L7; // tape age 0..1, dropouts, head switching, red smear
    float4 L8; // seconds since this clip started recording (tracking; < 0: none), tracking, auto-iris, seconds since the last frame
    uint4 T[4]; // five lines of twelve characters, four to a uint: top left, top right, bottom left, bottom right x2
};
Texture2D source : register(t0);
// The auto-iris (ps_adapt): x the brightness it has adapted to, y the scene's brightness this frame.
Texture2D adapted : register(t1);
SamplerState linear_clamp : register(s0);

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

struct Interpolated { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };

Interpolated vs_main(uint id : SV_VertexID)
{
    Interpolated o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = p;
    return o;
}

// ---- the camcorder's character generator: 5x7, ASCII 32..95; '#' is the record dot, '[=]' the battery
static const uint2 FONT[64] =
{
    uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x1dfffdc0u, 0x0u),
    uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u),
    uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u),
    uint2(0x00000000u, 0x0u), uint2(0x000f8000u, 0x0u), uint2(0x8c000000u, 0x1u), uint2(0x42222210u, 0x0u),
    uint2(0xa33ae62eu, 0x3u), uint2(0x884210c4u, 0x3u), uint2(0xc444422eu, 0x7u), uint2(0xa304111fu, 0x3u),
    uint2(0x11f4a988u, 0x2u), uint2(0xa3083c3fu, 0x3u), uint2(0xa317844cu, 0x3u), uint2(0x8422221fu, 0x0u),
    uint2(0xa317462eu, 0x3u), uint2(0x910f462eu, 0x1u), uint2(0x08401080u, 0x0u), uint2(0x00000000u, 0x0u),
    uint2(0x00000000u, 0x0u), uint2(0x3fffffe0u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u),
    uint2(0x00000000u, 0x0u), uint2(0x631fc62eu, 0x4u), uint2(0xe317c62fu, 0x3u), uint2(0xa210862eu, 0x3u),
    uint2(0xd318c527u, 0x1u), uint2(0xc217843fu, 0x7u), uint2(0x4217843fu, 0x0u), uint2(0xa31e862eu, 0x7u),
    uint2(0x631fc631u, 0x4u), uint2(0x8842108eu, 0x3u), uint2(0x9284211cu, 0x1u), uint2(0x52519531u, 0x4u),
    uint2(0xc2108421u, 0x7u), uint2(0x631ad771u, 0x4u), uint2(0x639ace31u, 0x4u), uint2(0xa318c62eu, 0x3u),
    uint2(0x4217c62fu, 0x0u), uint2(0x9358c62eu, 0x5u), uint2(0x5257c62fu, 0x4u), uint2(0xe107043eu, 0x3u),
    uint2(0x0842109fu, 0x1u), uint2(0xa318c631u, 0x3u), uint2(0x1518c631u, 0x1u), uint2(0xab5ac631u, 0x2u),
    uint2(0x62a22a31u, 0x4u), uint2(0x08422a31u, 0x1u), uint2(0xc222221fu, 0x7u), uint2(0xc210843fu, 0x7u),
    uint2(0x00000000u, 0x0u), uint2(0xd18c610fu, 0x3u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u),
};

float glyph_bit(uint code, int fx, int fy)
{
    uint index = (code >= 32u && code < 96u) ? code - 32u : 0u;
    uint bit = (uint)(fy * 5 + fx);
    uint2 g = FONT[index];
    return (float)(bit < 32u ? (g.x >> bit) & 1u : (g.y >> (bit - 32u)) & 1u);
}

uint text_char(int row_at, int col_at)
{
    uint word = (uint)(row_at * 3 + (col_at >> 2));
    uint4 v = T[word >> 2];
    uint w = v[word & 3u];
    return (w >> (8u * (uint)(col_at & 3))) & 0xffu;
}

// x: on_bit, y: the record dot (red).
float2 text_at(float2 q)
{
    float gp = L6.x;
    float left = -L0.w + 0.14, right = L0.w - 0.14 - 72.0 * gp;
    float top = -0.88, bottom = 0.88 - 7.0 * gp;
    int row_at = -1;
    float2 origin = float2(0.0, 0.0);
    if (q.y >= top && q.y < top + 7.0 * gp) { row_at = q.x < 0.0 ? 0 : 1; origin = float2(q.x < 0.0 ? left : right, top); }
    else if (q.y >= bottom && q.y < bottom + 7.0 * gp) { row_at = q.x < 0.0 ? 2 : 4; origin = float2(q.x < 0.0 ? left : right, bottom); }
    else if (q.x >= 0.0 && q.y >= bottom - 9.0 * gp && q.y < bottom - 2.0 * gp) { row_at = 3; origin = float2(right, bottom - 9.0 * gp); }
    if (row_at < 0) return float2(0.0, 0.0);
    float2 cell = (q - origin) / gp;
    int col_at = (int)floor(cell.x / 6.0);
    int fx = (int)floor(cell.x) - col_at * 6;
    int fy = (int)floor(cell.y);
    if (col_at < 0 || col_at >= 12 || fx < 0 || fx >= 5 || fy < 0 || fy >= 7) return float2(0.0, 0.0);
    uint code = text_char(row_at, col_at);
    float on_bit = glyph_bit(code, fx, fy);
    return float2(on_bit, code == 35u ? on_bit : 0.0);
}

// Premultiplied white (or red) text with the dark edge the camcorder gives it.
float4 text_sample(float2 q)
{
    float2 f = text_at(q);
    float shadow = text_at(q - L6.x * 0.9).x * 0.75 * (1.0 - f.x);
    float3 tint = f.y > 0.5 ? float3(0.95, 0.12, 0.08) : float3(1.0, 1.0, 1.0);
    return float4(tint * L6.y * f.x, f.x + shadow);
}

// ---- a hand right in front of the lens: palm towards it, fingers up (hand units: about a palm wide).
// That close it is out of focus: a soft, rounded shape with no sharp detail.
float sq(float x) { return x * x; }
// A finger: a capsule that narrows from `r0` at its base to `r1` at its tip.
float tapered(float2 p, float2 a, float2 b, float r0, float r1, out float along)
{
    float2 pa = p - a, ba = b - a;
    along = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * along) - lerp(r0, r1, along);
}
float rounded_box(float2 p, float2 centre, float2 half_size, float r)
{
    float2 d = abs(p - centre) - half_size + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}
float smooth_min(float a, float b, float k)
{
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0 - h);
}
float ellipse(float2 p, float2 centre, float2 radii)
{
    return (length((p - centre) / radii) - 1.0) * min(radii.x, radii.y);
}
// x: distance (negative inside), y: the faint folds at the finger joints. The camera operator reaches round the
// camera with their free arm, so the hand comes in at an angle with the wrist and forearm behind it.
float2 hand(float2 p)
{
    float d = rounded_box(p, float2(0.02, -0.17), float2(0.35, 0.37), 0.24);
    d = smooth_min(d, ellipse(p, float2(-0.24, -0.33), float2(0.25, 0.29)), 0.10); // the pad at the base of the thumb
    // Fingers: base, direction (radians from straight up: spread apart), length, radii, and a slight
    // curl of the last joints (towards the middle, and towards the lens: a little shorter).
    float4 bx = float4(-0.25, -0.085, 0.085, 0.235), by = float4(0.16, 0.21, 0.21, 0.15);
    float4 spread = float4(-0.17, -0.05, 0.07, 0.22), lengths = float4(0.64, 0.72, 0.68, 0.54);
    float4 r0 = float4(0.097, 0.103, 0.101, 0.088), r1 = float4(0.076, 0.081, 0.079, 0.068);
    float4 curl = float4(0.10, 0.05, -0.04, -0.10);
    float fingers = 9.0, folds = 0.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float2 base = float2(bx[i], by[i]);
        float2 knuckle = base + float2(sin(spread[i]), cos(spread[i])) * lengths[i] * 0.58;
        float2 tip = knuckle + float2(sin(spread[i] + curl[i]), cos(spread[i] + curl[i])) * lengths[i] * 0.42 * 0.9;
        float rm = lerp(r0[i], r1[i], 0.58);
        float h1, h2;
        float f = smooth_min(tapered(p, base, knuckle, r0[i], rm, h1), tapered(p, knuckle, tip, rm, r1[i], h2), 0.02);
        float fold = exp(-sq((h1 - 0.62) / 0.12)) + exp(-sq((h2 - 0.45) / 0.15));
        if (f < fingers) { fingers = f; folds = fold; }
    }
    float along;
    float thumb = min(tapered(p, float2(-0.30, -0.36), float2(-0.58, -0.06), 0.12, 0.10, along),
                      tapered(p, float2(-0.58, -0.06), float2(-0.66, 0.18), 0.10, 0.082, along));
    float arm = tapered(p, float2(0.04, -0.55), float2(0.30, -2.4), 0.27, 0.33, along);
    d = smooth_min(d, fingers, 0.09);
    d = smooth_min(d, thumb, 0.08);
    d = smooth_min(d, arm, 0.12);
    return float2(d, fingers <= d + 0.03 ? folds : 0.0);
}

float3 fetch(float2 uv)
{
    return source.SampleLevel(linear_clamp, uv, 0.0).rgb;
}

// The auto-iris, a pass of its own into a 1x1 target before the picture: how bright the scene is
// now (the mean of 64 samples, in log), and the brightness the iris has settled to, which follows it
// over about a second (the previous frame's, from the other of two targets).
float2 ps_adapt(Interpolated i) : SV_Target
{
    float sum = 0.0;
    [unroll] for (int y = 0; y < 8; ++y)
        [unroll] for (int x = 0; x < 8; ++x)
            sum += log(max(dot(fetch(float2((x + 0.5) / 8.0, (y + 0.5) / 8.0)), float3(0.299, 0.587, 0.114)), 0.002));
    float current = exp(sum / 64.0);
    float previous = adapted.Load(int3(0, 0, 0)).x;
    float k = 1.0 - exp(-max(L8.w, 0.0) / 1.1);
    float settled = (previous > 0.0 && previous < 100.0) ? lerp(previous, current, k) : current;
    return float2(settled, current);
}

float4 ps_main(Interpolated i) : SV_Target
{
    float screen_aspect = L1.x;
    // The tape: the picture's lines as recorded. Head switching tears the bottom few lines sideways;
    // when a clip starts recording, the tracking settles: a band of tearing rolls up the frame.
    float lines = min(L3.w, 480.0);
    float scan = floor(i.uv.y * lines);
    float frame = floor(L1.w * 29.97);
    float age = L7.x;
    float2 tape_uv = i.uv;
    if (L7.z > 0.5 && i.uv.y > 0.972)
    {
        float k = (i.uv.y - 0.972) / 0.028;
        tape_uv.x += (hash12(float2(scan, frame)) - 0.3) * 0.04 * k * (0.5 + age);
    }
    float tracking = 0.0;
    if (L8.y > 0.5 && L8.x >= 0.0 && L8.x < 0.5)
    {
        float t = L8.x / 0.5;
        tracking = exp(-sq((i.uv.y - (1.1 - t * 1.3)) / 0.03)) * (1.0 - t);
        tape_uv.x += (hash12(float2(scan, frame + 7.0)) - 0.5) * 0.025 * tracking + sin(i.uv.y * 60.0 + L1.w * 30.0) * 0.0012 * (1.0 - t);
    }
    float2 c = tape_uv * 2.0 - 1.0;
    float2 q = float2(c.x * screen_aspect, c.y);
    // 4:3: black at the sides.
    if (L1.y > 0.5 && abs(q.x) > L0.w)
        return float4(0.0, 0.0, 0.0, 1.0);
    bool sdr = L3.x < 0.5;
    // The bend: the MK1 squeezes a wide view into the frame. A point at radius r (half heights) sees
    // the direction r * the half angle off the axis; the game drew that direction at tan(angle) / tan(FOV / 2).
    float r = length(q);
    float2 along = r > 1e-5 ? q / r : float2(0.0, 0.0);
    float rs = r;
    if (L0.x > 0.0)
    {
        float theta = min(r * L0.y, 1.5);
        rs = lerp(r, tan(theta) / L0.z, L0.x);
    }
    float2 s = along * rs;
    float2 uv = float2(s.x / screen_aspect, s.y) * 0.5 + 0.5;
    float2 inside = min(uv, 1.0 - uv);
    float margin = min(inside.x, inside.y);
    float2 texel = 1.0 / max(L3.zw, 1.0);
    float sd = max(L3.w / 480.0, 1.0); // standard definition: a few hundred lines
    float3 colour = float3(0.0, 0.0, 0.0);
    if (margin >= 0.0)
    {
        if (L2.w > 0.5)
        {
            // Soft, with the camcorder's sharpening halo; colour bleeds sideways on tape.
            float2 close_tap = texel * sd * 0.6;
            float2 wide = texel * sd * 1.8;
            float3 soft = (fetch(uv + float2(close_tap.x, 0)) + fetch(uv - float2(close_tap.x, 0)) + fetch(uv + float2(0, close_tap.y)) + fetch(uv - float2(0, close_tap.y))) * 0.25;
            float3 blur = (fetch(uv + float2(wide.x, wide.y)) + fetch(uv + float2(-wide.x, wide.y)) + fetch(uv + float2(wide.x, -wide.y)) + fetch(uv - wide)) * 0.25;
            colour = soft + (soft - blur) * 0.55;
            float3 right = fetch(uv + float2(texel.x * sd * 2.5, 0.0));
            float3 weights = float3(0.299, 0.587, 0.114);
            float luma = dot(colour, weights);
            float3 mixed = lerp(colour, right, 0.5);
            colour = luma + (mixed - dot(mixed, weights));
        }
        else
        {
            colour = fetch(uv);
        }
        // Reds smear on tape: they trail to the right.
        if (L7.w > 0.5)
            colour.r = lerp(colour.r, fetch(uv + float2(texel.x * sd * 4.0, 0.0)).r, 0.3);
    }
    // The hand in front of the lens: lit by the scene, then dark and glowing red through the skin
    // once it is pressed over the glass.
    if (L4.w > 0.0)
    {
        float2 p = q - L4.xy;
        p.x *= L5.x > 0.5 ? -1.0 : 1.0;
        float ca = cos(-L4.z), sa = sin(-L4.z);
        p = float2(p.x * ca - p.y * sa, p.x * sa + p.y * ca) / L4.w;
        p.y = -p.y; // the picture's y runs down; the fingers point up
        // The palm is turned a little away round its long axis: narrower, its near side bigger.
        p.x /= 0.9; // cos(0.45)
        p /= 1.0 + 0.22 * p.x;
        float2 h = hand(p);
        float blur = 0.05 + L5.y; // out of focus, more so while it moves
        float cover = 1.0 - smoothstep(-blur, blur, h.x);
        if (cover > 0.0)
        {
            // Rounded shading from the shape's slope, sampled wide so it stays smooth.
            const float e = 0.06;
            float2 gradient = float2(hand(p + float2(e, 0.0)).x - hand(p - float2(e, 0.0)).x,
                                     hand(p + float2(0.0, e)).x - hand(p - float2(0.0, e)).x) / (2.0 * e);
            float depth = saturate(-h.x / 0.32);
            float3 n = normalize(float3(gradient * (1.0 - depth) + float2(0.25, 0.0), sqrt(saturate(1.0 - sq(1.0 - depth))) * 0.85 + 0.15));
            float3 light = normalize(float3(-0.35, 0.55, 0.76));
            float wrap = saturate((dot(n, light) + 0.4) / 1.4); // soft light wrapping round the skin
            float3 weights = float3(0.299, 0.587, 0.114);
            float ambient = (dot(fetch(float2(0.5, 0.5)), weights) + dot(fetch(float2(0.25, 0.3)), weights) + dot(fetch(float2(0.75, 0.3)), weights) +
                             dot(fetch(float2(0.25, 0.75)), weights) + dot(fetch(float2(0.75, 0.75)), weights)) * 0.2;
            float blotch = sin(p.x * 9.1 + sin(p.y * 7.3)) * sin(p.y * 8.7 + sin(p.x * 5.9)) * 0.5; // uneven skin tone
            float3 skin = float3(0.74, 0.53, 0.43) * (1.0 + 0.06 * blotch) * (0.30 + 0.75 * wrap);
            skin += float3(0.22, 0.05, 0.02) * sq(1.0 - depth) * 0.6; // light through the thin edges
            skin *= 1.0 - 0.07 * saturate(h.y);
            skin *= 1.0 - 0.12 * exp(-(sq(p.x) / 0.05 + sq(p.y + 0.08) / 0.06)); // the palm's hollow
            skin *= 1.0 + 0.10 * pow(saturate(1.0 - length((p - float2(-0.22, -0.33)) / float2(0.22, 0.26))), 0.7); // the thumb's pad swells to the light
            skin *= 0.55 + ambient;
            float pressed = saturate((L4.w - 2.6) / 1.4);
            float glow = exp(-abs(h.x) * 14.0);
            float3 dark = float3(0.05, 0.02, 0.015) + float3(0.5, 0.1, 0.04) * glow * 0.6;
            skin = lerp(skin, dark, pressed);
            if (!sdr)
                skin *= 0.5;
            colour = lerp(colour, skin, cover);
        }
    }
    if (sdr && L2.x > 0.5)
    {
        // The VX's colour: saturated, contrasty, warm, blacks a touch crushed.
        colour = saturate(colour);
        float luma = dot(colour, float3(0.299, 0.587, 0.114));
        colour = saturate(lerp(luma.xxx, colour, 1.2));
        float3 curve = colour * colour * (3.0 - 2.0 * colour);
        colour = lerp(colour, curve, 0.4);
        colour *= float3(1.05, 1.01, 0.93);
        colour = saturate((colour - 0.025) / 0.975);
    }
    if (sdr)
        colour *= L3.y; // the iris: down while the hand covers it, open too wide for a moment after
    // The auto-iris lags the scene: into the sun the picture blows out for a moment, into shade it
    // goes murky, then each settles. (Not in HDR10, whose values are not linear.)
    if (L8.z > 0.5 && L3.x < 1.5)
    {
        float2 iris = adapted.Load(int3(0, 0, 0)).xy;
        if (iris.x > 0.0 && iris.x < 100.0 && iris.y > 0.0)
            colour *= clamp(pow(iris.y / iris.x, 0.75), 0.5, 2.0);
    }
    // The hood: the fisheye's picture ends in a soft dark edge, and darkens towards it.
    if (L1.z > 0.5)
    {
        float edge = smoothstep(0.0, 0.035, margin);
        float falloff = 1.0 - 0.4 * smoothstep(0.75, 1.85, r * (L0.x > 0.0 ? 1.0 : 0.85));
        colour *= edge * falloff;
    }
    else if (margin < 0.0)
    {
        colour = float3(0.0, 0.0, 0.0);
    }
    // The date and the viewfinder: burnt in by the electronics, so straight (no bend) and over the
    // hood, but softened by the tape like everything else.
    if (L6.z > 0.5 && abs(q.y) > 0.88 - 18.0 * L6.x)
    {
        float4 text;
        if (L2.w > 0.5)
        {
            float px = 2.0 / max(L3.w, 1.0) * sd;
            float4 near_taps = (text_sample(q + float2(px * 0.6, 0.0)) + text_sample(q - float2(px * 0.6, 0.0)) +
                                text_sample(q + float2(0.0, px * 0.6)) + text_sample(q - float2(0.0, px * 0.6))) * 0.25;
            float4 wide_taps = (text_sample(q + px * 1.8) + text_sample(q - px * 1.8) +
                                text_sample(q + float2(px * 1.8, -px * 1.8)) + text_sample(q + float2(-px * 1.8, px * 1.8))) * 0.25;
            text = near_taps + (near_taps - wide_taps) * 0.55;
            text.a = saturate(text.a);
            text.rgb = max(text.rgb, 0.0);
        }
        else
        {
            text = text_sample(q);
        }
        colour = colour * (1.0 - text.a) + text.rgb;
    }
    // Dropouts: now and then a short white streak where the tape lost its coating (more, the older it is).
    if (L7.y > 0.5)
    {
        [unroll] for (int k = 0; k < 3; ++k)
        {
            if (hash12(float2(frame, k * 13.1)) < age * age * 0.05)
            {
                float y0 = hash12(float2(frame + 3.1, k));
                float x0 = hash12(float2(k, frame + 9.7)) * 1.1 - 0.1;
                float span = 0.04 + 0.25 * hash12(float2(frame * 1.7, k + 2.0));
                if (abs(i.uv.y - y0) * lines < 1.0 && i.uv.x > x0 && i.uv.x < x0 + span)
                    colour = lerp(colour, ((0.6 + 0.4 * hash12(float2(i.uv.x * 900.0, frame))) * (sdr ? 1.0 : L6.y)).xxx, 0.85);
            }
        }
    }
    // Head switching: the bottom lines also carry a strip of noise.
    if (L7.z > 0.5 && i.uv.y > 0.985)
        colour = lerp(colour, (hash12(float2(i.uv.x * 400.0, frame + scan)) * (sdr ? 0.8 : 0.4 * L6.y)).xxx, 0.35 + 0.3 * age);
    // The tracking band: its picture loses its colour and fills with noise.
    if (tracking > 0.01)
    {
        float grey = dot(colour, float3(0.299, 0.587, 0.114));
        colour = lerp(colour, (grey + (hash12(float2(i.uv.x * 700.0, frame + scan)) - 0.5) * 0.2).xxx, saturate(tracking * 0.5));
    }
    // Interlaced fields: every other line a little darker.
    if (L2.y > 0.5)
        colour *= (fmod(scan, 2.0) < 1.0) ? 1.0 : 0.93;
    if (sdr && L2.z > 0.5)
    {
        float n = frac(sin(dot(floor(i.uv * float2(720.0, 480.0)) + frac(L1.w * 37.0) * 91.0, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
        colour += n * 0.045;
    }
    return float4(colour, 1.0);
}
